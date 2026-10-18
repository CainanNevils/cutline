"""Generates src/app/cutline.ico (original artwork): blue rounded square, white chevron and cut.
Pure Python (zlib + struct), PNG-compressed ICO with 16/24/32/48/64/256 px images."""
import math
import struct
import sys
import zlib


def render(size):
    px = bytearray(size * size * 4)
    s = size / 24.0

    def put(x, y, r, g, b, a):
        i = (y * size + x) * 4
        # alpha-over onto existing
        ea = px[i + 3] / 255.0
        na = a + ea * (1 - a)
        if na <= 0:
            return
        for k, c in enumerate((r, g, b)):
            px[i + k] = int((c * a + px[i + k] * ea * (1 - a)) / na)
        px[i + 3] = int(na * 255)

    def coverage(fn, x, y):
        n = 4
        hit = 0
        for sy in range(n):
            for sx in range(n):
                if fn((x + (sx + 0.5) / n) / s, (y + (sy + 0.5) / n) / s):
                    hit += 1
        return hit / (n * n)

    def rounded(u, v):
        x0, y0, x1, y1, r = 2, 3, 22, 21, 3.5
        if u < x0 or u > x1 or v < y0 or v > y1:
            return False
        cx = min(max(u, x0 + r), x1 - r)
        cy = min(max(v, y0 + r), y1 - r)
        return (u - cx) ** 2 + (v - cy) ** 2 <= r * r

    def seg_dist(u, v, ax, ay, bx, by):
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((u - ax) * dx + (v - ay) * dy) / (dx * dx + dy * dy)))
        return math.hypot(u - ax - t * dx, v - ay - t * dy)

    def mark(u, v):
        w = 1.25
        return (seg_dist(u, v, 7.5, 7.5, 11.5, 12) < w or seg_dist(u, v, 11.5, 12, 7.5, 16.5) < w
                or seg_dist(u, v, 13, 7, 16.5, 17) < w)

    for y in range(size):
        for x in range(size):
            a = coverage(rounded, x, y)
            if a > 0:
                put(x, y, 0x2D, 0x8C, 0xEB, a)
            m = coverage(mark, x, y)
            if m > 0:
                put(x, y, 255, 255, 255, m)
    return bytes(px)


def png(size, rgba):
    raw = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(out):
    sizes = [16, 24, 32, 48, 64, 256]
    images = [png(s, render(s)) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, data in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(out, "wb") as f:
        f.write(header + entries + b"".join(images))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "src/app/cutline.ico")
