- [ ] Every network call gives up after a bounded time, and a slap server
      listens on loopback only (audit 2026-10-05). Today tcp-recv on a silent
      peer and tcp-connect to a blackhole (10.255.255.1) block forever, and
      tcp-listen binds INADDR_ANY. Decided: loopback, hardcoded; recv and
      send time out after 30 s, connect after 10 s; tcp-accept keeps
      blocking, since a server waits for clients. Tradeoff: a long-poll
      client must retry, and a LAN server needs a setting, which enters only
      when a program needs it.
  1. Tests in `make test-slow`, since each waits out a timeout: tcp-recv on
     a silent peer gives 'no within 35 s; tcp-connect to 10.255.255.1 gives
     'no within 15 s. In make test, run_kv.py: a client at the machine's
     non-loopback address is refused.
  2. nosigpipe (every socket passes it) sets SO_RCVTIMEO and SO_SNDTIMEO to
     30 s. recv and send then fail with EAGAIN; the 'no says "timed out
     after 30 s".
  3. prim_tcp_connect: a non-blocking connect and poll(10 s) per address,
     then blocking again. On expiry: 'no "tcp-connect: timed out after 10 s".
  4. prim_tcp_listen binds INADDR_LOOPBACK. The readme's tcp section states
     both.

- [ ] You read a bound list with `i 'xs nth`, and every symbol has one type, sym
      (breaker rounds 14 and 15; simplification review). Decided: nth reads its
      name from the symbol written right before it, as at, into and edit read
      their keys. Symbol types lose their labels: no K_LVAR, no `named`, no
      widening, and no eq/neq special case. Then
      `[1 2] 'xs let 'xs 'k let [k 'ys] drop` and `k 'ys member` pass, which nth
      refuses today. Tradeoff: nth's name can no longer come from a let-bound
      symbol; no example does this.
  1. expect.slap first: `[1 2] 'xs let 1 'xs nth must 2 eq assert`, and both
     programs above. errors.slap: `'xs 1 nth` says that nth needs the list's
     name written right before it.
  2. ty_range: S_NTH reads toks[i-1] as S_AT does. prim_nth pops the name, then
     the index.
  3. K_SYM unifies like K_INT. Record literal keys and `on`'s event read the
     symbol of the token that pushed them: the K_SYM a TOK_SYM makes keeps it in
     `ty[t].sym`, and unify ignores it. Test `{'kind 'atom}`, a symbol as a
     value. Delete K_LVAR, ty_label_var, `named`, the label branch in
     ty_unify_chain, the eq/neq special case, and the LVAR branches in ty_copy
     and ty_parse.
  4. Migrate the ~130 nth sites; the checker refuses each old one. `'g i nth` →
     `i 'g nth`; `'g swap nth` → `'g nth`; `'g over nth` → `dup 'g nth`;
     `'g x 10 mod nth` → `x 10 mod 'g nth`. readme (life example) and claude.md
     (Invariants) follow.

- [ ] You do float math with f-words, and the only protocol left is copy
      (simplification review). Decided: as in OCaml, `fplus fsub fmul fdiv flt`
      take floats, and `plus sub mul div lt sort` take ints. num and ord go, and
      a type variable carries one bit: copyable or linear.
  1. errors.slap first: `1.0 2.0 plus` says "float is not int". expect.slap:
     `1.0 2.0 fplus 3.0 eq assert`, `1.0 2.0 flt assert`.
  2. TYPES as decided, with `'sort ( int list -> int list )`. The f-prims reuse
     prim_plus, prim_sub, prim_mul, prim_div and prim_lt. Prelude: fneg uses
     fsub, fabs uses flt. max, min, abs, sqr, clamp, sign, gt, ge and le become
     int-only; add an f-variant only where a program needs it.
  3. Migrate: the checker refuses each site. A scratch build counts about 185:
     raycast 101, plasma 42, zoom 15, the libraries 18, uxn 2, and euler/44 and
     euler/45 3 each.
  4. Delete P_NUM and P_ORD, their words in ty_prot_word, and the protocol names
     in ty_prot_name. readme: arithmetic, floats, and the protocol table become
     one sentence on copy. claude.md follows.
  5. This also ends a silent wrong answer: `[1.5 -1.0 fsqrt 0.5] sort` returns
     the list unsorted, since nan has no order. sort takes ints only.

