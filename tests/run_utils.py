#!/usr/bin/env python3
"""Integration test for examples/utils: each util runs on real files and must
print exactly what the system tool prints under LC_ALL=C."""

import os, random, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(__file__))
import harness

LIBS = ["examples/lib/strings.slap"]
check, count = harness.make_check("utils")
tmp = tempfile.mkdtemp(prefix="slap-utils-")


def words(n):
    rng = random.Random(7)
    vocab = [
        "apple",
        "Apple",
        "banana",
        "ö",
        "日本",
        "a-b",
        "",
        "x y",
        "\tz",
        "zz",
        "a",
    ]
    return "".join(
        " ".join(rng.choice(vocab) for _ in range(rng.randrange(4))) + "\n"
        for _ in range(n)
    )


INPUTS = {
    "empty": "",
    "unterminated": "hello",
    "blank": "\n\na\n\n",
    "duplicates": "a\na\nb\nb\nb\na\n",
    "utf8": "héllo wörld\n日本語 テキスト\n  spaced   out\t\ttabs\r\n",
    "many": words(3000),
}
PATHS = {}
for name, text in INPUTS.items():
    PATHS[name] = os.path.join(tmp, name + ".txt")
    with open(PATHS[name], "wb") as f:
        f.write(text.encode())

ENV = dict(os.environ, LC_ALL="C")


def slap(util, *argv):
    src = b"".join(open(f, "rb").read() for f in LIBS + [f"examples/utils/{util}.slap"])
    return subprocess.run(["./slap", *argv], input=src, capture_output=True, timeout=60)


def system(*argv):
    return subprocess.run(argv, capture_output=True, env=ENV, timeout=60).stdout


for name, path in PATHS.items():
    for util, argv, tool in [
        ("cat", [path], ["cat", path]),
        ("head", [path], ["head", path]),
        ("uniq", [path], ["uniq", path]),
        ("sort", [path], ["sort", path]),
        ("grep", ["a", path], ["grep", "-F", "a", path]),
        ("grep", ["ö", path], ["grep", "-F", "ö", path]),
        ("grep", ["", path], ["grep", "-F", "", path]),
    ]:
        r = slap(util, *argv)
        want = system(*tool)
        # BSD uniq copies an unterminated last line as it is; GNU uniq ends it, and so does this one.
        if util == "uniq" and want and not want.endswith(b"\n"):
            want += b"\n"
        check(
            f"{util} {' '.join(argv[:-1])} {name}",
            r.returncode == 0 and r.stdout == want,
            f"exit {r.returncode}\nwant {want[:200]!r}\ngot  {r.stdout[:200]!r}\n{r.stderr.decode()[-600:]}",
        )
    # BSD and GNU wc pad their columns differently, so compare the three counts.
    r = slap("wc", path)
    want = system("wc", path).split()[:3]
    check(
        f"wc {name}",
        r.returncode == 0 and r.stdout.split()[:3] == want,
        f"exit {r.returncode}\nwant {want}\ngot  {r.stdout[:200]!r}\n{r.stderr.decode()[-600:]}",
    )

r = slap("cat", os.path.join(tmp, "missing.txt"))
check("cat missing file", r.returncode == 1 and b"cannot read" in r.stderr and b"missing.txt" in r.stderr, r.stderr.decode()[-600:])
r = subprocess.run(["./slap"], input=open(LIBS[0], "rb").read() + b"0 arg-bytes drop", capture_output=True, timeout=60)
check("arg-bytes no argument", r.returncode == 1 and b"must:" not in r.stderr and b"expected a file name as argument" in r.stderr, r.stderr.decode()[-600:])
r = slap("cat")
check("cat no argument", r.returncode == 1 and b"expected 1 argument(s): FILE" in r.stderr, r.stderr.decode()[-600:])
r = slap("cat", PATHS["blank"], PATHS["blank"])
check("cat two arguments", r.returncode == 1 and b"expected 1 argument(s): FILE" in r.stderr, r.stderr.decode()[-600:])
r = slap("grep", "a")
check("grep one argument", r.returncode == 1 and b"expected 2 argument(s): PATTERN FILE" in r.stderr, r.stderr.decode()[-600:])
big = os.path.join(tmp, "big.txt")
with open(big, "wb") as f:
    f.write(words(40000).encode())
r = slap("sort", big)
check("sort 40000 lines", r.returncode == 0 and r.stdout == system("sort", big), r.stderr.decode()[-600:])

print(f"utils: {count[0]} checks passed")
