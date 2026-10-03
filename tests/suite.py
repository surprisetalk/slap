#!/usr/bin/env python3
"""Run every check in parallel. `suite.py` is `make test`, `suite.py slow` is
`make test-slow`, and `suite.py status` is `make status`."""

import concurrent.futures, glob, json, os, random, re, shutil, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(__file__))
import harness

# Euler problems that take more than 2 s each. They run under `make test-slow`.
SLOW_EULER = {12, 14, 23, 25, 34, 47}
TIMEOUT = 60
SCRATCH = tempfile.mkdtemp(prefix="slap-suite-")


def lib(*names):
    return " ".join(f"examples/lib/{n}.slap" for n in names)


def euler(path):
    m = re.search(r"^-- Answer: (\S+)", open(path).read(), re.M)
    if not m:
        return None
    return (
        f"cat {lib('strings')} {path} | ./slap",
        lambda out, want=m.group(1): out.strip().splitlines()[-1:] == [want],
    )


def closures():
    """A closure that escapes a word gets its own frame; 20,000 of them must stay small."""
    src = b"('x let (x)) 'mk let 0 (dup 20000 lt) (dup mk drop 1 plus) while 20000 eq assert\n"
    err = tempfile.TemporaryFile()
    p = subprocess.Popen(["./slap"], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=err)
    p.stdin.write(src)
    p.stdin.close()
    # os.wait4 is the only way to read one child's peak RSS, and it has no timeout.
    watchdog = threading.Timer(TIMEOUT, p.kill)
    watchdog.start()
    _, status, ru = os.wait4(p.pid, 0)
    watchdog.cancel()
    # ru_maxrss is bytes on macOS and KiB on Linux.
    mb = ru.ru_maxrss / (1 << 20 if sys.platform == "darwin" else 1 << 10)
    code = os.waitstatus_to_exitcode(status)
    err.seek(0)
    return code == 0 and mb < 200, f"exit {code}, peak RSS {mb:.0f} MB (limit 200)\n" + err.read().decode()[-2000:]


def profile():
    """--profile prints folded stacks, one `a;b;c nanoseconds` line per call path, even when the program dies."""
    folded = re.compile(r"^[^ ;]+(;[^ ;]+)* [0-9]+$")
    def run(src):
        r = subprocess.run(["./slap", "--profile"], input=src, capture_output=True, text=True, timeout=TIMEOUT)
        return r.returncode, r.stderr.splitlines()
    code, lines = run("(1 plus) 'inc2 let (0 (dup 1000 lt) (inc2) while) 'count let count drop\n")
    if code != 0 or not lines or not all(folded.match(l) for l in lines):
        return False, f"plain run: exit {code}\n" + "\n".join(lines[:20])
    if not any(re.search(r"(^|;)count;while;inc2;plus [0-9]+$", l) for l in lines):
        return False, "no count;while;inc2;plus line:\n" + "\n".join(lines[:20])
    code, lines = run("(0 (dup 3000 lt) (1 plus) while drop) 'pc let (1 (pc) () if) 'pb let (1 (pb) () if) 'pa let pa\n")
    if code != 0 or not any(re.search(r"^pa;if;pb;if;pc;while", l) for l in lines):
        return False, "nested words under if must keep their own frames:\n" + "\n".join(lines[:20])
    code, lines = run("(1 0 div) 'boom let boom\n")
    if code != 1 or not any(re.match(r"^boom;div [0-9]+$", l) for l in lines):
        return False, f"dying run: exit {code}\n" + "\n".join(lines[-20:])
    code, lines = run("'odd? [int lent in  int move out] effect\n"
                      "(dup 0 eq (drop 1) (1 sub odd?) if) 'even? let\n"
                      "(dup 0 eq (drop 0) (1 sub even?) if) 'odd? let\n"
                      "2000 even? drop\n")
    if code != 0 or len(lines) > 20:
        return False, f"mutual recursion: exit {code}, {len(lines)} lines (limit 20)\n" + "\n".join(lines[:30])
    return True, ""


