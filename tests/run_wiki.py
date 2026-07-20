#!/usr/bin/env python3
"""Integration test for examples/wiki.slap: start the server on a random port,
drive it with real HTTP requests, verify pages persist and hostile input fails safely."""

import os, random, socket, subprocess, sys, tempfile, time, urllib.error, urllib.request

LIBS = ["examples/lib/strings.slap", "examples/lib/parse.slap"]
WIKI = "examples/wiki.slap"


def die(msg):
    print(f"wiki: {msg}", file=sys.stderr)
    sys.exit(1)


def fetch(url, data=None, timeout=5):
    try:
        with urllib.request.urlopen(url, data=data, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def raw(port, payload, timeout=5):
    # The server may close mid-conversation (oversize requests), which the
    # kernel surfaces as RST; report whatever arrived before the reset.
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    chunks = b""
    try:
        s.sendall(payload)
        while True:
            c = s.recv(4096)
            if not c:
                break
            chunks += c
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        s.close()
    return chunks.decode(errors="replace")


def main():
    if not os.access("./slap", os.X_OK):
        die("no ./slap binary; run 'make slap' first")
    for f in LIBS + [WIKI]:
        if not os.path.exists(f):
            die(f"cannot find {f}; run from the repo root")

    src = "".join(open(f).read() for f in LIBS + [WIKI])

    r = subprocess.run(
        ["./slap", "--check"], input=src, capture_output=True, text=True, timeout=30
    )
    if r.returncode != 0:
        die(f"--check failed:\n{r.stderr}")

    passed = 0
    with tempfile.TemporaryDirectory() as pages:
        with open(os.path.join(pages, "Home.txt"), "w") as f:
            f.write("seed home page with a [Linked] page\n")

        port = random.randint(20000, 40000)
        proc = subprocess.Popen(
            ["./slap", str(port), pages],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        proc.stdin.write(src)
        proc.stdin.close()
        try:
            for _ in range(50):
                if proc.poll() is not None:
                    die(f"server exited early:\n{proc.stderr.read()}")
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                    break
                except OSError:
                    time.sleep(0.1)
            else:
                die("server never started listening")

            base = f"http://127.0.0.1:{port}"

            def check(name, cond, detail=""):
                nonlocal passed
                if not cond:
                    # a mid-test server panic surfaces as connection errors;
                    # the panic message on stderr is the actual diagnostic
                    proc.terminate()
                    try:
                        proc.wait(5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                    err = proc.stderr.read()
                    tail = f"\n  server stderr:\n{err}" if err.strip() else ""
                    die(f"{name} FAILED {detail}{tail}")
                passed += 1

            code, body = fetch(base + "/")
            check(
                "get-home", code == 200 and "seed home page" in body, f"(code {code})"
            )
            check("linkify", "<a href='/Linked'>Linked</a>" in body)

            code, body = fetch(base + "/NoSuchPage")
            check(
                "missing-404",
                code == 404 and "/edit/NoSuchPage" in body,
                f"(code {code})",
            )

            code, body = fetch(base + "/edit/Home")
            check("edit-form", code == 200 and "<textarea" in body, f"(code {code})")

            # urllib follows the 303 back to GET /TestPage
            code, body = fetch(
                base + "/edit/TestPage",
                data=b"content=Hello+world%21+%3Cb%3Ebold%3C%2Fb%3E%0D%0Anext",
            )
            check("post-follows-303", code == 200, f"(code {code})")
            code, body = fetch(base + "/TestPage")
            check("post-persisted", "Hello world!" in body)
            check(
                "post-escaped",
                "&lt;b&gt;bold&lt;/b&gt;" in body and "<b>bold</b>" not in body,
            )
            check("post-br", "<br>next" in body)
            on_disk = open(os.path.join(pages, "TestPage.txt")).read()
            check("file-on-disk", on_disk == "Hello world! <b>bold</b>\nnext")

            code, body = fetch(base + "/edit/TestPage", data=b"content=rewritten")
            code, body = fetch(base + "/TestPage")
            check("re-post-updates", "rewritten" in body and "Hello" not in body)

            code, body = fetch(base + "/index")
            check(
                "index-lists", "/Home" in body and "/TestPage" in body, f"(code {code})"
            )

            check(
                "traversal-raw",
                raw(port, b"GET /../pwn HTTP/1.0\r\n\r\n").startswith("HTTP/1.0 400"),
            )
            code, body = fetch(base + "/edit/..%2Fpwn", data=b"content=owned")
            check("traversal-encoded", code == 400, f"(code {code})")
            check(
                "no-escaped-file",
                not os.path.exists(os.path.join(pages, "..", "pwn.txt"))
                and not os.path.exists("pwn.txt"),
            )

            check("garbage-400", raw(port, b"XYZ\r\n\r\n").startswith("HTTP/1.0 400"))
            check("bare-crlf-400", raw(port, b"\r\n\r\n").startswith("HTTP/1.0 400"))

            # regression: "content-length:" in the target or another header
            # name must not be read as the body length (used to stall forever)
            check(
                "cl-in-path-prompt-400",
                raw(port, b"GET /content-length:99 HTTP/1.0\r\n\r\n").startswith(
                    "HTTP/1.0 400"
                ),
            )
            check(
                "cl-lookalike-header",
                raw(port, b"GET / HTTP/1.0\r\nX-Content-Length: 50\r\n\r\n").startswith(
                    "HTTP/1.0 200"
                ),
            )
            check(
                "cl-malformed-400",
                raw(
                    port, b"POST /edit/A HTTP/1.0\r\nContent-Length: abc\r\n\r\n"
                ).startswith("HTTP/1.0 400"),
            )

            code, _ = fetch(base + "/" + "a" * 64)
            check("name-64-ok", code == 404, f"(code {code})")
            code, _ = fetch(base + "/" + "a" * 65)
            check("name-65-rejected", code == 400, f"(code {code})")
            check(
                "method-405",
                raw(port, b"DELETE /Home HTTP/1.0\r\n\r\n").startswith("HTTP/1.0 405"),
            )
            big = (
                b"POST /edit/Big HTTP/1.0\r\nContent-Length: 50000\r\n\r\n"
                + b"x" * 50000
            )
            # 413 if the response outran the RST from closing on unread bytes;
            # an empty reply (pure reset) also proves the server refused it.
            big_reply = raw(port, big)
            check(
                "oversize-defended",
                big_reply == "" or "413" in big_reply.splitlines()[0],
            )
            check(
                "oversize-post-413",
                fetch(base + "/edit/Cap", data=b"content=" + b"a" * 4000)[0] == 413,
            )

            code, body = fetch(base + "/")
            check("still-alive", code == 200, f"(code {code})")
            check("server-running", proc.poll() is None)
        finally:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()

    print(f"wiki: {passed} checks passed")


if __name__ == "__main__":
    main()
