# claude.md

Guidance for working in this repo. The language itself is documented in readme.md.

## Build and run

```bash
make slap                        # terminal interpreter (C99, -O3 -flto, -lm, OpenSSL)
make slap-sdl                    # SDL2 build (-DSLAP_SDL)
make slap-wasm FILE=prog.slap    # Emscripten build of one program
./slap [--check] [--headless] [--profile] [args...] < file.slap
```

A plain run type-checks first too, so `--check` before a run is redundant. A plain run refuses a program that names a word this build lacks (SDL words in `./slap`, `tcp-*` in wasm) before anything runs, even when the word sits in a branch that never runs; `--check` accepts it.

## Tests

```bash
make test        # tests/suite.py: every check in parallel, a few seconds; builds slap-sdl where SDL2 is installed
make test-slow   # the Euler problems in SLOW_EULER (tests/suite.py)
make status      # every likely failure mode, scored; 1.0 is the minimum pass
```

- New behaviour gets an assertion in `tests/expect.slap`. expect.slap needs `examples/lib/strings.slap` and `parse.slap` prepended and runs as `./slap hello world <scratch-file>`; suite.py does both.
- A new error gets a case in `tests/errors.slap`: one or more `-- EXPECT: <text>` lines, optional `-- EXPECT-LINE:`/`-- EXPECT-COL:`, then code up to the next EXPECT. A case fails on exit 0, on a signal, or on a timeout. EXPECT text must appear in the message: the quoted source line does not count, so a case that ends in `"done-x" fail` cannot pass on a type error in that line.
- A bug gets a failing test first. Check it fails against the old binary; a test that cannot fail is not a regression test.
- Breaker and fuzz rounds run an ASan/UBSan build: `cc -std=c99 -g -O1 -fsanitize=address,undefined -D_POSIX_C_SOURCE=200809L -o slap-asan slap.c -lm $(pkg-config --cflags --libs openssl)`. The runtime trusts the checker, so a checker hole shows up as memory damage or a wrong answer, not as a message. Never link SDL into an ASan build: on macOS, sdl2-compat cannot load SDL3 under ASan, and each run opens two dialogs on the user's screen. Test SDL words with the plain `make slap-sdl` binary and `--headless`.
- macOS ignores `ulimit -v`, so a program that allocates without end fills the disk with swap and takes the machine down. Never run one: no `read` of `/dev/zero`, `/dev/urandom` or a FIFO, and no binary older than a limit on input past that limit. A test of a limit reads a bounded input, such as a file one byte past it. Run unbounded experiments on pc (Linux) under `ulimit -v`.
- `tests/pico8/*.p8` carts are diffed pixel for pixel against `.ref` renders from zepto8. After changing a cart, remake the refs with `tests/pico8/refs.sh` (it runs zepto8 on pc; `tests/pico8/zepto8/build.sh` rebuilds it there). After changing lua.slap's numbers or drawing, run `python3 tests/pico8/fuzz.py 40 <seed>`: random carts against zepto8, printing each that differs. In lua.slap, never call `mget`, `mset` or `hget` with an extra value above the dicts: their `dip`s would set that value aside instead. Bind each value first.
- `tests/tic80/*.lua` carts (`-- script: lua` and `-- frames: N` lines) are diffed pixel for pixel against `.ref` renders from TIC-80's core, built by hand on pc under `~/tic80-ref` at release v1.1.2837 (`tests/tic80/build.sh`, a Lua-only build with `dump.c` as its harness). After changing a cart, remake the refs with `tests/tic80/refs.sh`. lua.slap's tic80 line port needs `_f32` (float32 rounding) because TIC-80's `drawLine` steps in C `float`.
- `PCM` in `tests/suite.py` holds each test file's pcm from the decoder libraries as a length and SHA-256, recorded when the C decoders they replaced still checked them. A change that alters a library's output on purpose records the new hash, and its commit says why.
- `tests/scale.slap` holds library inputs too large to run on every load (past the interpreter's recursion limit); suite.py runs each section, up to a blank line, as its own step, so a section must stand alone.
- `make test-uxn-refs` compares `examples/uxn.slap` pixel for pixel with mkeeter/raven's reference renders (network on first run, cached in `tests/.uxn-refs/`). Run it after touching uxn.slap's Screen path. The 60-frame count matches raven's harness and is load-bearing. `make test-uxn-sweep` checks each ROM's render moves with the frame count as declared. bunnymark is not a benchmark: its population never grows.

## Architecture

One file, `slap.c`: lex → type-check → eval. It includes `vendor/tsf.h` (TinySoundFont, pinned in `vendor/readme.md`) for `render-midi`. Never edit it by hand: replace it whole from upstream, then apply `vendor/tsf.patch`, a fix kept as a patch so it survives an update. Audio files decode in slap: `examples/lib/wav.slap`, `flac.slap` and `mp3.slap`.

A body never changes once built, so `build_tuple` records its element starts once (`body_starts`) and the header's `flags` carry the index from bit 8 up; `eval_run` reads them there, and any code that writes a tuple header must keep those bits. The `run` rules name, as tokens, the primitives that run a body and drop it (`dip`, `pthen`, `if`, `while`, `case`); a primitive that keeps, returns or prints a body it takes must never join them. A profile charges an inlined word for a call that a plain run does not make, so time the plain binary before you optimize one. `fail` (`..a str -> ..b`) dies with its text at the nearest call outside the prelude, so any stack may follow it; use it where a branch needs a free stack.

Architecture, the shell and the fallible-operations table are in readme.md under "## Internals".

## Invariants

- A compound is a flat run of `Value`s on the operand stack with its header last. Primitives move runs in place (`memmove`, `swap_blocks`, `replace_run`). Anything that must step aside while code runs goes on the aux stack (`aux_take`). No C buffer may be sized by data: it overflows the C stack with no message.
- A list whose elements are all ints in 0-255 may be one `VAL_BYTES` slot (`Bytes`: a heap block with one owner, as a dict). String literals, `read`, `tcp-recv`, `push_byte_list`, and `pack_top` (after `[...]`, `each`, `filter`) make one. Every list word takes either form; a word with no packed path calls `unpack_top` or `unpack_under` first. Equal lists may differ in slots, so equality never compares slot counts of lists. A body that holds a string literal has `VF_BYTES`: each pushed copy clones its strings (`clone_bytes_run`), and a staged body frees them (`staged_free_bytes`). A primitive that pops a packed string frees it, so nothing may restore `sp` over it afterwards (`fail` reads its text in place).
- The aux stack never moves, so bodies execute from it; `eval_body` releases what a primitive staged when the primitive returns.
- The checker refuses a `let` of a visible name, so a frame never binds a name twice: `frame_bind` always appends, and no binding is written while a word runs from it.
- Deep-copy (never `VCPY`) when a value is duplicated and its source survives (`dup`, `peek`, `nth`, `of`, `lend`): boxes and dicts are heap objects with one owner.
- A binding that holds a dict owns it (`Binding.heap`): a lookup deep-copies it, and rebinding, trimming or restoring the binding frees it; a closure's child frame takes it over. The checker refuses a binding of a box or socket. Tagging a linear value keeps it linear.
- parse.slap's combinators scan with an index and cut once; recursing per character exhausts the C stack.
- A lookup of a bound list deep-copies it, and `drop-n`/`take-n` copy what they keep, so a library loop that does either per element is O(n²). Keep a growing list on the stack, and cut once after a search.
- Removing a limit exposes whatever it was hiding. Audit every consumer of the newly unbounded value first.
- Integer `plus`/`sub`/`mul` wrap at 64 bits. Division by zero, `INT64_MIN -1 div`, and shift counts outside 0-63 are errors.
- Every runtime failure exits nonzero with a message naming what was expected and what arrived. Never clamp, default, or skip to keep running.
- The runtime trusts the checker: it never checks what the checker proves (types, stack depth, record keys, case coverage, a body's output count). A check that only catches a checker bug does not belong in a primitive.
- A library returns `'no` for input it cannot read. `must` stays only after a check of the same condition, or on a structure the program built.
- Reading element `i` of a list whose elements span several slots walks the list, so a loop of `nth`/`get` over records or tagged values is O(n²). Use `each`, `filter`, `fold` or `index-of`.
