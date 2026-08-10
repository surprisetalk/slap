#!/usr/bin/env python3
"""Integration test for examples/feed.slap: type-check it, render both checked-in
fixtures, confirm the failure paths report rather than print an empty digest, and
bracket the parse size ceiling from both sides."""

import os, subprocess, sys, tempfile

LIBS = [
    "examples/lib/strings.slap",
    "examples/lib/parse.slap",
    "examples/lib/xml.slap",
    "examples/lib/rss.slap",
]
FEED = "examples/feed.slap"
RSS = "examples/feeds/sample.xml"
ATOM = "examples/feeds/sample-atom.xml"

passed = 0


def die(msg):
    print(f"feed: {msg}", file=sys.stderr)
    sys.exit(1)


def run(*argv, timeout=20):
    return subprocess.run(
        ["./slap", *argv], input=SRC, capture_output=True, text=True, timeout=timeout
    )


def check(name, cond, detail=""):
    global passed
    if not cond:
        die(f"{name} FAILED {detail}")
    passed += 1


def big_feed(n_items):
    """An RSS document with n_items entries, used to bracket the parse ceiling."""
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


if not os.access("./slap", os.X_OK):
    die("no ./slap binary; run 'make slap' first")
for f in LIBS + [FEED, RSS, ATOM]:
    if not os.path.exists(f):
        die(f"cannot find {f}; run from the repo root")

SRC = "".join(open(f).read() for f in LIBS + [FEED])

r = run("--check")
if r.returncode != 0:
    die(f"--check failed:\n{r.stderr}")
passed += 1

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

    # ---- the parse ceiling, from both sides ----
    # The old ceiling was exactly LOCAL_MAX bytes of source: `case` staged the
    # matched tagged payload through a 16384-slot C buffer, and parse.slap
    # carries the *remaining input* through `then`. `case` unwraps in place now,
    # `swap` is an in-place block rotation, and `into` neither stages the new
    # field nor rebuilds the record. The ceiling did not disappear -- it moved to
    # the next LOCAL_MAX buffer in the chain, `push` adding the finished element
    # record to its parent's child list, and past that the frame arena fills.
    # The bracket is exact and measured: 116 items renders, 117 does not. Both
    # ends are pinned so either direction of change is noticed.
    under = os.path.join(d, "under.xml")
    with open(under, "w") as f:
        f.write(big_feed(116))
    assert os.path.getsize(under) > 16384, os.path.getsize(under)
    r = run(under)
    check(
        "past-old-16384-cap-renders",
        r.returncode == 0 and r.stdout.rstrip().endswith("116 items"),
        f"{os.path.getsize(under)} bytes: {r.stderr[:200]}",
    )

    over = os.path.join(d, "over.xml")
    with open(over, "w") as f:
        f.write(big_feed(117))
    r = run(over)
    check(
        "over-the-remaining-ceiling",
        r.returncode != 0,
        f"{os.path.getsize(over)} bytes -- if this now PASSES, the next link in "
        f"the LOCAL_MAX chain was fixed too; raise both numbers to the new "
        f"measured boundary and update the note in feed.slap. "
        f"stderr: {r.stderr[:200]}",
    )
    check(
        "over-ceiling-reports-cleanly",
        r.returncode < 128,
        f"a size limit must report, never segfault. code {r.returncode}",
    )

print(f"feed: {passed} checks passed")