def steps(slow):
    euler_files = sorted(
        glob.glob("examples/euler/*.slap"), key=lambda p: int(re.sub(r"\D", "", p))
    )
    missing = SLOW_EULER - {int(re.sub(r"\D", "", p)) for p in euler_files}
    if missing:
        sys.exit(f"suite: SLOW_EULER names problems with no file: {sorted(missing)}")
    pick = [p for p in euler_files if (int(re.sub(r"\D", "", p)) in SLOW_EULER) == slow]
    out = {}
    for p in pick:
        e = euler(p)
        if e is None:
            out[p] = (f"echo '{p} has no -- Answer: line' >&2; false", None)
        else:
            out[p] = e
    if slow:
        return out
    out.update(
        {
            "expect.slap": (
                f"cat {lib('strings', 'parse')} tests/expect.slap | ./slap hello world {SCRATCH}/fs.bin",
                None,
            ),
            "errors": ("python3 tests/run_errors.py", None),
            "closures": (closures, None),
            "profile": (profile, None),
            "deep closure chain": (
                "echo \"(0) 100000 ('c let (c apply 1 plus) 'g let 'g quote) repeat drop\" | ./slap",
                None,
            ),
            "deep value error prints once": (
                "test $(echo \"[] 'n tag 20000 (list swap push 'n tag) repeat print\" | ./slap 2>&1 | grep -c 'C stack exhausted') -eq 1",
                None,
            ),
            "closed stdout": ("echo '42 print' | ./slap >&-; test $? -eq 1", None),
            "stdout reader quits": ("echo '(1) (1 print) while' | ./slap | head -1 >/dev/null; true", None),
            "stdout write keeps order": (
                """echo '42 print "/dev/stdout" "x" write must drop 43 print' | ./slap""",
                lambda out: out == "42\nx43\n",
            ),
            "wiki": ("python3 tests/run_wiki.py", None),
            "kv": ("python3 tests/run_kv.py", None),
            "feed": ("python3 tests/run_feed.py", None),
            "todo": ("python3 tests/run_todo.py", None),
            "serve": ("python3 tests/run_serve.py", None),
            "codec": ("python3 tests/run_codec.py", None),
            "utils": ("python3 tests/run_utils.py", None),
        }
    )
    for name in ["chip8", "uxn", "maze", "raycast"]:
        out[name] = (
            f"./slap --headless < examples/{name}.slap",
            lambda o, n=name: f"{n}-selftest-ok" in o,
        )
    for name in ["icn", "chr", "nmt", "tga", "gly", "ulz"]:
        out[f"lib/{name}"] = (f"./slap < examples/lib/{name}.slap", None)
    for combo in [
        ("icn", "ufx"),
        ("strings", "parse", "json"),
        ("strings", "parse", "xml", "rss"),
    ]:
        out["lib/" + "+".join(combo)] = (f"cat {lib(*combo)} | ./slap", None)
    out["scale"] = (f"cat {lib('strings', 'parse', 'json', 'xml')} tests/scale.slap | ./slap", None)
    for name in [
        "ant",
        "dots",
        "fish",
        "flock",
        "fonts",
        "gradient",
        "life",
        "scratch",
        "snake",
        "zoom",
    ]:
        out[name] = (f"./slap --check < examples/{name}.slap", None)
    return out


def run(name, cmd, ok):
    start = time.time()
    if callable(cmd):
        passed, detail = cmd()
        return name, cmd.__name__, passed, detail, time.time() - start
    try:
        r = subprocess.run(
            ["bash", "-o", "pipefail", "-c", cmd],
            capture_output=True,
            text=True,
            timeout=TIMEOUT,
        )
        passed = r.returncode == 0 and (ok is None or ok(r.stdout))
        detail = r.stdout[-2000:] + r.stderr[-4000:]
    except subprocess.TimeoutExpired:
        passed, detail = False, f"timed out after {TIMEOUT} s"
    return name, cmd, passed, detail, time.time() - start


