- [ ] A tight stack loop runs at the cost of its cheapest primitives: `if`,
      `let` and `dip` each cost at most twice `swap` (zoom.slap
      microbenchmarks). Where the time goes: run callgrind on zoom's tick on
      pc (`nix-shell -p valgrind --run "valgrind --tool=callgrind ./slap <
      prog"`, then `callgrind_annotate --auto=yes`). clang lays out eval_run's hot
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
  2. Design the next cut and get it approved; callgrind on zoom's tick (pc,
     after body_starts): per-primitive staged/aux bookkeeping in eval_run
     ~12%, frame scans for lookups ~9%, nth_must's scan ~7%.
  3. Keep a change only if every benchmark in step 1 gets no slower.

- [ ] You copy a type from a type error into a signature and it checks:
      messages print types in the signature notation. Today ty_show prints
      `( ..a int -> ..a int )` and `{'ok int 'no str} either`, which a
      signature cannot use. Tradeoff: messages grow, as a result prints
      `dict "ok" 'a var insert "no" 'b var insert either`.
  1. Tests first: change the EXPECT lines in errors.slap that quote a type
     to the new notation; check they fail.
  2. ty_show prints `'a var` (`'a copy` when copyable), `T 'list tag`,
     `[ins] [outs] fn` with `'s rest` heads, dicts of types with rec or
     either, `"_" 'r var insert` for an open rest, and `T 'a as`.
  3. readme's type table drops the "In a message" column; claude.md's
     Checker paragraph drops the shorter-notation sentence.

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
      With no subject, a file app opens the last file. A file app saves by
      tags only, with no file name. Decide: the first app (prose, home, stuff
      and shell were too few options); whether you publish your own ware
      registry.
      - Dir apps:
        - home: the launcher. A grid of app icons (or text) and widgets, and
          search. Or maybe timeline history with graphics?
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
