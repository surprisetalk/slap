# Slap

A minimal concatenative language with linear types and row polymorphism.

```
-- sum of squares
[1 2 3 4 5] 0 (dup imul iplus) fold

-- mutable state via boxes
42 box 'counter def
counter (1 iplus) lend swap free inc assert

-- closures
'make-adder ('n let (n iplus)) def
3 make-adder 'add3 def
10 add3   -- 13
```

~54 primitives. 4 syntax forms. A type system that statically prevents use-after-free, double-free, and resource leaks — without garbage collection.

---

## Design

Values are split into **Stackable** (copyable) and **Linear** (must-consume):

| Stackable | Linear |
|-----------|--------|
| Int, Bool, Float, Symbol | Box |
| Tuple `(...)` | List |
| Record `{...}` | Dict |
| Slice `[...]` / `"..."` | String |
| Dice | |

Stackable values can be freely `dup`'d and `drop`'d. Linear values must be consumed exactly once via `free`, mutation, or explicit `clone`.

The bridge between worlds is **`lend`**: it borrows a stackable snapshot from a linear value, runs a body on it, then restores the linear value.

```
[1 2 3] list () lend swap free   -- List<...> [1 2 3]
42 box (1 iplus) lend swap free  -- Box<42> 43
```

---

## Syntax

```
42              -- integer
3.14            -- float
true false      -- booleans
'foo            -- symbol (interned, Copy)
"hello"         -- string slice (sugar for [104 101 108 108 111])

[1 2 3]         -- slice: evaluates contents, collects results
(2 imul)        -- tuple: pushes an unevaluated block
{'x 0 'y 0}    -- record: evaluates contents as symbol/value pairs

iplus           -- word: looks up name, executes or pushes
-- comment      -- to end of line
```

---

## Primitives

### Meta (2)

| Word | Stack Effect | Description |
|------|-------------|-------------|
| `def` | `'name val --` | Bind name; auto-executes tuples on lookup |
| `let` | `val 'name --` | Bind name; pushes value on lookup |

### Stack (4)

| Word | Stack Effect | Description |
|------|-------------|-------------|
| `dup` | `a -- a a` | Copy top (Stackable only) |
| `drop` | `a --` | Discard top (Stackable only) |
| `swap` | `a b -- b a` | Swap top two |
| `dip` | `a (body) -- [body] a` | Stash top, run body, restore |

### Control (5)

| Word | Stack Effect | Description |
|------|-------------|-------------|
| `apply` | `(body) -- [body]` | Execute a tuple |
| `if` | `bool (then) (else) -- [branch]` | Two-way conditional |
| `loop` | `(body) -- ...` | Repeat until body pushes false |
| `cond` | `val [(pred body)...] (default) -- [body]` | Multi-way conditional |
| `match` | `'key {k1 (b1) ...} (default) -- [body]` | Symbol dispatch |

### Bool (3)

`not`, `and`, `or`

### Compare (2)

| Word | Description |
|------|-------------|
| `eq` | Deep equality on scalars (Int, Bool, Float, Symbol) |
| `lt` | Less-than (Int, Float) |

### Math (11)

**Int**: `iplus`, `isub`, `imul`, `idiv`, `imod`
**Float**: `fplus`, `fsub`, `fmul`, `fdiv`
**Conversion**: `itof`, `ftoi`

### Tuples (3)

| Word | Stack Effect | Description |
|------|-------------|-------------|
| `compose` | `(a) (b) -- (a b)` | Join two tuples |
| `cons` | `(a) v -- (a v)` | Append value to tuple |
| `car` | `(a v) -- (a) v` | Extract last value (lossy for words) |

### Records (3)

| Word | Stack Effect | Description |
|------|-------------|-------------|
| `rec` | `(('k v)...) -- {k v...}` | Build record from tuple of pairs |
| `get` | `{...} 'k -- val` | Read field |
| `set` | `{...} val 'k -- {...'}` | Set field (returns new record) |

### Slices (4)

| Word | Stack Effect | Description |
|------|-------------|-------------|
| `len` | `[...] -- n` | Length |
| `fold` | `[...] init (fn) -- result` | Left fold |
| `at` | `[...] idx default -- val` | Element at index (default if OOB) |
| `put` | `[...] idx val -- [...]` | Replace element (returns new slice) |

### Dices (3)

| Word | Stack Effect | Description |
|------|-------------|-------------|
| `dice` | `[(k v)...] -- dice` | Create from slice of tuples |
| `grab` | `dice key default -- val` | Lookup with default |
| `ifsert` | `dice key val -- dice'` | Insert or update |

### Memory (3)

| Word | Stack Effect | Description |
|------|-------------|-------------|
| `lend` | `linear (body) -- linear result` | Borrow stackable snapshot |
| `clone` | `linear -- linear linear'` | Deep copy |
| `free` | `linear --` | Consume and deallocate |

### Box (1)

| Word | Stack Effect | Description |
|------|-------------|-------------|
| `box` | `val -- Box` | Wrap value in heap-allocated box |

### Lists (3)

| Word | Stack Effect | Description |
|------|-------------|-------------|
| `list` | `[...] -- List` | Create mutable list from slice |
| `list-concat` | `List [...] -- List'` | Append slice to list |
| `list-assign` | `List idx val -- List'` | Replace element |

### Dicts (3)

| Word | Stack Effect | Description |
|------|-------------|-------------|
| `dict` | `dice -- Dict` | Create mutable dict from dice |
| `dict-insert` | `Dict key val -- Dict'` | Insert or update |
| `dict-remove` | `Dict key -- Dict'` | Remove entry |

### Strings (3)

| Word | Stack Effect | Description |
|------|-------------|-------------|
| `str` | `[ints] -- Str` | Create mutable string from char codes |
| `str-concat` | `Str [ints] -- Str'` | Append |
| `str-assign` | `Str idx char -- Str'` | Replace byte |

### IO (4)

`print`, `print-stack`, `assert`, `halt`

---

## Build

`slap.c` is a C99 library. Frontends include it directly.

```bash
# Desktop (requires SDL2)
gcc -std=c99 -O2 $(pkg-config --cflags --libs sdl2) -lm sdl.c -o slap
./slap program.slap          # SDL window
./slap program.slap --test   # headless (one tick + render, then exit)

# WASM (requires emscripten)
./wasm.sh                         # default: examples/life.slap
./wasm.sh examples/program.slap   # specify program
cd web && python3 -m http.server 8080

# Tests
./run-tests.sh
```

## License

TBD
