- [ ] You read a record field with `'k at`, and it never fails, because a real type system proves it.
  Decisions, made up front (2026-09-28): replace the flow checker with a type system in the Rust/Haskell sense. When in doubt the checker refuses what it cannot prove; false positives come second.
  - Types by unification. A body's type is its stack effect, `( ..s int -- ..s int )`. Records are rows `{l: t | r}` whose fields are present or absent (Rémy flags): `into` sets a key whether or not the record had it, as the runtime replaces a key it finds; `at`/`edit` need it present. (Corrected 2026-09-28: the runtime never repeats a label, so repeated-label rows made a loop that updates a field grow its type.) Branches and clauses must have equal types.
  - Tags: `'ok`/`'no` form a generic result type (`ok a | no b`). Every other tag is a constructor with one payload type program-wide; recursive data (xml trees, linked lists) goes through tags.
  - Lists hold one type. `if` branches are bodies.
  - L1: a name defined from a body written right before `'name let` (or `[sig] effect 'name let`) is a word, and a lookup runs it. Any other binding holds a value, and a lookup pushes it; `apply` runs a body value.
  - Keys are literal (2026-09-28, replacing S2): `at`/`into`/`edit` need the key written right before them, as `tag` needs its tag; a symbol's type is `sym`. S2 was unsound: symbols that meet must widen to `sym` (`key 'up eq`, `['a 'b]`, branches), and unification cannot tell that from passing an argument to a word that needs exactly `'z`. For keys that are data, use a dict.
  - The new checker is built beside the old one and compared on every example and test before the switch; breaker findings (scratchpad task5/task7/task8) become its tests.
  3. The spec, agreed 2026-09-28. Write it as the type table the new checker reads, in the signature syntax below.
     Types: `int` `float` `sym` `str` (= `int list`) `'a` `'a list` `'a dict` `'a box` (linear) `( int -- int )` (a body: its stack effect; the rest of the stack is implicit, `..s` names it) `{'name str 'age int}` (exact) `{'name str | 'r}` (open) `{'ok 'a 'no 'b} either` (result) `tagged` (the program's own tags).
     Rules: composition of word types; words from bodies written in place are generalized, values are not (boxes); recursion is monomorphic inside the body, except that each recursive call gets its own stack rest (`dup 1 sub f mul` calls `f` above one more value); a signature is rigid, including a bare `num` or `list`; `[…] effect` is checked with rigid variables; branches, clauses, loop bodies and container elements have equal types; records are rows of present/absent fields; `at`/`into`/`edit` take the key written right before them; `'ok`/`'no` build results, every other tag has one payload type program-wide; nothing at the top level takes more than the stack holds; ownership stays a separate check.
     Record `cat` is typed when every field of the right record is known: the result has the right record's fields over the left's. Bodies are functions: `len`, `get`, `each` and the like take lists, not bodies.
     Examples: `apply ( ..s (..s -- ..t) -- ..t )`, `if ( ..s int (..s -- ..t) (..s -- ..t) -- ..t )`, `each ( ..s 'a list (..s 'a -- ..s 'b) -- ..s 'b list )`, `'k at ( {'k 't | 'r} -- 't )`, `'k into ( {'k 'p field | 'r} 't -- {'k 't | 'r} )` (`field`: present or absent), `then ( {'ok 'a 'no 'b} either ('a -- {'ok 'c 'no 'b} either) -- {'ok 'c 'no 'b} either )`, `mutate ( 'a box ('a -- 'b) -- 'b box )`.
     Programs that change: json and xml decoders become bodies `( json -- 'a result )`, built by combinators, as Elm's are inside; the json value tree and xml nodes become tags; `if` branches that build records with different fields give both branches the same fields; tests that use a body as a sequence use a list.
  4. The new checker core: stack effects with row variables, scalars, bodies, words (let-polymorphism), recursion, annotations with rigid variables. A `make` target builds it in place of the old one; `make test` runs against both until the switch.
     Status: in slap.c under `#ifdef SLAP_NEXT`, built by `make slap-next`. `make status` scores it (see claude.md). Every program it refuses is a change listed below; of the errors.slap cases the old checker refuses, the ones it lets through are ownership (step 7) or rules the decisions retire (below).
     tests/next.slap holds the new checker's errors (errors.slap's format; `make test` runs it with `./slap-next`); at the switch its cases move into errors.slap. Breaker rounds 9-11 (scratchpad break9/-break11/) are in tests/next.slap. Their runtime findings wait for step 9: a word's or a body's `let` is visible to other words and bodies; a closure made in a word loses a name it uses only inside a `{...}` case clause list (the runtime's capture scan skips literals); `cat` of two closures over different frames dies ("close over different scopes") where the checker types a composition; `eval_tuple_scoped` moves a returned closure's names into a child frame only when the closure sits at the top of the stack above the word's inputs and was made in the word's own frame, so `( 'p let (p len) ) 'mk let "xyz" mk apply` loses `p` (the input was below the scan window), as does a closure inside a list, record, tag or box, or one a callee made.
     Known limit, as Haskell's runST: a body value used inside an `each`/`fold`/`edit`/`mutate` body and also on the real stack is refused, since the sealed rest would escape. Two sealed rests may join, so one body value serves several sealed calls.
     Sockets are their own type (`socket`), not `int box`: free, lend and mutate do not reach them. `each`/`fold` over a dict give a `(key value)` body until the switch. The checker seals the stack below the bodies of `each`, `fold`, `edit` and `mutate` (`..!r` in TYPES), since the runtime keeps other values there and counts them.
     `each`/`fold` pick the dict form only when they see a dict type at the call; in a word whose input is still unknown they take a list, so kv-server's `save-snapshot` needs a signature at the migration.
     Decided while building (no question needed): `while ( ..a (..a -- ..b int) (..b -- ..a) -- ..b )`, since a condition may leave a value the body consumes (euler/44); `x no must` and `none must` never return, only when `no`/`none` are the prelude's; a `case` default on the program's own tags runs only if the value may carry an unnamed tag, known when the enclosing word is inferred (an open set generalizes, so callers may pass any tag) or at the end of the program, where a still-open set closes; `cat` on two bodies composes them; user code never rebinds a name.
     Considered and declined 2026-09-28: one-step record construction (dropping `rec` growth and present/absent flags). Rows and `into` that adds a key stay.
     Decided 2026-09-28 after the first corpus run: programs keep the slot syntax `[int lent in  int move out] effect` (`--` starts a comment; only the internal table uses `( int -> int )`). `each`/`fold` over a dict give each entry as `{'key str 'value 'a}`, chosen by the type at the call; the runtime changes at the switch. `each` over a result goes away (use `then`). A `case` whose default has the clauses' type may leave tags unnamed: the default is a catch-all.
     Programs the new checker refuses, all by the decisions:
     - At the switch (the old checker refuses the fix): parse.slap's `parse-while-core` declares `tuple` and two unrelated list outputs. Signatures are rigid, so a `tuple` input cannot be applied, and a slot signature cannot name the word's own stack rest for a body type to share. Drop signatures the new checker infers (parse-while-core, xml.slap's), or let a body type's `..s` name the word's rest.
     - Dynamic scope: json.slap `_jc` reads `_jin`, xml.slap `_xc` reads `_xin` (step 8). Names are lexical: pass the value, or define the readers inside the word that binds it.
     - Bodies used as lists: expect.slap `(1 2 3) len`, `() len`.
     - Dict iteration: `each`/`fold` over a dict hand the body a `(key value)` body, and programs `get` from it (euler/49, kv-server, expect.slap).
     - expect.slap: tags reused with different payloads ('a 'b 'c 'x 'foo); a computed tag (`s tag`); `each` over a result; `default` on a non-result tag; result `case`s whose `()` default could run; record `cat` (step 5).
     - Retired errors.slap rules: "missing clause" when the default has the clauses' type (the default is a catch-all); the C-stack case (the new checker nests 1,600 bodies).
  5. Records: rows of present/absent fields, literal keys, `rec`/`into`/`at`/`edit`/literals/`cat`.
  6. Tags and results: constructors with one payload type each, `case` exhaustiveness, `ok`/`no`/`none`/`must`/`then`/`default`/`pthen`.
  7. Ownership is built in the new checker (tests/next.slap): one protocol, `copy`. A box, or a result or tag that holds one, is not copyable; the stack carries it from `box` to `free`. TYPES asks for `copy` where a primitive drops or duplicates an input, or a list, dict, box or record holds a value; `let`, `{...}` values, `into`/`edit`, a `case` whose default can run, predicate clauses, `show`'s top value, `lent`/`copy` signature slots and the program's last stack ask it too. The runtime lets a binding own a dict (`Binding.heap`: lookups deep-copy it, the binding frees it). Move semantics were declined: they add a second axis to every body type for kv-server's four socket bindings, which now stay on the stack.
     At the switch: errors.slap's box-binding and dict-binding cases become "cannot be let-bound" and disappear; claude.md's and readme.md's ownership rules follow.
  8+9. The switch, one change: the rewritten libraries do not pass the old checker, so they land with it. Order:
     a. json.slap: the parser's helpers move inside `_json-parse`, after `'_jin let`, so `_jin` is lexical. The value tree becomes tags ('obj {'key str 'value …} list, 'arr, 'str, 'num, 'bool, 'null). Decoders become bodies `( json -> 'a result )`: `jd-str`, `jd-field`, `jd-list`, `jd-map`, `jd-and-then`, `jd-one-of`, `jd-maybe` build them, `jd-run` applies one. todo.slap follows.
     b. xml.slap: the same for `_xin`; nodes become 'text str and 'elem {'name str 'attrs … 'children …}; the `dup len 3 eq` probes become cases; xml decoders become bodies like json's. rss.slap and feed.slap follow. `make status` times the feed: check it after each change.
     c. parse.slap: drop `parse-while-core`'s signature (the checker infers it). kv-server: `save-snapshot` gets a signature so its `fold` sees a dict.
     d. Runtime: `each`/`fold` over a dict give `{'key k 'value v}`; `each` over a result, and `len`/`get` on bodies, go; `at must`/`edit must` become an error ("at is total; drop must") and `prim_at_must`/`prim_edit_must` go; every `at must` site (`grep -c "at must"`) becomes `at`.
     e. Scoping: names are lexical at runtime. A word's or a body's `let` never changes what other words see, and a returned closure keeps every name it uses (the step-4 list above). Each gets an expect.slap case first.
     f. The new checker becomes the only one: delete `typecheck_tokens`, the tc_* code, BUILTIN_TYPES and the prelude's slot signatures it reads; drop `#ifdef SLAP_NEXT`, the `slap-next` target and the three status conditions that count the migration.
     g. Tests: tests/next.slap merges into errors.slap. Each errors.slap case either keeps an EXPECT the new checker or the runtime prints, gets the new message, or goes (a rule the decisions retired: box bindings, missing clauses with a typed default, the old literal-key and C-stack cases). expect.slap drops the retired features (bodies as lists, `each` on results, reused tag payloads, computed tags) and keeps what they tested where it still exists.
     h. Docs: readme.md's type-checker section describes the new checker (types by unification, rows, tags, `copy`, sealed bodies, rigid signatures, literal keys); claude.md's Checker and Records paragraphs follow.

- [ ] A long-running server that makes closures per request holds steady memory.
  1. Frames made for escaping closures are never freed (about 0.5 KB each). Freeing them needs lifetime tracking for tuple envs: count references on copy, drop and trim.
  2. A dict inside a list or record leaks when `len`, `eq`, `get`, `take-n`, `drop-n`, `index-of` or `at` discard the rest of the value: they drop slots without `deep_free_values`. Free what each primitive discards; `leaks --atExit` on breaker round 11's cases (scratchpad break11/) shows each one.

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
