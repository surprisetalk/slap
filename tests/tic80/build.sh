#!/bin/sh
# Builds ~/tic80-ref/dump on pc: TIC-80's core with only Lua, no SDL front end,
# plus tests/tic80/dump.c. Pinned to release v1.1.2837. On pc, once:
#   mkdir -p ~/tic80-ref && cd ~/tic80-ref
#   git clone --depth 1 --branch v1.1.2837 --recurse-submodules https://github.com/nesbox/TIC-80 src
# Then, from the repo: tests/tic80/build.sh
# The core is compiled by hand: the CMake build wants every script language.
set -eu
cd "$(dirname "$0")"
scp -q dump.c taylor@100.97.122.7:tic80-ref/dump.c
ssh taylor@100.97.122.7 'sh -s' <<'REMOTE'
set -eu
cd ~/tic80-ref
[ "$(git -C src describe --tags)" = v1.1.2837 ] || { echo "src is not v1.1.2837" >&2; exit 1; }
S=src; V=src/vendor
LUA=$(ls $V/lua/l*.c | grep -v 'lua\.c$\|luac\.c$')
gcc -std=gnu11 -O1 -w -DTIC_BUILD_WITH_LUA=1 -DLUA_COMPAT_5_2 -I$S/include -I$S/src -I$V/lua -I$V/blip-buf -I$V/zlib \
  -o dump dump.c $S/src/core/core.c $S/src/core/draw.c $S/src/core/io.c $S/src/core/languages.c $S/src/core/sound.c $S/src/tic.c $S/src/cart.c $S/src/tools.c $S/src/zip.c $S/src/tilesheet.c $S/src/api/lua.c \
  $LUA $V/blip-buf/blip_buf.c \
  $V/zlib/adler32.c $V/zlib/compress.c $V/zlib/crc32.c $V/zlib/deflate.c $V/zlib/inflate.c $V/zlib/infback.c \
  $V/zlib/inftrees.c $V/zlib/inffast.c $V/zlib/trees.c $V/zlib/uncompr.c $V/zlib/zutil.c -lm
REMOTE
