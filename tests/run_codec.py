#!/usr/bin/env python3
"""Integration test for examples/banner.slap and examples/plasma.slap: render a
real bitmap font to text and a real plasma to a TGA, then check the results from
outside Slap -- the glyph bitmap against the font file, the image against the
TGA spec."""

import os, subprocess, sys, tempfile

STRINGS = "examples/lib/strings.slap"
PARSE = "examples/lib/parse.slap"
BANNER_LIBS = [STRINGS, "examples/lib/icn.slap"]
PLASMA_LIBS = [STRINGS, PARSE, "examples/lib/tga.slap"]
BANNER = "examples/banner.slap"
PLASMA = "examples/plasma.slap"
FONT = "fonts/atari8.uf1"

passed = 0


def die(msg):
    print(f"codec: {msg}", file=sys.stderr)
    sys.exit(1)


def check(name, cond, detail=""):
    global passed
    if not cond:
        die(f"{name} FAILED {detail}")
    passed += 1


if not os.access("./slap", os.X_OK):
    die("no ./slap binary; run 'make slap' first")
for f in BANNER_LIBS + PLASMA_LIBS + [BANNER, PLASMA, FONT]:
    if not os.path.exists(f):
        die(f"cannot find {f}; run from the repo root")

BANNER_SRC = "".join(open(f).read() for f in BANNER_LIBS + [BANNER])
PLASMA_SRC = "".join(open(f).read() for f in PLASMA_LIBS + [PLASMA])


def run(src, *argv, binary=False):
    return subprocess.run(
        ["./slap", *argv],
        input=src.encode() if binary else src,
        capture_output=True,
        text=not binary,
        timeout=60,
    )


for label, src in (("banner", BANNER_SRC), ("plasma", PLASMA_SRC)):
    r = run(src, "--check")
    if r.returncode != 0:
        die(f"{label} --check failed:\n{r.stderr}")
    passed += 1

# ===== banner.slap =====

r = run(BANNER_SRC, "S", FONT)
check("banner-exit", r.returncode == 0, r.stderr[:300])
rows = r.stdout.split("\n")[:-1]
check("banner-8-rows", len(rows) == 8, repr(rows))
# The exact bitmap of 'S' in atari8.uf1. This is what catches a glyph offset
# that is off by one byte or one glyph, and an LSB-first icn-decode -- both of
# which still produce eight plausible-looking rows.
check(
    "banner-S-bitmap",
    rows
    == [
        "  ####  ",
        " ##  ## ",
        " ##     ",
        "  ####  ",
        "     ## ",
        " ##  ## ",
        "  ####  ",
        "        ",
    ],
    repr(rows),
)
# every row is the glyph's advance width wide, from the font's own width table
width = open(FONT, "rb").read()[ord("S")]
check("banner-width-from-table", all(len(x) == width for x in rows), f"want {width}")

r = run(BANNER_SRC, " ", FONT)
check("banner-space-blank", set(r.stdout) <= {" ", "\n"}, repr(r.stdout))

r = run(BANNER_SRC, "SLAP", FONT)
check(
    "banner-multichar", len(r.stdout.split("\n")[0]) == 4 * width, repr(r.stdout[:40])
)
check(
    "banner-default-font", run(BANNER_SRC).stdout == r.stdout, "default is SLAP/atari8"
)
check("banner-other-uf1", run(BANNER_SRC, "A", "fonts/orca8.uf1").returncode == 0)

# .uf2 and .uf3 are the same layout at 32 and 72 bytes per glyph; decoding one
# as .uf1 would produce eight rows of nonsense rather than an error
for font, stride in [("fonts/chicago12.uf2", 32), ("fonts/times24.uf3", 72)]:
    r = run(BANNER_SRC, "A", font)
    check(f"banner-reject-{stride}", r.returncode != 0, font)
    check(
        f"banner-reject-{stride}-names-stride",
        f"has {stride} bytes per glyph" in r.stderr,
        repr(r.stderr[:200]),
    )
    check(f"banner-reject-{stride}-suggests", ".uf1 only" in r.stderr)

