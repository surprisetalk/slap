#!/usr/bin/env python3
"""Run every case in tests/errors.slap. The file's header gives the format."""

import concurrent.futures, os, re, signal, subprocess, sys

LOC_RE = re.compile(r"-- (?:TYPE )?ERROR [^:\s]+:(\d+)(?::(\d+))?")
# A report quotes the source line ("   12| code") and marks it with carets. The
# quote holds the case's own code, so a marker such as "done-fp1" fail would
# match a type error on that line: EXPECT text must come from the message.
QUOTE_RE = re.compile(r"^\s*\d+\| |^\s*\^+\s*$")
NEEDS_PARSE = re.compile(r"parse-(int|float|exact|spaces|while|until)\b")
# A case gets CPU seconds, so a busy machine does not fail it. The wall limit
# only stops a case that blocks, which uses no CPU.
CPU_LIMIT = 5
WALL_LIMIT = 30


def cases(text):
    out, cur = [], None
    for n, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if s.startswith("-- EXPECT:"):
            if cur is None or cur["code"]:
                cur = {"line": n, "expects": [], "loc": {}, "code": []}
                out.append(cur)
            text = s.split(":", 1)[1].strip()
            if not text:
                sys.exit(f"errors.slap:{n}: an empty EXPECT matches anything")
            cur["expects"].append(text)
        elif s.startswith("-- EXPECT-LINE:") or s.startswith("-- EXPECT-COL:"):
            cur["loc"]["line" if "LINE" in s else "col"] = int(s.split(":", 1)[1])
        elif s and not s.startswith("--"):
            if cur is None:
                sys.exit(f"errors.slap:{n}: code before any -- EXPECT: line")
            cur["code"].append(line)
    for c in out:
        if not c["code"]:
            sys.exit(f"errors.slap:{c['line']}: EXPECT with no code under it")
    if not out:
        sys.exit("errors.slap: no cases found")
    return out


def check(case, parse_lib):
    code = "\n".join(case["code"]) + "\n"
    src = (parse_lib if NEEDS_PARSE.search(code) else "") + code
    try:
        # ulimit -t, not preexec_fn: preexec_fn can deadlock a child forked from a thread.
        r = subprocess.run(
            ["bash", "-c", f"ulimit -t {CPU_LIMIT} && exec ./slap"],
            input=src,
            capture_output=True,
            text=True,
            timeout=WALL_LIMIT,
        )
    except subprocess.TimeoutExpired:
        return f"no exit after {WALL_LIMIT} s: the case blocks, or the machine is too busy to run it"
    if r.returncode == -signal.SIGXCPU:
        return f"used more than {CPU_LIMIT} s of CPU"
    if r.returncode == 0:
        return "expected an error, but it exited 0"
    if r.returncode != 1:
        return f"exited {r.returncode} (a negative code is a signal); errors exit 1\n{r.stderr.strip()}"
    message = "\n".join(l for l in r.stderr.splitlines() if not QUOTE_RE.match(l))
    missing = [e for e in case["expects"] if e not in message]
    if missing:
        return f"the error message lacks {missing}; a quoted source line does not count\n{r.stderr.strip()}"
    if case["loc"]:
        m = LOC_RE.search(r.stderr)
        got = {"line": int(m.group(1)), "col": int(m.group(2) or 0)} if m else {}
        if any(got.get(k) != v for k, v in case["loc"].items()):
            return f"expected location {case['loc']}, got {got or 'none'}\n{r.stderr.strip()}"
    return None


def main():
    if not os.access("./slap", os.X_OK):
        sys.exit("errors: no ./slap binary. Run `make slap` from the repo root.")
    all_cases = cases(open("tests/errors.slap").read())
    parse_lib = open("examples/lib/parse.slap").read()
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for case, why in zip(
            all_cases, pool.map(lambda c: check(c, parse_lib), all_cases)
        ):
            if why:
                failed += 1
                code = "\n    ".join(case["code"])
                print(
                    f"errors.slap:{case['line']}: {why}\n  code:\n    {code}",
                    file=sys.stderr,
                )
    if failed:
        sys.exit(f"errors.slap: {failed} of {len(all_cases)} cases failed")
    print(f"errors.slap: {len(all_cases)} cases passed")


if __name__ == "__main__":
    main()
