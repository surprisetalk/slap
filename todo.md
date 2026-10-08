- [ ] serve.slap and wiki.slap answer 500 with the reason for a file they
      cannot read, and 404 only for a missing one. Today both treat every
      `read` or `ls` 'no as missing: a mode-000 page answers 404, and wiki
      offers to create a write-only page, whose save replaces its bytes.
  1. tests/run_serve.py and run_wiki.py first: a mode-000 file answers 500
     naming "Permission denied"; a missing one stays 404; a write-only wiki
     page keeps its bytes.
  2. serve.slap:175 falls through to the listing only on "Is a directory";
     serve.slap:95 and wiki.slap:100 answer 404 only on "No such file or
     directory", as todo.slap and kv-server.slap do. That is the third caller
     of the test, so it moves into strings.slap as one word.

- [ ] A crash, a full disk or a file size limit during `write` leaves the old
      file whole. Today `write` opens with "wb", which empties the file before
      it writes: `(ulimit -f 2; todo add zzz)` cut a 1862-byte todo file to
      1024 bytes. Decided: `write` replaces a regular file atomically. It
      writes a temp file made with mkstemp beside the target, gives it the
      target's mode, fsyncs it, and renames it over the target; on any failure
      it unlinks the temp and gives `"path: reason" no`. A symlink resolves to
      its target (realpath), so the link stays. A path that exists and is not a
      regular file (/dev/null, a FIFO, a directory) is written in place as
      today; /dev/stdout and /dev/stderr keep their streams. slap ignores
      SIGXFSZ, so a size limit fails the write with EFBIG instead of killing
      the process and leaving the temp behind. Tradeoffs: the target's
      directory must be writable; the owner is not kept; each write costs an
      fsync.
  1. Tests first, each checked against the old binary: run_todo.py and
     run_kv.py write under RLIMIT_FSIZE (preexec_fn) and check that the old
     bytes survive, the exit is nonzero naming "File too large", and no temp
     file remains. expect.slap: a write through a symlink keeps the link and
     changes the target; a mode-0600 file keeps its mode; "/dev/null" still
     takes a write.
  2. prim_write in slap.c; `signal(SIGXFSZ, SIG_IGN)` in main.
  3. readme and claude.md's fallible table say write replaces a file
     atomically.

- [ ] kv-server refuses a SET that would take its store past the size SAVE
      can write, and keeps serving. Today SAVE builds the snapshot with
      dict-entries and a fold beside it, and the fold body's `dup` and `fv`
      copy each value again: past about 1 MB the stack overflows, the server
      dies with every change since the last save, and a 1-2 MB snapshot loads
      but dies at the boot save. Decided: a byte count rides on the stack
      beside the store (snapshot bytes: each key and value plus TAB and LF);
      SET, and DEL of a key, update it; a SET past the bound answers `ERR
      store full: N of B bytes` and changes nothing; load-snapshot refuses a
      file past the bound with its size.
  1. Tests first: run_kv.py fills the store to just under the bound with raw
     sockets, checks SAVE, SHUTDOWN and reboot keep every key, then a SET past
     the bound answers ERR store full and PING still answers; a snapshot file
     past the bound is refused, untouched.
  2. save-snapshot binds each entry once and reads key and value from the
     binding, so a value is never on the stack twice; then measure the
     largest store SAVE writes and set the bound below it, with a comment
     naming the measurement's cause (STACK_MAX, two copies of the text).
  3. handle-cmd and the accept loop thread `store bytes`; the self-tests
     follow.

