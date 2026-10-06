- [ ] You read a bound list with `i 'xs nth`, and every symbol has one type, sym
      (breaker rounds 14 and 15; simplification review). Decided: nth reads its
      name from the symbol written right before it, as at, into and edit read
      their keys. Symbol types lose their labels: no K_LVAR, no `named`, no
      widening, and no eq/neq special case. Then
      `[1 2] 'xs let 'xs 'k let [k 'ys] drop` and `k 'ys member` pass, which nth
      refuses today. Tradeoff: nth's name can no longer come from a let-bound
      symbol; no example does this.
  1. expect.slap first: `[1 2] 'xs let 1 'xs nth must 2 eq assert`, and both
     programs above. errors.slap: `'xs 1 nth` says that nth needs the list's
     name written right before it.
  2. ty_range: S_NTH reads toks[i-1] as S_AT does. prim_nth pops the name, then
     the index.
  3. K_SYM unifies like K_INT. Record literal keys and `on`'s event read the
     symbol of the token that pushed them: the K_SYM a TOK_SYM makes keeps it in
     `ty[t].sym`, and unify ignores it. Test `{'kind 'atom}`, a symbol as a
     value. Delete K_LVAR, ty_label_var, `named`, the label branch in
     ty_unify_chain, the eq/neq special case, and the LVAR branches in ty_copy
     and ty_parse.
  4. Migrate the ~130 nth sites; the checker refuses each old one. `'g i nth` →
     `i 'g nth`; `'g swap nth` → `'g nth`; `'g over nth` → `dup 'g nth`;
     `'g x 10 mod nth` → `x 10 mod 'g nth`. readme (life example) and claude.md
     (Invariants) follow.

- [ ] You do float math with f-words, and the only protocol left is copy
      (simplification review). Decided: as in OCaml, `fplus fsub fmul fdiv flt`
      take floats, and `plus sub mul div lt sort` take ints. num and ord go, and
      a type variable carries one bit: copyable or linear.
  1. errors.slap first: `1.0 2.0 plus` says "float is not int". expect.slap:
     `1.0 2.0 fplus 3.0 eq assert`, `1.0 2.0 flt assert`.
  2. TYPES as decided, with `'sort ( int list -> int list )`. The f-prims reuse
     prim_plus, prim_sub, prim_mul, prim_div and prim_lt. Prelude: fneg uses
     fsub, fabs uses flt. max, min, abs, sqr, clamp, sign, gt, ge and le become
     int-only; add an f-variant only where a program needs it.
  3. Migrate: the checker refuses each site. A scratch build counts about 185:
     raycast 101, plasma 42, zoom 15, the libraries 18, uxn 2, and euler/44 and
     euler/45 3 each.
  4. Delete P_NUM and P_ORD, their words in ty_prot_word, and the protocol names
     in ty_prot_name. readme: arithmetic, floats, and the protocol table become
     one sentence on copy. claude.md follows.

- [ ] You choose between read-time and runtime literals from a measured cost
      (simplification review). Today a `[...]` or `{...}` literal is built once,
      when the program is read, so it cannot use a name bound at runtime:
      `[i i] insert` and `[ 1 mk ]` are refused. Runtime literals delete
      ty_literal, ty_lit_depth, tyb_visible, the VF_DICT copy and about 8
      messages; `{'x x 'y y}` works, so into can be replace-only and the PRE/ABS
      fields and rec go. The cost is speed: a constant literal in a hot loop is
      built on every pass.
  1. In a scratch copy, evaluate a literal when it is reached. Build a literal
     that holds only constants once, if speed needs it.
  2. Time make status (the 600 KB feed and sort.slap), make bench-uxn and make
     test-slow against the current binary: interleave the runs and take the
     minimum, since the machine runs other loads.
  3. Write the numbers into this task, then decide.

