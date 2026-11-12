#!/usr/bin/env python3
"""Converts a MAME LED layout (.lay) into an SVG screen LCD Lab can draw, and a C array.

Usage: tools/lay_to_svg.py elbaskb.lay elbaskb > src/content/builtin_elbaskb.inc

Covers the elements MAME's elbaskb.lay uses: rect, disk, led7seg and an embedded SVG
line, groups with flips and quarter turns. Text is not drawn (nanosvg has no fonts).
Lamps named x.y.z become segments, drawn after all static shapes (the layout shows either
the lit or the unlit state, never both, so this is the same picture); 7-segment digits are mapped to the MCU outputs
with elbaskb_state::update_display's formula. The layout is CC0-1.0 (hap).
"""
import math
import re
import sys
import xml.etree.ElementTree as ET


def num(e, k, d=0.0):
    return float(e.get(k, d))


def bounds(b):
    w, h = num(b, "width"), num(b, "height")
    x = num(b, "xc") - w / 2 if "xc" in b.attrib else num(b, "x")
    y = num(b, "yc") - h / 2 if "yc" in b.attrib else num(b, "y")
    if "left" in b.attrib:
        x, y = num(b, "left"), num(b, "top")
        w, h = num(b, "right") - x, num(b, "bottom") - y
    return x, y, w, h


def orient(o):
    """MAME orientation as (swap_xy, flip_x, flip_y)."""
    if o is None:
        return (False, False, False)
    rot = int(o.get("rotate", "0"))
    swap, fx, fy = {0: (False, False, False), 90: (True, True, False), 180: (False, True, True),
                    270: (True, False, True)}[rot]
    if o.get("flipx") == "yes":
        fx = not fx
    if o.get("flipy") == "yes":
        fy = not fy
    return (swap, fx, fy)


def mapper(box, o):
    """Unit square content -> layout coordinates."""
    x, y, w, h = box
    swap, fx, fy = o

    def f(u, v):
        if swap:
            u, v = v, u
        if fx:
            u = 1 - u
        if fy:
            v = 1 - v
        return x + u * w, y + v * h
    return f


def colour(c):
    r, g, b = (int(round(255 * float(c.get(k, "1")))) for k in ("red", "green", "blue"))
    return "#%02x%02x%02x" % (r, g, b), float(c.get("alpha", "1"))


def poly(points, fill, alpha, title=None):
    d = "M" + " L".join("%.3f %.3f" % p for p in points) + " Z"
    t = "<title>%s</title>" % title if title else ""
    op = ' fill-opacity="%.3f"' % alpha if alpha < 1 else ""
    return '<path d="%s" fill="%s"%s>%s</path>' % (d, fill, op, t)


def disk(f, fill, alpha, title=None, n=24):
    return poly([f(0.5 + 0.5 * math.cos(2 * math.pi * i / n), 0.5 + 0.5 * math.sin(2 * math.pi * i / n))
                 for i in range(n)], fill, alpha, title)


# 7-segment geometry in a 0..1 box: a, b, c, d, e, f, g
SEG7 = [[(0.18, 0.06), (0.82, 0.06), (0.74, 0.12), (0.26, 0.12)],
        [(0.84, 0.08), (0.84, 0.48), (0.78, 0.45), (0.78, 0.14)],
        [(0.84, 0.52), (0.84, 0.92), (0.78, 0.86), (0.78, 0.55)],
        [(0.18, 0.94), (0.82, 0.94), (0.74, 0.88), (0.26, 0.88)],
        [(0.16, 0.52), (0.16, 0.92), (0.22, 0.86), (0.22, 0.55)],
        [(0.16, 0.08), (0.16, 0.48), (0.22, 0.45), (0.22, 0.14)],
        [(0.2, 0.5), (0.26, 0.47), (0.74, 0.47), (0.8, 0.5), (0.74, 0.53), (0.26, 0.53)]]


def digit_key(i, seg):
    """elbaskb_state::update_display: digit i, segment seg -> MCU output x.y.z"""
    return "%d.%d.%d" % (((i << 1) | (seg >> 2 ^ 1)) & 3, seg & 3, i >> 1 ^ 1)


