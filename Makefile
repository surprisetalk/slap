CC = cc
CFLAGS = -std=c99 -Wall -Wextra -O3 -flto -D_POSIX_C_SOURCE=200809L
slap: slap.c
	$(CC) $(CFLAGS) -o slap slap.c -lm
UNAME_S := $(shell uname -s)
SDL_EXTRA :=
ifeq ($(UNAME_S),Darwin)
SDL_EXTRA := -lobjc
endif
slap-sdl: slap.c
	$(CC) $(CFLAGS) -DSLAP_SDL -o slap-sdl slap.c -lm $(SDL_EXTRA) $(shell sdl2-config --cflags --libs 2>/dev/null || echo "-lSDL2")
# GROWABLE_ARRAYBUFFERS=0: with ALLOW_MEMORY_GROWTH, emscripten >=6 backs the
# heap with a *resizable* ArrayBuffer, and TextDecoder.decode() refuses those.
# Startup throws before the main loop runs and the canvas stays blank.
slap-wasm: slap.c shell.html
	@if [ -z "$(FILE)" ]; then echo "usage: make slap-wasm FILE=program.slap"; exit 1; fi
	@if [ ! -f "$(FILE)" ]; then echo "slap-wasm: no such program: $(FILE)" >&2; exit 1; fi
	@command -v emcc > /dev/null || { \
	    echo "slap-wasm: emcc not found on PATH." >&2; \
	    echo "  The WASM build needs Emscripten. Install and activate emsdk:" >&2; \
	    echo "    git clone https://github.com/emscripten-core/emsdk && cd emsdk" >&2; \
	    echo "    ./emsdk install latest && ./emsdk activate latest" >&2; \
	    echo "    source ./emsdk_env.sh" >&2; \
	    exit 1; }
	@NAME=$$(basename $(FILE) .slap); \
	sed "s/SLAP_NAME/$$NAME/g" shell.html > .shell_$$NAME.html; \
	emcc -std=c99 -O3 -D_POSIX_C_SOURCE=200809L -DSLAP_SDL -DSLAP_WASM -sUSE_SDL=2 \
	    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=4194304 -sGROWABLE_ARRAYBUFFERS=0 \
	    --embed-file $(FILE)@program.slap \
	    -o $$NAME.html slap.c -lm \
	    --shell-file .shell_$$NAME.html; \
	status=$$?; \
	rm -f .shell_$$NAME.html; \
	[ $$status -eq 0 ] || { echo "slap-wasm: emcc failed (exit $$status); no output written" >&2; exit $$status; }; \
	echo "wrote $$NAME.html $$NAME.js $$NAME.wasm"
clean:
	rm -f slap slap-sdl *.wasm *.js
	@find . -maxdepth 1 -name '*.html' ! -name 'shell.html' -delete
test: slap slap-sdl
	@python3 tests/suite.py
# Euler problems that take seconds each; the list is SLOW_EULER in tests/suite.py.
test-slow: slap
	@python3 tests/suite.py slow
# Every likely failure mode, scored so that 1.0 is the minimum pass.
status: slap
	@python3 tests/suite.py status
# Outside `test`: the first run downloads ROMs and reference renders from GitHub.
test-uxn-refs: slap
	@python3 tests/run_uxn_refs.py

test-uxn-sweep: slap
	@python3 tests/run_uxn_refs.py --sweep

# Prints uxn instructions/sec; never fails.
bench-uxn: slap
	@python3 tests/run_uxn_refs.py --bench
.PHONY: clean test test-slow status test-uxn-refs test-uxn-sweep bench-uxn
