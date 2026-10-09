#!/usr/bin/env python3
"""Integration test for examples/serve.slap and examples/fetch.slap: boot the
server over a temp directory, drive it with raw sockets for the cases a polite
client cannot produce, then fetch from it with the repo's own client and compare
bytes. The client half is the only place parse-http is used as designed."""

import os, random, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(__file__))
import harness

LIBS = ["examples/lib/strings.slap", "examples/lib/parse.slap"]
SERVE = "examples/serve.slap"
FETCH = "examples/fetch.slap"

SERVE_SRC = "".join(open(f).read() for f in LIBS + [SERVE])
FETCH_SRC = "".join(open(f).read() for f in LIBS + ["examples/lib/http.slap", FETCH])

port = random.randint(20000, 40000)

with tempfile.TemporaryDirectory() as d:
    root = os.path.join(d, "www")
    os.makedirs(os.path.join(root, "sub"))
    with open(os.path.join(root, "index.html"), "w") as f:
        f.write("<h1>hello</h1>\n")
    with open(os.path.join(root, "a.txt"), "w") as f:
        f.write("plain text\n")
    with open(os.path.join(root, "sub", "n.txt"), "w") as f:
        f.write("nested\n")
    # NULs and high bytes: the body must survive read -> tcp-send -> tcp-recv
    BINARY = bytes(range(256))
    with open(os.path.join(root, "b.bin"), "wb") as f:
        f.write(BINARY)
    # a file outside the root, to prove traversal cannot reach it
    with open(os.path.join(d, "secret.txt"), "w") as f:
        f.write("SECRET")

    def raw(payload, half_close=False):
        return harness.raw(port, payload, half_close=half_close, decode=False)

    def get(path, method=b"GET"):
        return raw(method + b" " + path + b" HTTP/1.0\r\n\r\n")

    def fetch(*argv, want_ok=True):
        r = subprocess.run(
            ["./slap", "127.0.0.1", str(port), *argv],
            input=FETCH_SRC.encode(),
            capture_output=True,
            timeout=20,
        )
        if want_ok and r.returncode != 0:
            harness.die(
                "serve", f"fetch {argv} crashed:\n{r.stderr.decode(errors='replace')}"
            )
        return r

    proc = harness.boot("serve", [str(port), root], SERVE_SRC, port)
    check, count = harness.server_check("serve", lambda: proc)

    try:
        # ---- ordinary GETs ----
        r = get(b"/a.txt")
        check("200-status", r.startswith(b"HTTP/1.0 200 OK\r\n"), repr(r[:40]))
        check("200-type", b"Content-Type: text/plain; charset=utf-8\r\n" in r)
        check("200-length", b"Content-Length: 11\r\n" in r)
        check("200-body", r.endswith(b"plain text\n"), repr(r[-20:]))
        check("html-type", b"text/html; charset=utf-8" in get(b"/index.html"))

        # the body is byte-transparent, which is the point of keeping it a byte
        # list from `read` all the way to `tcp-send`
        r = get(b"/b.bin")
        check("binary-length", b"Content-Length: 256\r\n" in r, repr(r[:120]))
        check("binary-body", r.split(b"\r\n\r\n", 1)[1] == BINARY, "all 256 bytes")
        check("binary-type", b"application/octet-stream" in r)

        # ---- directory listing ----
        r = get(b"/")
        check("index-200", r.startswith(b"HTTP/1.0 200 OK"), repr(r[:40]))
        for name in (b"a.txt", b"index.html", b"b.bin", b"sub"):
            check(
                f"index-lists-{name.decode()}",
                b'href="/' + name + b'"' in r,
                repr(r[-260:]),
            )
        # absolute hrefs: a listing reached without a trailing slash still links
        # correctly, which a relative href would get wrong
        r = get(b"/sub")
        check("subdir-no-slash", b'href="/sub/n.txt"' in r, repr(r[-160:]))
        check("subdir-slash", b'href="/sub/n.txt"' in get(b"/sub/"))
        check("nested-file", get(b"/sub/n.txt").endswith(b"nested\n"))

        # ---- misses ----
        check(
            "404-missing", get(b"/nope.txt").startswith(b"HTTP/1.0 404"), "missing file"
        )
        check("404-missing-dir", get(b"/nope/").startswith(b"HTTP/1.0 404"))
        check("404-not-a-dir", get(b"/a.txt/x").startswith(b"HTTP/1.0 404"))
        check("404-name-too-long", get(b"/" + b"a" * 300).startswith(b"HTTP/1.0 404"))
        # a file or directory it cannot read is a 500 that names the reason
        if os.geteuid() != 0:
            locked = os.path.join(root, "locked.txt")
            with open(locked, "w") as f:
                f.write("x")
            os.chmod(locked, 0)
            r = get(b"/locked.txt")
            os.chmod(locked, 0o644)
            os.remove(locked)
            check(
                "500-unreadable-file",
                r.startswith(b"HTTP/1.0 500")
                and b"Cannot read /locked.txt: Permission denied." in r
                and root.encode() not in r,
                repr(r[-200:]),
            )
            lockdir = os.path.join(root, "lockdir")
            os.mkdir(lockdir)
            os.chmod(lockdir, 0)
            r = get(b"/lockdir/")
            os.chmod(lockdir, 0o755)
            os.rmdir(lockdir)
            check(
                "500-unreadable-dir",
                r.startswith(b"HTTP/1.0 500") and b"Permission denied." in r and root.encode() not in r,
                repr(r[-200:]),
            )

        # ---- traversal, raw and percent-encoded ----
        for label, path in [
            ("dotdot", b"/../secret.txt"),
            ("deep-dotdot", b"/sub/../../secret.txt"),
            ("encoded", b"/%2e%2e/secret.txt"),
            ("encoded-slash", b"/..%2fsecret.txt"),
            ("mixed", b"/%2E%2E/secret.txt"),
            ("backslash", b"/..\\secret.txt"),
        ]:
            r = get(path)
            check(
                f"traversal-{label}-refused",
                r.startswith(b"HTTP/1.0 403"),
                repr(r[:60]),
            )
            check(f"traversal-{label}-no-leak", b"SECRET" not in r)
        # a NUL in the target must not truncate the path check
        r = get(b"/a.txt\x00/../secret.txt")
        check("traversal-nul-refused", r.startswith(b"HTTP/1.0 403"), repr(r[:60]))
        check("traversal-nul-no-leak", b"SECRET" not in r)
        # A doubled slash is not a traversal: the target is always prefixed with
        # the root, so //etc/passwd resolves under it and is simply absent. It
        # must 404, not 403 -- refusing it would mean the rule is guessing.
        r = get(b"//etc/passwd")
        check("double-slash-404", r.startswith(b"HTTP/1.0 404"), repr(r[:60]))
        check("double-slash-no-leak", b"root:" not in r)
        # a path that merely contains dots is fine -- the rule is "..", not "."
        with open(os.path.join(root, "v1.2.3.txt"), "w") as f:
            f.write("dots ok\n")
        check("dots-allowed", get(b"/v1.2.3.txt").endswith(b"dots ok\n"))

        # ---- methods ----
        r = get(b"/a.txt", method=b"HEAD")
        check("head-200", r.startswith(b"HTTP/1.0 200 OK"), repr(r[:40]))
        check("head-declares-length", b"Content-Length: 11\r\n" in r, "must not be 0")
        check("head-no-body", r.split(b"\r\n\r\n", 1)[1] == b"", repr(r[-20:]))
        for m in (b"POST", b"PUT", b"DELETE", b"OPTIONS"):
            check(
                f"{m.decode()}-405",
                get(b"/a.txt", method=m).startswith(b"HTTP/1.0 405"),
            )

        # ---- malformed and hostile ----
        check("no-request-line", b"400" in raw(b"\r\n\r\n"), "empty request line")
        check("one-token", b"400" in raw(b"JUNK\r\n\r\n"))
        check("garbage", b"400" in raw(b"\x00\x01\x02\r\n\r\n"))
        r = raw(b"GET /a.txt HTTP/1.0\r\nX-Pad: " + b"x" * 9000 + b"\r\n\r\n")
        check("oversize-413", b"413" in r, repr(r[:80]))
        # a short request with no terminator is a disconnect, not an oversize
        # request, and must not be reported as one
        r = raw(b"GET /a.txt HTTP/1.0", half_close=True)
        check("early-close-not-413", b"413" not in r, repr(r[:80]))
        check("server-still-up", get(b"/a.txt").startswith(b"HTTP/1.0 200"))

        # ---- the client half ----
        r = fetch("/a.txt")
        check("fetch-body", r.stdout == b"plain text\n", repr(r.stdout))
        r = fetch("/a.txt", "-i")
        check(
            "fetch-i-status",
            r.stdout.startswith(b"HTTP/1.0 200\n"),
            repr(r.stdout[:40]),
        )
        check(
            "fetch-i-header", b"Content-Type: text/plain; charset=utf-8\n" in r.stdout
        )
        check("fetch-i-body", r.stdout.endswith(b"plain text\n"))
        r = fetch("/a.txt", "-I")
        check("fetch-I-status", r.stdout.startswith(b"HTTP/1.0 200\n"))
        check("fetch-I-no-body", b"plain text" not in r.stdout, repr(r.stdout))
        # parse-http reads the status off the response line, so a 404 must come
        # back as 404 and not as the 0 it would report for a request line
        r = fetch("/nope.txt", "-I")
        check(
            "fetch-404-status",
            r.stdout.startswith(b"HTTP/1.0 404\n"),
            repr(r.stdout[:40]),
        )
        check(
            "fetch-binary",
            fetch("/b.bin").stdout == BINARY,
            "the client must not mangle bytes either",
        )
        check("fetch-index", b'href="/a.txt"' in fetch("/").stdout)

        # client-side errors
        r = fetch("/a.txt", "-x", want_ok=False)
        check("fetch-bad-flag", r.returncode != 0 and b"unknown flag" in r.stderr)
        r = subprocess.run(
            ["./slap", "127.0.0.1", str(port)],
            input=FETCH_SRC.encode(),
            capture_output=True,
            timeout=20,
        )
        check("fetch-usage", r.returncode != 0 and b"usage" in r.stderr)
        # a port with nothing on it: report, do not hang
        r = subprocess.run(
            ["./slap", "127.0.0.1", "1", "/"],
            input=FETCH_SRC.encode(),
            capture_output=True,
            timeout=20,
        )
        check("fetch-refused", r.returncode != 0, "a dead port must fail, not hang")
        # a client that resets is logged with its reason, and the server keeps serving
        harness.reset(port)
        check("reset-survives", get(b"/a.txt").startswith(b"HTTP/1.0 200"))
        harness.kill(proc)
        log = proc.stderr.read()
        check("reset-logged", "serve: recv failed: Connection reset by peer" in log, repr(log[-300:]))
        if os.geteuid() != 0:
            check("500-logged", f"serve: {root}/locked.txt: Permission denied" in log, repr(log[-300:]))
        harness.accept_failures(check, "serve", [str(port), root], SERVE_SRC, port)
    finally:
        harness.kill(proc)

print(f"serve: {count[0]} checks passed")