def main():
    root = ET.parse(sys.argv[1]).getroot()
    name = sys.argv[2]
    elements = {e.get("name"): e for e in root.findall("element")}
    groups = {g.get("name"): g for g in root.findall("group")}
    out, lit = [], []

    def draw_element(ref, f, lamp):
        el = elements[ref]
        for comp in el:
            if comp.tag in ("rect", "disk"):
                c = comp.find("color")
                fill, alpha = colour(c) if c is not None else ("#ffffff", 1.0)
                state = comp.get("state")
                if lamp is None or state is None:
                    shape = disk(f, fill, alpha) if comp.tag == "disk" else \
                        poly([f(0, 0), f(1, 0), f(1, 1), f(0, 1)], fill, alpha)
                    out.append(shape)
                elif state == "0":
                    out.append(disk(f, fill, alpha))
                elif state == "1":
                    lit.append(disk(f, fill, alpha, lamp))
            elif comp.tag == "led7seg":
                fill, _ = colour(comp.find("color"))
                i = int(lamp[5:])
                for s, pts in enumerate(SEG7):
                    out.append(poly([f(u, v) for u, v in pts], fill, 0.125))
                    lit.append(poly([f(u, v) for u, v in pts], fill, 1.0, digit_key(i, s)))
            elif comp.tag == "image":
                rect = re.search(r'<rect width="([\d.]+)" height="([\d.]+)"[^>]*rotate\(([\d.]+)\)[^>]*fill="(#\w+)"',
                                 comp.find("data").text)
                t = math.radians(float(rect.group(3)))
                th = float(rect.group(2)) / 100
                ex = (1.0, math.tan(t))
                nx, ny = -math.sin(t) * th, math.cos(t) * th
                pts = [(0, 0), ex, (ex[0] + nx, ex[1] + ny), (nx, ny)]
                out.append(poly([f(u, v) for u, v in pts], rect.group(4), 1.0))

    def walk(node, f_outer):
        for item in node:
            b = item.find("bounds")
            if item.tag not in ("element", "group") or b is None:
                continue
            x, y, w, h = bounds(b)
            p0, p1 = f_outer(x, y), f_outer(x + w, y + h)
            box = (min(p0[0], p1[0]), min(p0[1], p1[1]), abs(p1[0] - p0[0]), abs(p1[1] - p0[1]))
            o = orient(item.find("orientation"))
            if item.tag == "element":
                draw_element(item.get("ref"), mapper(box, o), item.get("name"))
            else:
                g = groups[item.get("ref")]
                gx, gy, gw, gh = bounds(g.find("bounds")) if g.find("bounds") is not None else (0, 0, 1, 1)
                if g.find("bounds") is None:
                    xs = [bounds(e.find("bounds")) for e in g if e.find("bounds") is not None]
                    gx, gy = min(a[0] for a in xs), min(a[1] for a in xs)
                    gw = max(a[0] + a[2] for a in xs) - gx
                    gh = max(a[1] + a[3] for a in xs) - gy
                outer = mapper(box, o)
                walk(g, lambda u, v, gx=gx, gy=gy, gw=gw, gh=gh, outer=outer: outer((u - gx) / gw, (v - gy) / gh))

    view = root.find("view")
    vx, vy, vw, vh = bounds(view.find("bounds"))
    walk(view, lambda u, v: (u, v))
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="%.3f %.3f %.3f %.3f">'
           '<rect x="%.3f" y="%.3f" width="%.3f" height="%.3f" fill="#000000"/>%s</svg>'
           % (round(vw * 10), round(vh * 10), vx, vy, vw, vh, vx, vy, vw, vh, "".join(out + lit)))
    data = svg.encode()
    print("/* Generated by tools/lay_to_svg.py from MAME 0.289 src/mame/layout/%s.lay (CC0-1.0, hap). */" % name)
    print("const char lcd_builtin_%s_svg[%d] = {" % (name, len(data) + 1))
    for i in range(0, len(data), 24):
        print("    " + ", ".join("0x%02x" % c for c in data[i:i + 24]) + ",")
    print("    0x00};")
    sys.stderr.write("%s: %dx%d, %d bytes\n" % (name, round(vw * 10), round(vh * 10), len(data)))


if __name__ == "__main__":
    main()
