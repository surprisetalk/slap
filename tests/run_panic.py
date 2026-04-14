#!/usr/bin/env python3
"""Run each panic test case from tests/panic.slap and verify it crashes with the expected error.

Each test is preceded by `-- EXPECT: <substring>` and optionally `-- EXPECT-LINE: N` and
`-- EXPECT-COL: N` directives that assert the reported source position.
"""
import re, subprocess, sys, tempfile, os

LOC_RE = re.compile(r"-- ERROR [^:\s]+:(\d+)(?::(\d+))?")

def main():
    with open("tests/panic.slap") as f:
        lines = f.readlines()

    with open("examples/lib/parse.slap") as f:
        parse_lib = f.read()

    expect = None
    expect_line = None
    expect_col = None
    passed = 0
    failed = 0

    for i, line in enumerate(lines):
        line = line.strip()
        if not line or line.startswith("--"):
            if line.startswith("-- EXPECT:"):
                expect = line.split("-- EXPECT:", 1)[1].strip()
            elif line.startswith("-- EXPECT-LINE:"):
                expect_line = int(line.split("-- EXPECT-LINE:", 1)[1].strip())
            elif line.startswith("-- EXPECT-COL:"):
                expect_col = int(line.split("-- EXPECT-COL:", 1)[1].strip())
            continue

        if expect is None:
            continue

        needs_parse = re.search(r"parse-(int|float|exact|spaces|while|until)\b", line) is not None
        with tempfile.NamedTemporaryFile(mode="w", suffix=".slap", delete=False) as tmp:
            if needs_parse:
                tmp.write(parse_lib)
            tmp.write(line + "\n")
            tmp_path = tmp.name

        try:
            r = subprocess.run(
                ["./slap"],
                stdin=open(tmp_path), capture_output=True, text=True, timeout=5
            )
            if r.returncode == 0:
                print(f"FAIL line {i+1}: expected panic '{expect}', but succeeded")
                print(f"  code: {line}")
                failed += 1
            elif expect not in r.stderr:
                print(f"FAIL line {i+1}: expected '{expect}' in error, got:")
                print(f"  code: {line}")
                print(f"  stderr: {r.stderr.strip()}")
                failed += 1
            elif expect_line is not None or expect_col is not None:
                m = LOC_RE.search(r.stderr)
                if not m:
                    print(f"FAIL line {i+1}: expected line:col in error banner, got:")
                    print(f"  code: {line}")
                    print(f"  stderr: {r.stderr.strip()}")
                    failed += 1
                else:
                    got_line = int(m.group(1))
                    got_col = int(m.group(2)) if m.group(2) else None
                    ok = True
                    if expect_line is not None and got_line != expect_line: ok = False
                    if expect_col is not None and got_col != expect_col: ok = False
                    if ok:
                        passed += 1
                    else:
                        print(f"FAIL line {i+1}: expected loc {expect_line}:{expect_col}, got {got_line}:{got_col}")
                        print(f"  code: {line}")
                        failed += 1
            else:
                passed += 1
        except subprocess.TimeoutExpired:
            print(f"FAIL line {i+1}: timed out")
            print(f"  code: {line}")
            failed += 1
        finally:
            os.unlink(tmp_path)

        expect = None
        expect_line = None
        expect_col = None

    if failed > 0:
        print(f"panic tests: {passed} passed, {failed} FAILED")
        sys.exit(1)
    else:
        print(f"panic tests: {passed} passed")

if __name__ == "__main__":
    main()
