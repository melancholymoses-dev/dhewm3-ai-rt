#!/usr/bin/env python3
"""
Dump a draw-call census from a RenderDoc capture of dhewm3-rt, by debug-label region.

Answers optimization X7 (is geometry submitted more times than the light count explains)
without sifting thousands of events in the Event Browser.  Needs the capture to have been
taken with r_vkDebugLabels 1 — that is what turns the profiler's 26 phases into the
marker regions this walks.

Two CSVs next to the capture:
  <name>_regions.csv  one row per marker region: eid range, draws, indices, instances,
                      dispatches.  Read DepthPrepass against Interactions here.
  <name>_draws.csv    one row per draw: eid, region path, name, indices, instances.
                      Sort by indices to find the same geometry submitted twice.

Every labelled stage appears TWICE per frame: Doom 3 renders the world view and then the
2D GUI overlay as a second RC_DRAW_VIEW.  The region rows carry an `occurrence` column so
the two do not get summed — the GUI one is the near-empty second occurrence.

Usage, inside RenderDoc (Window -> Python Shell) with the capture already open.  One
line; it runs on the open capture immediately:
    exec(open(r'C:\\Users\\jonat\\source\\repos\\dhewm3_rt\\tools\\rdoc_draw_census.py').read())

Then census_in_ui() again for the next capture, or census_in_ui(r'C:\\prefix') to pick
the output path.

Usage, standalone — only works on a RenderDoc build that ships pymodules (the stable
Windows installer does not; its Python API lives inside qrenderdoc.exe).  The module is
also a version-specific C extension, so the interpreter has to match the one RenderDoc
was built against:
    $env:PYTHONPATH = "C:\\Program Files\\RenderDoc\\pymodules"
    python tools/rdoc_draw_census.py capture.rdc

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
"""

import csv
import os
import sys

try:
    import renderdoc as rd
except ImportError:
    rd = None  # set by the UI shell; checked at call time


# --- API shims.  RenderDoc renamed drawcalls to actions around 1.19 and moved names
# --- into the structured file, so probe rather than assume a version.

def _flag_enum():
    return getattr(rd, "ActionFlags", None) or rd.DrawFlags


def _root_actions(controller):
    get = getattr(controller, "GetRootActions", None) or controller.GetDrawcalls
    return get()


def _children(action):
    return getattr(action, "children", []) or []


def _name(action, sdfile):
    get = getattr(action, "GetName", None)
    if get is not None and sdfile is not None:
        try:
            return get(sdfile)
        except TypeError:
            pass
    return getattr(action, "customName", None) or getattr(action, "name", "") or ""


def _is(action, flag_name):
    flags = _flag_enum()
    flag = getattr(flags, flag_name, None)
    if flag is None:
        return False
    return bool(action.flags & flag)


# --- The walk.  One pass collects both CSVs: regions accumulate their subtree totals,
# --- draws are emitted flat with the region path they sit under.

class Census:
    def __init__(self, sdfile):
        self.sdfile = sdfile
        self.regions = []
        self.draws = []
        self._seen = {}  # region path -> occurrence count

    def walk(self, actions, path):
        """Returns (draws, indices, instances, dispatches) for this subtree."""
        totals = [0, 0, 0, 0]
        for action in actions:
            name = _name(action, self.sdfile)
            kids = _children(action)

            # A marker region is any node with children that is not itself a draw.
            # Nested regions (RT phases inside a frame label) recurse the same way.
            if kids and not _is(action, "Drawcall"):
                sub_path = path + [name] if name else path
                sub = self.walk(kids, sub_path)
                if name:
                    key = "/".join(sub_path)
                    self._seen[key] = self._seen.get(key, 0) + 1
                    first, last = self._eid_range(action, kids)
                    self.regions.append({
                        "region": key,
                        "depth": len(sub_path),
                        "occurrence": self._seen[key],
                        "eid_first": first,
                        "eid_last": last,
                        "draws": sub[0],
                        "indices": sub[1],
                        "instances": sub[2],
                        "dispatches": sub[3],
                    })
                for i in range(4):
                    totals[i] += sub[i]
                continue

            if _is(action, "Drawcall"):
                idx = int(getattr(action, "numIndices", 0) or 0)
                inst = int(getattr(action, "numInstances", 0) or 0)
                totals[0] += 1
                totals[1] += idx
                totals[2] += max(inst, 1)
                self.draws.append({
                    "eid": action.eventId,
                    "region": "/".join(path),
                    "name": name,
                    "indices": idx,
                    "instances": inst,
                })
            elif _is(action, "Dispatch"):
                totals[3] += 1

            # A draw can still have children (indirect sub-draws); fold them in.
            if kids and _is(action, "Drawcall"):
                sub = self.walk(kids, path)
                for i in range(4):
                    totals[i] += sub[i]
        return tuple(totals)

    @staticmethod
    def _eid_range(action, kids):
        first = getattr(action, "eventId", 0)
        last = first
        stack = list(kids)
        while stack:
            node = stack.pop()
            last = max(last, getattr(node, "eventId", 0))
            stack.extend(_children(node))
        return first, last


