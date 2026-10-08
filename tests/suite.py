#!/usr/bin/env python3
"""Run every check in parallel. `suite.py` is `make test`, `suite.py slow` is
`make test-slow`, and `suite.py status` is `make status`."""

import concurrent.futures, glob, json, os, random, re, shlex, shutil, socket, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(__file__))
import harness

# Euler problems that take more than 2 s each. They run under `make test-slow`.
SLOW_EULER = {12, 14, 23, 25, 34, 47}
TIMEOUT = 60
SCRATCH = tempfile.mkdtemp(prefix="slap-suite-")


def lib(*names):
    return " ".join(f"examples/lib/{n}.slap" for n in names)


# A shell command: each program, run after the libraries, exits 1 with its message on stderr.
def refuses(libs, cases):
    q = shlex.quote
    run = f'out=$({{ cat {lib(*libs)}; echo "$1"; }} | ./slap 2>&1 >/dev/null); test $? -eq 1 && grep -qF -- "$2" <<<"$out"'
    return f"w() {{ {run}; }}; " + " && ".join(f"w {q(p)} {q(m)}" for p, m in cases)


def euler(path):
    m = re.search(r"^-- Answer: (\S+)", open(path).read(), re.M)
    if not m:
        return None
    return (
        f"cat {lib('strings')} {path} | ./slap",
        lambda out, want=m.group(1): out.strip().splitlines()[-1:] == [want],
    )


# fill-rect exists only in slap-sdl. A rect this large takes minutes unless it is clipped before it draws.
FILL_RECT = (
    "test -x ./slap-sdl || { echo 'no ./slap-sdl: run make slap-sdl' >&2; exit 1; };"
    ' out=$(echo "0 (drop 0 0 1000000000 1000000000 3 fill-rect \\"drew\\" fail) \'tick on (drop) show" | ./slap-sdl --headless 2>&1);'
    ' test $? -eq 1 && grep -q drew <<<"$out"'
)
# A color outside 0-3 dies naming the word and the color, also for a pixel off the canvas.
BAD_COLOR = (
    "test -x ./slap-sdl || { echo 'no ./slap-sdl: run make slap-sdl' >&2; exit 1; };"
    " for c in '0 0 4 pixel:pixel: color 4 is not 0-3' '-1 -1 9 pixel:pixel: color 9 is not 0-3'"
    " '4 clear:clear: color 4 is not 0-3' '0 0 1 1 -1 fill-rect:fill-rect: color -1 is not 0-3'; do"
    ' out=$(echo "0 (drop ${c%%:*}) \'tick on (drop) show" | ./slap-sdl --headless 2>&1);'
    ' test $? -eq 1 && grep -qF "${c#*:}" <<<"$out" || exit 1; done'
)
# Without SDL2, make test leaves out the slap-sdl check and says so; make status fails a condition.
HAS_SDL = shutil.which("sdl2-config") is not None

DICT = 'dict "k" 1 insert'
# Each program makes and drops a value 100,000 times. A leak of one dict per pass passes 30 MB.
# The box program runs 1,000,000 passes: a box's own header is 16 bytes.
STEADY = [
    "('x let (x)) 'mk let 0 (dup 100000 lt) (dup mk drop 1 plus) while 100000 eq assert",
    "('x let x 'a tag {'a (x plus)} case) 'f let 0 100000 (drop 5 f) repeat drop",
    f"0 1000000 (drop {DICT} box free 0) repeat drop",
    f"0 100000 (drop {{'b {DICT}}} drop 0) repeat drop",
    f"0 100000 (drop [] {DICT} push dup print len) repeat drop",
    f"0 100000 (drop [] {DICT} push [] {DICT} push eq) repeat drop",
    f"0 100000 (drop [] {DICT} push {DICT} push dup 0 get must drop 5 get drop 0) repeat drop",
    f"0 100000 (drop [] {DICT} push {DICT} push 1 take-n drop 0) repeat drop",
    f"0 100000 (drop [] {DICT} push {DICT} push 1 drop-n drop 0) repeat drop",
    f"0 100000 (drop [] {DICT} push {DICT} push {DICT} index-of must) repeat drop",
    f"0 100000 (drop {{}} {DICT} 'd into 1 'n into 'n at) repeat drop",
    f"0 100000 (drop [] {DICT} push {DICT} push (drop 0) filter len) repeat drop",
]


