- [ ] You write a signature as ordinary data: two lists of type values, ins
      then outs, as in `(2 mul) [int] [int] effect 'double let` and `'triple
      [int] [int] effect`. Prelude words build types as tagged values, so
      `[int str] 'sig let sig len print` prints 2. Decided:
      - Base types are words: int, float, sym, str and socket.
      - Constructors are their tags: `int 'list tag`, `'a var 'dict tag`,
        `'a var 'box tag`. dict and box already name value words.
      - A list holds one type, so a type variable is `'a var`, or `'a copy`
        when the body may copy or drop it.
      - A body type is `[ins] [outs] fn`; `[] [] fn` is today's `()`. A named
        stack rest is `'s rest` at the head of a list, and a sealed one
        `'s sealed`. A body type with no rest runs on the word's rest, as a
        body type in a slot does today.
      - A record type and a tag set are dicts of types, keyed by name:
        `dict "x" int insert rec` and `dict "ok" int insert "no" str insert
        either`. Key "_" holds an open rest: `dict "x" int insert "_" 'r var
        insert rec`. rec is `('rec tag)` and either `('either tag)`, both
        prelude words: a dict holds one type, so no new primitive is needed.
        The parser refuses a key given twice, where insert would replace it.
      - A recursive type is `T 'l as`.
      - TYPES uses the same notation, as `'name [ins] [outs]`, and the same
        parser. Nothing evaluates TYPES.
      The checker reads the two literals before effect as types, as at reads
      its key, and checks them as ordinary literals too; the runtime skips
      them there, as it skips `[...] effect` today. Every type variable has a
      name by construction, so today's `[tagged own in  tagged move out]`,
      which names two tag sets and says "'h' declares ( ..a tagged .. -> ..a
      tagged .. ), but its body is ( ..a tagged .. -> ..a tagged .. )",
      cannot be written. Tradeoffs: the slot modes go; signatures and forward
      declarations stay. Signatures grow: `'each ['s rest 'a var 'list tag
      ['r sealed 'a var] ['r sealed 'b var] fn] ['s rest 'b var 'list tag]`,
      and each result in TYPES is `dict "ok" 'a var insert "no" 'b var insert
      either`. Programs bind str 3 times and rest once as local names; those
      sites get new names.
  1. Tests first; check each fails against the old binary. expect.slap:
     `(2 mul) [int] [int] effect 'double let 5 double 10 eq assert`;
     `'triple [int] [int] effect (3 mul) 'triple let`; `(dup) ['a copy]
     ['a var 'a var] effect`; `[int str] len 2 eq assert`; a list-length
     word declared `[dict "cons" dict "hd" int insert "tl" 'l var insert rec
     insert "nil" [] [] fn insert either 'l as] [int] effect`.
     errors.slap: `(dup) ['a var] ['a var 'a var] effect` says the body
     needs a copyable value; `[int own in] effect` says a signature is two
     lists of types; `['a] [] effect` says a type variable is `'a var`;
     `[dict "x" int insert "x" str insert rec] [] effect` says "x" is given
     twice.
  2. Prelude: the type words, each a `tag` over its payload.
  3. Checker: one parser for type literals replaces ty_parse, ty_parse_fn and
     ty_parse_slots. It accepts only the forms above and names the form it
     expected. Rewrite TYPES in the new notation. Delete own, lent, move,
     auto, in, out, `->`, `|`, the type words seq, tuple and tagged, and the
     `{...}` record and either forms; rec and either are now prelude words.
  4. Runtime: skip `[...] [...] effect` where it skips `[...] effect` today.
  5. Migrate every test signature with a one-off script in the scratchpad,
     and rename the str and rest locals.
  6. readme: a "types are values" section replaces effect annotations and
     protocol constraints; the "tuples" section becomes "bodies", the word
     claude.md uses. claude.md's Checker paragraph (TYPES notation) follows.

- [ ] You pass a value whose cases closed a tag absent to `must`, `pthen` or
      a signature that names that tag. Today `5 ok 'r let r {'ok (drop 1)}
      case drop r must` is refused ("this value is never tagged 'no"): an
      `either` in an input slot makes its tags K_PRESENT, as an output must.
      OCaml types such an input `[< ...]`. Do it with or after the signature
      task, which rewrites the parser.
  1. Tests first; check each fails against the old binary. expect.slap: the
     program above. errors.slap's case "x can never carry 'a after its second
     case" becomes an expect.slap pass.
  2. The type parser gives each tag of an `either` in an `in` slot a fresh
     presence variable instead of K_PRESENT. It generalizes with the scheme
     and is rigid while the body is checked, so a body case that drops the
     tag is still refused. Outputs stay K_PRESENT. TYPES' inputs (`must`,
     `pthen`) follow.
  3. One variable shared by an in slot and an out slot takes the out slot's
     K_PRESENT. Tradeoff: a body that passes such an input to a K_PRESENT
     input of another word is refused, as a rigid variable meets a constant.

- [ ] A tight stack loop runs at the cost of its cheapest primitives: `if`,
      `let` and `dip` each cost at most twice `swap` (zoom.slap
      microbenchmarks). Where the time goes: run callgrind on zoom's tick on
      pc (`nix-shell -p valgrind --run "valgrind --tool=callgrind ./slap <
      prog"`, then `callgrind_annotate --auto=yes`). The big remaining costs:
      `elem_starts` and the element offsets it feeds, since every run of a
      body that holds a nested body walks its elements twice; the
      per-primitive aux and staged bookkeeping in eval_run; frame scans for
      lookups, and `nth`'s scan for its list. clang lays out eval_run's hot
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
  2. Element offsets once per body, not per run, without making a Value
     larger: every compound copy moves Values.
  3. Keep a change only if every benchmark in step 1 gets no slower.

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
      Decide: the first app; what a file app opens with no subject (the last
      file, or an empty one); whether a file app saves by tags only, with no
      file name; and whether you publish your own ware registry.
      - Dir apps:
        - home: the launcher. A grid of app icons (or text) and widgets, and
          search.
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
