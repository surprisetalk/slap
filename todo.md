- [ ] borrow checking plus reference counting: https://verdagon.dev/blog/ante-blending-borrowing-rc
  - investigated and deferred. RC fixes none of the defects that were actually
    there, and costs 3 of the 6 linear-discipline probes (`42 box dup` and
    friends would become CLEAN_RUN). Box wants write-through on `mutate` while
    dict wants copy-on-write, and the ~69 bare `sp--` sites give RC nowhere to
    hook. RC for lists/records is worse still: compounds are flat Value runs on
    the stack with no heap identity, and heap-boxing them would destroy the O(1)
    in-place `set` that chip8/uxn are built on.
  - what the investigation did turn up, now fixed: the frame value arena leaked
    on every growing rebind (an accumulator loop died at ~520 iterations);
    `nth`/`of` pushed heap pointers without copying (SIGTRAP / silent wrong
    answers); a self-referential box segfaulted the copy and free walks; the
    lend guard fired on the four provably-safe content types and missed the two
    that alias; a let-bound dict aliased its heap object; sockets leaked a
    BoxData per send/recv/accept.
  - both remaining gaps are now closed, so `tests/adversarial/` carries no
    KNOWN-GAP entries: a tagged output built from a linear input is itself
    linear (so `'x tag` no longer launders a box past `insert`/`push`/`drop`/
    `dup`), and a Box binding is single-use across all its lookups (`free`,
    `tcp-close` and `clone` retire it; `swap` and the tcp-* pair, which hand the
    box back out, do not).
  - chasing the frame arena also turned up three `Value buf[n]` VLAs sized by
    data rather than by a constant -- `take-n`/`drop-n`, `let`, and `lend`'s
    result staging. Each overflowed the 8 MiB C stack past ~250k slots and died
    as a bare SIGSEGV; that is what made euler/10 pass or fail run to run. None
    of them needed a buffer at all.
  - the frame save buffer is measured and now grows on demand instead of being a
    fixed 4194304-Value BSS array (134 MB of address space). Peak across the
    whole suite is 90,414 slots; typical programs use a few hundred.
  - the frame value arena itself cannot be made growable the same way.
    `dispatch_word` hands `&frame->vals[offset]` to `eval_tuple_scoped` and the
    body it runs binds names in that same frame, so a realloc would free the
    code mid-execution. Getting past it means either copying every dispatched
    body (the hot path) or a chunked arena where `Binding.offset` is no longer
    a plain index. Not worth it: the array is demand-zero, so max RSS is 5 MB
    for chip8 and 100 MB for euler/10, which really does touch a million slots.
  - revisit only with a concrete program that needs shared mutable cells.

- [ ] replicate some big projects using only slap to confirm it works
  - done: `examples/wiki.slap` (HTTP wiki server), `examples/kv-server.slap`+`kv-client.slap` (TCP key/value store), `examples/chip8.slap` (CHIP-8 emulator), `examples/uxn.slap` (Uxn/Varvara emulator) — all wired into `make test`
  - uxn is validated against another implementation, not just its own self-test:
    `./slap --headless game.rom [frames]` dumps the canvas as palette indices,
    and 8 of the 9 ROMs in mkeeter/raven's snapshot suite match its reference
    renders pixel for pixel (the 9th differs by 22 pixels of audio VU meter).
    That found three real Screen bugs the self-test had missed.
  - that comparison is now checked in as `make test-uxn-refs`
    (`tests/run_uxn_refs.py`), kept out of `make test` because it downloads the
    ROMs and reference renders from GitHub at a pinned SHA; they cache under
    `tests/.uxn-refs/` so only the first run needs network. The frame count is
    load-bearing and matches raven's 60 redraws -- at 2 frames `screen.rom`
    renders the reference image shifted two pixels, which reads as a Screen bug
    and is not one.
  - `make test-uxn-sweep` pins whether each ROM's render moves with the frame
    count. Seven are byte-identical from 1 frame to 240; only `screen` and
    `audio` animate. That separates "renders correctly" from "renders correctly
    at exactly 60 because two errors cancelled there", which the 60-frame-only
    comparison cannot. mandelbrot is skipped and says so: 0 frame-loop
    instructions at any count, ~5.5 min per render.
  - `make bench-uxn` reports throughput off the dump's new `INS` line (frame-loop
    instructions only, setup subtracted by running each ROM at 0 frames too).
    `screen.rom` ~83k uxn instructions/sec (26k/frame, sprite-blit bound) and
    `drool.rom` ~168k (21k/frame, compute bound). Those two are the only ROMs
    here with real per-frame work -- everything else paints once and idles.
    drool has no reference render in raven's suite, so it is marked
    `allow=None` and excluded from the comparison and the sweep; its rate times
    work that is NOT verified correct, which is why screen stays the headline.
  - bunnymark is NOT a usable benchmark and should not be retried: its RNG is a
    self-modifying xorshift seeded from `DEI2 0xc0` (Datetime), which uxn.slap
    deliberately does not implement. Zero is xorshift's fixed point, so the RNG
    emits 0 forever, every bunny lands at (0,0), the counter at 0x063f never
    advances, and the ROM idles at 425 instructions/frame drawing its header.
    A real Datetime device would be the prerequisite, not a harness change.
  - next: pico8/tic80 both need a Lua interpreter first — that's the real next project, not another emulator shell. decker or duskos are closer to reach.

