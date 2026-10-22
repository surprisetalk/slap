#!/usr/bin/env python3
"""Integration test for examples/lua.slap: each tests/lua/*.lua must print
exactly what the system `lua` prints; pico8-only syntax prints what pico8
would; and each refused program dies with its message and line."""

import concurrent.futures, glob, os, re, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(__file__))
import harness

check, count = harness.make_check("lua")
tmp = tempfile.mkdtemp(prefix="slap-lua-")
SRC = b"".join(
    open(f, "rb").read() for f in ["examples/lib/strings.slap", "examples/lua.slap"]
)


def run(path):
    return subprocess.run(["./slap", path], input=SRC, capture_output=True, timeout=60)


def run_text(name, text):
    path = os.path.join(tmp, name + ".lua")
    with open(path, "w") as f:
        f.write(text)
    return run(path)


for path in sorted(glob.glob("tests/lua/*.lua")):
    want = subprocess.run(["lua", path], capture_output=True, timeout=60)
    check(f"system lua runs {path}", want.returncode == 0, want.stderr.decode())
    got = run(path)
    check(
        path,
        got.returncode == 0 and got.stdout == want.stdout,
        f"exit {got.returncode}\nwant {want.stdout[:400]!r}\ngot  {got.stdout[:400]!r}\n{got.stderr.decode()[-600:]}",
    )

# pico8's own syntax, which the system lua cannot run.
PICO8 = [
    ("not equal", "print(1 != 2, 1 != 1)", "true\tfalse\n"),
    ("integer division", "print(7 \\ 2, -7 \\ 2)", "3\t-4\n"),
    ("xor", "print(12 ^^ 10)", "6\n"),
    ("binary and hex fractions", "print(0b101, 0x1.8, 0b1.1)", "5\t1.5\t1.5\n"),
    ("decimal fraction", "print(0.5, .25, 1.75 + 1)", "0.5\t0.25\t2.75\n"),
    ("logical shift", "print(-1 >>> 31 == 0x0.0001)", "true\n"),
    ("arithmetic shift", "print(-16 >> 2)", "-4\n"),
    (
        "rotate",
        "print(1 <<> 16 == 0x0.0001, 1 << 16, 0x0.0001 >>< 1 == 0x8000)",
        "true\t0\ttrue\n",
    ),
    ("16.16 wraps", "print(32767 + 1)", "-32768\n"),
    ("hex wraps", "print(0xffff)", "-1\n"),
    ("multiply keeps fractions", "print(1.5 * 1.5, -1.5 * 2)", "2.25\t-3\n"),
    ("divide", "print(7 / 2, 1 / 3, -1 / 4)", "3.5\t0.3333\t-0.25\n"),
    ("power", "print(2 ^ 10, 3 ^ 0)", "1024\t1\n"),
    ("double-slash comment", "print(1) // print(2)", "1\n"),
    ("long comment", "--[[ print(1)\nprint(2) ]] print(3)", "3\n"),
    ("leveled long comment", "--[==[\nprint(1)\n]] ]=]\n]==] print(2) --[=[ x ]=] print(3)", "2\n3\n"),
    ("leveled long string", "print([==[a]]b]=]c]==])", "a]]b]=]c\n"),
    ("long string folds CR LF", "print(#[[a\r\nb]], #[[a\rb]])", "3\t3\n"),
    ("string call", 'print "hi"', "hi\n"),
    ("compound assignment", "local a = 5 a += 2 a *= 3 a -= 1 a ..= '!' print(a) b = 7 b \\= 2 b ^^= 1 print(b)", "20!\n2\n"),
    ("one-line if", "local x = 3\nif (x > 2) print('big') x = 0\nif (x > 2) print('no') else print('else')\nprint(x)", "big\nelse\n0\n"),
    ("one-line while", "local n = 0\nwhile (n < 3) n += 1\nprint(n)", "3\n"),
    ("compound field assignment", "local q = {}\nq.v = 1 q.v += 4 q['v'] *= 2\nprint(q.v)", "10\n"),
    ("question print", "?1, 'two'\n? 3", "1\ttwo\n3\n"),
]
# Each program is a separate ./slap; they run in parallel and are checked in order.
POOL = concurrent.futures.ThreadPoolExecutor(os.cpu_count())


def run_all(cases):
    return POOL.map(lambda c: run_text(c[0].replace(" ", "-"), c[1]), cases)


for (name, text, want), r in zip(PICO8, run_all(PICO8)):
    check(
        f"pico8 {name}",
        r.returncode == 0 and r.stdout.decode() == want,
        f"exit {r.returncode}\nwant {want!r}\ngot  {r.stdout.decode()!r}\n{r.stderr.decode()[-600:]}",
    )

