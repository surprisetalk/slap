# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
make slap          # Terminal interpreter (C99, -O3 -flto, links -lm)
make slap-sdl      # SDL graphics build (adds -DSLAP_SDL, links SDL2)
make slap-wasm FILE=prog.slap  # Emscripten/WASM build (embeds .slap file, outputs .html, .js, .wasm)
make test           # Run all test suites
make clean          # Remove binaries
```

CLI: `./slap [--check] [--headless] [args...] < file.slap`
- `--check` — type-check only, no execution
- `--headless` — (SDL build) run without a window, tick loop continues indefinitely
- Positional args available via `args` primitive; `isheadless` and `cwd` also available

## Tests

`make test` runs nineteen checks in order:
1. `make check-refs` — every file the build and docs reference exists on disk
2. `cat examples/lib/strings.slap examples/lib/parse.slap tests/expect.slap | ./slap` — integration tests (assert-based, halts on failure)
3. `./slap --check < tests/type.slap` — type system tests
4. `./slap < tests/type.slap > /dev/null` — execute type tests
5. The same concatenated expect.slap stream under `--check` — type-check the integration tests
6. `python3 tests/run_panic.py` + `python3 tests/run_type_errors.py` — verify expected errors
7. `args` with and without positional arguments
8. `python3 tests/run_euler.py` — 52 Euler solutions, each prepended with `strings.slap`
9. `python3 tests/run_wiki.py` — boots `examples/wiki.slap` on a random port, drives it over real HTTP (GET/POST/404/traversal/oversize), kills it
10. `python3 tests/run_kv.py` — boots `examples/kv-server.slap`, drives it through `examples/kv-client.slap` and raw sockets (roundtrip, spaced values, persistence across restart, corrupt-snapshot refusal, unwritable-save survival), kills it
11. `./slap --check < examples/chip8.slap`, then `./slap --headless < examples/chip8.slap | grep -q chip8-selftest-ok` — the CHIP-8 emulator's in-language opcode self-test (SDL words type-check unconditionally; `halt` fires before any `on`/`show` dispatches, so no SDL build is needed)
12. `./slap --check < examples/uxn.slap`, then `./slap --headless < examples/uxn.slap | grep -q uxn-selftest-ok` — the Uxn/Varvara emulator's self-test: every base opcode, each mode flag, the immediates, the System/Console/Screen/Datetime devices, and an end-to-end boot of the embedded demo ROM. Same no-SDL-needed trick as chip8.

    The Datetime block (ports 0xc0–0xca, read-only, computed per read from the `datetime` primitive) is checked through the real DEI path against `datetime` itself rather than against constants, so a wrong port index fails instead of returning a plausible number. Only the fields that cannot move during a run are compared — hour/minute/second are range-checked, because asserting on a value that ticks mid-test is a flaky test, not a device. Both range edges (0xbf and 0xcb must read the device page) are pinned: an off-by-one in the `q 191 gt q 203 lt` guard is the likely bug, and nothing else would catch it. Verified falsifiable — shifting the port index by one, or narrowing the guard by one, each fails the self-test.

    None of raven's nine reference ROMs read Datetime at all (measured, not assumed: instrumenting the read path counts 0 for every one), so the pixel comparison cannot become clock-dependent. `drool` reads it 8 times and `bunnymark` 79.

    The self-test also pins the five Screen behaviours that real ROMs depend on and that nothing else here would catch: `Screen/auto`'s bits drive the *opposite* axis inside the draw loop (auto-Y lays tiles out along X) while the port writeback afterwards uses the normal axes and moves by a single 8; the blend table is irregular and cannot be derived arithmetically; a sprite whose x has wrapped past 0 shows its right half at the left edge; and a flipped fill is exclusive of x. Each assertion was checked to fail against the pre-fix behaviour — an assertion that cannot fail is not a regression test.
13. `python3 tests/run_feed.py` — renders `examples/feed.slap` over both checked-in fixtures (RSS 2.0 and Atom), checks the failure paths report instead of printing an empty digest, and pins the 16384-byte parse ceiling from both sides
14. `python3 tests/run_todo.py` — drives `examples/todo.slap` through a real JSON file: add/done/undone/rm, escaping, control-byte refusal, and six kinds of unreadable file that must be refused and left on disk
15. `python3 tests/run_serve.py` — boots `examples/serve.slap` over a temp directory, drives it with raw sockets (traversal raw and percent-encoded, HEAD, 405, 413, malformed request lines), then fetches from it with `examples/fetch.slap`
16. `python3 tests/run_codec.py` — renders a `.uf1` font to ASCII with `examples/banner.slap` and checks the bitmap against the font file, then writes a TGA with `examples/plasma.slap` and checks it against the format
17. `./slap --check < examples/maze.slap`, then `./slap --headless < examples/maze.slap | grep -q maze-selftest-ok` — the maze generator's property self-test. Same no-SDL-needed trick as chip8
18. `./slap --check < examples/raycast.slap`, then `./slap --headless < examples/raycast.slap | grep -q raycast-selftest-ok` — the raycaster's DDA self-test against hand-computed distances
19. `examples/lib/` load/typecheck combos, then `bash tests/adversarial/run.sh`

Checks 13-18 are the demos added on 2026-08-08 and together take about 2.3s. Three things they pin that nothing else does, each found by writing them:

- **`case`, `then` and `default` used to stage the whole scrutinee through `LOCAL_MAX`; `must`, `each` and `pthen` never did.** Writing these demos is what surfaced it, and it is fixed (see **Buffers and limits**). The demos still use `each` to shrink a large success payload before branching, because that form was always safe and costs nothing.
- **`ufx-decode` cannot read a real font.** 256 glyphs x 64 bits plus headers is 16641 slots against the 16384 cap, so it only ever worked on the short synthetic fixture in its own tests. `banner.slap` slices the eight bytes for one glyph and calls `icn-decode` on those instead.

Note that expect.slap is **not** run bare: it depends on `strings.slap` and `parse.slap`, which must be `cat`ed ahead of it.

Tests use `assert` (halts on first failure). Python scripts validate that specific inputs produce expected error messages. `tests/adversarial/run.sh` classifies probes as `TYPECHECK_REJECT` / `PANIC` / `CLEAN_RUN` and fails if a classification changes.

### `make test-uxn-refs` (separate: needs network)

`python3 tests/run_uxn_refs.py` checks `examples/uxn.slap` against [mkeeter/raven](https://github.com/mkeeter/raven)'s snapshot suite — nine ROMs, compared pixel for pixel with its reference renders. It is **not** part of `make test`, because it downloads those ROMs and PNGs from GitHub at a pinned SHA and the suite has to work offline; downloads are cached under `tests/.uxn-refs/` so only the first run needs network (`--offline` thereafter). Run it after touching anything in uxn.slap's Screen path: the in-language self-test passed while three real Screen bugs were live, and this is what found them.

Three things it has to get right, each of which silently reads as an emulator bug when wrong:

- **Frame count.** raven calls `dev.redraw()` exactly 60 times after the input, and these ROMs animate. At two frames `screen.rom` renders the reference image shifted two pixels, which looks exactly like an off-by-two in `Screen/sprite`.
- **Geometry.** The harness rewrites `SCR-W`/`SCR-H`/`SCR-N`/`FG-B`/`STATE-N` so the emulator is rebuilt at each ROM's own resolution. Without that the comparison is "structurally similar at a different aspect ratio". It hard-errors if those declarations no longer match its patterns rather than comparing something meaningless.
- **Input.** Several of these ROMs install only a Mouse or Controller vector and never a Screen vector, so they render blank without the replay. That lives in uxn.slap's dump block, not in the harness.

Expected diffs are checked in per ROM: every one is 0 except piano's 22 pixels of audio level meter, which is the missing Audio device showing through. Raising a number there needs a stated reason.

### `make test-uxn-sweep` — does the render move with the frame count?

The comparison above only ever looks at 60 frames, so it cannot distinguish a ROM that renders correctly from one that renders correctly *at exactly that count* because two errors cancelled. `--sweep` renders each ROM at 1, 60 and 120 and pins the `motion` column of the `ROMS` table:

- **static** (screen_auto, screen_blending, screen_bounds, screen_pixel, controller, piano, mandelbrot) — byte-identical at every count. Measured from 1 frame to 240. If one of these starts drifting, state is surviving between Screen-vector calls that should not: an uncleared dirty box, a stack pointer that creeps, a device-page write that leaks.
- **anim** (screen, audio) — must *not* be identical across those counts. If an animated ROM goes flat, the frame loop has stopped advancing state and its 60-frame match no longer proves anything. `screen` is the sharpest: 0 diffs at 60, 16 at 59, 610 at 61.

`mandelbrot` is skipped and the skip is printed. It paints from its reset vector, installs no Screen vector, and retires **0** frame-loop instructions at any count — three identical renders for 16 minutes.

### `make bench-uxn` — throughput

Prints uxn instructions/sec; never fails. uxn.slap's dump block emits an `INS n` line counting instructions retired *in the frame loop only* (`run-at` loads `BUDGET-I` with its cap and `step` decrements it once per instruction, so cap-minus-remainder is the work done). Each ROM is run twice — once at 0 frames for the setup baseline, once at N — because every ROM pays a fixed boot cost and mandelbrot's reset vector alone is ~5.5 minutes.

Two ROMs have a real per-frame workload and are the default:

| ROM | ins/frame | ins/sec | verified? |
|-----|-----------|---------|-----------|
| `screen` | 25,945 @ 256×176 | ~83k | pixel-exact vs raven |
| `drool` | ~21,000 @ 320×240 | ~169k | **no reference render exists** |

`screen`'s instruction count is exact and reproducible — it reads no clock, so `--sweep` pins it at 25,945 every run. `drool`'s is not: it reads Datetime 8 times and its count drifts about 0.2% run to run (1,258,796–1,261,434 over 60 frames measured a second apart), so it is quoted rounded. Don't treat a changed drool number under ~0.5% as a regression.

`screen` is the headline precisely because it is also pixel-exact — the benchmark times *correct* work. `drool` is in raven's `roms/` but not its snapshot suite, so it is marked `allow=None` in the `ROMS` table (`BENCH_ONLY`), which skips its PNG download and excludes it from `--sweep` and the comparison. Naming only bench-only ROMs to a comparison mode is an error, not a vacuous pass. The two rates differ by 2× because screen.rom is sprite-blit bound and drool is compute bound; neither is "the" number.

**bunnymark is still not usable here, and implementing Datetime did not change that.** The original diagnosis was half right: its RNG (`0x03f2`) is a self-modifying xorshift whose seed literal is EOR-ed from `DEI2 0xc0`, so with no Datetime device it was seeded with 0 — xorshift's fixed point — and emitted 0 forever. Adding the device fixed exactly that much and no more:

| | before Datetime | after |
|---|---|---|
| seed literal at `0x03f3` | `0x0000` | `0x5759` |
| bunny record at `0x0642` | all zeros | varies per frame |
| render at 1 vs 240 frames | byte-identical | 12 lines differ |
| instructions/frame | 425 | 441, flat to 240 frames |
| population | 0 | **1, and it stays 1** |

The workload still does not grow, so it still measures nothing. bunnymark installs *only* a Screen vector (`0x0163`) and no Mouse vector, so nothing the harness does to `Mouse/state` reaches an add-bunny path — verified both with the button held (the replay's default) and toggled every frame; the population is 1 either way. Whatever gates the spawn is a third thing, unidentified. Don't reach for this ROM, and don't assume the next missing device is the answer either. `drool.rom` remains the honest second number.

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

### Buffers and limits

`LOCAL_MAX` (16384) bounds the C-stack scratch buffers that primitives use for a single value: `get`/`pop`/`peek`/`nth` element copies, `cat` results, `each`/`fold` inputs, `into`, `dip`, and `case`'s *predicate-mode* scrutinee. All of them check it and die with a `value too large (N slots, max M)`-style message.

Two that used to be on that list are not any more, and the reason is the same both times — the copy was never needed:

- **`case` on a tagged scrutinee unwraps in place.** The payload lies directly under the header, so a match only has to drop the header (`sp--`), exactly as `must` and `pthen` already did. Predicate mode still copies, and must: the scrutinee is re-pushed for every predicate. This lifted the cap on `then` and `default` too, since both are prelude words over tagged `case`.
- **`swap` is an in-place block rotation.** Reverse the whole two-block region, then reverse each block where it lands; that exchanges their positions while restoring each one's internal order, with no temporary. `tests/expect.slap` pins both the size behaviour and the order behaviour — a single reversal would swap the blocks and scramble their contents, and only the order test catches that.

These two were the ceiling on every `xd-*`/`jd-*` parse: `parse-exact` and `parse-spaces` carry the *remaining input* through `then`, and `parse-while-acc` ends in `acc swap` stepping the accumulator over it. A feed capped at 16384 bytes of source before, and parses past 16714 now. **It is not uncapped** — the next buffer in the chain is `into` building the element record (around 17k bytes of source), and past that the frame arena fills. `tests/run_feed.py` brackets both ends.

**A separate, deeper limit sits under all of this: parse.slap's combinators recurse once per character.** `parse-spaces-core` **segfaults** at about 3000 consecutive spaces — well under the old 16384 cap, so this predates and is independent of the changes above; `parse-while`/`parse-int` fill the frame arena at about 5000 with a clean error. `EVAL_DEPTH_MAX` is 10000, which is too high to catch the C-stack exhaustion first. Long *documents* are fine; a single long *token run* is not. Making those five words iterative is the remaining work, and it is a redesign rather than an edit: the "remaining input on the stack" convention wants `swap`/`dip` to step over the input, and an index-based scan wants the string `let`-bound, which is an O(n) frame copy per call.

**Any `Value buf[n]` whose `n` comes from data rather than from a constant is a latent bare SIGSEGV** — the C stack is 8 MiB and a `Value` is 32 bytes, so ~250k slots overflows it with no diagnostic. Three such buffers existed and are gone: `take-n`/`drop-n` (a slice is contiguous, so it moves in place), `let` (`frame_bind` only ever copies *out* of its argument, so it can read the operand stack directly), and `lend`'s result staging (a `memmove` on the operand stack inserts the box underneath instead). Adding a new one is a bug unless it is bounded by `LOCAL_MAX` first. This is what made `euler/10` pass or fail depending on how much C stack happened to be left.

`FRAME_VALS_MAX` (2097152) is the frame value arena, and `vals` is inline in `Frame`, so it sets `sizeof(Frame)` — 64 MiB. Measured peak across the whole suite is 1,000,822 slots (`euler/37`, then `euler/10`), both binding a million-element sieve snapshot inside a `lend` body; `examples/uxn.slap`'s ROM dump peaks at 331,841 and ordinary programs sit under 4,000. At most 7 frames are live at once. `frame_new` reports allocation failure rather than dereferencing NULL.

**`vals` cannot become a `realloc`-growable buffer**, which is the obvious way to stop `sizeof(Frame)` being 64 MiB. `dispatch_word` takes `Value *v = &lu.frame->vals[b->offset]` and hands it to `eval_tuple_scoped` for the whole duration of the call (slap.c:2578) — and that body binds names in the very frame the pointer points into. One growing rebind mid-body and the code being executed is freed memory. Every other `vals` pointer is fine (`quote`, `nth`, the restore loop, and `frame_bind` itself all read and are done), so this one site is the whole blocker. The two ways past it are copying the body out on every word dispatch — the hot path for chip8/uxn — or a chunked arena that never moves what it has already handed out, which means `Binding.offset` stops being a plain index. Neither is worth it: the array is demand-zero, so the cost is address space and not RSS. Measured max RSS is 5 MB for chip8 and 100 MB for `euler/10`, which genuinely touches a million slots.

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
| `get` | `element ok` | `none` | Index out of bounds; consumes the compound |
| `peek` | `element ok` | `none` | Index out of bounds; compound stays on the stack |
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

A slice is a contiguous run of its source, so `prim_slice_n` moves it in place rather than copying it out. Both ops used to stage through a `Value tmp[LOCAL_MAX]` with no bounds check, which capped a result at 16384 slots and, past that, smashed the C stack with no message at all. There is no size limit on either now.

### Type system

Two categories of types:
- **Stackable** (copyable): Int, Float, Symbol, Tuple, Record, List, String, Tagged, Dict. Support `dup`/`drop`. Dict `dup` deep-clones; `drop` deep-frees.

**Dict is stackable but cannot be `let`-bound.** Unlike every other stackable type it is a heap object, and `frame_bind` copies the `Value` bitwise — so the binding and the stack copy would share one `DictData`, and `drop` (or `len`) on either leaves the other reading freed memory. That surfaced as `of` returning a *wrong answer* rather than crashing, so `'name let` on a dict is now a hard type error. Thread it on the stack instead; `examples/kv-server.slap` is the worked example. A record is the bindable alternative.
- **Linear**: Box only. Must be consumed exactly once via `free`, `lend`, `mutate`, or `clone`. `free`/`clone` reject dicts at type time — use `drop`/`dup` instead.