- [ ] convert my personal app library to slap (e.g. snews, snail)?

<!--
- no vigil. nothing kept in intermediate state outside of physical notes and single working copy. publish sequels not incremental improvements.
- fullscreen apps only. starts with app launcher like ios.
  - slide apps left/right, the launcher is always leftmost. eventually, apps can take up partial width (full height) and slide around.
    - this works very nicely on mobile and desktop

implement lots of emulators: pico8, tic80, uxntal, duskos, decker, etc.

apps: launch, write, surf, watch, query, chat, talk

write the apps in slap, and then write an interpreter in swift that loads the roms

slap 0

slap.swift
launch.slap
write.slap
code.slap
surf.slap
query.slap
chat.slap
talk.slap
find.slap
debug.slap

file browser should be search based. sql or fql to find files instead of navigating dirs

step 1 is to move blog and all projects into sauce as slaps/scraps

also like the idea of making concurrent gofunc-esque threads with their own input queue and state

- taylor-town
  - pages (indexed)
  - assets (nonindexed)
- md editor
  - vim/leap movement
  - minimap
  - image preview
  - linters (like hemingway)
  - ai editing


TYPES

  i8, i16, i32, u8, u16, u32, f16, f32
  int, float, str
  'x box, 'x list, 'x slice, 'v 'k dict, 'v 'k dice, ['b 'a], [.. 'b 'a], {'k 'v}, {.. 'k 'v}

examples
  'a 1 def a 1 eq assert
  2 'b def b 2 eq assert
  3 dup eq assert
  4 1 drop 4 eq assert
  6 5 swap lt assert
  7 8 (1 plus) dip mul 64 eq assert
  9 (1 plus) apply 10 eq assert
  11 10 (10 eq) (dup mul) -1 if 121 eq assert
  12 11 {(11 eq) (dup mul)} -1 cond 144 eq assert
  13 'k {'k (dup mul)} -1 match 169 eq assert
  14 ((50 lt) (2 mul 1) (1 mul 0) if) loop 56 eq assert
  0 not assert
  1 1 and assert
  0 1 or assert
  (2) (2 mul) compose apply 4 eq assert
  list 0 give len 1 eq assert list eq assert
  list 0 give grab 0 eq assert list eq assert
  list 0 give 12 0 set 0 get 12 eq assert list eq assert
  stack 0 push size 1 eq assert stack eq assert
  stack 0 push pop 0 eq assert stack eq assert
  stack 0 "a" 12 0 put 0 pull "a" eq assert stack eq assert
  list 123 give box (0 get) lend 123 eq assert free
  list 123 give box ((1 plus) map) mutate 124 eq assert free
  list 123 give clone list 123 give eq assert free
  [] list eq assert
  () stack eq assert
  {} rec eq assert


123 'foo tag {'foo (1 plus)} -1 either 124 eq


[ succeed (#element)
    "<" symbol skip
    (isalphanum) chomp-while chomp-get keep
    spaces skip
    [] ('rev let ...) ploop
    spaces skip
    [ succeed []
        "/>" symbol skip
      succeed
        ">" symbol skip
        (drop children) lazy keep
        closing-tag
    ] one-of
  ("<" neq) chomp-while chomp-get
    (("" eq) (drop "expected text" problem) (#text succeed) if) pthen
]
one-of

---

- 001 why i built it
- 002 better api/patterns/idioms. write less code and build more dsls (e.g. elm encoders/decoders).
- 003 open #tag constructors? #ok #no and no panic? rethink apis? set? dict? threads? what other batteries do we need to include?
- 004 ui framework? like charm. also wysiwyg ui editor! build templates and components visually
- 005 graphics stack lang (sneeze? splat? spill?)
- 006 editor, surfer, filer, feeder, mailer, player, claude, hypocard via charmbracelet-like ui
- 007 query lang
- 008 running taylor.town from sauce os
- 009 off to scrapscript

--->
