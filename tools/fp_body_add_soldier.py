# Adds a trimmed copy of the stock "soldiermesh" (unarmored t-shirt body) to the first-person
# body mesh base/models/sp_player.md5mesh.  mars_city1 uses skin tshirt_mp, which nodraws the
# armored body surface, so without this mesh the fp body is invisible there.
#
# Trim rule: keep a triangle only if every vertex sits at or below --cut (bind pose Z) and every
# significant weight uses a joint the fp armored body already uses (drops arms, neck, head).
# Re-running replaces any soldier mesh already in the output.
#
# Usage: python tools/fp_body_add_soldier.py <stock spplayer.md5mesh> [fp mesh] [--cut 60]
#
# This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
# and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
# Code.
#
# It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
# Code release.

import argparse
import math
import re

SOLDIER_SHADER = "models/characters/male_npc/soldier/soldier"
BODY_SHADER = "models/characters/player/body"
NUM = r"(-?[\d.eE+-]+)"


def parse_joints(text):
    block = re.search(r"joints\s*\{(.*?)\n\}", text, re.S).group(1)
    joints = []
    pat = re.compile(r'"([^"]+)"\s+(-?\d+)\s*\(\s*' + r"\s+".join([NUM] * 3) + r"\s*\)\s*\(\s*" +
                     r"\s+".join([NUM] * 3) + r"\s*\)")
    for m in pat.finditer(block):
        pos = tuple(float(m.group(i)) for i in (3, 4, 5))
        x, y, z = (float(m.group(i)) for i in (6, 7, 8))
        w = 1.0 - x * x - y * y - z * z
        w = -math.sqrt(w) if w > 0.0 else 0.0  # idTech4 stores w as the negative root
        joints.append((m.group(1), pos, (x, y, z, w)))
    return joints


def rotate(q, v):
    x, y, z, w = q
    # v + 2w(q x v) + 2 q x (q x v)
    cx, cy, cz = y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]
    cx2, cy2, cz2 = y * cz - z * cy, z * cx - x * cz, x * cy - y * cx
    return (v[0] + 2 * (w * cx + cx2), v[1] + 2 * (w * cy + cy2), v[2] + 2 * (w * cz + cz2))


def split_meshes(text):
    """Returns (header text before first mesh, list of raw mesh block strings)."""
    starts = [m.start() for m in re.finditer(r"^mesh\s*\{", text, re.M)]
    header = text[:starts[0]] if starts else text
    blocks = []
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else len(text)
        blocks.append(text[s:e].rstrip() + "\n")
    return header, blocks


def parse_mesh(block):
    shader = re.search(r'shader\s+"([^"]+)"', block).group(1)
    verts = [(float(m.group(2)), float(m.group(3)), int(m.group(4)), int(m.group(5)))
             for m in re.finditer(r"vert\s+(\d+)\s*\(\s*" + NUM + r"\s+" + NUM + r"\s*\)\s*(\d+)\s+(\d+)", block)]
    tris = [tuple(int(m.group(i)) for i in (2, 3, 4))
            for m in re.finditer(r"tri\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", block)]
    weights = [(int(m.group(2)), float(m.group(3)), tuple(float(m.group(i)) for i in (4, 5, 6)))
               for m in re.finditer(r"weight\s+(\d+)\s+(\d+)\s+" + NUM + r"\s*\(\s*" +
                                    r"\s+".join([NUM] * 3) + r"\s*\)", block)]
    return shader, verts, tris, weights


def bind_positions(joints, verts, weights):
    out = []
    for _, _, start, count in verts:
        p = [0.0, 0.0, 0.0]
        for j, bias, wp in weights[start:start + count]:
            r = rotate(joints[j][2], wp)
            jp = joints[j][1]
            for k in range(3):
                p[k] += bias * (jp[k] + r[k])
        out.append(tuple(p))
    return out


