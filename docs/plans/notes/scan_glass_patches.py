"""Scan Doom 3 .map sources for glass-material Bezier patches and classify how each
one curves.  Evidence base for docs/plans/20261013_glass_geometric_normal.md.

  CREASE  collinear control points in every span -> geometrically piecewise-flat, so
          any normal variation was invented by idSurface_Patch::GenerateNormals
  ARC     real curvature; the smoothed normal is correct
  MIXED   both, within one patch -> normal choice must be per-hit, not per-surface

Usage: python scan_glass_patches.py <dir containing .map files>

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
"""
import glob, math, os, re, sys

PT = re.compile(r'\(\s*(-?[\d.eE+-]+)\s+(-?[\d.eE+-]+)\s+(-?[\d.eE+-]+)\s+(-?[\d.eE+-]+)\s+(-?[\d.eE+-]+)\s*\)')

def sub(a, b): return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def dot(a, b): return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]
def norm(a):
    l = math.sqrt(dot(a, a))
    return None if l < 1e-9 else (a[0]/l, a[1]/l, a[2]/l)

def grid_normals(g):
    """Per-interior-vertex normals from the control grid (cross of row/col spans)."""
    h, w = len(g), len(g[0])
    out = []
    for r in range(h):
        for c in range(w):
            du = sub(g[r][min(c+1, w-1)], g[r][max(c-1, 0)])
            dv = sub(g[min(r+1, h-1)][c], g[max(r-1, 0)][c])
            n = norm(cross(du, dv))
            if n: out.append(n)
    return out

def max_angle(ns):
    worst = 0.0
    for i in range(len(ns)):
        for j in range(i+1, len(ns)):
            d = abs(dot(ns[i], ns[j]))          # abs: ignore winding/side
            worst = max(worst, math.degrees(math.acos(min(1.0, d))))
    return worst

def extent(g):
    pts = [p for row in g for p in row]
    return max(math.sqrt(dot(sub(a, b), sub(a, b))) for a in pts for b in pts)

def scan(path):
    lines = open(path, 'r', errors='replace').read().split('\n')
    hits, i = [], 0
    while i < len(lines):
        if lines[i].strip() in ('patchDef2', 'patchDef3'):
            kind = lines[i].strip()
            j = i + 1
            while j < len(lines) and lines[j].strip() != '{': j += 1
            mat = lines[j+1].strip().strip('"')
            dims = re.findall(r'-?\d+', lines[j+2])
            # collect control points until the row block closes
            depth, pts, k = 0, [], j + 3
            while k < len(lines):
                s = lines[k].strip()
                if s == '}' and depth == 0: break
                for m in PT.finditer(s):
                    pts.append(tuple(float(m.group(q)) for q in (1, 2, 3)))
                k += 1
            if 'glass' in mat.lower() and len(dims) >= 2:
                # patchDef2 dims are ( rows cols ... ) — rows first.
                h, w = int(dims[0]), int(dims[1])
                if w * h == len(pts) and w >= 2 and h >= 2:
                    g = [pts[r*w:(r+1)*w] for r in range(h)]
                    ns = grid_normals(g)
                    if len(ns) >= 2:
                        ang = max_angle(ns)
                        ext = extent(g)
                        hits.append((ang, ext, mat, kind, w, h, g[0][0], g))
            i = k
        i += 1
    return hits

rows = []
for f in sorted(glob.glob(sys.argv[1] + '/*.map')):
    for hit in scan(f):
        rows.append((os.path.basename(f),) + hit)

def span_shape(g):
    """A patchDef2 is quadratic Bezier spans of 3 control points.  Collinear
    control points => that span is geometrically FLAT, so any normal variation
    across it was invented by GenerateNormals' smoothing (a crease).  Curved
    control points => real surface curvature, where smoothing is correct."""
    h, w = len(g), len(g[0])
    flat = curved = 0
    for r in range(h):                                  # spans along each row
        for c in range(0, w - 2, 2):
            a, b, d = g[r][c], g[r][c+1], g[r][c+2]
            ln = norm(cross(sub(b, a), sub(d, a)))
            (flat if ln is None else curved).__class__   # no-op, keep both named
            if ln is None: flat += 1
            else: curved += 1
    for c in range(w):                                  # spans along each column
        for r in range(0, h - 2, 2):
            a, b, d = g[r][c], g[r+1][c], g[r+2][c]
            ln = norm(cross(sub(b, a), sub(d, a)))
            if ln is None: flat += 1
            else: curved += 1
    return flat, curved

def classify(ang, ext, g):
    if ang < 1.0: return 'FLAT'
    flat, curved = span_shape(g)
    return 'CREASE' if curved == 0 else ('MIXED' if flat else 'ARC')

rows.sort(key=lambda r: -r[1])
print(f"{'map':<17} {'ang':>7} {'ext':>6} {'grid':>6} {'origin':<23} {'class':<7} material")
for f, ang, ext, mat, kind, w, h, o, g in rows:
    org = f"({o[0]:.0f} {o[1]:.0f} {o[2]:.0f})"
    print(f"{f:<17} {ang:>6.1f}d {ext:>6.0f} {h}x{w:<4} {org:<23} "
          f"{classify(ang, ext, g):<7} {mat}")

tot = len(rows)
print(f"\n{tot} glass patches in {len(set(r[0] for r in rows))} maps")
for c in ('FLAT', 'CREASE', 'MIXED', 'ARC'):
    n = sum(1 for r in rows if classify(r[1], r[2], r[8]) == c)
    print(f"  {c:<7} {n:>4}  ({100.0*n/tot:.0f}%)")