- [ ] You choose between read-time and runtime literals from a measured cost
      (simplification review). Today a `[...]` or `{...}` literal is built once,
      when the program is read, so it cannot use a name bound at runtime:
      `[i i] insert` and `[ 1 mk ]` are refused. Runtime literals delete
      ty_literal, ty_lit_depth, tyb_visible, the VF_DICT copy and about 8
      messages; `{'x x 'y y}` works, so into can be replace-only and the PRE/ABS
      fields go. The cost is speed: a constant literal in a hot loop is built
      on every pass.
  1. In a scratch copy, evaluate a literal when it is reached. Build a literal
     that holds only constants once, if speed needs it.
  2. Time make status (the 600 KB feed and sort.slap), make bench-uxn and make
     test-slow against the current binary: interleave the runs and take the
     minimum, since the machine runs other loads.
  3. Write the numbers into this task, then decide.

- [ ] You lend a box to a body that leaves one value, as you give a body to
      mutate or each, and lend is an ordinary word (simplification review).
      Decided: lend's body leaves exactly one value, so lend goes in TYPES as
      `'lend ( ..s 'a box ( ..!r 'a -> ..!r 'b ) -> ..s 'a box 'b )` and its
      form goes. Then every body a primitive runs on a sealed stack leaves one
      value, and `( 'g let 5 box (g apply) lend swap free ) 'w let (1 plus) w`
      passes, as it does with mutate. Tradeoff: a body that only asserts must
      leave a value for the caller to drop. Every lend outside lend's own
      tests already leaves one.
  1. Tests first; check each fails against the old binary. expect.slap: the
     `w` program above leaves 6. errors.slap: `5 box (drop) lend free` says
     "this body must turn its inputs into one value".
  2. TYPES: add the entry above. Delete ty_lend, S_LEND and lend's place in
     ty_reserved. prim_lend stays.
  3. Migrate expect.slap:1560-1561: each body leaves its result, and its
     assertion moves after the lend. errors.slap's lend messages ("lend takes
     a box", "lend's body does not fit the box's contents", "'lend' takes more
     values") become what ty_apply says; keep each case's intent.
  4. readme (boxes) and claude.md (Fallible operations: "Bodies given to
     each, fold and mutate") name lend with the others.

- [ ] You use a socket that the runtime keeps as a plain fd, with no box
      around it (simplification review). Today a socket is a box that holds
      one int. tcp-send, tcp-recv and tcp-accept unwrap it and push a new box
      on every call (2 mallocs), and pop_socket_fd checks the box tag, the
      slot count and `fstat(S_ISSOCK)`. The checker proves all three, so the
      checks break "the runtime trusts the checker". Decided: a socket is a
      VAL_INT at runtime, and K_SOCK alone keeps it linear. Tradeoff: none
      in behavior; a checker hole that copied a socket would show as a wrong
      fd, which the ASan breaker rounds look for anyway.
  1. No behavior changes, so the net is run_kv.py and run_serve.py. Run them
     before and after.
  2. push_socket_box pushes `val_int(fd)`; pop_socket_fd pops an int. Delete
     the three checks. The TYPES comment on sockets ("the runtime keeps it in
     a box") and claude.md's Invariants follow.

- [ ] You read fewer words in C: divmod, wrap, bnot and dict-keys live in the
      prelude, and parse-http lives in examples/lib/http.slap (simplification
      review). Each is rare and off every measured hot path: divmod in life
      and zoom, wrap in 5 files, bnot in uxn-sdl, dict-keys in kv-server's
      KEYS, parse-http in fetch.slap. rot, over, filter, zip, reverse and
      str-split stay in C: zoom or the feed benchmark runs them. Tradeoffs:
      `1 0 wrap` now dies with mod's "division by zero", and `INT64_MIN -1
      wrap` dies as `INT64_MIN -1 mod` does, where it gave 0. dict-keys
      copies each value once on its way to the key.
  1. The expect.slap cases for each word pass before and after; add one where
     a word has none. errors.slap's "wrap: modulus must be non-zero" case
     takes mod's message.
  2. Prelude: `(over over div rot rot mod swap) 'divmod let`; `(-1 bxor)
     'bnot let`; `(dict-entries ('key at) each) 'dict-keys let`; wrap is mod,
     plus the modulus when the remainder is nonzero and its sign differs from
     the modulus's. Name a prelude local as dedup's `'dd-x` is named.
  3. http.slap: `parse-http` keeps its name, its record and its three 'no
     messages, built from str-find, take-n and drop-n. expect.slap's
     parse-http cases move into its self-test. suite.py runs it as a library
     combination, as json's; run_serve.py and fetch.slap's header prepend it.
  4. Delete prim_divmod, prim_wrap, prim_bnot, prim_keys, prim_parse_http,
     memfind, their TYPES entries and registrations. readme's tables and
     claude.md's Fallible operations table follow.

