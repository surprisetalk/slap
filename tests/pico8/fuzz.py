#!/usr/bin/env python3
"""Differential fuzzer: random carts run in lua.slap and in zepto8 on pc (see
refs.sh), and every cart whose stdout differs is printed. Not part of make
test, since it needs pc.

    python3 tests/pico8/fuzz.py [carts] [seed]

Each cart printh()s random expressions over edge operands in tostr's hex form,
then draws with random arguments for one frame (clip stays on the screen:
zepto8 keeps clip bounds in bytes, which wrap and then fill past a row's end); the output is the printh lines,
then the screen. Differing carts stay in the scratch directory it prints."""

import concurrent.futures, os, random, subprocess, sys, tempfile

N = int(sys.argv[1]) if len(sys.argv) > 1 else 20
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1
PC = "taylor@100.97.122.7"
SRC = b"".join(
    open(f, "rb").read() for f in ["examples/lib/strings.slap", "examples/lua.slap"]
)

OPERANDS = [
    "0",
    "1",
    "-1",
    "0x0.0001",
    "-0x0.0001",
    "0x7fff.ffff",
    "0x8000",
    "0.5",
    "-0.5",
    "3",
    "-3",
    "32767",
    "-32767",
    "2",
    "0.25",
    "31",
    "32",
    "-33",
    "1.75",
    "-2.5",
    "100",
    "0x0.8",
]
BINOPS = ["+", "-", "*", "/", "%", "\\", "&", "|", "^^", "<<", ">>", ">>>", "<<>", ">><"]
# A comparison gives a boolean, which arithmetic refuses, so one goes only where a call reads it as a number.
COMPARE = ["==", "<", "<=", "~="]
CALLS1 = ["abs", "flr", "ceil", "sgn", "sqrt", "sin", "cos", "bnot"]
CALLS2 = [
    "min",
    "max",
    "atan2",
    "band",
    "bor",
    "bxor",
    "shl",
    "shr",
    "lshr",
    "rotl",
    "rotr",
]


def expr(rng, depth=0):
    r = rng.random()
    if depth > 2 or r < 0.3:
        return rng.choice(OPERANDS)
    if r < 0.6:
        return f"({expr(rng, depth + 1)} {rng.choice(BINOPS)} {expr(rng, depth + 1)})"
    if r < 0.7:
        return f"(- {expr(rng, depth + 1)})"
    if r < 0.8:
        return f"{rng.choice(CALLS1)}({expr(rng, depth + 1)})"
    if r < 0.9:
        return f"mid({expr(rng, depth + 1)}, {expr(rng, depth + 1)}, {expr(rng, depth + 1)})"
    if r < 0.95:
        return f"{rng.choice(CALLS2)}({expr(rng, depth + 1)}, {expr(rng, depth + 1)})"
    return f"{rng.choice(CALLS1)}({expr(rng, depth + 1)} {rng.choice(COMPARE)} {expr(rng, depth + 1)})"


def coord(rng):
    return str(
        rng.choice(
            [
                rng.randrange(-20, 148),
                rng.randrange(-20, 148),
                rng.choice([-32768, -129, -1, 0, 127, 128, 32767]),
            ]
        )
    )


def draw(rng):
    c = lambda: str(rng.randrange(0, 16))
    k = rng.randrange(14)
    if k == 0:
        return (
            f"rectfill({coord(rng)}, {coord(rng)}, {coord(rng)}, {coord(rng)}, {c()})"
        )
    if k == 1:
        return f"rect({coord(rng)}, {coord(rng)}, {coord(rng)}, {coord(rng)}, {c()})"
    if k == 2:
        return f"circfill({coord(rng)}, {coord(rng)}, {rng.randrange(0, 70)}, {c()})"
    if k == 3:
        return f"circ({coord(rng)}, {coord(rng)}, {rng.randrange(0, 70)}, {c()})"
    if k == 4:
        return f"line({coord(rng)}, {coord(rng)}, {coord(rng)}, {coord(rng)}, {c()})"
    if k == 5:
        return f"pset({coord(rng)}, {coord(rng)}, {c()})"
    if k == 6:
        return f"print('{rng.choice(['hi', 'a b', '0123', 'xyz!', '?'])}', {coord(rng)}, {coord(rng)}, {c()})"
    if k == 7:
        return f"spr({rng.randrange(0, 4)}, {coord(rng)}, {coord(rng)}, {rng.choice(['1', '2', '0.5', '1.5'])}, {rng.choice(['1', '2'])}, {rng.choice(['true', 'false'])})"
    if k == 8:
        return f"sspr(0, 0, 8, 8, {coord(rng)}, {coord(rng)}, {rng.randrange(-20, 40)}, {rng.randrange(-20, 40)})"
    if k == 9:
        return f"clip({rng.randrange(0, 128)}, {rng.randrange(0, 128)}, {rng.randrange(0, 140)}, {rng.randrange(0, 140)})"
    if k == 10:
        return f"camera({rng.randrange(-40, 40)}, {rng.randrange(-40, 40)})"
    if k == 11:
        return f"pal({c()}, {c()})"
    if k == 12:
        return f"map(0, 0, {coord(rng)}, {coord(rng)}, {rng.randrange(1, 8)}, {rng.randrange(1, 8)})"
    return f"printh(pget({coord(rng)}, {coord(rng)}))"


