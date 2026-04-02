CC = cc
CFLAGS = -std=c99 -Wall -Wextra -O3 -flto -D_POSIX_C_SOURCE=200809L
slap: slap.c
	$(CC) $(CFLAGS) -o slap slap.c -lm
slap-sdl: slap.c
	$(CC) $(CFLAGS) -DSLAP_SDL -o slap-sdl slap.c -lm $(shell sdl2-config --cflags --libs 2>/dev/null || echo "-lSDL2")
slap-wasm: slap.c shell.html
	@if [ -z "$(FILE)" ]; then echo "usage: make slap-wasm FILE=program.slap"; exit 1; fi
	@NAME=$$(basename $(FILE) .slap); \
	sed "s/SLAP_NAME/$$NAME/g" shell.html > .shell_$$NAME.html; \
	emcc -std=c99 -O3 -D_POSIX_C_SOURCE=200809L -DSLAP_SDL -DSLAP_WASM -sUSE_SDL=2 \
	    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=4194304 \
	    --embed-file $(FILE)@program.slap \
	    -o $$NAME.html slap.c -lm \
	    --shell-file .shell_$$NAME.html; \
	rm -f .shell_$$NAME.html; \
	echo "wrote $$NAME.html $$NAME.js $$NAME.wasm"
clean:
	rm -f slap slap-sdl *.wasm
test: slap
	@./slap tests/expect.slap
	@./slap --check tests/type.slap
	@./slap tests/type.slap > /dev/null
	@./slap --check tests/expect.slap
	@python3 tests/run_panic.py
	@python3 tests/run_type_errors.py
	@echo "All test suites passed."
.PHONY: clean test
