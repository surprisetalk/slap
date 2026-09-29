- [ ] `eq` on a deeply nested value dies with a message, not a segfault (code review).
  `() 'nil tag 50000 (rec swap 'tl into 1 'hd into 'cons tag) repeat dup eq print` exits 139: `val_equal` now recurses once per nesting level. `index-of`, `member` and `dedup` use it too (breaker round 15 c1: 200000 deep, no output at all).
  1. suite.py or errors.slap first: the program above exits 1 with "C stack exhausted".
  2. `c_stack_check("while comparing a deeply nested value")` at the top of `val_equal`, as `val_print` does.

- [ ] A predicate `case` in a body that binds names frees that body's frame on each run (code review, leak).
  `('x let x 0 {(0 gt) (drop 1)} case) 'f let 0 N (drop 5 f) repeat` grows to 408 MB at N=1e6; a tag case stays at 4.7 MB. `eval_run` takes a frame reference when it pushes a `{...}` tuple, and `prim_case` pops the clause list with `POP_VAL`, so the reference is never dropped.
  1. Test first: run the loop at N=1e6 under `/usr/bin/time` and bound its peak memory, or check it with `leaks --atExit`.
  2. `prim_case`: `stage_body(asp-1)` right after `POP_VAL(clauses)` when the clauses are a tuple.

- [ ] A program that halts cannot leave a box or socket behind (code review; breaker round 15 a2, a3).
  `(dup 0 eq (drop halt) (1 sub 5 box swap f) if) 'f let 3 f` checks and exits 0 with three boxes on the stack: each recursive call's fresh stack rest (ty_define) loses the copy mark `halt` put on the word's rest. `5 box (halt) dip free` exits 0 too, the same with a socket: dip holds its value aside, and the halt rule sees only the body's stack.
  1. errors.slap first: both programs expect "int box is a box".
  2. The fresh `K_SVAR` for a recursive call takes `prot = ty[tin].prot`, as `ty_value` does.
  3. `dip`: when its body's stack rest carries the copy mark, the held value must be copyable too (and the same for any word that holds values aside while a body runs: check TYPES for others).

- [ ] Two closures that read different lists by `nth` or `quote` compare unequal (silent-failure audit; breaker round 15 a4: `dedup` keeps one of two).
  `('xs let ('xs 0 nth must)) 'mk let [1] mk 'a let [2] mk 'b let 'a quote 'b quote eq` gives 1, though `a apply` gives 1 and `b apply` gives 2; `index-of` finds the wrong one. `body_reads_names` counts only non-primitive words as name reads, and `nth`/`quote` are primitives that read the name written before them.
  1. expect.slap first: the case above, `eq not assert`.
  2. `body_reads_names`: an XT for `S_NTH` or `S_QUOTE` also reads a name.

