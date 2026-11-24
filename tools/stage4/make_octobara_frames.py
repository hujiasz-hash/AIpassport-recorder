#!/usr/bin/env python3
"""Generate OctoBara Flash assets (requires Pillow):
1. Lossless Row-Delta-RLE RGB565 frames for the 184x192 typing animation (~105 KiB vs 690 KiB raw).
   - Frame 0: plain RGB565 (184 * 192 * 2 = 70,656 bytes), rendered directly from Flash.
   - Frames 1..9: uint16 row-offset table (9 * 192 * 2 = 3,456 bytes) + byte-aligned RLE/delta ops:
       0x80 | count (1..127): copy `count` pixels from Frame 0 at the same (x, y)
       0x40 | count (1..63):  repeat the following little-endian RGB565 color `count` times
       0x00 | count (1..63):  copy `count` literal little-endian RGB565 pixels from stream
2. Circular 96x96 RGB565 badge of headphone OctoBara composited onto HOME_BG (#F6F1E5) (18,432 bytes).
"""
import argparse
import math
from pathlib import Path
from struct import pack, unpack_from

from PIL import Image, ImageSequence

HOME_W, HOME_H, HOME_FRAMES = 184, 192, 10
REC_BADGE_W, REC_BADGE_H = 96, 96
BACKGROUND = (0xF6, 0xF1, 0xE5, 255)  # HOME_BG in app_ui.c


def rgb_to_565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def extract_home_frames(gif_path: Path) -> list[list[int]]:
    with Image.open(gif_path) as gif:
        frames = []
        for frame in ImageSequence.Iterator(gif):
            if frame.info.get("duration") != 100:
                raise ValueError("Expected 100 ms per frame")
            frames.append(frame.convert("RGBA"))
    if len(frames) != HOME_FRAMES:
        raise ValueError(f"Expected {HOME_FRAMES} frames, got {len(frames)}")
    boxes = [f.getbbox() for f in frames]
    bounds = (
        min(b[0] for b in boxes),
        min(b[1] for b in boxes),
        max(b[2] for b in boxes),
        max(b[3] for b in boxes),
    )
    out: list[list[int]] = []
    for frame in frames:
        cropped = frame.crop(bounds).resize((HOME_W, HOME_H), Image.Resampling.LANCZOS)
        paper = Image.new("RGBA", cropped.size, BACKGROUND)
        paper.alpha_composite(cropped)
        out.append([rgb_to_565(r, g, b) for r, g, b, _ in paper.getdata()])
    return out