- [ ] You read a checker message that names the real cause, in the right
      direction, with one name per variable (code review; silent-failure audit;
      breaker rounds 14 and 15). Decided: keep `box (body) lend` and its sealed
      stack. `( 'g let 5 box (g apply) lend swap free ) 'w let (1 plus) w` stays
      refused: lend's output count is unknown until a caller passes g. mutate
      and each accept such a body only because TYPES fixes their output count.
  1. errors.slap first: pin one message per step below, and check each fails
     against the old binary.
  2. Mismatches read backwards where the actual type is passed first: nth with a
     str index says "int is not a list", and `5 (drop) lend` says "'a box is not
     int". Call `ty_unify(expected, actual)` at nth and lend's box, as
     `ty_apply` and `ty_case` do.
  3. `'apply' takes ( ..a -> ..b ) / but the stack has ... / <why>`: the "takes"
     line shows a fresh copy of the word's type, so the why line uses other
     names. In ty_apply, print the instance and the stack top before `ty_unify`,
     with `ty_print_count` reset once. It is the hottest path: print only when a
     cheap pre-check fails, or keep a copy of the instance and print it after.
     ty_define and the case messages already do this.
  4. Inside a `[...]` literal, `1 [drop]` says "'drop' takes 'a copyable / but
     the stack has nothing": say that a literal's code starts from an empty
     stack.
  5. ty_lend: when `out` ends in a stack variable that is not `below`, say the
     body's output count is unknown at the lend and point to mutate or a body
     written in place. Otherwise keep "may not take values below". After an
     underflow, push only the box's place, not two fresh values, so later errors
     stay real.
  6. A `lent` or `copy` slot accepts a box or socket:
     `(free) [int box lent in] effect` passes. `ty_mark_copy` dies with an
     annotation error on K_BOX and K_SOCK.
  7. "one path leaves N more values ... a branch, clause, loop pass or recursive
     call" appears where there is no branch. A let-bound body used at two depths
     through a word, dip or if gets no let hint and can read "'g' takes int /
     but the stack has int / the stack is shorter": ty_apply adds the hint only
     when the body is a direct input of the failing word, as in
     `[(1 plus)] first 'f let (f apply) 'g let 1 g 2 3 g`. Tag-set variables
     print as an unnamed `tagged ..`, so "declares X, but its body is X" can
     show two equal types: name them like row variables. "program too long"
     prints the whole 160 KB source line. `{'a int | 'r | 's}` silently drops
     'r: refuse a second `|`.
  8. `'x [strng lent in] effect` reports "at line 1" in the text, with the caret
     at column 1. Pass the annotation's token to the error, as `ty_err` does for
     words; drop the "at line %d" text. errors.slap: EXPECT-COL on an unknown
     type word.
  9. readme: a signature passes a value through unchanged only when both slots
     name one variable. `[tagged own in  tagged move out]` is two tag sets, so a
     body that returns its input is refused with "a type the signature leaves
     open is ...". Write `['t own in  't move out]`: one sentence and the
     example beside "A signature is a promise for every type it allows".
  10. Stale: the comment above `binding_release` names the old checker's box
      bindings; claude.md's Frames paragraph gives "about a tenth of the run
      time". errors.slap: merge "a program redefines ok", "a program redefines
      no" and "a word redefined after a word that calls it" into the "already
      defined" block.

- [ ] The JSON and XML libraries refuse what they cannot read or write, and say
      where (silent-failure audit; breaker round 15). Accepted today: leading
      zeros (`"0123"` gives 123), duplicate object keys (first wins), duplicate
      attributes (first wins), an `encoding="ISO-8859-1"` declaration (its bytes
      read as UTF-8), and a float past the range (`"2e308"` gives `inf ok`).
      `"0.3"` does not equal `0.3`: the fraction is built as digit x 0.1^n. JSON
      refuses nesting at 257 and XML accepts 257. rss-to-xml writes RSS for any
      'kind but "atom" ("Atom" too). `_xe-render-attrs` and json `je-obj` read
      caller-built pairs with `nth must`/`get must`: an attribute of 3 items
      renders 2 with no error. `jd-one-of`/`xd-one-of` say only "oneOf: no
      decoders matched". Parse errors ("json: expected , or ] in an array",
      "json: trailing input", "xml: expected <", ...) give neither position nor
      byte: on a 600 KB feed there is no other way to find the fault.
  1. One failing test per case in json.slap/xml.slap/rss.slap; one pinned
     message each for the parse errors.
  2. json: refuse a leading 0 before a digit, a repeated key in one object, and
     a non-finite result; build the fraction as an integer mantissa and scale it
     once.
  3. xml: refuse a repeated attribute name and any declared encoding but UTF-8
     or US-ASCII. One nesting limit, one comparison, in both libraries.
  4. rss-to-xml accepts "rss" or "atom" only. `_xe-render-attrs` and `je-obj`
     take records `{'name 'value}`.
  5. Parse errors give the byte offset and the byte found.
     `jd-one-of`/`xd-one-of` list each alternative's error: "oneOf: [0] .a:
     expected int, got string; [1] ...".

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

