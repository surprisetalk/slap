## Open

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
  1. shell (the slap REPL in a window, typed through 'text), home
     (launcher, search), ware (registries; decide later whether you
     publish your own).
  2. Then, unsorted: stuff (files by tag, libraries; SQL or FQL), config,
     web (html/css), code, slides, prose (markdown, vim/leap, minimap,
     image preview, linters, AI editing), sheet, email, phone, chat, video,
     photo, camera, print, map, steno, cast. Unplaced: query, debug, talk,
     claude, hypercard.
  3. Batteries to choose from as apps need them: sized ints and floats,
     slices, `dice`, sets, SIMD, GPU, go-like threads with input queues, a
     charm-like UI framework with a WYSIWYG editor, a graphics stack
     language, a query language. Each: expect.slap, then TYPES, then readme.
  4. When SIMD, GPU or threads land, revisit zoom.slap: `escape` dominates
     deep keyframes, and every pass is data-parallel. Time the ticks
     first, then cut the tricks that no longer pay.
  5. Move the blog and all projects into sauce. taylor.town serves its
     pages (indexed) and assets (not indexed) from sauce.

- [ ] You open a MIDI file in midi, a sauce file app, and bounce it to a
      WAV beside it, each part on the font you chose for that work; audio
      plays the WAV. midi bounces now and becomes an editor later. Decided:
      audio stays audio-only; TinySoundFont, settings inside the .mid, fonts
      outside the repo, a font per part, render a whole movement (BWV 582's
      12 min take 4.1 s at most); stream in chunks only past 10 s, which
      needs `play` to append. Far off, synthesize in slap.
  1. Per part, store settings as the standard events at tick 0 of the
     part's track, which every player obeys: bank select CC0/CC32 and
     program change (preset), CC7 (gain), CC10 (pan), CC91 (reverb send,
     GM2). A font that is not GM (Jeux, Aeolus) numbers its own presets:
     GM's 19 plays Jeux's Gamba 8, unrefused.
  2. Per part, store the font as RP-019's Device Name meta event (FF 09)
     at the start of the track: the font's file name, found in the font
     directory midi's state.cbor names. A missing font stops the work
     with its name.
  3. midi splits a type 0 file into a track per channel (type 1) on its
     first write, with midi-tracks and midi-encode: type 0 holds one
     track, so one font.
  4. A channel that two tracks with different fonts share stops the work
     with both track names; tsf renders by channel.
  5. Figure out reverb: tsf has none, and a dry orchestra or organ sounds
     wrong. Candidates: Freeverb in C (about 200 lines, public domain),
     driven by each channel's CC91; or convolution with a hall's impulse
     response. Decide where the room lives: per work, or per font.
  6. Figure out space and panning: tsf pans each channel by CC10 only, so
     a quartet sits on one line. Weigh seating presets (orchestra, quartet,
     organ loft), distance as reverb send plus a low-pass, and whether
     headphones want HRTF.
  7. Add a compressor, done correctly: each render now scales to a -1 dBFS
     peak (`MIDI_PEAK_DB`), so a piano work with one sharp hit sounds
     quieter than an organ at the same peak. Weigh a loudness target (EBU
     R128) with a look-ahead limiter against a compressor inside a work.
  8. Add humanization: a score typed in note by note (Mutopia, OpenScore)
     plays every note on the grid at one velocity. Weigh small random
     shifts of timing and velocity, a seed kept with the work so a bounce
     repeats, and rubato from phrase shape; piano-midi.de shows the target.
  9. Decode piano-roll expression: scanned rolls (kunstderfuge's
     "(nc)smythe", ~/Music/midi/kunstderfuge/piano-rolls) encode dynamics
     and pedal as notes outside the keyboard; 9 play key 1, which
     render-midi refuses. Identify each roll's system (Ampico, Duo-Art,
     Welte) and map its codes to velocity and CC64.
 10. Then the editor: a parts view (track, font, preset name, gain) with
     keys to change them, which rewrites the work's events.

- [ ] You play a render and it moves at a tempo a pianist would take.
      Done: 255 piano-midi.de works and the nine flat Mutopia Bach files
      play from a `.tempo.mid` beside the source (midi-stretch, or tempo
      marks from the LilyPond score), fitted to the median MusicBrainz
      recording. ~/Music/renders/tempo-audit holds the scripts and tables.
  1. Match the 47 works in tempo-audit/unmatched.txt by hand (arrangements,
     joined movements, too few recordings), add their rows to lengths.tsv,
     and rerun tempo-audit/stretch.py.
  2. Mutopia's other single-tempo files (tempo.tsv, one event, not 60) set
     a tempo on purpose; check the slowest (BWV 622 and 646 at 20 bpm)
     against their recordings before you leave them.

- [ ] You run a tic80 cart with lua.slap, and its screen matches tic80's.
      Done: lua.slap reads a `.lua` cart with `-- script: lua`, draws with
      cls, pix, rect, rectb, line, circ, circb and tri, and two carts match
      TIC-80 v1.1.2837 (tests/tic80, refs from pc's ~/tic80-ref).
  1. Write tests/tic80/fuzz.py like tests/pico8/fuzz.py: random draw carts
     against the pc dumper, one ssh per batch. Fix each cart that differs.
  2. Add print with tic80's system font, then spr and map from the cart's
     sprite and map chunks; each with a cart and a ref.
  3. Add `math`, `string`, `table`, `//` and exponent literals (`1e9`);
     numbers stay 16.16, so time() wraps at 32768 ms; decide whether tic80
     carts get floats.
  4. Show tic80 carts in lua-sdl.slap.
  5. Then duskos and decker.

## Blocked on you

- [ ] You read a post series on slap: 001 why I built it; 002 API,
      patterns and idioms (DSLs, Elm-like decoders); 003 tags, 'ok/'no,
      batteries; 004 a charm-like UI framework; 005 a graphics stack
      language; 006 the apps; 007 a query language; 008 taylor.town from
      sauce; 009 off to scrapscript.
      Blocked: you write them.
  1. Claude drafts an outline from readme.md and git history on request.
