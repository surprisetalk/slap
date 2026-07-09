#!/usr/bin/env bash
# Classify each probe in probes.slap and compare against its declared expectation.
# Run from the repo root.
#
# Classifications:
#   TYPECHECK_REJECT  --check exits nonzero (the type system caught it)
#   PANIC             --check passes; running exits nonzero, gracefully
#   CLEAN_RUN         --check passes and running exits zero
#   CRASH(sig N)      killed by a signal — segfault, abort. Never expected.
#   TIMEOUT(phase)    hung until the alarm fired. Never expected.
#
# CRASH and TIMEOUT are distinct on purpose: they are nonzero exits, and folding
# them into PANIC would let a new segfault or a new infinite loop pass as a
# graceful panic. They match no expectation, so they always fail the suite.
#
# A mismatch is a soundness regression: a probe that moves from TYPECHECK_REJECT
# to CLEAN_RUN means the checker went blind.
#
# Probe format, in probes.slap:
#   -- EXPECT: <classification>
#   -- KNOWN-GAP: <why this classification is wrong but currently accepted>
#   -- GAP-TARGET: <the classification the docs promise>
#   <code, until the next EXPECT>

set -u

PROBES="tests/adversarial/probes.slap"
SLAP="./slap"
TIMEOUT=5

[ -x "$SLAP" ] || { echo "adversarial: no $SLAP binary; run 'make slap' first" >&2; exit 1; }
[ -f "$PROBES" ] || { echo "adversarial: cannot find $PROBES (cwd: $PWD)" >&2; exit 1; }
command -v perl > /dev/null || {
  echo "adversarial: perl not found; it provides the per-probe timeout." >&2
  echo "  Without a timeout every probe would exit nonzero and be misread as" >&2
  echo "  TYPECHECK_REJECT, so the suite would pass for the wrong reason." >&2
  exit 1; }

# macOS has no coreutils `timeout`. perl forks, waits, and kills on alarm, then
# exits normally with 142 -- if it let SIGALRM kill the child directly, bash
# would print "Alarm clock" to stderr and we would lose the child's exit code.
# A child killed by a signal is reported as 128+signo, so a segfault stays
# visible as CRASH rather than being flattened into PANIC.
TIMED_OUT=142

run_limited() {
  perl -e '
    my $t = shift;
    my $pid = fork();
    die "fork failed: $!" unless defined $pid;
    if (!$pid) { exec @ARGV; exit 127; }
    $SIG{ALRM} = sub { kill "KILL", $pid; waitpid($pid, 0); exit 142; };
    alarm $t;
    waitpid($pid, 0);
    alarm 0;
    my $st = $?;
    exit($st & 127 ? 128 + ($st & 127) : $st >> 8);
  ' "$TIMEOUT" "$@" > /dev/null 2>&1
}

# Map a raw exit status to a classification for one phase, or "" if it exited 0.
verdict() {
  local status="$1" phase="$2" reject="$3"
  [ "$status" -eq 0 ] && { echo ""; return 0; }
  [ "$status" -eq "$TIMED_OUT" ] && { echo "TIMEOUT($phase)"; return 0; }
  [ "$status" -gt 128 ] && { echo "CRASH(sig $((status - 128)))"; return 0; }
  echo "$reject"
}

classify() {
  local src="$1" status out
  printf '%s\n' "$src" | run_limited "$SLAP" --check
  status=$?
  out=$(verdict "$status" "--check" TYPECHECK_REJECT)
  [ -n "$out" ] && { echo "$out"; return 0; }

  printf '%s\n' "$src" | run_limited "$SLAP"
  status=$?
  out=$(verdict "$status" "run" PANIC)
  [ -n "$out" ] && { echo "$out"; return 0; }
  echo CLEAN_RUN
}

passed=0
failed=0
gaps=0
expect=""
gap=""
target=""
code=""
probe=0

check() {
  [ -n "$expect" ] || return 0
  probe=$((probe + 1))

  # An EXPECT with no code is a probe that silently tests nothing.
  if [ -z "$code" ]; then
    failed=$((failed + 1))
    echo "  probe $probe: '-- EXPECT: $expect' has no code beneath it" >&2
    return 0
  fi

  local got
  got=$(classify "$code")

  if [ "$got" = "$expect" ]; then
    passed=$((passed + 1))
    if [ -n "$gap" ]; then
      gaps=$((gaps + 1))
      echo "  known gap (probe $probe, classified $got): $gap"
    fi
    return 0
  fi

  failed=$((failed + 1))
  # A KNOWN-GAP probe that reaches its documented target is a fix, not a regression.
  if [ -n "$target" ] && [ "$got" = "$target" ]; then
    echo "  probe $probe: KNOWN-GAP now reaches its target ($target). This is a FIX." >&2
    echo "    Retire the gap: set '-- EXPECT: $target' and delete the KNOWN-GAP/GAP-TARGET notes." >&2
  else
    echo "  probe $probe: expected $expect, got $got" >&2
  fi
  echo "    code: $(printf '%s' "$code" | tr '\n' ';')" >&2
}

while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in
    "-- EXPECT:"*)
      check
      expect=$(printf '%s' "$line" | sed 's/^-- EXPECT: *//' | tr -d ' \r')
      gap=""
      target=""
      code=""
      ;;
    "-- KNOWN-GAP:"*)
      [ -n "$gap" ] || gap=$(printf '%s' "$line" | sed 's/^-- KNOWN-GAP: *//')
      ;;
    "-- GAP-TARGET:"*)
      target=$(printf '%s' "$line" | sed 's/^-- GAP-TARGET: *//' | tr -d ' \r')
      ;;
    "--"*|"") ;;
    *)
      if [ -z "$code" ]; then code="$line"; else code="$code
$line"; fi
      ;;
  esac
done < "$PROBES"
check

if [ "$failed" -gt 0 ]; then
  echo "adversarial: $passed passed, $failed FAILED" >&2
  exit 1
fi
if [ "$gaps" -gt 0 ]; then
  echo "adversarial: $passed probes classified as expected, $gaps known gap(s) above"
  exit 0
fi
echo "adversarial: $passed probes classified as expected"
