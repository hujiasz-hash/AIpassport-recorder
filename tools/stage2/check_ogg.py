import struct, sys, glob, os

T = []
for i in range(256):
    r = i << 24
    for _ in range(8):
        r = (((r << 1) ^ 0x04C11DB7) & 0xFFFFFFFF) if r & 0x80000000 else ((r << 1) & 0xFFFFFFFF)
    T.append(r)


def ogg_crc(b):
    c = 0
    for x in b:
        c = ((c << 8) & 0xFFFFFFFF) ^ T[((c >> 24) ^ x) & 0xFF]
    return c


def check(path):
    d = open(path, "rb").read()
    i = 0
    pages = bad = seq_gaps = 0
    prev_seq = None
    gran, flags, serials = [], [], set()
    truncated = False
    while i < len(d):
        if d[i:i + 4] != b"OggS":
            j = d.find(b"OggS", i + 1)
            if j < 0:
                break
            bad += 1
            i = j
            continue
        if i + 27 > len(d):
            truncated = True
            break
        ns = d[i + 26]
        ps = 27 + ns + sum(d[i + 27:i + 27 + ns])
        if i + ps > len(d):
            truncated = True
            break
        pg = bytearray(d[i:i + ps])
        stored = struct.unpack_from("<I", pg, 22)[0]
        pg[22:26] = b"\0\0\0\0"
        if ogg_crc(pg) != stored:
            bad += 1
        seq = struct.unpack_from("<I", d, i + 18)[0]
        if prev_seq is not None and seq != prev_seq + 1:
            seq_gaps += 1
        prev_seq = seq
        pages += 1
        gran.append(struct.unpack_from("<q", d, i + 6)[0])
        flags.append(d[i + 5])
        serials.add(struct.unpack_from("<I", d, i + 14)[0])
        i += ps
    mono = all(b >= a or b == -1 for a, b in zip(gran, gran[1:]))
    print(
        f"{os.path.basename(path)}: size={len(d)} pages={pages} crc_bad={bad} "
        f"seq_gaps={seq_gaps} serials={len(serials)} gran_monotonic={mono} "
        f"last_gran_48k={gran[-1] if gran else None} "
        f"bos={sum(1 for x in flags if x & 2)} eos={sum(1 for x in flags if x & 4)} "
        f"truncated_tail={truncated}"
    )


for p in sys.argv[1:]:
    check(p)
