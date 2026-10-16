#!/usr/bin/env python3
"""
Read a RenderDoc Quad Overdraw overlay saved as EXR and report where the fragment
cost actually is.

RenderDoc's overlay holds the literal per-pixel layer count in every channel, but the
Texture Viewer has no legend and its default 0-1 range saturates everything above the
first layer, so the on-screen colours carry no scale.  This prints the distribution and
re-renders the image with a readable ramp.

Outputs next to the input:
  <name>_heat.png   the count with a discrete colour ramp and a legend strip
  <name>_tail.png   a mask of pixels at or above --tail, i.e. where the cost concentrates

Two numbers matter more than the mean.  Pixels% says how much screen is deep; fragments%
says how much of the frame's shading those pixels actually buy — a small deep region can
outweigh a large shallow one, and only the second number ranks a fix.

Save the overlay from RenderDoc as EXR with no compression (Save Texture -> .exr).  JPEG
and PNG cannot carry the counts: they are 8-bit, and the alpha handling composites the
viewer's checkerboard into the image.

Usage:
    python tools/rdoc_overdraw.py tools/output/r2r2.exr
    python tools/rdoc_overdraw.py tools/output/r2r2.exr --tail 12 --prepass 1

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
"""

import argparse
import os
import struct
import zlib

import numpy as np
from PIL import Image, ImageDraw


def read_exr(path):
    """Minimal scanline EXR reader: uncompressed, ZIPS and ZIP, half or float."""
    buf = open(path, "rb").read()
    if struct.unpack_from("<I", buf, 0)[0] != 20000630:
        raise ValueError("not an EXR file")
    off = 8

    attrs = {}
    while True:
        end = buf.index(b"\0", off)
        name = buf[off:end].decode()
        off = end + 1
        if not name:
            break
        end = buf.index(b"\0", off)
        atype = buf[off:end].decode()
        off = end + 1
        size = struct.unpack_from("<i", buf, off)[0]
        off += 4
        attrs[name] = (atype, buf[off:off + size])
        off += size

    chans = []
    cbuf = attrs["channels"][1]
    i = 0
    while i < len(cbuf) - 1:
        e = cbuf.index(b"\0", i)
        chans.append((cbuf[i:e].decode(), struct.unpack_from("<i", cbuf, e + 1)[0]))
        i = e + 1 + 16

    x0, y0, x1, y1 = struct.unpack_from("<4i", attrs["dataWindow"][1], 0)
    w, h = x1 - x0 + 1, y1 - y0 + 1
    comp = attrs["compression"][1][0]
    rows = {0: 1, 1: 1, 2: 1, 3: 16, 4: 32}.get(comp)
    if rows is None:
        raise ValueError("unsupported EXR compression %d — re-save with none/ZIP" % comp)

    nchunks = (h + rows - 1) // rows
    offsets = struct.unpack_from("<%dQ" % nchunks, buf, off)
    psize = {0: 4, 1: 2, 2: 4}
    rowbytes = sum(psize[t] for _, t in chans) * w

    out = np.empty((h, w), dtype=np.float32)
    cname, ctype = chans[0]  # all channels carry the same count
    for c in range(nchunks):
        p = offsets[c]
        y, n = struct.unpack_from("<ii", buf, p)
        raw = buf[p + 8:p + 8 + n]
        if comp != 0:
            raw = _unzip(raw)
        for r in range(min(rows, h - (y - y0))):
            seg = raw[r * rowbytes:r * rowbytes + psize[ctype] * w]
            dt = np.float16 if ctype == 1 else np.float32
            out[y - y0 + r] = np.frombuffer(seg, dtype=dt).astype(np.float32)
    return out


def _unzip(data):
    d = bytearray(zlib.decompress(data))
    for i in range(1, len(d)):
        d[i] = (d[i - 1] + d[i] - 128) & 0xFF
    out = bytearray(len(d))
    half = (len(d) + 1) // 2
    out[0::2] = d[:half]
    out[1::2] = d[half:]
    return bytes(out)