def _write(path, rows, fields):
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    print("wrote %s (%d rows)" % (path, len(rows)))


def census(controller, out_prefix):
    sdfile = None
    get_sd = getattr(controller, "GetStructuredFile", None)
    if get_sd is not None:
        sdfile = get_sd()

    walker = Census(sdfile)
    totals = walker.walk(_root_actions(controller), [])

    _write(out_prefix + "_regions.csv", walker.regions,
           ["region", "depth", "occurrence", "eid_first", "eid_last",
            "draws", "indices", "instances", "dispatches"])
    _write(out_prefix + "_draws.csv", walker.draws,
           ["eid", "region", "name", "indices", "instances"])

    print("\nframe totals: %d draws, %d indices, %d dispatches" % (totals[0], totals[1], totals[3]))

    # The X7 headline, printed so it does not need a spreadsheet to see.
    print("\ntop regions by draw count (first occurrence = world view):")
    ranked = sorted(walker.regions, key=lambda r: r["draws"], reverse=True)
    for row in ranked[:12]:
        print("  %-44s occ%d  draws=%-6d indices=%-9d" %
              (row["region"][-44:], row["occurrence"], row["draws"], row["indices"]))

    depth = next((r for r in walker.regions
                  if r["region"].endswith("DepthPrepass") and r["occurrence"] == 1), None)
    inter = next((r for r in walker.regions
                  if r["region"].endswith("Interactions") and r["occurrence"] == 1), None)
    if depth and inter and depth["draws"]:
        print("\nX7 amplification: Interactions/DepthPrepass = %.2fx (%d / %d draws)" %
              (inter["draws"] / depth["draws"], inter["draws"], depth["draws"]))
        print("Compare against lights.view from the native profiler run: an amplification")
        print("well above the light count means lights are drawing surfaces they shouldn't.")


def census_in_ui(out_prefix=None):
    """Call this from RenderDoc's Python Shell with a capture already open."""
    ctx = globals().get("pyrenderdoc")
    if ctx is None:
        print("no pyrenderdoc in scope — run this inside RenderDoc's Python Shell")
        return
    global rd
    if rd is None:
        import renderdoc as _rd
        rd = _rd
    prefix = out_prefix or os.path.splitext(ctx.GetCaptureFilename())[0]
    ctx.Replay().BlockInvoke(lambda controller: census(controller, prefix))


def main(argv):
    if rd is None:
        print("renderdoc module not found. Either add RenderDoc's pymodules dir to")
        print("PYTHONPATH, or run census_in_ui() from RenderDoc's Python Shell.")
        return 2
    if len(argv) < 2:
        print(__doc__)
        return 2

    path = argv[1]
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(path, "", None)
    if not _ok(result):
        print("could not open %s: %s" % (path, result))
        return 1
    if not cap.LocalReplaySupport():
        print("this capture cannot be replayed locally (different API or GPU)")
        return 1

    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if not _ok(result):
        print("could not replay %s: %s" % (path, result))
        return 1

    try:
        census(controller, os.path.splitext(path)[0])
    finally:
        controller.Shutdown()
        cap.Shutdown()
    return 0


def _ok(result):
    # ResultDetails (newer) exposes OK(); older builds return a ReplayStatus enum.
    check = getattr(result, "OK", None)
    if callable(check):
        return check()
    succeeded = getattr(getattr(rd, "ResultCode", None), "Succeeded", None)
    if succeeded is None:
        succeeded = rd.ReplayStatus.Succeeded
    return result == succeeded


# RenderDoc's Python Shell runs with __name__ == "__main__" and no argv, so a bare
# __main__ guard would send an exec() of this file into the usage path.  Presence of
# pyrenderdoc is what distinguishes "inside the UI" from "run as a script".
if "pyrenderdoc" in globals():
    census_in_ui()
    print("\nre-run with census_in_ui() after loading another capture,")
    print("or census_in_ui(r'C:\\some\\prefix') to choose the output path.")
elif __name__ == "__main__":
    sys.exit(main(sys.argv))