# Each program dies, exit 1, and its message names the line.
ERRORS = [
    (
        "arithmetic on nil",
        "print(1)\nprint(x + 1)",
        "lua: line 2: attempt to perform arithmetic on a nil value",
    ),
    ("call nil", "\n\nfoo()", "lua: line 3: attempt to call a nil value"),
    ("compare", 'print(1 < "a")', "lua: line 1: attempt to compare number with string"),
    (
        "concatenate",
        "print(1 .. nil)",
        "lua: line 1: attempt to concatenate a nil value",
    ),
    ("length", "print(#5)", "lua: line 1: attempt to get the length of a number value"),
    ("divide by zero", "print(1 / 0)", "lua: line 1: division by zero"),
    ("modulo by zero", "print(1 % 0)", "lua: line 1: modulo by zero"),
    (
        "fractional power",
        "print(2 ^ 0.5)",
        "lua: line 1: ^ takes a whole exponent from 0 to 32767",
    ),
    (
        "unclosed call",
        "print(1",
        "lua: line 1: expected ')' to close the arguments, found the end of the input",
    ),
    ("not a statement", "x", "lua: line 1: expected a statement, found 'x'"),
    ("not an expression", "print(+)", "lua: line 1: expected an expression, found '+'"),
    ("unfinished string", 'print("ab\n")', "lua: line 1: unfinished string"),
    ("unknown escape", 'print("\\q")', "lua: line 1: unknown escape \\q"),
    ("malformed number", "print(3x)", "lua: line 1: malformed number"),
    (
        "long fraction",
        "print(0.1234567891)",
        "lua: line 1: a decimal fraction has more than 9 digits",
    ),
    ("unexpected character", "print(1) $", "lua: line 1: unexpected character 36"),
    (
        "unfinished long string",
        "print([[abc",
        "lua: line 1: unfinished long string or comment",
    ),
    ("backslash at the end", 'print("a\\', "lua: line 1: unfinished string"),
    ("CR counts lines", "print(1)\rprint(nil+1)", "lua: line 2: attempt to perform arithmetic"),
    ("hex point alone", "print(0x.)", "lua: line 1: expected a digit"),
    ("NUL byte", "print(1)\0print(2)", "lua: line 1: unexpected character 0"),
    ("unfinished leveled comment", "print(1) --[==[ x\n]=]", "lua: line 1: unfinished long string or comment"),
    ("bad long bracket", "print([=x)", "lua: line 1: expected [ after [= to start a long string"),
    ("break outside a loop", "break", "lua: line 1: break outside a loop"),
    ("goto", "goto x", "lua: line 1: goto and labels are not supported"),
    ("vararg parameter", "function f(...) end", "lua: line 1: varargs (...) are not supported"),
    ("missing end", "if x then print(1)", "lua: line 1: expected 'end' to close 'if', found the end of the input"),
    ("return not last", "return 1 print(2)", "lua: line 1: expected the end of the block after 'return', found 'print'"),
    ("assign to a call", "f() = 1", "lua: line 1: expected a name or a field to assign to, found 'f'"),
    ("parameters need commas", "function f(a b) end", "lua: line 1: expected ')' to close the parameters, found 'b'"),
    ("deep recursion", "local function f(n) return f(n + 1) end f(1)", "lua: line 1: stack overflow: more than 1000 nested calls"),
    ("for step zero", "for i = 1, 2, 0 do end", "lua: line 1: 'for' step is zero"),
    ("for start", "for i = nil, 2 do end", "lua: line 1: 'for' start must be a number"),
    ("call a number", "local x = 5\nx()", "lua: line 2: attempt to call a number value"),
    ("index nil", "local t = nil\nprint(t.x)", "lua: line 2: attempt to index a nil value"),
    ("set a nil key", "local t = {}\nt[nil] = 1", "lua: line 2: table index is nil"),
    ("pairs of nil", "for k in pairs(nil) do end", "lua: line 1: bad argument #1 to 'pairs' (table expected, got nil)"),
    ("next of a missing key", "print(next({}, 'x'))", "lua: line 1: invalid key to 'next'"),
    ("method on nil", "local o = {}\no:m()", "lua: line 2: attempt to call a nil value"),
    ("for in needs in", "for k, v do end", "lua: line 1: expected '=' or 'in' after the for's names, found 'do'"),
    ("unclosed table", "local t = {1, 2", "lua: line 1: expected ',' or '}' in the table, found the end of the input"),
    ("varargs", "print(...)", "lua: line 1: varargs (...) are not supported"),
    (
        "deep nesting",
        "print(" + "(" * 300 + "1" + ")" * 300 + ")",
        "lua: line 1: an expression nests deeper than 200 levels",
    ),
]
for (name, text, want), r in zip(ERRORS, run_all(ERRORS)):
    err = r.stderr.decode()
    check(
        f"error {name}",
        r.returncode == 1 and want in err,
        f"exit {r.returncode}\nwant {want!r}\n{err[-600:]}",
    )

