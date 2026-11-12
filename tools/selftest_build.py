#!/usr/bin/env python3
"""Builds the original LCD Lab SM5A self-test ROM set (no Nintendo code or art).

The SM5xx program counter low bits are a 6-bit LFSR, so consecutive
instructions are placed at successive LFSR positions within a 64-byte page.
Writes roms/lcdlab_selftest.zip and tests/fixtures/,
and prints the CRC32 and SHA-1 values used in src/content/registry.c.
With --gnw DIR it writes only the .gnw test fixtures into DIR.
"""
import hashlib
import os
import sys
import zipfile
import zlib

ROM_SIZE = 0x740
PAGE = 0x3C0  # SM5A reset vector: PU=0, PM=0xF, PL=0

OPS = {
    "SKIP": 0x00, "ATR": 0x01, "ATBP": 0x03, "COMA": 0x0A, "TAL": 0x50, "TB": 0x51,
    "TIS": 0x58, "TW": 0x5C, "WR": 0x62, "WS": 0x63, "KTA": 0x6A,
}


def lfsr_order():
    pl, seen = 0, []
    while pl not in seen:
        seen.append(pl)
        feed = 0 if ((pl >> 1 ^ pl) & 1) else 0x20
        pl = feed | (pl >> 1 & 0x1F)
    return seen


PROGRAM = """
main:   TIS
        TR tick
        TR show
tick:   LB 0
        LDA 0
        ADX 1
        SKIP
        EXC 0
show:   LB 0
        LDA 0
        WS
        KTA
        WR
        LDA 0
        COMA
        WS
        KTA
        COMA
        WR
        LAX 5
        WS
        LAX 10
        WR
        LAX 15
        WS
        LAX 0
        WR
        LDA 0
        WS
        TW
        TAL
        TR beep
        TB
        TR beep
        LAX 1
        ATR
        TR main
beep:   LB 0x41
        LDA 0
        COMA
        EXC 0
        ATR
        TR main
"""


def assemble(source):
    lines = []
    for raw in source.strip().splitlines():
        label, _, rest = raw.partition(":") if ":" in raw else ("", "", raw)
        parts = rest.split()
        lines.append((label.strip(), parts))
    order = lfsr_order()
    if len(lines) > len(order):
        sys.exit("program does not fit in one page")
    labels = {lab: order[i] for i, (lab, _) in enumerate(lines) if lab}
    page = [0x00] * 64
    for i, (_, parts) in enumerate(lines):
        mnem, args = parts[0], parts[1:]
        val = lambda a: labels[a] if a in labels else int(a, 0)
        if mnem in OPS:
            op = OPS[mnem]
        elif mnem == "TR":
            op = 0x80 | val(args[0])
        elif mnem == "LB":
            op = 0x40 | (val(args[0]) & 0xF)
        elif mnem == "LDA":
            op = 0x18 | val(args[0])
        elif mnem == "EXC":
            op = 0x10 | val(args[0])
        elif mnem == "ADX":
            op = 0x30 | val(args[0])
        elif mnem == "LAX":
            op = 0x20 | val(args[0])
        else:
            sys.exit("unknown mnemonic " + mnem)
        page[order[i]] = op
    rom = bytearray(ROM_SIZE)
    rom[PAGE:PAGE + 64] = bytes(page)
    return bytes(rom)


def svg():
    cols, rows, cell, pad = 9, 8, 80, 12
    w, h = cols * cell, rows * cell
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">',
           f'<rect x="0" y="0" width="{w}" height="{h}" fill="#ffffff"/>',
           f'<rect x="4" y="4" width="{w - 8}" height="{h - 8}" fill="none" stroke="#c0c0c0" stroke-width="2"/>']
    for x in range(cols):
        for z in range(2):
            for y in range(4):
                r = z * 4 + y
                cx, cy = x * cell + pad, r * cell + pad
                size = cell - 2 * pad
                if (x + y) % 2:
                    out.append(f'<g><title>{x}.{y}.{z}</title><circle cx="{cx + size / 2}" cy="{cy + size / 2}" '
                               f'r="{size / 2}" fill="#101010"/></g>')
                else:
                    out.append(f'<rect x="{cx}" y="{cy}" width="{size}" height="{size}" rx="6" fill="#101010">'
                               f'<title>{x}.{y}.{z}</title></rect>')
    out.append("</svg>")
    return ("\n".join(out) + "\n").encode()


def png(width, height, pixel, colour_type=6, palette=None):
    """Writes a PNG, cycling the row filter through all five types."""
    import struct
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour_type]
    raw, prev = bytearray(), bytes(width * channels)
    for y in range(height):
        row = bytes(v for x in range(width) for v in pixel(x, y))
        ftype = y % 5
        out = bytearray()
        for i, v in enumerate(row):
            a = out_left = row[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if ftype == 0:
                p = 0
            elif ftype == 1:
                p = a
            elif ftype == 2:
                p = b
            elif ftype == 3:
                p = (a + b) // 2
            else:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                p = a if pa <= pb and pa <= pc else b if pb <= pc else c
            out.append((v - p) & 0xFF)
        raw += bytes([ftype]) + out
        prev = row

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, colour_type, 0, 0, 0))
    if palette:
        data += chunk(b"PLTE", bytes(v for rgb in palette for v in rgb))
    return data + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")


