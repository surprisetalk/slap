#!/usr/bin/env python3
"""Check examples/uxn.slap against another Uxn implementation's reference renders.

Deliberately NOT part of `make test`: it fetches ~90 KB of ROMs and PNGs from
GitHub, and the suite has to work offline. Run it by hand, or `make test-uxn-refs`.
Downloads are cached under tests/.uxn-refs/, so only the first run needs network.

    python3 tests/run_uxn_refs.py [--offline] [--write-png] [--frames N] [rom ...]

An emulator's own self-test only covers the parts its author thought about.
uxn.slap's ~110 assertions all passed while three real Screen bugs were live;
comparing whole frames against mkeeter/raven found every one of them.

Two things make the comparison exact rather than "looks about right":

  * uxn.slap's canvas geometry is six constants, so this rebuilds the emulator
    at each ROM's own resolution instead of scoring a 320x240 render against a
    512x320 one.
  * uxn.slap's dump prints palette INDICES plus the palette itself, so the
    reference's RGB is mapped back to an index. Nothing depends on how uxn.slap
    ranks the four entries into greys.

The input replay lives in uxn.slap's dump block, not here -- several of these
ROMs install only a Mouse or Controller vector and render blank without it.
"""

import argparse, os, re, struct, subprocess, sys, urllib.request, zlib

REPO = "mkeeter/raven"
# Pinned, not HEAD: a moving reference turns an upstream change into a failure
# in this repo. Bump deliberately, and re-check the expected diffs below.
SHA = "14dbcbeb38cf15fa941f35095f6acf944252f4b3"
ROM_URL = f"https://raw.githubusercontent.com/{REPO}/{SHA}/roms/{{}}.rom"
PNG_URL = f"https://raw.githubusercontent.com/{REPO}/{SHA}/raven-varvara/tests/{{}}.png"

CACHE = "tests/.uxn-refs"
TIMEOUT = 900
# Must match raven's snapshot harness, which calls dev.redraw() exactly 60 times
# after the input. These ROMs animate, so a different count is a different frame:
# screen.rom at 2 frames is the reference image shifted two pixels, which reads
# as a Screen bug and is not one.
FRAMES = 60

# name, width, height, pixels allowed to differ.
# Every entry is 0 but piano, which draws an audio level meter -- uxn.slap has
# no Audio device (slap has no audio primitive), so those 22 pixels sit at the
# wrong level. Raise a number here only with an explanation of what is missing.
ROMS = [
    ("screen", 256, 176, 0),
    ("screen_auto", 160, 32, 0),
    ("screen_blending", 256, 268, 0),
    ("screen_bounds", 512, 320, 0),
    ("screen_pixel", 200, 200, 0),
    ("controller", 512, 320, 0),
    ("mandelbrot", 378, 288, 0),
    ("audio", 512, 320, 0),
    ("piano", 384, 224, 22),
]


def fetch(url, path, offline):
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return
    if offline:
        die(
            f"--offline, but {path} is not cached yet.",
            f"  It would come from {url}",
            "  Run once without --offline to populate tests/.uxn-refs/.",
        )
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        with urllib.request.urlopen(url, timeout=60) as r:
            data = r.read()
    except Exception as e:
        die(
            f"cannot download {url}: {e}",
            "  This test needs network. It is not part of `make test` for exactly",
            "  that reason. Use --offline once the cache is populated.",
        )
    if not data:
        die(f"{url} returned an empty body.")
    with open(path, "wb") as f:
        f.write(data)


def die(*lines):
    print("uxn-refs: " + lines[0], file=sys.stderr)
    for l in lines[1:]:
        print(l, file=sys.stderr)
    sys.exit(1)


