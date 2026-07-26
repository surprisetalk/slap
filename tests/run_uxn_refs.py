#!/usr/bin/env python3
"""Check examples/uxn.slap against another Uxn implementation's reference renders.

Deliberately NOT part of `make test`: it fetches ~90 KB of ROMs and PNGs from
GitHub, and the suite has to work offline. Run it by hand, or `make test-uxn-refs`.
Downloads are cached under tests/.uxn-refs/, so only the first run needs network.

    python3 tests/run_uxn_refs.py [--offline] [--write-png] [--frames N] [rom ...]
    python3 tests/run_uxn_refs.py --sweep    # pin static-vs-animated per ROM
    python3 tests/run_uxn_refs.py --bench    # uxn instructions/sec

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

import argparse, os, re, struct, subprocess, sys, time, urllib.request, zlib

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

# name, width, height, pixels allowed to differ, motion.
# The pixel allowance is 0 everywhere but piano, which draws an audio level
# meter -- uxn.slap has no Audio device (slap has no audio primitive), so those
# 22 pixels sit at the wrong level. Raise a number here only with an explanation
# of what is missing.
#
# `motion` is what --sweep pins. "static" means the ROM paints once and its
# render is byte-identical at every frame count; "anim" means the render depends
# on how many times the Screen vector ran. Measured, not assumed: six of these
# are identical from 1 frame to 240, while screen and audio differ at both ends.
ROMS = [
    ("screen", 256, 176, 0, "anim"),
    ("screen_auto", 160, 32, 0, "static"),
    ("screen_blending", 256, 268, 0, "static"),
    ("screen_bounds", 512, 320, 0, "static"),
    ("screen_pixel", 200, 200, 0, "static"),
    ("controller", 512, 320, 0, "static"),
    ("mandelbrot", 378, 288, 0, "static"),
    ("audio", 512, 320, 0, "anim"),
    ("piano", 384, 224, 22, "static"),
    # Bench only: raven ships this ROM but not a reference render for it, so
    # there is nothing to compare against and no measured motion class. It earns
    # its place because it is the one ROM here besides screen with a real
    # per-frame workload -- ~20.7k instructions/frame at uxn.slap's native
    # 320x240. Timing it measures work that is NOT verified correct, which is
    # exactly why screen.rom stays the headline number.
    ("drool", 320, 240, None, None),
]

# ROMs with allow=None have no reference PNG: skip the download and every mode
# that needs something to compare against.
BENCH_ONLY = {"drool"}

# mandelbrot draws the whole set from its reset vector and installs no Screen
# vector, so it costs ~5.5 minutes and retires 0 frame-loop instructions at any
# count. Sweeping it is three identical renders for 16 minutes, so --sweep skips
# it and says so rather than quietly implying it was covered.
SWEEP_SKIP = {"mandelbrot"}
# Two off-counts either side of the canonical 60. A static ROM must match both;
# an animated one must differ from its own 60-frame render at one of them.
SWEEP_FRAMES = (1, 120)


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
    """uxn.slap prints `INS <n>`, `PAL <12 hex nibbles>`, `UXNDUMP`, then one digit
    per pixel. INS counts only the frame loop -- the reset vector and the input
    replay are setup, so a ROM that paints once and idles reports near zero."""
    lines = [l.strip().strip('"') for l in text.splitlines()]
    ins_line = next((l for l in lines if l.startswith("INS ")), None)
    if ins_line is None:
        die(
            f"{rom}: the dump has no INS line.",
            "  examples/uxn.slap's dump block prints it before PAL; if that was",
            "  removed, --bench has nothing to measure.",
        )
    ins = int(ins_line[4:])
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
    return pal, grid, ins


def render(name, w, h, frames, offline):
    """Rebuild uxn.slap at w*h, boot the ROM, run `frames` Screen frames.
    Returns (palette, grid, frame-loop instructions, wall seconds)."""
    rom_path = f"{CACHE}/{name}.rom"
    fetch(ROM_URL.format(name), rom_path, offline)
    if name not in BENCH_ONLY:
        fetch(PNG_URL.format(name), f"{CACHE}/{name}.png", offline)
    # Written under a pid-unique name and renamed into place: the patched source
    # is per-ROM, so two runs at once (make -j test-uxn-refs test-uxn-sweep)
    # would otherwise have one process reading a file the other is half through
    # writing, and a truncated emulator fails in a way that reads as a ROM bug.
    patched = f"{CACHE}/uxn_{name}.slap"
    tmp = f"{patched}.{os.getpid()}.tmp"
    with open(tmp, "w") as f:
        f.write(patch_geometry(open("examples/uxn.slap").read(), w, h))
    os.replace(tmp, patched)
    t = time.perf_counter()
    try:
        r = subprocess.run(
            ["./slap", "--headless", rom_path, str(frames)],
            stdin=open(patched),
            capture_output=True,
            text=True,
            timeout=TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        die(f"{name}: no output within {TIMEOUT}s at {frames} frames.")
    secs = time.perf_counter() - t
    if r.returncode != 0:
        err = (r.stderr.strip() or r.stdout.strip()).splitlines()
        die(
            f"{name}: ./slap exited {r.returncode} at {frames} frames.",
            "  " + (err[-1] if err else "(no output)"),
        )
    pal, grid, ins = load_dump(r.stdout, name)
    return pal, grid, ins, secs


def skip_bench_only(todo, verb):
    """Drop ROMs that have no reference render. Says which, when asked by name --
    silently dropping one the user typed would read as it having passed."""
    keep = [r for r in todo if r[0] not in BENCH_ONLY]
    dropped = [r[0] for r in todo if r[0] in BENCH_ONLY]
    if dropped and verb:
        print(
            f"  (not {verb}: {', '.join(dropped)} -- raven ships no reference render for it; --bench only)"
        )
    # Reporting "0 ROMs match" and exiting 0 would be a pass that checked
    # nothing, which is worse than an error.
    if not keep and verb:
        die(
            f"nothing left to be {verb}: {', '.join(dropped)} has no reference render.",
            "  Use --bench for it, or name a ROM from raven's snapshot suite.",
        )
    return keep


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


def compare_mode(todo, a):
    failed = 0
    for name, w, h, allow, _ in skip_bench_only(todo, "compared"):
        pal, mine, ins, secs = render(name, w, h, a.frames, a.offline)
        if a.write_png:
            png_write(f"{CACHE}/mine_{name}.png", mine, pal)
        rw, rh, ref = png_read(f"{CACHE}/{name}.png")
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

    n = len(skip_bench_only(todo, None))
    if failed:
        print(f"uxn-refs: {n - failed} matched, {failed} FAILED")
        return 1
    print(f"uxn-refs: {n} ROM{'' if n == 1 else 's'} match {REPO}@{SHA[:7]}")
    return 0


def sweep_mode(todo, a):
    """Render each ROM at 1, 60 and 120 frames and pin whether it moves.

    The reference comparison only ever looks at 60 frames, so it cannot tell a
    ROM that paints a correct image once from one that paints a correct image
    because two errors cancelled at exactly that count. This can: a ROM declared
    static must be byte-identical at every count, and one declared animated must
    not be. A static ROM that starts drifting is state surviving across Screen
    vector calls that should not -- an uncleared dirty box, a stack pointer that
    creeps, a device page write that leaks. An animated ROM that goes flat is the
    frame loop no longer advancing anything, which would make its 60-frame match
    meaningless.
    """
    todo = skip_bench_only(todo, "swept")
    skipped = [n for n, _, _, _, _ in todo if n in SWEEP_SKIP]
    todo = [r for r in todo if r[0] not in SWEEP_SKIP]
    failed = 0
    for name, w, h, allow, motion in todo:
        counts = (SWEEP_FRAMES[0], a.frames, SWEEP_FRAMES[1])
        grids, note = {}, []
        for f in counts:
            pal, grid, ins, secs = render(name, w, h, f, a.offline)
            grids[f] = [tuple(row) for row in grid]
            note.append(f"{f}:{ins}")
        same = {f: grids[f] == grids[a.frames] for f in counts}
        moved = not all(same.values())
        ok = moved == (motion == "anim")
        flags = " ".join(("=" if same[f] else "~") + str(f) for f in counts)
        if ok:
            print(f"  {name:<17} {motion:<6} {flags:<16} ok    ins {' '.join(note)}")
        else:
            failed += 1
            off = [f for f in counts if f != a.frames]
            want = (
                f"at least one of {off} frames must differ from the {a.frames}-frame render"
                if motion == "anim"
                else f"all of {list(counts)} must render identically"
            )
            print(f"  {name:<17} {motion:<6} {flags:<16} FAILED")
            print(f"      declared {motion}, so {want}.")
            print(
                "      A static ROM that drifts means state is surviving between Screen"
            )
            print(
                "      vector calls; an animated one that stops means the frame loop is"
            )
            print(
                "      no longer advancing it, and its 60-frame match proves nothing."
            )
    if skipped:
        print(
            f"  (skipped {', '.join(skipped)}: paints from the reset vector, 0 frame-loop"
        )
        print("   instructions at any count, ~5.5 min per render)")
    if failed:
        print(f"uxn-refs: sweep {len(todo) - failed} ok, {failed} FAILED")
        return 1
    k = len(todo)
    print(f"uxn-refs: sweep ok, {k} ROM{'' if k == 1 else 's'} hold the declared motion classification")
    return 0


def bench_mode(todo, a):
    """Throughput, in uxn instructions per second of frame-loop time.

    Wall time alone is not the rate: every ROM pays a fixed boot cost, and
    mandelbrot's reset vector alone is ~5.5 minutes. So each ROM is run twice --
    once at 0 frames for the setup baseline, once at N -- and the rate is the
    frame-loop instructions over the difference.
    """
    print(f"  {'rom':<17} {'ins/frame':>10} {'frame s':>9} {'ins/sec':>10}")
    for name, w, h, allow, _ in todo:
        _, _, _, base = render(name, w, h, 0, a.offline)
        _, _, ins, secs = render(name, w, h, a.frames, a.offline)
        loop, per = max(secs - base, 0.0), ins / a.frames
        # Under a second of frame-loop time the subtraction is mostly process
        # noise, so a rate computed from it would be fiction -- these ROMs paint
        # once and idle, and dividing their few hundred instructions by ~0
        # seconds prints tens of millions of ips. Say nothing instead.
        if per < 1000 or loop < 1.0:
            print(
                f"  {name:<17} {per:>10.0f} {loop:>9.2f} {'--':>10}   idle: paints once, no per-frame work"
            )
        else:
            print(f"  {name:<17} {per:>10.0f} {loop:>9.2f} {ins / loop:>10.0f}")
    print(
        f"uxn-bench: {a.frames} frames per ROM, setup subtracted, {REPO}@{SHA[:7]} ROMs"
    )
    return 0


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
    ap.add_argument(
        "--sweep",
        action="store_true",
        help="pin each ROM's static/animated classification across frame counts",
    )
    ap.add_argument(
        "--bench",
        action="store_true",
        help="report uxn instructions/sec (default: screen, the only ROM here with a real per-frame workload)",
    )
    a = ap.parse_args()

    if a.sweep and a.bench:
        die("--sweep and --bench do different runs; pick one.")
    if not os.access("./slap", os.X_OK):
        die("no ./slap binary; run 'make slap' first.", f"  cwd: {os.getcwd()}")
    if not os.path.exists("examples/uxn.slap"):
        die(
            "cannot find examples/uxn.slap.",
            f"  cwd: {os.getcwd()}",
            "  Run from the repo root.",
        )

    want = {r.lower() for r in a.roms}
    known = {n for n, _, _, _, _ in ROMS}
    for r in want - known:
        die(f"no such reference ROM: {r}", "  known: " + ", ".join(sorted(known)))
    if a.bench and not want:
        # The only two ROMs here with a real per-frame workload. screen is the
        # headline because it is also pixel-exact against raven, so its rate is
        # timing verified-correct work; drool has no reference render at all.
        want = {"screen", "drool"}
    todo = [r for r in ROMS if not want or r[0] in want]

    mode = sweep_mode if a.sweep else bench_mode if a.bench else compare_mode
    sys.exit(mode(todo, a))


if __name__ == "__main__":
    main()