def status():
    """Each condition scores 1.0 at the minimum pass and 0.0 at total failure."""
    score = {}
    t = time.time()
    r = subprocess.run([sys.executable, __file__], capture_output=True)
    score["make test passes within 10 s."] = 10 / (time.time() - t) if r.returncode == 0 else 0.0
    docs = open("readme.md").read() + open("claude.md").read()
    samples = re.findall(r"```slap\n(.*?)```", open("readme.md").read(), re.S)
    ran = 0
    for code in samples:
        # SDL and network samples cannot run here; they must still type-check.
        check = ["--check"] if re.search(r"\b(show|on|tcp-\w+)\b", code) else []
        src = open(lib("strings")).read() + code
        ran += subprocess.run(["./slap", *check], input=src, capture_output=True, text=True, timeout=TIMEOUT).returncode == 0
    score["Every readme slap sample runs."] = ran / len(samples)
    paths = set(re.findall(r"\b(?:examples|tests|fonts|assets)/[\w./-]*\w\.\w+", docs))
    score["Every file the docs name exists."] = sum(os.path.exists(p) for p in paths) / max(len(paths), 1)
    score["The docs cite no slap.c line numbers."] = 0.0 if re.search(r"slap\.c:\d", docs) else 1.0
    r = subprocess.run(
        ["cc", "-std=c99", "-Wall", "-Wextra", "-O3", "-D_POSIX_C_SOURCE=200809L", "-o", os.path.join(SCRATCH, "slap"), "slap.c", "-lm"],
        capture_output=True, text=True,
    )
    score["slap.c compiles with no warnings."] = 1.0 if r.returncode == 0 and "warning" not in r.stderr else 0.0
    feed = os.path.join(SCRATCH, "feed.xml")
    with open(feed, "w") as f:
        f.write(harness.big_feed(4100))
    assert os.path.getsize(feed) >= 600_000, os.path.getsize(feed)
    src = "".join(open(lib(n)).read() for n in ("strings", "parse", "xml", "rss")) + open("examples/feed.slap").read()
    t = time.time()
    r = subprocess.run(["./slap", feed], input=src, capture_output=True, text=True, timeout=60)
    score["A 600 KB feed renders in under a second."] = 1 / (time.time() - t) if r.stdout.rstrip().endswith("4100 items") else 0.0
    rng = random.Random(1)
    vocab = ["apple", "Apple", "banana", "cherry", "ö", "日本", "zeta", "alpha", "beta", "gamma", "delta"]
    text = "".join(" ".join(rng.choice(vocab) for _ in range(rng.randrange(1, 6))) + "\n" for _ in range(20000)).encode()
    lines = os.path.join(SCRATCH, "lines.txt")
    with open(lines, "wb") as f:
        f.write(text)
    src = open(lib("strings"), "rb").read() + open("examples/utils/sort.slap", "rb").read()
    t = time.time()
    r = subprocess.run(["./slap", lines], input=src, capture_output=True, timeout=60)
    ok = r.returncode == 0 and r.stdout == b"".join(sorted(text.splitlines(keepends=True)))
    score["sort.slap sorts 20,000 lines in under a second."] = 1 / (time.time() - t) if ok else 0.0
    shutil.rmtree(SCRATCH)
    print(json.dumps({k: {"0": round(v, 2)} for k, v in score.items()}, indent=1))
    if min(score.values()) < 1.0:
        sys.exit(1)


def main():
    if not os.access("./slap", os.X_OK):
        sys.exit("suite: no ./slap binary. Run `make slap` from the repo root.")
    if sys.argv[1:] == ["status"]:
        return status()
    todo = steps(slow=sys.argv[1:] == ["slow"])
    start, failed, slowest = time.time(), [], ("", 0.0)
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for name, cmd, passed, detail, dur in pool.map(
            lambda kv: run(kv[0], *kv[1]), todo.items()
        ):
            slowest = max(slowest, (name, dur), key=lambda t: t[1])
            if not passed:
                failed.append(name)
                print(
                    f"FAIL {name} ({dur:.1f} s)\n  $ {cmd}\n" + detail.rstrip(),
                    file=sys.stderr,
                )
    took = time.time() - start
    shutil.rmtree(SCRATCH)
    if failed:
        sys.exit(
            f"suite: {len(failed)} of {len(todo)} failed in {took:.1f} s: {', '.join(failed)}"
        )
    print(
        f"suite: {len(todo)} checks passed in {took:.1f} s (slowest: {slowest[0]} {slowest[1]:.1f} s)"
    )


if __name__ == "__main__":
    main()
