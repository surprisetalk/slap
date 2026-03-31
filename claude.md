# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is Slap

A minimal concatenative language with linear types and row polymorphism. Single-file C implementation (`slap.c`).

## Build & Test

```bash
# Build (no SDL needed for headless)
gcc -std=c99 -O2 -lm slap.c -o slap

# Run all tests (builds first)
./run-tests.sh

# Run a single program
./slap program.slap --test   # headless
```

Tests have three categories:
- `tests/expect.slap` — assertion-based tests (should pass)
- `tests/type-err.slap` — each non-comment line should produce `TYPE ERROR`
- `tests/panic.slap` — each non-comment line should produce `SLAP PANIC`

## Architecture (slap.c)

The implementation flows as: **Parse -> Type Check -> Evaluate**.

### Values

Tagged union (`Val`) with 11 types split into **Stackable** (copyable) and **Linear**:

**Stackable** (freely duplicated with `dup`):
- Int, Float, Symbol — scalars (`true`/`false` are sugar for `1`/`0`; conditionals use zero/nonzero)
- Tuple — code/data `(1 2 iplus)`, refcounted
- Record — symbol-keyed `{'a 1 'b 2}`, refcounted
- Slice — immutable array `[1 2 3]` or string `"hello"`, refcounted
- Dice — key-value lookup, created via `dice` from slice of tuples, refcounted

**Linear** (must be consumed exactly once via `free`, or mutated in place):
- Box — wraps single value, heap-allocated
- List — mutable list, created from slice via `list`
- Dict — mutable hash table, created from dice via `dict`
- String — mutable byte array, created from slice via `str`

### AST

Flat array of `Node` structs. Node types: N_PUSH, N_WORD, N_SLICE, N_TUPLE, N_RECORD. Bracket nodes (tuple/slice/record) store body offset+length into the same array.

### Memory

Pool-based allocation with freelists for slices, tuples, records, lists, dicts, strings, heap slots, and scopes. Copyable compound types (Tuple, Slice, Record) use reference counting. Symbol interning via linear search.

### Type Checker

Hindley-Milner-style inference with row polymorphism for records and stack effects. Tracks Copy/Linear constraints. `get`/`set` are row-aware (field lookup on closed rows catches missing fields and type mismatches). `match`/`cond` unify branch effects. `clone`/`free`/`lend` require linear types. No escape constraints needed — `lend` and `cond` only expose stackable values to borrowing contexts. Runs before evaluation; errors halt before any code executes.

### Evaluator

Stack machine. Word lookup: `def` bindings auto-execute tuples, `let` bindings push values. Prelude is a C string literal parsed before user code.

### Primitives (~97 core + 5 SDL)

**Meta**: `def` (`'name val def`), `let` (`val 'name let`), `recur` (`'name recur (body) def` — enables self-recursion in body)

**Stack**: `dup`, `drop` (stackable only), `swap`, `dip`

**Control**: `apply`, `if` (zero/nonzero), `loop`, `while` (`(pred) (body) while`), `cond`, `match`

**Logic**: `not` (0→1, nonzero→0), `and`, `or` — all operate on integers

**Compare**: `eq`, `lt` — return Int 0/1

**Polymorphic Math**: `plus`, `sub`, `mul`, `div`, `mod` — dispatch on Int/Float (mod is Int-only)

**Int Math**: `iplus`, `isub`, `imul`, `idiv`, `imod` (concrete-typed aliases)

**Float Math**: `fplus`, `fsub`, `fmul`, `fdiv`, `fsqrt`, `fsin`, `fcos`, `ftan`, `ffloor`, `fceil`, `fround`, `fexp`, `flog`, `fpow`, `fatan2`

**Conversion**: `itof`, `ftoi`

**Tuples**: `compose`, `cons`, `car` (car is lossy for word nodes — returns symbol)

**Records**: `rec`, `get`, `set`

**Slices**: `len`, `fold`, `reduce` (fold without init), `at` (with default), `put`

**Iteration**: `each`, `map`, `filter`, `range`

**Array Ops**: `sort`, `cat`, `take`, `drop-n`, `scan`, `rotate`, `select`, `keep-mask`, `windows`, `rise`, `fall`, `index-of`, `reshape`, `transpose`, `shape`, `classify`, `pick`, `group`, `partition`

**Dices**: `dice`, `grab` (with default), `ifsert`

**Memory**: `lend` (borrow snapshot), `clone` (deep copy), `free`

**Box**: `box`

**Lists**: `list`, `list-zero`, `list-concat`, `list-assign`, `list-at`

**Dicts**: `dict`, `dict-insert`, `dict-remove`

**Strings**: `str`, `str-concat`, `str-assign`

**IO**: `print`, `print-stack`, `assert`, `halt`, `random` (`max random` — int in [0,max))

**Console** (requires `-DSLAP_SDL`): `on` (`model 'event (handler) on` — register event handler), `show` (`model (render) show` — start SDL loop), `clear` (`color clear` — fill canvas), `pixel` (`x y color pixel` — set pixel), `millis` (push SDL ticks)

### Prelude (~54 entries)

**Defs**: peek, inc, dec, neg, abs, over, nip, rot, keep, bi, iszero, ispos, isneg, iseven, isodd, max, min, neq, gt, ge, le, sum, product, isany, isall, count, first, last, sqr, cube, fneg, fabs, fclamp, lerp, clamp, isbetween, sign, repeat, reverse, flatten, zip, where, member, dedup, table, sort-desc, max-of, min-of, frecip, fsign, couple, find

**Constants** (via `let`): pi, tau, e

### Key Semantics

- No Bool type — `true`/`false` are `1`/`0`. Conditionals (`if`, `loop`, `while`, `cond`, `filter`) branch on zero/nonzero. Comparisons (`eq`, `lt`) return Int 0/1. This enables APL-style boolean mask arithmetic: `(3 mod 0 eq) map sum`
- `def` is `'name val def` — auto-executes tuples on lookup
- `let` is `val 'name let` — pushes value on lookup (no auto-exec); value must be copyable
- `recur` is `'name recur (body) def` — marks name for self-recursion before the tuple literal; the TC pre-binds the name with a polymorphic tuple type so the body can call itself
- `lend` borrows a stackable snapshot from a linear value: Box yields inner value, List yields slice, Dict yields dice, String yields slice of char codes
- `cond` predicates receive borrowed snapshot of scrutinee; matching body consumes the original
- `cons`/`car` operate on tuple AST nodes — `car` is lossy for word nodes (returns symbol), `compose` is the lossless way to combine code
- String literals `"hello"` create slices of int char codes
- Closures capture creation-site scope via snapshot
- Runtime errors are panics with Elm-style messages, no recovery

### Fantasy Console (`-DSLAP_SDL`)

640×480 canvas, 2-bit grayscale (0-3). C-side framebuffer — drawing via `pixel`/`clear` primitives, not a first-class value.

- `on` registers event handlers: `model 'event-name (handler) on → model`. Handlers: `event-data model → model`. Events: `'keydown` (Int keycode), `'tick` (Int frame count).
- `show` starts the render loop: `model (render) show`. Render receives readonly model snapshot, draws via `pixel`/`clear`. Never returns.
- `--test` flag: `show` runs one tick + one render, then exits (headless).
