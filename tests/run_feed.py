#!/usr/bin/env python3
"""Integration test for examples/feed.slap: type-check it, render both checked-in
fixtures, confirm the failure paths report rather than print an empty digest, and
bracket the parse size ceiling from both sides."""

import os, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(__file__))
import harness

LIBS = [
    "examples/lib/strings.slap",
    "examples/lib/parse.slap",
    "examples/lib/xml.slap",
    "examples/lib/rss.slap",
]
FEED = "examples/feed.slap"
RSS = "examples/feeds/sample.xml"
ATOM = "examples/feeds/sample-atom.xml"

check, count = harness.make_check("feed")


def run(*argv, timeout=20):
    return subprocess.run(
        ["./slap", *argv], input=SRC, capture_output=True, text=True, timeout=timeout
    )


SRC = "".join(open(f).read() for f in LIBS + [FEED])


# ---- RSS 2.0 fixture ----
r = run(RSS)
check("rss-exit-0", r.returncode == 0, r.stderr[:300])
out = r.stdout
check("rss-kind", "== Slap Notes (rss)" in out, repr(out[:80]))
check("rss-link", "http://example.com/slap" in out)
check("rss-count", out.rstrip().endswith("4 items"), repr(out[-40:]))
check("rss-entities", "Boxes & linear types" in out, "&amp; must decode")
# the squeeze: no description reaches the screen with its source indentation
check("rss-squeezed", "list.\n        peek" not in out and "  peek leaves" not in out)
check("rss-clipped", "..." in out, "long summaries are clipped")
# item 4 omits pubDate and description entirely -- those lines must vanish, not
# render as blank indented lines. This is the only thing that catches a `field`
# guard that tests the wrong operand.
tail = out[out.index(" 4. Untitled draft") :]
check(
    "rss-empty-fields-omitted",
    [ln for ln in tail.splitlines() if ln.strip()][:2]
    == [" 4. Untitled draft", "    http://example.com/slap/draft"],
    repr(tail.splitlines()[:4]),
)

# ---- Atom fixture ----
r = run(ATOM)
check("atom-exit-0", r.returncode == 0, r.stderr[:300])
out = r.stdout
check("atom-kind", "== Slap Releases (atom)" in out, repr(out[:80]))
check("atom-count", out.rstrip().endswith("2 items"))
# entry 1 has <published>, entry 2 only <updated>; entry 1 has <summary>,
# entry 2 only <content>. Both fallbacks must fire or these are empty.
check("atom-published", "2025-02-01T12:00:00Z" in out)
check("atom-updated-fallback", "2025-02-08T12:00:00Z" in out)
check("atom-summary", "Eight of raven's nine reference ROMs" in out)
check("atom-content-fallback", "Ports 0xc0 to 0xca" in out)
check("atom-link-attr", "http://example.com/slap/releases/uxn" in out)

# ---- default source ----
r = run()
check("default-source", r.returncode == 0 and "Slap Notes" in r.stdout)

# ---- failure paths: report, never an empty digest ----
r = run("/nonexistent/feed.xml")
check("missing-exit", r.returncode != 0, f"(code {r.returncode})")
check("missing-names-path", "/nonexistent/feed.xml" in r.stderr, repr(r.stderr[:200]))
check("missing-no-digest", "==" not in r.stdout, repr(r.stdout[:120]))

