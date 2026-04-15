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
- **Primitives**: ~75 C functions registered via `prim_register`. Macros `ARITH2`, `FLOAT1`, `CMP2` generate families of math/comparison ops.
- **Prelude**: ~70 derived definitions in Slap itself (embedded string in slap.c). Loaded at startup before user code.
- **Self-reference**: `def` makes the bound name visible inside its own body when referenced textually, enabling recursion without a keyword.

### Tagged unions (sum types)

`tag` wraps a value with a symbol tag: `123 'ok tag` → `VAL_TAGGED`. Prelude words `ok`/`no` are sugar for `'ok tag`/`'no tag`. Stack layout: `[...payload..., TAGGED_HEADER]` where header reuses `compound` struct with `compound.len` = tag symbol ID, `compound.slots` = total slots.

- **`tag`**: `payload 'sym tag` — creates tagged value
- **`case`**: unified conditional. Two dispatch modes:
  - On tagged scrutinee: `tagged default {'sym1 (body1) 'sym2 (body2)} case` — match by tag symbol, payload pushed before body. Default fires on unmatched tag.
  - On non-tagged scrutinee: `value default {(pred1) (body1) (pred2) (body2)} case` — evaluates predicates in order (scrutinee pushed for each); on truthy, runs body.
- **`then`**: `tagged (body) then` — monadic chain (hardcoded `'ok`): unwrap, apply body, re-wrap. Non-ok passes through.
- **`default`**: `tagged fallback default` — unwrap `'ok` payload, or drop tagged and push fallback.
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
- **Stackable** (copyable): Int, Float, Symbol, Tuple, Record, List, String, Tagged. Support `dup`/`drop`.
- **Linear**: Box only. Must be consumed exactly once via `free`, `lend`, `mutate`, or `clone`.

`lend` borrows a stackable snapshot from a Box. The snapshot itself is stackable, but when the box contains a compound value (list/record/tuple/tagged) the checker forbids `let`/`def`-binding the snapshot inside the `lend` body — a later `mutate` would silently corrupt the binding, which aliases the box's backing storage (slap.c:950–953).

### Protocols (built-in typeclasses)

Constraints formalize which operations work on which types. Used in `[...] effect` annotations. Protocols live entirely in the type checker (`tc_constraint_matches`); no runtime dispatch.

| Protocol | Keyword | Types | Methods |
|----------|---------|-------|---------|
| Eq | `eq` | all stackable | `eq` |
| Ord | `ord` (implies Eq) | int, float | `lt`, `sort` |
| Num | `num` (implies Eq) | int, float | `plus`, `sub`, `mul`, `div` |
| Integral | `integral` (implies Num) | int | `mod`, `divmod`, `wrap`, bitwise |
| Semigroup | `semigroup` | list, tuple, record | `cat` |
| Seq | `seq` (implies Semigroup) | list | `get`, `set`, `push`, `pop` |
| Sized | `sized` | list, tuple, record, dict | `len` |

Additional constraint keywords recognized in effect annotations: `eql` (alias for `eq`), `monoid`, `functor`, `applicative`, `foldable`, `monad`, `dict`, `linear`. These exist in the constraint lattice (slap.c:589–624) but currently don't gate additional user-visible prelude operations — use sparingly.

`each` iterates over lists (producing a new list) and over `'ok`-tagged values (applies body to payload, re-wraps; non-ok passes through). `then` chains on `'ok`-tagged values (hardcoded tag; not yet general over any tag). `fold`, `filter`, `scan` work on lists. These aren't surfaced as named protocols because they don't generalize beyond their current types.

Side-effect iteration: `(body) each drop`.

### `either` type annotation

Declares tagged variant types in effect annotations: `{'ok type 'no type} either`. Used to give precise types to fallible operations.

```
'pop ['a seq own in  'a seq move out  {'ok 'a 'no ()} either move out] effect
'read [list own in  {'ok list 'no list} either move out] effect
```

Supports type variables (`'a`) that resolve against the sig's other slots. `default` enforces that the fallback value matches the `ok` payload type — `[1 2 3] pop () default` is a type error because `()` (tuple) doesn't match the list element type (int).

Parsed in `parse_type_annotation`. Stored in `TypeSlot.either_syms/either_types/either_tvars`. Applied via `UnionDef` creation in `tc_check_word`.

List ops: `push`, `pop`, `set`, `len`, `cat`. `compose` is a separate tuple-concat primitive for function composition.

### def vs let

Both take value-then-name: `val 'name def` / `val 'name let`.

- `def` — auto-executes tuples on lookup. For function definitions.
- `let` — pushes the bound value on lookup. For binding stack arguments in function bodies.

### SDL graphics (optional)

Compiled with `-DSLAP_SDL`. 640×480 canvas, 2-bit grayscale. Primitives: `clear`, `pixel`, `fill-rect`, `on`, `show`. Event callbacks for mouse/keyboard.
