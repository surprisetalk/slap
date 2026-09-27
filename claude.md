# claude.md

Guidance for working in this repo. The language itself is documented in readme.md.

## Build and run

```bash
make slap                        # terminal interpreter (C99, -O3 -flto, -lm)
make slap-sdl                    # SDL2 build (-DSLAP_SDL)
make slap-wasm FILE=prog.slap    # Emscripten build of one program
./slap [--check] [--headless] [args...] < file.slap
```

`--check` type-checks and stops. A plain run type-checks first too, so `--check` before a run is redundant.

## Tests

```bash
make test        # tests/suite.py: every check in parallel, a few seconds
make test-slow   # the Euler problems in SLOW_EULER (tests/suite.py)
make status      # every likely failure mode, scored; 1.0 is the minimum pass
```

- New behaviour gets an assertion in `tests/expect.slap`. expect.slap needs `examples/lib/strings.slap` and `parse.slap` prepended and runs as `./slap hello world <scratch-file>`; suite.py does both.
- A new error gets a case in `tests/errors.slap`: one or more `-- EXPECT: <text>` lines, optional `-- EXPECT-LINE:`/`-- EXPECT-COL:`, then code up to the next EXPECT. A case fails on exit 0, on a signal, or on a timeout.
- A bug gets a failing test first. Check it fails against the old binary; a test that cannot fail is not a regression test.
- `make test-uxn-refs` compares `examples/uxn.slap` pixel for pixel with mkeeter/raven's reference renders (network on first run, cached in `tests/.uxn-refs/`). Run it after touching uxn.slap's Screen path. The 60-frame count matches raven's harness and is load-bearing. `make test-uxn-sweep` checks each ROM's render moves with the frame count as declared. bunnymark is not a benchmark: its population never grows.

## Architecture

One file, `slap.c`: lex → type-check → eval.

- **Lexer** turns source into tokens. Each bracket token records the offset to its partner (`span`), so nothing scans for matches. String literals are UTF-8 bytes.
- **Checker** (`typecheck_tokens`) runs over builtins, prelude and program before anything executes. Type variables are union-find. Every `(...)` body gets a `TupleEffect` (inputs, outputs, scheme). A pre-scan estimates the effect for recursive references, and the real check then sets the counts. A declared signature must match the real check. Linear values are tracked per binding. `BUILTIN_TYPES` holds the primitive signatures and `PRELUDE` the words written in slap.
- **Evaluator**: `build_tuple` turns tokens into a tuple body; `eval_body` runs it. A word resolves at build time to its primitive (`prim_fns`, indexed by symbol) or, for `X must`, to the fused variant (`prim_must_fns`). Other words look up a frame binding when they run.
- **Frames** chain lexical scopes. Each binding owns a heap block of its values. `eval_tuple_scoped` trims the bindings a word makes and puts back caller bindings it rebinds; bindings that returned closures refer to move into a child frame.

## Invariants

- A compound is a flat run of `Value`s on the operand stack with its header last. Primitives move runs in place (`memmove`, `swap_blocks`, `replace_run`). Anything that must step aside while code runs goes on the aux stack (`aux_take`). No C buffer may be sized by data: it overflows the C stack with no message.
- The aux stack never moves, so bodies execute from it; `eval_body` releases what a primitive staged when the primitive returns.
- `dispatch_word` pins the block a word executes from; `frame_bind` never writes into a pinned block.
- Deep-copy (never `VCPY`) when a value is duplicated and its source survives (`dup`, `peek`, `nth`, `of`, `lend`): boxes and dicts are heap objects with one owner.
- A dict cannot be `let`-bound (the binding would alias it). A Box binding is single-use across all its lookups. Tagging a linear value keeps it linear.
- parse.slap's combinators scan with an index and cut once; recursing per character exhausts the C stack.
- Removing a limit exposes whatever it was hiding. Audit every consumer of the newly unbounded value first.
- The `TypeChecker` is `static`: it is megabytes, and `-flto` inlines `typecheck_tokens` into `main`, whose frame lives for the whole run.
- Integer `plus`/`sub`/`mul` wrap at 64 bits. Division by zero, `INT64_MIN -1 div`, and shift counts outside 0-63 are errors.
- Every runtime failure exits nonzero with a message naming what was expected and what arrived. Never clamp, default, or skip to keep running.

## Fallible operations

These return `value ok` on success and `none` (or `payload no`) on failure:

| Operation | Success | Failure |
|---|---|---|
| `pop` | list, then `element ok` | list, then `none` when empty |
| `get` / `peek` / `nth` | `element ok` | `none` out of bounds (`get` consumes the list, `peek` leaves it, `nth` reads a bound name) |
| `set` | `list ok` | `none` out of bounds |
| `at` / `edit` | `value ok` / `record ok` | `none` for a missing key (the record is consumed) |
| `index-of` / `str-find` | `index ok` | `none` |
| `of` (dict lookup) | `value ok` | `key no` |
| `read` / `write` / `ls` | `bytes ok` / `1 ok` / `entries ok` | `path no` |
| `tcp-connect` / `tcp-send` / `tcp-recv` / `tcp-listen` / `tcp-accept` | `socket ok` / `1 ok` / `data ok` / `socket ok` / `client ok` | `message no` |
| `parse-http` | `{'status 'headers 'body} ok` | `message no` |

`take-n`/`drop-n` clamp a count past the end; a negative count is an error. `must` unwraps `'ok` and dies on anything else. Bodies given to `each`, `fold` and `mutate` must leave exactly one value; the runtime checks.