- [ ] You make an empty list with `[]` and an empty record with `{}`, and no
      word duplicates them (simplification review). Decided: the words `list`
      and `rec` go. `dict` stays, since a dict has no literal; `list` stays a
      type word. Tradeoff: a script changes about 330 sites.
  1. errors.slap first: `list` is an unknown word. expect.slap keeps `[] 1
     push len 1 eq assert` and `{} 1 'x into 'x at 1 eq assert`.
  2. Delete prim_list, prim_rec, their TYPES entries and registrations. The
     prelude's couple, flatten, dedup and chunks use `[]`.
  3. Migrate with a scratchpad script: the token `list` outside signatures,
     strings and comments becomes `[]`, and `rec` becomes `{}`.
  4. readme (the tour, lists, records) and claude.md follow.

- [ ] You write a record key or an event name right before its word:
      `(1 plus) 'x edit` and `(drop step) 'tick on` (simplification review).
      Today at, into, tag and let read the token right before them, but edit
      and on read a symbol written before their body: `'x (1 plus) edit`.
      Decided: the body comes first, as into's value does, and the key comes
      last. The checker's look-back past a body goes. Tradeoff: a script
      changes 113 edit and 31 on sites.
  1. Tests first; check each fails against the old binary. expect.slap:
     `{'x 1} (1 plus) 'x edit 'x at 2 eq assert`. errors.slap: `{'x 1} 'x
     (1 plus) edit` says edit needs its key right before it, as in `(1 plus)
     'x edit`.
  2. Checker: S_EDIT reads toks[i-1] as S_AT does; delete the TOK_RPAREN
     look-back, and the key sits above the body in edit's stack type. S_ON
     pops the event, then the handler.
  3. Runtime: prim_edit pops the key, then the body; prim_on likewise.
  4. Migrate with a scratchpad script that moves the key past the body, in
     examples, tests, suite.py's FILL_RECT program, readme and claude.md.

- [ ] You tag a value with any payload, and `'ok`/`'no` are ordinary tags: a
      tagged type lists each tag with its payload, as a record type lists each
      key with its value (simplification review). Today every tag but
      'ok/'no has one payload type in the whole program (ty_tag_payload). That
      types recursive data without declarations: json's 'arr holds json
      values. A result needs a payload type per use, so results get their own
      kind, K_RES, and case refuses a result clause beside another tag.
      Decided: the payload lives in the row, as in OCaml's polymorphic
      variants. A type may be cyclic only through a tag payload, as OCaml
      allows. `{'ok int 'no str} either` is a closed tag set. Gains:
      `{'ok (…) 'no (…) 'retry (…)} case` works, and a program's 'int no
      longer collides with json.slap's. Tradeoff: the checker gains cyclic
      types, and the runtime trusts it. Do this after the runtime-literals
      decision: if into becomes replace-only, PRE/ABS go, and records and tag
      sets have one row shape. The signature task follows this one; a
      signature names a recursive type there.
  1. Tests first; check each fails against the old binary. expect.slap: the
     three-clause case above; `5 'n tag` and `"x" 'n tag` in two words that
     never meet. errors.slap: a mismatch on a recursive
     type prints it with `as`. errors.slap cases that pin a payload conflict
     ("conflicts with its payload elsewhere", "unlike its payload elsewhere")
     and "a result is tagged only 'ok or 'no" become expect.slap passes.
  2. Tag sets reuse K_REXT, K_RNIL and K_RVAR, with the payload as the field.
     Delete K_TEXT, K_TNIL, K_TVAR, K_RES, ty_tagpay, ty_tag_payload and
     ty_tag_take. `'t tag` makes `{'t p | 'r}`. In ty_case, a clause's
     payload is its tag's field in the value's row, and '_ gets the value
     with its whole row. TYPES' results (`must`, `pthen`, `get`, the I/O
     words) and nth's output become closed `{'ok 'a 'no 'b} either`.
     ty_copy_parts: a tag set is copyable when each payload is.
  3. Cycles: the occurs check reports an occurrence only on a path that
     crosses no tag payload, and still lowers levels everywhere.
     ty_unify_chain remembers each K_TAG pair it is inside and treats a pair
     met again as unified. ty_copy and ty_subst_at record a term's copy in
     ty_to before they recurse into its parts. ty_show prints a term met again
     on one path as `'j`, and its first visit as `(… 'j as)`.
  4. json.slap's and xml.slap's self-tests and tests/scale.slap cover
     recursive data. Then run breaker and fuzz rounds on the ASan build: a
     checker hole shows up as memory damage, not as a message.
  5. readme: tagged unions and the type table lose "one payload type in the
     whole program" and the result row; recursive data gets one example. claude.md's Checker paragraph (kinds, ty_tag_payload, K_RES)
     follows.

