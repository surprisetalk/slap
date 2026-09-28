# claude.md

Guidance for working in this repo. The language itself is documented in readme.md.

## Build and run

```bash
make slap                        # terminal interpreter (C99, -O3 -flto, -lm)
make slap-sdl                    # SDL2 build (-DSLAP_SDL)
make slap-wasm FILE=prog.slap    # Emscripten build of one program
./slap [--check] [--headless] [--profile] [args...] < file.slap
```

`--check` type-checks and stops. A plain run type-checks first too, so `--check` before a run is redundant. `--profile` prints folded stacks (`word;word nanoseconds`) to stderr at exit.

## Tests

```bash
make test        # tests/suite.py: every check in parallel, a few seconds
make test-slow   # the Euler problems in SLOW_EULER (tests/suite.py)
make status      # every likely failure mode, scored; 1.0 is the minimum pass
```

- New behaviour gets an assertion in `tests/expect.slap`. expect.slap needs `examples/lib/strings.slap` and `parse.slap` prepended and runs as `./slap hello world <scratch-file>`; suite.py does both.
- A new error gets a case in `tests/errors.slap`: one or more `-- EXPECT: <text>` lines, optional `-- EXPECT-LINE:`/`-- EXPECT-COL:`, then code up to the next EXPECT. A case fails on exit 0, on a signal, or on a timeout.
- A bug gets a failing test first. Check it fails against the old binary; a test that cannot fail is not a regression test.
- `tests/scale.slap` holds library inputs too large to run on every load (past the interpreter's recursion limit); suite.py runs it once.
- `make test-uxn-refs` compares `examples/uxn.slap` pixel for pixel with mkeeter/raven's reference renders (network on first run, cached in `tests/.uxn-refs/`). Run it after touching uxn.slap's Screen path. The 60-frame count matches raven's harness and is load-bearing. `make test-uxn-sweep` checks each ROM's render moves with the frame count as declared. bunnymark is not a benchmark: its population never grows.

## Architecture

One file, `slap.c`: lex → type-check → eval.

- **Lexer** turns source into tokens. Each bracket token records the offset to its partner (`span`), so nothing scans for matches. String literals are UTF-8 bytes.
- **Checker** (`typecheck_tokens`) runs over builtins, prelude and program before anything executes. Type variables are union-find. Every `(...)` body gets a `TupleEffect` (inputs, outputs, scheme). A pre-scan estimates the effect. Inside its own body a recursive word is code of unknown effect, so `if` takes its effect from a branch that returns with a known effect; `x no must`, `none must` and `halt` never return. `case` takes its effect from its clauses. Branches and clauses must agree; the checker infers the tags a value can carry (`UnionDef.inferred`) to know when a `case` default can run. A word used before its definition must be declared first. The real check then sets the counts. A declared signature must match the real check. Linear values are tracked per binding. `BUILTIN_TYPES` holds the primitive signatures and `PRELUDE` the words written in slap.
- **Records**: `at` and `edit` never fail, so the checker proves every key they read. A record type's root tvar carries `row` (keys it has → field tvars), `rest` (a record whose keys it also has), `need` (keys read from it while it stood for a caller's value; every caller must pass them, and a merge never drops them) and `open` (a body input). A call's copy of an input is `param`: meeting its argument makes it that argument. A tagged type's `vrow` maps each tag to its payload type; `must` reads the `'ok` one. `rec_has` reads a key, `rec_flow` checks a caller's record against a `need`, `row_join` decides what a merged type knows. Higher-order words run their bodies as trials (`tc_trial`) on the real values and meet the results (`if`, `case`, loops, `each`, `fold`, `on`/`show`, `lend`/`mutate`). Code of unknown effect makes the checker forget the records below it (`tc_forget_below`); a body whose input needs keys and escapes where no trial runs it is an error (`tc_escape`). `'k at must` is the unchecked old form: new code writes `'k at`, and `make status` counts what is left.
- **Evaluator**: `build_tuple` turns tokens into a tuple body; `eval_body` runs it. A word resolves at build time to its primitive (`prim_fns`, indexed by symbol) or, for `X must`, to the fused variant (`prim_must_fns`). Other words look up a frame binding when they run. Under `--profile` primitives stay unresolved, so every word reaches `prof_dispatch`. `[...]` and `{...}` literals are evaluated by `build_tuple`, once, when the program is read. `(then) (else) if` with both branches written in place runs the chosen branch from the body itself instead of copying both; `make status` times a 600 KB feed, which depends on it.
- **Frames** chain lexical scopes. A frame is `captured` once a tuple made in it may refer to it; after that, bindings a body makes outlive the body. Each binding owns a heap block of its values. `eval_tuple_scoped` trims the bindings a word makes and puts back caller bindings it rebinds; bindings that returned closures refer to move into a child frame.

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
- A library returns `'no` for input it cannot read. `must` stays only after a check of the same condition, or on a structure the program built.
- Reading element `i` of a list whose elements span several slots walks the list, so a loop of `nth`/`get` over records or tagged values is O(n²). Use `each`, `filter`, `fold` or `index-of`.

## Fallible operations

These return `value ok` on success and `none` (or `payload no`) on failure:

| Operation | Success | Failure |
|---|---|---|
| `pop` | list, then `element ok` | list, then `none` when empty |
| `get` / `peek` / `nth` | `element ok` | `none` out of bounds (`get` consumes the list, `peek` leaves it, `nth` reads a bound name) |
| `set` | `list ok` | `none` out of bounds |
| `index-of` / `str-find` | `index ok` | `none` |
| `of` (dict lookup) | `value ok` | `key no` |
| `read` / `write` / `ls` | `bytes ok` / `1 ok` / `entries ok` | `path no` |
| `tcp-connect` / `tcp-send` / `tcp-recv` / `tcp-listen` / `tcp-accept` | `socket ok` / `1 ok` / `data ok` / `socket ok` / `client ok` | `message no` |
| `parse-http` | `{'status 'headers 'body} ok` | `message no` |

`at` and `edit` are not fallible: the checker proves the key. `take-n`/`drop-n` clamp a count past the end; a negative count is an error. `must` unwraps `'ok` and dies on anything else. Bodies given to `each`, `fold` and `mutate` must leave exactly one value; the runtime checks.
