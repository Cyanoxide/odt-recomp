#!/usr/bin/env python3
"""Native-code coverage for the README progress bar.

ODT's executable code lives in three images (the whole disc contains only two
PS-X EXE headers; MENU.BIN is a raw blob loaded as code):

    SLUS_006.98   614,400 B   main executable, statically recompiled at build time
    MOVIES.EXE    200,704 B   boot/FMV overlay, loaded to 0x80170000
    RSC/MENU.BIN   61,480 B   menu code, loaded to 0x800B4000

Coverage = how many of those bytes exist as native compiled code rather than
running on the dirty-RAM interpreter. It is read from the recompiler's own
`.ranges` manifests, so it reflects what was actually emitted.

CEILING: a module is part code, part data, and data never becomes "covered".
The main EXE tops out at 576,680 of 614,400 (93.9%) with the remainder being
the rodata tail. So raw file bytes would cap the bar around 94% and look
broken. Each module therefore declares a ceiling: the measured code-byte count
where we have one, otherwise file_size * CODE_RATIO as an estimate. Replace an
estimate with a measurement as soon as that module is recompiled.

Usage:  python3 tools/coverage.py [--write]
        --write refreshes docs/coverage.svg and docs/coverage.json
"""

import argparse
import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Ratio of code bytes to file bytes, measured on the main EXE (576680/614400).
# Used only as a fallback for modules we have not yet recompiled.
CODE_RATIO = 0.939

MODULES = [
    {
        "name": "SLUS_006.98",
        "desc": "main executable",
        "size": 614_400,
        # Emitted by the build; present as soon as `generate` has run.
        "ranges": ["generated/SLUS_006.98_full.ranges"],
    },
    {
        "name": "MOVIES.EXE",
        "desc": "boot/FMV overlay @ 0x80170000",
        "size": 200_704,
        # AOT shards or runtime-captured shards compiled by compile_overlays.py.
        "ranges": [
            "generated/MOVIES*.ranges",
            "build-release/cache/**/*.ranges",
        ],
    },
    {
        "name": "RSC/MENU.BIN",
        "desc": "menu code @ 0x800B4000",
        "size": 61_480,
        "ranges": ["build-release/cache/**/*.ranges"],
        # Shares a cache dir with MOVIES.EXE; extents are separated by address.
        "addr_lo": 0x800B4000,
        "addr_hi": 0x800C3008,
    },
]

# MOVIES.EXE occupies 0x80170000..0x801A1000; the main EXE 0x80010000..0x800A6000.
ADDR_WINDOWS = {
    "SLUS_006.98": (0x80010000, 0x800A6000),
    "MOVIES.EXE": (0x80170000, 0x801A1000),
}


def parse_ranges(path):
    """Yield (lo, length) from a psxrecomp .ranges manifest ('R <lo> <len>')."""
    out = []
    try:
        with open(path) as fh:
            for line in fh:
                if line.startswith("R "):
                    _, lo, ln = line.split()
                    out.append((int(lo, 16), int(ln, 16)))
    except OSError:
        pass
    return out


def union_bytes(extents, lo=None, hi=None):
    """Total bytes covered by possibly-overlapping extents, optionally windowed.

    The manifests list one extent per function and those overlap, so summing
    them naively over-counts (the main EXE sums to 206%). Merge first.
    """
    spans = []
    for a, ln in extents:
        b = a + ln
        if lo is not None:
            a, b = max(a, lo), min(b, hi)
        if b > a:
            spans.append([a, b])
    spans.sort()
    merged = []
    for a, b in spans:
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    return sum(b - a for a, b in merged)


def measure():
    rows = []
    for m in MODULES:
        extents = []
        for pat in m["ranges"]:
            for p in glob.glob(os.path.join(ROOT, pat), recursive=True):
                extents.extend(parse_ranges(p))
        lo, hi = ADDR_WINDOWS.get(m["name"], (m.get("addr_lo"), m.get("addr_hi")))
        covered = union_bytes(extents, lo, hi) if extents else 0
        # Measured ceiling once a module has been recompiled; estimate until then.
        if covered > 0:
            ceiling, measured = covered, True
        else:
            ceiling, measured = int(m["size"] * CODE_RATIO), False
        rows.append({**m, "covered": covered, "ceiling": ceiling, "measured": measured})
    return rows


