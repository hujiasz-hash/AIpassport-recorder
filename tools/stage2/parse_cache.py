import struct, sys, zlib, os

SEC, HDR = 4096, 20
MAGIC = 0x31435541

data = open(sys.argv[1], "rb").read()
out_dir = sys.argv[2] if len(sys.argv) > 2 else None
n = len(data) // SEC
sessions = {}
order = []
count = 0
for i in range(n):
    s = data[i * SEC:(i + 1) * SEC]
    magic, seq, sess, ln = struct.unpack_from("<IIIH", s, 0)
    crc = struct.unpack_from("<I", s, 16)[0]
    if magic != MAGIC or seq != i + 1 or ln > SEC - HDR:
        break
    if (zlib.crc32(s[0:16] + s[HDR:HDR + ln]) & 0xFFFFFFFF) != crc:
        print(f"sector {i}: CRC bad, stop")
        break
    if sess not in sessions:
        sessions[sess] = bytearray()
        order.append(sess)
    sessions[sess] += s[HDR:HDR + ln]
    count += 1

print("valid sectors:", count)
for sess in order:
    print(f"session {sess:08x}: {len(sessions[sess])} bytes")
if out_dir:
    os.makedirs(out_dir, exist_ok=True)
    for sess in order:
        path = os.path.join(out_dir, f"sess_{sess:08x}.ogg")
        open(path, "wb").write(sessions[sess])
        print("wrote", path)