def steady():
    """A loop that makes and drops closures, frames and dicts holds steady memory."""
    bad = []
    for src in STEADY:
        err = tempfile.TemporaryFile()
        p = subprocess.Popen(
            ["./slap"], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=err
        )
        p.stdin.write(src.encode())
        p.stdin.close()
        # os.wait4 is the only way to read one child's peak RSS, and it has no timeout.
        watchdog = threading.Timer(TIMEOUT, p.kill)
        watchdog.start()
        _, status, ru = os.wait4(p.pid, 0)
        watchdog.cancel()
        # ru_maxrss is bytes on macOS and KiB on Linux.
        mb = ru.ru_maxrss / (1 << 20 if sys.platform == "darwin" else 1 << 10)
        code = os.waitstatus_to_exitcode(status)
        if code != 0 or mb >= 16:
            err.seek(0)
            bad.append(
                f"exit {code}, peak RSS {mb:.0f} MB (limit 16): {src}\n"
                + err.read().decode()[-500:]
            )
    return not bad, "\n".join(bad)


def profile():
    """--profile prints folded stacks, one `a;b;c nanoseconds` line per call path, even when the program dies."""
    folded = re.compile(r"^[^ ;]+(;[^ ;]+)* [0-9]+$")

    def run(src):
        r = subprocess.run(
            ["./slap", "--profile"],
            input=src,
            capture_output=True,
            text=True,
            timeout=TIMEOUT,
        )
        return r.returncode, r.stderr.splitlines()

    code, lines = run(
        "(1 plus) 'inc2 let (0 (dup 1000 lt) (inc2) while) 'count let count drop\n"
    )
    if code != 0 or not lines or not all(folded.match(l) for l in lines):
        return False, f"plain run: exit {code}\n" + "\n".join(lines[:20])
    if not any(re.search(r"(^|;)count;while;inc2;plus [0-9]+$", l) for l in lines):
        return False, "no count;while;inc2;plus line:\n" + "\n".join(lines[:20])
    code, lines = run(
        "(0 (dup 3000 lt) (1 plus) while drop) 'pc let (1 (pc) () if) 'pb let (1 (pb) () if) 'pa let pa\n"
    )
    if code != 0 or not any(re.search(r"^pa;if;pb;if;pc;while", l) for l in lines):
        return False, "nested words under if must keep their own frames:\n" + "\n".join(
            lines[:20]
        )
    code, lines = run("(1 0 div) 'boom let boom\n")
    if code != 1 or not any(re.match(r"^boom;div [0-9]+$", l) for l in lines):
        return False, f"dying run: exit {code}\n" + "\n".join(lines[-20:])
    code, lines = run(
        "'odd? [int lent in  int move out] effect\n"
        "(dup 0 eq (drop 1) (1 sub odd?) if) 'even? let\n"
        "(dup 0 eq (drop 0) (1 sub even?) if) 'odd? let\n"
        "2000 even? drop\n"
    )
    if code != 0 or len(lines) > 20:
        return (
            False,
            f"mutual recursion: exit {code}, {len(lines)} lines (limit 20)\n"
            + "\n".join(lines[:30]),
        )
    return True, ""


def timed_slap(src, limit, want='"no '):
    start = time.time()
    try:
        r = subprocess.run(
            ["./slap"], input=src, capture_output=True, text=True, timeout=limit
        )
    except subprocess.TimeoutExpired:
        return False, f"no answer within {limit} s"
    took = time.time() - start
    detail = f"{r.stdout.strip()} (after {took:.1f} s)"
    return r.returncode == 0 and r.stdout.startswith(want), detail + r.stderr[-2000:]


def recv_silent_peer():
    port = random.randint(41000, 49000)
    # The listener never accepts: the backlog completes the connection, so the client waits.
    return timed_slap(
        f'{port} tcp-listen must "127.0.0.1" {port} tcp-connect must 16 tcp-recv'
        ' {\'ok (drop "got data" print) \'no ("no " swap cat print)} case tcp-close tcp-close',
        35,
        '"no timed out after 30 s"',
    )


