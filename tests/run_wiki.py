#!/usr/bin/env python3
"""Integration test for examples/wiki.slap: start the server on a random port,
drive it with real HTTP requests, verify pages persist and hostile input fails safely."""

import os, random, sys, tempfile, urllib.error, urllib.request

sys.path.insert(0, os.path.dirname(__file__))
import harness

LIBS = ["examples/lib/strings.slap", "examples/lib/parse.slap"]
WIKI = "examples/wiki.slap"


def fetch(url, data=None, timeout=5):
    try:
        with urllib.request.urlopen(url, data=data, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def main():
    src = "".join(open(f).read() for f in LIBS + [WIKI])

    with tempfile.TemporaryDirectory() as pages:
        with open(os.path.join(pages, "Home.txt"), "w") as f:
            f.write("seed home page with a [Linked] page\n")

        port = random.randint(20000, 40000)
        proc = harness.boot("wiki", [str(port), pages], src, port)
        check, count = harness.server_check("wiki", lambda: proc)

        try:
            base = f"http://127.0.0.1:{port}"

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
                harness.raw(port, b"GET /../pwn HTTP/1.0\r\n\r\n").startswith(
                    "HTTP/1.0 400"
                ),
            )
            code, body = fetch(base + "/edit/..%2Fpwn", data=b"content=owned")
            check("traversal-encoded", code == 400, f"(code {code})")
            check(
                "no-escaped-file",
                not os.path.exists(os.path.join(pages, "..", "pwn.txt"))
                and not os.path.exists("pwn.txt"),
            )

            check(
                "garbage-400",
                harness.raw(port, b"XYZ\r\n\r\n").startswith("HTTP/1.0 400"),
            )
            check(
                "bare-crlf-400",
                harness.raw(port, b"\r\n\r\n").startswith("HTTP/1.0 400"),
            )

            # regression: "content-length:" in the target or another header
            # name must not be read as the body length (used to stall forever)
            check(
                "cl-in-path-prompt-400",
                harness.raw(
                    port, b"GET /content-length:99 HTTP/1.0\r\n\r\n"
                ).startswith("HTTP/1.0 400"),
            )
            check(
                "cl-lookalike-header",
                harness.raw(
                    port, b"GET / HTTP/1.0\r\nX-Content-Length: 50\r\n\r\n"
                ).startswith("HTTP/1.0 200"),
            )
            check(
                "cl-malformed-400",
                harness.raw(
                    port, b"POST /edit/A HTTP/1.0\r\nContent-Length: abc\r\n\r\n"
                ).startswith("HTTP/1.0 400"),
            )

            code, _ = fetch(base + "/" + "a" * 64)
            check("name-64-ok", code == 404, f"(code {code})")
            code, _ = fetch(base + "/" + "a" * 65)
            check("name-65-rejected", code == 400, f"(code {code})")
            check(
                "method-405",
                harness.raw(port, b"DELETE /Home HTTP/1.0\r\n\r\n").startswith(
                    "HTTP/1.0 405"
                ),
            )
            big = (
                b"POST /edit/Big HTTP/1.0\r\nContent-Length: 50000\r\n\r\n"
                + b"x" * 50000
            )
            # 413 if the response outran the RST from closing on unread bytes;
            # an empty reply (pure reset) also proves the server refused it.
            big_reply = harness.raw(port, big)
            check(
                "oversize-defended",
                big_reply == "" or "413" in big_reply.splitlines()[0],
            )
            check(
                "oversize-post-413",
                fetch(base + "/edit/Cap", data=b"content=" + b"a" * 4000)[0] == 413,
            )

            # a page it cannot read is a 500 naming the reason, never an offer
            # to create it; a save it cannot write and an index it cannot list
            # are 500s too, and the server stays up
            if os.geteuid() != 0:
                locked = os.path.join(pages, "Locked.txt")
                for label, mode in [("mode-000", 0), ("write-only", 0o200)]:
                    with open(locked, "w") as f:
                        f.write("keep me\n")
                    os.chmod(locked, mode)
                    for path in ["/Locked", "/edit/Locked"]:
                        code, body = fetch(base + path)
                        check(
                            f"{label}-{path}-500",
                            code == 500
                            and "Cannot read page Locked: Permission denied." in body
                            and pages not in body
                            and "create it" not in body
                            and "<textarea" not in body,
                            f"(code {code}) {body[-200:]}",
                        )
                    if mode == 0:
                        code, body = fetch(base + "/edit/Locked", data=b"content=new")
                        check(
                            "unwritable-save-500",
                            code == 500 and "Cannot save page Locked: Permission denied." in body and pages not in body,
                            f"(code {code}) {body[-200:]}",
                        )
                    os.chmod(locked, 0o644)
                    check(f"{label}-untouched", open(locked).read() == "keep me\n")
                os.remove(locked)
                os.chmod(pages, 0o300)
                code, body = fetch(base + "/index")
                os.chmod(pages, 0o755)
                check(
                    "unlistable-index-500",
                    code == 500 and "Cannot list the pages: Permission denied." in body and pages not in body,
                    f"(code {code}) {body[-200:]}",
                )
            # with the pages directory gone, a page is not missing: no offer to create it
            os.rename(pages, pages + ".gone")
            for path in ["/Nope", "/edit/Nope"]:
                code, body = fetch(base + path)
                check(f"dir-gone-{path}-500", code == 500 and "create it" not in body and "<textarea" not in body, f"(code {code})")
            os.rename(pages + ".gone", pages)

            code, body = fetch(base + "/")
            check("still-alive", code == 200, f"(code {code})")
            check("server-running", proc.poll() is None)
            # a client that resets is logged with its reason, and the server keeps serving
            harness.reset(port)
            code, body = fetch(base + "/")
            check("reset-survives", code == 200, f"(code {code})")
            harness.kill(proc)
            log = proc.stderr.read()
            check("reset-logged", "wiki: recv failed: Connection reset by peer" in log, repr(log[-300:]))
            if os.geteuid() != 0:
                check("500-logged", f"wiki: {pages}/Locked.txt: Permission denied" in log, repr(log[-300:]))
            harness.accept_failures(check, "wiki", [str(port), pages], src, port)
        finally:
            harness.kill(proc)

    print(f"wiki: {count[0]} checks passed")


if __name__ == "__main__":
    main()
