#!/usr/bin/env python3
"""Run every case in tests/errors.slap. The file's header gives the format."""

import concurrent.futures, os, re, subprocess, sys

LOC_RE = re.compile(r"-- (?:TYPE )?ERROR [^:\s]+:(\d+)(?::(\d+))?")
NEEDS_PARSE = re.compile(r"parse-(int|float|exact|spaces|while|until)\b")
TIMEOUT = 5


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


def check(case, parse_lib, binary):
    code = "\n".join(case["code"]) + "\n"
    src = (parse_lib if NEEDS_PARSE.search(code) else "") + code
    try:
        r = subprocess.run(
            [binary], input=src, capture_output=True, text=True, timeout=TIMEOUT
        )
    except subprocess.TimeoutExpired:
        return f"timed out after {TIMEOUT} s"
    if r.returncode == 0:
        return "expected an error, but it exited 0"
    if r.returncode != 1:
        return f"exited {r.returncode} (a negative code is a signal); errors exit 1\n{r.stderr.strip()}"
    missing = [e for e in case["expects"] if e not in r.stderr]
    if missing:
        return f"stderr lacks {missing}\n{r.stderr.strip()}"
    if case["loc"]:
        m = LOC_RE.search(r.stderr)
        got = {"line": int(m.group(1)), "col": int(m.group(2) or 0)} if m else {}
        if any(got.get(k) != v for k, v in case["loc"].items()):
            return f"expected location {case['loc']}, got {got or 'none'}\n{r.stderr.strip()}"
    return None


def main():
    # `run_errors.py ./slap-next tests/next.slap` runs the new checker's cases (todo.md step 4).
    binary, path = (sys.argv[1:3] + ["./slap", "tests/errors.slap"][len(sys.argv[1:3]):])
    if not os.access(binary, os.X_OK):
        sys.exit(f"errors: no {binary} binary. Run `make {binary[2:]}` from the repo root.")
    name = os.path.basename(path)
    all_cases = cases(open(path).read())
    parse_lib = open("examples/lib/parse.slap").read()
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for case, why in zip(
            all_cases, pool.map(lambda c: check(c, parse_lib, binary), all_cases)
        ):
            if why:
                failed += 1
                code = "\n    ".join(case["code"])
                print(
                    f"{name}:{case['line']}: {why}\n  code:\n    {code}",
                    file=sys.stderr,
                )
    if failed:
        sys.exit(f"{name}: {failed} of {len(all_cases)} cases failed")
    print(f"{name}: {len(all_cases)} cases passed")


if __name__ == "__main__":
    main()