- [ ] You write a signature as ordinary data: two lists of type values, ins
      then outs, as in `(2 mul) [int] [int] effect 'double let` and `'triple
      [int] [int] effect` (simplification review). Prelude words build types
      as tagged values, so `[int str] 'sig let sig len print` prints 2.
      Decided:
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
      cannot be written. Tradeoffs: this reopens "the slot modes stay"
      (2026-10-04); signatures and forward declarations stay. Signatures grow:
      `'each ['s rest 'a var 'list tag ['r sealed 'a var] ['r sealed 'b var]
      fn] ['s rest 'b var 'list tag]`, and each result in TYPES is
      `dict "ok" 'a var insert "no" 'b var insert either`. Programs bind str
      3 times and rest
      once as local names; those sites get new names. Do this after the
      tag-payload task (under one payload per tag, json's 'int holds an int
      and the type 'int holds `()`), the f-words task (copy is the only
      protocol) and the list/rec task (rec becomes the type word).
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

- [ ] A tight stack loop runs at the cost of its cheapest primitives: `rot`,
      `if`, `let` and `dip` each cost at most twice `swap` (zoom.slap
      microbenchmarks, 2026-10-05). Measured per use, minimum of 3 runs of
      2,000,000 passes of a `while` loop: push, drop, swap, over, int plus and
      float mul 3-4 ns; a name lookup 8-10 ns; `rot` 17 ns; `if` with branches
      written in place 20 ns; `let` 15-20 ns per binding; `dip` 40 ns; an empty
      `while` pass with `body apply` 50 ns. zoom.slap's escape loop spends
      about 240 ns on the 30 tokens of one iteration, about 60 ns of it in
      `rot`. Its per-pixel pass (reuse, guess, dither, `pixel`) spends about
      150 ms on a 640x480 frame, mostly in `let` and `if`. Tradeoff: each fast
      path is more C to keep correct.
  1. Benchmark first. Each line below times one construct; subtract the empty
     loop. Run it before and after each change, interleaved, and take the
     minimum, since the machine runs other loads:
     `2000000 'N let ('label let 'body let millis 't0 let 0 (dup N lt) (body apply 1 plus) while drop millis t0 sub 'dt let label print dt print) 'bench let`
     then for example `() "empty" bench`, `(1.0 2.0 swap drop drop) "swap" bench`,
     `(1.0 2.0 3.0 rot rot drop drop drop) "rot rot" bench`,
     `(1 (2) (3) if drop) "if" bench`, `(5 'v let) "let" bench`,
     `(1 2 (drop) dip drop) "dip" bench`.
  2. prim_rot: copy prim_swap's fast path. When the top three values are one
     slot each (tag <= VAL_XT), rotate them in place, with no val_start and no
     swap_blocks. This is the lowest-hanging fruit.
  3. `if`, then `let`, then `dip`: profile each with `--profile` and perf on
     pc, and remove the work that does not depend on the data. Candidates: the
     branch dispatch of an in-place `if`; the pool frame that the first `let`
     of a body takes; the POP_BODY and POP_VAL copies of `dip`. `(body) dip`
     written in place can run from the body itself, as `if` and `while` do.
  4. Keep a change only if make status's feed time, make bench-uxn and the
     zoom.slap frame time (`cat examples/zoom.slap` plus a loop of
     `k (pixel) frame` under millis, in ./slap-sdl) do not get slower.

