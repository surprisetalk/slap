"""Shared helpers for the tests/run_*.py integration test runners."""

import resource, socket, struct, subprocess, sys, time


def die(prefix, msg):
    print(f"{prefix}: {msg}", file=sys.stderr)
    sys.exit(1)


def make_check(prefix, tail=lambda: ""):
    count = [0]

    def check(name, cond, detail=""):
        if not cond:
            die(prefix, f"{name} FAILED {detail}{tail()}")
        count[0] += 1

    return check, count


def boot(prefix, argv, src, port, preexec_fn=None):
    """Popen ./slap with argv, feed it src on stdin, wait until it accepts
    connections on port. Dies if it exits early or never starts listening."""
    p = subprocess.Popen(
        ["./slap", *argv],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        preexec_fn=preexec_fn,
    )
    p.stdin.write(src)
    p.stdin.close()
    for _ in range(50):
        if p.poll() is not None:
            die(prefix, f"server exited early:\n{p.stderr.read()}")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return p
        except OSError:
            time.sleep(0.1)
    die(prefix, "server never started listening")


def kill(p):
    p.terminate()
    try:
        p.wait(5)
    except subprocess.TimeoutExpired:
        p.kill()


def raw(port, payload, timeout=5, half_close=False, decode=True):
    # the server may close mid-conversation (oversize requests, panics), which
    # the kernel surfaces as RST; report whatever arrived before the reset
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    out = b""
    try:
        s.sendall(payload)
        if half_close:
            s.shutdown(socket.SHUT_WR)
        while True:
            c = s.recv(65536)
            if not c:
                break
            out += c
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        s.close()
    return out.decode(errors="replace") if decode else out


def rst(s):
    """Close s with SO_LINGER 0, so the peer gets a reset."""
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()


def reset(port):
    """Send part of a line, wait until an idle server is blocked reading it,
    then reset: its recv fails with "Connection reset by peer"."""
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(b"GET /")
    time.sleep(0.3)
    rst(s)


def reset_burst(port):
    """120 clients that reset before the server accepts them: one client holds
    the server in recv while the others queue (listen's backlog is 128)."""
    hold = socket.create_connection(("127.0.0.1", port), timeout=5)
    hold.sendall(b"GET /")
    time.sleep(0.3)
    for _ in range(120):
        rst(socket.create_connection(("127.0.0.1", port), timeout=5))
    rst(hold)


def accept_failures(check, name, argv, src, port):
    """Run ./slap argv on src with no free file descriptor past the listener,
    so accept fails: on macOS once per connection, on Linux without end.
    Checks that the server stops after 100 failures in a row."""
    p = subprocess.Popen(
        ["./slap", *argv],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (4, 4)),
    )
    p.stdin.write(src)
    p.stdin.close()
    for _ in range(500):  # boot: refused until the server listens
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.01)
    for _ in range(300):
        if p.poll() is not None:
            break
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
        except OSError:
            break
    try:
        p.wait(5)
    except subprocess.TimeoutExpired:
        kill(p)
    err = p.stderr.read()
    check(
        "accept-failures-bounded",
        p.returncode not in (0, -15, -9)
        and f"{name}: accept failed 100 times in a row; the last: Too many open files" in err,
        f"(code {p.returncode}) {err[-300:]!r}",
    )


def server_check(prefix, get_proc):
    """make_check wired to a boot()ed server: on failure, read its stderr if
    it already died, then kill it before dying."""

    def tail():
        p = get_proc()
        err = p.stderr.read() if p.poll() is not None else ""
        t = f"\n  server stderr:\n{err}" if err.strip() else ""
        kill(p)
        return t

    return make_check(prefix, tail)


def big_feed(n_items):
    """An RSS document with n_items entries."""
    items = "".join(
        f"    <item><title>Post {i}</title><link>http://e.com/{i}</link>"
        f"<description>Body {i}. {'pad ' * 10}</description></item>\n"
        for i in range(n_items)
    )
    return (
        '<?xml version="1.0"?>\n<rss version="2.0"><channel>'
        "<title>Big</title><link>http://e.com</link><description>d</description>\n"
        f"{items}</channel></rss>\n"
    )
