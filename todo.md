## Open

- [ ] You read RSS 1.0 (RDF) feeds, ISO-8859-1 feeds, and RSS channels
      without a description. Design approved 2026-10-22.
  1. RDF: `_rss-dispatch` (rss.slap:80) gains a `rdf:RDF` root branch. It
     reads channel (title, link, description via `_rss-opt`) and items
     (via `_rss2-item`) from the root, where they are siblings. `'kind`
     stays "rss", since feed.slap and rss-to-xml switch on it.
  2. Latin-1: at `_xml-parse` entry (xml.slap:218), a declaration of
     ISO-8859-1 or latin1 (any case) transcodes each byte >= 0x80 to two
     UTF-8 bytes. Every other encoding stays refused, windows-1252 too.
  3. No description: rss.slap:43 uses `_rss-opt`, so it reads as `none`.
     rss-to-xml omits the element for `none`, so a round trip is exact.
  4. Tests first in rss.slap's and xml.slap's assert sections: flip the
     refusal cases at xml.slap:564 and rss.slap:277, 381; add an RDF
     case. Update readme ~579 and the comment at rss.slap:11.

- [ ] You paste the type a message prints for a word built on `each`,
      `fold`, `lend`, `mutate`, `dip` or `repeat` into its signature, and it
      checks. Design approved 2026-10-22.
  1. Tests first. expect.slap: each pasted signature checks and runs.
     errors.slap: a pasted each called with a body that reaches below is
     still refused; two different sealed names still clash.
  2. slap.c:853 (ty_bind): a sealed variable meeting a sealed K_SVAR links
     the free one to the rigid one; two rigid ones still clash.
  3. The `takes` message prefixes the word's rest (`'a rest int ['a rest]
     ['a rest] fn`) when a printed body type shares it, so dip and repeat
     types paste.

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
  7. Milestone 1: examples/lua.slap lexer (by index, as json.slap) and
     expressions; tests/run_lua.py diffs `print` against system `lua` on
     tests/lua/*.lua, integers below 32768, no `/`, Lua's `3.0` normalized.
  8. Milestones 2-4, same diff: statements and functions; tables, `pairs`
     and `#`; closures.
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
