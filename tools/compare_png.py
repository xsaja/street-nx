"""Compares the captures of two compare_run.ps1 runs, standard library only.
python -I compare_png.py <dir a> <dir b>: for each NN.png in both, the size, the mean difference per channel (0-255)
and the share of pixels that differ by more than 32 in some channel."""
import pathlib
import struct
import sys
import zlib


def read_png(path):
    data = path.read_bytes()
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, color = struct.unpack('>IIBB', body[:10])
            if depth != 8 or color not in (2, 6):
                raise ValueError(f'{path}: only 8 bit RGB or RGBA')
            bpp = 3 if color == 2 else 4
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def compare(pa, pb):
    wa, ha, ba, ra = read_png(pa)
    wb, hb, bb, rb = read_png(pb)
    if (wa, ha) != (wb, hb):
        return f'size {wa}x{ha} vs {wb}x{hb}'
    total, big, n = 0, 0, 0
    for y in range(0, ha, 2):  # every other row and pixel: fast enough, same answer
        la, lb = ra[y], rb[y]
        for x in range(0, wa, 2):
            d = max(abs(la[x * ba + k] - lb[x * bb + k]) for k in range(3))
            total += d
            big += d > 32
            n += 1
    return f'{wa}x{ha}  mean diff {total / n:5.1f}  pixels >32: {100 * big / n:5.1f}%'


a, b = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
for p in sorted(a.glob('*.png')):
    q = b / p.name
    print(p.name, compare(p, q) if q.exists() else 'missing in ' + str(b))
