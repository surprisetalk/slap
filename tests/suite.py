#!/usr/bin/env python3
"""Run every check in parallel. `suite.py` is `make test`, `suite.py slow` is
`make test-slow`, and `suite.py status` is `make status`."""

import concurrent.futures, glob, json, os, re, shutil, subprocess, sys, tempfile, threading, time

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
            "closed stdout": ("echo '42 print' | ./slap >&-; test $? -eq 1", None),
            "stdout reader quits": ("echo '(1) (1 print) while' | ./slap | head -1 >/dev/null; true", None),
            "wiki": ("python3 tests/run_wiki.py", None),
            "kv": ("python3 tests/run_kv.py", None),
            "feed": ("python3 tests/run_feed.py", None),
            "todo": ("python3 tests/run_todo.py", None),
            "serve": ("python3 tests/run_serve.py", None),
            "codec": ("python3 tests/run_codec.py", None),
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