- [ ] You draw with a color outside 0-3 and the program dies, naming the
      color and the word (zoom.slap review). Today `clear`, `pixel` and
      `fill-rect` keep `color & 3`, so a bug that makes color 4 draws black
      with no message. Decided: die, as every runtime failure does. A pixel
      off the canvas stays clipped: drawing past an edge is normal.
      Tradeoff: a program that relied on the mask now dies.
  1. errors.slap cannot run SDL words, so the test goes in tests/suite.py
     beside the fill-rect check: `0 'tick (drop 0 0 4 pixel) on (drop) show`
     under `./slap-sdl --headless` dies with "pixel: color 4 is not 0-3";
     likewise `4 clear` and `0 0 1 1 -1 fill-rect`.
  2. prim_clear, prim_pixel and prim_fill_rect: die on a color outside 0-3
     before they draw.
  3. Run every SDL example and pair (`grep -l "pixel\|fill-rect\|clear"
     examples/*.slap`) headless for a few frames; fix any that passed a
     color past 3. readme's SDL table says the range.

- [ ] You read a checker message that names the real cause, in the right
      direction, with one name per variable (code review; silent-failure audit;
      breaker rounds 14 and 15).
  1. errors.slap first: pin one message per step below, and check each fails
     against the old binary.
  2. Mismatches read backwards where the actual type is passed first: nth with a
     str index says "int is not a list". Call `ty_unify(expected, actual)` at
     nth, as `ty_apply` and `ty_case` do.
  3. `'apply' takes ( ..a -> ..b ) / but the stack has ... / <why>`: the "takes"
     line shows a fresh copy of the word's type, so the why line uses other
     names. In ty_apply, print the instance and the stack top before `ty_unify`,
     with `ty_print_count` reset once. It is the hottest path: print only when a
     cheap pre-check fails, or keep a copy of the instance and print it after.
     ty_define and the case messages already do this.
  4. Inside a `[...]` literal, `1 [drop]` says "'drop' takes 'a copyable / but
     the stack has nothing": say that a literal's code starts from an empty
     stack.
  5. "one path leaves N more values ... a branch, clause, loop pass or recursive
     call" appears where there is no branch. A let-bound body used at two depths
     through a word, dip or if gets no let hint and can read "'g' takes int /
     but the stack has int / the stack is shorter": ty_apply adds the hint only
     when the body is a direct input of the failing word, as in
     `[(1 plus)] first 'f let (f apply) 'g let 1 g 2 3 g`. Tag-set variables
     print as an unnamed `tagged ..`, so "declares X, but its body is X" can
     show two equal types: name them like row variables. "program too long"
     prints the whole 160 KB source line, and its location is the prelude's
     last line: say "the program has N tokens; the limit is M (TOK_MAX minus
     K for the prelude and builtins)" at <stdin>. `{'a int | 'r | 's}`
     silently drops 'r: refuse a second `|`.
  6. An unknown type word in a signature, such as `strng`, reports "at line 1"
     in the text, with the caret at column 1. Pass the annotation's token to
     the error, as `ty_err` does for words; drop the "at line %d" text.
     errors.slap: EXPECT-COL on an unknown type word.
  7. Stale: the comment above `binding_release` names the old checker's box
     bindings; claude.md's Frames paragraph gives "about a tenth of the run
     time". errors.slap: merge "a program redefines ok", "a program redefines
     no" and "a word redefined after a word that calls it" into the "already
     defined" block.
  8. An int list prints as `str`: `('f let (f apply) each) 'm let 5 [1 2]
     (plus) m` says "the stack has str". A string is an int list with no
     flag. Print `str` only for a type made by a string literal or a TYPES
     entry, with a flag on its K_LIST that ty_unify ignores. Printing `int
     list` everywhere is wrong: it rewrites 20+ pinned messages such as
     "'read' takes str".