- [ ] You drag a slap window's edge, and the canvas takes the new size. A new
      window opens at the size of the screen. Decided: the OS sets the size,
      and no word changes it. A `'resize` handler takes `w h`. show runs it
      once before the first tick and again after each change. One canvas pixel
      is one window point. Tradeoff: a program that draws for 640x480 fills
      only the top-left corner of a large window, since pixel and fill-rect
      clip.
  1. errors.slap first: `'resize (drop) on (drop) show` says that the handler
     must take the event's w and h.
  2. Checker (`S_ON`): add 'resize to the event list and its message. Its
     handler takes two ints, as a mouse handler does (`ty_on_mouse`).
  3. Runtime: the canvas and the pixel buffer become heap blocks sized by
     `canvas_w` and `canvas_h`. sdl_init opens the window at
     SDL_GetDisplayUsableBounds. On SDL_WINDOWEVENT_SIZE_CHANGED, reallocate
     the canvas (cleared to 0), the buffer and the texture, then run the
     'resize handlers. Delete CANVAS_W, CANVAS_H and SDL_RenderSetLogicalSize.
  4. Headless runs 'resize once with 640 480 before tick 0. Test: suite.py
     builds slap-sdl and runs headless a program whose 'resize handler prints
     w and h and whose 'tick handler ends the run with fail. It expects
     `640 480` and the fail text.
  5. wasm: shell.html sizes the canvas to the viewport, not 640x480. Then
     sdl_init asks for SDL_WINDOW_RESIZABLE on wasm too, and the comment that
     explains why it does not goes.
  6. Audit each example that hard-codes the size
     (`grep -ln '640\|480' examples/*.slap`): it reads the 'resize size or
     keeps its 640x480 region. The readme's SDL section and slap-wasm
     paragraph follow.

- [ ] You save a slap value to a file as CBOR and read it back unchanged (the
      lofi apps store everything in CBOR). Decided: examples/lib/cbor.slap
      gives encoders (`ce-*`) and decoders (`cd-*`) in the shape of
      json.slap's `je-*` and `jd-*`, over byte lists. The encoder writes the
      shortest head for each int and length; floats are always float64. The
      decoder refuses what the apps never write: tags, indefinite lengths,
      simple values other than false, true and null, a repeated map key, and
      trailing bytes. Each refusal is a 'no with the byte offset and the byte
      found.
  1. Tests first: RFC 8949 Appendix A lists values beside their hex. Each row
     in scope becomes two assertions: decode the hex, and encode the value.
  2. Add `float-bits ( float -> int )` and `bits-float ( int -> float )` to
     TYPES, a memcpy each: no word gives a float's IEEE bits. The decoder reads
     float16, float32 and float64.
  3. An int past the int64 range is a 'no, not a wrapped value.
  4. suite.py runs cbor.slap's self-test, as it runs the other libraries. The
     readme gets a section beside json.

- [ ] You type slap at a prompt in the terminal and see the stack after each
      line ("a nice slap shell"). Decided: there are two shells, one in the
      terminal and one in sauce (shell, below). Both run one loop in slap.c,
      so the slap-sdl and wasm builds of sauce get it free; slap.swift needs
      its own copy. The terminal shell comes first, since sauce does not
      exist yet.
  1. `./slap` with a TTY on stdin reads lines. The checker's stack type and
     the global frame persist across lines. An error discards only its line.

