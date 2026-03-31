#!/bin/bash
set -euo pipefail

SLAP="./slap"
PASS=0
FAIL=0

green() { printf "\033[32m%s\033[0m" "$1"; }
red()   { printf "\033[31m%s\033[0m" "$1"; }

SDL_FLAGS=$(pkg-config --cflags --libs sdl2 2>/dev/null || echo "-lSDL2")
gcc -std=c99 -O2 $SDL_FLAGS -lm sdl.c -o slap 2>&1

# ── 1. Main test suite (should pass) ──
echo "=== expect.slap ==="
if "$SLAP" tests/expect.slap --test > /dev/null 2>&1; then
    green "PASS"; echo " expect.slap"
    PASS=$((PASS + 1))
else
    red "FAIL"; echo " expect.slap"
    "$SLAP" tests/expect.slap --test 2>&1 | head -5
    FAIL=$((FAIL + 1))
fi

# ── 2. Type error tests (each non-comment line should produce TYPE ERROR) ──
echo ""
echo "=== type-err.slap (each line) ==="
lineno=0
while IFS= read -r line; do
    lineno=$((lineno + 1))
    # skip blank lines and comments
    [[ -z "$line" || "$line" == --* ]] && continue
    label="$line"
    tmpfile=$(mktemp /tmp/slap_test_XXXXXX.slap)
    echo "$line" > "$tmpfile"
    stderr=$("$SLAP" "$tmpfile" --test 2>&1 1>/dev/null || true)
    ec=0; "$SLAP" "$tmpfile" --test > /dev/null 2>/dev/null || ec=$?
    rm -f "$tmpfile"
    if [ "$ec" -ne 0 ] && echo "$stderr" | grep -q "TYPE ERROR"; then
        green "PASS"; echo "  L$lineno: $label"
        PASS=$((PASS + 1))
    elif [ "$ec" -eq 0 ]; then
        red "FAIL"; echo "  L$lineno: $label (expected type error, but passed)"
        FAIL=$((FAIL + 1))
    else
        red "FAIL"; echo "  L$lineno: $label (expected TYPE ERROR, got: $(echo "$stderr" | head -1))"
        FAIL=$((FAIL + 1))
    fi
done < tests/type-err.slap

# ── 3. Runtime panic tests (each non-comment line should produce SLAP PANIC) ──
echo ""
echo "=== panic.slap (each line) ==="
lineno=0
while IFS= read -r line; do
    lineno=$((lineno + 1))
    [[ -z "$line" || "$line" == --* ]] && continue
    label="$line"
    tmpfile=$(mktemp /tmp/slap_test_XXXXXX.slap)
    echo "$line" > "$tmpfile"
    stderr=$("$SLAP" "$tmpfile" --test 2>&1 1>/dev/null || true)
    ec=0; "$SLAP" "$tmpfile" --test > /dev/null 2>/dev/null || ec=$?
    rm -f "$tmpfile"
    if [ "$ec" -ne 0 ] && echo "$stderr" | grep -q "SLAP PANIC"; then
        green "PASS"; echo "  L$lineno: $label"
        PASS=$((PASS + 1))
    elif [ "$ec" -eq 0 ]; then
        red "FAIL"; echo "  L$lineno: $label (expected panic, but passed)"
        FAIL=$((FAIL + 1))
    elif echo "$stderr" | grep -q "TYPE ERROR"; then
        red "FAIL"; echo "  L$lineno: $label (expected PANIC, got TYPE ERROR)"
        FAIL=$((FAIL + 1))
    else
        red "FAIL"; echo "  L$lineno: $label (exit $ec, no SLAP PANIC found)"
        FAIL=$((FAIL + 1))
    fi
done < tests/panic.slap

# ── Summary ──
echo ""
TOTAL=$((PASS + FAIL))
echo "════════════════════════════════════"
printf "  Total: %d  " "$TOTAL"
green "Pass: $PASS"; printf "  "; red "Fail: $FAIL"; echo ""
echo "════════════════════════════════════"

[ "$FAIL" -eq 0 ] || exit 1
