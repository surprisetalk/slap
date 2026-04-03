# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
make slap          # Terminal interpreter (C99, -O3 -flto, links -lm)
make slap-sdl      # SDL graphics build (adds -DSLAP_SDL, links SDL2)
make slap-wasm FILE=prog.slap  # Emscripten/WASM build (embeds .slap file, outputs .html/.js/.wasm)
make test           # Run all test suites
make clean          # Remove binaries
```

CLI: `./slap [--check] [--headless] [args...] < file.slap`
- `--check` — type-check only, no execution
- `--headless` — (SDL build) run without a window, tick loop continues indefinitely
- Positional args available via `args` primitive; `isheadless` and `cwd` also available

## Tests

`make test` runs five checks in order:
1. `./slap < tests/expect.slap` — integration tests (assert-based, halts on failure)
2. `./slap --check < tests/type.slap` — type system tests
3. `./slap < tests/type.slap > /dev/null` — execute type tests
4. `./slap --check < tests/expect.slap` — type-check the integration tests
5. `python3 tests/run_panic.py` + `python3 tests/run_type_errors.py` — verify expected errors

Tests use `assert` (halts on first failure). Python scripts validate that specific inputs produce expected error messages.

## Architecture

Single-file C interpreter (`slap.c`). Pipeline: **lex → typecheck → eval**.

**Two-phase model**: Type-check ALL code (builtins + prelude + user) first, then execute only user code. The prelude (library functions written in Slap) is executed before user code but after type-checking.

### Key subsystems in slap.c

- **Lexer** (`lex`): Source → tokens. Token types: INT, FLOAT, SYM, WORD, STRING, brackets, EOF.
- **Type checker** (`typecheck_tokens` → `tc_process_range`): Union-find type inference, effect system (consumed/produced stack slots), linear value tracking. Type variables use path-compressed union-find for unification.
- **Evaluator** (`eval` → `build_tuple` → `eval_body`): Tokens → compound values (tuples), then stack-machine execution. `dispatch_word` resolves names via frame lookup then primitive table.
- **Frames**: Lexical scope chain with refcounting. Closures capture their defining frame. `def` bindings auto-execute tuples on lookup; `let` bindings push values.
- **Primitives**: ~85 C functions registered via `prim_register`. Macros `ARITH2`, `FLOAT1`, `CMP2` generate families of math/comparison ops.
- **Prelude**: ~65 derived definitions in Slap itself (embedded string in slap.c). Loaded at startup before user code.
- **Recur**: `'name recur (body) def` enables self-referencing definitions for recursion.

### Type system

Two categories of types:
- **Stackable** (copyable): Int, Float, Symbol, Tuple, Record, List, String. Support `dup`/`drop`.
- **Linear**: Box only. Must be consumed exactly once via `free`, `lend`, `mutate`, or `clone`.

`lend` borrows a stackable snapshot from a Box. No escape constraints needed because borrowed values are always stackable.

### def vs let

- `'name val def` — name then value. Auto-executes tuples on lookup. For function definitions.
- `val 'name let` — value then name. Pushes value on lookup. For binding stack arguments in function bodies.

### SDL graphics (optional)

Compiled with `-DSLAP_SDL`. 640×480 canvas, 2-bit grayscale. Primitives: `clear`, `pixel`, `fill-rect`, `on`, `show`. Event callbacks for mouse/keyboard.
