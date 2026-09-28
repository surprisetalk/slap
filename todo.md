- [ ] You read a record field with `'k at`; it never fails, because the checker proves the key.
  Decisions, made up front: inference only, no shape syntax. `edit` is total too. `'k at must` and `edit must` become errors at the end.
  `make status` counts the sites left ("No record read can fail"); every other program already reads with bare `at`.
  1. xml.slap and rss.slap (the last `at must` sites). Their nodes are records of two shapes in one list, built in `while`/`pthen` loops and read through `quote apply` decoders and the forward-declared `_xelem`/`_xapply`. Decide how the checker learns a node's keys, then tag nodes `'text`/`'elem` and read element fields in `{'elem (…)}` clauses:
     a. Tag rows program-wide: every `'T tag` payload must have every key some `{'T (…)}` clause reads (a whole-program check after `typecheck_tokens`' pass).
     b. Or: `then`/`pthen`/`default` and declared words carry payload maps (`vrow`) and record rows through their inferred scheme, not only their signature.
  2. A word's `let` rebinds in the frame the word was made in, so it shadows a top-level name for every word it calls: `'getx [int move out] effect ('x let getx) 'h let {'a 1} 'x let (x 'a at) 'getx let {'b 1} h` passes `--check` and dies in `at`. Decide: give each call its own frame at runtime (claude.md says frames chain lexical scopes), or have the checker refuse a word's `let` of a name some word it calls reads as a global.
  3. These pass `--check` and then die in `at` with "the checker proved it has". Each needs a failing case in the Soundness section of tests/errors.slap first.
     a. Branch joins meet only the values a branch leaves on top; one that replaces deeper values escapes the meet. Meet every position either branch consumes, and the default's for `case`:
        `{'a 1} {'x 1} 1 (drop {'y 1}) (drop drop {'b 1} {'y 1}) if drop 'b at`
        `{'a 1} 5 {'y 1} {(3 lt) (drop drop {'b 1} {'y 1})} case drop 'b at`
     b. A body that reads keys loses its effect and then runs unchecked: a word leaving it under another value, `quote` inside a body, a fold's initial value, a `while` whose other body is not literal, an `edit` body that applies one. Rather than one rule per path, give a tuple's tvar the key it reads, so `apply` of a tuple with no effect refuses it wherever it came from:
        `(('a at) 1) 'mk let {'b 1} mk drop apply`
        `('a at) 'f let {'b 1} ('f quote) apply apply`
        `{'b 1} [1] ('a at) (drop) fold apply`
        `('cond let {'b 0} 'cond quote ('a (1 plus) edit) while) 'run let (1) run`
        `('bd let {'a 1} {'k 0} 'k ('bd quote apply) edit drop 'a at) 'run let (drop drop drop {'b 1} {'k 0} 0) run`
     c. A value of unknown type (from `at must`, `quote`) that meets a known record takes its keys; `row_join` clears keys only for a type bound to `rec`. Mark values whose type is unknown, and let them make keys unknown too:
        `('k at must 1 () (drop {'b 1}) if 'b at) 'f let {'k {'z 1}} f`
        `{'z 1} 'r let 1 ('r quote) ({'b 1}) if 'b at`
     d. `on` handlers go unchecked when `show` runs inside a word, below the handlers' state, or with a render body that is not literal.
     e. A recursive call to a declared word does not forget the records below its inputs: `.unknown` is read from the placeholder while the body is still being checked.
     f. A trial passes a body fewer values than it takes (case predicates, `filter`, `lend`, `mutate`), and what the body replaces below keeps its keys: `{'a 1} {'b 1} box (swap) mutate free 'a at`.
     g. A trial of a body with unknown effect (inside `each`, `fold`, `case`, `mutate`) trusts what comes out: `[{'a 1}] (1 (drop {'b 1}) repeat) each 0 get must 'a at`.
     h. `if` meets what the other branch leaves, not values it passes through: `{'b 1} 1 () (drop {'a 1}) if 'a at`.
     i. A declared `either` is trusted as closed; a tag a called word emits gets through: `('z tag) 'mk let (mk) [int own in {'a int} either move out] effect 'f let 5 f {'b 1} {'a (drop {'a 1})} case 'a at`.
     j. A declared type-variable output is not checked against the body: `(drop {'z 1}) ['a own in 'a move out] effect 'f let {'a 1} f 'a at` (and `(drop 5)` for ints).
     k. A recursive `let` skips "already defined", so a program can redefine `ok`, `no` or a global the checker already relied on: `(0 (ok) (drop {'b 1} 'ok tag) if) 'ok let {'a 1} ok must 'a at`.
     l. Tuple `cat` keeps only the first body's effect: `{'z 1} (drop {'a 1}) (drop {'b 1}) cat apply 'a at`.
     m. `each` over a dict runs its body unchecked: `dict "k" {'a 1} insert (drop {'b 1}) each "k" of must nip 'zzz at`.
     Holes of the same kind that die elsewhere: an `if` condition's type is not checked (`"s" (1) (2) if`), a loop body that changes the stack's depth passes (`1 2 3 3 (drop) repeat`), and a box is freed twice through a body (`1 box (dup) apply free free`).
     Programs it refuses that it should accept: `then`/`pthen` bodies cannot read payload keys (`{'a 1} ok ('a at ok) then`), `nth` on a bound list of records, nested `repeat` over a record, and a `case` whose default cannot run is still type-compared with its clauses.
  4. A record that holds records of its own kind (a linked list: `(dup 'next at 0 eq ('v at) ('next at walk) if) 'walk let`) has no finite row, so the checker refuses it and points to `at must`. Decide: rows get recursive types, or `at must` stays for such fields and step 5 keeps it.
  5. When no `at must`/`edit must` remains: make them errors ("at never fails; drop the must"), drop `prim_at_must`/`prim_edit_must` and the unchecked runtime message, and remove the site count from `make status`.

- [ ] A long-running server that makes closures per request holds steady memory.
  1. Frames made for escaping closures are never freed (about 0.5 KB each). Freeing them needs lifetime tracking for tuple envs: count references on copy, drop and trim.

- [ ] You run a pico8 cart headless for N frames and its screen matches a reference render.
  Tradeoffs, decided up front: pico8 only; tic80 reuses the interpreter later. The screen has 4 greys, so 16 colours rank-map to 4 like uxn.slap. No audio: sfx/music are no-ops. Speed is ~190k uxn instructions/s, so carts run far below 30 fps; correctness first. Lua tables cannot be slap values (dicts cannot be let-bound, dup deep-copies), so all Lua state lives in one heap threaded on the stack.
  1. examples/lua.slap lexer: read the source by index with `nth`, as json.slap does. No per-character recursion.
  2. The parser emits a flat int bytecode list. A `while` + step loop runs it, like uxn.slap's run-more; the Lua call stack is data, not slap recursion.
  3. Heap: tables are int ids; entries live in one dict keyed by encoded `id:key` bytes, plus a key list per table for `pairs`. Closures are (proto id, upvalue ids).
  4. Numbers are pico8 16.16 fixed point in ints.
  5. tests/run_lua.py diffs `print` output against system `lua` on tests/lua/*.lua scripts that stay in integers.
  6. Cart loader for the .p8 text format: __lua__, __gfx__, __map__ sections.
  7. API subset: cls pset pget rectfill circfill line spr map btn print rnd flr sin cos; _init/_update/_draw.
  8. tests/run_pico8.py runs a small cart headless and diffs a palette-index pixel dump against a committed reference made with pico8's export (or zepto8).

<!--
- no vigil. nothing kept in intermediate state outside of physical notes and single working copy. publish sequels not incremental improvements.
- fullscreen apps only. starts with app launcher like ios.
  - slide apps left/right, the launcher is always leftmost. eventually, apps can take up partial width (full height) and slide around.
    - this works very nicely on mobile and desktop

implement lots of emulators: pico8, tic80, uxntal, duskos, decker, etc.

apps: launch, write, surf, watch, query, chat, talk

write the apps in slap, and then write an interpreter in swift that loads the roms

slap 0

slap.swift
launch.slap
write.slap
code.slap
surf.slap
query.slap
chat.slap
talk.slap
find.slap
debug.slap

file browser should be search based. sql or fql to find files instead of navigating dirs

step 1 is to move blog and all projects into sauce as slaps/scraps

also like the idea of making concurrent gofunc-esque threads with their own input queue and state

- taylor-town
  - pages (indexed)
  - assets (nonindexed)
- md editor
  - vim/leap movement
  - minimap
  - image preview
  - linters (like hemingway)
  - ai editing


TYPES

  i8, i16, i32, u8, u16, u32, f16, f32
  int, float, str
  'x box, 'x list, 'x slice, 'v 'k dict, 'v 'k dice, ['b 'a], [.. 'b 'a], {'k 'v}, {.. 'k 'v}

examples
  'a 1 def a 1 eq assert
  2 'b def b 2 eq assert
  3 dup eq assert
  4 1 drop 4 eq assert
  6 5 swap lt assert
  7 8 (1 plus) dip mul 64 eq assert
  9 (1 plus) apply 10 eq assert
  11 10 (10 eq) (dup mul) -1 if 121 eq assert
  12 11 {(11 eq) (dup mul)} -1 cond 144 eq assert
  13 'k {'k (dup mul)} -1 match 169 eq assert
  14 ((50 lt) (2 mul 1) (1 mul 0) if) loop 56 eq assert
  0 not assert
  1 1 and assert
  0 1 or assert
  (2) (2 mul) compose apply 4 eq assert
  list 0 give len 1 eq assert list eq assert
  list 0 give grab 0 eq assert list eq assert
  list 0 give 12 0 set 0 get 12 eq assert list eq assert
  stack 0 push size 1 eq assert stack eq assert
  stack 0 push pop 0 eq assert stack eq assert
  stack 0 "a" 12 0 put 0 pull "a" eq assert stack eq assert
  list 123 give box (0 get) lend 123 eq assert free
  list 123 give box ((1 plus) map) mutate 124 eq assert free
  list 123 give clone list 123 give eq assert free
  [] list eq assert
  () stack eq assert
  {} rec eq assert


123 'foo tag {'foo (1 plus)} -1 either 124 eq


[ succeed (#element)
    "<" symbol skip
    (isalphanum) chomp-while chomp-get keep
    spaces skip
    [] ('rev let ...) ploop
    spaces skip
    [ succeed []
        "/>" symbol skip
      succeed
        ">" symbol skip
        (drop children) lazy keep
        closing-tag
    ] one-of
  ("<" neq) chomp-while chomp-get
    (("" eq) (drop "expected text" problem) (#text succeed) if) pthen
]
one-of

---

- 001 why i built it
- 002 better api/patterns/idioms. write less code and build more dsls (e.g. elm encoders/decoders).
- 003 open #tag constructors? #ok #no and no panic? rethink apis? set? dict? threads? what other batteries do we need to include?
- 004 ui framework? like charm. also wysiwyg ui editor! build templates and components visually
- 005 graphics stack lang (sneeze? splat? spill?)
- 006 editor, surfer, filer, feeder, mailer, player, claude, hypocard via charmbracelet-like ui
- 007 query lang
- 008 running taylor.town from sauce os
- 009 off to scrapscript

--->
