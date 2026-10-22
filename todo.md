## Blocked on you

- [ ] You read RSS 1.0 (RDF) feeds, ISO-8859-1 feeds, and RSS channels
      without a description (6 of the 917 in taylor-town/feeds.tsv).
      Blocked: approve a design.
  1. Decide per case: rss.slap's strictness is the spec; readers accept
     these. Each needs xml.slap or rss.slap changes and a test.

- [ ] A tight stack loop runs at the cost of its cheapest primitives: `if`,
      `let` and `dip` each cost at most twice `swap`.
      Blocked: approve the next cut (step 3).
  1. Benchmark by CPU time, minimum of 10 rounds, order alternated each
     round (ABBA); without it the first binary wins by ~5%. Keep a change
     only if no benchmark gets slower. The microbenchmark
     `2000000 'N let ('label let 'body let millis 't0 let 0 (dup N lt) (body apply 1 plus) while drop millis t0 sub 'dt let label print dt print) 'bench let`
     with `() "empty" bench`, `(1 (2) (3) if drop) "if" bench`,
     `(5 'v let) "let" bench`, `(1 2 (drop) dip drop) "dip" bench`,
     `(1 2 swap drop drop) "swap" bench`; zoom's tick without SDL
     (zoom.slap, X0/XW/Y0/YW at 640x480, `start`, then `0 (plot) step` to
     `3 (plot) step`, plot ending in five `drop`s); the 600 KB feed; and
     uxn.slap (patch_geometry 320x240) on drool.rom for 60 frames.
  2. Refused, do not retry: a `VF_PLAIN` flag that skips staged/aux
     bookkeeping (swap +11%, zoom +6%); a per-frame 64-bit name mask for
     lookups (uxn +1.7%, empty +3.3%). clang lays out eval_run's loop
     differently when it grows: keep new paths in noinline helpers.
  3. On pc, callgrind zoom's tick (`nix-shell -p valgrind --run "valgrind
     --tool=callgrind ./slap < prog"`, `callgrind_annotate --auto=yes`).
     Read the hot lines in prim_nth_impl and frame_lookup, then design the
     cut. Last profile: staged/aux bookkeeping ~12%, frame scans ~9%,
     nth_must ~7%.

- [ ] You paste the type of a word built on `each`, `fold`, `lend`,
      `mutate`, `dip` or `repeat` from a message into its signature, and it
      checks. Today `(each) ['a copy 'list tag ['b sealed 'a copy] ['b sealed
      'c copy] fn] ['c copy 'list tag] effect 'ee let` is refused: "this body
      must turn its inputs into one value".
      Blocked: approve a design.
  1. Find why a signature's `'b sealed` does not unify with each's sealed
     rest (ty_unify, the `sealed` branch near "cannot reach the stack").
  2. For dip and repeat, `takes` omits the word's own rest, so the pasted
     body's `['c rest]` is unrelated to it. Decide: print the word's rest
     in `takes`, or say that such a type does not paste.

- [ ] You run a pico8 cart headless for N frames and its screen matches a
      reference render. Decided: pico8 first; tic80 reuses the interpreter
      later. 16 colours rank-map to 4 greys like uxn.slap. No audio. Carts
      run far below 30 fps; correctness first. Lua tables cannot be slap
      values (a lookup of a bound dict copies it, and so does dup), so all
      Lua state lives in one heap threaded on the stack.
      Blocked: approve a design with data shapes and failure modes.
  1. examples/lua.slap lexer: read the source by index with `nth`, as
     json.slap does. No per-character recursion.
  2. The parser emits a flat int bytecode list. A `while` + step loop runs
     it, like uxn.slap's run-more; the Lua call stack is data.
  3. Heap: tables are int ids; entries live in one dict keyed by encoded
     `id:key` bytes, plus a key list per table for `pairs`. Closures are
     (proto id, upvalue ids).
  4. Numbers are pico8 16.16 fixed point in ints.
  5. tests/run_lua.py diffs `print` output against system `lua` on
     tests/lua/*.lua scripts that stay in integers.
  6. Cart loader for the .p8 text format: **lua**, **gfx**, **map**.
  7. API subset: cls pset pget rectfill circfill line spr map btn print rnd
     flr sin cos; _init/_update/_draw.
  8. tests/run_pico8.py runs a small cart headless and diffs a
     palette-index pixel dump against a committed reference made by
     zepto8 on pc (real pico8 needs a license).
  9. Then tic80 on the same lua.slap; duskos and decker after.

- [ ] You run sauce: your own collection of lofi slap apps, fullscreen in a
      row you slide through, with home leftmost. Decided: fullscreen apps
      only, a launcher like iOS's; later, apps take partial width (full
      height) and slide around, on mobile and desktop. No vigil: nothing
      kept in intermediate state outside physical notes and a single
      working copy; publish sequels, not incremental improvements. slap-sdl
      hosts the apps first; wasm and slap.swift come later. The first
      release is "slap 0". Apps store everything in CBOR
      (examples/lib/cbor.slap) and share a charmbracelet-like UI. A dir app
      opens a general thing. A file app opens one specific thing, as viewer
      and editor; with no subject it opens the last file; it saves by tags
      only. First app: feed (examples/feed-app.slap).
      Dir apps: home (launcher, search), stuff (files by tag, libraries),
      shell (the slap REPL). File apps: ware (package registries), config,
      web, book, feed, code, slides, prose, sheet, email, cal, phone, chat,
      video, music, photo, camera, print, clock, map, steno, cast.
      Unplaced: query, debug, talk, claude, hypercard.
      Blocked: decide whether you publish your own ware registry, and which
      batteries the first app needs.
  1. Batteries to choose from: sized ints and floats (`i8`…`f32`), slices,
     `dice`, sets, SIMD, GPU, go-like threads with input queues, a charm-like
     UI framework with a WYSIWYG editor, a graphics stack language, a query
     language. Each chosen one: expect.slap first, then TYPES, then readme.
  2. When SIMD, GPU or threads land, revisit zoom.slap: `escape` dominates
     deep keyframes (~240 ns an iteration), and every pass is
     data-parallel. Time the ticks first, then cut the tricks that no
     longer pay: guesses, tweens, line reuse, the two-step unroll.
  3. home.slap lists the apps and runs one.
  4. prose.slap: a markdown editor with vim/leap movement, a minimap, image
     preview, linters (like hemingway) and AI editing.
  5. stuff.slap: SQL or FQL finds files by tag, instead of directories.
  6. Move the blog and all projects into sauce. taylor.town serves its
     pages (indexed) and assets (not indexed) from sauce.

- [ ] You read a post series on slap: 001 why I built it; 002 API,
      patterns and idioms (DSLs, Elm-like decoders); 003 tags, 'ok/'no,
      batteries; 004 a charm-like UI framework; 005 a graphics stack
      language; 006 the apps; 007 a query language; 008 taylor.town from
      sauce; 009 off to scrapscript.
      Blocked: you write them.
  1. Claude drafts an outline from readme.md and git history on request.