- [ ] The JSON and XML libraries refuse what they cannot read or write, and say
      where (silent-failure audit; breaker round 15). Accepted today: leading
      zeros (`"0123"` gives 123), duplicate object keys (first wins), duplicate
      attributes (first wins), an `encoding="ISO-8859-1"` declaration (its bytes
      read as UTF-8), and a float past the range (`"2e308"` gives `inf ok`).
      `"0.3"` does not equal `0.3`: the fraction is built as digit x 0.1^n. JSON
      refuses nesting at 257 and XML accepts 257. rss-to-xml writes RSS for any
      'kind but "atom" ("Atom" too). `_xe-render-attrs` and json `je-obj` read
      caller-built pairs with `nth must`/`get must`: an attribute of 3 items
      renders 2 with no error. `jd-one-of`/`xd-one-of` say only "oneOf: no
      decoders matched". Parse errors ("json: expected , or ] in an array",
      "json: trailing input", "xml: expected <", ...) give neither position nor
      byte: on a 600 KB feed there is no other way to find the fault.
  1. One failing test per case in json.slap/xml.slap/rss.slap; one pinned
     message each for the parse errors.
  2. json: refuse a leading 0 before a digit, a repeated key in one object, and
     a non-finite result; build the fraction as an integer mantissa and scale it
     once.
  3. xml: refuse a repeated attribute name and any declared encoding but UTF-8
     or US-ASCII. One nesting limit, one comparison, in both libraries.
  4. rss-to-xml accepts "rss" or "atom" only. `_xe-render-attrs` and `je-obj`
     take records `{'name 'value}`.
  5. Parse errors give the byte offset and the byte found.
     `jd-one-of`/`xd-one-of` list each alternative's error: "oneOf: [0] .a:
     expected int, got string; [1] ...".

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

- [ ] You drag a slap window's edge, and the canvas takes the new size. A new
      window opens at the size of the screen. Decided: the OS sets the size,
      and no word changes it. A `'resize` handler takes `w h`. show runs it
      once before the first tick and again after each change. One canvas pixel
      is one window point. Tradeoff: a program that draws for 640x480 fills
      only the top-left corner of a large window, since pixel and fill-rect
      clip.
  1. errors.slap first: `'resize (drop) on (drop) show` says that the handler
     must take the event's w and h.
  2. Checker (`S_ON`): add 'resize to the event list and its message. Its
     handler takes two ints, as a mouse handler does (`ty_on_mouse`).
  3. Runtime: the canvas and the pixel buffer become heap blocks sized by
     `canvas_w` and `canvas_h`. sdl_init opens the window at
     SDL_GetDisplayUsableBounds. On SDL_WINDOWEVENT_SIZE_CHANGED, reallocate
     the canvas (cleared to 0), the buffer and the texture, then run the
     'resize handlers. Delete CANVAS_W, CANVAS_H and SDL_RenderSetLogicalSize.
  4. Headless runs 'resize once with 640 480 before tick 0. Test: suite.py
     builds slap-sdl and runs headless a program whose 'resize handler prints
     w and h and whose 'tick handler ends the run with fail. It expects
     `640 480` and the fail text.
  5. wasm: shell.html sizes the canvas to the viewport, not 640x480. Then
     sdl_init asks for SDL_WINDOW_RESIZABLE on wasm too, and the comment that
     explains why it does not goes.
  6. Audit each example that hard-codes the size
     (`grep -ln '640\|480' examples/*.slap`): it reads the 'resize size or
     keeps its 640x480 region. The readme's SDL section and slap-wasm
     paragraph follow.

