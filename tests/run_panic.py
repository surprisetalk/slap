#!/usr/bin/env python3
"""Run each panic test case from tests/panic.slap and verify it crashes with the expected error."""
import subprocess, sys, tempfile, os

def main():
    with open("tests/panic.slap") as f:
        lines = f.readlines()

    expect = None
    passed = 0
    failed = 0

    for i, line in enumerate(lines):
        line = line.strip()
        if not line or line.startswith("--"):
            if line.startswith("-- EXPECT:"):
                expect = line.split("-- EXPECT:")[1].strip()
            continue

        if expect is None:
            continue

        with tempfile.NamedTemporaryFile(mode="w", suffix=".slap", delete=False) as tmp:
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
            else:
                passed += 1
        except subprocess.TimeoutExpired:
            print(f"FAIL line {i+1}: timed out")
            print(f"  code: {line}")
            failed += 1
        finally:
            os.unlink(tmp_path)

        expect = None

    if failed > 0:
        print(f"panic tests: {passed} passed, {failed} FAILED")
        sys.exit(1)
    else:
        print(f"panic tests: {passed} passed")

if __name__ == "__main__":
    main()