def png_read(path):
    """Minimal PNG decoder: 8-bit RGB/RGBA, all five filters. No PIL dependency."""
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        die(f"{path} is not a PNG (bad magic).")
    i, idat, w = 8, b"", None
    while i < len(d):
        ln, typ = struct.unpack(">I", d[i : i + 4])[0], d[i + 4 : i + 8]
        body = d[i + 8 : i + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype not in (2, 6):
                die(
                    f"{path}: only 8-bit RGB/RGBA PNGs are supported, got depth={depth} colour-type={ctype}."
                )
            nch = 4 if ctype == 6 else 3
        elif typ == b"IDAT":
            idat += body
        i += 12 + ln
    if w is None:
        die(f"{path}: no IHDR chunk.")
    raw, out, stride, prev = zlib.decompress(idat), [], w * nch, bytearray(w * nch)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - nch] if x >= nch else 0
            b = prev[x]
            c = prev[x - nch] if x >= nch else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (
                    line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)
                ) & 255
        out.append([tuple(line[x * nch : x * nch + 3]) for x in range(w)])
        prev = line
    return w, h, out


def png_write(path, grid, pal):
    h, w = len(grid), len(grid[0])
    raw = b"".join(b"\x00" + bytes(v for px in row for v in pal[px]) for row in grid)

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))

    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def patch_geometry(src, w, h):
    """Rebuild uxn.slap at w*h. The layers are flat runs inside the state list,
    so moving the screen size moves FG-B and STATE-N with it."""
    n = w * h
    bg = int(one(src, r"^(\d+)\s+'BG-B let", "BG-B"))
    subs = [
        (r"^\d+ 'FG-B let", f"{bg + n} 'FG-B let", "FG-B"),
        (r"^\d+ 'STATE-N let", f"{bg + 2 * n} 'STATE-N let", "STATE-N"),
        (
            r"^\d+ 'SCR-W let  \d+ 'SCR-H let  \d+ 'SCR-N let",
            f"{w} 'SCR-W let  {h} 'SCR-H let  {n} 'SCR-N let",
            "SCR-W/SCR-H/SCR-N",
        ),
    ]
    for pat, rep, what in subs:
        src, k = re.subn(pat, rep, src, flags=re.M)
        if k != 1:
            die(
                f"cannot patch the {what} constant in examples/uxn.slap ({k} matches for /{pat}/).",
                "  This harness rescales the canvas by rewriting those declarations,",
                "  so renaming or reformatting them silently breaks the comparison.",
                "  Update the patterns in tests/run_uxn_refs.py to match the file.",
            )
    return src


def one(src, pat, what):
    m = re.search(pat, src, flags=re.M)
    if not m:
        die(f"cannot find the {what} constant in examples/uxn.slap (/{pat}/).")
    return m.group(1)


def load_dump(text, rom):
    """uxn.slap prints `PAL <12 hex nibbles>`, then `UXNDUMP`, then one digit per pixel."""
    lines = [l.strip().strip('"') for l in text.splitlines()]
    pal_line = next((l for l in lines if l.startswith("PAL ")), None)
    if pal_line is None or "UXNDUMP" not in lines:
        tail = "\n".join(l for l in lines if l)[-600:]
        die(
            f"{rom}: the dump has no PAL/UXNDUMP header -- the emulator did not get that far.",
            "  last output was:",
            tail,
        )
    n = pal_line[4:]
    if len(n) != 12:
        die(
            f"{rom}: PAL line has {len(n)} nibbles, expected 12 (r g b per entry, 4 entries)."
        )
    pal = [tuple(int(n[i * 3 + c], 16) * 17 for c in range(3)) for i in range(4)]
    start = lines.index("UXNDUMP") + 1
    grid = [[int(c) for c in l] for l in lines[start:] if l and l[0] in "0123"]
    if not grid:
        die(f"{rom}: UXNDUMP header present but no pixel rows followed it.")
    return pal, grid


