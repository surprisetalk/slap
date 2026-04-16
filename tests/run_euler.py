#!/usr/bin/env python3
"""Run every examples/euler/*.slap with a per-file timeout; fail on nonzero exit or timeout."""
import glob, subprocess, sys, time

TIMEOUT = 20

def main():
    files = sorted(glob.glob("examples/euler/*.slap"))
    if not files:
        print("euler: no files found under examples/euler/")
        sys.exit(1)

    failed = 0
    for path in files:
        name = path.split("/")[-1]
        start = time.time()
        try:
            with open("examples/lib/strings.slap") as lib, open(path) as prog:
                src = lib.read() + prog.read()
            r = subprocess.run(
                ["./slap"], input=src,
                capture_output=True, text=True, timeout=TIMEOUT
            )
            dur = time.time() - start
            if r.returncode != 0:
                print(f"  euler/{name:<8} FAILED ({dur:.2f}s)")
                if r.stderr.strip():
                    print(f"    stderr: {r.stderr.strip().splitlines()[-1]}")
                failed += 1
            else:
                print(f"  euler/{name:<8} ok ({dur:.2f}s)")
        except subprocess.TimeoutExpired:
            print(f"  euler/{name:<8} TIMEOUT (>{TIMEOUT}s)")
            failed += 1

    if failed > 0:
        print(f"euler: {len(files) - failed} passed, {failed} FAILED")
        sys.exit(1)
    print(f"euler: {len(files)} passed")

if __name__ == "__main__":
    main()