`lend` borrows a stackable snapshot from a Box. `BOX_UNPACK` copies the box's `Value` run bitwise, so what the snapshot shares with the box depends entirely on what is in it:

- **list / record / tuple / tagged content** — copied. Ints, floats, symbols and compound headers carry no heap pointer, so the snapshot is genuinely independent and survives a later `mutate` or even the box's `free`. `tests/expect.slap` ("lend snapshot independence after mutate") pins this. `let`-binding such a snapshot inside the body is allowed.
- **box or dict content** — aliased. Only the pointer is copied, and `mutate`'s `deep_free_values` frees the pointee while the binding still refers to it. The checker forbids `let`-binding a borrowed snapshot of these two types inside the `lend` body.

Only fires when the bound value carries the borrowed flag; binding a freshly-built tuple literal inside a lend body is fine. Use `k peek` for indexed reads without binding anything.

`deep_free_values` recursively frees boxes inside compounds (poisoning `BoxData->data=NULL` so any stale reference hits `double-free detected` rather than use-after-free). This catches rare TC gaps where a linear value escapes into a stackable compound that's later dropped.

**Tagging a linear value keeps it linear.** `apply_sig` marks a `TC_TAGGED` output `AT_LINEAR` when any consumed input was linear — `tag` is the only builtin shaped that way. Without it `42 box 'x tag` handed back a plain stackable, and `drop`, `dup`, `push` and `insert` all accepted it, laundering the box past every linear check. `42 box ok must free` still works: that is the same code path, consuming the box exactly once.

