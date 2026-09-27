- [ ] `--check` rejects exactly the type errors a word body can make.
  1. `(2 mul)` under `['a num lent in 'a num move out]` accepts a float and dies at runtime: literal types inside a body do not unify with the signature's variables.
  2. `apply`, `if` and named calls apply a tuple's effect three different ways. Routing all three through the scheme path gives a false positive on `(push) dip` in the prelude's `chunks`: the scheme's input positions ignore the value `dip` sets aside. Fix that before unifying.
  3. A bare forward declaration `'name [sig] effect` leaves `'name` on the runtime stack.
  4. A brace literal that starts like a record but has an odd count (`{'a 1 'b}`) becomes a tuple; the error surfaces later as "expected record, got tuple". Refuse it where it is written.
  5. A `case` whose clauses are plain values (`{'ok 1 'no 2}`) is rejected as "rec vs int", but runs: a matching value clause leaves the payload under the value. Refuse value clauses where they are written, since a clause body `(drop 1)` says the same thing.

- [ ] You read a record field the checker knows exists without `must`.
  1. The checker tracks record keys: a `{...}` literal and `into` add them.
  2. Decide the word: a total accessor the checker accepts only on a record known to have the key. `at` stays fallible for records of unknown shape.
  3. Replace `'key at must` in examples and libs (about 300 sites).

- [ ] You profile a slap program as a flame graph.
  1. Count and time each word in `dispatch_word` behind a `--profile` flag; print folded stacks to stderr at exit.

- [ ] A long-running server that makes closures per request holds steady memory.
  1. Frames made for escaping closures are never freed (about 0.5 KB each). Freeing them needs lifetime tracking for tuple envs: count references on copy, drop and trim.

- [ ] You run coreutils written in slap (cat, wc, head, grep -F, sort, uniq).
  1. Put them in examples/utils/; one runner diffs them against the system tools; reuse them as benchmarks.

- [ ] You run pico8/tic80 carts. Needs a Lua interpreter in slap first; decker or duskos are closer.

- [ ] You run your personal apps (snews, snail) in slap.

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