def encode_home_rle(frames: list[list[int]]) -> bytes:
    f0 = frames[0]
    raw_f0 = pack(f"<{len(f0)}H", *f0)

    stream = bytearray()
    row_offsets: list[int] = []

    for fi in range(1, HOME_FRAMES):
        f = frames[fi]
        for y in range(HOME_H):
            if len(stream) > 0xFFFF:
                raise ValueError("Delta stream exceeded uint16 offset range")
            row_offsets.append(len(stream))
            x = 0
            base = y * HOME_W
            while x < HOME_W:
                if f[base + x] == f0[base + x]:
                    run = 0
                    while (
                        x + run < HOME_W
                        and run < 127
                        and f[base + x + run] == f0[base + x + run]
                    ):
                        run += 1
                    stream.append(0x80 | run)
                    x += run
                else:
                    c = f[base + x]
                    run = 1
                    while (
                        x + run < HOME_W
                        and run < 63
                        and f[base + x + run] == c
                    ):
                        run += 1
                    if run >= 2:
                        stream.append(0x40 | run)
                        stream.extend(pack("<H", c))
                        x += run
                    else:
                        lit = 1
                        while x + lit < HOME_W and lit < 63:
                            if (
                                x + lit + 1 < HOME_W
                                and f[base + x + lit] == f0[base + x + lit]
                                and f[base + x + lit + 1] == f0[base + x + lit + 1]
                            ):
                                break
                            if (
                                x + lit + 1 < HOME_W
                                and f[base + x + lit] == f[base + x + lit + 1]
                            ):
                                break
                            lit += 1
                        stream.append(lit)
                        for k in range(lit):
                            stream.extend(pack("<H", f[base + x + k]))
                        x += lit

    # Pad to 4-byte alignment
    while len(stream) % 4 != 0:
        stream.append(0)

    # Bit-exact self-verification before writing
    for fi in range(1, HOME_FRAMES):
        cur_frame = frames[fi]
        for y in range(HOME_H):
            pos = row_offsets[(fi - 1) * HOME_H + y]
            rec: list[int] = []
            base = y * HOME_W
            while len(rec) < HOME_W:
                op = stream[pos]
                pos += 1
                if op & 0x80:
                    cnt = op & 0x7F
                    rec.extend(f0[base + len(rec) : base + len(rec) + cnt])
                elif op & 0x40:
                    cnt = op & 0x3F
                    (c,) = unpack_from("<H", stream, pos)
                    pos += 2
                    rec.extend([c] * cnt)
                else:
                    cnt = op & 0x3F
                    rec.extend(unpack_from(f"<{cnt}H", stream, pos))
                    pos += cnt * 2
            if rec != cur_frame[base : base + HOME_W]:
                raise AssertionError(f"Bit-exact decode mismatch at frame={fi} row={y}")

    offsets_bin = pack(f"<{len(row_offsets)}H", *row_offsets)
    header = pack("<4H2I", HOME_W, HOME_H, HOME_FRAMES, 0, len(offsets_bin), len(stream))
    return header + raw_f0 + offsets_bin + bytes(stream)


def build_rec_badge(avatar_path: Path) -> bytes:
    with Image.open(avatar_path) as im:
        rgba = im.convert("RGBA")
    ss = REC_BADGE_W * 4
    cropped = rgba.crop((2, 2, rgba.width - 2, rgba.height - 2)).resize(
        (ss, ss), Image.Resampling.LANCZOS
    )
    cx = cy = (ss - 1) / 2.0
    rad = ss / 2.0 - 1.5
    alpha_bytes = bytearray(ss * ss)
    for y in range(ss):
        row = y * ss
        for x in range(ss):
            d = math.hypot(x - cx, y - cy)
            a = max(0.0, min(1.0, rad + 0.5 - d))
            alpha_bytes[row + x] = int(round(a * 255))
    mask = Image.frombytes("L", (ss, ss), bytes(alpha_bytes))

    canvas = Image.new("RGBA", (ss, ss), BACKGROUND)
    canvas.paste(cropped, (0, 0), mask)
    final = canvas.resize((REC_BADGE_W, REC_BADGE_H), Image.Resampling.LANCZOS).convert("RGB")
    pixels = [rgb_to_565(r, g, b) for r, g, b in final.getdata()]
    return pack(f"<{len(pixels)}H", *pixels)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gif", type=Path, help="01_octobara_typing_480.gif")
    parser.add_argument("home_output", type=Path, help="octobara.rgb565 (RLE-delta)")
    parser.add_argument(
        "--rec-avatar",
        type=Path,
        help="08_头像_深夜极客_连帽卫衣_512.png",
    )
    parser.add_argument(
        "--rec-output",
        type=Path,
        help="octobara_rec.rgb565 (96x96 RGB565)",
    )
    args = parser.parse_args()

    home_blob = encode_home_rle(extract_home_frames(args.gif))
    args.home_output.write_bytes(home_blob)
    print(f"Wrote {args.home_output}: {len(home_blob):,} bytes ({len(home_blob)/1024:.1f} KiB)")

    if args.rec_avatar and args.rec_output:
        rec_blob = build_rec_badge(args.rec_avatar)
        args.rec_output.write_bytes(rec_blob)
        print(f"Wrote {args.rec_output}: {len(rec_blob):,} bytes ({len(rec_blob)/1024:.1f} KiB)")
