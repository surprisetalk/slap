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
      outside the repo, a font per part, render a whole movement. Later:
      stream in chunks; far off, synthesize in slap.
  1. midi app, first cut: open one .mid (with none, the last); list its
     channels with each one's program; b bounces to NAME.wav beside it,
     every channel on the font state.cbor names. ~/Music/renders/render.slap
     holds the WAV header code to start from.
  2. Per part, store settings as the standard events at tick 0 of the
     part's track, which every player obeys: bank select CC0/CC32 and
     program change (preset), CC7 (gain), CC10 (pan), CC91 (reverb send,
     GM2). A font that is not GM (Jeux, Aeolus) numbers its own presets:
     GM's 19 plays Jeux's Gamba 8, unrefused.
  3. Per part, store the font as RP-019's Device Name meta event (FF 09)
     at the start of the track: the font's file name, found in the font
     directory midi's state.cbor names. A missing font stops the work
     with its name.
  4. midi splits a type 0 file into a track per channel (type 1) on its
     first write, with midi-tracks and midi-encode: type 0 holds one
     track, so one font.
  5. A channel that two tracks with different fonts share stops the work
     with both track names; tsf renders by channel.
  6. Render whole: BWV 582 (12 min) decodes in 59 ms and renders in 1.7 s
     on MuseScore_General, 4.1 s on Jeux's principal chorus. Stream only
     if a movement takes past 10 s; that needs `play` to append.
  7. Figure out reverb: tsf has none, and a dry orchestra or organ sounds
     wrong. Candidates: Freeverb in C (about 200 lines, public domain),
     driven by each channel's CC91; or convolution with a hall's impulse
     response. Decide where the room lives: per work, or per font.
  8. Figure out space and panning: tsf pans each channel by CC10 only, so
     a quartet sits on one line. Weigh seating presets (orchestra, quartet,
     organ loft), distance as reverb send plus a low-pass, and whether
     headphones want HRTF.
  9. Add a compressor, done correctly: each render now scales to a -1 dBFS
     peak (`MIDI_PEAK_DB`), so a piano work with one sharp hit sounds
     quieter than an organ at the same peak. Weigh a loudness target (EBU
     R128) with a look-ahead limiter against a compressor inside a work.
 10. Add humanization: a score typed in note by note (Mutopia, OpenScore)
     plays every note on the grid at one velocity. Weigh small random
     shifts of timing and velocity, a seed kept with the work so a bounce
     repeats, and rubato from phrase shape; piano-midi.de shows the target.
 11. Decode piano-roll expression: scanned rolls (kunstderfuge's
     "(nc)smythe", ~/Music/midi/kunstderfuge/piano-rolls) encode dynamics
     and pedal as notes outside the keyboard; 9 play key 1, which
     render-midi refuses. Identify each roll's system (Ampico, Duo-Art,
     Welte) and map its codes to velocity and CC64.
 12. Then the editor: a parts view (track, font, preset name, gain) with
     keys to change them, which rewrites the work's events.

- [ ] You play a render and it moves at a tempo a pianist would take.
      Audit (31 Oct): render-midi times files correctly; the files are
      fast or flat. piano-midi.de runs 8-19% shorter than the median
      MusicBrainz recording (Chopin, Debussy, Liszt, Grieg, Ravel: 14 of 22
      below the 25th percentile). Nine Mutopia Bach files hold one tempo,
      60, LilyPond's default with no `\tempo`: BWV 565's Adagio and
      Prestissimo play at one speed.
  1. Per work, store a stretch inside the .mid: scale its own FF 51 tempo
     events with midi-tracks and midi-encode, as midi's editor will. Never
     a flag on render.sh. Write the stretched file beside the source, never
     over it.
  2. Stretch piano-midi.de works to their median recording length; the
     audit's scripts and results are in ~/Music/renders/tempo-audit.
  3. For BWV 565 and the other eight (tempo.tsv: one event, 60), find a
     performed source on kunstderfuge, or write the score's tempo marks
     into the .mid as tempo events.

- [ ] You run a tic80 cart with lua.slap, and its screen matches tic80's.
      Decided: tic80's API (240x136 screen, 16-colour palette in RAM,
      `TIC()` each frame) is a second API table beside pico8's, chosen by
      the cart format.
  1. Build tic80 headless on pc as the reference renderer, as zepto8 is
     pico8's; write tests/tic80/refs.sh beside tests/pico8/refs.sh.
  2. Add the API table and a first cart diffed pixel for pixel.
  3. Then duskos and decker.

## Blocked on you

- [ ] You read a post series on slap: 001 why I built it; 002 API,
      patterns and idioms (DSLs, Elm-like decoders); 003 tags, 'ok/'no,
      batteries; 004 a charm-like UI framework; 005 a graphics stack
      language; 006 the apps; 007 a query language; 008 taylor.town from
      sauce; 009 off to scrapscript.
      Blocked: you write them.
  1. Claude drafts an outline from readme.md and git history on request.