def background_png():
    """Original test background: a warm horizontal ramp with a dark frame."""
    def pixel(x, y):
        if x < 8 or y < 8 or x >= 232 or y >= 200:
            return (60, 40, 30, 255)
        return (220 - x // 4, 200, 150 + y // 8, 255)
    return png(240, 208, pixel)


def gnw_file(rom):
    """Game & Watch FPGA .gnw (format v1) for the self-test: same grid as the SVG, drawn at 720x720."""
    import struct
    side, cols, rows = 720, 9, 8
    cell = 80
    bg, ids = bytearray(side * side * 3), [None] * (side * side)
    for i in range(side * side):
        bg[i * 3:i * 3 + 3] = b"\xc8\xc8\xb4"
    mask = bytearray(bg)
    for x in range(cols):
        for z in range(2):
            for y in range(4):
                r = z * 4 + y
                for py in range(r * cell + 12, r * cell + 68):
                    for px in range(x * cell + 12, x * cell + 68):
                        i = py * side + px
                        mask[i * 3:i * 3 + 3] = b"\x10\x10\x10"
                        ids[i] = x << 6 | y << 2 | z
    header = bytearray([1, 4, 0]) + (720 | 720 << 10).to_bytes(3, "little") + b"\0\0"
    s_ports = [0x8C, 0x8E, 0x8D, 0x7F] + [0x7F] * 28  # IN.0: Time, Game B, Game A
    header += bytes(s_ports) + bytes([0x82, 0x83, 0x0F, 1]) + b"\0" * 4
    header += b"\0" * (0x79 - len(header)) + b"lcdlab0"
    header += b"\0" * (0x100 - len(header))
    image = bytearray(side * side * 6)
    image[0::2] = bg
    image[1::2] = mask
    runs = bytearray()
    for py in range(side):
        px = 0
        while px < side:
            i = py * side + px
            if ids[i] is None:
                px += 1
                continue
            start, sid = px, ids[i]
            while px < side and ids[py * side + px] == sid:
                px += 1
            v = sid | start << 10 | py << 20 | (px - start) << 30
            runs += v.to_bytes(5, "little")
    runs += b"\0" * (0x2DB40 - len(runs))
    return bytes(header + image + runs + rom)


def write_zip(path, members):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in members:
            info = zipfile.ZipInfo(name, date_time=(2026, 11, 12, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)


def write_gnw(directory, rom):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "selftest.gnw"), "wb") as f:
        f.write(gnw_file(rom))
    unknown = bytearray(rom)
    unknown[1] = 0x02  # not in the registry: the header describes the machine
    with open(os.path.join(directory, "unknown.gnw"), "wb") as f:
        f.write(gnw_file(bytes(unknown)))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    rom, art = assemble(PROGRAM), svg()
    if len(sys.argv) == 3 and sys.argv[1] == "--gnw":
        write_gnw(sys.argv[2], rom)
        return
    path = os.path.join(root, "roms", "lcdlab_selftest.zip")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in (("lcdlab-sm5a.bin", rom), ("lcdlab_selftest.svg", art)):
            info = zipfile.ZipInfo(name, date_time=(2026, 11, 12, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    fixtures = os.path.join(root, "tests", "fixtures")
    os.makedirs(fixtures, exist_ok=True)
    write_zip(os.path.join(fixtures, "selftest_background.zip"),
              [("lcdlab-sm5a.bin", rom), ("lcdlab_selftest.svg", art), ("background.png", background_png())])
    # split set: the clone archive holds only its own program ROM, shared files stay in the parent
    clone = bytearray(rom)
    clone[0] = 0x01  # unused byte, different CRC
    clone = bytes(clone)
    split = os.path.join(fixtures, "split")
    os.makedirs(split, exist_ok=True)
    write_zip(os.path.join(split, "lcdlab_selftest.zip"),
              [("lcdlab-sm5a.bin", rom), ("lcdlab_selftest.svg", art), ("background.png", background_png())])
    write_zip(os.path.join(split, "lcdlab_selftest_b.zip"), [("lcdlab-sm5a-b.bin", clone)])
    # clone that shares the parent's program ROM: its archive holds only its own screen
    write_zip(os.path.join(split, "lcdlab_selftest_c.zip"), [("lcdlab_selftest_c.svg", art)])
    print(f"clone rom: crc=0x{zlib.crc32(clone):08x} sha1={hashlib.sha1(clone).hexdigest()}")
    for name, data in (("rom", rom), ("svg", art)):
        print(f"{name}: size={len(data)} crc=0x{zlib.crc32(data):08x} sha1={hashlib.sha1(data).hexdigest()}")
    print("wrote", path)


if __name__ == "__main__":
    main()