- [ ] A closure stored in an outer frame's binding cannot form a cycle: the
      checker refuses the binding. Today
      `( 'k let ( k apply 1 plus) ) 'wr let 0 100000 (drop (0 plus) 3 (wr) repeat 'c let 0) repeat drop`
      reaches 201 MB: F0's `c` holds C3, whose frame F3 holds C2, ..., F1
      holds `(0 plus)`, whose frame is F0, so no refcount reaches zero. A
      cycle can also run through a parent link: a closure made by a body that
      runs in a child of F, bound in F. Decided: reject in the checker; no
      collector. Rule: in a body that makes a frame (not the top level, whose
      frame lives forever), a `let` after the first body literal that the
      body pushes as a value (not an in-place if, while, dip, case or pthen
      body; in-place bodies count for the literals inside them) may not bind
      a value that holds a body. A body written right before `'name let` is a
      word over this frame, which its own binding holds weakly, so it is not a
      value here. "Holds a body" is a new bit on type variables, as P_COPY is:
      the let marks the bound type's variables, the bit spreads into lists,
      records, dicts, tag payloads and results, and a variable with the bit
      that meets K_FN is refused at that point, generic instances included.
      Lets before the first such literal stay free, since nothing made in
      this frame exists yet: `'d let` of a decoder at the start of a word is
      fine. The message names the binding and the literal's line and says to
      bind the value before the body is made, or in a word of its own.
      Tradeoff: some acyclic programs are refused.
  1. Measure first: run the rule as a warning over the corpus (examples,
     libs, expect.slap, scale.slap) and count the sites it refuses. If a
     library idiom such as json.slap's decoders is refused, stop and bring
     the count back here before going on.
  2. Tests first, each checked against the old binary: errors.slap gets the
     program above and a parent-link cycle (a nested binding body returns a
     closure that the outer body binds); expect.slap gets `'d let` before a
     literal, and a word bound after a literal.
  3. Checker: the taint point per frame-making body in ty_range, the bit in
     ty_bind/ty_need beside P_COPY, the message.
  4. Breaker and fuzz rounds on the ASan build, with the memory watchdog.
     readme's closures section and claude.md's Frames paragraph say a cycle
     cannot form.

- [ ] You type slap at a prompt in the terminal and see the stack after each
      line ("a nice slap shell"). Decided: there are two shells, one in the
      terminal and one in sauce (below). Both run one loop in slap.c, so the
      slap-sdl and wasm builds of sauce get it free; slap.swift needs its own
      copy. The terminal shell comes first. An error discards its line: the
      stack and the global bindings return to their state before the line;
      output the line already wrote stays.
  1. `./slap` with a TTY on stdin reads lines. The checker's stack type and
     the global frame persist across lines. An error discards only its line.

- [ ] You build a literal from names bound at runtime: `{'x x 'y y}`,
      `[i i] insert` and `[ 1 mk ]` work. Today a `[...]` or `{...}` literal
      is built once, when the program is read. Decided: a literal is built
      when it is reached, in the running frame; `{...} case` clauses stay
      built once, or `case` loses its in-place path. Measured in a scratch
      build: feed, sort.slap, the slow Euler problems and bench-uxn moved
      under 1.5%; in hot loops only `[]` is rebuilt.
  1. Tests first, each checked against the old binary: expect.slap gets the
     three literals above; errors.slap cases that pin "built when it is read"
     or "starts from an empty stack" become passes or go.
  2. build_tuple: a literal becomes a body plus a marker that counts its
     results into a list or record header; eval_run fuses the pair. Its loc
     is the literal's open bracket, not the marker. If `{...} case` clauses
     cannot stay built once, stop and time make status's feed with clauses
     built on every `case` before going on.
  3. Checker: a literal's code is ordinary code on an empty stack. Delete
     ty_literal, ty_lit_depth, tyb_visible, the VF_DICT copy and their
     messages.
  4. into becomes replace-only; delete K_PRE and K_ABS. Then the tag-payload
     task below has one row shape.
  5. Breaker and fuzz rounds on the ASan build. readme and claude.md (Records,
     Evaluator, Invariants on VF_DICT) follow.

