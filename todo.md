## Open

- [ ] You run a pico8 cart headless for N frames and its screen matches a
      reference render. Design approved 2026-10-22. pico8 first; tic80
      reuses the interpreter later. No audio. Correctness first: an
      estimated 100-200k VM ops/s, so a small cart's 60 frames take 10-20 s.
  1. A Lua value is one int, `payload*8 + tag`: 0 nil, 1 bool, 2 num
     (signed 16.16), 3 str id, 4 table id, 5 closure id, 6 builtin id, 7
     cell id. Tagged slap values lose: they span slots, so `nth` is O(n).
  2. Machine: one flat int list at fixed offsets, as uxn.slap: registers,
     bytecode `[op arg]` pairs, Lua stack (4096), call stack (200 frames
     of ret-pc, base, closure, nargs), protos, 128x128 screen of palette
     indices, gfx, map, flags, buttons. Loops use `while`, not `each`.
  3. Heap: one int dict above the machine, never `let`-bound (a lookup
     would copy it). 8-byte keys: kind, id, key. Kinds: entry, head, tail,
     next-link (insertion order for `pairs`), length, cell, closure cell.
     Captured locals box at compile time; no open upvalues. No GC: the
     heap bound crashes a cart that allocates every frame.
  4. Number words: slap `mod`/`div` truncate and `shr` is logical, so
     pico8 `%`, `\` and `>>` get floor/arithmetic words; add/sub/mul wrap
     to 32 bits.
  5. Compile-time crash with line and fix: metatables, coroutines, `goto`,
     varargs, string methods, `load`, `require`. In scope: `+= != ?`,
     `\`, bit ops, `if (c) stmt`, `//`, `#`, sub tostr tonum add del all
     count foreach min max mid abs sgn. Unknown API calls crash naming it.
  6. Bounds crash and name the counter: instructions per frame, heap
     entries, strings, call depth, Lua stack, table ids.
  7. Done: lexer, expressions, statements, functions, closures (cells made
     per declaration; see lua.slap's compiler comment). Next, milestone 3:
     tables, `pairs`, `#`, `.`/`[]`/`:` access, table constructors, in the
     tests/run_lua.py diff.
  8. Assignment to table fields needs the object and key below the value:
     a multiple assignment with several table targets must find each pair.
  9. Milestone 5: .p8 loader (__lua__, __gfx__, __map__) into
     `{'lua str 'gfx int list 'map int list 'flags int list}`; a bad hex
     digit names section and line. Golden arrays from a tiny cart.
  10. Milestone 6: cls pset pget rectfill circfill line spr map btn print
     flr sin cos; _init/_update/_draw. tests/run_pico8.py diffs a 16-index
     pixel dump against a zepto8 reference made on pc. Test carts avoid
     `rnd` until a bit-exact PRNG lands. Greys only at display.

## Blocked on you

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
      only. First app: feed (examples/feed-app.slap), done.
      Blocked: sort the app list in step 1 by priority. Decide later
      whether you publish your own ware registry.
  1. Apps, unsorted: home (launcher, search), stuff (files by tag,
     libraries; SQL or FQL), shell (the slap REPL), ware (registries),
     config, web (html/css), book, code, slides, prose (markdown, vim/leap,
     minimap, image preview, linters, AI editing), sheet, email, cal,
     phone, chat, video, music, photo, camera, print, clock, map, steno,
     cast. Unplaced: query, debug, talk, claude, hypercard.
  2. Batteries to choose from as apps need them: sized ints and floats,
     slices, `dice`, sets, SIMD, GPU, go-like threads with input queues, a
     charm-like UI framework with a WYSIWYG editor, a graphics stack
     language, a query language. Each: expect.slap, then TYPES, then readme.
  3. When SIMD, GPU or threads land, revisit zoom.slap: `escape` dominates
     deep keyframes, and every pass is data-parallel. Time the ticks
     first, then cut the tricks that no longer pay.
  4. Move the blog and all projects into sauce. taylor.town serves its
     pages (indexed) and assets (not indexed) from sauce.

- [ ] You read a post series on slap: 001 why I built it; 002 API,
      patterns and idioms (DSLs, Elm-like decoders); 003 tags, 'ok/'no,
      batteries; 004 a charm-like UI framework; 005 a graphics stack
      language; 006 the apps; 007 a query language; 008 taylor.town from
      sauce; 009 off to scrapscript.
      Blocked: you write them.
  1. Claude drafts an outline from readme.md and git history on request.
