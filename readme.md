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

With a terminal on stdin, `slap` is a shell. It prints the stack after each line, and a line that fails to check or to run is discarded: the stack, the names it bound and the sockets on the stack go back to what they were before it. Files it wrote and network I/O it did stay done. A box or socket may wait on the stack between lines, each line must close its own brackets, and the session's lines together hold at most a program's tokens. At the end of input the shell exits with the last line's status.

```
$ slap
> 1 2 plus
3
> dup 'x let
3
> x 0 div

-- ERROR <stdin>:3:5 ---------------------------------

    div: division by zero
...
3
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
  --headless   (SDL) run without a window: show runs the tick handlers and the render body each frame until a handler fails
  --profile    at exit, print one `word;word;word nanoseconds` line per call path to stderr
```

A run (not `--check`) dies before anything executes if the program names a word this build lacks: the SDL words in the terminal build, `tcp-*` in the wasm build. The scan reads every token, so a word in a branch that never runs is refused too.

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

A number is digits, with an optional leading `-` and `.digits`. It ends at a space or a bracket, so `5incr` and `0x10` are errors.

Stack manipulation:

```slap
3 dup                -- 3 3
4 1 drop             -- 4     (drop 1 item)
6 5 swap             -- 5 6
7 8 (1 plus) dip     -- 8 8   (apply under top)
(2 3 plus) apply     -- 5     (execute a tuple literal)
```

### floats

`plus`, `sub`, `mul`, `div`, `lt` and `sort` take ints. `fplus`, `fsub`, `fmul`, `fdiv` and `flt` take floats, and `fdiv` follows IEEE (`1.0 0.0 fdiv` is `inf`). `eq` takes any copyable type. `itof` and `ftoi` convert. `float-str` gives the shortest of `%.15g`, `%.16g` and `%.17g` that reads back as the same float, with a point or an exponent, or `nan`, `inf` or `-inf`. `float-bits` and `bits-float` convert between a float and its IEEE 754 binary64 bits as an int.

```slap
2.0 3.0 fplus        -- 5.0
9.0 fsqrt            -- 3.0
42 itof              -- 42.0
3.7 ftoi             -- 3
0.1 0.2 fplus float-str  -- "0.30000000000000004"
1.0 float-bits       -- 4607182418800017408
4607182418800017408 bits-float  -- 1.0
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

### control flow

```slap
-- if: condition then two branches
10 dup 5 lt (2 mul) (3 mul) if    -- 30

-- case: one clause per tag (see tagged unions)
5 'big tag {'big (2 mul) 'small (3 mul)} case  -- 10

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
3 add5 apply print     -- 8
7 add5 apply print     -- 12

