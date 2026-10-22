#!/bin/sh
# Builds dump.bin, zepto8 (github.com/samhocevar/zepto8) driven headless:
# `dump cart.p8 steps` prints the raw screen after that many 60 Hz steps.
# On pc, once:
#   mkdir -p ~/scratch/zepto8 && cd ~/scratch/zepto8
#   cp <repo>/tests/pico8/zepto8/{dump.cpp,shell.nix,build.sh} .
#   git clone --recurse-submodules --depth 1 https://github.com/samhocevar/zepto8 src
#   cd src && nix-shell ../shell.nix --run ./bootstrap
#   nix-shell ../shell.nix --run './configure CXXFLAGS="-O2 -include cstring -include utility" CFLAGS="-O2 -include string.h"'
#   Patch for clang (GCC 16 refuses the code): src/pico8/vm.cpp's line with abs(..) and std::min(..) takes
#   std::abs and std::min<double>; src/pico8/code.cpp needs #include <climits>.
#   nix-shell ../shell.nix --run 'make -k -j14 -C src/3rdparty/lolengine/src'
#   cp ../dump.cpp src/ && nix-shell ../shell.nix --run ~/scratch/zepto8/build.sh
# zepto8 reads pico8/bios.p8 from its working directory, so ./dump (below) runs dump.bin from src/src.
set -e
cd ~/scratch/zepto8/src/src
CC=$(make -n ../z8tool 2>&1 | grep "^echo.*z8tool.cpp" | sed "s/^echo [^;]*;//; s/-MT [^ ]* -MD -MP -MF [^ ]*//; s/-c -o .*//")
for f in dump textfile filter; do eval "$CC -c -o $f.o $f.cpp"; done
make -C . libzepto8.la >/dev/null
LINK=$(make -n ../z8tool 2>&1 | grep "^echo.*CXXLD" | sed "s/^echo [^;]*;//; s/-o ..\/z8tool .*//")
eval "$LINK -o ../../dump.bin dump.o textfile.o filter.o libzepto8.la -lm -lpthread"
printf '#!/bin/sh\nc=$(realpath "$1") || exit 1\ncd ~/scratch/zepto8/src/src && exec ../../dump.bin "$c" "$2"\n' > ../../dump
chmod +x ../../dump
