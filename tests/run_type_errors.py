#!/usr/bin/env python3
"""Run each type error test case and verify it produces a type error."""
import subprocess, sys, tempfile, os

def main():
    with open("tests/type_errors.slap") as f:
        lines = f.readlines()

    expect = None
    code_lines = []
    passed = 0
    failed = 0
    test_num = 0

    def run_test(expect_str, code, test_n):
        nonlocal passed, failed
        with tempfile.NamedTemporaryFile(mode="w", suffix=".slap", delete=False) as tmp:
            tmp.write(code)
            tmp_path = tmp.name
        try:
            r = subprocess.run(
                ["./slap", "--check"],
                stdin=open(tmp_path), capture_output=True, text=True, timeout=5
            )
            if expect_str not in r.stderr.lower():
                if r.returncode == 0:
                    print(f"FAIL test {test_n}: expected type error '{expect_str}', but type check passed")
                else:
                    print(f"FAIL test {test_n}: expected '{expect_str}' in stderr, got:")
                    print(f"  stderr: {r.stderr.strip()}")
                print(f"  code: {code.strip()}")
                failed += 1
            else:
                passed += 1
        except subprocess.TimeoutExpired:
            print(f"FAIL test {test_n}: timed out")
            failed += 1
        finally:
            os.unlink(tmp_path)

    for line in lines:
        line_s = line.strip()
        if not line_s or line_s.startswith("-- TYPE ERROR:"):
            continue
        if line_s.startswith("-- EXPECT:"):
            if code_lines and expect:
                test_num += 1
                run_test(expect.lower(), "\n".join(code_lines) + "\n", test_num)
                code_lines = []
            expect = line_s.split("-- EXPECT:")[1].strip()
            continue
        if line_s.startswith("--"):
            continue
        code_lines.append(line_s)

    if code_lines and expect:
        test_num += 1
        run_test(expect.lower(), "\n".join(code_lines) + "\n", test_num)

    if failed > 0:
        print(f"type error tests: {passed} passed, {failed} FAILED")
        sys.exit(1)
    else:
        print(f"type error tests: {passed} passed")

if __name__ == "__main__":
    main()