- [ ] You tag a value with any payload, and `'ok`/`'no` are ordinary tags: a
      tagged type lists each tag with its payload, as a record type lists each
      key with its value. Today every tag but 'ok/'no has one payload type in
      the whole program (ty_tag_payload), and results have their own kind,
      K_RES, so case refuses a result clause beside another tag. Decided: the
      payload lives in the row, as in OCaml's polymorphic variants. A type may
      be cyclic only through a tag payload, as OCaml allows.
      `{'ok int 'no str} either` is a closed tag set. Gains:
      `{'ok (…) 'no (…) 'retry (…)} case` works, and a program's 'int no
      longer collides with json.slap's. Tradeoff: the checker gains cyclic
      types, and the runtime trusts it. Do this after the runtime-literals
      task; the signature task follows this one.
  1. Tests first; check each fails against the old binary. expect.slap: the
     three-clause case above; `5 'n tag` and `"x" 'n tag` in two words that
     never meet. errors.slap: a mismatch on a recursive type prints it with
     `as`. errors.slap cases that pin a payload conflict ("conflicts with its
     payload elsewhere", "unlike its payload elsewhere") and "a result is
     tagged only 'ok or 'no" become expect.slap passes.
  2. Tag sets reuse K_REXT, K_RNIL and K_RVAR, with the payload as the field.
     Delete K_TEXT, K_TNIL, K_TVAR, K_RES, ty_tagpay, ty_tag_payload and
     ty_tag_take. `'t tag` makes `{'t p | 'r}`. In ty_case, a clause's
     payload is its tag's field in the value's row, and '_ gets the value
     with its whole row. TYPES' results (`must`, `pthen`, `get`, the I/O
     words) and nth's output become closed `{'ok 'a 'no 'b} either`.
     ty_copy_parts: a tag set is copyable when each payload is.
  3. Cycles: the occurs check reports an occurrence only on a path that
     crosses no tag payload, and still lowers levels everywhere.
     ty_unify_chain remembers each K_TAG pair it is inside and treats a pair
     met again as unified. ty_copy and ty_subst_at record a term's copy in
     ty_to before they recurse into its parts. ty_show prints a term met again
     on one path as `'j`, and its first visit as `(… 'j as)`.
  4. json.slap's and xml.slap's self-tests and tests/scale.slap cover
     recursive data. Then run breaker and fuzz rounds on the ASan build: a
     checker hole shows up as memory damage, not as a message.
  5. readme: tagged unions and the type table lose "one payload type in the
     whole program" and the result row; recursive data gets one example.
     claude.md's Checker paragraph (kinds, ty_tag_payload, K_RES) follows.

