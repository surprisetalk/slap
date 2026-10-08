#!/usr/bin/env python3
"""Integration test for examples/kv-server.slap + kv-client.slap: start the
server, drive it through the Slap client, verify persistence survives a
restart, and confirm hostile input fails safely rather than crashing the loop."""

import os, random, re, resource, shutil, socket, struct, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(__file__))
import harness

LIBS = ["examples/lib/strings.slap", "examples/lib/parse.slap"]
SERVER = "examples/kv-server.slap"
CLIENT = "examples/kv-client.slap"


def main():
    server_src = "".join(open(f).read() for f in LIBS + [SERVER])
    client_src = "".join(open(f).read() for f in LIBS + [CLIENT])

    port = random.randint(20000, 40000)

    with tempfile.TemporaryDirectory() as d:
        snap = os.path.join(d, "kv.snap")

        def client(*words):
            r = subprocess.run(
                ["./slap", str(port), *words],
                input=client_src,
                capture_output=True,
                text=True,
                timeout=10,
            )
            if r.returncode != 0:
                harness.die("kv", f"client {words} crashed:\n{r.stderr}")
            return r.stdout

        def raw(payload, half_close=False):
            # exercise paths the well-behaved client can't produce (no newline,
            # oversize, embedded control bytes, mid-line disconnect). The server
            # may close mid-write -> tolerate RST.
            return harness.raw(port, payload, half_close=half_close)

        proc = harness.boot("kv", [str(port), snap], server_src, port)
        check, count = harness.server_check("kv", lambda: proc)

        try:
            # The OS reports the bound address, so the check needs no network.
            if not shutil.which("lsof"):
                sys.exit("kv: lsof is not on PATH; the loopback-only check needs it to read the listener's address")
            listen = subprocess.run(
                ["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN"], capture_output=True, text=True, timeout=10
            ).stdout
            check("loopback-only", f"127.0.0.1:{port}" in listen, f"(expected a listener on 127.0.0.1:{port}, lsof shows:\n{listen})")
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
            # a client that resets mid-line is a recv error: logged with its
            # reason (checked after shutdown), and the server keeps serving
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            s.sendall(b"GET gree")
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            s.close()
            check("reset-survives", client("ping") == "PONG\n")

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
            log = proc.stderr.read()
            # the reset reaches accept or recv, depending on when it arrives
            check("reset-logged", re.search(r"kv-server: (accept|recv) failed: \w", log), repr(log[-300:]))

            proc = harness.boot("kv", [str(port), snap], server_src, port)
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
                saved = client("save")
                check(
                    "unwritable-save-reports",
                    saved == f"ERR save failed: {snap}: Permission denied\n",
                    repr(saved),
                )
                check("survives-failed-save", client("ping") == "PONG\n")
                bye = client("shutdown")
                check(
                    "unwritable-shutdown-reports",
                    bye == f"ERR save failed: {snap}: Permission denied; still running\n",
                    repr(bye),
                )
                check("survives-failed-shutdown", client("ping") == "PONG\n")
                os.chmod(snap, 0o644)
            harness.kill(proc)

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

            def boot_refused(label, want, text):
                try:
                    r = subprocess.run(
                        ["./slap", str(port), snap], input=server_src, capture_output=True, text=True, timeout=5
                    )
                except subprocess.TimeoutExpired as e:
                    harness.die("kv", f"{label}: the server started on a snapshot it must refuse:\n{e.stderr}")
                check(f"{label}-refused", r.returncode != 0 and want in r.stderr, repr(r.stderr[:300]))
                if text is not None:
                    check(f"{label}-untouched", open(snap, newline="").read() == text)

            # a torn last line (no LF) is refused like a line with no TAB
            with open(snap, "w") as f:
                f.write("good\tvalue here\ntorn\tval")
            boot_refused("torn", "refusing to load corrupt snapshot", "good\tvalue here\ntorn\tval")
            # every line must be a valid key, a TAB and a value without control bytes, once per key
            for label, text, want in [
                ("blank-line", "a\tb\n\nc\td\n", "line 2: the line is empty"),
                ("only-newline", "\n", "line 1: the line is empty"),
                ("empty-key", "\tv\n", "line 1: bad key"),
                ("key-with-space", "a b\tv\n", "line 1: bad key"),
                ("cr-in-value", "a\tb\r\n", "line 1: the value holds a control byte"),
                ("repeated-key", "a\tb\na\tc\n", "line 2: key a appears twice"),
            ]:
                with open(snap, "w") as f:
                    f.write(text)
                boot_refused(label, f"refusing to load corrupt snapshot {snap} -- {want}", text)
            # a snapshot it cannot read stops the boot by path and reason
            if os.geteuid() != 0:
                for label, mode in [("mode-000", 0), ("write-only", 0o200)]:
                    with open(snap, "w") as f:
                        f.write("k\tv\n")
                    os.chmod(snap, mode)
                    boot_refused(label, f"{snap}: Permission denied", None)
                    os.chmod(snap, 0o644)
                    check(f"{label}-untouched", open(snap).read() == "k\tv\n")
            os.remove(snap)
            os.mkdir(snap)
            boot_refused("dir", f"{snap}: Is a directory", None)
            os.rmdir(snap)

            # A large snapshot loads in linear time.
            big = os.path.join(d, "big.snap")
            for label, lines in [
                ("many-lines", [f"k{i}\tv{i}\n" for i in range(50000)]),
                ("big-values", [f"k{i}\t{'v' * 4000}\n" for i in range(300)]),
            ]:
                with open(big, "w") as f:
                    f.write("".join(lines))
                t0 = time.monotonic()
                proc = harness.boot("kv", [str(port), big], server_src, port)
                took = time.monotonic() - t0
                check(f"{label}-boots-fast", took < 3, f"{took:.1f} s")
                key, value = lines[-1].rstrip("\n").split("\t")
                check(f"{label}-loaded", raw(f"GET {key}\n".encode()) == f"VALUE {value}\n")
                harness.kill(proc)

            # With no free file descriptor, accept fails: on macOS once per
            # connection, on Linux without end. The server stops after 100
            # failures in a row.
            p = subprocess.Popen(
                ["./slap", str(port), snap],
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (4, 4)),
            )
            p.stdin.write(server_src)
            p.stdin.close()
            for _ in range(300):
                if p.poll() is not None:
                    break
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=1).close()
                except OSError:
                    time.sleep(0.01)
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                harness.kill(p)
            err = p.stderr.read()
            check(
                "accept-failures-bounded",
                p.returncode not in (0, None, -15, -9)
                and "accept failed 100 times in a row; the last: Too many open files" in err,
                f"(code {p.returncode}) {err[-300:]!r}",
            )
        finally:
            harness.kill(proc)

    print(f"kv: {count[0]} checks passed")


if __name__ == "__main__":
    main()
