#!/usr/bin/env python3
"""Run every check in parallel. `suite.py` is `make test`, `suite.py slow` is
`make test-slow`, and `suite.py status` is `make status`."""

import concurrent.futures, glob, json, os, random, re, resource, shlex, shutil, socket, subprocess, sys, tempfile, threading, time

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
# A headless run sends 'resize 640 480 once, then runs the tick handlers and the render body
# each frame until a handler fails.
HEADLESS = (
    "test -x ./slap-sdl || { echo 'no ./slap-sdl: run make slap-sdl' >&2; exit 1; };"
    " e=$(mktemp); trap 'rm -f $e' EXIT;"
    ' out=$(echo "0 (swap print print) \'resize on (drop 1 plus dup 3 eq (\\"stop\\" fail) () if) \'tick on (print) show" | ./slap-sdl --headless 2>$e);'
    ' test $? -eq 1 && test "$(tr "\\n" " " <<<"$out")" = "640 480 1 2 " && grep -q stop $e'
)


# A headless run never ends on its own, so a demo check stops it at frame 15. The check binds
# the demo's resize, key and mouse handlers as words (test-resize, test-keydown, ...), runs the
# probe each frame, and
# calls test-resize with 0 0 at frame 5 and 1920 1080 at frame 10, so a demo that divides by
# its size dies. The canvas stays 640x480, and pixel and fill-rect clip.
def drive_demo(files, probe=""):
    src = "".join(open(f).read() for f in files)
    end = re.search(r"\bshow\s*\Z", src)
    head = src[: end.start()] if end else src
    n = head.count("'resize on")
    if n != 1 or not end:
        return (
            None,
            f"{files[-1]} must register one 'resize handler (found {n}) and end with show",
        )
    for ev in ["resize", "keydown", "keyup", "mousedown", "mouseup", "mousemove"]:
        if head.count(f"'{ev} on") == 1:
            head = head.replace(f"'{ev} on", f"'test-{ev} let (test-{ev}) '{ev} on")
    driver = (
        f"('test-frame let {probe}"
        " test-frame 5 eq (0 0 test-resize) () if"
        " test-frame 10 eq (1920 1080 test-resize) () if"
        ' test-frame 15 eq ("test-done" fail) () if) \'tick on\n'
    )
    try:
        r = subprocess.run(
            ["./slap-sdl", "--headless"],
            input=head + driver + "show\n",
            capture_output=True,
            text=True,
            timeout=TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        return None, f"timed out after {TIMEOUT} s"
    if r.returncode != 1 or "test-done" not in r.stderr:
        return None, r.stdout[-2000:] + r.stderr[-4000:]
    return r.stdout, None


def headless_demo(files, ok, probe=""):
    def check():
        out, err = drive_demo(files, probe)
        return (False, err) if err else (ok is None or ok(out), out[-2000:])

    check.__name__ = f"headless_demo({' '.join(files)})"
    return check


# raycast: each key sets only its own flag (state slots 3 to 8: forward, back, left, right,
# turn left, turn right), the left arrow turns (slot 2), and two runs start facing different
# directions.
RAYCAST_KEYS = {
    119: 3,
    1073741906: 3,
    115: 4,
    1073741905: 4,
    97: 5,
    100: 6,
    1073741904: 7,
    1073741903: 8,
}
RAYCAST_FLAGS = " ".join(f"{i} peek must print" for i in range(3, 9))
RAYCAST_PROBE = (
    "test-frame 0 eq (dup 'st at 2 peek must print drop) () if"
    + "".join(
        f" test-frame {f} eq ({k} test-keydown dup 'st at {RAYCAST_FLAGS} drop {k} test-keyup) () if"
        for f, k in enumerate(RAYCAST_KEYS, 1)
    )
    + " test-frame 9 eq (1073741904 test-keydown) () if"
    + " test-frame 10 eq (dup 'st at 2 peek must print drop 1073741904 test-keyup) () if"
)


def raycast_keys():
    want = [
        ["1.0" if i == slot else "0.0" for i in range(3, 9)]
        for slot in RAYCAST_KEYS.values()
    ]
    starts = []
    for _ in range(2):
        out, err = drive_demo(
            ["examples/raycast.slap", "examples/raycast-sdl.slap"], RAYCAST_PROBE
        )
        if err:
            return False, err
        v = out.split()[-50:]
        flags = [v[1 + 6 * i : 7 + 6 * i] for i in range(8)]
        if "raycast-selftest-ok" not in out or flags != want or v[0] == v[-1]:
            return (
                False,
                "want the start angle, one flag per key, then a turned angle:\n" + out,
            )
        starts.append(v[0])
    return starts[0] != starts[1], f"both runs start at angle {starts[0]}"


# life draws the grid on the frame where C (slot 7) equals F (slot 6). A mouse edit and its
# mouseup draw on the next frame, no generation runs while the button is down, a resize draws
# on the next frame, a generation runs on every third tick, and a 1920x1080 canvas holds
# 213x120 cells (slots 0 and 1).
LIFE_PROBE = (
    "test-frame 1 eq (10 10 test-mousedown 7 peek must print 6 peek must print) () if"
    " test-frame 2 eq (7 peek must print 6 peek must print) () if"
    " test-frame 3 eq (7 peek must print 10 10 test-mouseup) () if"
    " test-frame 4 eq (7 peek must print 6 peek must print) () if"
    " test-frame 11 eq (7 peek must print 6 peek must print 0 peek must print 1 peek must print) () if"
    " test-frame 12 eq (7 peek must print) () if"
    " test-frame 13 eq (7 peek must print) () if"
)
LIFE_OUT = "2 1 2 2 2 4 4 11 11 213 120 12 12".split()


# Drawing before show has no canvas to draw on, so it dies.
NO_CANVAS = (
    "test -x ./slap-sdl || { echo 'no ./slap-sdl: run make slap-sdl' >&2; exit 1; };"
    " for c in '0 0 1 pixel:pixel: no canvas yet' '1 clear:clear: no canvas yet' '0 0 1 1 1 fill-rect:fill-rect: no canvas yet'; do"
    ' out=$(echo "${c%%:*} 0 (drop) \'tick on (drop) show" | ./slap-sdl --headless 2>&1);'
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


# write replaces a regular file whole: through symlinks (relative, absolute, in a subdirectory,
# a chain), keeping the mode, giving a new file 0666 minus the umask, and under a file size limit,
# where the old bytes survive and no temp file stays. Where a replacement cannot match the old file
# (a hard link, a directory the user may not write) it writes in place. A link loop is refused.
def write_replaces():
    # realpath: /var is a symlink on macOS, and resolving it would lengthen every path below
    d = os.path.realpath(tempfile.mkdtemp(prefix="slap-write-"))
    try:
        p = lambda *n: os.path.join(d, *n)
        for name, text in [("t.txt", "old"), ("t2.txt", "old2"), ("h1", "old"), ("big.txt", "k" * 2000), ("deep.txt", "old")]:
            with open(p(name), "w") as f:
                f.write(text)
        os.chmod(p("t.txt"), 0o600)
        os.link(p("h1"), p("h2"))
        os.symlink("t.txt", p("link"))
        os.symlink("link", p("chain"))
        os.symlink(p("t2.txt"), p("abs"))
        os.mkdir(p("sub"))
        os.symlink("../t2.txt", p("sub", "rel"))
        os.symlink("made.txt", p("dangling"))
        os.symlink("loop", p("loop"))
        long = p("a" * 250)
        with open(long, "w") as f:
            f.write("old")
        os.mkdir(p("ro"))
        with open(p("ro", "f"), "w") as f:
            f.write("old")
        os.chmod(p("ro"), 0o555)
        q = lambda path: '"' + path + '"'
        prog = " ".join(
            f'{q(path)} "{text}" write must drop'
            for path, text in [
                (p("chain"), "new"), (p("abs"), "abs"), (p("sub", "rel"), "rel"), (p("dangling"), "made"),
                ("/dev/null", "x"), (p("h1"), "hard"), (long, "long"), (p("ro", "f"), "ro"),
                # 1021-1022 bytes: the temp name, 3 bytes longer, passes macOS's PATH_MAX of 1024
                (d + "/" + "./" * ((1013 - len(d)) // 2) + "deep.txt", "deep"),
            ]
        ) + f' {q(p("loop"))} "x" write {{\'ok (drop "wrote" print) \'no (print)}} case'
        r = subprocess.run(
            ["./slap"], input=prog, capture_output=True, text=True, timeout=10, preexec_fn=lambda: os.umask(0o022)
        )
        os.chmod(p("ro"), 0o755)
        if r.returncode:
            return False, r.stderr[-1000:]
        fails = []
        if r.stdout != f'"{p("loop")}: Too many levels of symbolic links"\n':
            fails.append(f"a symlink loop must be refused, got {r.stdout!r}")
        for path, want in [("t.txt", "new"), ("t2.txt", "rel"), ("made.txt", "made"), ("h1", "hard"), ("h2", "hard"), ("deep.txt", "deep"), ("a" * 250, "long"), (os.path.join("ro", "f"), "ro")]:
            if open(p(path)).read() != want:
                fails.append(f"{path} holds {open(p(path)).read()!r}, expected {want!r}")
        if not all(os.path.islink(p(n)) for n in ("link", "chain", "abs", "dangling", os.path.join("sub", "rel"))):
            fails.append("a write through a symlink must keep the link")
        for path, mode in [("t.txt", 0o600), ("made.txt", 0o644)]:
            if os.stat(p(path)).st_mode & 0o777 != mode:
                fails.append(f"{path} has mode {oct(os.stat(p(path)).st_mode & 0o777)}, expected {oct(mode)}")
        r = subprocess.run(
            ["./slap"],
            input=q(p("big.txt")) + """ 0 3000 range (drop 107) each write {'ok (drop "wrote" print) 'no (print)} case""",
            capture_output=True,
            text=True,
            timeout=10,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024)),
        )
        if r.stdout != f'"{p("big.txt")}: File too large"\n':
            fails.append(f"a write past the size limit must give File too large, got {r.stdout!r} {r.stderr[-300:]!r} (exit {r.returncode})")
        if open(p("big.txt")).read() != "k" * 2000:
            fails.append(f"a failed write must keep the old bytes; big.txt holds {os.path.getsize(p('big.txt'))} bytes")
        if os.geteuid() != 0:
            os.chmod(p("t2.txt"), 0o444)
            r = subprocess.run(["./slap"], input=q(p("t2.txt")) + """ "x" write {'ok (drop "wrote" print) 'no (print)} case""", capture_output=True, text=True, timeout=10)
            os.chmod(p("t2.txt"), 0o644)
            if r.stdout != f'"{p("t2.txt")}: Permission denied"\n' or open(p("t2.txt")).read() != "rel":
                fails.append(f"a read-only file must be refused and kept, got {r.stdout!r}")
        left = [n for n in os.listdir(d) + os.listdir(p("ro")) + os.listdir(p("sub")) if n.startswith(".slap")]
        if left:
            fails.append(f"temp files left behind: {left}")
        return not fails, "\n".join(fails)
    finally:
        os.chmod(os.path.join(d, "ro"), 0o755)
        shutil.rmtree(d)


# With a TTY on stdin, slap is a shell: it prints the stack after each line, and a
# line that fails to check or to run is discarded, its bindings and stack changes
# with it. A box may wait on the stack between lines.
def shell():
    import pty

    # (line, the stack after it, or None for a box on top)
    steps = [
        ("1 2 plus", "3"),
        ("dup", "3 3"),
        ("plus 'x let", "(empty)"),
        ("x 1 plus", "7"),
        ('"a" plus', "7"),
        ("0 div", "7"),
        ("5 'y let 1 0 div", "7"),
        ("y", "7"),
        ("(1 plus) 'inc2 let inc2", "8"),
        ("inc2 (0 div) 'bad let bad", "8"),
        ("bad", "8"),
        ("[1 2 3] (0 div) each", "8"),
        ("drop 5 box", None),
        ("1 0 div", None),
        ("free 0 tcp-listen must", "SOCKET"),
        ("tcp-close 1 0 div", "SOCKET"),
        ("tcp-close", "(empty)"),
        ("1 2", "1 2"),
    ]
    master, slave = pty.openpty()
    p = subprocess.Popen(["./slap"], stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    os.close(slave)
    # the last line ends without a newline: ^D sends it, and a second ^D ends the input
    os.write(master, ("\n".join(line for line, _ in steps)).encode() + b"\x04\x04")
    try:
        out, err = p.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        return False, "no answer within 10 s"
    finally:
        os.close(master)
    out, err = out.decode(), err.decode()
    shown = out.split("> ")
    got = [x.rstrip("\n") for x in shown[1:-1]]
    fails = []
    if p.returncode != 0 or shown[0] != "" or shown[-1] != "\n":
        fails.append(f"exit {p.returncode}, expected 0 (the last line worked) with a prompt per line and a newline at the end")
    sock = got[14] if len(got) > 14 else ""
    for i, (line, w) in enumerate(steps):
        g = got[i] if i < len(got) else "(nothing)"
        want = "<box>" if w is None else sock if w == "SOCKET" else w
        if g != want or (w == "SOCKET" and not g.isdigit()):
            fails.append(f"after {line!r} the stack is {g!r}, expected {want!r}")
    # each error names the line as typed, counting discarded lines
    for msg in ["<stdin>:5 ", "'plus' takes int int", "<stdin>:6:3", "division by zero", "unknown word 'y'", "<stdin>:10:", "<stdin>:16:"]:
        if msg not in err:
            fails.append(f"stderr lacks {msg!r}")
    if "Bad file descriptor" in err:
        fails.append("a socket closed by a discarded line must stay open")
    return not fails, "\n".join(fails) + "\nstdout: " + out[-600:] + "\nstderr: " + err[-1500:]


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
            input=f'"x" 19 (dup cat) repeat \'b let "127.0.0.1" {port} tcp-connect must '
            + send * 40
            + "tcp-close",
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
    return (
        r.returncode != 0
        and '"timed out after 30 s"' in r.stdout
        and took is not None
        and 29000 <= took <= 31000,
        detail,
    )


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
            "write replaces a file whole": (write_replaces, None),
            "shell": (shell, None),
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
                    ("strings", "parse", "json", "xml", "rss", "cbor"),
                    [
                        (
                            """[] {} "a" 'name into 1 je-int 'value into push {} "a" 'name into 2 je-int 'value into push je-obj print""",
                            'json: je-obj: duplicate key "a"',
                        ),
                        (
                            """[] {} "a" 'name into 1 ce-int 'value into push {} "a" 'name into 2 ce-int 'value into push ce-map print""",
                            'cbor: ce-map: duplicate key "a"',
                        ),
                        (
                            """2 ce-bool print""",
                            "cbor: ce-bool: expected 0 or 1, got 2",
                        ),
                        (
                            """"r" [] {} "x" 'name into "1" 'value into push {} "x" 'name into "2" 'value into push [] xe-elem xml-render print""",
                            'xml: duplicate attribute "x" in <r>',
                        ),
                        (
                            """"r" [] {} "x y" 'name into "1" 'value into push [] xe-elem xml-render print""",
                            'xml: attribute name "x y" in <r> is not an XML name',
                        ),
                        (
                            """"" [] [] xe-elem xml-render print""",
                            'xml: element name "" is not an XML name',
                        ),
                        (
                            """"1a" [] [] xe-elem xml-pretty print""",
                            'xml: element name "1a" is not an XML name',
                        ),
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
            "a stack swap of a let-bound body gets no let hint": (
                'out=$(printf "[(1 plus)] first \'f let f swap\\n" | ./slap 2>&1); test $? -eq 1 && grep -q "\'swap\' takes" <<<"$out" && ! grep -q \'bound with let\' <<<"$out"',
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
        out["drawing before show dies"] = (NO_CANVAS, None)
        out["headless runs 'resize 640 480 first, then tick and render"] = (
            HEADLESS,
            None,
        )
    else:
        print(
            "suite: warning: sdl2-config is not on PATH, so slap-sdl is not built and the SDL checks"
            " only type-check the demos. make status fails until SDL2 is installed (brew install sdl2, or"
            " nix-shell -p SDL2 pkg-config).",
            file=sys.stderr,
        )
    for name in ["icn", "chr", "nmt", "tga", "gly", "ulz"]:
        out[f"lib/{name}"] = (f"./slap < examples/lib/{name}.slap", None)
    for combo in [
        ("icn", "ufx"),
        ("strings", "parse", "json"),
        ("strings", "cbor"),
        ("strings", "http"),
        ("strings", "parse", "xml", "rss"),
    ]:
        out["lib/" + "+".join(combo)] = (f"cat {lib(*combo)} | ./slap", None)
    out["scale"] = (
        f"cat {lib('strings', 'parse', 'json', 'xml', 'rss', 'http', 'cbor')} tests/scale.slap | ./slap",
        None,
    )
    # A demo fills the window, so it reads the canvas size from 'resize. A core file runs its
    # self-test headless before its window shell.
    demos = {
        n: ([f"examples/{n}.slap"], None)
        for n in [
            "ant",
            "dots",
            "fish",
            "flock",
            "fonts",
            "gradient",
            "life",
            "scratch",
            "snake",
        ]
    }
    for n in ["chip8", "maze", "raycast", "uxn", "zoom"]:
        demos[n] = (
            [f"examples/{n}.slap", f"examples/{n}-sdl.slap"],
            lambda o, n=n: f"{n}-selftest-ok" in o,
        )
    probes = {"life": (LIFE_PROBE, lambda o: o.split() == LIFE_OUT)}
    for name, (files, ok) in demos.items():
        if not HAS_SDL:
            out[name] = (
                f"cat {' '.join(files)} | ./slap --check"
                + (f" && ./slap --headless < {files[0]}" if ok else ""),
                ok,
            )
        elif name == "raycast":
            out[name] = (raycast_keys, None)
        elif name in probes:
            probe, pok = probes[name]
            out[name] = (headless_demo(files, pok, probe), None)
        else:
            out[name] = (headless_demo(files, ok), None)
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
    r = subprocess.run([sys.executable, __file__], capture_output=True, text=True)
    score["make test passes within 10 s."] = (
        10 / (time.time() - t) if r.returncode == 0 else 0.0
    )
    if r.returncode:
        print(
            f"status: make test failed:\n{r.stdout[-6000:]}{r.stderr[-2000:]}",
            file=sys.stderr,
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