r = run(BANNER_SRC, "A", "fonts/nope.uf1")
check(
    "banner-missing-font",
    r.returncode != 0 and "cannot read" in r.stderr,
    repr(r.stderr[:160]),
)

# ===== plasma.slap =====

with tempfile.TemporaryDirectory() as d:
    out = os.path.join(d, "p.tga")

    r = run(PLASMA_SRC, out, "48")
    check("plasma-exit", r.returncode == 0, r.stderr[:400])
    check("plasma-reports", "48x48" in r.stdout, repr(r.stdout))

    raw = open(out, "rb").read()
    check("plasma-size", len(raw) == 18 + 48 * 48 * 3, len(raw))
    check("plasma-id-len", raw[0] == 0)
    check("plasma-no-colormap", raw[1] == 0)
    check("plasma-type-2", raw[2] == 2, "uncompressed true-color")
    check("plasma-cmap-spec-zero", raw[3:12] == b"\x00" * 9)
    # little-endian 16-bit width and height
    check("plasma-w", raw[12] | (raw[13] << 8) == 48, (raw[12], raw[13]))
    check("plasma-h", raw[14] | (raw[15] << 8) == 48, (raw[14], raw[15]))
    check("plasma-depth", raw[16] == 24)
    check("plasma-descriptor", raw[17] == 0, "0 means bottom-left origin")

    px = raw[18:]
    check("plasma-not-uniform", len(set(px)) > 16, len(set(px)))
    check("plasma-uses-full-range", max(px) > 200 and min(px) < 40, (min(px), max(px)))

    def lum(i):
        b, g, r_ = px[i * 3], px[i * 3 + 1], px[i * 3 + 2]
        return (r_ * 299 + g * 587 + b * 114) / 1000

    # The vignette is exp(-r^2), so the middle is bright and the corners are
    # dark. Scrambled row order, swapped channels or a broken field would not
    # keep that gradient, and it holds without pinning any exact pixel -- which
    # matters because fexp/flog/fatan2 are libm and may differ in the last bits.
    n = 48
    centre = sum(lum(y * n + x) for y in range(20, 28) for x in range(20, 28)) / 64
    corners = (
        sum(lum(y * n + x) for y in (0, 1, n - 2, n - 1) for x in (0, 1, n - 2, n - 1))
        / 16
    )
    check(
        "plasma-vignette",
        centre > corners + 30,
        f"centre {centre:.1f} corners {corners:.1f}",
    )

    # nothing here reads a clock or `random`, so two runs must agree byte for byte
    out2 = os.path.join(d, "p2.tga")
    run(PLASMA_SRC, out2, "48")
    check("plasma-deterministic", open(out2, "rb").read() == raw)

    # the documented maximum square
    r = run(PLASMA_SRC, os.path.join(d, "max.tga"), "73")
    check("plasma-73-ok", r.returncode == 0, r.stderr[:300])
    check(
        "plasma-73-size",
        os.path.getsize(os.path.join(d, "max.tga")) == 18 + 73 * 73 * 3,
    )

    # over the cap: refused up front, naming the real limit, rather than dying
    # inside tga-header with a message about concat
    r = run(PLASMA_SRC, os.path.join(d, "too.tga"), "74")
    check("plasma-74-refused", r.returncode != 0)
    check("plasma-74-explains", "caps at 16384" in r.stderr, repr(r.stderr[:250]))
    check("plasma-74-suggests", "73x73" in r.stderr, repr(r.stderr[:250]))
    check("plasma-74-no-file", not os.path.exists(os.path.join(d, "too.tga")))

    r = run(PLASMA_SRC, os.path.join(d, "zero.tga"), "0")
    check("plasma-zero-refused", r.returncode != 0 and "at least 1" in r.stderr)

print(f"codec: {passed} checks passed")
