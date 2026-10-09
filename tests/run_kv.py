#!/usr/bin/env python3
"""Integration test for examples/kv-server.slap + kv-client.slap: start the
server, drive it through the Slap client, verify persistence survives a
restart, and confirm hostile input fails safely rather than crashing the loop."""

import os, random, resource, shutil, socket, subprocess, sys, tempfile, time

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

        def split(payload, cut):
            # the pause lets the server read the first segment alone. A server
            # that answers the first segment closes, and may reset the second.
            with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
                s.sendall(payload[:cut])
                time.sleep(0.05)
                try:
                    s.sendall(payload[cut:])
                except (BrokenPipeError, ConnectionResetError):
                    pass
                try:
                    return s.recv(4096).decode()
                except ConnectionResetError:
                    return "(the connection reset before any reply)"

        proc = harness.boot("kv", [str(port), snap], server_src, port)
        check, count = harness.server_check("kv", lambda: proc)

        try:
            # The OS reports the bound address, so the check needs no network.
            if not shutil.which("lsof"):
                sys.exit(
                    "kv: lsof is not on PATH; the loopback-only check needs it to read the listener's address"
                )
            listen = subprocess.run(
                ["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN"],
                capture_output=True,
                text=True,
                timeout=10,
            ).stdout
            check(
                "loopback-only",
                f"127.0.0.1:{port}" in listen,
                f"(expected a listener on 127.0.0.1:{port}, lsof shows:\n{listen})",
            )
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
            # A head (the bytes before LF, less one trailing CR) holds at most
            # 4095 bytes, whether the line arrives whole or in two segments.
            wrong = []
            for n in (4095, 4096):
                for end in (b"\n", b"\r\n"):
                    line = f"SET k{n} ".encode() + b"v" * (n - 10) + end
                    want = "OK\n" if n < 4096 else "ERR line too long"
                    for cut in [None, *range(4093, min(4098, len(line)))]:
                        reply = raw(line) if cut is None else split(line, cut)
                        if not reply.startswith(want):
                            wrong.append(
                                f"head {n}, end {end!r}, cut {cut}: {reply[:40]!r}"
                            )
            check("head-limit", not wrong, "\n  " + "\n  ".join(wrong))
            check("head-limit-refused-not-stored", client("get", "k4096") == "NIL\n")
            check(
                "head-limit-accepted-stored",
                client("get", "k4095") == f"VALUE {'v' * 4085}\n",
            )
            client("del", "k4095")
            # a short line with no newline is an early disconnect, NOT an oversize
            # line -- the server must not misreport it as "line too long"
            check(
                "early-close-not-misreported",
                "line too long" not in raw(b"GET greeting", half_close=True),
            )
            # a client that resets mid-line is a recv error: logged with its
            # reason (checked after shutdown), and the server keeps serving
            harness.reset(port)
            check("reset-survives", client("ping") == "PONG\n")
            # clients that reset before accept never stop the server
            harness.reset_burst(port)
            check("reset-burst-survives", client("ping") == "PONG\n")

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
            check(
                "reset-logged",
                "kv-server: recv failed: Connection reset by peer" in log,
                repr(log[-300:]),
            )

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
                    bye
                    == f"ERR save failed: {snap}: Permission denied; still running\n",
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
                        ["./slap", str(port), snap],
                        input=server_src,
                        capture_output=True,
                        text=True,
                        timeout=5,
                    )
                except subprocess.TimeoutExpired as e:
                    harness.die(
                        "kv",
                        f"{label}: the server started on a snapshot it must refuse:\n{e.stderr}",
                    )
                check(
                    f"{label}-refused",
                    r.returncode != 0 and want in r.stderr,
                    repr(r.stderr[:300]),
                )
                if text is not None:
                    check(f"{label}-untouched", open(snap, newline="").read() == text)

            # a SAVE past a file size limit keeps the old snapshot whole
            with open(snap, "w") as f:
                f.write("a\tb\n")
            proc = harness.boot(
                "kv",
                [str(port), snap],
                server_src,
                port,
                lambda: resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024)),
            )
            check("cap-set", raw(f"SET big {'v' * 2000}\n".encode()) == "OK\n")
            saved = raw(b"SAVE\n")
            check(
                "size-limit-save-refused",
                saved == f"ERR save failed: {snap}: File too large\n",
                repr(saved),
            )
            check("size-limit-save-untouched", open(snap).read() == "a\tb\n")
            check(
                "size-limit-no-temp",
                sorted(os.listdir(d)) == ["kv.snap"],
                repr(os.listdir(d)),
            )
            check("size-limit-survives", raw(b"PING\n") == "PONG\n")
            harness.kill(proc)

            # The store holds at most 1000000 bytes of snapshot. Large values,
            # many small pairs and 64-byte keys, 100 under the bound, SAVE and
            # answer KEYS; a SET one past it changes nothing, a SET to it and a
            # SET after a DEL do; SHUTDOWN saves the full store, and it loads.
            for label, pairs in [
                ("big-values", [(f"k{i:03d}", "v" * 3990) for i in range(250)]),
                ("small-pairs", [(f"k{i:06d}", f"v{i:07d}") for i in range(58800)]),
                ("long-keys", [(f"{i:064d}", "") for i in range(15150)]),
            ]:
                fill = 1000000 - 100 - sum(len(k) + len(v) + 2 for k, v in pairs)
                if fill:
                    pairs.append(("zfill", "f" * (fill - 7)))
                with open(snap, "w") as f:
                    f.write("".join(f"{k}\t{v}\n" for k, v in pairs))
                t0 = time.monotonic()
                proc = harness.boot("kv", [str(port), snap], server_src, port)
                check(f"{label}-save", raw(b"SAVE\n") == "OK\n")
                keys = raw(b"KEYS\n")
                check(
                    f"{label}-keys",
                    keys.startswith("KEYS ") and len(keys.split()) == len(pairs) + 1,
                    repr(keys[:80]),
                )
                k, v = pairs[-1]
                full = raw(f"SET zz {'w' * 97}\n".encode())
                check(
                    f"{label}-full",
                    full
                    == "ERR store full: the store would hold 1000001 of 1000000 bytes\n",
                    repr(full[:120]),
                )
                check(f"{label}-full-changes-nothing", raw(b"GET zz\n") == "NIL\n")
                check(
                    f"{label}-full-replace",
                    raw(f"SET {k} {v}{'w' * 101}\n".encode()).startswith(
                        "ERR store full"
                    ),
                )
                check(
                    f"{label}-full-replace-keeps",
                    raw(f"GET {k}\n".encode()) == f"VALUE {v}\n",
                )
                check(
                    f"{label}-to-the-bound",
                    raw(f"SET zy {'w' * 96}\n".encode()) == "OK\n",
                )
                check(
                    f"{label}-del",
                    raw(b"DEL zy\n") == "OK\n"
                    and raw(f"SET zy {'w' * 96}\n".encode()) == "OK\n",
                )
                check(f"{label}-shutdown", raw(b"SHUTDOWN\n") == "BYE\n")
                proc.wait(10)
                proc = harness.boot("kv", [str(port), snap], server_src, port)
                check(f"{label}-reloaded", raw(b"GET zy\n") == f"VALUE {'w' * 96}\n")
                harness.kill(proc)
                check(
                    f"{label}-fast",
                    time.monotonic() - t0 < 5,
                    f"{time.monotonic() - t0:.1f} s",
                )
            # a snapshot file past the bound, or with a line longer than any SET writes, is refused, untouched
            text = "".join(f"k{i:03d}\t{'v' * 3990}\n" for i in range(251))
            with open(snap, "w") as f:
                f.write(text)
            boot_refused(
                "past-bound",
                f"refusing to load corrupt snapshot {snap} -- line 251: the store would hold 1002996 of 1000000 bytes",
                text,
            )
            text = "a\t" + "v" * 4095 + "\n"
            with open(snap, "w") as f:
                f.write(text)
            boot_refused(
                "long-line",
                f"refusing to load corrupt snapshot {snap} -- line 1: the line holds 4097 bytes, more than a SET can write",
                text,
            )

            # a torn last line (no LF) is refused like a line with no TAB
            with open(snap, "w") as f:
                f.write("good\tvalue here\ntorn\tval")
            boot_refused(
                "torn",
                "refusing to load corrupt snapshot",
                "good\tvalue here\ntorn\tval",
            )
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
                boot_refused(
                    label, f"refusing to load corrupt snapshot {snap} -- {want}", text
                )
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
                ("big-values", [f"k{i}\t{'v' * 4000}\n" for i in range(240)]),
            ]:
                with open(big, "w") as f:
                    f.write("".join(lines))
                t0 = time.monotonic()
                proc = harness.boot("kv", [str(port), big], server_src, port)
                took = time.monotonic() - t0
                check(f"{label}-boots-fast", took < 3, f"{took:.1f} s")
                key, value = lines[-1].rstrip("\n").split("\t")
                check(
                    f"{label}-loaded",
                    raw(f"GET {key}\n".encode()) == f"VALUE {value}\n",
                )
                harness.kill(proc)

            # The server stops after 100 accept failures in a row.
            harness.accept_failures(
                check, "kv-server", [str(port), snap], server_src, port
            )
        finally:
            harness.kill(proc)

    print(f"kv: {count[0]} checks passed")


if __name__ == "__main__":
    main()
