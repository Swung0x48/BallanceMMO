"""tas_raw.py IN.tas OUT.raw : a TASSupport record (u32 decompressed size, then zlib) as the raw frames
<f32 frame ms><u32 key bits> that BallanceMMOSimTool --tas reads (the SimTool has no zlib)."""
import struct
import sys
import zlib

src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
size = struct.unpack('<I', data[:4])[0]
raw = zlib.decompress(data[4:])
assert len(raw) == size and size % 8 == 0, (len(raw), size)
open(dst, 'wb').write(raw)
print(dst, size // 8, 'frames, first delta', struct.unpack('<f', raw[:4])[0])
