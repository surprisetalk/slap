#!/usr/bin/env python3
"""Integration test for examples/todo.slap: drive the CLI through a real JSON
file, check the bytes it writes are the bytes json.slap can read back, and
confirm a file it cannot decode is refused and left alone rather than replaced
with an empty list."""

import json, os, resource, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(__file__))
import harness

LIBS = [
    "examples/lib/strings.slap",
    "examples/lib/parse.slap",
    "examples/lib/json.slap",
]
TODO = "examples/todo.slap"

check, count = harness.make_check("todo")

SRC = "".join(open(f).read() for f in LIBS + [TODO])


def run(*argv):
    return subprocess.run(
        ["./slap", *argv], input=SRC, capture_output=True, text=True, timeout=20
    )


with tempfile.TemporaryDirectory() as d:
    f = os.path.join(d, "todo.json")

    def todo(*argv):
        r = run(f, *argv)
        if r.returncode != 0:
            harness.die("todo", f"todo {argv} crashed:\n{r.stdout}\n{r.stderr}")
        return r.stdout

    def fails(*argv):
        r = run(f, *argv)
        return r.returncode != 0, r.stderr

    # ---- first run: a missing file is an empty list, and stays missing ----
    check("empty-message", "nothing to do" in todo())
    check("no-file-created", not os.path.exists(f), "a bare list must not write")

    # ---- add ----
    check("add-echoes", todo("add", "buy", "milk") == "1. [ ] buy milk\n")
    check("add-created-file", os.path.exists(f))
    todo("add", "write", "tests")
    todo("add", "ship", "it")
    check(
        "three-items",
        todo() == "1. [ ] buy milk\n2. [ ] write tests\n3. [ ] ship it\n",
        repr(todo()),
    )

    # the file is real JSON with real booleans, not strings or 0/1
    doc = json.load(open(f))
    check("json-shape", list(doc) == ["items"], repr(doc)[:120])
    check("json-len", len(doc["items"]) == 3)
    check(
        "json-item",
        doc["items"][0] == {"text": "buy milk", "done": False},
        repr(doc["items"][0]),
    )
    check("json-done-is-bool", doc["items"][0]["done"] is False, "must be a JSON bool")

    # ---- done / undone / rm ----
    check(
        "done-marks",
        todo("done", "2") == "1. [ ] buy milk\n2. [x] write tests\n3. [ ] ship it\n",
    )
    check("done-persisted", json.load(open(f))["items"][1]["done"] is True)
    check(
        "undone-clears",
        todo("undone", "2") == "1. [ ] buy milk\n2. [ ] write tests\n3. [ ] ship it\n",
    )
    check("undone-persisted", json.load(open(f))["items"][1]["done"] is False)
    todo("done", "3")
    check("rm-removes", todo("rm", "1") == "1. [ ] write tests\n2. [x] ship it\n")
    check(
        "rm-kept-the-right-flags",
        [i["done"] for i in json.load(open(f))["items"]] == [False, True],
        "rm must drop the same index from both columns",
    )
    check("rm-last", todo("rm", "2") == "1. [ ] write tests\n")
    check("rm-to-empty", "nothing to do" in todo("rm", "1"))
    check("empty-file-is-valid-json", json.load(open(f)) == {"items": []})

    # ---- text that has to survive JSON encoding ----
    todo("add", 'a "quoted" and a \\ backslash')
    check(
        "escapes-roundtrip",
        json.load(open(f))["items"][0]["text"] == 'a "quoted" and a \\ backslash',
        repr(json.load(open(f))["items"][0]["text"]),
    )
    check("escapes-render", '[ ] a "quoted" and a \\ backslash' in todo())
    for text in ["bell\x07here", "tab\there"]:
        todo("add", text)
    items = [i["text"] for i in json.load(open(f))["items"]]
    check(
        "control-bytes-roundtrip",
        items[-2:] == ["bell\x07here", "tab\there"],
        repr(items),
    )
    check("control-bytes-read-back", "bell\x07here" in todo())

    # ---- bad input ----
    for argv, want in [
        (("done", "abc"), "is not an item number"),
        (("done", "12x"), "is not an item number"),
        (("done", "99"), "no item 99"),
        (("done", "99999999999999999999"), "is not an item number"),
        (("done", "0"), "no item 0"),
        (("done",), "needs exactly one item number"),
        (("done", "1", "2"), "needs exactly one item number"),
        (("add",), "needs some text"),
        (("frobnicate",), "unknown command"),
    ]:
        bad, err = fails(*argv)
        check(f"reject{argv}", bad and want in err, repr(err[:160]))

    before = open(f).read()
    fails("done", "99")
    check("bad-index-left-file-alone", open(f).read() == before)

    # ---- a file that does not decode is refused, not replaced ----
    for label, text, want in [
        (
            "wrong-type",
            '{"items":[{"text":1,"done":false}]}',
            "expected string, got int",
        ),
        ("missing-field", '{"items":[{"text":"a"}]}', 'missing field "done"'),
        ("wrong-root", "[]", "expected object, got array"),
        ("not-json", "this is not json", "json: expected true"),
        ("empty-file", "", "json: unexpected end of input"),
        ("whitespace-only", "  \n\t ", "json: unexpected end of input"),
        (
            "truncated-array",
            '{"items":[{"text":"a","done":false}',
            "json: unexpected end of input in an array",
        ),
        (
            "trailing-garbage",
            '{"items":[]} nonsense',
            "json: expected the end of input after the value at byte 13",
        ),
    ]:
        with open(f, "w") as fh:
            fh.write(text)
        bad, err = fails()
        check(f"refuse-{label}", bad, "must not start from an empty list")
        check(
            f"refuse-{label}-says-why",
            "is not a todo file" in err and want in err,
            repr(err[:200]),
        )
        check(f"refuse-{label}-names-file", f in err, repr(err[:200]))
        check(f"refuse-{label}-untouched", open(f).read() == text, "must not rewrite")

    # the decoder's path annotation is the whole point of using json.slap here
    with open(f, "w") as fh:
        fh.write('{"items":[{"text":"ok","done":false},{"text":2,"done":false}]}')
    _, err = fails()
    check("error-names-the-path", "$.items[1].text" in err, repr(err[:200]))
    check("error-names-the-types", "expected string, got int" in err, repr(err[:200]))

    # ---- a file it cannot read is refused by path and reason, never replaced ----
    os.remove(f)
    os.mkdir(f)
    bad, err = fails("add", "x")
    check("dir-refused", bad and f"{f}: Is a directory" in err, repr(err[:200]))
    os.rmdir(f)
    if os.geteuid() != 0:
        text = '{"items":[{"text":"keep me","done":false}]}'
        for label, mode in [("mode-000", 0), ("write-only", 0o200)]:
            with open(f, "w") as fh:
                fh.write(text)
            os.chmod(f, mode)
            for argv in [(), ("add", "x")]:
                bad, err = fails(*argv)
                check(
                    f"{label}-refused{argv}",
                    bad and f"{f}: Permission denied" in err,
                    repr(err[:200]),
                )
            os.chmod(f, 0o644)
            check(f"{label}-untouched", open(f).read() == text)
        # read works and write does not
        with open(f, "w") as fh:
            fh.write(text)
        os.chmod(f, 0o444)
        bad, err = fails("add", "x")
        os.chmod(f, 0o644)
        check("read-only-refused", bad and f"cannot write {f}: Permission denied" in err, repr(err[:200]))
        check("read-only-untouched", open(f).read() == text)

    # ---- a write past a file size limit keeps the old file whole ----
    text = json.dumps({"items": [{"text": "item %d" % i, "done": False} for i in range(60)]})
    with open(f, "w") as fh:
        fh.write(text)
    r = subprocess.run(
        ["./slap", f, "add", "zzz"],
        input=SRC,
        capture_output=True,
        text=True,
        timeout=20,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024)),
    )
    check("size-limit-refused", r.returncode != 0 and f"cannot write {f}: File too large" in r.stderr, repr(r.stderr[-300:]))
    check("size-limit-untouched", open(f).read() == text)
    check("size-limit-no-temp", os.listdir(d) == ["todo.json"], repr(os.listdir(d)))

    # ---- keys and fields todo.slap does not use survive every write ----
    doc = {
        "version": 2,
        "items": [
            {
                "text": "a",
                "done": False,
                "due": 1.5,
                "tags": ["x", None, True, -0.25, 7, 0.1],
            }
        ],
        "owner": {"name": "té", "ratio": 0.30000000000000004, "big": 1e300},
    }
    with open(f, "w") as fh:
        json.dump(doc, fh)
    todo("add", "b")
    want = dict(doc, items=doc["items"] + [{"text": "b", "done": False}])
    got = json.load(open(f))
    check("unknown-kept-add", got == want and list(got) == list(doc), repr(got)[:300])
    check(
        "unknown-kept-floats",
        got["owner"]["ratio"] == 0.30000000000000004
        and got["items"][0]["tags"][5] == 0.1,
    )
    todo("done", "1")
    want["items"][0]["done"] = True
    check(
        "unknown-kept-done", json.load(open(f)) == want, repr(json.load(open(f)))[:300]
    )
    todo("rm", "2")
    want["items"].pop()
    check("unknown-kept-rm", json.load(open(f)) == want, repr(json.load(open(f)))[:300])

print(f"todo: {count[0]} checks passed")
