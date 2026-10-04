![FontBook](assets/fonts.png)

<p align="center">
  <img src="assets/life.gif" width="240" alt="Game of Life">
  <img src="assets/flock.gif" width="240" alt="Boids flocking">
  <img src="assets/ant.gif" width="240" alt="Langton's ant">
</p>

# slap

A stack-based programming language with static type inference and linear types. Single-file C99 interpreter.

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
slap [--check] [--headless] [--profile] [args...] < file.slap
  --check      type-check only, no execution
  --headless   (SDL) run without a window, tick loop continues indefinitely
  --profile    at exit, print one `word;word;word nanoseconds` line per call path to stderr
```

`--profile` output is folded stacks. Recursion folds into one frame, and primitives are frames, so time spent in a body that `each` or `if` runs goes to the words in that body. Draw it with [FlameGraph](https://github.com/brendangregg/FlameGraph):

```bash
./slap --profile args < prog.slap 2> prog.folded
flamegraph.pl --countname=ns prog.folded > prog.svg
```

System primitives:
- `args` — pushes list of CLI positional args (strings)
- `isheadless` — pushes 1 if `--headless`, else 0

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

`let` takes value-then-name. A body written right before `'name let` defines a word: looking the name up runs it. Any other value bound by name is pushed when looked up; `apply` runs a body value.

```slap
-- a body written before its name: a word
(2 mul) 'double let
5 double              -- 10

-- any other value: pushed on lookup
42 'answer let
answer                -- 42
[(2 mul)] first 'twice let
5 twice apply         -- 10
```

A word whose body is a body pushes that body when it runs:

```slap
((1 2 3)) 'foo let
foo                   -- (1 2 3) on stack
```

`quote` pushes a word's body without running it:

```slap
(1 plus) 'inc1 let
'inc1 quote           -- (1 plus), pushed without running it
```

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

5 make-adder 'add5 let  -- a body made at runtime: a value
3 add5 apply           -- 8
7 add5 apply           -- 12

