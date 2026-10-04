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
- **Checker** (`infer_program`) runs over the prelude and the program before anything executes. A type is a term in one pool (`Ty`); `ty_unify` binds variables with union-find, and a structural link waits until the whole chain unifies (`ty_links`). Kinds: values (`K_INT` to `K_TAG`), stacks (`K_SVAR`, `K_SNIL`, `K_SCONS`), record rows (`K_RVAR`, `K_RNIL`, `K_REXT`, with fields `K_PRE`/`K_ABS`), labels (`K_LSYM`, and `K_LVAR` for a symbol's label) and tag sets (`K_TVAR`, `K_TNIL`, `K_TEXT`). A body is `K_FN`, from the stack it takes to the stack it leaves. `TYPES` holds the primitives' types (`( ins -> outs )`; `--` would start a comment); the prelude's words are inferred like the program's. Levels decide generalization: a word defined from a body written in place is generic, a value is not; a bound body's stack rest is made fresh at each use when nothing else holds it (`ty_bound_rest`), since a caller's body may read below its inputs. A waiting case can still tie the rest to more, so once a word's cases settle, `ty_gens_settle` refuses a freshened rest that gained structure or that a caller's stack reaches, and passes one tied outside the word to the scope outside. A signature is rigid (`ty_rigid`), and a variable that would escape its scope is an error (`ty_occurs` returns 2). A body type in a slot runs on the word's own rest (`ty_slot_rest`). Each recursive call gets a fresh stack rest, made at its word's level (`ty_rec_owner`). Every tag but `'ok`/`'no` has one payload type program-wide (`ty_tag_payload`); `'ok`/`'no` build `K_RES`. A `case` whose default may run waits (`TyLater`) until its word's type is known; `ty_cases_settle` settles the waiting cases to a fixpoint, keeps a set a caller can reach open, checks a local set for coverage, and closes every set at the end of the program. Ownership is the `copy` protocol (`P_COPY`): `TYPES` asks for it where a primitive drops or duplicates a value or a container holds one, and `let`, literals, a `case` default, `show` and the program's last stack ask for it too. No word ends a program early: a word that holds a box aside while it runs a caller's body cannot see an exit inside that body, so the box would leak. `show` exits when its window closes, so it asks copy of the whole stack. The stack below the bodies of `each`, `fold`, `edit`, `mutate` and `lend` is sealed (`..!r` in `TYPES`). A pending forward declaration forbids running code in its scope (`ty_runs`), since that code may call it. Code in a literal sees only top-level names (`ty_lit_depth`).
- **Records** are rows of present or absent fields: `into` takes `{'k 'p field | 'r}` and leaves `{'k 't | 'r}`, since the runtime replaces a key it finds; `at` and `edit` need `'k` present. `at`, `into` and `edit` read the symbol written right before them (forms in `ty_range`, not `TYPES` entries). Record `cat` needs every field of the right record known, and the runtime puts each one into the left record. `at` and `edit` never fail at runtime: a missing key there is a checker bug and dies saying so (`KEY_MISSING`).
- **Evaluator**: `build_tuple` turns tokens into a tuple body; `eval_body` runs it. A word resolves at build time to its primitive (`prim_fns`, indexed by symbol) or, for `X must`, to the fused variant (`prim_must_fns`). Other words look up a frame binding when they run: a binding is a word (`Binding.word`) when a body was written right before its `'name let`, and a lookup runs a word and pushes any other value. Under `--profile` primitives stay unresolved, so every word reaches `prof_dispatch`. `[...]` and `{...}` literals are evaluated by `build_tuple`, once, when the program is read. `(then) (else) if` with both branches written in place runs the chosen branch from the body itself instead of copying both; `make status` times a 600 KB feed, which depends on it.
- **Frames** are lexical. Each run of a body that binds names (`VF_BINDS` on its tuple header, set by `build_tuple`) gets a frame from a pool whose parent is the frame the body was made in; top-level code binds straight into the global frame. A frame counts what keeps it (`refs`): its run, the tuples that close over it (`deep_copy_values`/`deep_free_values` count them, and a lookup's copy counts again) and its child frames. Tuples in its own bindings do not count, so a word's local words do not keep it. At zero it waits on a list, and one loop releases the bindings of every waiting frame, so a chain of closures frees without C recursion. Then the frame goes back to the pool. `cat` of two closures over different frames makes a frame that binds them as the words `cat-left` and `cat-right`. A primitive's `POP_BODY` bodies, and `case`'s clause list, lose their reference when it returns (`staged`); one that holds a dict is freed whole. `case` clauses run in the frame that runs `case`. A cycle (a nested body's closure stored back in an outer frame) is never freed. Each binding owns a heap block of its values. The hot lookup and bind paths are forced inline: a call there costs about a tenth of the run time.

## Invariants

- A compound is a flat run of `Value`s on the operand stack with its header last. Primitives move runs in place (`memmove`, `swap_blocks`, `replace_run`). Anything that must step aside while code runs goes on the aux stack (`aux_take`). No C buffer may be sized by data: it overflows the C stack with no message.
- The aux stack never moves, so bodies execute from it; `eval_body` releases what a primitive staged when the primitive returns.
- `dispatch_word` pins the block a word executes from; `frame_bind` never writes into a pinned block.
- Deep-copy (never `VCPY`) when a value is duplicated and its source survives (`dup`, `peek`, `nth`, `of`, `lend`): boxes and dicts are heap objects with one owner.
- A binding that holds a dict owns it (`Binding.heap`): a lookup or `quote` deep-copies it, and rebinding, trimming or restoring the binding frees it; a closure's child frame takes it over. A `[...]` or `{...}` literal is built once, so pushing one that holds a dict (`VF_DICT` on its header, set by `build_tuple`) deep-copies it. The checker refuses a binding of a box or socket. Tagging a linear value keeps it linear.
- parse.slap's combinators scan with an index and cut once; recursing per character exhausts the C stack.
- Removing a limit exposes whatever it was hiding. Audit every consumer of the newly unbounded value first.
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

`at` and `edit` are not fallible: the checker proves the key, and `at must` is an error unless the field holds a result. `take-n`/`drop-n` clamp a count past the end; a negative count is an error. `must` unwraps `'ok` and dies on anything else. Bodies given to `each`, `fold` and `mutate` must leave exactly one value; the runtime checks.