def report_body_push(joints, stock_body, fp_body):
    """Diagnostic: bind-pose offset of hand-edited fp body verts vs stock, matched by UV."""
    s_pos = bind_positions(joints, stock_body[1], stock_body[3])
    f_pos = bind_positions(joints, fp_body[1], fp_body[3])
    by_uv = {}
    for v, p in zip(stock_body[1], s_pos):
        by_uv.setdefault((round(v[0], 4), round(v[1], 4)), []).append(p)
    bands = {}
    for v, p in zip(fp_body[1], f_pos):
        cands = by_uv.get((round(v[0], 4), round(v[1], 4)))
        if not cands:
            continue
        # Mirrored UVs put both legs on one key; the nearest candidate is the same vertex.
        s = min(cands, key=lambda c: sum((c[k] - p[k]) ** 2 for k in range(3)))
        band = int(s[2] // 10) * 10
        bands.setdefault(band, []).append(tuple(p[k] - s[k] for k in range(3)))
    print("fp body vs stock body, mean bind offset by stock Z band (X = forward):")
    for band in sorted(bands):
        d = bands[band]
        mean = [sum(x[k] for x in d) / len(d) for k in range(3)]
        worst = max(math.sqrt(sum(x[k] ** 2 for k in range(3))) for x in d)
        print("  Z %3d-%3d  n=%4d  dx=%6.2f dy=%6.2f dz=%6.2f  max|d|=%5.2f" % (band, band + 10, len(d), *mean,
                                                                              worst))


def build_soldier(joints, soldier, allowed, cut, min_bias):
    _, verts, tris, weights = soldier
    pos = bind_positions(joints, verts, weights)

    def ok(vi):
        _, _, start, count = verts[vi]
        if pos[vi][2] > cut:
            return False
        return all(j in allowed for j, bias, _ in weights[start:start + count] if bias >= min_bias)

    good = [vi for vi in range(len(verts)) if ok(vi)]
    good_set = set(good)
    kept_tris = [t for t in tris if all(v in good_set for v in t)]
    used = sorted({v for t in kept_tris for v in t})
    remap = {old: new for new, old in enumerate(used)}

    lines = ["mesh {", '  shader "%s"' % SOLDIER_SHADER, "", "  numverts %d" % len(used)]
    out_weights = []
    for old in used:
        u, v, start, count = verts[old]
        lines.append("  vert %d ( %.10f %.10f ) %d %d" % (remap[old], u, v, len(out_weights), count))
        out_weights.extend(weights[start:start + count])
    lines += ["", "  numtris %d" % len(kept_tris)]
    for i, t in enumerate(kept_tris):
        lines.append("  tri %d %d %d %d" % (i, remap[t[0]], remap[t[1]], remap[t[2]]))
    lines += ["", "  numweights %d" % len(out_weights)]
    for i, (j, bias, wp) in enumerate(out_weights):
        lines.append("  weight %d %d %.10f ( %.10f %.10f %.10f )" % (i, j, bias, *wp))
    lines.append("}")
    print("soldier: %d/%d verts, %d/%d tris kept (cut Z<=%.1f)" % (len(used), len(verts), len(kept_tris),
                                                                   len(tris), cut))
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stock")
    ap.add_argument("fp", nargs="?", default="base/models/sp_player.md5mesh")
    ap.add_argument("--cut", type=float, default=60.0)
    ap.add_argument("--min-bias", type=float, default=0.1, help="ignore weights below this when joint-filtering")
    args = ap.parse_args()

    stock_text = open(args.stock).read()
    fp_text = open(args.fp).read()
    joints = parse_joints(stock_text)
    fp_joints = parse_joints(fp_text)
    if [j[0] for j in joints] != [j[0] for j in fp_joints]:
        raise SystemExit("joint names/order differ between stock and fp mesh")

    _, stock_blocks = split_meshes(stock_text)
    stock_meshes = {m[0]: m for m in map(parse_mesh, stock_blocks)}
    header, fp_blocks = split_meshes(fp_text)
    fp_meshes = [parse_mesh(b) for b in fp_blocks]
    fp_body = next(m for m in fp_meshes if m[0] == BODY_SHADER)

    allowed = {j for j, _, _ in fp_body[3]}
    print("allowed joints: " + ", ".join(joints[j][0] for j in sorted(allowed)))
    report_body_push(joints, stock_meshes[BODY_SHADER], fp_body)

    soldier = build_soldier(joints, stock_meshes[SOLDIER_SHADER], allowed, args.cut, args.min_bias)
    kept = [b for b, m in zip(fp_blocks, fp_meshes) if m[0] != SOLDIER_SHADER]
    header = re.sub(r"numMeshes\s+\d+", "numMeshes %d" % (len(kept) + 1), header)
    eol = "\r\n" if "\r\n" in open(args.fp, newline="").read(4096) else "\n"  # keep the file's line endings
    with open(args.fp, "w", newline=eol) as f:
        f.write(header + "\n".join(kept) + "\n" + soldier)
    print("wrote " + args.fp)


if __name__ == "__main__":
    main()