- [ ] You write a signature as ordinary data: two lists of type values, ins
      then outs, as in `(2 mul) [int] [int] effect 'double let` and `'triple
      [int] [int] effect`. Prelude words build types as tagged values, so
      `[int str] 'sig let sig len print` prints 2. Do this after the
      tag-payload task (under one payload per tag, json's 'int holds an int
      and the type 'int holds `()`). Decided:
      - Base types are words: int, float, sym, str and socket.
      - Constructors are their tags: `int 'list tag`, `'a var 'dict tag`,
        `'a var 'box tag`. dict and box already name value words.
      - A list holds one type, so a type variable is `'a var`, or `'a copy`
        when the body may copy or drop it.
      - A body type is `[ins] [outs] fn`; `[] [] fn` is today's `()`. A named
        stack rest is `'s rest` at the head of a list, and a sealed one
        `'s sealed`. A body type with no rest runs on the word's rest, as a
        body type in a slot does today.
      - A record type and a tag set are dicts of types, keyed by name:
        `dict "x" int insert rec` and `dict "ok" int insert "no" str insert
        either`. Key "_" holds an open rest: `dict "x" int insert "_" 'r var
        insert rec`. rec is `('rec tag)` and either `('either tag)`, both
        prelude words: a dict holds one type, so no new primitive is needed.
        The parser refuses a key given twice, where insert would replace it.
      - A recursive type is `T 'l as`.
      - TYPES uses the same notation, as `'name [ins] [outs]`, and the same
        parser. Nothing evaluates TYPES.
      The checker reads the two literals before effect as types, as at reads
      its key, and checks them as ordinary literals too; the runtime skips
      them there, as it skips `[...] effect` today. Every type variable has a
      name by construction, so today's `[tagged own in  tagged move out]`,
      which names two tag sets and says "'h' declares ( ..a tagged .. -> ..a
      tagged .. ), but its body is ( ..a tagged .. -> ..a tagged .. )",
      cannot be written. Tradeoffs: the slot modes go; signatures and forward
      declarations stay. Signatures grow: `'each ['s rest 'a var 'list tag
      ['r sealed 'a var] ['r sealed 'b var] fn] ['s rest 'b var 'list tag]`,
      and each result in TYPES is `dict "ok" 'a var insert "no" 'b var insert
      either`. Programs bind str 3 times and rest once as local names; those
      sites get new names.
  1. Tests first; check each fails against the old binary. expect.slap:
     `(2 mul) [int] [int] effect 'double let 5 double 10 eq assert`;
     `'triple [int] [int] effect (3 mul) 'triple let`; `(dup) ['a copy]
     ['a var 'a var] effect`; `[int str] len 2 eq assert`; a list-length
     word declared `[dict "cons" dict "hd" int insert "tl" 'l var insert rec
     insert "nil" [] [] fn insert either 'l as] [int] effect`.
     errors.slap: `(dup) ['a var] ['a var 'a var] effect` says the body
     needs a copyable value; `[int own in] effect` says a signature is two
     lists of types; `['a] [] effect` says a type variable is `'a var`;
     `[dict "x" int insert "x" str insert rec] [] effect` says "x" is given
     twice.
  2. Prelude: the type words, each a `tag` over its payload.
  3. Checker: one parser for type literals replaces ty_parse, ty_parse_fn and
     ty_parse_slots. It accepts only the forms above and names the form it
     expected. Rewrite TYPES in the new notation. Delete own, lent, move,
     auto, in, out, `->`, `|`, the type words seq, tuple and tagged, and the
     `{...}` record and either forms; rec and either are now prelude words.
  4. Runtime: skip `[...] [...] effect` where it skips `[...] effect` today.
  5. Migrate every test signature with a one-off script in the scratchpad,
     and rename the str and rest locals.
  6. readme: a "types are values" section replaces effect annotations and
     protocol constraints; the "tuples" section becomes "bodies", the word
     claude.md uses. claude.md's Checker paragraph (TYPES notation) follows.

- [ ] A tight stack loop runs at the cost of its cheapest primitives: `if`,
      `let` and `dip` each cost at most twice `swap` (zoom.slap
      microbenchmarks). Where the time goes: run callgrind on zoom's tick on
      pc (`nix-shell -p valgrind --run "valgrind --tool=callgrind ./slap <
      prog"`, then `callgrind_annotate --auto=yes`). The big remaining costs:
      `elem_starts` and the element offsets it feeds, since every run of a
      body that holds a nested body walks its elements twice; the
      per-primitive aux and staged bookkeeping in eval_run; frame scans for
      lookups, and `nth`'s scan for its list. clang lays out eval_run's hot
      loop differently when it grows, so a fast path inlined there can make
      zoom slower: keep new paths in noinline helpers. Tradeoff: each fast
      path is more C to keep correct.
  1. Benchmark first, interleaved, minimum of 3 runs: the microbenchmark
     `2000000 'N let ('label let 'body let millis 't0 let 0 (dup N lt) (body apply 1 plus) while drop millis t0 sub 'dt let label print dt print) 'bench let`
     with `() "empty" bench`, `(1 (2) (3) if drop) "if" bench`,
     `(5 'v let) "let" bench`, `(1 2 (drop) dip drop) "dip" bench`; zoom's
     tick without SDL (zoom.slap, `start`, and zoom-sdl's tick body with
     `fill-rect` as five `drop`s, run 4 times); the 600 KB feed; and
     uxn.slap on `tests/.uxn-refs/drool.rom` for 60 frames.
  2. Element offsets once per body, not per run, without making a Value
     larger: every compound copy moves Values.
  3. Keep a change only if every benchmark in step 1 gets no slower.