def send_trickle_peer():
    # A peer that reads 4 KB every 5 s frees room before a blocking send's timeout runs out, so that timeout alone never ends the call.
    # 40 sends of 512 KB overfill the socket buffers. Each send prints how long it took when it gives 'no; the kernel may grow its buffers
    # first, so the run as a whole takes longer than one send.
    srv = socket.create_server(("127.0.0.1", 0))
    port, stop = srv.getsockname()[1], threading.Event()

    def trickle():
        conn, _ = srv.accept()
        with conn:
            while not stop.wait(5):
                conn.recv(4096)

    threading.Thread(target=trickle, daemon=True).start()
    send = "millis swap b tcp-send {'ok (drop swap drop) 'no (print swap millis swap sub print \"stuck\" fail)} case "
    try:
        r = subprocess.run(
            ["./slap"],
            input=f'"x" 19 (dup cat) repeat \'b let "127.0.0.1" {port} tcp-connect must ' + send * 40 + "tcp-close",
            capture_output=True,
            text=True,
            timeout=90,
        )
    except subprocess.TimeoutExpired:
        return False, "no answer within 90 s"
    finally:
        stop.set()
        srv.close()
    out = r.stdout.split()
    took = int(out[-1]) if out and out[-1].isdigit() else None
    detail = f"stdout {r.stdout[-200:]!r}, stderr {r.stderr[-200:]!r}"
    return r.returncode != 0 and '"timed out after 30 s"' in r.stdout and took is not None and 29000 <= took <= 31000, detail