- [ ] You save a slap value to a file as CBOR and read it back unchanged (the
      lofi apps store everything in CBOR). Decided: examples/lib/cbor.slap
      gives encoders (`ce-*`) and decoders (`cd-*`) in the shape of
      json.slap's `je-*` and `jd-*`, over byte lists. The encoder writes the
      shortest head for each int and length; floats are always float64. The
      decoder refuses what the apps never write: tags, indefinite lengths,
      simple values other than false, true and null, a repeated map key, and
      trailing bytes. Each refusal is a 'no with the byte offset and the byte
      found.
  1. Tests first: RFC 8949 Appendix A lists values beside their hex. Each row
     in scope becomes two assertions: decode the hex, and encode the value.
  2. Add `float-bits ( float -> int )` and `bits-float ( int -> float )` to
     TYPES, a memcpy each: no word gives a float's IEEE bits. The decoder reads
     float16, float32 and float64.
  3. An int past the int64 range is a 'no, not a wrapped value.
  4. suite.py runs cbor.slap's self-test, as it runs the other libraries. The
     readme gets a section beside json.

- [ ] You type slap at a prompt in the terminal and see the stack after each
      line ("a nice slap shell"). Decided: there are two shells, one in the
      terminal and one in sauce (shell, below). Both run one loop in slap.c,
      so the slap-sdl and wasm builds of sauce get it free; slap.swift needs
      its own copy. The terminal shell comes first, since sauce does not
      exist yet.
  1. `./slap` with a TTY on stdin reads lines. The checker's stack type and
     the global frame persist across lines. An error discards only its line.

## Blocked on you

- [ ] A closure stored in an outer frame's binding is freed when nothing reaches
      it (code review). A cycle is never freed: a closure made by a nested body
      and stored in an outer frame's binding keeps that frame, which keeps the
      closure's frame.
      `( 'k let ( k apply 1 plus) ) 'wr let 0 100000 (drop (0 plus) 3 (wr) repeat 'c let 0) repeat drop`
      reaches 201 MB: `(0 plus)` closes over the outer body's frame, and the
      chain ends in that frame's binding `c`. With `(0 plus)` bound at the top
      level it stays at 4.5 MB. Decide: weak parent links, or a collector.
  1. Tests first: a loop that makes such a cycle each pass holds steady memory.

- [ ] You apply a body while a copy of it is on the stack (breaker round 15 b1).
      `(5) dup apply print drop` is refused: `dup` gives both copies one type,
      and applying one changes the depth below the other. This caused nearly all
      of the 145 refusals among 66,000 generated well-typed programs. Body cat
      is gone, so only dup remains. Decide: keep the refusal (a let-bound body
      runs at one depth too), or give each copy of a body value its own stack
      rest when nothing else holds it, which brings back the fresh-rest
      machinery (ty_bound_rest, ty_gens_settle) that let-bound bodies lost.
  1. Tests first for the choice: the program above.

- [ ] The example apps report every I/O failure and keep the data they do not
      change (silent-failure audit, critical; breaker round 15). `read` gives
      `path no` for every failure, so a directory, a mode-000 file or an I/O
      error reads as "no file": todo.slap lists nothing and exits 0, and with a
      write-only file `add` replaces the items. kv-server's boot `save-snapshot`
      then writes the empty store back. todo.slap rewrites only text/done and
      "items", dropping other fields and keys. kv-server: a recv error reads as
      EOF; send errors vanish; SAVE always says "snapshot not writable"; a torn
      last snapshot line loads as a short value; a client that connects and
      sends nothing blocks every other client. feed.slap cuts text by bytes,
      which can split a UTF-8 sequence. Decide: `read` reports why (errno text
      in the 'no payload, e.g. "path: not found"), or programs check existence
      with `ls` of the parent. Recommend the errno text: one change in
      `prim_read`, and every caller can tell. The network-timeout task above
      bounds a silent kv-server client to 30 s.
  1. tests/run_todo.py and run_kv first: a directory, a mode-000 file and a
     write-only file each exit nonzero naming the path; the file keeps its
     bytes.
  2. todo.slap:47 and kv-server.slap:53: start empty only on "not found";
     anything else dies with the path and the reason. A path with a NUL byte
     gives the reason "contains a NUL byte" (read, write and ls give 'no for it
     today, with the path as payload).
  3. todo.slap keeps the decoded JSON and changes only those fields. kv-server
     reports each error with its reason and refuses a line without TAB and
     newline. feed.slap cuts at a character boundary.

- [ ] A decoder tells a missing optional field from a present but malformed one
      (silent-failure audit, critical). `jd-maybe`/`xd-maybe` turn every failure
      into `none ok`: `{"a":"x"}` with `"a" jd-int jd-field jd-maybe` gives
      none. rss's `_rss-opt-text` (`xd-child xd-maybe ("" default)`) makes
      `<title>A <b>bold</b> post</title>` and Atom `<content type="xhtml">` give
      "", and lets an RSS channel without its required title, link or
      description parse; rss-to-xml then writes `<pubDate></pubDate>` and
      `<link href=""/>`. Decide: jd-maybe/xd-maybe give `none` only for "missing
      field"/"no such child" and pass every other 'no on (Elm's `maybe` swallows
      all errors; `optionalField` does not); rss keeps absent optional fields as
      `none`, not "". Text with element children: take the text of xhtml
      content, or refuse it.
  1. Tests first: the three inputs above, and a channel without a title.
  2. jd-field/xd-child failures carry a distinct 'missing payload shape (or
     message prefix) that the maybe decoders test.
  3. rss.slap: required fields use xd-child without maybe; rss-to-xml skips
     absent fields.
  4. `_rss-link` and `_atom-link-href` use xd-maybe too: a `<link>` with element
     children, or a malformed href, reads as no link.
  5. Links are text, never checked as URLs: three breaker rounds each found one
     (an empty href, a space-only href, then `href="&#160;"`, which gives
     `[194 160] ok` since XML whitespace is ASCII only). Decide: rss-parse
     refuses a link that is not an absolute http(s) URL, or keeps the text as
     written.