('lo let 'hi let (dup lo le not swap hi lt and)) 'make-between let
10 1 make-between 'in-range let
5 in-range apply       -- 1
15 in-range apply      -- 0
```

A body passed as an input is a value: `apply` runs it, and the name passes it on as it is:

```slap
('pred let dup 0 gt (dup pred apply drop 1 sub pred countdown) () if) 'countdown let
5 (iseven) countdown    -- applies pred at each step, terminates at 0
```

### composition

`cat` joins two tuples into one body:

```slap
(2 mul) (1 plus) cat
3 swap apply              -- 7

(1 plus) (2 mul) cat (3 sub) cat (sqr) cat
5 swap apply              -- 81
```

Two closures made in different frames join too. The joined body runs the first closure, then the second, and each one looks its names up in its own frame.

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
[10 20 30] 1 get must           -- 20            (consumes the list)
[10 20 30] 1 peek must          -- 20, with [10 20 30] still under it
[10 20 30] 1 99 set must        -- [10 99 30]
[1 2] [3 4] cat                 -- [1 2 3 4]

-- slicing (clamped; no tagged result)
[1 2 3 4 5] 3 take-n            -- [1 2 3]
[1 2 3 4 5] 2 drop-n            -- [3 4 5]

-- higher-order
[1 2 3] (2 mul) each            -- [2 4 6] (map is spelled `each`)
[1 2 3 4 5] (2 mod 1 eq) filter -- [1 3 5]
[1 2 3] 0 (plus) fold           -- 6

-- sorting and searching
[3 1 2] sort                    -- [1 2 3]
[1 2 3] reverse                 -- [3 2 1]
[1 2 2 3 3] dedup               -- [1 2 3]
[10 20 30] 20 index-of must     -- 1

-- structural
[1 2 3] [4 5 6] zip         -- [[1 4] [2 5] [3 6]]
[[1 2] [3 4]] flatten        -- [1 2 3 4]
```

### tuples

Parenthesized code blocks. `apply` runs one; `cat` joins two into one.

```slap
(10 20 30) apply             -- pushes 10 20 30
3 (1 plus) (2 mul) cat apply -- 8
```

### records

Key-value maps keyed by symbols.

```slap
{'x 10 'y 20}                -- record
{'x 10 'y 20} 'x at          -- 10
{'x 10 'y 20} 'x (1 plus) edit  -- {'x 11 'y 20}
{'x 10 'y 20} 30 'x into     -- {'x 30 'y 20}
rec 10 'x into 20 'y into    -- {'x 10 'y 20}
{'x 10 'y 20} {'x 5} cat     -- {'x 5 'y 20}
```

`at` and `edit` never fail: the checker proves the record has the key, and refuses the program otherwise. A record's type names its keys and the type of each value, `{'x int 'y int}`. `into` adds a key, or replaces the value of a key the record has. `cat` puts each field of the right record into the left one. A word that reads `'k` from its input takes any record that has `'k`, so every caller must pass one. Records in one list, or left by the two branches of an `if`, have the same keys. The key is written as a literal right before `at`, `into` or `edit`; for keys that are data, use a dict.

### dicts

Maps from strings to values of one type.

```slap
dict "a" 1 insert "b" 2 insert  -- a dict of ints
"a" of must                     -- 1, with the dict still under it
drop "b" remove dict-keys       -- the dict and ["a"]
```

`of` gives `value ok`, or `key no` for a key the dict does not have. `each` and `fold` give the body each entry as the record `{'key str 'value v}`, in no set order: `('value at 10 mul) each` maps the values, and `0 ('value at plus) fold` sums them. A bound dict is copied by each lookup, so two names never share one.

### strings

Strings are lists of bytes. A literal holds its UTF-8 bytes, which is what `read` and `tcp-recv` return, so a literal equals the same text read from a file. Escapes: `\n \t \\ \" \0`.

```slap
"hello" len                  -- 5
"hello" 0 get must           -- 104
"é" len                      -- 2 (two UTF-8 bytes)
"ab" "cd" cat                -- "abcd"
```

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

10 2 safe-div (3 mul ok) then -1 default   -- 15
10 0 safe-div (3 mul ok) then -1 default   -- -1
```

`then` runs its body on an `'ok` payload and passes anything else through; the body returns the next tagged value:

```slap
123 ok (1 plus ok) then (2 mul ok) then -1 default  -- 248
"fail" no (1 plus ok) then (2 mul ok) then -1 default  -- -1
```

### boxes (linear types)

Boxes wrap a value in a linear container that must be consumed exactly once.

```slap
42 box free                   -- box, then free it

42 box (21 mul) lend          -- the box, then 882: the body reads a copy
drop free

42 box (1 plus) mutate        -- the body replaces the contents
() lend 43 eq assert
free
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

A box stays on the stack from `box` to `free`: it cannot be bound with `let`, stored in a list, record or dict, duplicated or dropped. `lend` and `mutate` give it back. A socket from `tcp-listen`, `tcp-connect` or `tcp-accept` follows the same rules and ends with `tcp-close`.

## type system

All code is type-checked before it runs. Types are inferred, so a program needs no annotations. The checker unifies types, as ML and Haskell do; when it cannot prove a program safe, it refuses the program.

A body's type is its stack effect: `(1 plus)` takes an int and leaves an int, and the rest of the stack stays as it was. A word defined from a body is generic: `(dup) 'twin let` works on any value it can copy. A value bound with `let` has one type, but a bound body the program made may run at any stack depth. A caller's body may not, since it may read below its inputs, and neither may a body whose type a later `case` default or a tag's payload can still change: the checker refuses such a use.

### types

| Type | Value | In a signature |
|------|-------|----------------|
| int, float, symbol | `1` `1.5` `'a` | `int` `float` `sym` |
| list | `[1 2]` | `int list` |
| string | `"hi"` | `str` (a list of bytes: `int list`) |
| body | `(1 plus)` | `( int -> int )` |
| record | `{'x 1}` | `{'x int}`, or `{'x int \| 'r}` for "at least `'x`" |
| tagged | `5 'n tag` | `{'n int 'z ()} either` |
| result | `5 ok`, `"e" no` | `{'ok int 'no str} either` |
| dict | `dict` | `int dict` |
| box | `5 box` | `int box` |
| socket | `0 tcp-listen must` | `socket` |

Type variables are symbols: `'a list`. A signature is a promise for every type it allows, so a bare `list` means "a list of whatever element type the caller picks": a body that leaves `int list` must say `int list`.

Every tag but `'ok` and `'no` has one payload type in the whole program: once `5 'n tag` appears, `'n` always holds an int. Recursive data goes through tags, as in `rec 1 'hd into nil 'tl into 'cons tag`. `ok` and `no` build results, which `then`, `pthen`, `default` and `must` take.

### ownership

A value is copyable unless it holds a box or a socket. Only a copyable value may be bound with `let`, duplicated, dropped, put in a list, record or dict, or left on the stack when the program ends. A body given to `each`, `fold`, `edit`, `mutate` or `lend` sees only its input, not the stack below it.

### effect annotations

Optional type annotations declare stack effects:

```slap
(2 mul) [int lent in  int move out] effect 'double let
(dup mul) [int lent in  int move out] effect 'square let
```

A signature can also come first, as `'name [sig] effect`; the body later bound to that name is checked against it. A word used before its definition, as in mutual recursion, needs one:

```slap
'triple [int lent in  int move out] effect
(3 mul) 'triple let
```

The checker holds the body to the signature for every type the signature allows: `(2 mul) ['a num lent in  'a num move out] effect` is an error, because `2` makes the body int-only. A `lent` or `copy` slot takes a copyable value; `own`, `move` and `auto` slots may hold a box. A body type in a slot runs on the word's own stack below its declared inputs, so `(apply) [( -> int ) own in  int move out] effect` checks; a body type that names its own rest, as in `( ..x int -> ..x int )`, has a stack of its own.

### protocol constraints

Built-in protocols group types by capability. Use them in signatures, as in `['a ord list own in  'a ord list move out]`:

| Protocol | Keyword | Types | Operations |
|----------|---------|-------|------------|
| Num | `num` | int, float | `plus`, `sub`, `mul`, `div` |
| Ord | `ord` | int, float | `lt`, `sort` |
| Sized | `sized` | list, record, dict | `len` |
| Semigroup | `semigroup` | list, record, body | `cat` |
| Copy | `copy` | every type without a box or socket | `let`, `dup`, `drop`, `eq` |

Symbols are comparable with `eq` but not orderable.

## prelude

Definitions written in slap itself, loaded at startup.

### stack

| Word | Effect | Example |
|------|--------|---------|
| `over` | a b → a b a | `1 2 over` → `1 2 1` |
| `nip` | a b → b | `1 2 nip` → `2` |
| `rot` | a b c → b c a | `1 2 3 rot` → `2 3 1` |
| `not` | n → n==0 | `1 not` → `0` |
| `repeat` | x n f → f^n(x) | `1 10 (2 mul) repeat` → `1024` |

### arithmetic

| Word | Effect | Example |
|------|--------|---------|
| `inc` | int → int+1 | `5 inc` → `6` |
| `dec` | int → int-1 | `5 dec` → `4` |
| `neg` | int → -int (`fneg` for floats) | `5 neg` → `-5` |
| `abs` | n → \|n\| | `-3 abs` → `3` |
| `sqr` | n → n\*n | `5 sqr` → `25` |
| `max` | a b → max | `3 5 max` → `5` |
| `min` | a b → min | `3 5 min` → `3` |
| `sign` | n → -1/0/1 | `-3 sign` → `-1` |
| `clamp` | n lo hi → clamped | `15 1 10 clamp` → `10` |

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
| `iseven` | n → even? | `4 iseven` → `1` |

### list utilities

| Word | Effect | Example |
|------|--------|---------|
| `sum` | list → total | `[1 2 3] sum` → `6` |
| `first` | list → elem | `[1 2 3] first` → `1` |
| `last` | list → elem | `[1 2 3] last` → `3` |
| `member` | list val → bool | `[1 2 3] 2 member` → `1` |
| `couple` | a b → [a b] | `1 2 couple` → `[1 2]` |
| `flatten` | nested → flat | `[[1 2] [3 4]] flatten` → `[1 2 3 4]` |
| `reverse` | list → reversed | `[1 2 3] reverse` → `[3 2 1]` |

### structural utilities

| Word | Effect | Example |
|------|--------|---------|
| `zip` | a b → pairs | `[1 2 3] [4 5 6] zip` → `[[1 4] [2 5] [3 6]]` |

### tagged unions

| Word | Effect | Example |
|------|--------|---------|
| `ok` | x → x 'ok tagged | `42 ok` → `42 'ok tagged` |
| `no` | x → x 'no tagged | `"err" no` → `"err" 'no tagged` |
| `tag` | x 'sym → tagged | `1 'foo tag` → `1 'foo tagged` |
| `then` | tagged body → tagged | `42 ok (inc ok) then` → `43 'ok tagged` |
| `default` | tagged fallback → value | `42 ok -1 default` → `42` |

### float math

| Word | Effect | Example |
|------|--------|---------|
| `fneg` | f → -f | `3.0 fneg` → `-3.0` |
| `fabs` | f → \|f\| | `-2.5 fabs` → `2.5` |

### constants

| Word | Value |
|------|-------|
| `pi` | 3.14159265... |
| `tau` | 6.28318530... |

### time

Two clocks, because they answer different questions and neither substitutes for the other.

| Word | Effect | Example |
|------|--------|---------|
| `millis` | → milliseconds | `millis` → `1837402` |
| `datetime` | → 9-element list | `datetime` → `[2026 6 25 19 59 9 6 205 1]` |

`millis` is monotonic and has no epoch — only differences between two reads mean anything. Use it for timing.

`datetime` is local wall clock, already broken down, in this order:

| # | field | range |
|---|-------|-------|
| 0 | year | full, e.g. `2026` |
| 1 | month | `0`–`11` |
| 2 | day | `1`–`31` |
| 3 | hour | `0`–`23` |
| 4 | minute | `0`–`59` |
| 5 | second | `0`–`60` (60 is a leap second) |
| 6 | day of week | `0`–`6`, `0` = Sunday |
| 7 | day of year | `0`–`365` |
| 8 | is DST | `0` or `1` |

Fields are broken down in C because day-of-week and DST need the timezone database, which a Slap program cannot reach. The order is exactly Varvara's Datetime ports, which is what `examples/uxn.slap` indexes it as.

### strings

String primitives plus library helpers. Strings are byte lists. Higher-level helpers (`int-str`, `str-join`, `crlf`, `http-request`) live in `examples/lib/strings.slap` — cat it with your program: `cat examples/lib/strings.slap myprog.slap | slap`.

| Word | Effect | Example |
|------|--------|---------|
| `str-find` | haystack needle → `index ok` or `none` | `"hello world" "world" str-find must` → `6` |
| `str-split` | str delim → list of substrings | `"a,b,c" "," str-split` → `["a" "b" "c"]` |

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

Decoders/encoders for compact binary formats. These live in `examples/lib/` as loadable libraries (not prelude) — cat the file with your program: `cat examples/lib/icn.slap myprog.slap | ./slap`. Each pairs a `*-decode`/`*-encode` that round-trip with the corresponding byte layout. A decoder returns `value ok`, or `msg no` for input it cannot read, since its input comes from outside the program. Useful for tile graphics, tilemaps, fonts, and lightweight compression.

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
| `examples/lib/xml.slap` | Elm-style XML decoder (requires `strings.slap` for `int-str`) |
| `examples/lib/rss.slap` | RSS/Atom feed parser (requires `xml.slap`) |
| `examples/lib/json.slap` | Elm-style JSON decoder (requires `strings.slap` for `int-str`) |
| `examples/lib/strings.slap` | `crlf`, `int-str`, `str-join`, `http-request`, `arg-count`, `arg-bytes`, `stdout-write` |

`jd-run` and `xd-run` return `value ok`, or `msg no` for a syntax error as well as a shape error. Neither library recurses per byte or per element, so input size is bounded by memory; nesting deeper than 256 is refused. `jd-str` decodes `\u` escapes, surrogate pairs included, to UTF-8 and refuses a raw control byte; `je-str` escapes every byte under 0x20, so anything it writes reads back.

### networking / http

Built on `tcp-connect`/`tcp-send`/`tcp-recv`/`tcp-close` primitives plus `parse-http`. `http-request` lives in `examples/lib/strings.slap`.

| Word | Effect |
|------|--------|
| `parse-http` | raw response bytes → `{'status 'headers 'body} ok` |
| `http-request` | `method host path headers body → request-bytes` (from `strings.slap`) |

## SDL graphics

Build with `make slap-sdl`. Opens a 640x480 canvas with 2-bit grayscale (4 shades: 0=black, 1=dark, 2=light, 3=white).

### primitives

| Word | Effect |
|------|--------|
| `clear` | Fill canvas with color (0-3) |
| `pixel` | `x y color pixel` — set one pixel |
| `fill-rect` | `x y w h color fill-rect` — fill a rectangle |
| `on` | `'event (handler) on` — register event callback |
| `show` | `(render) show` — start event loop with render function |

### events

| Event | Stack on callback |
|-------|-------------------|
| `tick` | frame-count |
| `keydown` | SDL keycode |
| `keyup` | SDL keycode |
| `mousedown` | x y |
| `mouseup` | x y |
| `mousemove` | x y |

### example: Game of Life (abridged)

```slap
160 'W let  120 'H let  W H mul 'N let  4 'S let

('g let
  -- wrapped cell lookup; the grid stays let-bound and nth reads it by name,
  -- so the words that read it live where 'g is bound
  (H plus H mod W mul  swap W plus W mod  plus 'g swap nth must) 'cell let
  ('cy let 'cx let
    cx 1 sub cy 1 sub cell
    cx       cy 1 sub cell plus
    cx 1 plus cy 1 sub cell plus
    cx 1 sub cy       cell plus
    cx 1 plus cy       cell plus
    cx 1 sub cy 1 plus cell plus
    cx       cy 1 plus cell plus
    cx 1 plus cy 1 plus cell plus
  ) 'neighbors let
  list 0
  (dup N lt) (
    dup W divmod 'y let 'x let
    x y neighbors 'n let
    'g over nth must 1 eq (n 2 eq n 3 eq or) (n 3 eq) if
    (1) (0) if
    swap (push) dip 1 plus
  ) while drop
) 'step let

list N (2 random push) repeat

'tick (drop step) on
('sg let 0 clear
  0 (dup N lt) (
    dup 'i let
    'sg i nth must 1 eq (i W mod S mul  i W div S mul  S S 3 fill-rect) () if
    1 plus
  ) while drop
) show
```

## examples

[Project Euler](https://projecteuler.net/) solutions in `examples/euler/`:

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
(sqr) each sum 'sum-of-sq let
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
| `chip8.slap` | CHIP-8 emulator: runs real ROMs, 16-key hex keypad, sound-timer border flash |
| `uxn.slap` | Uxn/Varvara emulator: full 32-opcode CPU with all mode flags, System/Console/Screen/Controller/Mouse |
| `maze.slap` | Aldous-Broder maze generation and a BFS solve, both on one flat int list |
| `raycast.slap` | Grid raycaster: DDA per screen column, wall slices drawn as 1-pixel `fill-rect`s |
| `dots.slap`, `fish.slap`, `gradient.slap`, `zoom.slap` | More graphics demos |

`chip8`, `uxn`, `maze` and `raycast` each come in two files: `x.slap` holds the machine and its self-test, and `x-sdl.slap` holds the window, keys and drawing. `on` and `show` run only at the top level, and no word ends a program early, so the shell is a separate file.

```bash
make slap-sdl
cat examples/chip8.slap examples/chip8-sdl.slap | ./slap-sdl roms/pong.ch8   # or no arg for the built-in demo ROM
./slap --headless < examples/chip8.slap                                      # run the opcode self-test (no SDL needed)
cat examples/uxn.slap examples/uxn-sdl.slap | ./slap-sdl game.rom            # likewise for the uxn emulator
```

The whole machine — 4 KB of memory, registers, call stack, keypad, and the 64×32 display — is one flat int list threaded on the stack, with no boxes. `set` is an in-place O(1) store and `peek` an O(1) non-consuming read, so a `cycle` decodes and executes one instruction against the live state without ever copying it. That is what keeps the per-cycle cost independent of how big the machine is. The full opcode set (including the COSMAC shift/`FX55`/`FX65` quirks and `DXYN` sprite collision) is covered by an in-language self-test that runs on the plain terminal build.

`uxn.slap` is the same idea at 16× the scale: a 220191-cell machine holding uxn's full 64 KB address space, both 256-byte stacks, the device page, and two 320×240 screen layers. All 32 base opcodes are written once each — keep, return and short modes are handled by six state cells set during decode, so one `ADD` body serves all eight encodings. Varvara pixels are already 2-bit palette indices, which is exactly the canvas depth, so nothing is lost but hue; the palette is mapped by *rank* rather than absolute luminance so that four shades of one colour stay distinguishable. Audio and File are stubbed — the file header says why.

It is checked against another implementation rather than only against itself. `./slap --headless game.rom [frames] < examples/uxn.slap` boots a ROM with no window and writes the composited canvas to stdout as one palette index per pixel, with the four palette entries on a `PAL` line. Run against the nine ROMs in [mkeeter/raven](https://github.com/mkeeter/raven)'s snapshot suite — each at its own resolution, since the canvas geometry is six constants — eight match raven's reference renders **pixel for pixel**, including `screen_blending` (every blend mode × depth × flip) and `mandelbrot` (108864 pixels of pure integer arithmetic). The ninth, `piano`, differs by 22 pixels: an audio level meter, which is the missing Audio device showing through.

`make test-uxn-refs` runs that comparison. It is kept out of `make test` because it downloads the ROMs and reference renders from GitHub and the suite has to work offline; they are cached under `tests/.uxn-refs/`, so only the first run needs network. Run it after any change to the Screen path: uxn.slap's own self-test once passed while three real Screen bugs were live.

`make test-uxn-sweep` renders each ROM at 1, 60 and 120 frames and checks that static ROMs stay byte-identical and animated ones keep moving. That separates a ROM that is right from one that is right *at exactly 60* because two errors cancelled there.

`make bench-uxn` reports uxn instructions per second on `screen.rom` (pixel-exact, so it times correct work) and `drool.rom` (no reference render).

App demos (terminal build):

| File | Description |
|------|-------------|
| `wiki.slap` | HTTP wiki server: browse, edit, and link pages stored as flat text files |
| `kv-server.slap` + `kv-client.slap` | Persistent key/value store over TCP with a one-shot CLI client |
| `serve.slap` | Static file HTTP server: directory listings, MIME by extension, path-traversal refusal |
| `fetch.slap` | A small curl. Builds the request with `http-request` and reads the reply with `parse-http` |
| `feed.slap` | RSS 2.0 and Atom reader — the consumer for `xml.slap` and `rss.slap` |
| `todo.slap` | Todo list in a JSON file, decoded with `jd-*` and written with `je-*` |
| `banner.slap` | Text as ASCII art from a real `.uf1` bitmap font, via `icn.slap` |
| `plasma.slap` | Writes a 24-bit TGA. Float-only: there is no `sin`, so it builds one |

```bash
cat examples/lib/strings.slap examples/lib/parse.slap examples/wiki.slap | ./slap 8080 examples/wiki-pages
# then open http://localhost:8080/
```

The wiki serves one blocking connection at a time (HTTP/1.0, connection-close), parses requests with `parse.slap` combinators, escapes all user content, and rejects page names that could escape the pages directory. `[PageName]` in a page body becomes a link.

```bash
# terminal 1: start the store (port, snapshot file)
cat examples/lib/strings.slap examples/lib/parse.slap examples/kv-server.slap | ./slap 4321 kv.snap
# terminal 2: talk to it
cat examples/lib/strings.slap examples/lib/parse.slap examples/kv-client.slap | ./slap 4321 set greeting hello world
cat examples/lib/strings.slap examples/lib/parse.slap examples/kv-client.slap | ./slap 4321 get greeting   # -> VALUE hello world
```

```bash
# a static file server, and the repo's own client fetching from it
cat examples/lib/strings.slap examples/lib/parse.slap examples/serve.slap | ./slap 8080 .
cat examples/lib/strings.slap examples/lib/parse.slap examples/fetch.slap | ./slap 127.0.0.1 8080 /readme.md -i

# an RSS/Atom digest, and a todo list in JSON
cat examples/lib/strings.slap examples/lib/parse.slap examples/lib/xml.slap \
    examples/lib/rss.slap examples/feed.slap | ./slap examples/feeds/sample.xml
cat examples/lib/strings.slap examples/lib/parse.slap examples/lib/json.slap \
    examples/todo.slap | ./slap todo.json add buy milk

# a bitmap font as ASCII art, and a plasma written out as a real TGA
cat examples/lib/strings.slap examples/lib/icn.slap examples/banner.slap | ./slap SLAP
cat examples/lib/strings.slap examples/lib/parse.slap examples/lib/tga.slap \
    examples/plasma.slap | ./slap plasma.tga 64
```

Coreutils in `examples/utils/`: `cat`, `wc`, `head` (10 lines), `grep` (a fixed string, like `grep -F`), `uniq` and `sort` (byte order, like `LC_ALL=C sort`). Each reads the file named by its argument (grep takes a pattern, then a file), because the program itself arrives on stdin, and refuses any other number of arguments. `tests/run_utils.py` diffs each one against the system tool, and `make status` times `sort.slap` on 20,000 lines.

```bash
cat examples/lib/strings.slap examples/utils/sort.slap | ./slap words.txt
cat examples/lib/strings.slap examples/utils/grep.slap | ./slap needle words.txt
```

`serve.slap` decodes the request target before testing it, so `%2e%2e` and `..` are the same string by the time the rule sees them, and the rule is a byte allowlist rather than a denylist. `fetch.slap` is the one place `parse-http` is used as designed: it reads a *response*, where the number after the first space is the status — hand it a request line and it reports 0 and loses the method and path, which is why both servers here parse requests by hand.

The store keeps its data in a dict threaded on the stack and persists to a flat `key<TAB>value` snapshot on `SAVE`/`SHUTDOWN`. The protocol is one LF-terminated command per connection (`SET`/`GET`/`DEL`/`KEYS`/`SAVE`/`PING`/`SHUTDOWN`); malformed input, dead peers, and an unwritable snapshot are all reported without taking the single-threaded server down, while a corrupt snapshot is refused loudly at boot rather than silently pruned.

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

The checker runs on the prelude and the program before anything executes. It refuses:

**Types**
- A word given a value of the wrong type: `"a" 1 plus`.
- `at` or `edit` on a key the record may lack. The checker follows records through `let`, stack words, calls, branches, loops, lists and tag payloads.
- A list whose values differ in type, and `if` branches or `case` clauses that leave different types.
- A tag used with two payload types.
- A `case` that may meet a tag it does not name, when its default is not of the clauses' type. The checker infers which tags a value can carry from `ok`, `no`, `'x tag`, `then`, `pthen` and declared `either` types.
- A body that breaks its declared signature for some type the signature allows.
- Code that takes more values than the stack holds.
- A word used before its definition without `'name [sig] effect`, and a declared word never defined.
- A name used where it is not bound. Names are lexical: a body sees the names bound where it is written, and never a caller's.

**Ownership**
- A box or socket that is bound, duplicated, dropped, stored in a container, or left at the end.
- A body given to `each`, `fold`, `edit`, `mutate` or `lend` that reaches below its input.

**Literals**
- A `[...]` or `{...}` literal is built once, when the program is read. Its code sees only what is written inside it and the prelude. Build such a value at runtime instead: `list x push`, `rec x 'key into`.
- A `{...}` literal is a record when its values pair up as `'key value`, and a tuple otherwise; `{}` is the empty record. Right before `case`, a `{...}` literal is a clause list, and every clause body is `(...)`: write `(drop 1)`, not `1`.

**What it does *not* catch**
- Division by zero, out-of-bounds `set`, `nth` or `peek`, and `must` on a `'no` (runtime errors with a message).
- Recursion depth, memory limits, and other runtime resource exhaustion.

## testing

```bash
make test        # everything, in parallel, in a few seconds
make test-slow   # the slowest Euler problems
make status      # every likely failure mode, scored; 1.0 is the minimum pass
```

`tests/expect.slap` holds the assertions, `tests/errors.slap` every error a program can hit (each case names the message it must print), and `tests/run_*.py` drive the apps from outside: servers over real sockets, CLIs against real files, codecs against their file formats. `tests/suite.py` runs them all.

`.githooks/pre-commit` runs `make test` before each commit. Enable it once per clone:

```bash
git config core.hooksPath .githooks
```

## building

Requires only a C99 compiler and `-lm`. No dependencies beyond SDL2 for the graphics build.

```bash
make slap          # terminal interpreter
make slap-sdl      # SDL2 graphics build (requires SDL2)
make slap-wasm FILE=examples/life.slap  # Emscripten/WASM build (requires emcc)
make clean         # remove binaries and generated pages
```

`slap-wasm` writes `<name>.html`, `<name>.js`, and `<name>.wasm`, wrapping the program in `shell.html` (a 640×480 canvas; `SLAP_NAME` is substituted with the program name). It needs `emcc` on `PATH` — `brew install emscripten`, or install [emsdk](https://emscripten.org/docs/getting_started/downloads.html).

The build passes `-sGROWABLE_ARRAYBUFFERS=0`. With `ALLOW_MEMORY_GROWTH`, emscripten ≥6 backs the heap with a resizable `ArrayBuffer`, and browsers' `TextDecoder.decode()` rejects those — startup throws and the canvas stays blank.
