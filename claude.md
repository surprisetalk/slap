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
- **Evaluator** (`eval` → `build_tuple` → `eval_body`): Tokens → compound values (tuples), then stack-machine execution. Words that resolve to a primitive at build time are stored as `VAL_XT` with a direct function pointer; unresolved names are stored as `VAL_XT` with `fn=NULL` and looked up at dispatch via `dispatch_word` (frame lookup → primitive table).
- **Frames**: Lexical scope chain with refcounting. Closures capture their defining frame. `let` bindings auto-execute tuples on lookup; scalars push as values.
- **Primitives**: ~100 C functions registered via `prim_register`. Macros `ARITH2`, `FLOAT1`, `CMP2` generate families of math/comparison ops.
- **Prelude**: ~70 derived definitions in Slap itself (embedded string in slap.c). Loaded at startup before user code. Non-core string helpers (`crlf`, `int-str`, `str-join`, `http-request`) live in `examples/lib/strings.slap` — cat alongside your program when needed.
- **Self-reference**: A name bound via `let` is visible inside its own body when referenced textually, enabling recursion without a keyword.

### Tagged unions (sum types)

`tag` wraps a value with a symbol tag: `123 'ok tag` → `VAL_TAGGED`. Prelude words `ok`/`no` are sugar for `'ok tag`/`'no tag`. Stack layout: `[...payload..., TAGGED_HEADER]` where header reuses `compound` struct with `compound.len` = tag symbol ID, `compound.slots` = total slots.

- **`tag`**: `payload 'sym tag` — creates tagged value
- **`case`**: unified conditional. Two dispatch modes:
  - On tagged scrutinee: `tagged default {'sym1 (body1) 'sym2 (body2)} case` — match by tag symbol, payload pushed before body. Default fires on unmatched tag.
  - On non-tagged scrutinee: `value default {(pred1) (body1) (pred2) (body2)} case` — evaluates predicates in order (scrutinee pushed for each); on truthy, runs body.
- **`then`** (prelude): `tagged (body) then` — if `'ok`, unwrap payload, run body (body returns a new tagged); if not ok, re-wrap with `'no`. Implemented as `'body let () {'ok (body) 'no (no)} case` (body auto-execs on lookup).
- **`default`** (prelude): `tagged fallback default` — unwrap `'ok` payload, or drop tagged and push fallback. Implemented as `'fb let fb {'ok () 'no (drop fb)} case`. Note: since `let` auto-execs tuples on lookup, a tuple fallback will run rather than be pushed — pass `((body))` if you want the literal tuple.
- **`union`**: `{'ok 'int 'no 'str} union` — runtime no-op, type annotation only. Drops the schema record.
- **`ok`/`no`** (prelude): sugar for `'ok tag` / `'no tag`
- **`none`** (prelude): sugar for `() no` — the empty error value
- **`must`**: extract `'ok` payload, crash with clear error on `'no`. Used in prelude internals where failure is a bug.

Tagged values are stackable (copyable). `case` is an HO op with `HO_BRANCHES_AGREE`; when the scrutinee is tagged, box-payload and linear-default checks fire. `then` is HO with `HO_BODY_1TO1`. Type constraint: `TC_TAGGED`.

### Fallible operations (return tagged results)

These operations return `value ok` on success and `() no` (or `payload no`) on failure instead of panicking:

| Operation | Success | Failure | Notes |
|-----------|---------|---------|-------|
| `pop` | `element ok` | `none` | Empty list/tuple/record |
| `get` | `element ok` | `none` | Index out of bounds |
| `set` | `compound ok` | `none` | Index out of bounds |
| `nth` | `element ok` | `none` | Index out of bounds |
| `at` | `value ok` | `none` | Key not found |
| `edit` | `record ok` | `none` | Key not found |
| `index-of` | `index ok` | `none` | Element not found |
| `str-find` | `position ok` | `none` | Substring not found |
| `read` | `bytes ok` | `path no` | File open/read error |
| `write` | `1 ok` | `path no` | File open/write error |
| `ls` | `entries ok` | `path no` | Directory open error |
| `utf8-encode` | `bytes ok` | `position no` | Invalid codepoint |
| `utf8-decode` | `codepoints ok` | `position no` | Invalid byte sequence |
| `tcp-connect` | `socket ok` | `message no` | Connection error |
| `tcp-send` | `1 ok` | `message no` | Send error |
| `tcp-recv` | `data ok` | `message no` | Receive error |
| `tcp-listen` | `socket ok` | `message no` | Bind/listen error |
| `tcp-accept` | `client ok` | `message no` | Accept error |
| `parse-http` | `status headers body ok` | `message no` | Parse error |

Pattern: `[] pop (1 plus ok) then -1 default` → `-1` (empty list, default). `[1 2 3] pop (1 plus ok) then -1 default` → `4` (success path).

`take-n`/`drop-n` clamp to valid range instead of panicking. `random` clamps max to 1 minimum. `div`/`mod`/`divmod`/`wrap` still panic on zero (programmer errors).

### Type system

Two categories of types:
- **Stackable** (copyable): Int, Float, Symbol, Tuple, Record, List, String, Tagged, Dict. Support `dup`/`drop`. Dict `dup` deep-clones; `drop` deep-frees.
- **Linear**: Box only. Must be consumed exactly once via `free`, `lend`, `mutate`, or `clone`. `free`/`clone` reject dicts at type time — use `drop`/`dup` instead.