def compare(rom, pal, mine, ref, rw, rh):
    mh, mw = len(mine), len(mine[0])
    if (mw, mh) != (rw, rh):
        die(
            f"{rom}: rendered {mw}x{mh} but the reference is {rw}x{rh}.",
            "  The geometry patch and the ROMS table disagree; fix the table entry.",
        )
    # colour -> the set of palette slots holding it. A ROM is free to give two
    # slots the same RGB (piano does), and then the reference pixel genuinely
    # cannot tell them apart -- so any of them counts as a match.
    idx = {}
    for i, c in enumerate(pal):
        idx.setdefault(c, set()).add(i)
    unknown, diffs = {}, []
    for y in range(rh):
        for x in range(rw):
            c = ref[y][x]
            if c not in idx:
                unknown[c] = unknown.get(c, 0) + 1
            elif mine[y][x] not in idx[c]:
                diffs.append((x, y, min(idx[c]), mine[y][x]))
    return diffs, unknown


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("roms", nargs="*", help="only these ROMs (default: all nine)")
    ap.add_argument(
        "--offline", action="store_true", help="use the cache; never touch the network"
    )
    ap.add_argument(
        "--write-png", action="store_true", help="write tests/.uxn-refs/mine_<rom>.png"
    )
    ap.add_argument(
        "--frames",
        type=int,
        default=FRAMES,
        help=f"screen-vector frames to run (default {FRAMES})",
    )
    a = ap.parse_args()

    if not os.access("./slap", os.X_OK):
        die("no ./slap binary; run 'make slap' first.", f"  cwd: {os.getcwd()}")
    if not os.path.exists("examples/uxn.slap"):
        die(
            "cannot find examples/uxn.slap.",
            f"  cwd: {os.getcwd()}",
            "  Run from the repo root.",
        )
    src = open("examples/uxn.slap").read()

    want = {r.lower() for r in a.roms}
    known = {n for n, _, _, _ in ROMS}
    for r in want - known:
        die(f"no such reference ROM: {r}", "  known: " + ", ".join(sorted(known)))
    todo = [r for r in ROMS if not want or r[0] in want]

    failed = 0
    for name, w, h, allow in todo:
        rom_path, png_path = f"{CACHE}/{name}.rom", f"{CACHE}/{name}.png"
        fetch(ROM_URL.format(name), rom_path, a.offline)
        fetch(PNG_URL.format(name), png_path, a.offline)

        patched = f"{CACHE}/uxn_{name}.slap"
        with open(patched, "w") as f:
            f.write(patch_geometry(src, w, h))

        try:
            r = subprocess.run(
                ["./slap", "--headless", rom_path, str(a.frames)],
                stdin=open(patched),
                capture_output=True,
                text=True,
                timeout=TIMEOUT,
            )
        except subprocess.TimeoutExpired:
            print(f"  {name:<17} TIMEOUT (>{TIMEOUT}s)")
            failed += 1
            continue
        if r.returncode != 0:
            err = (r.stderr.strip() or r.stdout.strip()).splitlines()
            print(
                f"  {name:<17} EXIT {r.returncode}: {err[-1] if err else '(no output)'}"
            )
            failed += 1
            continue

        pal, mine = load_dump(r.stdout, name)
        if a.write_png:
            png_write(f"{CACHE}/mine_{name}.png", mine, pal)
        rw, rh, ref = png_read(png_path)
        diffs, unknown = compare(name, pal, mine, ref, rw, rh)

        total = rw * rh
        bad = len(diffs) + sum(unknown.values())
        pct = 100.0 * (total - bad) / total
        if bad <= allow:
            extra = f" ({bad} allowed)" if allow else ""
            print(f"  {name:<17} {w:>4}x{h:<4} {pct:6.2f}%  ok{extra}")
        else:
            failed += 1
            print(
                f"  {name:<17} {w:>4}x{h:<4} {pct:6.2f}%  FAILED: {bad} pixels differ, {allow} allowed"
            )
            if diffs:
                print(
                    "      (x,y ref mine):",
                    ", ".join(f"{x},{y} {e}!={g}" for x, y, e, g in diffs[:8]),
                )
            if unknown:
                worst = sorted(unknown.items(), key=lambda kv: -kv[1])[:4]
                print(
                    "      reference colours absent from the ROM's own palette:", worst
                )
                print(
                    "      that means the System palette was read wrong, not that a sprite is misplaced"
                )

    if failed:
        print(f"uxn-refs: {len(todo) - failed} matched, {failed} FAILED")
        sys.exit(1)
    print(f"uxn-refs: {len(todo)} ROMs match {REPO}@{SHA[:7]}")


if __name__ == "__main__":
    main()