def connect_blackhole():
    # Some networks refuse 10.255.255.1 at once; the 'no then names that reason.
    passed, detail = timed_slap(
        '"10.255.255.1" 80 tcp-connect {\'ok (tcp-close "connected" print) \'no ("no " swap cat print)} case',
        15,
    )
    print(f"tcp-connect to 10.255.255.1 got: {detail}", file=sys.stderr)
    return passed, detail


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
        out["tcp-recv on a silent peer gives 'no within 35 s"] = (
            recv_silent_peer,
            None,
        )
        out["tcp-send to a peer that reads a trickle gives 'no after 30 s"] = (
            send_trickle_peer,
            None,
        )
        out["tcp-connect to a blackhole gives 'no within 15 s"] = (
            connect_blackhole,
            None,
        )
        return out
    out.update(
        {
            "expect.slap": (
                f"cat {lib('strings', 'parse')} tests/expect.slap | ./slap hello world {SCRATCH}/fs.bin",
                None,
            ),
            "errors": ("python3 tests/run_errors.py", None),
            "steady memory": (steady, None),
            "profile": (profile, None),
            "deep closure chain": (
                'echo "(0) 100000 (\'c let (c apply 1 plus)) repeat drop" | ./slap',
                None,
            ),
            "deep value error prints once": (
                "test $(echo \"[] 'n tag 20000 ([] swap push 'n tag) repeat print\" | ./slap 2>&1 | grep -c 'C stack exhausted') -eq 1",
                None,
            ),
            "writers refuse": (
                refuses(
                    ("strings", "parse", "json", "xml", "rss"),
                    [
                        ("""[] {} "a" 'name into 1 je-int 'value into push {} "a" 'name into 2 je-int 'value into push je-obj print""", 'json: je-obj: duplicate key "a"'),
                        (""""r" [] {} "x" 'name into "1" 'value into push {} "x" 'name into "2" 'value into push [] xe-elem xml-render print""", 'xml: duplicate attribute "x" in <r>'),
                        (""""r" [] {} "x y" 'name into "1" 'value into push [] xe-elem xml-render print""", 'xml: attribute name "x y" in <r> is not an XML name'),
                        (""""" [] [] xe-elem xml-render print""", 'xml: element name "" is not an XML name'),
                        (""""1a" [] [] xe-elem xml-pretty print""", 'xml: element name "1a" is not an XML name'),
                    ],
                ),
                None,
            ),
            # Inputs errors.slap cannot hold: a NUL byte, a 3 MB literal, a small C stack, an SDL build.
            "NUL byte in the program": (
                "out=$(printf '1 print\\0 2 print\\n' | ./slap 2>&1); test $? -eq 1 && grep -q 'NUL byte at offset 7' <<<\"$out\"",
                None,
            ),
            # A file one byte past the limit, never an endless one: a read without the bound
            # allocates until the disk fills with swap, since macOS ignores ulimit -v.
            "read past the stack": (
                f"head -c 2097151 /dev/zero > {SCRATCH}/big.bin; out=$(echo '\"{SCRATCH}/big.bin\" read must len print' | ./slap 2>&1);"
                " test $? -eq 1 && grep -q 'holds more than 2097150 bytes' <<<\"$out\"",
                None,
            ),
            "program past 16 MiB": (
                "out=$(head -c 16777217 /dev/zero | tr '\\0' ' ' | ./slap 2>&1); test $? -eq 1 && grep -q 'larger than 16 MiB' <<<\"$out\"",
                None,
            ),
            "a program past the token limit says so briefly": (
                "out=$(python3 -c \"print('1 ' * 70000)\" | ./slap 2>&1); test $? -eq 1 && grep -q 'the program has more than 65[0-9]* tokens; the limit is 65[0-9]* (TOK_MAX minus [0-9]* for the prelude)' <<<\"$out\" && grep -q 'ERROR <stdin> -' <<<\"$out\" && ! grep -q 'source unavailable' <<<\"$out\" && test ${#out} -lt 1000",
                None,
            ),
            "a plain underflow through a let-bound body gets no let hint": (
                "out=$(printf \"[(1 plus)] first 'f let (f apply) 'g let g\\n\" | ./slap 2>&1); test $? -eq 1 && grep -q 'but the stack has nothing' <<<\"$out\" && ! grep -q 'bound with let' <<<\"$out\"",
                None,
            ),
            "an error's stack dump cuts a long string": (
                "out=$(echo '\"x\" 20 (dup cat) repeat 1 0 div' | ./slap 2>&1); test $? -eq 1 && grep -q '1048376 more bytes' <<<\"$out\" && test ${#out} -lt 2000",
                None,
            ),
            "old-order nth is one error": (
                "out=$(printf \"[1 2] 'xs let\\n'xs 1 nth must print\\n\" | ./slap 2>&1); test $? -eq 1 && grep -q 'nth needs the list' <<<\"$out\" && ! grep -q 'int index' <<<\"$out\"",
                None,
            ),
            "unreadable stdin": (
                "out=$(./slap < / 2>&1); test $? -eq 1 && grep -q 'cannot read the program' <<<\"$out\"",
                None,
            ),
            # Two dips hold 1.9M slots on the aux stack, so swap has no room to park a run there.
            "swap with a full aux stack": (
                "echo '0 950000 range 0 950000 range ((0 250000 range 0 250000 range swap len print len print) dip) dip len print len print' | ./slap",
                lambda out: out == "250000\n250000\n950000\n950000\n",
            ),
            "string literal past the stack": (
                "out=$(python3 -c \"print('\\\"' + 'a' * 3000000 + '\\\" len print')\" | ./slap 2>&1); test $? -eq 1 && grep -q 'a string literal: stack overflow' <<<\"$out\"",
                None,
            ),
            "small C stack": (
                "(ulimit -s 4096; out=$(python3 -c \"print('[ ' * 20000 + ' ] ' * 20000)\" | ./slap 2>&1); test $? -eq 1 && grep -q 'C stack exhausted' <<<\"$out\")"
                " && (ulimit -s 1024; out=$(echo '1 print' | ./slap 2>&1); test $? -eq 1 && grep -q 'ulimit -s 8192' <<<\"$out\")",
                None,
            ),
            "closed stdout": ("echo '42 print' | ./slap >&-; test $? -eq 1", None),
            "stdout reader quits": (
                "echo '(1) (1 print) while' | ./slap | head -1 >/dev/null; true",
                None,
            ),
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
    if HAS_SDL:
        out["fill-rect clips a huge rect"] = (FILL_RECT, None)
        out["a color outside 0-3 dies"] = (BAD_COLOR, None)
    else:
        print(
            "suite: warning: sdl2-config is not on PATH, so slap-sdl is not built and the fill-rect check"
            " does not run. make status fails until SDL2 is installed (brew install sdl2, or"
            " nix-shell -p SDL2 pkg-config).",
            file=sys.stderr,
        )
    for name in ["chip8", "uxn", "maze", "raycast", "zoom"]:
        out[name] = (
            f"cat examples/{name}.slap examples/{name}-sdl.slap | ./slap --check"
            f" && ./slap --headless < examples/{name}.slap",
            lambda o, n=name: f"{n}-selftest-ok" in o,
        )
    for name in ["icn", "chr", "nmt", "tga", "gly", "ulz"]:
        out[f"lib/{name}"] = (f"./slap < examples/lib/{name}.slap", None)
    for combo in [
        ("icn", "ufx"),
        ("strings", "parse", "json"),
        ("strings", "http"),
        ("strings", "parse", "xml", "rss"),
    ]:
        out["lib/" + "+".join(combo)] = (f"cat {lib(*combo)} | ./slap", None)
    out["scale"] = (
        f"cat {lib('strings', 'parse', 'json', 'xml', 'rss', 'http')} tests/scale.slap | ./slap",
        None,
    )
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
    score["make test passes within 10 s."] = (
        10 / (time.time() - t) if r.returncode == 0 else 0.0
    )
    docs = open("readme.md").read() + open("claude.md").read()
    samples = re.findall(r"```slap\n(.*?)```", open("readme.md").read(), re.S)
    ran = 0
    for code in samples:
        # SDL and network samples cannot run here; they must still type-check.
        check = ["--check"] if re.search(r"\b(show|on|tcp-\w+)\b", code) else []
        src = open(lib("strings")).read() + code
        ran += (
            subprocess.run(
                ["./slap", *check],
                input=src,
                capture_output=True,
                text=True,
                timeout=TIMEOUT,
            ).returncode
            == 0
        )
    score["Every readme slap sample runs."] = ran / len(samples)
    paths = set(re.findall(r"\b(?:examples|tests|fonts|assets)/[\w./-]*\w\.\w+", docs))
    score["Every file the docs name exists."] = sum(
        os.path.exists(p) for p in paths
    ) / max(len(paths), 1)
    score["The docs cite no slap.c line numbers."] = (
        0.0 if re.search(r"slap\.c:\d", docs) else 1.0
    )
    r = subprocess.run(
        [
            "cc",
            "-std=c99",
            "-Wall",
            "-Wextra",
            "-O3",
            "-D_POSIX_C_SOURCE=200809L",
            "-o",
            os.path.join(SCRATCH, "slap"),
            "slap.c",
            "-lm",
        ],
        capture_output=True,
        text=True,
    )
    score["slap.c compiles with no warnings."] = (
        1.0 if r.returncode == 0 and "warning" not in r.stderr else 0.0
    )
    sdl = (
        HAS_SDL
        and subprocess.run(["make", "-s", "slap-sdl"], capture_output=True).returncode
        == 0
    )
    score["slap-sdl builds and draws headless."] = (
        1.0
        if sdl
        and subprocess.run(
            ["bash", "-c", FILL_RECT], capture_output=True, timeout=TIMEOUT
        ).returncode
        == 0
        else 0.0
    )
    feed = os.path.join(SCRATCH, "feed.xml")
    with open(feed, "w") as f:
        f.write(harness.big_feed(4100))
    assert os.path.getsize(feed) >= 600_000, os.path.getsize(feed)
    src = (
        "".join(open(lib(n)).read() for n in ("strings", "parse", "xml", "rss"))
        + open("examples/feed.slap").read()
    )
    t = time.time()
    r = subprocess.run(
        ["./slap", feed], input=src, capture_output=True, text=True, timeout=60
    )
    score["A 600 KB feed renders in under a second."] = (
        1 / (time.time() - t) if r.stdout.rstrip().endswith("4100 items") else 0.0
    )
    rng = random.Random(1)
    vocab = [
        "apple",
        "Apple",
        "banana",
        "cherry",
        "ö",
        "日本",
        "zeta",
        "alpha",
        "beta",
        "gamma",
        "delta",
    ]
    text = "".join(
        " ".join(rng.choice(vocab) for _ in range(rng.randrange(1, 6))) + "\n"
        for _ in range(20000)
    ).encode()
    lines = os.path.join(SCRATCH, "lines.txt")
    with open(lines, "wb") as f:
        f.write(text)
    src = (
        open(lib("strings"), "rb").read()
        + open("examples/utils/sort.slap", "rb").read()
    )
    t = time.time()
    r = subprocess.run(["./slap", lines], input=src, capture_output=True, timeout=60)
    ok = r.returncode == 0 and r.stdout == b"".join(
        sorted(text.splitlines(keepends=True))
    )
    score["sort.slap sorts 20,000 lines in under a second."] = (
        1 / (time.time() - t) if ok else 0.0
    )
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