`lend` borrows a stackable snapshot from a Box. The snapshot itself is stackable, but when the box contains a compound value (list/record/tuple/tagged) the checker forbids `let`-binding a borrowed compound snapshot inside the `lend` body — a later `mutate` would silently corrupt the binding, which aliases the box's backing storage. Only fires when the bound value carries the borrowed flag; binding a freshly-built tuple literal inside a lend body is fine.

`deep_free_values` recursively frees boxes inside compounds (poisoning `BoxData->data=NULL` so any stale reference hits `double-free detected` rather than use-after-free). This catches rare TC gaps where a linear value escapes into a stackable compound that's later dropped.

### Protocols (built-in typeclasses)

Constraints formalize which operations work on which types. Used in `[...] effect` annotations. Protocols live entirely in the type checker (`tc_constraint_matches`); no runtime dispatch.

| Protocol | Keyword | Types | Methods |
|----------|---------|-------|---------|
| Eq | `eq` | all stackable | `eq` |
| Ord | `ord` (implies Eq) | int, float, sym | `lt`, `sort` |
| Num | `num` (implies Eq) | int, float | `plus`, `sub`, `mul`, `div` |
| Integral | `integral` (implies Num) | int | `mod`, `divmod`, `wrap`, bitwise |
| Semigroup | `semigroup` | list, tuple, record | `cat` |
| Seq | `seq` (implies Semigroup) | list | `get`, `set`, `push`, `pop` |
| Sized | `sized` | list, tuple, record, dict | `len` |

Additional constraint keywords recognized in effect annotations: `functor` (input constraint for `each`), `monad` (for `then`), `dict` (for the dict type), `linear` (synonym for box; no longer cross-matches dict).

`each` iterates over lists (producing a new list) and over `'ok`-tagged values (applies body to payload, re-wraps; non-ok passes through). `fold`, `filter`, `scan` work on lists. These aren't surfaced as named protocols because they don't generalize beyond their current types. `then`/`default` are prelude-level sugar over `case` — see above.

Side-effect iteration: `(body) each drop`.

### `either` type annotation

Declares tagged variant types in effect annotations: `{'ok type 'no type} either`. Used to give precise types to fallible operations.

```
'pop ['a seq own in  'a seq move out  {'ok 'a 'no ()} either move out] effect
'read [list own in  {'ok list 'no list} either move out] effect
```

Supports type variables (`'a`) that resolve against the sig's other slots. `default` enforces that the fallback value matches the `ok` payload type — `[1 2 3] pop () default` is a type error because `()` (tuple) doesn't match the list element type (int).

Parsed in `parse_type_annotation`. Stored in `TypeSlot.either_syms/either_types/either_tvars`. Applied via `UnionDef` creation in `tc_check_word`.

**Producer-side validation** (`tc_check_body_against_sig`): when a `(body) [sig] effect 'name let` declaration has an either-constrained output, the body is scanned for literal `'sym tag` emissions and bare `ok`/`no`/`none`. Any tag not in the declared variant set is a type error. Also: `'name [sig] effect` followed by a later `(body) 'name let` reconciles the body's effect shape against the forward declaration.

**Exhaustiveness enforcement**: `case` with missing variant clauses on a union that carries a linear payload is a *hard* error (non-recoverable) — silent drop of a linear variant is always a bug. Unions without linear variants remain soft errors for easier recovery during editing.

List ops: `push`, `pop`, `set`, `len`, `cat`. `compose` is a separate tuple-concat primitive for function composition.

### let (unified binding)

`val 'name let` — binds `val` to `name`. One keyword covers both "define a function" and "bind a stack argument":

- **Scalar bound**: lookup pushes the value. `42 'foo let; foo` → `42`.
- **Tuple bound**: lookup auto-executes the tuple. `(1 plus) 'inc let; 2 inc` → `3`.

This replaces the old `def`/`let` split. `def` is no longer a keyword.

**Binding a literal tuple as data** — wrap in extra parens so the outer tuple auto-execs and pushes the inner:

- `((1 2 3)) 'foo let` → `foo` pushes `(1 2 3)`
- `(1 2 3) 'foo let` → `foo plus plus` → `6` (auto-execs)

**HOF closure args** — when an HOF-style function receives a closure parameter, the caller wraps it at the call site so it's stored as a tuple. Inside the HOF body, a bare reference auto-executes:

```
('pred let [1 2 3 4] (pred) filter) 'keep-when let
(iseven) keep-when  -- → [2 4]
```

The `(pred)` inside `filter`'s tuple defers `pred`'s dispatch to apply-time.

### quote

`'name quote` pushes the raw value of `name`'s binding without auto-executing. Needed when **threading a closure-arg through a recursive call** — the bound closure would auto-exec on bare lookup, shadowing itself in the recursive frame. `quote` captures the value at the current scope.

```
('pred let dup 0 gt (dup pred drop 1 sub 'pred quote recurse) () if) 'recurse let
5 (iseven) recurse   -- applies pred at each step
```

Without `quote`, `pred recurse` at the recursive call site would auto-exec `pred` (pushing the bool result) instead of passing `pred` itself to the recursion.

Use `quote` sparingly — most HOF patterns work without it via the call-site-wraps idiom above. Recursion + closure arg is the main case that needs it.

### SDL graphics (optional)

Compiled with `-DSLAP_SDL`. 640×480 canvas, 2-bit grayscale. Primitives: `clear`, `pixel`, `fill-rect`, `on`, `show`. Event callbacks for mouse/keyboard.
