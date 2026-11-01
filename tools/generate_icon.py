#!/usr/bin/env python3
"""Generate the AMEGA PS4 homebrew icon using only Python's standard library."""
import math
import struct
import sys
import zlib

W = H = 512
BG = (18, 21, 27, 255)
GOLD = (245, 190, 55, 255)
WHITE = (235, 239, 245, 255)

def inside_round_rect(x, y, left, top, right, bottom, radius):
    if x < left or x >= right or y < top or y >= bottom:
        return False
    cx = left + radius if x < left + radius else right - radius - 1 if x >= right - radius else x
    cy = top + radius if y < top + radius else bottom - radius - 1 if y >= bottom - radius else y
    dx, dy = x - cx, y - cy
    return dx * dx + dy * dy <= radius * radius

def inside_polygon(x, y, points):
    inside = False
    j = len(points) - 1
    for i, (xi, yi) in enumerate(points):
        xj, yj = points[j]
        if ((yi > y) != (yj > y)) and x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi:
            inside = not inside
        j = i
    return inside

FONT = {
    "A": ("01110","10001","10001","11111","10001","10001","10001"),
    "M": ("10001","11011","10101","10101","10001","10001","10001"),
    "E": ("11111","10000","10000","11110","10000","10000","11111"),
    "G": ("01111","10000","10000","10111","10001","10001","01111"),
}

def png_chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "sce_sys/icon0.png"
    pixels = bytearray(W * H * 4)
    outer = (28, 28, 484, 484)
    inner = (39, 39, 473, 473)
    main_a = [(256, 78), (392, 382), (326, 382), (256, 218), (186, 382), (120, 382)]
    cutout = [(256, 266), (229, 330), (283, 330)]
    for y in range(H):
        for x in range(W):
            color = BG
            if inside_round_rect(x, y, *outer, 48) and not inside_round_rect(x, y, *inner, 40):
                color = GOLD
            elif inside_polygon(x, y, main_a) and not inside_polygon(x, y, cutout):
                color = GOLD
            idx = (y * W + x) * 4
            pixels[idx:idx+4] = bytes(color)

    # Compact AMEGA wordmark, rendered as a tiny custom pixel font.
    scale, gap = 7, 1
    text_width = (5 * 5 + 4 * gap) * scale
    start_x = (W - text_width) // 2
    start_y = 414
    for ci, letter in enumerate("AMEGA"):
        glyph = FONT[letter]
        gx = start_x + ci * (5 + gap) * scale
        for row, bits in enumerate(glyph):
            for col, bit in enumerate(bits):
                if bit == "1":
                    for sy in range(scale):
                        for sx in range(scale):
                            x, y = gx + col * scale + sx, start_y + row * scale + sy
                            idx = (y * W + x) * 4
                            pixels[idx:idx+4] = bytes(WHITE)

    raw = bytearray()
    stride = W * 4
    for y in range(H):
        raw.append(0)
        raw.extend(pixels[y * stride:(y + 1) * stride])

    png = bytearray(b"\x89PNG\r\n\x1a\n")
    png.extend(png_chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)))
    png.extend(png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
    png.extend(png_chunk(b"IEND", b""))
    with open(path, "wb") as out:
        out.write(png)
    print(f"Generated {path} ({len(png)} bytes)")

if __name__ == "__main__":
    main()