('lo let 'hi let (dup lo le not swap hi lt and)) 'make-between let
10 1 make-between 'in-range let
5 in-range apply print   -- 1
15 in-range apply print  -- 0
```

A word that has made a body cannot then bind a value holding one with `let`: a body keeps the names of the word that made it alive, so the binding could keep itself alive and never be freed. Bind such a value before the word makes its first body, or in a word of its own.

A body passed as an input is a value: `apply` runs it, and the name passes it on as it is:

```slap
('pred let dup 0 gt (dup pred apply drop 1 sub pred countdown) () if) 'countdown let
5 (2 mod 0 eq) countdown  -- applies pred at each step, terminates at 0
```

## data types

### lists

```slap
-- creation
[]                          -- []
[1 2 3]                     -- [1 2 3]
0 5 range                   -- [0 1 2 3 4]

-- mutation (pop/get/set return tagged; must unwraps or panics on no)
[] 10 push 20 push            -- [10 20]
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
[3 1 2] sort                    -- [1 2 3]   (ints only)
[1 2 3] reverse                 -- [3 2 1]
[1 2 2 3 3] dedup               -- [1 2 3]
[10 20 30] 20 index-of must     -- 1

-- structural
[1 2 3] [4 5 6] zip         -- [[1 4] [2 5] [3 6]]
[[1 2] [3 4]] flatten        -- [1 2 3 4]
```

### tuples

Parenthesized code blocks. `apply` runs one.

```slap
(10 20 30) apply             -- pushes 10 20 30
```

### records

Key-value maps keyed by symbols.

```slap
{'x 10 'y 20}                -- record
{'x 10 'y 20} 'x at          -- 10
{'x 10 'y 20} (1 plus) 'x edit  -- {'x 11 'y 20}
{'x 10 'y 20} 30 'x into     -- {'x 30 'y 20}
{} 10 'x into 20 'y into    -- {'x 10 'y 20}
```

`at` and `edit` never fail: the checker proves the record has the key, and refuses the program otherwise. A record's type names its keys and the type of each value, `{'x int 'y int}`. `into` adds a key, or replaces the value of a key the record has. A word that reads `'k` from its input takes any record that has `'k`, so every caller must pass one. Records in one list, or left by the two branches of an `if`, have the same keys. The key is written as a literal right before `at`, `into` or `edit`; for keys that are data, use a dict.

### dicts

Maps from strings to values of one type.

```slap
dict "a" 1 insert "b" 2 insert  -- a dict of ints
"a" of must                     -- 1, with the dict still under it
drop "b" remove dict-keys       -- the dict and ["a"]
drop dict-entries               -- the dict and [{'key "a" 'value 1}]
```

`of` gives `value ok`, or `key no` for a key the dict does not have. `each` and `fold` take only lists. Iterate a dict with `dict-entries`: it leaves the dict and a list of its entries as records `{'key str 'value v}`, in no set order. `dict-entries nip 0 ('value at plus) fold` sums the values. To map the values, fold the entries into a new dict: `dict-entries nip dict (dup 'key at swap 'value at 10 mul insert) fold`. A bound dict is copied by each lookup, so two names never share one.

### strings

Strings are lists of bytes. A literal holds its UTF-8 bytes, which is what `read` and `tcp-recv` return, so a literal equals the same text read from a file. Escapes: `\n \t \\ \" \0`. `read`, `write` and `ls` fail with the path and the C library's reason: `"todo.json: No such file or directory" no`. strings.slap's `is-missing` (`msg path -- int`) tells a missing file from any other failure. `write` replaces a regular file whole: it writes a hidden temp file in the file's directory and renames that over the old one, so a crash, a full disk or a size limit leaves the old file. It follows a symlink and keeps the file's mode and group. Where a replacement could not match the old file (another owner, a second hard link, a directory you may not write) and for a device such as `/dev/null`, it writes in place, where a failure leaves a short file. ACLs and extended attributes are not kept.

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
123 'foo tag {'foo (1 plus) 'bar (2 mul)} case  -- 124
123 'zzz tag {'foo (1 plus) '_ (drop 0)} case   -- 0: '_ takes any other tag

-- monadic chaining with then/default
('d let 'n let
  d 0 eq ("division by zero" no) (n d div ok) if
) 'safe-div let

10 2 safe-div (3 mul ok) then -1 default   -- 15
10 0 safe-div (3 mul ok) then -1 default   -- -1
```

A `case` has a clause for every tag its value may carry, and the checker proves it at the case. A clause runs on its tag's payload. A last `'_` clause runs on the tagged value itself, for every tag the other clauses do not name. Without `'_`, every tag the value may carry needs a clause, and a value that may carry another tag is refused. A value whose tags are still open takes the clauses' tags as its own. So a word whose case has no `'_` takes only those tags, and a caller that passes another tag is refused at the call. A result case has an `'ok` and a `'no` clause, or a `'_` clause. When the value's tags are closed, a clause that can never run is refused: one for a tag the value never carries, or a `'_` after a clause for every tag.

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
  ((1 plus) 'count edit) mutate
  ((1 plus) 'count edit) mutate
  ((100 plus) 'total edit) mutate
  () lend
  dup 'count at 2 eq assert
  'total at 100 eq assert
free
```

A box stays on the stack from `box` to `free`: it cannot be bound with `let`, stored in a list, record or dict, duplicated or dropped. `lend` and `mutate` give it back. The body of `lend`, as the body of `mutate`, leaves exactly one value. A socket from `tcp-listen`, `tcp-connect` or `tcp-accept` follows the same rules and ends with `tcp-close`.

## type system

All code is type-checked before it runs. Types are inferred, so a program needs no annotations. The checker unifies types, as ML and Haskell do; when it cannot prove a program safe, it refuses the program.

A body's type is its stack effect: `(1 plus)` takes an int and leaves an int, and the rest of the stack stays as it was. A word defined from a body is generic: `(dup) 'twin let` works on any value it can copy. A value bound with `let` has one type, so a body the program made runs at one stack depth.

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

Every tag but `'ok` and `'no` has one payload type in the whole program: once `5 'n tag` appears, `'n` always holds an int. Recursive data goes through tags, as in `{} 1 'hd into nil 'tl into 'cons tag`. `ok` and `no` build results, which `then`, `pthen`, `default` and `must` take.

### ownership

A value is copyable unless it holds a box or a socket. Only a copyable value may be bound with `let`, duplicated, dropped, put in a list, record or dict, or left on the stack when the program ends. A body given to `each`, `fold`, `edit`, `mutate` or `lend` sees only its input, not the stack below it. A type variable carries one bit: copyable or linear.

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

The checker holds the body to the signature for every type the signature allows: `(2 mul) ['a lent in  'a move out] effect` is an error, because `2` makes the body int-only. A `lent` or `copy` slot takes a copyable value; `own`, `move` and `auto` slots may hold a box. A body type in a slot runs on the word's own stack below its declared inputs, so `(apply) [( -> int ) own in  int move out] effect` checks; a body type that names its own rest, as in `( ..x int -> ..x int )`, has a stack of its own.

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
| `neg` | int → -int (`fneg` for floats) | `5 neg` → `-5` |
| `abs` | int → \|int\| | `-3 abs` → `3` |
| `sqr` | int → int\*int | `5 sqr` → `25` |
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
| `fail` | ..a text → ..b | `n "bad size" fail` ends the program with the text; any stack may follow |

### float math

| Word | Effect | Example |
|------|--------|---------|
| `fneg` | f → -f | `3.0 fneg` → `-3.0` |
| `fabs` | f → \|f\| | `-2.5 fabs` → `2.5` |
| `fgt` | a b → a>b | `3.0 2.0 fgt` → `1` |

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
| `examples/lib/cbor.slap` | CBOR (RFC 8949) decoder and encoder over byte lists (requires `strings.slap` for `int-str`) |
| `examples/lib/http.slap` | `parse-http` (requires `strings.slap` for `crlf`) |
| `examples/lib/strings.slap` | `crlf`, `int-str`, `str-join`, `http-request`, `arg-count`, `arg-bytes`, `stdout-write`, `is-missing`, `log-line`, `accept-loop` |

`jd-run` and `xd-run` return `value ok`, or `msg no` for a syntax error as well as a shape error. A parse error names the byte offset from the start of the input and the byte found there: `json: expected , or ] in an array at byte 3, found '2'`. Neither library recurses per byte or per element, so input size is bounded by memory; nesting deeper than 256 levels is refused. `jd-run` refuses a leading zero (`0123`), a repeated key in one object, and a nonzero number that rounds to 0 or past the float range (`2e308`, `1e-400`; `0e999` is `0.0`); a float is one integer mantissa scaled once by a power of ten, so `0.3` equals the literal `0.3` (for up to 15 digits and a scale within 22; past that it is within a few ulps). `xd-run` refuses a repeated attribute name and a declared encoding other than UTF-8 or US-ASCII; the declaration is `<?xml` and white space, then version, encoding and standalone once each, and any other `<?xml...?>` is a processing instruction. `je-obj` dies on a repeated key, and `xml-render` dies on a repeated attribute name or a name the parser refuses. `jd-one-of` and `xd-one-of` list each alternative's error. Attributes are `{'name 'value}` records, in a parsed element and in `xe-elem`; `je-obj` takes `{'name 'value}` records too. `je-value` writes back what `jd-run` reads with the decoder `(ok)`: the same values in the same key order, so a program can change one field and keep the rest. The text can differ: `1e2` comes back as `100.0`, and `-0` as `0`. `rss-to-xml` gives `xml ok`, or `msg no` for a kind other than `"rss"` or `"atom"`. `jd-str` decodes `\u` escapes, surrogate pairs included, to UTF-8 and refuses a raw control byte; `je-str` escapes every byte under 0x20, so anything it writes reads back.

`cbor.slap` reads and writes the CBOR the lofi apps store. It follows json: `cd-run` (`bytes dec -- value ok | msg no`) runs the decoders `cd-int`, `cd-float`, `cd-str`, `cd-bytes`, `cd-bool`, `cd-null`, `cd-value` (any value), `cd-list`, `cd-field`, `cd-map`, `cd-one-of`, `cd-maybe`, `cd-succeed` and `cd-fail`. A value is a tagged `'int`, `'float`, `'str` (text string), `'bytes`, `'bool`, `'null`, `'list` or `'map`; a map is a list of `{'key str 'value cv}` records and a key must be a text string. `ce-int`, `ce-float`, `ce-str`, `ce-bytes`, `ce-bool`, `ce-null`, `ce-list`, `ce-map` (a list of `{'name 'value}` records, as `je-obj`) and `ce-value` (any decoded value) give byte lists. The encoder writes the shortest head for each int and length, and a float is always float64 (`float-bits`). The decoder reads float16, float32 and float64, and refuses what the apps never write: tags, indefinite lengths, simple values other than false, true and null, an integer past the int64 range, a repeated map key and trailing bytes. A refusal is a `msg no` with the byte offset and the byte found: `cbor: tags are not supported at byte 0, found byte 192`. A length larger than the bytes that remain is refused before anything is built, and nesting past 256 levels is refused. Save and read back with `write` and `read`: `path value ce-value write must drop` and `path read must (ok) cd-run`.

### networking / http

Built on `tcp-connect`/`tcp-send`/`tcp-recv`/`tcp-close` primitives plus `parse-http` from `examples/lib/http.slap`. `tcp-recv` gives up to n bytes, and at most 64 KiB per call. `tcp-recv` gives `'no` after 30 s with no data, `tcp-send` gives `'no` when it has not sent everything after 30 s, and `tcp-connect` gives `'no` after 10 s. `tcp-accept` waits without a limit, and skips a client that resets before it is accepted. strings.slap's `accept-loop` (`..s server name (..s client -- ..s continue) -- ..s server`) hands each client to a body until it leaves 0; it logs a failed accept, such as no free file descriptor, and stops after 100 in a row. `tcp-listen` binds 127.0.0.1 only, so a client on another machine cannot reach a slap server. `http-request` lives in `examples/lib/strings.slap`.

| Word | Effect |
|------|--------|
| `parse-http` | raw response bytes → `{'status 'headers 'body} ok`, or `message no` (from `http.slap`) |
| `http-request` | `method host path headers body → request-bytes` (from `strings.slap`) |

## SDL graphics

Build with `make slap-sdl`. Opens a borderless window at the size of the screen, with a canvas of one pixel per window point and 2-bit grayscale (4 shades: 0=black, 1=dark, 2=light, 3=white). The OS sets the window size, and no word changes it. The canvas starts cleared to 0. When you drag the window's edge, the canvas takes the new size and starts cleared to 0 again. The canvas exists once `show` starts, so `clear`, `pixel` or `fill-rect` before `show` is an error: draw in a handler or the render body. A program that draws for 640x480 fills only the top-left corner of a larger window, since `pixel` and `fill-rect` clip. Every SDL demo keeps the size from its `'resize` handler in its model and fills the window. `examples/gradient.slap` is the smallest example.

### primitives

| Word | Effect |
|------|--------|
| `clear` | Fill canvas with color (0-3); another color is an error |
| `pixel` | `x y color pixel` — set one pixel; color 0-3, a pixel off the canvas is clipped |
| `fill-rect` | `x y w h color fill-rect` — fill a rectangle; color 0-3, clipped to the canvas |
| `on` | `(handler) 'event on` — register event callback |
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
| `resize` | w h: the canvas size. `show` runs it once before the first tick and again after each change. A headless run sends `640 480` once. |

### example: Game of Life (abridged)

```slap
160 'W let  120 'H let  W H mul 'N let  4 'S let

('g let
  -- wrapped cell lookup; the grid stays let-bound and nth reads it by name,
  -- so the words that read it live where 'g is bound
  (H plus H mod W mul  swap W plus W mod  plus 'g nth must) 'cell let
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
  [] 0
  (dup N lt) (
    dup W divmod 'y let 'x let
    x y neighbors 'n let
    dup 'g nth must 1 eq (n 2 eq n 3 eq or) (n 3 eq) if
    (1) (0) if
    swap (push) dip 1 plus
  ) while drop
) 'step let

[] N (2 random push) repeat

(drop step) 'tick on
('sg let 0 clear
  0 (dup N lt) (
    dup 'i let
    i 'sg nth must 1 eq (i W mod S mul  i W div S mul  S S 3 fill-rect) () if
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
| `raycast.slap` | Grid raycaster: DDA per screen column, wall slices drawn as `fill-rect`s; past 640 columns, one ray covers a strip of columns |
| `zoom.slap` | Mandelbrot zoom in to 1e5 and back out: reuses the last keyframe's rows and columns, guesses smooth gaps, tweens between keyframes, 8x8 Bayer dither |
| `dots.slap`, `fish.slap`, `gradient.slap` | More graphics demos |

`chip8`, `uxn`, `maze`, `raycast` and `zoom` each come in two files: `x.slap` holds the machine and its self-test, and `x-sdl.slap` holds the window, keys and drawing. `on` and `show` run only at the top level, and no word ends a program early, so the shell is a separate file.

```bash
make slap-sdl
cat examples/chip8.slap examples/chip8-sdl.slap | ./slap-sdl roms/pong.ch8   # or no arg for the built-in demo ROM
./slap --headless < examples/chip8.slap                                      # run the opcode self-test (no SDL needed)
cat examples/uxn.slap examples/uxn-sdl.slap | ./slap-sdl game.rom            # likewise for the uxn emulator
cat examples/zoom.slap examples/zoom-sdl.slap | ./slap-sdl                   # the Mandelbrot zoom
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
| `kv-server.slap` + `kv-client.slap` | Persistent key/value store over TCP with a one-shot CLI client; the store holds at most 1,000,000 bytes of snapshot |
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
cat examples/lib/strings.slap examples/lib/parse.slap examples/lib/http.slap examples/fetch.slap | ./slap 127.0.0.1 8080 /readme.md -i

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
- A `case` that may meet a tag no clause names, unless a last `'_` clause takes it, and a clause that can never run. The checker infers which tags a value can carry from `ok`, `no`, `'x tag`, `then`, `pthen`, declared `either` types and the other cases on it.
- A body that breaks its declared signature for some type the signature allows.
- Code that takes more values than the stack holds.
- A word used before its definition without `'name [sig] effect`, and a declared word never defined.
- A name used where it is not bound. Names are lexical: a body sees the names bound where it is written, and never a caller's.

**Ownership**
- A box or socket that is bound, duplicated, dropped, stored in a container, or left at the end.
- A body given to `each`, `fold`, `edit`, `mutate` or `lend` that reaches below its input.

**Literals**
- A `[...]` or `{...}` literal is built each time the program reaches it: `3 'x let [x x plus]` is `[6]`, and `{'x x}` is `{'x 3}`. Its code starts on an empty stack, so it cannot take values from below the literal, and a name it binds is its own.
- A `{...}` literal is a record: each value follows its `'key`, and `{}` is the empty record. For code that pushes values, write a body: `(1 2)`. Right before `case`, a `{...}` literal is a clause list of `'tag (body)` pairs: write `(drop 1)`, not `1`.

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

Where SDL2 is installed, `make test` builds `slap-sdl` too and runs each SDL demo headless. Without SDL2 it only type-checks the demos, with a warning, and `make status` fails until SDL2 is installed.

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

`slap-wasm` writes `<name>.html`, `<name>.js`, and `<name>.wasm`, wrapping the program in `shell.html` (a canvas the size of the browser viewport; `SLAP_NAME` is substituted with the program name). It needs `emcc` on `PATH` — `brew install emscripten`, or install [emsdk](https://emscripten.org/docs/getting_started/downloads.html).

The build passes `-sGROWABLE_ARRAYBUFFERS=0`. With `ALLOW_MEMORY_GROWTH`, emscripten ≥6 backs the heap with a resizable `ArrayBuffer`, and browsers' `TextDecoder.decode()` rejects those — startup throws and the canvas stays blank.
