#!/bin/sh
# Remakes each tests/pico8/*.ref with zepto8 on pc, which matches pico8 pixel
# for pixel. A ref is the screen after the frames a cart's "-- frames: N" line
# names: 128 lines of 128 hex digits, raw screen memory. zepto8 steps at 60 Hz
# and runs _update every other step, so a cart without _update60 takes 2N steps.
# tests/pico8/zepto8/build.sh says how pc builds the dumper.
set -eu
cd "$(dirname "$0")"
for cart in *.p8; do
  n=$(sed -n 's/^-- frames: \([0-9]*\)$/\1/p' "$cart")
  [ -n "$n" ] || { echo "$cart: no '-- frames: N' line" >&2; exit 1; }
  grep -q '_update60' "$cart" || n=$((n * 2))
  scp -q "$cart" "taylor@100.97.122.7:scratch/zepto8/$cart"
  ssh taylor@100.97.122.7 "cd ~/scratch/zepto8 && ./dump ~/scratch/zepto8/$cart $n" > "${cart%.p8}.ref"
  [ "$(wc -l < "${cart%.p8}.ref")" -eq 128 ] || { echo "$cart: zepto8 gave no screen" >&2; exit 1; }
done