- [ ] You run a pico8 cart headless for N frames and its screen matches a
      reference render. Decided up front: pico8 first; tic80 reuses the
      interpreter later. The screen has 4 greys, so 16 colours rank-map to 4
      like uxn.slap. No audio: sfx/music are no-ops. Speed is ~190k uxn
      instructions/s, so carts run far below 30 fps; correctness first. Lua
      tables cannot be slap values (a lookup of a bound dict copies it, and so
      does dup), so all Lua state lives in one heap threaded on the stack.
  1. examples/lua.slap lexer: read the source by index with `nth`, as json.slap
     does. No per-character recursion.
  2. The parser emits a flat int bytecode list. A `while` + step loop runs it,
     like uxn.slap's run-more; the Lua call stack is data, not slap recursion.
  3. Heap: tables are int ids; entries live in one dict keyed by encoded
     `id:key` bytes, plus a key list per table for `pairs`. Closures are (proto
     id, upvalue ids).
  4. Numbers are pico8 16.16 fixed point in ints.
  5. tests/run_lua.py diffs `print` output against system `lua` on
     tests/lua/*.lua scripts that stay in integers.
  6. Cart loader for the .p8 text format: **lua**, **gfx**, **map** sections.
  7. API subset: cls pset pget rectfill circfill line spr map btn print rnd flr
     sin cos; _init/_update/_draw.
  8. tests/run_pico8.py runs a small cart headless and diffs a palette-index
     pixel dump against a committed reference made with pico8's export (or
     zepto8).
  9. Then tic80 on the same lua.slap; duskos and decker after.

## Blocked on you

- [ ] A decoder tells a missing optional field from a present but malformed
      one. `jd-maybe`/`xd-maybe` turn every failure into `none ok`:
      `{"a":"x"}` with `"a" jd-int jd-field jd-maybe` gives none. rss's
      `_rss-opt-text` (`xd-child xd-maybe ("" default)`) makes `<title>A
      <b>bold</b> post</title>` and Atom `<content type="xhtml">` give "", and
      lets an RSS channel without its required title, link or description
      parse; rss-to-xml then writes `<pubDate></pubDate>` and `<link
      href=""/>`. Proposed: jd-maybe/xd-maybe give `none` only for "missing
      field"/"no such child" and pass every other 'no on (Elm's `maybe`
      swallows all errors; `optionalField` does not); rss keeps absent
      optional fields as `none`, not "". Decide: text with element children
      (take the text of xhtml content, or refuse it), and links (rss-parse
      refuses a link that is not an absolute http(s) URL, or keeps the text as
      written; three breaker rounds each found a bad link that passed).
  1. Tests first: the three inputs above, and a channel without a title.
  2. jd-field/xd-child failures carry a distinct 'missing payload shape (or
     message prefix) that the maybe decoders test.
  3. rss.slap: required fields use xd-child without maybe; rss-to-xml skips
     absent fields.
  4. `_rss-link` and `_atom-link-href` use xd-maybe too: a `<link>` with
     element children, or a malformed href, reads as no link.

- [ ] You run sauce: your own collection of lofi slap apps, fullscreen in a
      row you slide through, with home leftmost. Decided: fullscreen apps
      only, starting from home, a launcher like iOS's. Slide apps left and
      right; home is always leftmost. Later, apps take partial width (full
      height) and slide around; this works on mobile and desktop. No vigil:
      nothing kept in intermediate state outside physical notes and a single
      working copy; publish sequels, not incremental improvements. Write the
      apps in slap. slap-sdl hosts them first; wasm and an interpreter in
      Swift (slap.swift) that loads the ROMs come later. The first release is
      "slap 0". The apps store everything in CBOR (examples/lib/cbor.slap) and
      share a charmbracelet-like UI. A dir app opens a general thing. A file
      app opens one specific thing, and it is always a viewer and an editor.
      Decide: the first app; what a file app opens with no subject (the last
      file, or an empty one); whether a file app saves by tags only, with no
      file name; and whether you publish your own ware registry.
      - Dir apps:
        - home: the launcher. A grid of app icons (or text) and widgets, and
          search.
        - stuff: a table of files with an action column (e.g. edit image). No
          file names: tags and thumbnails. It holds libraries too (books,
          albums).
        - shell: the slap REPL in sauce. It runs the terminal shell's loop.
      - File apps:
        - ware: package registries. You add recommended registries or custom
          ones. Each registry holds apps. Each app has an author, a changelog
          and updates, a description, screenshots, etc.
        - config: tk.
        - web: tk. An html/css browser.
        - book, feed, code, slides, prose, sheet, email, cal, phone, chat,
          video, music, photo, camera, print, clock, map, steno, cast.
      - Unplaced: query, debug, talk, claude, hypercard.
  1. Move the blog and all projects into sauce as slaps/scraps. taylor.town
     serves its pages (indexed) and assets (not indexed) from sauce.
  2. home.slap lists the apps and runs one.
  3. prose.slap: a markdown editor with vim/leap movement, a minimap, image
     preview, linters (like hemingway) and AI editing.
  4. stuff.slap: SQL or FQL finds files by tag, instead of navigating
     directories.

- [ ] The language has the batteries the apps need. Sketched types: `i8, i16,
      i32, u8, u16, u32, f16, f32`; `int, float, str`; `'x box, 'x list, 'x
      slice, 'v 'k dict, 'v 'k dice, ['b 'a], [.. 'b 'a], {'k 'v}, {.. 'k
      'v}`. Other candidates: SIMD and GPU acceleration; sets; concurrent
      go-func-esque threads, each with its own input queue and state; a
      charm-like UI framework with a WYSIWYG editor that builds templates and
      components visually; a graphics stack language (sneeze? splat? spill?);
      a query language. Decide: which ones the first app needs.
  1. Each chosen one: expect.slap first, then TYPES, then a readme section.
  2. When SIMD, GPU or threads land, revisit examples/zoom.slap: it needs
     them most. Deep keyframes spend their time in `escape` (about 240 ns
     an iteration, about 185 iterations a pixel near 1e5), and every pass
     is data-parallel: pixels in a row and rows in a slice are independent.
     Candidates: `escape` on several pixels per SIMD lane; a keyframe's rows
     on threads; the tween's enlarge-and-dither as a GPU pass. Time the
     ticks first (`cat examples/zoom.slap` plus a loop of `k (pixel) step`
     under millis, in ./slap-sdl). Then cut the tricks that no longer pay:
     the guesses, the tweens, the line reuse, the two-steps-per-test unroll.

- [ ] You read a post series on slap.
  - 001 why I built it
  - 002 better API, patterns and idioms: write less code and build more DSLs
    (e.g. Elm encoders/decoders)
  - 003 open tag constructors? 'ok/'no and no panic? rethink APIs? set? dict?
    threads? what other batteries to include?
  - 004 a UI framework like charm, and a WYSIWYG UI editor that builds templates
    and components visually
  - 005 a graphics stack language (sneeze? splat? spill?)
  - 006 editor, surfer, filer, feeder, mailer, player, claude, hypercard via a
    charmbracelet-like UI
  - 007 a query language
  - 008 running taylor.town from sauce OS
  - 009 off to scrapscript
  1. You write them. Claude drafts an outline from readme.md and git history on
     request.
