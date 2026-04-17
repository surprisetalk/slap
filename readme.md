![FontBook](assets/fonts.png)

<p align="center">
  <img src="assets/life.gif" width="240" alt="Game of Life">
  <img src="assets/flock.gif" width="240" alt="Boids flocking">
  <img src="assets/ant.gif" width="240" alt="Langton's ant">
</p>

# slap

A stack-based programming language with static type inference and linear types. Single-file C99 interpreter (~3000 lines).

## install

```bash
brew install surprisetalk/tap/slap
```

Or build from source:

```bash
make slap
```

## quick start

```bash
echo '2 3 plus print' | slap        # → 5
slap < examples/euler/1.slap        # → 233168
```

For the SDL graphics build:
```bash
make slap-sdl                        # requires SDL2
slap-sdl < examples/life.slap
```

CLI:
```
slap [--check] [--headless] [args...] < file.slap
  --check      type-check only, no execution
  --headless   (SDL) run without a window, tick loop continues indefinitely
```

System primitives:
- `args` — pushes list of CLI positional args (strings)
- `isheadless` — pushes 1 if `--headless`, else 0
- `cwd` — pushes current working directory as string

## language tour

### arithmetic and stack

Values go on a stack. Words consume and produce values.

```slap
2 3 plus             -- 5
10 3 sub             -- 7
4 5 mul              -- 20
15 4 div             -- 3
15 4 mod             -- 3
```

Stack manipulation:

```slap
3 dup                -- 3 3
4 1 drop             -- 4     (drop 1 item)
6 5 swap             -- 5 6
7 8 (1 plus) dip     -- 8 8   (apply under top)
(2 3 plus) apply     -- 5     (execute a tuple literal)
```

### floats

```slap
2.0 3.0 plus         -- 5.0
9.0 fsqrt            -- 3.0
42 itof              -- 42.0
3.7 ftoi             -- 3
2.0 3.0 fpow         -- 8.0
1.0 flog             -- 0.0
```

### symbols

Atomic identifiers. Prefixed with `'`.

```slap
'hello               -- 'hello
'hello 'hello eq     -- 1
'hello 'world eq     -- 0
```

### booleans

`true` is `1`, `false` is `0`.

```slap
1 1 and              -- 1
0 1 or               -- 1
1 not                -- 0
3 5 lt               -- 1
5 3 gt               -- 1
3 3 eq               -- 1
```

### definitions

`let` takes value-then-name. On lookup, tuples auto-execute; other values push.

```slap
-- tuple binding: auto-executes on lookup (for functions)
(2 mul) 'double let
5 double              -- 10

-- scalar binding: pushes on lookup
42 'answer let
answer                -- 42
```

To bind a literal tuple as data (so lookup pushes it without running), wrap in extra parens — the outer tuple auto-execs and pushes the inner:

```slap
((1 2 3)) 'foo let
foo                   -- (1 2 3) on stack
```

To thread a bound closure through a nested call without auto-executing it, use `quote`:

```slap
'foo quote            -- pushes foo's raw value (no auto-exec)
```

