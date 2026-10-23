## Open

- [ ] You read a verdict on Valen's group borrowing
      (https://verdagon.dev/blog/valen-group-borrowing) for slap: adopt,
      adapt, or decline, with the cost of each.
  1. Read the post. Its idea: a reference carries the path it points into
     (`world.entities[]`). A write invalidates references inside the
     written object, not references to it. Signatures name the paths they
     write.
  2. Map it onto slap's ownership: `P_COPY`, one-owner boxes and dicts,
     deep copy on `dup`/`peek`/`nth`/`of`/`lend`, and the sealed stack
     under `each`, `fold`, `edit`, `mutate`, `lend`.
  3. Find the deep copies and O(n²) loops it would remove, such as a
     lookup of a bound list. Count them in examples/ and tests/.
  4. Write the verdict in readme.md under `## Pending design`: the choice,
     then the alternative and its cost in checker size and messages.

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
      only.
  1. cal events: a 'text event (SDL_TEXTINPUT, a UTF-8 string) in
     slap.c, for typing; shell needs it too. Then cal keeps events in a
     CBOR file, marks their days in the grid, lists the chosen day's events
     under it, and adds one with the typed text.
  2. book: read a text file a screen at a time; the CBOR state keeps the
     page.
  3. music, then shell (the slap REPL), home (launcher, search), ware
     (registries; decide later whether you publish your own).
  4. Then, unsorted: stuff (files by tag, libraries; SQL or FQL), config,
     web (html/css), code, slides, prose (markdown, vim/leap, minimap,
     image preview, linters, AI editing), sheet, email, phone, chat, video,
     photo, camera, print, map, steno, cast. Unplaced: query, debug, talk,
     claude, hypercard.
  5. Batteries to choose from as apps need them: sized ints and floats,
     slices, `dice`, sets, SIMD, GPU, go-like threads with input queues, a
     charm-like UI framework with a WYSIWYG editor, a graphics stack
     language, a query language. Each: expect.slap, then TYPES, then readme.
  6. When SIMD, GPU or threads land, revisit zoom.slap: `escape` dominates
     deep keyframes, and every pass is data-parallel. Time the ticks
     first, then cut the tricks that no longer pay.
  7. Move the blog and all projects into sauce. taylor.town serves its
     pages (indexed) and assets (not indexed) from sauce.

## Blocked on you

- [ ] You run a tic80 cart with lua.slap, and its screen matches tic80's.
      Blocked: approve a design.
  1. Design: tic80's API (240x136 screen, 16-colour palette in RAM, `TIC()`
     each frame) as a second API table beside pico8's, chosen by the cart
     format; its reference renderer, as zepto8 is pico8's.
  2. Then duskos and decker.

- [ ] You read a post series on slap: 001 why I built it; 002 API,
      patterns and idioms (DSLs, Elm-like decoders); 003 tags, 'ok/'no,
      batteries; 004 a charm-like UI framework; 005 a graphics stack
      language; 006 the apps; 007 a query language; 008 taylor.town from
      sauce; 009 off to scrapscript.
      Blocked: you write them.
  1. Claude drafts an outline from readme.md and git history on request.