- [ ] A body bound with let stays tied to anything that may still refine its stack rest (code review; breaker round 15 a1, c2; soundness).
  `ty_bound_rest` looks for the rest in the stack, the bodies being checked, the bindings and recursive calls, but not in tag payloads (one global type each) or in waiting cases (`ty_later` defaults and clauses). Breaker round 15 a1 (break15/f/ in the session scratchpad/a1.slap), which checks and dies "plus: type mismatch, got list and int":
      3 'b tag (swap 1 plus swap) {'a (drop (1 plus))} case 'f let
      "s" 5 f apply print drop
  a1b passes a caller's body as the case default; a1d goes through a tag payload. The code review's programs: This checks and dies "drop: stack underflow":
      ( 'x let  (x 0 {'n ('p let 1 (p apply) () if 0)} case) 0 drop 'h let  h apply drop ) 'w let
      (drop 5) 'n tag w
  A top-level form fails the same way: `('x let (x 0 {'n ('p let 1 (p apply) () if 0)} case) 0 drop 'h let h apply drop) 0 drop 'run let (drop 5) 'n tag run apply`.
  1. errors.slap first: both programs are refused.
  Decide first: scan payloads and waiting cases too (simple, but each let of a body already walks every binding's type, and a 188 KB program checks in 73 s, breaker c2), or generalize a bound body only after the cases in its scope settle, when nothing can refine it any more (no scans). Recommend the second.
  2. Whichever is chosen, a level test alone is not enough: it misses the top-level form.
  3. readme's "a bound body may run at any stack depth" is too broad: say a body the program made may, and a caller's body may not. claude.md's Checker paragraph follows.

- [ ] Dropping a long chain of closures frees it without deep C recursion (code review, crash).
  `(0) 100000 ('c let (c apply 1 plus) 'g let 'g quote) repeat drop` exits 139 (60000 links pass): `frame_drop` → `frame_trim` → `binding_release` → `frame_drop` recurses once per link. The comment above `frame_drop` claims a loop, which holds only for parent links.
  1. suite.py first: the program above exits 0.
  2. Frames that reach zero go on a work list, released in one bounded loop; fix the comment.

- [ ] Record `cat` replaces a key both records have (breaker round 15 a5).
  `{'a 1 'b 2} {'a 5} cat len` gives 3: `prim_concat` appends the right record's fields, so the left's 'a stays beside the new one, and `eq`, `dedup` and `print` see the stale field. The checker types the result as the right record's fields over the left's.
  1. expect.slap first: `{'a 1 'b 2} {'a 5} cat {'a 5 'b 2} eq assert` and `len 2`.
  2. `prim_concat` on records: `rec_put` each right field into the left record.

- [ ] parse-int refuses a number past the int range (breaker round 15 a6).
  It wraps past 2^63-1: `todo.slap done 18446744073709551617` marks item 1 done, and a port of 2^64+n passes as n in kv-server, serve, wiki and fetch.
  1. expect.slap first: `"9223372036854775808" ok parse-int` is 'no; the 2^63-1 edge still parses.
  2. parse.slap's digit loop checks for overflow before each multiply-add and returns `msg no`.

- [ ] todo.slap and kv-server start empty only when their file does not exist (silent-failure audit, critical).
  `read` gives `path no` for every failure, so a directory, a mode-000 file or an I/O error reads as "no file": todo.slap lists nothing and exits 0, and with a write-only file `add` replaces the items. kv-server's boot `save-snapshot` then writes the empty store back.
  Decide first: `read` reports why (errno text in the 'no payload, e.g. "path: not found"), or programs check existence with `ls` of the parent. Recommend the errno text: one change in `prim_read`, and every caller can tell.
  1. tests/run_todo.py and run_kv first: a directory, a mode-000 file and a write-only file each exit nonzero naming the path; the file keeps its bytes.
  2. todo.slap:47 and kv-server.slap:53: start empty only on "not found"; anything else dies with the path and the reason.

- [ ] A decoder tells a missing optional field from a present but malformed one (silent-failure audit, critical).
  `jd-maybe`/`xd-maybe` turn every failure into `none ok`: `{"a":"x"}` with `"a" jd-int jd-field jd-maybe` gives none. rss's `_rss-opt-text` (`xd-child xd-maybe ("" default)`) makes `<title>A <b>bold</b> post</title>` and Atom `<content type="xhtml">` give "", and lets an RSS channel without its required title, link or description parse; rss-to-xml then writes `<pubDate></pubDate>` and `<link href=""/>`.
  Decide first: jd-maybe/xd-maybe give `none` only for "missing field"/"no such child" and pass every other 'no on (Elm's `maybe` swallows all errors; `optionalField` does not); rss keeps absent optional fields as `none`, not "". Text with element children: take the text of xhtml content, or refuse it.
  1. Tests first: the three inputs above, and a channel without a title.
  2. jd-field/xd-child failures carry a distinct 'missing payload shape (or message prefix) that the maybe decoders test.
  3. rss.slap: required fields use xd-child without maybe; rss-to-xml skips absent fields.

- [ ] rss-parse takes an Atom feed's alternate link (silent-failure audit).
  `_atom-link` takes the first `<link>`: a `rel="self"` link first gives the feed's own URL, and a `rel="replies"` link first gives the comments URL. A first link without href gives "", even when a later one has it.
  1. rss.slap tests first: both feeds above.
  2. `_atom-link`: the first link whose rel is "alternate" or absent; none such is `none`.

- [ ] Decoder and parser errors say where and what (silent-failure audit).
  1. `jd-one-of`/`xd-one-of` say only "oneOf: no decoders matched": list each alternative's error, "oneOf: [0] .a: expected int, got string; [1] ...".
  2. Parse errors ("json: expected , or ] in an array", "json: trailing input", "xml: expected <", ...) give neither position nor byte: add the byte offset and the byte found. On a 600 KB feed there is no other way to find the fault.
  3. Tests: one pinned message each in json.slap and xml.slap.

- [ ] The JSON and XML parsers refuse what their standards call malformed (silent-failure audit).
  Today they accept: leading zeros (`"0123"` gives 123), duplicate object keys (first wins), duplicate attributes (first wins), an `encoding="ISO-8859-1"` declaration (its bytes read as UTF-8), and a float past the range (`"2e308"` gives `inf ok`). `"0.3"` does not equal `0.3`: the fraction is built as digit x 0.1^n.
  1. One failing test per case in json.slap/xml.slap.
  2. json: refuse a leading 0 before a digit, a repeated key in one object, and a non-finite result; build the fraction as an integer mantissa and scale it once.
  3. xml: refuse a repeated attribute name and any declared encoding but UTF-8 or US-ASCII.

- [ ] rss-to-xml and xml-render refuse values they cannot write (silent-failure audit).
  rss-to-xml writes RSS for any 'kind but "atom" ("Atom" too); xml-render and xml-render-pretty render an unknown node tag as "".
  1. Tests first; then rss-to-xml accepts "rss" or "atom" only, and the renderers' case default dies with the tag it met.

- [ ] You apply a body while a copy of it is on the stack, and `cat` bodies in a word (breaker round 15 b1, b2; decision needed).
  `(5) dup apply print drop` is refused: `dup` gives both copies one type, and applying one changes the depth below the other. This caused nearly all of the 145 refusals among 66,000 generated well-typed programs. `( cat ) 'c let (1 plus) (2 mul) c` is refused, though the readme lists body as a Semigroup; making cat generic over bodies as it stands would be unsound.
  Decide first: let `dup` (and every copy of a body value) give the copy its own stack rest when nothing else holds it, as `let` does now; and type `cat` on bodies as composition in the word's own type, or drop body from the readme's Semigroup row.

- [ ] `'k at must` on a field that holds a result checks (breaker round 15 b4).
  `{'r 5 ok} 'r at must` is refused by token adjacency, though the field is a result and must is right.
  1. errors.slap/expect.slap first: that program passes; `{'r 5} 'r at must` still says "at never fails ... Drop must".
  2. Drop the adjacency rule; when `must` after `at`/`edit` fails to unify with a result, add "at never fails: drop must" to that error.

- [ ] lend runs a body that applies a word's input (breaker round 15 b3).
  lend refuses such a body with "lend's body may not take values below the box's contents", which is misleading; `mutate` and `each` accept the same body.
  1. break15/f has the program: test it passes.
  2. ty_lend: collect the body's outputs after unifying its type, not before the input body's effect is known; reword the message.

- [ ] You `cat` two closures made in different frames, and the joined body runs.
  Today `cat` dies with "close over different scopes", though the checker types the join as a composition.
  1. In `prim_cat`, when the two tuples' `env` differ, build a tuple with no env of its own: the first closure as a value, `apply`, the second as a value, `apply`. Each inner tuple keeps its frame reference.
  2. expect.slap: two closures from two calls of one word, joined, give the composed result.

- [ ] A runtime error about a deeply nested value prints its message once.
  "C stack exhausted printing a deeply nested value" prints twice: the stack dump prints the value again, fails again, and is cut off.
  1. The dump shows a value too deep to print as `...` at the depth limit, instead of recursing into it.
  2. errors.slap: the 20000-deep `'n tag` case expects the message and no second copy.

- [ ] A type-annotation error puts its caret on the annotation.
  Today `'x [strng lent in] effect` reports "at line 1" in the text, with the caret at column 1.
  1. Pass the annotation's token to the error, as `ty_err` does for words, so the caret and line come from it; drop the "at line %d" text.
  2. errors.slap: EXPECT-COL on an unknown type word.

- [ ] A `filter` predicate sees only the element it tests.
  Today the prelude's filter runs the predicate inside a fold, so `[5 6 7 8] (drop over len 2 lt) filter` reads filter's own accumulator and gives [5 6]. It is type-safe but leaks the implementation.
  1. filter becomes a primitive, a loop like `prim_each`: copy each element, run the predicate, keep the element when the predicate leaves a nonzero int (`one_value_above` checks it leaves one value).
  2. TYPES: `'filter ( ..s 'a list ( ..!r 'a -> ..!r int ) -> ..s 'a list )`; delete the prelude definition.
  3. errors.slap: the predicate above is refused ("this body must turn its inputs into one value").

- [ ] A type error names each variable once across its lines (breaker round 14).
  `'apply' takes ( ..a -> ..b ) / but the stack has ... / <why>`: the "takes" line shows a fresh copy of the word's type, so its names ('a, ..a) are not the ones the why line uses. ty_define and the case messages already print their types before they unify.
  1. In ty_apply, print the word's instance and the stack's top before `ty_unify`, with `ty_print_count` reset once. It is the hottest path, so print into the buffers only when a cheap pre-check fails, or keep a copy of the instance and print it after.
  2. Inside a `[...]` literal, `1 [drop]` says "'drop' takes 'a copyable / but the stack has nothing"; say that a literal's code starts from an empty stack.
  3. errors.slap: pin one apply message whose why line names a variable from the takes line.

- [ ] You put a symbol that `nth` used into a list with other symbols (breaker rounds 14 and 15, decision needed).
  `[1 2] 'xs let 'xs 'k let k 0 nth must drop [k 'ys] drop` is refused: nth fixes k's type to exactly 'xs, and a list of symbols widens its element type to sym. `k 'ys member` is refused the same way. eq and neq compare without widening, so they pass.
  Decide first: keep the refusal (nth by a symbol read from a list is rare), or let a symbol type carry both its exact label for nth and a widened copy for containers.

- [ ] The readme says a signature passes a value through unchanged only when both slots name one variable (breaker round 14).
  `[tagged own in  tagged move out]` is two tag sets, so a body that returns its input is refused with "a type the signature leaves open is ...". Write `['t own in  't move out]`.
  1. readme.md, type system section: one sentence and the example beside "A signature is a promise for every type it allows".

- [ ] Library and checker edge cases the silent-failure audit rated low.
  - json.slap `_type-name` default "" would print "expected int, got " for a new tag: die instead.
  - xml.slap `_xe-render-attrs` and json `je-obj` read caller-built pairs with `nth must`/`get must`: an attribute of 3 items renders 2 with no error. Take attrs as records `{'name 'value}`.
  - todo.slap rewrites only text/done and "items", dropping other fields and keys: keep the decoded JSON and change only those fields.
  - kv-server: a recv error reads as EOF; send errors vanish; SAVE always says "snapshot not writable"; a torn last snapshot line loads as a short value. Report each with its reason; refuse a line without TAB and newline.
  - feed.slap cuts text by bytes, which can split a UTF-8 sequence: cut at a character boundary.
  - JSON refuses nesting at 257 and XML accepts 257: one limit, one comparison, in both (breaker round 15).
  - kv-server: a client that connects and sends nothing blocks every other client (breaker round 15). A per-connection read timeout, or say in the readme that it serves one client at a time.
  - `chunks` with size 0 dies with the generic must text at `<prelude>:38`: a message naming chunks, the size it got, and the caller's line needs a way for prelude code to fail with its own text.
  - Checker: `ty_lend` pushes two fresh values after an underflow whatever the body leaves; the "more than 4096 case forms" limit names no fix; the program-end leftover check waits until other errors are fixed.

- [ ] Checker messages and docs say what the code does (code review).
  1. Mismatch messages read backwards where the actual type is passed first: `[1] 'xs let 'xs "a" nth` says "int is not a list", `5 (drop) lend` says "'a box is not int", `5 0 {'a ()} case` says "tagged .. is not int", `{'a 1} len {'a 1} cat` says "{| ..a} is not int". Call `ty_unify(expected, actual)` at the case scrutinee, lend's box, nth and cat sites, as `ty_apply` does; pin one message each in errors.slap.
  2. A `lent` or `copy` slot accepts a box or socket type: `(free) [int box lent in] effect` passes. `ty_mark_copy` dies with an annotation error on K_BOX and K_SOCK.
  3. A word that runs `each`/`fold` over a dict input needs a signature, and without one the error is "int dict is not a list" with no fix: name the fix in the message and in readme's dicts section.
  4. `halt` is refused in every declared word and in each/fold bodies (their stacks are a caller's): say so in the readme, and name the fix in the message.
  5. readme's `{...}` rule and an expect.slap comment say an unpaired literal is a tuple; `{'a 1 'b}` is now an error. State the rule as the checker applies it.
  6. Stale: the comment above `binding_release` names the old checker's box bindings; claude.md's Frames paragraph gives "about a tenth of the run time", a number that goes stale.
  7. errors.slap: the case at 1182 now fails on `at must`, not its record-literal rule (drop the must); 1238, 1242 and 1246 repeat the "already defined" block at 430-460 (merge).
  8. Breaker round 15 notes: refusals caused by `halt` never mention halt; "one path leaves N more values ... a branch, clause, loop pass or recursive call" appears where there is no branch; tag-set variables print as an unnamed `tagged ..`, so "declares X, but its body is X" can show two equal types (name them like row variables); "program too long" prints the whole 160 KB source line; `{'a int | 'r | 's}` silently drops 'r (refuse a second `|`).

- [ ] A long-running server that makes closures per request holds steady memory.
  1. Frames are reference-counted (2026-09-28), but a cycle is never freed: a closure made by a nested body and stored in an outer frame's binding keeps that frame, which keeps the closure's frame. So does a value a primitive drops without `deep_free_values` (the leaks below).
  2. `dict "k" 1 insert box free` leaks the dict: `prim_free` does not deep-free the contents.
  3. A dict inside a list or record leaks when `len`, `eq`, `get`, `take-n`, `drop-n`, `index-of` or `at` discard the rest of the value: they drop slots without `deep_free_values`. Free what each primitive discards; `leaks --atExit` on breaker round 11's cases (scratchpad break11/) shows each one.

- [ ] You run a pico8 cart headless for N frames and its screen matches a reference render.
  Tradeoffs, decided up front: pico8 only; tic80 reuses the interpreter later. The screen has 4 greys, so 16 colours rank-map to 4 like uxn.slap. No audio: sfx/music are no-ops. Speed is ~190k uxn instructions/s, so carts run far below 30 fps; correctness first. Lua tables cannot be slap values (a lookup of a bound dict copies it, and so does dup), so all Lua state lives in one heap threaded on the stack.
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
