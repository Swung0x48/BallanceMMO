"""Minimal x86 minidump reader: exception record, faulting module, and a
return-address scan of the faulting thread's stack.

usage: minidump_info.py <dump> [map files...]
Each map file (MSVC /MAP output) is used to symbolize offsets of the module
whose name matches the map's base name.
"""
import bisect
import os
import struct
import sys

path = sys.argv[1]
maps = sys.argv[2:]
data = open(path, 'rb').read()

sig, ver, nstreams, dir_rva = struct.unpack_from('<4sIII', data, 0)
assert sig == b'MDMP', sig
streams = {}
for i in range(nstreams):
    stype, size, rva = struct.unpack_from('<III', data, dir_rva + 12 * i)
    streams.setdefault(stype, []).append((size, rva))


def read_string(rva):
    (length,) = struct.unpack_from('<I', data, rva)
    return data[rva + 4:rva + 4 + length].decode('utf-16-le', errors='replace')


modules = []
for size, rva in streams.get(4, []):
    (count,) = struct.unpack_from('<I', data, rva)
    for i in range(count):
        off = rva + 4 + 108 * i
        base, image_size, checksum, stamp, name_rva = struct.unpack_from('<QIIII', data, off)
        modules.append((base, image_size, read_string(name_rva)))
modules.sort()


def locate(address):
    for base, size, name in modules:
        if base <= address < base + size:
            return os.path.basename(name), address - base
    return None, None


memory = []
for size, rva in streams.get(5, []):
    (count,) = struct.unpack_from('<I', data, rva)
    for i in range(count):
        start, dsize, drva = struct.unpack_from('<QII', data, rva + 4 + 16 * i)
        memory.append((start, dsize, drva))
memory.sort()


def read_memory(address, length):
    for start, dsize, drva in memory:
        if start <= address < start + dsize:
            avail = min(length, start + dsize - address)
            return data[drva + (address - start):drva + (address - start) + avail]
    return b''


symbols = {}
for map_path in maps:
    name = os.path.splitext(os.path.basename(map_path))[0].lower()
    entries = []
    preferred = None
    for line in open(map_path, encoding='utf-8', errors='replace'):
        if 'Preferred load address is' in line:
            preferred = int(line.split()[-1], 16)
        parts = line.split()
        if len(parts) >= 4 and ':' in parts[0] and len(parts[2]) == 8 and all(c in '0123456789abcdefABCDEF' for c in parts[2]):
            try:
                rva_abs = int(parts[2], 16)
            except ValueError:
                continue
            if preferred is None:
                continue
            entries.append((rva_abs - preferred, parts[1]))
    entries.sort()
    symbols[name] = entries


def symbolize(module, offset):
    if module is None:
        return '?'
    entries = symbols.get(os.path.splitext(module)[0].lower())
    if not entries:
        return f'{module}+0x{offset:x}'
    keys = [e[0] for e in entries]
    i = bisect.bisect_right(keys, offset) - 1
    if i < 0:
        return f'{module}+0x{offset:x}'
    return f'{module}!{entries[i][1]}+0x{offset - entries[i][0]:x}'


for size, rva in streams.get(6, []):
    thread_id, _, code, flags, record, address, nparams = struct.unpack_from('<IIIIQQI', data, rva)
    params = struct.unpack_from('<15Q', data, rva + 8 + 32)
    ctx_size, ctx_rva = struct.unpack_from('<II', data, rva + 8 + 32 + 120)
    module, offset = locate(address)
    print(f'exception thread={thread_id} code=0x{code:08x} address=0x{address:x} -> {symbolize(module, offset)}')
    if code == 0xC0000005:
        print(f'  access violation: {"write" if params[0] == 1 else "read"} at 0x{params[1]:x}')
    ctx = data[ctx_rva:ctx_rva + ctx_size]
    if ctx_size >= 204:
        edi, esi, ebx, edx, ecx, eax, ebp, eip = struct.unpack_from('<8I', ctx, 156)
        (esp,) = struct.unpack_from('<I', ctx, 196)
        print(f'  eip=0x{eip:x} esp=0x{esp:x} ebp=0x{ebp:x} eax=0x{eax:x} ebx=0x{ebx:x} ecx=0x{ecx:x} edx=0x{edx:x} esi=0x{esi:x} edi=0x{edi:x}')
        stack = read_memory(esp, 0x1000)
        print('  stack scan (return-address candidates):')
        shown = 0
        for i in range(0, len(stack) - 3, 4):
            (value,) = struct.unpack_from('<I', stack, i)
            m, o = locate(value)
            if m and o > 0x1000:
                print(f'    [esp+0x{i:x}] 0x{value:08x} {symbolize(m, o)}')
                shown += 1
                if shown >= 40:
                    break

print('modules of interest:')
for base, size, name in modules:
    if any(k in name.lower() for k in ('ballancemmo', 'physics_rt', 'bml', 'ck2', 'player.exe')):
        print(f'  0x{base:x} size=0x{size:x} {name}')
