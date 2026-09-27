"""Shared helpers for the tests/run_*.py integration test runners."""

import socket, subprocess, sys, time


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


def boot(prefix, argv, src, port):
    """Popen ./slap with argv, feed it src on stdin, wait until it accepts
    connections on port. Dies if it exits early or never starts listening."""
    p = subprocess.Popen(
        ["./slap", *argv],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
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