# Carts: the .p8 text format.
HEAD = "pico-8 cartridge // http://www.pico-8.com\nversion 42\n"
CARTS = [
    ("bad hex", "__lua__\n__gfx__\n" + "0g" + "0" * 126 + "\n", "lua: cart line 5: __gfx__ has a digit that is not hex"),
    ("short row", "__lua__\n__map__\n0102\n", "lua: cart line 5: a __map__ row has 4 digits, not 256"),
    ("cart error line", "__lua__\nx = 1\nx = x + nil\n", "lua: line 5: attempt to perform arithmetic on a nil value"),
    ("frame loop error", "__lua__\n_update = 5\n", "lua: the frame loop's line 2: attempt to call a number value"),
]
for name, text, want in CARTS:
    path = os.path.join(tmp, name.replace(" ", "-") + ".p8")
    with open(path, "w") as f:
        f.write(HEAD + text)
    r = subprocess.run(["./slap", path, "2"], input=SRC, capture_output=True, timeout=60)
    err = r.stderr.decode()
    check(f"cart {name}", r.returncode == 1 and want in err, f"exit {r.returncode}\nwant {want!r}\n{err[-600:]}")
path = os.path.join(tmp, "frames.p8")
with open(path, "w") as f:
    f.write(HEAD + "__lua__\nn = 0\nfunction _update() n = n + 1 end\nfunction _draw() assert_n = n end\n")
r = subprocess.run(["./slap", path, "3"], input=SRC, capture_output=True, timeout=60)
check("cart dump", r.returncode == 0 and r.stdout == (b"0" * 128 + b"\n") * 128, r.stderr.decode()[-600:])
# Each tests/pico8 cart draws what zepto8 drew: tests/pico8/refs.sh made the .ref
# files with it, after the frames the cart's "-- frames: N" line names.
def cart_diff(path):
    n = re.search(r"^-- frames: (\d+)$", open(path).read(), re.M).group(1)
    r = subprocess.run(["./slap", path, n], input=SRC, capture_output=True, timeout=60)
    want = open(path[:-3] + ".ref", "rb").read().split()
    got = r.stdout.split()
    bad = [(y, x) for y in range(min(len(want), len(got))) for x in range(128) if want[y][x : x + 1] != got[y][x : x + 1]]
    return r, len(want) == len(got) == 128 and not bad, f"{len(bad)} pixels differ, first at (x, y) = {bad[0][::-1] if bad else None}"


for path, (r, same, why) in zip(sorted(glob.glob("tests/pico8/*.p8")), POOL.map(cart_diff, sorted(glob.glob("tests/pico8/*.p8")))):
    check(f"{path} matches zepto8", r.returncode == 0 and same, f"exit {r.returncode}: {why}\n{r.stderr.decode()[-600:]}")
# The SDL shell, headless: a held left arrow reaches btn(0), and the view the
# render body draws holds the cart's screen.
if os.path.exists("./slap-sdl"):
    path = os.path.join(tmp, "btn.p8")
    with open(path, "w") as f:
        f.write(HEAD + "__lua__\nfunction _draw() cls(1) if btn(0) then pset(0, 0, 8) end end\n")
    driver = (
        "('tf let tf 2 eq (1073741904 1 key!) () if"
        " tf 5 eq (dup 'screen at dup 0 get must print 1 get must print \"test-done\" fail) () if) 'tick on show\n"
    )
    sdl = open("examples/lua-sdl.slap").read()
    sdl = sdl[: sdl.rindex(") show")] + ") 'render let\n"
    r = subprocess.run(["./slap-sdl", "--headless", path], input=SRC + sdl.encode() + b"(render)" + driver.encode(), capture_output=True, timeout=60)
    check("lua-sdl headless", r.returncode == 1 and r.stdout == b"8\n1\n" and b"test-done" in r.stderr, f"exit {r.returncode} {r.stdout!r}\n{r.stderr.decode()[-600:]}")

r = run_text("frames-need-a-cart", "print(1)")
r = subprocess.run(["./slap", os.path.join(tmp, "frames-need-a-cart.lua"), "3"], input=SRC, capture_output=True, timeout=60)
check("frames need a cart", r.returncode == 1 and b"lua: a frame count needs a .p8 cart" in r.stderr, r.stderr.decode()[-600:])

r = subprocess.run(["./slap"], input=SRC, capture_output=True, timeout=60)
check(
    "no argument",
    r.returncode == 1 and b"usage: cat examples/lib/strings.slap" in r.stderr,
    r.stderr.decode()[-600:],
)
r = run(os.path.join(tmp, "missing.lua"))
check(
    "missing file",
    r.returncode == 1 and b"lua: cannot read" in r.stderr,
    r.stderr.decode()[-600:],
)

print(f"lua: {count[0]} checks passed")
