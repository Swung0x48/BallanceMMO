"""Every thread of an x64 minidump - its RIP and the return-address candidates on its stack - symbolized with
dbghelp against the images and PDBs on disk (the modules' paths in the dump; a slim build copy keeps its .pdb
files beside the .exe/.dll).  Written at FULL step SAXH's gate round for the process-exit hang; since step HXP
arena_map_test.py runs it on every hang dump its watcher writes (needs the 'minidump' package; cdb is broken
on the build machine).  The stack scan is heuristic: a candidate is any stack word inside a module, so read the
frames from the RIP down and expect stale entries between them.
usage: python -X utf8 sym_dump.py DUMP [MAX_FRAMES]"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import logging
from minidump.minidumpfile import MinidumpFile

# A dump without full memory has no PEB: the library logs that as an ERROR with
# a traceback and parses on; the threads and modules this script reads are there.
logging.disable(logging.ERROR)

path = sys.argv[1]
max_frames = int(sys.argv[2]) if len(sys.argv) > 2 else 18
mf = MinidumpFile.parse(path)
reader = mf.get_reader()
mods = [(m.baseaddress, m.baseaddress + m.size, m.name) for m in mf.modules.modules]

dbg = ctypes.WinDLL('dbghelp', use_last_error=True)
HP = wt.HANDLE(0x5A5A)
dbg.SymSetOptions(0x2 | 0x4 | 0x10)  # UNDNAME | DEFERRED_LOADS | LOAD_LINES
dbg.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
dbg.SymInitializeW(HP, None, False)
dbg.SymLoadModuleExW.restype = ctypes.c_uint64
dbg.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, ctypes.c_uint64, wt.DWORD, wt.LPVOID, wt.DWORD]
for b, e, n in mods:
    dbg.SymLoadModuleExW(HP, None, n, None, b, e - b, None, 0)


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG), ('Scope', wt.ULONG),
                ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG), ('Name', ctypes.c_wchar * 1)]


class LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', wt.LPVOID), ('LineNumber', wt.DWORD), ('FileName', wt.LPWSTR),
                ('Address', ctypes.c_uint64)]


dbg.SymFromAddrW.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), wt.LPVOID]
dbg.SymGetLineFromAddrW64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), wt.LPVOID]


def module_of(a):
    for b, e, n in mods:
        if b <= a < e:
            return n.split('\\')[-1], a - b
    return None


def sym(a):
    m = module_of(a)
    if not m:
        return None
    buf = ctypes.create_string_buffer(ctypes.sizeof(SYMBOL_INFOW) + 1024 * 2)
    si = ctypes.cast(buf, ctypes.POINTER(SYMBOL_INFOW)).contents
    si.SizeOfStruct = ctypes.sizeof(SYMBOL_INFOW)
    si.MaxNameLen = 1024
    disp = ctypes.c_uint64(0)
    name = '%s+0x%x' % m
    if dbg.SymFromAddrW(HP, a, ctypes.byref(disp), buf):
        nm = ctypes.wstring_at(ctypes.addressof(si) + SYMBOL_INFOW.Name.offset, si.NameLen)
        name = '%s!%s+0x%x' % (m[0], nm, disp.value)
        ln = LINE64()
        ln.SizeOfStruct = ctypes.sizeof(LINE64)
        d2 = wt.DWORD(0)
        if dbg.SymGetLineFromAddrW64(HP, a, ctypes.byref(d2), ctypes.byref(ln)):
            name += '  [%s:%d]' % (ln.FileName.split('\\')[-1], ln.LineNumber)
    return name


for t in mf.threads.threads:
    ctx = t.ContextObject
    rip = getattr(ctx, 'Rip', None)
    rsp = getattr(ctx, 'Rsp', None)
    print('=== thread %d  rip %s' % (t.ThreadId, sym(rip) if rip else None))
    if not rsp:
        continue
    try:
        data = reader.read(rsp, 0x6000)
    except Exception:
        try:
            data = reader.read(rsp, 0x1000)
        except Exception as e:
            print('   (stack unreadable: %s)' % e)
            continue
    n = 0
    for i in range(0, len(data) - 7, 8):
        v = struct.unpack_from('<Q', data, i)[0]
        m = module_of(v)
        if not m:
            continue
        s = sym(v)
        print('   [rsp+0x%x] %s' % (i, s))
        n += 1
        if n >= max_frames:
            break