with tempfile.TemporaryDirectory() as d:
    notfeed = os.path.join(d, "notfeed.xml")
    with open(notfeed, "w") as f:
        f.write("<html><body>hi</body></html>")
    r = run(notfeed)
    check("notfeed-exit", r.returncode != 0, f"(code {r.returncode})")
    check("notfeed-explains", "is not a feed" in r.stderr, repr(r.stderr[:200]))
    check("notfeed-reason", "unknown feed format" in r.stderr, repr(r.stderr[:200]))
    check("notfeed-no-digest", "==" not in r.stdout)

    # rel="self" names the feed document, not its page, so neither the feed nor the entry has a link line.
    selfonly = os.path.join(d, "self.xml")
    with open(selfonly, "w") as f:
        f.write(
            '<feed><title>S</title><link rel="self" href="http://u/feed.atom"/>'
            '<entry><title>E</title><link rel="self" href="http://u/e.atom"/><id>1</id></entry></feed>'
        )
    r = run(selfonly)
    check(
        "atom-self-link-omitted",
        r.returncode == 0 and r.stdout == "== S (atom)\n\n 1. E\n\n1 item\n",
        repr(r.stdout[:200]) + r.stderr[:200],
    )

    # A title holds markup: feed.slap decodes its entities and keeps its tags.
    markup = os.path.join(d, "markup.xml")
    with open(markup, "w") as f:
        f.write(
            "<rss><channel><title>AT&amp;T</title><link>L</link><description>d</description>"
            "<item><title>A <b>bold</b> post</title></item></channel></rss>"
        )
    r = run(markup)
    check(
        "markup-titles",
        r.returncode == 0
        and r.stdout.startswith("== AT&T (rss)\n")
        and " 1. A <b>bold</b> post\n" in r.stdout,
        repr(r.stdout[:200]) + r.stderr[:200],
    )

    # A feed far past the old ceiling of 116 items, which recursion per element set.
    big = os.path.join(d, "big.xml")
    with open(big, "w") as f:
        f.write(harness.big_feed(1000))
    r = run(big)
    check(
        "a-1000-item-feed-renders",
        r.returncode == 0 and r.stdout.rstrip().endswith("1000 items"),
        f"{os.path.getsize(big)} bytes: {r.stderr[:200]}",
    )

    # The 512-byte text cut and the 64-byte summary cut each land inside a
    # two-byte character; both cut before it.
    utf8 = os.path.join(d, "utf8.xml")
    title, desc = "a" + "é" * 300, "a" + "é" * 100
    with open(utf8, "w", encoding="utf-8") as f:
        f.write(
            '<?xml version="1.0"?><rss version="2.0"><channel><title>T</title>'
            "<link>http://e.com</link><description>d</description>"
            f"<item><title>{title}</title><description>{desc}</description></item></channel></rss>"
        )
    r = subprocess.run(
        ["./slap", utf8], input=SRC.encode(), capture_output=True, timeout=20
    )
    try:
        out = r.stdout.decode("utf-8")
    except UnicodeDecodeError as e:
        out = f"invalid UTF-8: {e}"
    check("utf8-exit-0", r.returncode == 0, r.stderr[:300])
    check("utf8-text-cut", " 1. a" + "é" * 255 + "\n" in out, repr(out[:80]))
    check("utf8-summary-cut", "    a" + "é" * 31 + "...\n" in out, repr(out[-120:]))

    # 3- and 4-byte characters, a cut on a character start, and stray
    # continuation bytes after a whole character, which the cut keeps whole.
    items = [
        ("a" + "\u20ac" * 300, "x"),
        ("t2", "ab" + "\U0001f600" * 40),
        ("t3", "a" * 64 + "é"),
    ]
    body = "".join(
        f"<item><title>{t}</title><description>{s}</description></item>"
        for t, s in items
    )
    with open(utf8, "wb") as f:
        f.write(
            (
                '<?xml version="1.0"?><rss version="2.0"><channel><title>T</title>'
                f"<link>http://e.com</link><description>d</description>{body}"
                "<item><title>t4</title><description>"
            ).encode()
            + b"a" * 60
            + "\U0001f600".encode()
            + b"\x80\x80\x80aaaa"
            + b"</description></item></channel></rss>"
        )
    r = subprocess.run(
        ["./slap", utf8], input=SRC.encode(), capture_output=True, timeout=20
    )
    check("utf8-wide-exit-0", r.returncode == 0, r.stderr[:300])
    out = r.stdout
    check(
        "utf8-3-byte-cut",
        (" 1. a" + "\u20ac" * 170 + "\n").encode() in out,
        repr(out[:80]),
    )
    check(
        "utf8-4-byte-cut",
        ("    ab" + "\U0001f600" * 15 + "...\n").encode() in out,
        repr(out[-400:]),
    )
    check(
        "utf8-cut-on-start",
        ("    " + "a" * 64 + "...\n").encode() in out,
        repr(out[-400:]),
    )
    check(
        "utf8-stray-bytes-kept-whole",
        b"    " + b"a" * 60 + "\U0001f600...\n".encode() in out,
        repr(out[-200:]),
    )
print(f"feed: {count[0]} checks passed")