- [ ] You run sauce: your own collection of lofi slap apps, fullscreen in a
      row you slide through, with home leftmost. Decided: fullscreen apps
      only, starting from home, a launcher like iOS's. Slide apps left and
      right; home is always leftmost. Later, apps take partial width (full
      height) and slide around; this works on mobile and desktop. No vigil:
      nothing kept in intermediate state outside physical notes and a single
      working copy; publish sequels, not incremental improvements. Write the
      apps in slap. slap-sdl hosts them first; wasm and an interpreter in
      Swift (slap.swift) that loads the ROMs come later. The notes name the
      first release "slap 0". The apps store everything in CBOR (task above)
      and share a charmbracelet-like UI. A dir app opens a general thing. A
      file app opens one specific thing, and it is always a viewer and an
      editor. Decide: the first app; what a file app opens with no subject
      (the last file, or an empty one); whether a file app saves by tags only,
      with no file name; and whether you publish your own ware registry.
      - Dir apps:
        - home: the launcher. A grid of app icons (or text) and widgets, and
          search.
        - stuff: a table of files with an action column (e.g. edit image). No
          file names: tags and thumbnails. It holds libraries too (books,
          albums).
        - shell: the slap REPL in sauce. It runs the terminal shell's loop
          (task above).
      - File apps:
        - ware: package registries. You add recommended registries or custom
          ones. Each registry holds apps. Each app has an author, a changelog
          and updates, a description, screenshots, etc.
        - config: tk.
        - web: tk. An html/css browser.
        - book, feed, code, slides, prose, sheet, email, cal, phone, chat,
          video, music, photo, camera, print, clock, map, steno, cast.
      - Earlier notes name apps with no match above: query, debug, talk,
        claude, hypercard.
  1. Move the blog and all projects into sauce as slaps/scraps. taylor.town
     serves its pages (indexed) and assets (not indexed) from sauce.
  2. home.slap lists the apps and runs one.
  3. prose.slap: a markdown editor with vim/leap movement, a minimap, image
     preview, linters (like hemingway) and AI editing.
  4. stuff.slap: SQL or FQL finds files by tag, instead of navigating
     directories.

- [ ] The language has the batteries the apps need. The notes sketched these
      types: `i8, i16, i32, u8, u16, u32, f16, f32`; `int, float, str`;
      `'x box, 'x list, 'x slice, 'v 'k dict, 'v 'k dice, ['b 'a], [.. 'b 'a], {'k 'v}, {.. 'k 'v}`.
      Other candidates: SIMD and GPU acceleration; sets; concurrent
      go-func-esque threads, each with its own input queue and state; a
      charm-like UI framework with a WYSIWYG editor that builds templates and
      components visually; a graphics stack language (sneeze? splat? spill?);
      a query language. Decide: which ones the first
      app needs.
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
