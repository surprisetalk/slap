## Blocked on you

- [ ] You run a tic80 cart with lua.slap, and its screen matches tic80's.
      Blocked: approve a design.
  1. Design: tic80's API (240x136 screen, 16-colour palette in RAM, `TIC()`
     each frame) as a second API table beside pico8's, chosen by the cart
     format; its reference renderer, as zepto8 is pico8's.
  2. Then duskos and decker.

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
