CC = cc
CFLAGS = -std=c99 -Wall -Wextra -O3 -flto -D_POSIX_C_SOURCE=200809L
# `cat a b c | ./slap` reports only ./slap's status, so a missing input file
# would let the suite pass having tested nothing. pipefail closes that hole.
SHELL := /bin/bash
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
LIBS := icn chr nmt tga gly ulz parse ufx strings json xml rss
# Inputs the suite would otherwise skip silently: a missing expect.slap just
# means `cat` prints nothing and ./slap happily type-checks the rest.
FIXTURES := tests/expect.slap tests/type.slap tests/panic.slap tests/type_errors.slap \
            tests/adversarial/probes.slap tests/adversarial/run.sh \
            tests/run_panic.py tests/run_type_errors.py tests/run_euler.py tests/run_wiki.py tests/run_kv.py \
            examples/wiki.slap examples/wiki-pages/Home.txt examples/kv-server.slap examples/kv-client.slap \
            examples/chip8.slap shell.html
check-refs:
	@{ for n in $(LIBS); do echo examples/lib/$$n.slap; done; \
	   for f in $(FIXTURES); do echo $$f; done; \
	   grep -ohE 'examples/lib/[a-z0-9_-]+\.slap' Makefile readme.md claude.md tests/*.py 2>/dev/null; \
	 } | sort -u | while read -r f; do \
	    [ -f "$$f" ] || echo "  referenced by the build or docs, but does not exist: $$f"; \
	done > .check-refs.out 2>&1; \
	if [ -s .check-refs.out ]; then \
	    cat .check-refs.out >&2; rm -f .check-refs.out; \
	    echo "" >&2; \
	    echo "check-refs: the build or docs name a file that is not on disk." >&2; \
	    echo "  Either create it, or stop referencing it. A documented file that was" >&2; \
	    echo "  never committed breaks 'make test' for everyone but its author." >&2; \
	    exit 1; \
	fi; \
	rm -f .check-refs.out; \
	echo "check-refs: every referenced file is present."
test: slap check-refs
	@set -o pipefail; cat examples/lib/strings.slap examples/lib/parse.slap tests/expect.slap | ./slap
	@./slap --check < tests/type.slap
	@./slap < tests/type.slap > /dev/null
	@set -o pipefail; cat examples/lib/strings.slap examples/lib/parse.slap tests/expect.slap | ./slap --check
	@python3 tests/run_panic.py
	@python3 tests/run_type_errors.py
	@echo 'args len 2 eq assert  args 0 get must "hello" eq assert  args 1 get must "world" eq assert' | ./slap hello world
	@echo 'args len 0 eq assert' | ./slap
	@rm -f _test_fs.bin
	@python3 tests/run_euler.py
	@python3 tests/run_wiki.py
	@python3 tests/run_kv.py
	@./slap --check < examples/chip8.slap > /dev/null
	@set -o pipefail; ./slap --headless < examples/chip8.slap | grep -q chip8-selftest-ok && echo "chip8: opcode self-test passed"
	@for f in icn chr nmt tga gly ulz parse; do ./slap < examples/lib/$$f.slap > /dev/null && ./slap --check < examples/lib/$$f.slap || exit 1; done
	@set -o pipefail; for combo in "icn ufx" "strings parse json" "strings parse xml" "strings parse xml rss"; do files=$$(echo $$combo | sed 's|[^ ]*|examples/lib/&.slap|g'); cat $$files | ./slap > /dev/null && cat $$files | ./slap --check || exit 1; done
	@bash tests/adversarial/run.sh
	@echo "All test suites passed."
.PHONY: clean test check-refs