def totals(rows):
    covered = sum(r["covered"] for r in rows)
    ceiling = sum(r["ceiling"] for r in rows)
    pct = 100.0 * covered / ceiling if ceiling else 0.0
    return covered, ceiling, min(pct, 100.0)


# Bar green sampled from the O.D.T. cover art; TEXT is a brighter, more
# saturated shade of the same hue so the number reads as a highlight.
BAR = "#809570"
TEXT = "#8fae72"
MUTED = "#656d76"


def colour(pct):
    """One colour: a progress bar already encodes progress by its length."""
    return BAR


def svg(pct, covered, ceiling):
    """Badge SVG using presentation attributes only.

    GitHub sanitises SVGs embedded in Markdown and strips <style> blocks, so
    CSS classes render unstyled. Everything here is inline attributes.

    Drawn wide (880x34) and referenced at width="100%" so it spans the README
    column. Corner radius is 3 to match the shields.io badges above it rather
    than reading as a pill.
    """
    # PAD 0 so the label sits flush with the README's other left-aligned text.
    W, H, PAD, BAR_H, RX = 880, 34, 0, 10, 3
    bar_w = W - 2 * PAD
    fill = max(RX * 2, int(bar_w * pct / 100.0))
    c = colour(pct)
    label = f"{pct:.0f}%"
    # Positioned from the left rather than text-anchor="end": anchored text at
    # the right edge is dropped entirely by some renderers (macOS Quick Look).
    # Deliberately generous: underestimating pushes the '%' past the viewBox
    # edge and it gets clipped. A few px of slack is invisible; a clipped
    # glyph is not.
    pct_w = 8 * (len(label) - 1) + 13         # digits ~8px, '%' ~13px at 12px semibold
    pct_x = W - PAD - pct_w
    font = "-apple-system,BlinkMacSystemFont,'Segoe UI',Helvetica,Arial,sans-serif"
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" \
viewBox="0 0 {W} {H}" role="img" aria-label="Native code coverage {pct:.0f} percent">
  <title>Native code: {pct:.0f}% ({covered:,} of {ceiling:,} bytes)</title>
  <text x="{PAD}" y="13" font-family="{font}" font-size="12" font-weight="600"
        fill="{MUTED}">Native code</text>
  <text x="{pct_x}" y="13" font-family="{font}" font-size="12" font-weight="600"
        fill="{TEXT}">{pct:.0f}%</text>
  <rect x="{PAD}" y="21" width="{bar_w}" height="{BAR_H}" rx="{RX}" fill="#d0d7de"
        fill-opacity="0.4"/>
  <rect x="{PAD}" y="21" width="{fill}" height="{BAR_H}" rx="{RX}" fill="{c}"/>
</svg>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true",
                    help="refresh docs/coverage.svg and docs/coverage.json")
    args = ap.parse_args()

    rows = measure()
    covered, ceiling, pct = totals(rows)

    w = max(len(r["name"]) for r in rows)
    print(f"{'module':<{w}}  {'covered':>9}  {'ceiling':>9}   how")
    print("-" * (w + 34))
    for r in rows:
        how = "measured" if r["measured"] else f"est. {CODE_RATIO:.0%} of file"
        print(f"{r['name']:<{w}}  {r['covered']:>9,}  {r['ceiling']:>9,}   {how}")
    print("-" * (w + 34))
    print(f"{'TOTAL':<{w}}  {covered:>9,}  {ceiling:>9,}   {pct:.1f}%")

    if args.write:
        d = os.path.join(ROOT, "docs")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "coverage.svg"), "w") as fh:
            fh.write(svg(pct, covered, ceiling))
        with open(os.path.join(d, "coverage.json"), "w") as fh:
            json.dump({"schemaVersion": 1, "label": "native code",
                       "message": f"{pct:.0f}%", "color": colour(pct).lstrip("#")},
                      fh, indent=2)
            fh.write("\n")
        print("\nwrote docs/coverage.svg and docs/coverage.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
