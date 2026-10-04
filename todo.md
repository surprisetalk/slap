- [ ] The checker refuses a `case` clause that can never run.
  Accepted today: `1 'a tag 'x let x {'a (x {'a (print) 'b (print)} case)} case`, where the inner 'b clause is dead because the outer case closed x's set to {a}; a clause for a tag that a signature's closed set lacks; and a `'_` after a clause for every tag of a closed set.
  Decided: check closed sets only. A set still open at the case takes the clause tags as its own, so a clause there names a tag the case accepts, as a word's input does. Finding dead clauses on an open set needs the set's final tags, which are known only when the word generalizes: that is the deferral the case entry removed.
  1. errors.slap first: the program above is refused, and the message names 'b and says the value is never tagged 'b. A closed set with a clause for every tag plus `'_` is refused, and the message says '_ never runs.
  2. ty_case: when the value's set is closed (`closed` is set), give that message for each clause tag the set lacks. With `'_` on a closed set, walk the set; when every tag has a clause, refuse the `'_`.
  3. Rewrite any example or library site the check refuses: delete the dead clause.

- [ ] You read a checker message that names the real cause, in the right direction, with one name per variable (code review; silent-failure audit; breaker rounds 14 and 15).
  Decided: keep `box (body) lend` and its sealed stack. `( 'g let 5 box (g apply) lend swap free ) 'w let (1 plus) w` stays refused: lend's output count is unknown until a caller passes g. mutate and each accept such a body only because TYPES fixes their output count.
  1. errors.slap first: pin one message per step below, and check each fails against the old binary.
  2. Mismatches read backwards where the actual type is passed first: `[1] 'xs let 'xs "a" nth` says "int is not a list", `5 (drop) lend` says "'a box is not int", `{'a 1} len {'a 1} cat` says "{| ..a} is not int". Call `ty_unify(expected, actual)` at nth, lend's box and cat, as `ty_apply` and `ty_case` do.
  3. `'apply' takes ( ..a -> ..b ) / but the stack has ... / <why>`: the "takes" line shows a fresh copy of the word's type, so the why line uses other names. In ty_apply, print the instance and the stack top before `ty_unify`, with `ty_print_count` reset once. It is the hottest path: print only when a cheap pre-check fails, or keep a copy of the instance and print it after. ty_define and the case messages already do this.
  4. Inside a `[...]` literal, `1 [drop]` says "'drop' takes 'a copyable / but the stack has nothing": say that a literal's code starts from an empty stack.
  5. ty_lend: when `out` ends in a stack variable that is not `below`, say the body's output count is unknown at the lend and point to mutate or a body written in place. Otherwise keep "may not take values below". After an underflow, push only the box's place, not two fresh values, so later errors stay real.
  6. A `lent` or `copy` slot accepts a box or socket: `(free) [int box lent in] effect` passes. `ty_mark_copy` dies with an annotation error on K_BOX and K_SOCK.
  7. "one path leaves N more values ... a branch, clause, loop pass or recursive call" appears where there is no branch. Tag-set variables print as an unnamed `tagged ..`, so "declares X, but its body is X" can show two equal types: name them like row variables. "program too long" prints the whole 160 KB source line. `{'a int | 'r | 's}` silently drops 'r: refuse a second `|`.
  8. `'x [strng lent in] effect` reports "at line 1" in the text, with the caret at column 1. Pass the annotation's token to the error, as `ty_err` does for words; drop the "at line %d" text. errors.slap: EXPECT-COL on an unknown type word.
  9. readme: a signature passes a value through unchanged only when both slots name one variable. `[tagged own in  tagged move out]` is two tag sets, so a body that returns its input is refused with "a type the signature leaves open is ...". Write `['t own in  't move out]`: one sentence and the example beside "A signature is a promise for every type it allows".
  10. Stale: the comment above `binding_release` names the old checker's box bindings; claude.md's Frames paragraph gives "about a tenth of the run time". errors.slap: merge "a program redefines ok", "a program redefines no" and "a word redefined after a word that calls it" into the "already defined" block.

- [ ] The JSON and XML libraries refuse what they cannot read or write, and say where (silent-failure audit; breaker round 15).
  Accepted today: leading zeros (`"0123"` gives 123), duplicate object keys (first wins), duplicate attributes (first wins), an `encoding="ISO-8859-1"` declaration (its bytes read as UTF-8), and a float past the range (`"2e308"` gives `inf ok`). `"0.3"` does not equal `0.3`: the fraction is built as digit x 0.1^n. JSON refuses nesting at 257 and XML accepts 257. rss-to-xml writes RSS for any 'kind but "atom" ("Atom" too). `_xe-render-attrs` and json `je-obj` read caller-built pairs with `nth must`/`get must`: an attribute of 3 items renders 2 with no error. `jd-one-of`/`xd-one-of` say only "oneOf: no decoders matched". Parse errors ("json: expected , or ] in an array", "json: trailing input", "xml: expected <", ...) give neither position nor byte: on a 600 KB feed there is no other way to find the fault.
  1. One failing test per case in json.slap/xml.slap/rss.slap; one pinned message each for the parse errors.
  2. json: refuse a leading 0 before a digit, a repeated key in one object, and a non-finite result; build the fraction as an integer mantissa and scale it once.
  3. xml: refuse a repeated attribute name and any declared encoding but UTF-8 or US-ASCII. One nesting limit, one comparison, in both libraries.
  4. rss-to-xml accepts "rss" or "atom" only. `_xe-render-attrs` and `je-obj` take records `{'name 'value}`.
  5. Parse errors give the byte offset and the byte found. `jd-one-of`/`xd-one-of` list each alternative's error: "oneOf: [0] .a: expected int, got string; [1] ...".