def cart(rng):
    gfx = "\n".join(
        "".join(rng.choice("0123456789abcdef") for _ in range(32)) + "0" * 96
        for _ in range(8)
    )
    mp = "\n".join(
        "".join(rng.choice(["00", "01", "02", "03"]) for _ in range(8)) + "00" * 120
        for _ in range(4)
    )
    lines = [f"printh(tostr({expr(rng)}, 1))" for _ in range(40)]
    draws = [draw(rng) for _ in range(30)]
    return (
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        + "function _init()\n srand("
        + str(rng.randrange(1, 1000))
        + ")\n"
        + "\n".join(" " + l for l in lines)
        + "\nend\n"
        + "function _draw()\n cls(1)\n"
        + "\n".join(" " + d for d in draws)
        + "\n printh(flr(rnd(1000)))\nend\n"
        + "__gfx__\n"
        + gfx
        + "\n__map__\n"
        + mp
        + "\n"
    )


tmp = tempfile.mkdtemp(prefix="slap-fuzz-")
rng = random.Random(SEED)
names = []
for i in range(N):
    name = f"f{SEED}-{i}.p8"
    with open(os.path.join(tmp, name), "w") as f:
        f.write(cart(rng))
    names.append(name)


def ours(name):
    return subprocess.run(
        ["./slap", os.path.join(tmp, name), "1"],
        input=SRC,
        capture_output=True,
        timeout=120,
    )


with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
    got = list(pool.map(ours, names))
subprocess.run(["ssh", PC, "mkdir -p ~/scratch/zepto8/fuzz"], check=True, timeout=60)
subprocess.run(
    ["scp", "-q"]
    + [os.path.join(tmp, n) for n in names]
    + [f"{PC}:scratch/zepto8/fuzz/"],
    check=True,
    timeout=120,
)
# One ssh: each cart's output, then a line that names the next.
script = "; ".join(
    f"echo '@@ {n}'; ~/scratch/zepto8/dump ~/scratch/zepto8/fuzz/{n} 2" for n in names
)
ref = subprocess.run(
    ["ssh", PC, script], capture_output=True, timeout=600
).stdout.decode()
want = {}
for part in ref.split("@@ ")[1:]:
    name, _, body = part.partition("\n")
    want[name] = body
bad = 0
for name, r in zip(names, got):
    mine = r.stdout.decode()
    if r.returncode != 0 or mine != want.get(name):
        bad += 1
        a, b = mine.split("\n"), want.get(name, "").split("\n")
        first = next(
            (
                k
                for k in range(max(len(a), len(b)))
                if (a[k] if k < len(a) else None) != (b[k] if k < len(b) else None)
            ),
            None,
        )
        print(
            f"{os.path.join(tmp, name)}: exit {r.returncode}, first difference at output line {first}"
        )
        if first is not None:
            print(
                f"  lua.slap: {a[first] if first < len(a) else None}\n  zepto8:   {b[first] if first < len(b) else None}"
            )
        if r.returncode != 0:
            print("  " + r.stderr.decode().strip().split("\n")[0][:200])
print(f"fuzz: {N - bad} of {N} carts match zepto8; carts in {tmp}")
sys.exit(1 if bad else 0)