## Blocked on you

- [ ] You read pthen in the prelude, not in C, and the 600 KB feed still renders
      in under a second (simplification review). Measured:
      `('pf let 'pd let {'ok (pf apply) 'no (no pd swap)} case) 'pthen let`
      after default in PRELUDE passes make test, but the feed in make status
      runs about 23% slower than with prim_pthen (best of 5 on one machine: 1.26
      s against 1.02 s). The feed scored 1.23 with prim_pthen, so the prelude
      version puts it at the 1.0 line. Decide: keep pthen in C, or move it and
      win the time back elsewhere (for example, a faster let or case in
      eval_body).
  1. If moved: add the prelude line; delete the TYPES entry, `R(pthen,pthen)`
     and prim_pthen. make status must still pass.

- [ ] A closure stored in an outer frame's binding is freed when nothing reaches
      it (code review). A cycle is never freed: a closure made by a nested body
      and stored in an outer frame's binding keeps that frame, which keeps the
      closure's frame.
      `( 'k let ( k apply 1 plus) ) 'wr let 0 100000 (drop (0 plus) 3 (wr) repeat 'c let 0) repeat drop`
      reaches 201 MB: `(0 plus)` closes over the outer body's frame, and the
      chain ends in that frame's binding `c`. With `(0 plus)` bound at the top
      level it stays at 4.5 MB. Decide: weak parent links, or a collector.
  1. Tests first: a loop that makes such a cycle each pass holds steady memory.

- [ ] You apply a body while a copy of it is on the stack (breaker round 15 b1).
      `(5) dup apply print drop` is refused: `dup` gives both copies one type,
      and applying one changes the depth below the other. This caused nearly all
      of the 145 refusals among 66,000 generated well-typed programs. Body cat
      is gone, so only dup remains. Decide: keep the refusal (a let-bound body
      runs at one depth too), or give each copy of a body value its own stack
      rest when nothing else holds it, which brings back the fresh-rest
      machinery (ty_bound_rest, ty_gens_settle) that let-bound bodies lost.
  1. Tests first for the choice: the program above.

- [ ] The example apps report every I/O failure and keep the data they do not
      change (silent-failure audit, critical; breaker round 15). `read` gives
      `path no` for every failure, so a directory, a mode-000 file or an I/O
      error reads as "no file": todo.slap lists nothing and exits 0, and with a
      write-only file `add` replaces the items. kv-server's boot `save-snapshot`
      then writes the empty store back. todo.slap rewrites only text/done and
      "items", dropping other fields and keys. kv-server: a recv error reads as
      EOF; send errors vanish; SAVE always says "snapshot not writable"; a torn
      last snapshot line loads as a short value; a client that connects and
      sends nothing blocks every other client. feed.slap cuts text by bytes,
      which can split a UTF-8 sequence. Decide: `read` reports why (errno text
      in the 'no payload, e.g. "path: not found"), or programs check existence
      with `ls` of the parent. Recommend the errno text: one change in
      `prim_read`, and every caller can tell. And for kv-server: a
      per-connection read timeout, or the readme says it serves one client at a
      time.
  1. tests/run_todo.py and run_kv first: a directory, a mode-000 file and a
     write-only file each exit nonzero naming the path; the file keeps its
     bytes.
  2. todo.slap:47 and kv-server.slap:53: start empty only on "not found";
     anything else dies with the path and the reason. A path with a NUL byte
     gives the reason "contains a NUL byte" (read, write and ls give 'no for it
     today, with the path as payload).
  3. todo.slap keeps the decoded JSON and changes only those fields. kv-server
     reports each error with its reason and refuses a line without TAB and
     newline. feed.slap cuts at a character boundary.

- [ ] A decoder tells a missing optional field from a present but malformed one
      (silent-failure audit, critical). `jd-maybe`/`xd-maybe` turn every failure
      into `none ok`: `{"a":"x"}` with `"a" jd-int jd-field jd-maybe` gives
      none. rss's `_rss-opt-text` (`xd-child xd-maybe ("" default)`) makes
      `<title>A <b>bold</b> post</title>` and Atom `<content type="xhtml">` give
      "", and lets an RSS channel without its required title, link or
      description parse; rss-to-xml then writes `<pubDate></pubDate>` and
      `<link href=""/>`. Decide: jd-maybe/xd-maybe give `none` only for "missing
      field"/"no such child" and pass every other 'no on (Elm's `maybe` swallows
      all errors; `optionalField` does not); rss keeps absent optional fields as
      `none`, not "". Text with element children: take the text of xhtml
      content, or refuse it.
  1. Tests first: the three inputs above, and a channel without a title.
  2. jd-field/xd-child failures carry a distinct 'missing payload shape (or
     message prefix) that the maybe decoders test.
  3. rss.slap: required fields use xd-child without maybe; rss-to-xml skips
     absent fields.
  4. `_rss-link` and `_atom-link-href` use xd-maybe too: a `<link>` with element
     children, or a malformed href, reads as no link.
  5. Links are text, never checked as URLs: three breaker rounds each found one
     (an empty href, a space-only href, then `href="&#160;"`, which gives
     `[194 160] ok` since XML whitespace is ASCII only). Decide: rss-parse
     refuses a link that is not an absolute http(s) URL, or keeps the text as
     written.