- [ ] You run a pico8 cart headless for N frames and its screen matches a reference render.
  Decided up front: pico8 first; tic80 reuses the interpreter later. The screen has 4 greys, so 16 colours rank-map to 4 like uxn.slap. No audio: sfx/music are no-ops. Speed is ~190k uxn instructions/s, so carts run far below 30 fps; correctness first. Lua tables cannot be slap values (a lookup of a bound dict copies it, and so does dup), so all Lua state lives in one heap threaded on the stack.
  1. examples/lua.slap lexer: read the source by index with `nth`, as json.slap does. No per-character recursion.
  2. The parser emits a flat int bytecode list. A `while` + step loop runs it, like uxn.slap's run-more; the Lua call stack is data, not slap recursion.
  3. Heap: tables are int ids; entries live in one dict keyed by encoded `id:key` bytes, plus a key list per table for `pairs`. Closures are (proto id, upvalue ids).
  4. Numbers are pico8 16.16 fixed point in ints.
  5. tests/run_lua.py diffs `print` output against system `lua` on tests/lua/*.lua scripts that stay in integers.
  6. Cart loader for the .p8 text format: __lua__, __gfx__, __map__ sections.
  7. API subset: cls pset pget rectfill circfill line spr map btn print rnd flr sin cos; _init/_update/_draw.
  8. tests/run_pico8.py runs a small cart headless and diffs a palette-index pixel dump against a committed reference made with pico8's export (or zepto8).
  9. Then tic80 on the same lua.slap; duskos and decker after.

## Blocked on you

- [ ] `chunks` with size 0 dies with a message that names chunks, the size it got and the caller's line (code review).
  Today it dies with the generic must text at chunks' line in the prelude.
  Decide: how prelude code fails with its own text (a `fail ( str -> )` primitive, or a must variant that takes a message).
  1. errors.slap first: `[1 2] 0 chunks` names chunks and 0.

- [ ] A closure stored in an outer frame's binding is freed when nothing reaches it (code review).
  A cycle is never freed: a closure made by a nested body and stored in an outer frame's binding keeps that frame, which keeps the closure's frame. `( 'k let ( k apply 1 plus) ) 'wr let 0 100000 (drop (0 plus) 3 (wr) repeat 'c let 0) repeat drop` reaches 201 MB: `(0 plus)` closes over the outer body's frame, and the chain ends in that frame's binding `c`. With `(0 plus)` bound at the top level it stays at 4.5 MB.
  Decide: weak parent links, or a collector.
  1. Tests first: a loop that makes such a cycle each pass holds steady memory.

- [ ] You apply a body while a copy of it is on the stack, and `cat` bodies in a word (breaker round 15 b1, b2).
  `(5) dup apply print drop` is refused: `dup` gives both copies one type, and applying one changes the depth below the other. This caused nearly all of the 145 refusals among 66,000 generated well-typed programs. `( cat ) 'c let (1 plus) (2 mul) c` is refused, though the readme lists body as a Semigroup; making cat generic over bodies as it stands would be unsound.
  Decide: let `dup` (and every copy of a body value) give the copy its own stack rest when nothing else holds it, as `let` does now; and type `cat` on bodies as composition in the word's own type, or drop body from the readme's Semigroup row.
  1. Tests first for the choice: both programs above.

- [ ] You put a symbol that `nth` used into a list with other symbols (breaker rounds 14 and 15).
  `[1 2] 'xs let 'xs 'k let k 0 nth must drop [k 'ys] drop` is refused: nth fixes k's type to exactly 'xs, and a list of symbols widens its element type to sym. `k 'ys member` is refused the same way. eq and neq compare without widening, so they pass.
  Decide: keep the refusal (nth by a symbol read from a list is rare), or let a symbol type carry both its exact label for nth and a widened copy for containers.
  1. If widened: expect.slap first, both programs pass.

- [ ] The example apps report every I/O failure and keep the data they do not change (silent-failure audit, critical; breaker round 15).
  `read` gives `path no` for every failure, so a directory, a mode-000 file or an I/O error reads as "no file": todo.slap lists nothing and exits 0, and with a write-only file `add` replaces the items. kv-server's boot `save-snapshot` then writes the empty store back. todo.slap rewrites only text/done and "items", dropping other fields and keys. kv-server: a recv error reads as EOF; send errors vanish; SAVE always says "snapshot not writable"; a torn last snapshot line loads as a short value; a client that connects and sends nothing blocks every other client. feed.slap cuts text by bytes, which can split a UTF-8 sequence.
  Decide: `read` reports why (errno text in the 'no payload, e.g. "path: not found"), or programs check existence with `ls` of the parent. Recommend the errno text: one change in `prim_read`, and every caller can tell. And for kv-server: a per-connection read timeout, or the readme says it serves one client at a time.
  1. tests/run_todo.py and run_kv first: a directory, a mode-000 file and a write-only file each exit nonzero naming the path; the file keeps its bytes.
  2. todo.slap:47 and kv-server.slap:53: start empty only on "not found"; anything else dies with the path and the reason.
  3. todo.slap keeps the decoded JSON and changes only those fields. kv-server reports each error with its reason and refuses a line without TAB and newline. feed.slap cuts at a character boundary.

- [ ] A decoder tells a missing optional field from a present but malformed one (silent-failure audit, critical).
  `jd-maybe`/`xd-maybe` turn every failure into `none ok`: `{"a":"x"}` with `"a" jd-int jd-field jd-maybe` gives none. rss's `_rss-opt-text` (`xd-child xd-maybe ("" default)`) makes `<title>A <b>bold</b> post</title>` and Atom `<content type="xhtml">` give "", and lets an RSS channel without its required title, link or description parse; rss-to-xml then writes `<pubDate></pubDate>` and `<link href=""/>`.
  Decide: jd-maybe/xd-maybe give `none` only for "missing field"/"no such child" and pass every other 'no on (Elm's `maybe` swallows all errors; `optionalField` does not); rss keeps absent optional fields as `none`, not "". Text with element children: take the text of xhtml content, or refuse it.
  1. Tests first: the three inputs above, and a channel without a title.
  2. jd-field/xd-child failures carry a distinct 'missing payload shape (or message prefix) that the maybe decoders test.
  3. rss.slap: required fields use xd-child without maybe; rss-to-xml skips absent fields.
  4. `_rss-link` and `_atom-link-href` use xd-maybe too: a `<link>` with element children, or a malformed href, reads as no link.
  5. Links are text, never checked as URLs: three breaker rounds each found one (an empty href, a space-only href, then `href="&#160;"`, which gives `[194 160] ok` since XML whitespace is ASCII only). Decide: rss-parse refuses a link that is not an absolute http(s) URL, or keeps the text as written.

- [ ] You type slap at a prompt and see the stack after each line ("a nice slap shell").
  Decide: a terminal REPL, or the sauce launcher below.
  1. If a REPL: `./slap` with a TTY on stdin reads lines; the checker's stack type and the global frame persist across lines; an error discards only its line.

- [ ] You run sauce: fullscreen slap apps in a row you slide through, the launcher leftmost.
  Decided: fullscreen apps only, starting from an app launcher like iOS. Slide apps left and right; the launcher is always leftmost. Later, apps take partial width (full height) and slide around; this works on mobile and desktop. No vigil: nothing kept in intermediate state outside physical notes and a single working copy; publish sequels, not incremental improvements. Write the apps in slap, then an interpreter in Swift (slap.swift) that loads the ROMs. The notes name the first release "slap 0".
  Apps: launch, write, code, surf, watch, query, chat, talk, find, debug; later filer, feeder, mailer, player, claude, hypercard, via a charmbracelet-like UI.
  Decide: the host for the first app (slap-sdl, wasm, or slap.swift) and the first app.
  1. Move the blog and all projects into sauce as slaps/scraps. taylor.town serves its pages (indexed) and assets (not indexed) from sauce.
  2. launch.slap lists the apps and runs one.
  3. write.slap: a markdown editor with vim/leap movement, a minimap, image preview, linters (like hemingway) and AI editing.
  4. find.slap: a search-based file browser. SQL or FQL finds files instead of navigating directories.

- [ ] The language has the batteries the apps need.
  The notes sketched these types: `i8, i16, i32, u8, u16, u32, f16, f32`; `int, float, str`; `'x box, 'x list, 'x slice, 'v 'k dict, 'v 'k dice, ['b 'a], [.. 'b 'a], {'k 'v}, {.. 'k 'v}`. Other candidates: sets; concurrent go-func-esque threads, each with its own input queue and state; a charm-like UI framework with a WYSIWYG editor that builds templates and components visually; a graphics stack language (sneeze? splat? spill?); a query language.
  Decide: which ones the first app needs.
  1. Each chosen one: expect.slap first, then TYPES, then a readme section.

- [ ] You read a post series on slap.
  - 001 why I built it
  - 002 better API, patterns and idioms: write less code and build more DSLs (e.g. Elm encoders/decoders)
  - 003 open tag constructors? 'ok/'no and no panic? rethink APIs? set? dict? threads? what other batteries to include?
  - 004 a UI framework like charm, and a WYSIWYG UI editor that builds templates and components visually
  - 005 a graphics stack language (sneeze? splat? spill?)
  - 006 editor, surfer, filer, feeder, mailer, player, claude, hypercard via a charmbracelet-like UI
  - 007 a query language
  - 008 running taylor.town from sauce OS
  - 009 off to scrapscript
  1. You write them. Claude drafts an outline from readme.md and git history on request.
