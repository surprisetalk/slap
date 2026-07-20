#!/usr/bin/env python3
"""Integration test for examples/kv-server.slap + kv-client.slap: start the
server, drive it through the Slap client, verify persistence survives a
restart, and confirm hostile input fails safely rather than crashing the loop."""

import os, random, socket, subprocess, sys, tempfile, time

LIBS = ["examples/lib/strings.slap", "examples/lib/parse.slap"]
SERVER = "examples/kv-server.slap"
CLIENT = "examples/kv-client.slap"


def die(msg):
    print(f"kv: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    if not os.access("./slap", os.X_OK):
        die("no ./slap binary; run 'make slap' first")
    for f in LIBS + [SERVER, CLIENT]:
        if not os.path.exists(f):
            die(f"cannot find {f}; run from the repo root")

    server_src = "".join(open(f).read() for f in LIBS + [SERVER])
    client_src = "".join(open(f).read() for f in LIBS + [CLIENT])

    for label, src in (("server", server_src), ("client", client_src)):
        r = subprocess.run(
            ["./slap", "--check"], input=src, capture_output=True, text=True, timeout=30
        )
        if r.returncode != 0:
            die(f"{label} --check failed:\n{r.stderr}")

    port = random.randint(20000, 40000)
    passed = 0

    with tempfile.TemporaryDirectory() as d:
        snap = os.path.join(d, "kv.snap")

        def boot():
            p = subprocess.Popen(
                ["./slap", str(port), snap],
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
            p.stdin.write(server_src)
            p.stdin.close()
            for _ in range(50):
                if p.poll() is not None:
                    die(f"server exited early:\n{p.stderr.read()}")
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                    return p
                except OSError:
                    time.sleep(0.1)
            die("server never started listening")

        def kill(p):
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()

        def client(*words):
            r = subprocess.run(
                ["./slap", str(port), *words],
                input=client_src,
                capture_output=True,
                text=True,
                timeout=10,
            )
            if r.returncode != 0:
                die(f"client {words} crashed:\n{r.stderr}")
            return r.stdout

        def raw(payload, half_close=False):
            # exercise paths the well-behaved client can't produce (no newline,
            # oversize, embedded control bytes, mid-line disconnect). The server
            # may close mid-write -> tolerate RST.
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            out = b""
            try:
                s.sendall(payload)
                if half_close:
                    s.shutdown(socket.SHUT_WR)
                while True:
                    c = s.recv(4096)
                    if not c:
                        break
                    out += c
            except (ConnectionResetError, BrokenPipeError):
                pass
            finally:
                s.close()
            return out.decode(errors="replace")

        proc = boot()

        def check(name, cond, detail=""):
            nonlocal passed
            if not cond:
                err = proc.stderr.read() if proc.poll() is not None else ""
                tail = f"\n  server stderr:\n{err}" if err.strip() else ""
                kill(proc)
                die(f"{name} FAILED {detail}{tail}")
            passed += 1

        try:
            check("ping", client("ping") == "PONG\n")
            check("set-ok", client("set", "greeting", "hello", "world") == "OK\n")
            check(
                "get-roundtrip",
                client("get", "greeting") == "VALUE hello world\n",
                repr(client("get", "greeting")),
            )
            check("get-missing", client("get", "nope") == "NIL\n")
            check("overwrite", client("set", "greeting", "hi") == "OK\n")
            check("overwrite-read", client("get", "greeting") == "VALUE hi\n")
            client("set", "a", "1")
            client("set", "b", "2")
            # a value with interior spaces must survive verbatim (the reason the
            # snapshot uses TAB, not space, as its separator)
            check(
                "set-spaced",
                client("set", "phrase", "hello", "there", "world") == "OK\n",
            )
            check("get-spaced", client("get", "phrase") == "VALUE hello there world\n")
            keys = client("keys")
            check(
                "keys-lists-all",
                keys.startswith("KEYS ")
                and set(keys[5:].split()) == {"greeting", "a", "b", "phrase"},
                repr(keys),
            )
            check("del-ok", client("del", "a") == "OK\n")
            check("del-gone", client("get", "a") == "NIL\n")
            check("del-absent-ok", client("del", "a") == "OK\n")

            check("err-unknown", client("frobnicate").startswith("ERR unknown command"))
            check("err-empty", raw(b"\n") == "ERR empty command; try PING\n")
            check("bad-key-spaces", client("get", "a", "b").startswith("ERR bad key"))
            check(
                "bad-key-empty",
                raw(b"SET  value\n").startswith("ERR bad key"),
                "SET with empty key",
            )
            check(
                "value-tab-rejected",
                raw(b"SET k va\tlue\n").startswith(
                    "ERR value must not contain control"
                ),
            )
            check(
                "value-cr-rejected",
                raw(b"SET k a\rb\n").startswith("ERR value must not contain control"),
            )
            check(
                "oversize-line-too-long",
                "ERR line too long" in raw(b"GET greeting" + b"x" * 5000),
            )
            # a short line with no newline is an early disconnect, NOT an oversize
            # line -- the server must not misreport it as "line too long"
            check(
                "early-close-not-misreported",
                "line too long" not in raw(b"GET greeting", half_close=True),
            )

            # SAVE's own on-disk write, verified BEFORE the SHUTDOWN path (which
            # saves independently) so a broken SAVE can't hide behind SHUTDOWN
            check("save-ok", client("save") == "OK\n")
            check("still-alive-after-save", client("ping") == "PONG\n")
            after_save = open(snap).read()
            check(
                "save-wrote-disk",
                "greeting\thi" in after_save
                and "phrase\thello there world" in after_save,
                repr(after_save),
            )
            # a DEL then SAVE must not resurrect the key on reload (truncating write)
            check(
                "save-del-save",
                client("del", "b") == "OK\n" and client("save") == "OK\n",
            )

            # persistence across a clean restart driven by SHUTDOWN
            check("shutdown", client("shutdown") == "BYE\n")
            proc.wait(5)
            check("shutdown-exit-0", proc.returncode == 0, f"(code {proc.returncode})")

            proc = boot()
            check("reload-survives", client("get", "greeting") == "VALUE hi\n")
            check(
                "reload-spaced-survives",
                client("get", "phrase") == "VALUE hello there world\n",
            )
            check("reload-del-a-persisted", client("get", "a") == "NIL\n")
            check("reload-del-b-persisted", client("get", "b") == "NIL\n")
            reload_keys = client("keys")
            check(
                "reload-keyset-exact",
                set(reload_keys[5:].split()) == {"greeting", "phrase"},
                repr(reload_keys),
            )

            # a runtime SAVE against an unwritable snapshot must report the error,
            # not crash the whole server (skip when running as root: 000 wouldn't bite)
            if os.geteuid() != 0:
                os.chmod(snap, 0)
                check(
                    "unwritable-save-reports",
                    client("save").startswith("ERR save failed"),
                )
                check("survives-failed-save", client("ping") == "PONG\n")
                os.chmod(snap, 0o644)
            kill(proc)

            # a corrupt snapshot (a line with no TAB, e.g. a torn prior write) must
            # be refused loudly and left ON DISK, never silently pruned + re-saved
            with open(snap, "w") as f:
                f.write("good\tvalue here\nbadline-with-no-tab\n")
            r = subprocess.run(
                ["./slap", str(port), snap],
                input=server_src,
                capture_output=True,
                text=True,
                timeout=10,
            )
            check("corrupt-refused-exit", r.returncode != 0, f"(code {r.returncode})")
            check(
                "corrupt-refused-message",
                "refusing to load corrupt snapshot" in r.stderr,
                repr(r.stderr[:200]),
            )
            check(
                "corrupt-snapshot-untouched",
                open(snap).read() == "good\tvalue here\nbadline-with-no-tab\n",
            )
        finally:
            kill(proc)

    print(f"kv: {passed} checks passed")


if __name__ == "__main__":
    main()
