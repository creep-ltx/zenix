# SPDX-License-Identifier: GPL-2.0-only
# Test helper (tools/disk-host-test.sh): remove the first partition from the RDB of an image, as another tool might.
import struct, sys
f = open(sys.argv[1], "r+b")
d = bytearray(f.read(1024))
r = list(struct.unpack(">128L", d[:512]))
p = list(struct.unpack(">128L", d[512:1024]))
r[7] = p[4]
r[2] = 0
r[2] = (-sum(r[:r[1]])) & 0xFFFFFFFF
f.seek(0)
f.write(struct.pack(">128L", *r))