# Discrete ramp: blue (cheap) through green/yellow to red (expensive), 16 steps then clamp.
RAMP = [
    (30, 40, 90), (40, 80, 170), (40, 130, 210), (40, 180, 200),
    (50, 200, 150), (90, 215, 90), (160, 225, 60), (215, 220, 40),
    (245, 195, 40), (250, 160, 40), (250, 120, 40), (245, 80, 50),
    (235, 45, 60), (210, 30, 90), (180, 25, 130), (150, 20, 170),
]


def colorize(vals):
    idx = np.clip(np.rint(vals).astype(int) - 1, 0, len(RAMP) - 1)
    lut = np.array(RAMP, dtype=np.uint8)
    return lut[idx]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exr")
    ap.add_argument("--tail", type=int, default=12, help="mask pixels at or above this depth")
    ap.add_argument("--prepass", type=int, default=1,
                    help="layers to attribute to the depth prepass before reporting")
    args = ap.parse_args()

    vals = read_exr(args.exr)
    v = vals.ravel()
    total = v.size
    counts = np.bincount(np.clip(np.rint(v), 0, 255).astype(int), minlength=66)
    frags = counts * np.arange(len(counts))
    total_frags = frags.sum()

    print("%dx%d   min %.0f  max %.0f  mean %.2f  (prepass offset %d -> interaction mean %.2f)"
          % (vals.shape[1], vals.shape[0], v.min(), v.max(), v.mean(),
             args.prepass, v.mean() - args.prepass))
    print("total fragment invocations: %.1f M" % (total_frags / 1e6))

    buckets = [(1, 2), (3, 3), (4, 5), (6, 7), (8, 10), (11, 15), (16, 24), (25, 10**6)]
    print("\n depth   pixels%    fragments%   cumulative frag%")
    cum = 0.0
    for lo, hi in buckets:
        hi = min(hi, len(counts) - 1)
        px = counts[lo:hi + 1].sum()
        fr = frags[lo:hi + 1].sum()
        if px == 0:
            continue
        fpct = 100.0 * fr / total_frags
        cum += fpct
        label = "%d-%d" % (lo, hi) if lo != hi else "%d" % lo
        print("  %-6s %7.2f%%   %8.2f%%   %8.2f%%" % (label, 100.0 * px / total, fpct, cum))

    base = os.path.splitext(args.exr)[0]

    # Heat map with a legend strip along the bottom.
    rgb = colorize(vals)
    img = Image.fromarray(rgb)
    strip_h = 48
    out = Image.new("RGB", (img.width, img.height + strip_h), (20, 20, 20))
    out.paste(img, (0, 0))
    draw = ImageDraw.Draw(out)
    cell = img.width // len(RAMP)
    for i, col in enumerate(RAMP):
        x = i * cell
        draw.rectangle([x, img.height + 6, x + cell - 2, img.height + 30], fill=col)
        label = str(i + 1) if i < len(RAMP) - 1 else "%d+" % len(RAMP)
        draw.text((x + 6, img.height + 32), label, fill=(230, 230, 230))
    out.save(base + "_heat.png")
    print("\nwrote %s_heat.png  (legend = layers, 16+ clamped)" % os.path.basename(base))

    # Tail mask: where the deep pixels are, over a dimmed copy for context.
    mask = vals >= args.tail
    dim = (rgb * 0.25).astype(np.uint8)
    dim[mask] = (255, 40, 40)
    Image.fromarray(dim).save(base + "_tail.png")
    print("wrote %s_tail.png  (red = depth >= %d, %.2f%% of screen, %.1f%% of fragments)"
          % (os.path.basename(base), args.tail,
             100.0 * mask.mean(), 100.0 * frags[args.tail:].sum() / total_frags))


if __name__ == "__main__":
    main()
