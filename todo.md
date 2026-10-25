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

- [ ] You read a brainstorm on Goose's array types
      (https://aardappel.github.io/goose/) for slap: which ideas fit, and
      what each costs.
  1. Read the page. Its idea: the brackets name the length kind. `T[k]`
     fixed, `T[]` frozen at construction, `T[..k]` inline capacity,
     `T[>..]` grow-only, `T[>..<]` grow-shrink, `T[:]` a slice over any
     kind. Arrays sit inline as `[metadata][elements...]` on a scoped data
     stack, never the heap. Strings are `u8` arrays of each kind.
  2. Map it onto slap: flat runs on the operand stack, `VAL_BYTES`, the
     deep copy on a lookup of a bound list, `take-n`/`drop-n` copies, and
     the "slices" and "sized ints" batteries in sauce step 4.
  3. Note which kinds the checker could prove (fixed length, grow-only,
     slice lifetime) and which loops in examples/ they would make O(n).
  4. Write the brainstorm in readme.md under `## Pending design`: each
     adopted idea, then the alternative and its cost.

- [ ] You load a CBOR file whose text string is not UTF-8, and `cd-str`
      refuses it with its byte offset, as RFC 8949 requires. Today
      cal-app.slap and feed-app.slap accept such text and draw it as `?`.
  1. Add a primitive that checks UTF-8, over libc's `mbrtowc` in a UTF-8
     locale, since the house rules forbid hand-rolled unicode. Refuse
     overlong forms, surrogates and code points past U+10FFFF.
  2. Call it in cbor.slap's text-string path; add the failing case to
     cbor.slap's tests first.

- [ ] You run a program that needs libraries without typing them in their
      order: a verdict on how slap composes files, with no `import
      "./foo"` inside a file. Today each app's readme row spells out
      `cat examples/lib/{strings,icn,ufx,ui}.slap examples/clock-app.slap
      examples/lib/ui-sdl.slap examples/clock-app-sdl.slap | ./slap-sdl`.
  1. List every multi-file program in examples/ and tests/suite.py with its
     cat line. Count how often a library joins only for another library
     (ui needs icn and ufx; cbor needs strings).
  2. Weigh three options, each with its cost in slap.c, tooling and
     messages:
     - Better errors: an unknown word that a file in examples/lib/
       defines names the file and the cat line to fix it.
     - A package registry and qualified calls (`strings.int-str`), which
       may fold into the ware app.
     - Bundles: a make target per program cats its files into
       `bin/clock.slap`, so a run is `./slap-sdl < bin/clock.slap`.
  3. Write the verdict in readme.md under `## Pending design`: the choice,
     then the alternatives and their costs. Better errors may land
     whatever the choice.

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
  1. music: a primitive `play` (bytes of 16-bit stereo PCM at 44.1 kHz)
     over SDL_QueueAudio, a no-op sink under `--headless`, and a
     `queued` word for the bytes still waiting. Decode WAV in slap
     (examples/lib/wav.slap, as tga.slap); MP3 and FLAC through vendored
     dr_mp3 and dr_flac (public domain, one header each) as primitives.
     The app lists a directory's tracks and plays them in order; the
     CBOR state keeps the track and offset.
  2. shell (the slap REPL in a window, typed through 'text), home
     (launcher, search), ware (registries; decide later whether you
     publish your own).
  3. Then, unsorted: stuff (files by tag, libraries; SQL or FQL), config,
     web (html/css), code, slides, prose (markdown, vim/leap, minimap,
     image preview, linters, AI editing), sheet, email, phone, chat, video,
     photo, camera, print, map, steno, cast. Unplaced: query, debug, talk,
     claude, hypercard.
  4. Batteries to choose from as apps need them: sized ints and floats,
     slices, `dice`, sets, SIMD, GPU, go-like threads with input queues, a
     charm-like UI framework with a WYSIWYG editor, a graphics stack
     language, a query language. Each: expect.slap, then TYPES, then readme.
  5. When SIMD, GPU or threads land, revisit zoom.slap: `escape` dominates
     deep keyframes, and every pass is data-parallel. Time the ticks
     first, then cut the tricks that no longer pay.
  6. Move the blog and all projects into sauce. taylor.town serves its
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