**A Box binding is single-use across all its lookups.** Looking one up is free — `lend` and `mutate` hand the box straight back, so the same name is read many times in ordinary code — but every lookup names the *same* heap cell, so only one may reach a word that retires it. The pushed value carries the binding's symbol in `AbstractType.sym_id`; `free`, `tcp-close` and `clone` check and set the binding's `consumed_line`. `swap` and the `tcp-send`/`recv`/`accept` family also declare `box own in` but hand the box back out, so they must *not* count — treating every `own in` as consumption rejects correct code (`euler/10`'s `lend … swap free`). `clone` does count: it returns the original alongside the copy. Identity rides through `swap` on the existing `src_sym` plumbing; it is lost through the tcp-* pair, whose box output slot is a container, so those are a known false-negative rather than a false-positive.

### Protocols (built-in typeclasses)

Constraints formalize which operations work on which types. Used in `[...] effect` annotations. Protocols live entirely in the type checker (`tc_constraint_matches`); no runtime dispatch.

| Protocol | Keyword | Types | Methods |
|----------|---------|-------|---------|
| Eq | `eq` | all stackable | `eq` |
| Ord | `ord` (implies Eq) | int, float | `lt`, `sort` |
| Num | `num` (implies Eq) | int, float | `plus`, `sub`, `mul`, `div` |
| Integral | `integral` (implies Num) | int | `mod`, `divmod`, `wrap`, bitwise |
| Semigroup | `semigroup` | list, tuple, record | `cat` |
| Seq | `seq` (implies Semigroup) | list | `get`, `peek`, `set`, `push`, `pop` |
| Sized | `sized` | list, tuple, record, dict | `len` |

Symbols are Eq-only (not Ord). Symbol ordering by intern id is an implementation accident, not a semantic — `lt`/`sort` on symbols is a typecheck error.

Additional constraint keywords recognized in effect annotations: `functor` (input constraint for `each`), `monad` (for `then`), `dict` (for the dict type), `linear` (parse alias for `box`).

`each` iterates over lists (producing a new list) and over `'ok`-tagged values (applies body to payload, re-wraps; non-ok passes through). `fold`, `filter` work on lists. These aren't surfaced as named protocols because they don't generalize beyond their current types. `then`/`default` are prelude-level sugar over `case` — see above.

Side-effect iteration: `(body) each drop`.

### `either` type annotation

Declares tagged variant types in effect annotations: `{'ok type 'no type} either`. Used to give precise types to fallible operations.

```
'pop ['a seq own in  'a seq move out  {'ok 'a 'no ()} either move out] effect
'read [list own in  {'ok list 'no list} either move out] effect
```

Supports type variables (`'a`) that resolve against the sig's other slots. An `either` slot in **input** position binds each variant's payload tvar from the incoming union (`tc_check_word`, the `s->either_count > 0` block in the `DIR_IN` loop). That is what makes `default` enforce that its fallback matches the `'ok` payload: `[1 2 3] pop () default` is a type error because `()` (tuple) doesn't match the list element type (int), and so is `[1 2 3] pop "str" default`.

The payload type is read off the incoming value's `UnionDef` (`tvars[...].union_id`), so it only fires when the payload type is known: `[] pop "str" default` still passes, because an empty list literal has no element type to conflict with.

Parsed in `parse_type_annotation`. Stored in `TypeSlot.either_syms/either_types/either_tvars`. Applied via `UnionDef` creation in `tc_check_word`.

**Producer-side validation** (`tc_check_body_against_sig`): when a `(body) [sig] effect 'name let` declaration has an either-constrained output, the body is scanned for literal `'sym tag` emissions and bare `ok`/`no`/`none`. Any tag not in the declared variant set is a type error. Also: `'name [sig] effect` followed by a later `(body) 'name let` reconciles the body's effect shape against the forward declaration.

**Exhaustiveness enforcement**: `case` with missing variant clauses on a union that carries a linear payload is a *hard* error (non-recoverable) — silent drop of a linear variant is always a bug. Unions without linear variants remain soft errors for easier recovery during editing.

List ops: `push`, `pop`, `get`, `peek`, `set`, `len`, `cat`. `compose` is a separate tuple-concat primitive for function composition.

### Indexed reads: `get` vs `peek` vs `nth`

Three ways to read element `i`, differing only in what happens to the container:

- **`get`** — `compound idx -- tagged`. Consumes the compound.
- **`peek`** — `compound idx -- compound tagged`. Leaves it. O(1) whenever every element is one slot (`compound_elem`'s `total_slots == len+1` fast path), regardless of list size.
- **`nth`** — `'sym idx -- tagged`. Reads a *bound name* zero-copy; the list never reaches the stack.

For a state machine threaded on the stack, `peek` to read and `set` to write are both O(1) and neither copies — so per-instruction cost stops scaling with the size of the state. Before `peek` existed the only way to read was `dup` (O(n) deep copy) or a `let` bind (O(n) frame copy); `examples/chip8.slap` got ~7x faster by dropping its `dup 'st let` snapshot.

`peek` needs the compound directly below the index, so chain reads by binding each one (`PC-I peek must 'pc let`) rather than stacking them.

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

Compiled with `-DSLAP_SDL`. 640×480 canvas, 2-bit grayscale. Primitives: `clear`, `pixel`, `fill-rect`, `on`, `show`. Event callbacks: `tick`, `keydown`, `keyup`, `mousedown`, `mouseup`, `mousemove` — registered `'event (handler) on`.
