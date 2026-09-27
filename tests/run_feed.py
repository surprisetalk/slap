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

    # A feed past 16384 bytes of source, the old ceiling on every parse.
    big = os.path.join(d, "big.xml")
    with open(big, "w") as f:
        f.write(big_feed(116))
    assert os.path.getsize(big) > 16384, os.path.getsize(big)
    r = run(big)
    check(
        "past-old-16384-cap-renders",
        r.returncode == 0 and r.stdout.rstrip().endswith("116 items"),
        f"{os.path.getsize(big)} bytes: {r.stderr[:200]}",
    )
print(f"feed: {count[0]} checks passed")
