#!/usr/bin/env python3
"""Run every examples/euler/*.slap with a per-file timeout; fail on nonzero exit or timeout."""
import glob, os, subprocess, sys, time

TIMEOUT = 20
LIB = "examples/lib/strings.slap"

def main():
    if not os.access("./slap", os.X_OK):
        print("euler: no ./slap binary; run 'make slap' first", file=sys.stderr)
        print(f"  cwd: {os.getcwd()}", file=sys.stderr)
        sys.exit(1)

    if not os.path.exists(LIB):
        print(f"euler: cannot find {LIB}", file=sys.stderr)
        print(f"  cwd: {os.getcwd()}", file=sys.stderr)
        print("  Every euler program is prepended with this library for int-str.", file=sys.stderr)
        print("  Run from the repo root, or restore the file if it is missing.", file=sys.stderr)
        sys.exit(1)

    files = sorted(glob.glob("examples/euler/*.slap"))
    if not files:
        print("euler: no files found under examples/euler/")
        sys.exit(1)

    try:
        lib_src = open(LIB).read()
    except OSError as e:
        print(f"euler: cannot read {LIB}: {e}", file=sys.stderr)
        sys.exit(1)
    except UnicodeDecodeError as e:
        print(f"euler: {LIB} is not valid UTF-8: {e}", file=sys.stderr)
        sys.exit(1)

    failed = 0
    for path in files:
        name = path.split("/")[-1]
        start = time.time()
        try:
            with open(path) as prog:
                src = lib_src + prog.read()
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