`quote` is mainly used when passing a closure parameter into a recursive call — see [closures](#closures).

### control flow

```slap
-- if: condition then two branches
10 dup 5 lt (2 mul) (3 mul) if    -- 30

-- case: multi-way conditional with default (also dispatches on tagged values)
10 0 {(5 lt) (2 mul) (20 lt) (3 mul)} case  -- 30

-- while: loop
1 (dup 100 lt) (2 mul) while  -- 128
```

### recursion

A name is self-visible inside its own body, so recursion needs no keyword:

```slap
(dup 1 le (drop 1) (dup 1 sub factorial mul) if) 'factorial let
5 factorial            -- 120

(dup 1 le () (dup 1 sub fib swap 2 sub fib plus) if) 'fib let
10 fib                 -- 55

(dup 0 eq (drop) (swap over mod gcd) if) 'gcd let
12 8 gcd               -- 4
```

### closures

Functions capture their defining scope:

```slap
('n let (n plus)) 'make-adder let

5 make-adder 'add5 let
3 add5                 -- 8
7 add5                 -- 12

('lo let 'hi let (dup lo le not swap hi lt and)) 'make-between let
10 1 make-between 'in-range let
5 in-range             -- 1
15 in-range            -- 0
```

When a closure is passed as a parameter and needs to be handed down to a recursive call, use `quote` to push it as data rather than running it:

```slap
-- 'pred let binds a predicate; recursion passes it down via 'pred quote
('pred let dup 0 gt (dup pred drop 1 sub 'pred quote countdown) () if) 'countdown let
5 (iseven) countdown    -- applies pred at each step, terminates at 0
```

### composition

```slap
(2 mul) (1 plus) compose
3 swap apply              -- 7

-- chain multiple
(1 plus) (2 mul) compose (3 sub) compose (sqr) compose
5 swap apply              -- 81
```

## data types

### lists

```slap
-- creation
list                        -- []
[1 2 3]                     -- [1 2 3]
0 5 range                   -- [0 1 2 3 4]

-- mutation (pop/get/set return tagged; must unwraps or panics on no)
list 10 push 20 push            -- [10 20]
[10 20 30] pop must             -- 30 [10 20]
[10 20 30] 1 get must           -- 20
[10 20 30] 1 99 set must        -- [10 99 30]
[1 2] [3 4] cat                 -- [1 2 3 4]

-- slicing (clamped; no tagged result)
[1 2 3 4 5] 3 take-n            -- [1 2 3]
[1 2 3 4 5] 2 drop-n            -- [3 4 5]

-- higher-order
[1 2 3] (2 mul) each            -- [2 4 6] (map is spelled `each`)
[1 2 3 4 5] (2 mod 1 eq) filter -- [1 3 5]
[1 2 3] 0 (plus) fold           -- 6
[1 2 3] (plus) reduce           -- 6
[1 2 3] 0 (plus) scan           -- [1 3 6]

-- sorting and searching
[3 1 2] sort                    -- [1 2 3]
[1 2 3] reverse                 -- [3 2 1]
[1 2 2 3 3] dedup               -- [1 2 3]
[10 20 30] 20 index-of must     -- 1
[10 20 30] -1 (15 lt not) find  -- 20 (default -1 if no match)

-- structural
[1 2 3] [4 5 6] zip         -- [[1 4] [2 5] [3 6]]
[1 2 3 4] 2 windows         -- [[1 2] [2 3] [3 4]]
[1 2 3 4 5] 2 rotate        -- [4 5 1 2 3]
[[1 2] [3 4]] flatten        -- [1 2 3 4]

-- analysis
[30 10 20] rise              -- [1 2 0] (ascending rank)
[30 10 20] fall              -- [0 2 1] (descending rank)
[1 2 1 3 2] classify         -- [0 1 0 2 1]
[5 3 8 1 7] (4 lt) where    -- [1 3] (indices)
[10 20 30 40] [0 2 3] select -- [10 30 40]
```

### tuples

Immutable sequences. Parenthesized. Also used as code blocks. Access via destructuring (`apply`) or `len`.

```slap
(10 20 30) apply             -- pushes 10 20 30
(1 2 3) len                  -- 3
```

### records

Key-value maps keyed by symbols.

```slap
{'x 10 'y 20}                -- record
{'x 10 'y 20} 'x at must     -- 10 (at returns tagged; must unwraps)
{'x 10 'y 20} 30 'x into     -- {'x 30 'y 20}
rec 10 'x into 20 'y into    -- {'x 10 'y 20}
```

### strings

Strings are lists of Unicode codepoints. String literals are UTF-8 decoded at lex time — `len` counts characters, not bytes.

```slap
"hello" len                  -- 5
"hello" 0 get                -- 104
"héllo" len                  -- 5 (not 6)
"héllo" 1 get must           -- 233 (U+00E9)
"ab" "cd" cat                -- "abcd"
```

For byte-oriented I/O (file reads, TCP, binary formats) use `utf8-encode` / `utf8-decode` to convert between codepoints and UTF-8 bytes explicitly.

### tagged unions (sum types)

Tag a value with a symbol to create a sum type. Use `ok`/`no` for result types, or `'sym tag` for custom tags.

```slap
-- creating tagged values
123 ok                          -- 123 'ok tagged
"oops" no                       -- "oops" 'no tagged
42 'custom tag                  -- 42 'custom tagged

-- pattern matching with case on a tagged value
123 'foo tag 0 {'foo (1 plus) 'bar (2 mul)} case  -- 124
123 'zzz tag 0 {'foo (1 plus) 'bar (2 mul)} case  -- 0 (default fires; unmatched tag)

-- monadic chaining with then/default
('d let 'n let
  d 0 eq ("division by zero" no) (n d div ok) if
) 'safe-div let

10 2 safe-div (3 mul) then -1 default   -- 15
10 0 safe-div (3 mul) then -1 default   -- -1
```

`then` chains operations on `'ok` values — non-ok values pass through unchanged:

```slap
123 ok (1 plus) then (2 mul) then -1 default  -- 248
"fail" no (1 plus) then (2 mul) then -1 default  -- -1
```

### boxes (linear types)

Boxes wrap a value in a linear container that must be consumed exactly once.

```slap
42 box free                   -- box then immediately free

42 box (21 mul) lend          -- 882 (borrow a snapshot)
free                          -- must free when done

42 box (1 plus) mutate        -- modify in place
() lend 43 eq assert          -- verify
free

42 box clone                  -- two independent boxes
free free                     -- each must be freed
```

Realistic example — a mutable counter:

```slap
{'count 0 'total 0} box
  ('count (1 plus) edit) mutate
  ('count (1 plus) edit) mutate
  ('total (100 plus) edit) mutate
  () lend
  dup 'count at 2 eq assert
  'total at 100 eq assert
free
```

## type system

All code is type-checked before execution. Types are inferred — no annotations required.

### stackable vs linear

| Category   | Types                                                        | Rules                        |
|------------|--------------------------------------------------------------|------------------------------|
| Stackable  | Int, Float, Symbol, Tuple, Record, List, String, Tagged, Dict | Freely `dup` and `drop`      |
| Linear     | Box                                                          | Must consume exactly once    |

Boxes must be consumed via `free`, `lend`, `mutate`, or `clone`. `lend` borrows a stackable snapshot from a box. Dicts are stackable: use `dup` to branch and `drop` to discard. `free`/`clone` reject dicts at type time — boxes only.

### effect annotations

Optional type annotations declare stack effects:

```slap
(2 mul) [int lent in  int move out] effect 'double let
(dup mul) [int lent in  int move out] effect 'square let
```

Forward declarations register a signature against a name defined later (used when a function needs to reference itself through a mutually-recursive helper):

```slap
'xml-render-pretty [int lent in  rec own in  list move out] effect
-- ...later...
(... xml-render-pretty ...) 'xml-render-pretty let
```

Ownership modes: `lent` (borrowed/copyable), `move` (consumed), `own` (linear ownership).

### protocol constraints

Built-in protocols group types by capability. Use in effect annotations:

```slap
(len) ['a sized lent in  int move out] effect 'my-len let
(sort) ['a ord seq own in  'a ord seq move out] effect 'my-sort let
```

| Protocol | Keyword | Types | Operations |
|----------|---------|-------|------------|
| Sized | `sized` | list, tuple, record, dict | `len` |
| Seq | `seq` | list | `get`, `set`, `push`, `pop`, `cat` |
| Eq | `eq` | all stackable | `eq` |
| Ord | `ord` | int, float | `lt`, `sort` |
| Num | `num` | int, float | `plus`, `sub`, `mul`, `div` |
| Integral | `integral` | int | `mod`, `divmod`, `wrap`, bitwise |
| Semigroup | `semigroup` | list, tuple, record | `cat` |

Additional keywords recognized in annotations: `functor` (required by `each`), `monad` (required by `then`), `dict` (for the dict type), `linear` (alias for Box). Symbols are comparable with `eq` but not orderable.

## prelude

~70 definitions in slap itself, loaded at startup.

### stack

| Word | Effect | Example |
|------|--------|---------|
| `over` | a b → a b a | `1 2 over` → `1 2 1` |
| `nip` | a b → b | `1 2 nip` → `2` |
| `rot` | a b c → b c a | `1 2 3 rot` → `2 3 1` |
| `tuck` | a b → b a b | `1 2 tuck` → `2 1 2` |
| `not` | n → n==0 | `1 not` → `0` |
| `bi` | x f g → f(x) g(x) | `3 (2 plus) (3 mul) bi` → `9 5` |
| `keep` | x f → f(x) x | `5 (sqr) keep` → `25 5` |
| `repeat` | x n f → f^n(x) | `1 10 (2 mul) repeat` → `1024` |
| `times-i` | n f → f(0) f(1) ... f(n-1) | `3 (print) times-i` → prints 0 1 2 |

### arithmetic

| Word | Effect | Example |
|------|--------|---------|
| `inc` | n → n+1 | `5 inc` → `6` |
| `dec` | n → n-1 | `5 dec` → `4` |
| `neg` | n → -n | `5 neg` → `-5` |
| `abs` | n → \|n\| | `-3 abs` → `3` |
| `sqr` | n → n\*n | `5 sqr` → `25` |
| `cube` | n → n\*n\*n | `3 cube` → `27` |
| `max` | a b → max | `3 5 max` → `5` |
| `min` | a b → min | `3 5 min` → `3` |
| `sign` | n → -1/0/1 | `-3 sign` → `-1` |
| `clamp` | lo hi n → clamped | `1 10 5 clamp` → `5` |

### comparison

| Word | Effect | Example |
|------|--------|---------|
| `neq` | a b → a!=b | `3 5 neq` → `1` |
| `gt` | a b → a>b | `5 3 gt` → `1` |
| `ge` | a b → a>=b | `3 3 ge` → `1` |
| `le` | a b → a<=b | `3 5 le` → `1` |

### predicates

| Word | Effect | Example |
|------|--------|---------|
| `iszero` | n → n==0 | `0 iszero` → `1` |
| `ispos` | n → n>0 | `5 ispos` → `1` |
| `iseven` | n → even? | `4 iseven` → `1` |
| `isodd` | n → odd? | `3 isodd` → `1` |
| `divides` | a b → b%a==0 | `3 9 divides` → `1` |
| `isbetween` | n lo hi → in range? | `5 1 10 isbetween` → `1` |

### list utilities

| Word | Effect | Example |
|------|--------|---------|
| `sum` | list → total | `[1 2 3] sum` → `6` |
| `product` | list → product | `[1 2 3 4] product` → `24` |
| `max-of` | list → max | `[5 1 3] max-of` → `5` |
| `min-of` | list → min | `[5 1 3] min-of` → `1` |
| `first` | list → elem | `[1 2 3] first` → `1` |
| `last` | list → elem | `[1 2 3] last` → `3` |
| `member` | list val → bool | `[1 2 3] 2 member` → `1` |
| `couple` | a b → [a b] | `1 2 couple` → `[1 2]` |
| `flatten` | nested → flat | `[[1 2] [3 4]] flatten` → `[1 2 3 4]` |
| `table` | list f → [[x f(x)]...] | `[1 2 3] (sqr) table` → `[[1 1] [2 4] [3 9]]` |
| `select` | list indices → sublist | `[10 20 30] [0 2] select` → `[10 30]` |
| `reduce` | list f → result | `[1 2 3] (plus) reduce` → `6` |
| `keep-mask` | list mask → filtered | `[10 20 30] [1 0 1] keep-mask` → `[10 30]` |

### structural utilities

| Word | Effect | Example |
|------|--------|---------|
| `rotate` | list n → rotated | `[1 2 3 4 5] 2 rotate` → `[4 5 1 2 3]` |
| `zip` | a b → pairs | `[1 2 3] [4 5 6] zip` → `[[1 4] [2 5] [3 6]]` |
| `windows` | list n → sublists | `[1 2 3 4] 2 windows` → `[[1 2] [2 3] [3 4]]` |
| `reshape` | list [r c] → matrix | `[1 2 3 4] [2 2] reshape` → `[[1 2] [3 4]]` |
| `transpose` | matrix → transposed | `[[1 2] [3 4]] transpose` → `[[1 3] [2 4]]` |
| `group` | data indices → groups | groups data by index labels |
| `classify` | list → indices | `[1 2 1 3 2] classify` → `[0 1 0 2 1]` |

### tagged unions

| Word | Effect | Example |
|------|--------|---------|
| `ok` | x → x 'ok tagged | `42 ok` → `42 'ok tagged` |
| `no` | x → x 'no tagged | `"err" no` → `"err" 'no tagged` |
| `tag` | x 'sym → tagged | `1 'foo tag` → `1 'foo tagged` |
| `then` | tagged body → tagged | `42 ok (inc) then` → `43 'ok tagged` |
| `default` | tagged fallback → value | `42 ok -1 default` → `42` |

### float math

| Word | Effect | Example |
|------|--------|---------|
| `fneg` | f → -f | `3.0 fneg` → `-3.0` |
| `fabs` | f → \|f\| | `-2.5 fabs` → `2.5` |
| `frecip` | f → 1/f | `4.0 frecip` → `0.25` |
| `fsign` | f → -1.0/0.0/1.0 | `-3.0 fsign` → `-1.0` |
| `fclamp` | lo hi f → clamped | `1.0 10.0 5.0 fclamp` → `5.0` |
| `lerp` | a b t → interpolated | `0.0 10.0 0.5 lerp` → `5.0` |

### constants

| Word | Value |
|------|-------|
| `pi` | 3.14159265... |
| `tau` | 6.28318530... |
| `e` | 2.71828182... |

### strings

String primitives plus library helpers. Strings are lists of Unicode codepoints; `utf8-encode`/`utf8-decode` convert to/from UTF-8 byte lists. Higher-level helpers (`int-str`, `str-join`, `crlf`, `http-request`) live in `examples/lib/strings.slap` — cat it with your program: `cat examples/lib/strings.slap myprog.slap | slap`.

| Word | Effect | Example |
|------|--------|---------|
| `str-find` | haystack needle → `index ok` or `none` | `"hello world" "world" str-find must` → `6` |
| `str-split` | str delim → list of substrings | `"a,b,c" "," str-split` → `["a" "b" "c"]` |
| `utf8-encode` | codepoints → bytes | |
| `utf8-decode` | bytes → codepoints | |

From `examples/lib/strings.slap`:

| Word | Effect | Example |
|------|--------|---------|
| `str-join` | parts sep → joined | `["a" "b" "c"] "," str-join` → `"a,b,c"` |
| `int-str` | n → decimal string | `42 int-str` → `"42"` |
| `crlf` | → `"\r\n"` as byte list | |
| `http-request` | `method host path headers body → request-bytes` | |

### bitwise and byte utilities

| Word | Effect | Example |
|------|--------|---------|
| `byte-mask` | n → n & 0xFF | `300 byte-mask` → `44` |
| `byte-bits` | byte → 8-bit list | `5 byte-bits` → `[0 0 0 0 0 1 0 1]` |
| `bits-byte` | 8-bit list → byte | `[0 0 0 0 0 1 0 1] bits-byte` → `5` |
| `chunks` | list n → sublists of size n | `[1 2 3 4 5 6] 2 chunks` → `[[1 2] [3 4] [5 6]]` |

### binary format codecs

Decoders/encoders for compact binary formats. These live in `examples/lib/` as loadable libraries (not prelude) — cat the file with your program: `cat examples/lib/icn.slap myprog.slap | ./slap`. Each pairs a `*-decode`/`*-encode` that round-trip with the corresponding byte layout. Useful for tile graphics, tilemaps, fonts, and lightweight compression.

| File | Format |
|------|--------|
| `examples/lib/icn.slap` | 1-bit 8×8 tiles |
| `examples/lib/chr.slap` | 2-bit 8×8 tiles (two planes) |
| `examples/lib/nmt.slap` | Nametable cells (addr + color per 3 bytes) |
| `examples/lib/tga.slap` | Uncompressed true-color TGA images |
| `examples/lib/gly.slap` | ASCII-inline 1-bit glyphs |
| `examples/lib/ufx.slap` | Proportional bitmap fonts (requires `icn.slap`) |
| `examples/lib/ulz.slap` | LZ-compressed byte stream (decode only) |
| `examples/lib/parse.slap` | `parse-int`/`parse-float`/`parse-exact`/`parse-spaces`/`parse-while`/`parse-until` |
| `examples/lib/xml.slap` | Elm-style XML decoder |
| `examples/lib/rss.slap` | RSS/Atom feed parser (requires `xml.slap`) |
| `examples/lib/json.slap` | Elm-style JSON decoder (requires `parse.slap` and `strings.slap` for `int-str`) |
| `examples/lib/strings.slap` | `crlf`, `int-str`, `str-join`, `http-request` (formerly prelude) |

### networking / http

Built on `tcp-connect`/`tcp-send`/`tcp-recv`/`tcp-close` primitives plus `parse-http`. `http-request` lives in `examples/lib/strings.slap`.

| Word | Effect |
|------|--------|
| `parse-http` | raw bytes → `status headers body` |
| `http-request` | `method host path headers body → request-bytes` (from `strings.slap`) |

## SDL graphics

Build with `make slap-sdl`. Opens a 640x480 canvas with 2-bit grayscale (4 shades: 0=black, 1=dark, 2=light, 3=white).

### primitives

| Word | Effect |
|------|--------|
| `clear` | Fill canvas with color (0-3) |
| `pixel` | `x y color pixel` — set one pixel |
| `fill-rect` | `x y w h color fill-rect` — fill a rectangle |
| `on` | `(handler) 'event on` — register event callback |
| `show` | `(render) show` — start event loop with render function |

### events

| Event | Stack on callback |
|-------|-------------------|
| `tick` | frame-count |
| `keydown` | SDL keycode |
| `mousedown` | x y |
| `mouseup` | x y |
| `mousemove` | x y |

### example: Game of Life (abridged)

```slap
160 'W let  120 'H let  19200 'N let  4 'S let

(H plus H mod W mul swap W plus W mod plus nth) 'cell let

('cy let 'cx let 'gs let
  gs cx 1 sub cy 1 sub cell
  gs cx       cy 1 sub cell plus
  gs cx 1 plus cy 1 sub cell plus
  gs cx 1 sub cy       cell plus
  gs cx 1 plus cy       cell plus
  gs cx 1 sub cy 1 plus cell plus
  gs cx       cy 1 plus cell plus
  gs cx 1 plus cy 1 plus cell plus
) 'neighbors let

('g let  list  0 'i let
  (i N lt) (
    i W mod 'x let  i W div 'y let
    g x y neighbors 'n let
    g i nth must 1 eq (n 2 eq n 3 eq or) (n 3 eq) if
    (1) (0) if push
    i 1 plus 'i let
  ) while
) 'step let

list N (2 random push) repeat

(drop step) 'tick on
('g let 0 clear
  0 'i let (i N lt) (
    g i nth must 1 eq (i W mod S mul i W div S mul S S 3 fill-rect) () if
    i 1 plus 'i let
  ) while
) show
```

## examples

50 [Project Euler](https://projecteuler.net/) solutions in `examples/euler/`:

```slap
-- Euler #1: sum of multiples of 3 or 5 below 1000
1 1000 range
  (dup 3 mod 0 eq swap 5 mod 0 eq or) filter
  sum
print  -- 233168
```

```slap
-- Euler #6: sum-square difference for 1-100
1 101 range dup
(sqr) map sum 'sum-of-sq let
sum sqr 'sq-of-sum let
sq-of-sum sum-of-sq sub print  -- 25164150
```

Interactive SDL demos in `examples/`:

| File | Description |
|------|-------------|
| `life.slap` | Conway's Game of Life with mouse drawing |
| `flock.slap` | Boids flocking with mouse attraction and predator |
| `ant.slap` | Langton's ant cellular automaton |
| `snake.slap` | Snake game with arrow key controls |
| `dots.slap`, `fish.slap`, `gradient.slap`, `zoom.slap` | More graphics demos |

## libraries

Slap has no import statement. A program is whatever you pipe into `slap`, so you compose files by concatenating them:

```bash
cat lib.slap main.slap | slap
```

Order matters — definitions must appear before use. This works for remote libraries too:

```bash
curl -s https://example.com/lib.slap | cat - main.slap | slap
```

Or pre-fetch and cache:

```bash
curl -so lib.slap https://example.com/lib.slap
cat lib.slap main.slap | slap
```

## what the type system catches

The type checker runs on all code (builtins, prelude, user) before execution.
It catches the following at compile time:

**Linear resources (boxes):**
- A box must be consumed exactly once via `free`, `lend`, `mutate`, or `clone`.
- Embedding a box into a stackable container (list, tuple, record, tagged) is rejected.
- Duplicating a box with `dup` is rejected (boxes aren't copyable).
- A closure that captures a linear outer binding is marked linear itself — applying it twice is rejected.
- Higher-order ops (`each`, `fold`, `scan`, `while`, `find`) reject bodies that capture linear outer bindings.
- `compose` propagates linear-capture: merging a linear-capturing closure with a pure one yields a linear-capturing result.
- A linear-capturing closure cannot recurse on itself — each recursive call would re-consume the capture.

**Aliasing through `lend`/`mutate`:**
- `lend`'s body may not `let`-bind the compound-box snapshot as a word-accessible name — pulling the snapshot to the stack aliases the box's backing storage, and a later `mutate` would corrupt it.
- Indexed read access via `'name k nth` is exempt — nth doesn't pull the list to the stack.

**Tagged unions:**
- Tags are open by default; `case` on an untyped tagged value accepts any variant with a default clause.
- Declaring a closed schema via `{'sym 'type ...} union` enables exhaustiveness: `case` must cover every variant (hard-errored when any variant carries a linear payload, soft-warned otherwise).
- `either` in effect annotations (`{'ok int 'no str} either`) validates `tag` emission sites: the body may only emit variants named in the schema.

**Protocols (typeclasses):**
- `ord` (`lt`, `sort`) accepts int and float only. Symbols are Eq but not Ord — ordering symbols by intern id is an implementation accident, not a semantic.
- `num`, `integral`, `seq`, `semigroup`, `sized`, `functor`, `monad` gate other ops.

**Effect annotations:**
- `(body) [sig] effect 'name let` validates the body's stack shape against the declared signature.
- Forward declarations `'name [sig] effect` reconcile with the body when `name` is later defined.

**What the type system does *not* catch:**
- Division by zero, modulo by zero, out-of-bounds `set` or `nth` (runtime panics).
- Non-exhaustive `case` on a tagged value with no `union` declaration — the default clause silently fires on any unmatched variant (this is by design; declare a union to opt into exhaustiveness).
- Correctness of effect-annotation schemas (the user is trusted when they write `[sig] effect`; only the body's *shape* is validated, not its meaning).
- Recursion depth, memory limits, or other runtime resource exhaustion.

## testing

```bash
make test
```

Runs:
1. `cat examples/lib/strings.slap examples/lib/parse.slap tests/expect.slap | ./slap` — 250+ integration tests (assert-based)
2. `./slap --check < tests/type.slap` + `./slap < tests/type.slap` — type system validation
3. Same expect.slap stream re-run under `--check`
4. `python3 tests/run_panic.py` + `python3 tests/run_type_errors.py` — expected error messages
5. `python3 tests/run_euler.py` — 52 Project Euler solutions (strings.slap prepended)
6. Loadable libraries under `examples/lib/` — each run and type-checked in the combos it's designed for

Adversarial probes (`bash tests/adversarial/run.sh`) are a separate suite that classifies each probe as `TYPECHECK_REJECT` / `PANIC` / `CLEAN_RUN` — useful for spotting soundness regressions.

## building

Requires only a C99 compiler and `-lm`. No dependencies beyond SDL2 for the graphics build.

```bash
make slap          # terminal interpreter
make slap-sdl      # SDL2 graphics build (requires SDL2)
make slap-wasm FILE=examples/life.slap  # Emscripten/WASM build
make clean         # remove binaries
```