- [ ] You run sauce: your own collection of lofi slap apps, fullscreen in a
      row you slide through, with home leftmost. Decided: fullscreen apps
      only, starting from home, a launcher like iOS's. Slide apps left and
      right; home is always leftmost. Later, apps take partial width (full
      height) and slide around; this works on mobile and desktop. No vigil:
      nothing kept in intermediate state outside physical notes and a single
      working copy; publish sequels, not incremental improvements. Write the
      apps in slap. slap-sdl hosts them first; wasm and an interpreter in
      Swift (slap.swift) that loads the ROMs come later. The notes name the
      first release "slap 0". The apps store everything in CBOR (task above)
      and share a charmbracelet-like UI. A dir app opens a general thing. A
      file app opens one specific thing, and it is always a viewer and an
      editor. Decide: the first app; what a file app opens with no subject
      (the last file, or an empty one); whether a file app saves by tags only,
      with no file name; and whether you publish your own ware registry.
      - Dir apps:
        - home: the launcher. A grid of app icons (or text) and widgets, and
          search.
        - stuff: a table of files with an action column (e.g. edit image). No
          file names: tags and thumbnails. It holds libraries too (books,
          albums).
        - shell: the slap REPL in sauce. It runs the terminal shell's loop
          (task above).
      - File apps:
        - ware: package registries. You add recommended registries or custom
          ones. Each registry holds apps. Each app has an author, a changelog
          and updates, a description, screenshots, etc.
        - config: tk.
        - web: tk. An html/css browser.
        - book, feed, code, slides, prose, sheet, email, cal, phone, chat,
          video, music, photo, camera, print, clock, map, steno, cast.
      - Earlier notes name apps with no match above: query, debug, talk,
        claude, hypercard.
  1. Move the blog and all projects into sauce as slaps/scraps. taylor.town
     serves its pages (indexed) and assets (not indexed) from sauce.
  2. home.slap lists the apps and runs one.
  3. prose.slap: a markdown editor with vim/leap movement, a minimap, image
     preview, linters (like hemingway) and AI editing.
  4. stuff.slap: SQL or FQL finds files by tag, instead of navigating
     directories.

- [ ] The language has the batteries the apps need. The notes sketched these
      types: `i8, i16, i32, u8, u16, u32, f16, f32`; `int, float, str`;
      `'x box, 'x list, 'x slice, 'v 'k dict, 'v 'k dice, ['b 'a], [.. 'b 'a], {'k 'v}, {.. 'k 'v}`.
      Other candidates: SIMD and GPU acceleration; sets; concurrent
      go-func-esque threads, each with its own input queue and state; a
      charm-like UI framework with a WYSIWYG editor that builds templates and
      components visually; a graphics stack language (sneeze? splat? spill?);
      a query language. Decide: which ones the first
      app needs.
  1. Each chosen one: expect.slap first, then TYPES, then a readme section.

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
