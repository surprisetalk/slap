## Open

- [ ] You open a feed of up to 16 MiB in feed, such as taylor.town's 4.3 MB
      feed (today 40 of the 917 feeds in taylor-town/feeds.tsv pass the
      1 MiB cap), and a string costs a byte per byte, not a 32-byte slot.
      Chosen 2026-10-22 over a bigger stack (measured: 1.5 GB peak for
      taylor.town). Blocked: approve the design (steps 1-10).
  1. Data: `VAL_BYTES`, one stack slot whose `as.box` points to a heap
     block `{int len, cap; uint8_t b[]}`. `"hi"` is `{tag VAL_BYTES, box ->
     {len 2, b "hi"}}` where today it is 3 slots. One owner, as a dict: a
     copy (dup, a lookup, nth) copies the bytes; a drop frees them.
  2. No new type. `str` and `int list` stay one type; the checker does not
     change. Packing is how the runtime stores a list, never what a
     program sees: same results, same printing, same eq and sort order.
  3. Rule: a list is packed exactly when it is non-empty and every element
     is an int in 0-255. Each word that builds a list keeps the rule:
     string literals, `[...]`, read, tcp-recv, push, set, cat, take-n,
     drop-n, reverse, str-split, each, filter, sort, zip, range.
  4. A word that breaks the rule unpacks once: `"ab" 300 push` gives
     `[97 98 300]` as slots; `[300] 0 0 set` packs again. Unpacking a large
     string can overflow the stack, and the message says so: `push: a
     4194304-byte string becomes a list of values here, since 300 is not a
     byte; that needs 4194305 of the 2097152 stack slots`.
  5. Reading words take either form: len, get, peek, nth, pop, index-of,
     str-find, fold, eq, lt, print, write, tcp-send, insert, of, and every
     record or tagged value that holds a list. nth, get and peek on bytes
     are O(1).
  6. Limits: read returns at most 256 MiB (`"path: 300000000 bytes, more
     than the 268435456 read returns" no`); https-get's cap becomes 16 MiB;
     feed's 512 KiB text cap goes, since items then take a few slots each.
     A malloc that fails dies: `out of memory for a 300000000-byte string`.
  7. Tests first: expect.slap asserts each word gives the same result on a
     packed and an unpacked list, at the boundary values 0, 255, 256 and
     -1; errors.slap gets the unpack-overflow message. suite.py's
     3,000,000-byte literal check becomes a pass (it fits now), and the
     read cap is tested on a sparse file one byte past 256 MiB, which costs
     no disk and which read refuses from its size before reading.
  8. make status's 600 KB feed timing and sort.slap timing must not get
     slower; zoom and uxn benchmarks (todo's ABBA recipe) must not either.
  9. ASan breaker rounds: the runtime trusts the checker, so a missed
     branch shows as memory damage. Run the fuzz rounds claude.md names.
  10. Tradeoff: every list word gains a second path, roughly 40 primitives;
     the strongest alternative, an 8x stack, is a one-line change but keeps
     32 bytes per byte and a 1 GiB runaway ceiling.

- [ ] You add a feed that has moved, or that refuses clients without a
      User-Agent, and feed reads it (83 of the 917 feeds redirect; some 403s
      want a User-Agent). Design approved 2026-10-22.
  1. https-get sends `User-Agent: slap`.
  2. On 301, 302, 303, 307 or 308 with a Location, it follows: an
     `https://host[:port]/path` URL or an absolute `/path` on the same host,
     at most 5 hops. A Location to http:// gives 'no "https-get: e.com/a
     redirects to http://e.com/b, which drops TLS".
  3. Any other Location ("feed.xml") gives 'no naming it. More than 5 hops
     gives 'no "https-get: more than 5 redirects from e.com/a; the last went
     to ...".
  4. Test: run_serve.py's local TLS server answers /a with 301 to
     https://localhost:PORT/b, and a chain of 6, and an http:// Location.

- [ ] You read a feed whose server hangs up without close_notify and sends
      no Content-Length (31 of the 917). Design approved 2026-10-22.
  1. https-get then gives the reply with `'whole 0`; every other reply has
     `'whole 1`. A record field, so callers decide.
  2. feed takes `'whole 0` only when rss-parse succeeds: a cut body loses
     the root's end tag, so an XML parse of it fails.
  3. fetch.slap dies on `'whole 0` with today's message, since it prints
     bodies it cannot check.

- [ ] You read RSS 1.0 (RDF) feeds, ISO-8859-1 feeds, and RSS channels
      without a description (6 of the 917). Blocked: write a design.
  1. Decide per case: rss.slap's strictness is the spec; readers accept
     these. Each needs xml.slap or rss.slap changes and a test.

## Blocked on you

- [ ] A tight stack loop runs at the cost of its cheapest primitives: `if`,
      `let` and `dip` each cost at most twice `swap` (zoom.slap
      microbenchmarks). Where the time goes: run callgrind on zoom's tick on
      pc (`nix-shell -p valgrind --run "valgrind --tool=callgrind ./slap <
      prog"`, then `callgrind_annotate --auto=yes`). clang lays out eval_run's hot
      loop differently when it grows, so a fast path inlined there can make
      zoom slower: keep new paths in noinline helpers. Tradeoff: each fast
      path is more C to keep correct.
      Blocked: design the next cut (step 9).
  1. Benchmark first, by CPU time, minimum of 10 rounds, order alternated
     each round (ABBA): without it the first binary in a pair wins by ~5%.
     The microbenchmark
     `2000000 'N let ('label let 'body let millis 't0 let 0 (dup N lt) (body apply 1 plus) while drop millis t0 sub 'dt let label print dt print) 'bench let`
     with `() "empty" bench`, `(1 (2) (3) if drop) "if" bench`,
     `(5 'v let) "let" bench`, `(1 2 (drop) dip drop) "dip" bench`,
     `(1 2 swap drop drop) "swap" bench`; zoom's tick without SDL
     (zoom.slap, X0/XW/Y0/YW at 640x480, `start`, then `0 (plot) step` to
     `3 (plot) step`, plot ending in five `drop`s); the 600 KB feed; and
     uxn.slap (patch_geometry 320x240) on drool.rom for 60 frames.
  2. Callgrind on zoom's tick (pc, after body_starts): per-primitive
     staged/aux bookkeeping in eval_run ~12%, frame scans for lookups ~9%,
     nth_must's scan ~7%.
  3. Tried and refused: a `VF_PLAIN` flag that let ~25 stack and arithmetic
     primitives skip the staged/aux bookkeeping. ABBA: swap +11%, dip +10%,
     zoom +6%. The branch in eval_run's loop costs more than the
     bookkeeping. Next: design a cut for frame scans or nth_must.
  4. Chosen: nth_must. Every list zoom reads with `nth` is flat, so
     compound_elem is O(1); the scan is frame_lookup. `'old nth` walks up
     several frames, and frame_find scans each frame's bindings in full
     before it moves to the parent.
  5. Design: Frame gains `uint64_t mask`, bit `sym & 63` set for each name
     the frame binds. frame_bind sets it (line ~342, the one place a binding
     is added); frame_end_run and the pool reset clear it. frame_lookup
     skips a frame whose mask lacks the bit, before frame_find.
  6. Every lookup speeds up, not only nth: words and values too. No change
     to eval_run's loop, so no layout trap there. Rejected: a per-call-site
     cache of (hops, index). It hits faster, but it writes into a body,
     which never changes once built, and it needs the call site passed to
     prim_nth.
  7. Failure mode: a bit missing for a bound name hides that binding, so a
     lookup falls through to a parent or the global frame and finds the
     wrong value, or none. A stale bit only costs a scan. Test: a
     shadowing case in expect.slap whose name shares `sym & 63` with a
     parent's name.
  8. Keep a change only if every benchmark in step 1 gets no slower.
  9. Tried and refused: the frame mask (steps 5-7). ABBA: zoom -0.4%, uxn
     +1.7%, empty +3.3%, let +2.3%. The frame walk is not the 7%. Next: on
     pc, `callgrind_annotate --auto=yes` on zoom's tick, and read the lines
     inside prim_nth_impl and frame_lookup before designing again.

- [ ] You paste the type of a word built on `each`, `fold`, `lend`,
      `mutate`, `dip` or `repeat` from a message into its signature, and it
      checks. Today `(each) ['a copy 'list tag ['b sealed 'a copy] ['b sealed
      'c copy] fn] ['c copy 'list tag] effect 'ee let` is refused: "this body
      must turn its inputs into one value". HEAD refuses it too.
      Blocked: approve a design.
  1. Find why a signature's `'b sealed` does not unify with each's sealed
     rest (ty_unify, the `sealed` branch near "cannot reach the stack").
  2. For dip and repeat, `takes` omits the word's own rest, so the pasted
     body's `['c rest]` is unrelated to it. Decide: print the word's rest
     in `takes`, or say that such a type does not paste.

- [ ] You run a pico8 cart headless for N frames and its screen matches a
      reference render. Decided up front: pico8 first; tic80 reuses the
      interpreter later. The screen has 4 greys, so 16 colours rank-map to 4
      like uxn.slap. No audio: sfx/music are no-ops. Speed is ~190k uxn
      instructions/s, so carts run far below 30 fps; correctness first. Lua
      tables cannot be slap values (a lookup of a bound dict copies it, and so
      does dup), so all Lua state lives in one heap threaded on the stack.
      Blocked: approve a design with data shapes and failure modes before building.
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
     pixel dump against a committed reference. Decided: zepto8 makes the
     references, built on pc; real pico8 needs a license.
  9. Then tic80 on the same lua.slap; duskos and decker after.

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
      tags only, with no file name. First app: feed (examples/feed-app.slap); it
      holds at most 512 KiB of text, since each byte takes a stack slot. Decide:
      whether you publish your own ware registry.
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
