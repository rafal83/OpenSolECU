"""Print public capture frames without a Wireshark dependency."""
import pathlib, struct, sys
d = pathlib.Path(sys.argv[1]).read_bytes()
p = n = 0
while p < len(d):
    t, length = struct.unpack_from('<II', d, p)
    if length < 12 or p + length > len(d):
        raise ValueError('invalid pcapng block')
    if t == 6:
        n += 1
        cap = struct.unpack_from('<I', d, p + 20)[0]
        print(n, d[p+28:p+28+cap].hex())
    p += length
