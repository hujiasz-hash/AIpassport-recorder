#!/usr/bin/env python3
"""Convert the OctoBara typing GIF to flash-backed RGB565 frames (requires Pillow)."""
import argparse
from pathlib import Path
from struct import pack

from PIL import Image, ImageSequence

WIDTH, HEIGHT = 184, 192
BACKGROUND = (246, 241, 229, 255)  # HOME_BG in app_ui.c


def convert(source):
    with Image.open(source) as gif:
        frames = []
        for frame in ImageSequence.Iterator(gif):
            if frame.info.get("duration") != 100:
                raise ValueError("Expected 100 ms per frame")
            frames.append(frame.convert("RGBA"))
    if len(frames) != 10:
        raise ValueError("Expected 10 frames")
    boxes = [f.getbbox() for f in frames]
    bounds = (min(b[0] for b in boxes), min(b[1] for b in boxes),
              max(b[2] for b in boxes), max(b[3] for b in boxes))
    result = []
    for frame in frames:
        frame = frame.crop(bounds).resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        paper = Image.new("RGBA", frame.size, BACKGROUND)
        paper.alpha_composite(frame)
        pixels = [(r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
                  for r, g, b, _ in paper.getdata()]
        result.append(pack(f"<{len(pixels)}H", *pixels))
    if len(set(result)) < 2:
        raise ValueError("Expected visible changes after conversion")
    return b"".join(result)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gif", type=Path, help="01_octobara_typing_480.gif")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_bytes(convert(args.gif))
