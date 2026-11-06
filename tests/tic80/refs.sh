#!/bin/sh
# Remakes each tests/tic80/*.ref with TIC-80 v1.1.2837's core on pc. A ref is
# the screen after the frames a cart's "-- frames: N" line names: trace lines,
# then 136 lines of 240 hex digits, one palette index per pixel. TIC() runs once
# per frame. tests/tic80/build.sh says how pc builds the dumper.
set -eu
cd "$(dirname "$0")"
for cart in *.lua; do
  n=$(sed -n 's/^-- frames: \([0-9]*\)$/\1/p' "$cart")
  [ -n "$n" ] || { echo "$cart: no '-- frames: N' line" >&2; exit 1; }
  scp -q "$cart" "taylor@100.97.122.7:tic80-ref/$cart"
  # shellcheck disable=SC2029 # $cart and $n expand here on purpose; the remote gets plain words.
  ssh taylor@100.97.122.7 "cd ~/tic80-ref && ./dump $cart $n" > "${cart%.lua}.ref"
  [ "$(wc -l < "${cart%.lua}.ref")" -ge 136 ] || { echo "$cart: tic80 gave no screen" >&2; exit 1; }
done
