#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#ifndef SLAP_WASM
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif
#define STACK_MAX  2097152
#define SYM_MAX   4096
#define TOK_MAX   65536
typedef enum { VAL_INT, VAL_FLOAT, VAL_SYM, VAL_XT, VAL_TUPLE, VAL_LIST, VAL_RECORD, VAL_BOX, VAL_TAGGED, VAL_DICT } ValTag;
typedef struct Frame Frame;
typedef void (*PrimFn)(Frame *env);
typedef struct Value {
    ValTag tag;
    uint32_t flags; /* VF_BINDS on a tuple header: its body makes names, so each run gets its own frame.
                       VF_DICT on a literal's header: it holds a dict, so each push copies it and a primitive that takes it frees it. */
    uint64_t loc;  // (fid<<56) | (line<<24) | col ; 0 = no location
    union {
        int64_t i; double f; uint32_t sym;
        struct { uint32_t sym; PrimFn fn; } xt;
        struct { uint32_t len; uint32_t slots; Frame *env; } compound;
        void *box;
    } as;
} Value;
#define LOC_PACK(fid, line, col) \
    (((uint64_t)(fid) << 56) | (((uint64_t)(line) & 0xFFFFFFFFu) << 24) | ((uint64_t)(col) & 0xFFFFFFu))
#define LOC_FID(loc)  ((int)((loc) >> 56))
#define LOC_LINE(loc) ((int)(((loc) >> 24) & 0xFFFFFFFFu))
#define LOC_COL(loc)  ((int)((loc) & 0xFFFFFFu))
__attribute__((noreturn)) static void die(const char *fmt, ...);
typedef struct { char *key; int klen; Value *vals; int nvals; } DictEntry;
typedef struct DictData { DictEntry *entries; int cap; int len; } DictData;
static int is_compound(ValTag tag) { return tag == VAL_TUPLE || tag == VAL_LIST || tag == VAL_RECORD || tag == VAL_TAGGED; }
static const char *valtag_name(ValTag t) {
    const char *names[] = {"int","float","symbol","xt","tuple","list","record","box","tagged","dict"};
    return (t <= VAL_DICT) ? names[t] : "?";
}
static int val_slots(Value v) {
    if (is_compound(v.tag)) {
        int s = (int)v.as.compound.slots;
        if (s < 1) die("corrupt value: compound with %d slots", s);
        return s;
    }
    return 1;
}
static char *sym_names[SYM_MAX];
static int sym_count = 0;
static uint32_t sym_intern(const char *name) {
    for (int i = 0; i < sym_count; i++)
        if (strcmp(sym_names[i], name) == 0) return (uint32_t)i;
    if (sym_count >= SYM_MAX) die("more than %d distinct names; SYM_MAX in slap.c sets the limit", SYM_MAX);
    sym_names[sym_count] = strdup(name);
    return (uint32_t)sym_count++;
}
static const char *sym_name(uint32_t id) { return sym_names[id]; }
#define SRC_MAX 4
#define FID_PRELUDE 1
#define FID_BUILTIN 2
#define FID_STDIN   3
static const char *src_files[SRC_MAX] = { "<unknown>", "<prelude>", "<builtin>", "<stdin>" };
static char        *src_text[SRC_MAX]       = { 0 };
static const char **src_lines[SRC_MAX]      = { 0 };
static int          src_line_count[SRC_MAX] = { 0 };
/* Where the running code or the token being read came from; die() reports it. */
static uint64_t current_loc = LOC_PACK(FID_STDIN, 0, 0);
static void print_stack_summary(FILE *out);
static void store_source_lines(const char *src, int fid) {
    src_text[fid] = strdup(src);
    int count = 1; for (const char *p = src_text[fid]; *p; p++) if (*p == '\n') count++;
    src_lines[fid] = malloc(count * sizeof(char *)); src_line_count[fid] = 0;
    char *p = src_text[fid];
    while (*p) { src_lines[fid][src_line_count[fid]++] = p; char *nl = strchr(p, '\n'); if (nl) { *nl = '\0'; p = nl + 1; } else break; }
}
static void print_source_line(FILE *out, int fid, int line, int col) {
    if (fid<0||fid>=SRC_MAX||!src_lines[fid]||line<1||line>src_line_count[fid]) { fprintf(out,"    (source unavailable)\n"); return; }
    fprintf(out, "    %4d| %s\n", line, src_lines[fid][line - 1]);
    if (col > 0) { fprintf(out, "          "); for (int i = 1; i < col; i++) fputc(' ', out); fprintf(out, "^^^\n"); }
}
__attribute__((noreturn))
static void die(const char *fmt, ...) {
    static int dying = 0; int fid = LOC_FID(current_loc), line = LOC_LINE(current_loc), col = LOC_COL(current_loc);
    const char *f = src_files[fid];
    va_list ap; va_start(ap, fmt);
    if (col > 0) fprintf(stderr, "\n-- ERROR %s:%d:%d ", f, line, col);
    else fprintf(stderr, "\n-- ERROR %s:%d ", f, line);
    int hl = 10+(int)strlen(f)+10; for(int i=hl;i<60;i++) fputc('-',stderr);
    fprintf(stderr, "\n\n    "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n\n");
    print_source_line(stderr, fid, line, col); va_end(ap);
    if (!dying) { dying = 1; print_stack_summary(stderr); fprintf(stderr, "\n"); }
    exit(1);
}
static Value stack[STACK_MAX];
static int sp = 0;
static char **cli_args=NULL; static int cli_argc=0;
static int headless_mode=0;
static void spush(Value v) { if (sp >= STACK_MAX) die("stack overflow: all %d value slots are in use. Recursion that leaves values behind, or one value this large, fills it.", STACK_MAX); stack[sp++] = v; }
/* Room for n more slots on the stack, or die naming who needed them. */
static void stack_room(int n, const char *who) {
    if (sp + n > STACK_MAX) die("%s: stack overflow: this needs %d more value slots, but only %d of %d are free. One value this large, or recursion that leaves values behind, fills the stack; read a large input in parts.", who, n, STACK_MAX - sp, STACK_MAX);
}
static Value spop(void) { if (sp <= 0) die("stack underflow: this word needs a value, but the stack is empty"); return stack[--sp]; }
static Value speek(void) { if (sp <= 0) die("stack underflow: this word needs a value, but the stack is empty"); return stack[sp - 1]; }
#define VCPY(d,s,n) memcpy(d,s,(n)*sizeof(Value))
#define SPUSH(src,n) do{ if(sp+(int)(n)>STACK_MAX) die("stack overflow: %d of %d value slots are in use and this pushes %d more.\n" \
    "  The operand stack is a fixed array. A list being built one element at a time sits on it,\n" \
    "  so a single value near this size is enough to fill it.", sp, STACK_MAX, (int)(n)); \
    VCPY(&stack[sp],src,n);sp+=(n);}while(0)
#define MKVAL(t) Value v={0};v.tag=t
#define VF_BINDS 1u
#define VF_DICT 2u
static Value val_int(int64_t i){MKVAL(VAL_INT);v.as.i=i;return v;}
static Value val_float(double f){MKVAL(VAL_FLOAT);v.as.f=f;return v;}
static Value val_sym(uint32_t s){MKVAL(VAL_SYM);v.as.sym=s;return v;}
static Value val_xt(uint32_t s,PrimFn fn){MKVAL(VAL_XT);v.as.xt.sym=s;v.as.xt.fn=fn;return v;}
static Value val_compound(ValTag tag,uint32_t len,uint32_t slots){MKVAL(tag);v.as.compound.len=len;v.as.compound.slots=slots;v.as.compound.env=NULL;return v;}
typedef enum {
    TOK_INT, TOK_FLOAT, TOK_SYM, TOK_WORD, TOK_STRING,
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACKET, TOK_RBRACKET, TOK_LBRACE, TOK_RBRACE
} TokTag;
typedef struct {
    TokTag tag;
    union { int64_t i; double f; uint32_t sym; struct { int *codes; int len; } str; } as;
    int line;
    int col;
    int fid;
    int span; /* offset to the matching bracket; 0 for other tokens */
} Token;
static Token tokens[TOK_MAX];
static int tok_count = 0;
#define LEX_ADVANCE() do { if (*p == '\n') { line++; col = 1; } else { col++; } p++; } while (0)
static void lex(const char *src, int fid) {
    tok_count = 0; int line = 1; int col = 1; const char *p = src;
    static int open_at[TOK_MAX]; int depth = 0;
    while (*p) {
        if (*p == '\n') { line++; col = 1; p++; continue; }
        if (isspace((unsigned char)*p)) { col++; p++; continue; }
        if (p[0] == '-' && p[1] == '-') { while (*p && *p != '\n') { col++; p++; } continue; }
        if (tok_count >= TOK_MAX) die("program too long: more than %d tokens; TOK_MAX in slap.c sets the limit", TOK_MAX);
        Token *t = &tokens[tok_count];
        t->line = line; t->col = col; t->fid = fid; current_loc = LOC_PACK(fid, line, col);
        t->span = 0;
        { static const char brackets[]="()[]{}"; static const TokTag btags[]={TOK_LPAREN,TOK_RPAREN,TOK_LBRACKET,TOK_RBRACKET,TOK_LBRACE,TOK_RBRACE};
          const char *bp=strchr(brackets,*p);
          if(bp){ int k=(int)(bp-brackets); t->tag=btags[k];
            if(k%2==0) open_at[depth++]=tok_count;
            else {
                if(!depth) die("line %d: `%c` closes nothing", line, *p);
                int o=open_at[--depth];
                if(tokens[o].tag!=btags[k-1]) die("line %d: `%c` closes the `%c` opened at line %d", line, *p, brackets[(tokens[o].tag-TOK_LPAREN)], tokens[o].line);
                tokens[o].span=tok_count-o; t->span=o-tok_count;
            }
            LEX_ADVANCE();tok_count++;continue;} }
        if (*p == '"') {
            LEX_ADVANCE();
            int *codes = NULL; int len = 0, cap = 0;
            while (*p != '"') {
                if (!*p) die("string literal opened at line %d never closes: add the missing \"", t->line);
                int ch = (unsigned char)*p;
                if (*p == '\\') { LEX_ADVANCE();
                    switch (*p) { case 'n': ch='\n'; break; case 't': ch='\t'; break; case '\\': ch='\\'; break; case '"': ch='"'; break; case '0': ch=0; break;
                    case 0: die("string literal opened at line %d ends in a lone backslash", t->line);
                    default: die("string literal at line %d: unknown escape \\%c; the escapes are \\n \\t \\\\ \\\" \\0", t->line, *p); } }
                LEX_ADVANCE();
                if (len >= cap) { cap = cap ? cap*2 : 16; codes = realloc(codes, cap * sizeof(int)); }
                codes[len++] = ch;
            }
            LEX_ADVANCE();
            t->tag = TOK_STRING; t->as.str.codes = codes; t->as.str.len = len; tok_count++; continue;
        }
        if (*p == '\'') {
            LEX_ADVANCE();
            const char *start = p;
            while (*p && !isspace((unsigned char)*p) && *p!='(' && *p!=')' && *p!='[' && *p!=']' && *p!='{' && *p!='}') LEX_ADVANCE();
            int len = (int)(p - start);
            if (len == 0) die("empty symbol literal");
            char buf[256]; if (len >= (int)sizeof(buf)) die("a symbol is %d characters long; the limit is %d", len, (int)sizeof(buf)-1);
            memcpy(buf, start, len); buf[len] = 0;
            t->tag = TOK_SYM; t->as.sym = sym_intern(buf); tok_count++; continue;
        }
        if (isdigit((unsigned char)*p) || (*p == '-' && isdigit((unsigned char)p[1]))) {
            const char *start = p; if (*p == '-') LEX_ADVANCE();
            while (isdigit((unsigned char)*p)) LEX_ADVANCE();
            if (*p == '.' && isdigit((unsigned char)p[1])) {
                LEX_ADVANCE();
                while (isdigit((unsigned char)*p)) LEX_ADVANCE();
                t->tag = TOK_FLOAT; t->as.f = strtod(start, NULL);
            } else { errno = 0; t->tag = TOK_INT; t->as.i = strtoll(start, NULL, 10);
                if (errno == ERANGE) die("integer literal at line %d does not fit in 64 bits", t->line); }
            tok_count++; continue;
        }
        { const char *start = p;
          while (*p && !isspace((unsigned char)*p) && *p!='(' && *p!=')' && *p!='[' && *p!=']' && *p!='{' && *p!='}') LEX_ADVANCE();
          int len = (int)(p - start); char buf[256];
          if (len >= (int)sizeof(buf)) die("a word is %d characters long; the limit is %d", len, (int)sizeof(buf)-1);
          memcpy(buf, start, len); buf[len] = 0;
          if (strcmp(buf, "true") == 0) { t->tag = TOK_INT; t->as.i = 1; tok_count++; continue; }
          if (strcmp(buf, "false") == 0) { t->tag = TOK_INT; t->as.i = 0; tok_count++; continue; }
          t->tag = TOK_WORD; t->as.sym = sym_intern(buf); tok_count++;
        }
    }
    if (depth) die("line %d: `%c` is never closed", tokens[open_at[depth-1]].line, "([{"[(tokens[open_at[depth-1]].tag-TOK_LPAREN)/2]);
}
static inline Value with_tok(Value v, const Token *t) {
    v.loc = LOC_PACK(t->fid, t->line, t->col);
    return v;
}
/* Each binding owns a heap block of its values. A trimmed binding keeps its
   block for the next binding made at that index. `pinned` counts the calls
   executing from the block; a rebind never writes into a pinned block. */
/* word: bound from a body written right before `'name let`, so a lookup runs it; any other binding is a value. */
/* heap: the values hold a dict, which the binding owns: a lookup copies it, and the binding frees it. */
/* tuples: the values hold a tuple, whose frame a lookup's copy must count again. */
typedef struct Binding { uint32_t sym; int slots, cap, pinned, word, heap, tuples; Value *vals; } Binding;
/* Names are lexical: each run of a body that makes names gets its own frame, whose parent is the frame
   the body was made in. refs counts what keeps a frame: the run, the tuples that close over it (not
   the ones in its own bindings, which would keep it forever) and its child frames. At zero it frees
   its bindings and goes back to the pool. The global frame is never freed. */
struct Frame {
    struct Frame *parent; int bind_count, bind_cap;
    int refs;
    Binding *bindings;
    int32_t *hash; uint32_t hash_mask; /* binding index+1 by symbol; 0 is empty */
};
static Frame *global_frame, *frame_pool;
/* Symbols some frame other than the global one has bound: a lookup of any other symbol goes straight
   to the global frame, since no frame on the way can hold it. */
static uint8_t sym_local[SYM_MAX];
static Frame *frame_new(Frame *parent) {
    Frame *f = calloc(1, sizeof(Frame));
    if (!f) die("out of memory: cannot allocate a call frame");
    f->parent = parent; return f;
}
static void frame_ref(Frame *f) { if (f && f != global_frame) f->refs++; }
/* A frame of up to 16 names is scanned; a bigger one (the global frame) is hashed. */
static inline __attribute__((always_inline)) int frame_find(Frame *f, uint32_t sym) {
    if (!f->hash) { for (int i = f->bind_count-1; i >= 0; i--) if (f->bindings[i].sym == sym) return i; return -1; }
    for (uint32_t s = sym & f->hash_mask;; s = (s+1) & f->hash_mask) {
        int32_t e = f->hash[s]; if (!e) return -1;
        if (f->bindings[e-1].sym == sym) return e-1;
    }
}
static void hash_put(Frame *f, int bi) {
    uint32_t s = f->bindings[bi].sym & f->hash_mask;
    while (f->hash[s]) s = (s+1) & f->hash_mask;
    f->hash[s] = bi+1;
}
static void frame_grow(Frame *f) {
    int cap = f->bind_cap ? f->bind_cap*2 : 8;
    Binding *b = realloc(f->bindings, (size_t)cap*sizeof(Binding));
    if (!b) die("out of memory: cannot grow a frame to %d bindings", cap);
    memset(&b[f->bind_cap], 0, (size_t)(cap-f->bind_cap)*sizeof(Binding));
    f->bindings = b; f->bind_cap = cap;
    if (cap <= 16) return;
    int32_t *h = calloc((size_t)cap*4, sizeof(int32_t));
    if (!h) die("out of memory: cannot grow a frame to %d bindings", cap);
    free(f->hash); f->hash = h; f->hash_mask = (uint32_t)cap*4-1;
    for (int i = 0; i < f->bind_count; i++) hash_put(f, i);
}
static void deep_free_owned(Value *vals, int slots);
/* Most runs hold only ints and symbols: the scan stays inline until it meets a value that owns something. */
static inline __attribute__((always_inline)) void deep_free_values(Value *vals, int slots) {
    for(int i=0;i<slots;i++) if(vals[i].tag==VAL_TUPLE||vals[i].tag==VAL_BOX||vals[i].tag==VAL_DICT){ deep_free_owned(vals+i,slots-i); return; }
}
static void frame_drop(Frame *f);
/* A binding's values leave: dicts it owns are freed and each tuple's frame loses a reference, except
   frame f's own tuples, which a binding in f holds weakly. Boxes are left alone: a Box binding (old
   checker) hands its cell to one lookup. */
static inline __attribute__((always_inline)) void binding_release(Frame *f, Binding *b) {
    if (!b->tuples && !b->heap) { b->slots = 0; return; }
    for (int i = 0; i < b->slots; i++) {
        Value *v = &b->vals[i];
        if (v->tag == VAL_TUPLE && v->as.compound.env != f) frame_drop(v->as.compound.env);
        else if (v->tag == VAL_DICT && b->heap) deep_free_values(v, 1);
    }
    b->heap = 0; b->tuples = 0; b->slots = 0;
}
/* Dicts the program has made; with none, no binding can hold one and binds skip the scan. */
static int dicts_made;
static int vals_hold_dict(const Value *v, int n) { if (!dicts_made) return 0; for (int i = 0; i < n; i++) if (v[i].tag == VAL_DICT) return 1; return 0; }
/* Drop bindings [n, bind_count). Newest first, so no probe chain is cut short. */
static void frame_trim(Frame *f, int n) {
    for (int i = f->bind_count-1; i >= n; i--) {
        binding_release(f, &f->bindings[i]);
        if (!f->hash) continue;
        uint32_t s = f->bindings[i].sym & f->hash_mask;
        while (f->hash[s] != i+1) s = (s+1) & f->hash_mask;
        f->hash[s] = 0;
    }
    f->bind_count = n;
}
static inline __attribute__((always_inline)) void frame_bind(Frame *f, uint32_t sym, Value *vals, int slots, int word) {
    int bi = frame_find(f, sym);
    if (bi < 0) { if (f->bind_count == f->bind_cap) frame_grow(f); bi = f->bind_count++; f->bindings[bi].sym = sym; if (f->hash) hash_put(f, bi); }
    if (f != global_frame) sym_local[sym] = 1;
    Binding *b = &f->bindings[bi];
    /* a pinned block runs a word now; its values stay where they are */
    if (!b->pinned) binding_release(f, b);
    if (slots > b->cap || b->pinned) {
        if (b->pinned) { b->vals = NULL; b->cap = 0; }
        b->vals = realloc(b->vals, (size_t)slots*sizeof(Value));
        if (!b->vals) die("out of memory: cannot bind %d values to '%s", slots, sym_name(sym));
        b->cap = slots;
    }
    VCPY(b->vals, vals, slots); b->slots = slots; b->word = word; b->heap = 0; b->tuples = 0;
    for (int i = 0; i < slots; i++) {
        if (b->vals[i].tag == VAL_TUPLE) {
            b->tuples = 1;
            /* a closure over f that f itself holds does not keep f */
            if (b->vals[i].as.compound.env == f && f != global_frame) f->refs--;
        } else if (b->vals[i].tag == VAL_DICT) b->heap = 1;
    }
}
/* A run that made names is over, or a closure over f is gone. A frame at zero waits on `pending`, linked
   through its parent field, and one loop trims the waiting frames: a trim in place would recurse through
   binding_release once per link of a closure chain. */
/* More frames than memory holds: only a cycle in the frame graph reaches it. */
#define FRAME_CHAIN_MAX 1000000000L
static void frame_drop(Frame *f) {
    static Frame *pending; static int busy;
    for (long hops = 0; f && f != global_frame; hops++) {
        if (hops > FRAME_CHAIN_MAX) die("frame_drop: expected a chain of parent frames to reach the global frame, but it ran past %ld links.\n"
                                        "  The frames form a cycle, which is an interpreter bug. Report the program.", hops);
        if (--f->refs > 0) break;
        if (f->refs < 0) die("frame_drop: a frame lost more references than it had (internal)");
        Frame *p = f->parent; f->parent = pending; pending = f; f = p;
    }
    if (busy || !pending) return;
    busy = 1;
    for (long trimmed = 1; pending; trimmed++) {
        if (trimmed > FRAME_CHAIN_MAX) die("frame_drop: expected each frame to wait once to be freed, but %ld frames waited in one call.\n"
                                           "  A frame waited twice, which is an interpreter bug. Report the program.", trimmed);
        Frame *g = pending; pending = g->parent;
        frame_trim(g, 0);
        g->parent = frame_pool; frame_pool = g;
    }
    busy = 0;
}
static Frame *frame_acquire(Frame *parent) {
    Frame *f = frame_pool;
    if (f) frame_pool = f->parent; else f = frame_new(NULL);
    f->parent = parent; f->refs = 1; frame_ref(parent); return f;
}
/* Tuples in a pushed copy of v close over their frames once more. */
static void vals_retain(const Value *v, int n) { for (int i = 0; i < n; i++) if (v[i].tag == VAL_TUPLE) frame_ref(v[i].as.compound.env); }
typedef struct { Binding *bind; Frame *frame; } Lookup;
static inline __attribute__((always_inline)) Lookup frame_lookup(Frame *f, uint32_t sym) {
    if (!sym_local[sym]) { int i = frame_find(global_frame, sym); Lookup r = {i >= 0 ? &global_frame->bindings[i] : NULL, i >= 0 ? global_frame : NULL}; return r; }
    for (Frame *cur = f; cur; cur = cur->parent) {
        int i = frame_find(cur, sym);
        if (i >= 0) { Lookup r = {&cur->bindings[i], cur}; return r; }
    }
    Lookup r = {NULL, NULL}; return r;
}
static void eval(Token *toks, int count, Frame *env);
static void eval_body(Value *body, int slots, Frame *env);
static void eval_run(Value *body, int slots, Frame *ee);
/* Runs a tuple body: in a frame of its own when it makes names, so they are its own, and in ee
   otherwise. */
static inline void eval_in(Value *body, int slots, Frame *ee) {
    if(!(body[slots-1].flags&VF_BINDS)){ eval_run(body,slots,ee); return; }
    Frame *f=frame_acquire(ee); eval_run(body,slots,f); frame_drop(f);
}
static inline void dispatch_word(uint32_t sym, Frame *env);
/* Primitives by symbol id; the second table holds the fused `X must` variant. */
static PrimFn prim_fns[SYM_MAX], prim_must_fns[SYM_MAX];
static int64_t pop_int(void) { Value v=spop(); if(v.tag==VAL_INT) return v.as.i; die("expected int, got %s",valtag_name(v.tag)); }
static double pop_float(void) { Value v=spop(); if(v.tag==VAL_FLOAT) return v.as.f; die("expected float, got %s",valtag_name(v.tag)); }
static uint32_t pop_sym(void) { Value v=spop(); if(v.tag==VAL_SYM) return v.as.sym; die("expected symbol, got %s",valtag_name(v.tag)); }
typedef struct { int base; int slots; } ElemRef;
static ElemRef compound_elem(Value *data, int total_slots, int len, int64_t index) {
    if (index < 0 || index >= len) { ElemRef ref = { -1, 0 }; return ref; }
    if (total_slots == len + 1) { ElemRef ref = { (int)index, 1 }; return ref; }
    int elem_end = total_slots - 1;
    for (int i = len - 1; i > index; i--) elem_end -= val_slots(data[elem_end - 1]);
    int sz = val_slots(data[elem_end - 1]);
    ElemRef ref = { elem_end - sz, sz }; return ref;
}
static ElemRef record_field(Value *data, int total_slots, int len, uint32_t key, int *found) {
    int elem_end = total_slots - 1; *found = 0; ElemRef ref = {0, 0};
    for (int i = len - 1; i >= 0; i--) {
        int lp = elem_end - 1; Value last = data[lp];
        int vsize = val_slots(last);
        int val_base = elem_end - vsize, key_pos = val_base - 1;
        if (key_pos < 0) die("malformed record (len=%d, total_slots=%d)", len, total_slots);
        if (data[key_pos].tag != VAL_SYM) die("record key must be symbol, got %s", valtag_name(data[key_pos].tag));
        if (data[key_pos].as.sym == key) { ref.base = val_base; ref.slots = vsize; *found = 1; return ref; }
        elem_end = key_pos;
    }
    return ref;
}
static int eval_depth = 0;
#define EVAL_DEPTH_MAX 10000
/* 7 MiB of the 8 MiB main-thread stack, leaving room for die() to report. */
#define C_STACK_MAX (7*1024*1024)
static char *c_stack_base = NULL;
static void c_stack_check(const char *what) {
    char probe; long used = !c_stack_base ? 0 : &probe > c_stack_base ? &probe - c_stack_base : c_stack_base - &probe;
    if(used > C_STACK_MAX)
        die("C stack exhausted %s -- %ld KB used, limit %ld KB.\n"
            "  Each nested word call or nesting level keeps a C frame alive. Rewrite deep recursion\n"
            "  as a `while` loop, or flatten the data.", what, used/1024, (long)(C_STACK_MAX/1024));
}
static void val_print(Value *data, int slots, FILE *out);
/* Error reports show at most this many elements per compound; 0 means all. */
static int print_max = 0, print_depth = 0;
/* An error report shows nesting this deep and elides the rest with `...`. */
#define PRINT_DEPTH_MAX 16
/* A record prints its keys as elements, so every compound prints n elements. */
static void print_elems(Value *data, int slots, int n, char open, char close, FILE *out) {
    int *st=malloc((size_t)(n+1)*sizeof(int)); if(!st) die("print: out of memory for %d elements", n);
    st[n]=slots-1; for(int i=n-1;i>=0;i--) st[i]=st[i+1]-val_slots(data[st[i+1]-1]);
    fputc(open,out);
    int shown=print_max&&n>print_max?print_max:n;
    for(int i=0;i<shown;i++){ if(i>0) fputc(' ',out); val_print(&data[st[i]],st[i+1]-st[i],out); }
    if(shown<n) fprintf(out," ...%d more",n-shown);
    fputc(close,out); free(st);
}
static void val_print_node(Value *data, int slots, FILE *out);
static void val_print(Value *data, int slots, FILE *out) {
    if (print_max && print_depth >= PRINT_DEPTH_MAX) { fputs("...", out); return; }
    if (!print_max) c_stack_check("printing a deeply nested value"); /* the error report runs on the deep stack that caused the error */
    print_depth++; val_print_node(data, slots, out); print_depth--;
}
static void val_print_node(Value *data, int slots, FILE *out) {
    Value top = data[slots - 1];
    switch (top.tag) {
    case VAL_INT: fprintf(out, "%lld", (long long)top.as.i); break;
    case VAL_FLOAT: { /* shortest text that reads back as the same double, with a point so it never looks like an int */
        char b[40]; snprintf(b,sizeof b,"%.15g",top.as.f); if(strtod(b,NULL)!=top.as.f) snprintf(b,sizeof b,"%.17g",top.as.f);
        fputs(b,out); if(!strpbrk(b,".eni")) fputs(".0",out); break; }
    case VAL_SYM: fprintf(out, "'%s", sym_name(top.as.sym)); break;
    case VAL_XT: fprintf(out, "%s", sym_name(top.as.xt.sym)); break;
    case VAL_LIST: {
        int len=(int)top.as.compound.len, is_str=len>0&&slots==len+1;
        for(int i=0;is_str&&i<len;i++){Value v=data[i];if(v.tag!=VAL_INT||v.as.i<32||v.as.i>126)is_str=0;}
        if(is_str){fputc('"',out);for(int i=0;i<len;i++)fputc((char)data[i].as.i,out);fputc('"',out);break;}
        print_elems(data,slots,len,'[',']',out); break;
    }
    case VAL_TUPLE: print_elems(data,slots,(int)top.as.compound.len,'(',')',out); break;
    case VAL_RECORD: print_elems(data,slots,2*(int)top.as.compound.len,'{','}',out); break;
    case VAL_BOX: fprintf(out, "<box>"); break;
    case VAL_DICT: { DictData *dd=(DictData*)top.as.box; fprintf(out,"<dict:%d>",dd?dd->len:0); break; }
    case VAL_TAGGED: val_print(data,(int)top.as.compound.slots-1,out); fprintf(out," '%s tagged",sym_name(top.as.compound.len)); break;
    }
}
static void print_stack_summary(FILE *out) {
    print_max = 32; print_depth = 0;
    if (sp == 0) { fprintf(out, "\n    stack: (empty)\n"); return; }
    fprintf(out, "\n    stack (%d slot%s):\n", sp, sp==1?"":"s");
    int pos=sp, shown=0;
    while(pos>0&&shown<5){int s=val_slots(stack[pos-1]);pos-=s;fprintf(out,"      %d: ",shown);val_print(&stack[pos],s,out);fprintf(out,"\n");shown++;}
    if(pos>0){int rem=0;while(pos>0){pos-=val_slots(stack[pos-1]);rem++;}fprintf(out,"      ... %d more\n",rem);}
}
static DictEntry *dict_get(DictData *dd, const char *key, int klen);
static uint32_t S_NTH;
/* A body that looks a name up at runtime: a word that is not a primitive, or `nth`, which reads the name written before it. */
static int body_reads_names(Value *v, int slots) {
    for (int i = 0; i < slots; i++) if (v[i].tag == VAL_XT && (!prim_fns[v[i].as.xt.sym] || v[i].as.xt.sym == S_NTH)) return 1;
    return 0;
}
/* Structural equality, element by element: a record by its keys, whatever order they were written in, a
   dict by its entries, and a body by its code, and by the frame it closes over when it reads names. A
   compound's elements are read from its end, where each one's header gives its size. */
static int val_equal(Value *a, int aslots, Value *b, int bslots) {
    c_stack_check("while comparing a deeply nested value");
    Value atop = a[aslots - 1], btop = b[bslots - 1];
    if (atop.tag != btop.tag) return 0;
    switch (atop.tag) {
    case VAL_INT: return atop.as.i == btop.as.i;
    case VAL_FLOAT: return atop.as.f == btop.as.f;
    case VAL_SYM: return atop.as.sym == btop.as.sym;
    case VAL_XT: return atop.as.xt.sym == btop.as.xt.sym;
    case VAL_BOX: return atop.as.box == btop.as.box;
    case VAL_DICT: {
        DictData *da = (DictData*)atop.as.box, *db = (DictData*)btop.as.box;
        if (da->len != db->len) return 0;
        for (int i = 0; i < da->cap; i++) { DictEntry *e = &da->entries[i], *f; if (!e->key) continue;
            if (!(f = dict_get(db, e->key, e->klen)) || !val_equal(e->vals, e->nvals, f->vals, f->nvals)) return 0; }
        return 1; }
    case VAL_RECORD: {
        int len = (int)atop.as.compound.len;
        if (len != (int)btop.as.compound.len) return 0;
        for (int p = aslots - 1, k = 0; k < len; k++) {
            int vs = val_slots(a[p-1]), found; uint32_t key = a[p-1-vs].as.sym;
            ElemRef r = record_field(b, bslots, len, key, &found);
            if (!found || !val_equal(&a[p-vs], vs, &b[r.base], r.slots)) return 0;
            p -= vs + 1; }
        return 1; }
    case VAL_TUPLE:
        if (atop.as.compound.env != btop.as.compound.env && (body_reads_names(a, aslots) || body_reads_names(b, bslots))) return 0;
        /* fall through */
    case VAL_LIST: case VAL_TAGGED:
        if (atop.as.compound.len != btop.as.compound.len || aslots != bslots) return 0;
        for (int p = aslots - 1, q = bslots - 1; p > 0 && q > 0; ) {
            int as = val_slots(a[p-1]), bs = val_slots(b[q-1]);
            if (!val_equal(&a[p-as], as, &b[q-bs], bs)) return 0;
            p -= as; q -= bs; }
        return 1;
    }
    return 0;
}
static int val_less(Value *a, int aslots, Value *b, int bslots) {
    Value atop = a[aslots - 1], btop = b[bslots - 1];
    if (atop.tag != btop.tag) die("lt: type mismatch, got %s and %s", valtag_name(atop.tag), valtag_name(btop.tag));
    switch (atop.tag) {
    case VAL_INT: return atop.as.i < btop.as.i;
    case VAL_FLOAT: return atop.as.f < btop.as.f;
    default: die("lt: unsupported type %s (only int and float are ordered)", valtag_name(atop.tag)); return 0;
    }
}
static uint32_t S_LET, S_EFFECT, S_OK, S_NO, S_NONE, S_TAG, S_CASE, S_MUST, S_AT, S_EDIT, S_INTO, S_KEY, S_VALUE, S_ON, S_SHOW, S_WILD;
static void syms_init(void) {
    S_LET=sym_intern("let"); S_EFFECT=sym_intern("effect");
    S_OK=sym_intern("ok"); S_NO=sym_intern("no"); S_NONE=sym_intern("none"); S_CASE=sym_intern("case"); S_MUST=sym_intern("must");
    S_TAG=sym_intern("tag");
    S_AT=sym_intern("at"); S_EDIT=sym_intern("edit"); S_INTO=sym_intern("into"); S_KEY=sym_intern("key"); S_VALUE=sym_intern("value"); S_WILD=sym_intern("_");
    S_NTH=sym_intern("nth"); S_ON=sym_intern("on"); S_SHOW=sym_intern("show");
}
/* ---- TYPE CHECKER ---- */
/* ==== TYPES: inference by unification ====
   A type is a term in one pool: a value, a stack (its top and the rest), a record row, a label or a
   tag set. A variable is a term that union-find binds to another. A body's type is its stack effect:
   a function from the stack it takes to the stack it leaves, whose untouched rest is a variable, so a
   word works on any stack below what it touches. Levels decide what a word's type generalizes. */
enum { K_VAR, K_INT, K_FLOAT, K_SYM, K_LIST, K_DICT, K_BOX, K_SOCK, K_FN, K_REC, K_RES, K_TAG,
       K_SVAR, K_SNIL, K_SCONS, K_RVAR, K_RNIL, K_REXT, K_LSYM, K_LVAR, K_TVAR, K_TNIL, K_TEXT, K_PRE, K_ABS };
/* A symbol's type is K_SYM, whose `a` is a label variable (K_LVAR): the variable's `a` is the K_LSYM of
   the literal the symbol came from, or 0 once two different symbols met; only `{...}` keys, `nth` and
   `on` read it, and they refuse 0. The label sits in a variable so that a word's own symbol types are
   copied for each use, like its other variables, while a symbol type from outside the word is one
   fact every use shares. Record keys are K_LSYM. */
/* A row field is K_PRE (the record has the key, of type a) or K_ABS (it has not), or a variable for
   either. A closed row (K_RNIL) has no other key. `into` sets a key whether or not the record had it,
   as the runtime replaces a key it finds. */
/* What a value variable must be: protocols a word asks of its inputs. */
/* copy: the value may be copied, dropped, bound or stored. A box is not, nor a result or tag that
   holds one; the stack carries it from the word that makes it to the word that frees it. */
enum { P_NUM = 1, P_ORD = 2, P_COPY = 4 };
/* rigid: a signature's variable while a body is checked against it. sealed: the stack below a body
   that must not reach it (`each`, `edit`); instances keep it. */
/* named: a label variable whose name nth read a list by; it may not meet another symbol. */
typedef struct { uint8_t kind, prot, rigid, sealed, named; int level, a, b, c, link; uint32_t sym; } Ty;
#define GENERIC 0x3fffffff
static Ty *ty; static int ty_n = 1, ty_cap, ty_level;
/* Walks over a type visit each term once, since types share parts: ty_mark holds the walk that last
   saw a term, ty_to what a copying walk made of it. */
static int *ty_mark, *ty_to, ty_stamp;
static int ty_new(int kind, int a, int b, int c) {
    if (ty_n >= ty_cap) { int old = ty_cap; ty_cap = ty_cap ? 2*ty_cap : 65536;
        ty = realloc(ty, (size_t)ty_cap * sizeof(Ty)); ty_mark = realloc(ty_mark, (size_t)ty_cap * sizeof(int)); ty_to = realloc(ty_to, (size_t)ty_cap * sizeof(int));
        if (!ty || !ty_mark || !ty_to) die("type checker: out of memory for %d types", ty_cap);
        memset(ty + old, 0, (size_t)(ty_cap - old) * sizeof(Ty)); memset(ty_mark + old, 0, (size_t)(ty_cap - old) * sizeof(int)); }
    ty[ty_n] = (Ty){(uint8_t)kind, 0, 0, 0, 0, ty_level, a, b, c, 0, 0};
    return ty_n++;
}
static int ty_sym(int kind, uint32_t s) { int t = ty_new(kind, 0, 0, 0); ty[t].sym = s; return t; }
static int ty_isvar(int k) { return k == K_VAR || k == K_SVAR || k == K_RVAR || k == K_LVAR || k == K_TVAR; }
/* A symbol type's label variable, and the label it holds (a K_LSYM, or 0). */
static int ty_find(int t);
static int ty_label_var(int t) { return ty_find(ty[ty_find(t)].a); }
static int ty_sym_label(int t) { t = ty_find(t); return ty[t].kind == K_SYM ? ty[ty_label_var(t)].a : 0; }
/* A variable links to what it is bound to; a structure links to one it was unified with. */
static int ty_find(int t) {
    for (int hops = 0; ty[t].link; hops++) {
        if (hops == ty_n) die("type checker bug: a type links in a cycle (%d hops)", hops);
        t = ty[t].link;
    }
    return t;
}
/* The next term along a stack (b) or a row or tag set (c); a chain longer than the pool is a cycle. */
/* hops starts at -ty_n, the pool's size when the walk begins, so a walk that makes terms stays bounded. */
static int ty_rest(int x, int *hops) {
    if (++*hops > 0) die("type checker bug: a stack or row links in a cycle");
    return ty_find(ty[x].kind == K_SCONS ? ty[x].b : ty[x].c);
}
static int *ty_work, ty_work_cap;
static void ty_work_push(int *n, int t) {
    if (*n >= ty_work_cap) { ty_work_cap = ty_work_cap ? 2*ty_work_cap : 4096; ty_work = realloc(ty_work, (size_t)ty_work_cap * sizeof(int)); if (!ty_work) die("type checker: out of memory for %d terms to visit", ty_work_cap); }
    ty_work[(*n)++] = t;
}
/* Values of these kinds satisfy these protocols. */
static int ty_prot_of(int k) {
    switch (k) {
    case K_INT: case K_FLOAT: return P_NUM | P_ORD | P_COPY;
    case K_SYM: case K_FN: return P_COPY;
    case K_LIST: case K_DICT: case K_REC: return P_COPY;
    default: return 0;
    }
}
static const char *ty_prot_name(int p) { return p & P_NUM ? "num" : p & P_ORD ? "ord" : "copyable"; }

/* ---- printing, for messages ---- */
static int ty_print_names[64], ty_print_count;
/* ty_show's calls for one message: a message about a large shared type stops at a budget. */
static int ty_show_calls;
static void ty_print_var(char *out, size_t cap, int t, char prefix) {
    int k = 0; while (k < ty_print_count && ty_print_names[k] != t) k++;
    if (k == 64) { snprintf(out, cap, "%c_", prefix); return; }
    if (k == ty_print_count) ty_print_names[ty_print_count++] = t;
    if (k < 26) snprintf(out, cap, "%c%c", prefix, 'a' + k); else snprintf(out, cap, "%c%d", prefix, k);
}
static void ty_show(char *out, size_t cap, int t, int depth);
/* Append to out, cutting at its end: a message may be long, but never longer than its buffer. */
static void ty_put(char *out, size_t cap, size_t *len, const char *fmt, ...) {
    if (*len + 1 >= cap) return;
    va_list ap; va_start(ap, fmt); int n = vsnprintf(out + *len, cap - *len, fmt, ap); va_end(ap);
    *len = n < 0 || *len + (size_t)n >= cap ? cap - 1 : *len + (size_t)n;
}
static void ty_show_stack(char *out, size_t cap, int s, int depth) {
    /* bottom first, as a signature reads: the rest, then each value up to the top */
    int items[32], n = 0; s = ty_find(s);
    while (ty[s].kind == K_SCONS && n < 32) { items[n++] = ty[s].a; s = ty_find(ty[s].b); }
    size_t len = 0; out[0] = 0;
    if (ty[s].kind == K_SVAR) { char v[16]; ty_print_var(v, sizeof v, s, '.'); ty_put(out, cap, &len, ".%s", v); }
    else if (ty[s].kind == K_SCONS) ty_put(out, cap, &len, "...");
    for (int k = n - 1; k >= 0; k--) { char e[256]; ty_show(e, sizeof e, items[k], depth + 1); ty_put(out, cap, &len, "%s%s", len ? " " : "", e); }
}
/* The top n values of a stack, bottom first. */
static void ty_show_top(char *out, size_t cap, int s, int n) {
    int items[32], k = 0; s = ty_find(s); ty_show_calls = 0;
    while (ty[s].kind == K_SCONS && k < n && k < 32) { items[k++] = ty[s].a; s = ty_find(ty[s].b); }
    size_t len = 0; out[0] = 0;
    if (k < n && k < 32) ty_put(out, cap, &len, k ? "only %d value%s:" : "nothing", k, k == 1 ? "" : "s");
    for (int j = k - 1; j >= 0; j--) { char e[256]; ty_show(e, sizeof e, items[j], 1); ty_put(out, cap, &len, "%s%s", len ? " " : "", e); }
}
static void ty_show(char *out, size_t cap, int t, int depth) {
    if (depth == 0) ty_show_calls = 0;
    if (depth > 8 || ++ty_show_calls > 2000) { snprintf(out, cap, "..."); return; }
    t = ty_find(t); char a[256], b[256];
    switch (ty[t].kind) {
    case K_VAR: { ty_print_var(out, cap, t, '\''); int p = ty[t].prot & ~P_COPY ? ty[t].prot & ~P_COPY : ty[t].prot;
        if (p) { size_t l = strlen(out); snprintf(out + l, cap - l, " %s", ty_prot_name(p)); } return; }
    case K_INT: snprintf(out, cap, "int"); return;
    case K_FLOAT: snprintf(out, cap, "float"); return;
    case K_SYM: { int l = ty_sym_label(t); if (l) snprintf(out, cap, "'%s", sym_name(ty[l].sym)); else snprintf(out, cap, "sym"); return; }
    case K_LIST: { int e = ty_find(ty[t].a); if (ty[e].kind == K_INT) { snprintf(out, cap, "str"); return; } ty_show(a, sizeof a, e, depth + 1); snprintf(out, cap, "%s list", a); return; }
    case K_DICT: ty_show(a, sizeof a, ty[t].a, depth + 1); snprintf(out, cap, "%s dict", a); return;
    case K_BOX: ty_show(a, sizeof a, ty[t].a, depth + 1); snprintf(out, cap, "%s box", a); return;
    case K_SOCK: snprintf(out, cap, "socket"); return;
    case K_TVAR: snprintf(out, cap, "tagged .."); return;
    case K_RVAR: { char v[16]; ty_print_var(v, sizeof v, t, '.'); snprintf(out, cap, "{| .%s}", v); return; }
    case K_FN: ty_show_stack(a, sizeof a, ty[t].a, depth); ty_show_stack(b, sizeof b, ty[t].b, depth); snprintf(out, cap, "( %s%s->%s%s )", a, *a ? " " : "", *b ? " " : "", b); return;
    case K_RES: ty_show(a, sizeof a, ty[t].a, depth + 1); ty_show(b, sizeof b, ty[t].b, depth + 1); snprintf(out, cap, "{'ok %s 'no %s} either", a, b); return;
    case K_TAG: case K_TEXT: case K_TNIL: {
        size_t len = 0; int r = ty[t].kind == K_TAG ? ty_find(ty[t].a) : t; ty_put(out, cap, &len, "tagged");
        for (int hops = 0; ty[r].kind == K_TEXT; r = ty_find(ty[r].c), hops++) { if (hops == ty_n) die("type checker bug: a tag set links in a cycle"); ty_put(out, cap, &len, " '%s", sym_name(ty[r].sym)); }
        if (ty[r].kind == K_TVAR) ty_put(out, cap, &len, " ..");
        return; }
    case K_PRE: ty_show(out, cap, ty[t].a, depth + 1); return;
    case K_ABS: snprintf(out, cap, "absent"); return;
    case K_REC: case K_REXT: case K_RNIL: {
        size_t len = 0; int r = ty[t].kind == K_REC ? ty_find(ty[t].a) : t, n = 0; ty_put(out, cap, &len, "{");
        for (int hops = 0; ty[r].kind == K_REXT; hops++) {
            if (hops == ty_n) die("type checker bug: a row links in a cycle");
            int f = ty_find(ty[r].b);
            if (ty[f].kind != K_ABS) {
                if (ty[f].kind == K_PRE) ty_show(a, sizeof a, ty[f].a, depth + 1); else { ty_show(a, sizeof a, f, depth + 1); strncat(a, "?", sizeof a - strlen(a) - 1); }
                ty_put(out, cap, &len, "%s'%s %s", n ? " " : "", sym_name(ty[ty[r].a].sym), a); n++;
            }
            r = ty_find(ty[r].c);
        }
        if (ty[r].kind == K_RVAR) { char v[16]; ty_print_var(v, sizeof v, r, '.'); ty_put(out, cap, &len, "%s| .%s", n ? " " : "", v); }
        ty_put(out, cap, &len, "}"); return;
    }
    case K_SVAR: case K_SNIL: case K_SCONS: ty_show_stack(out, cap, t, depth); return;
    default: snprintf(out, cap, "?"); return;
    }
}

/* ---- unification ----
   ty_why says what failed, for the caller's message. The occurs check walks the term being bound and
   lowers the levels of its variables to v's. */
static char ty_why[512];
static int ty_occurs(int v, int t, int level) {
    int n = 0, stamp = ++ty_stamp; ty_work_push(&n, t);
    while (n) {
        int x = ty_find(ty_work[--n]);
        if (x == v) return 1;
        if (ty_mark[x] == stamp) continue;
        ty_mark[x] = stamp;
        if (ty_isvar(ty[x].kind)) { if (ty[x].level > level && ty[x].level != GENERIC) { if (ty[x].rigid) return 2; ty[x].level = level; } continue; }
        if (ty[x].a) ty_work_push(&n, ty[x].a);
        if (ty[x].b) ty_work_push(&n, ty[x].b);
        if (ty[x].c) ty_work_push(&n, ty[x].c);
    }
    return 0;
}
static int ty_unify(int a, int b);
static int ty_unify_at(int a, int b, int depth);
static int ty_copy_parts(int t);
static int ty_tag_payload(uint32_t tag);
static int ty_fixed(int v) { return ty[v].rigid || ty[v].sealed; }
/* A compound kind by name, for a message where the full type would read as another type (`int list`
   prints as str). */
static const char *ty_kind_noun(int k) {
    switch (k) { case K_LIST: return "a list"; case K_DICT: return "a dict"; case K_BOX: return "a box"; case K_REC: return "a record";
    case K_FN: return "a body"; case K_TAG: return "a tagged value"; case K_RES: return "a result"; default: return 0; }
}
static int ty_rigid_rest;
/* The last failed unify met two stacks of different depths. */
static int ty_depth_why;
static int ty_bind(int v, int t) {
    if (ty[v].sealed && ty[t].kind == K_SVAR && ty[t].sealed && !ty[v].rigid && !ty[t].rigid) { ty[v].link = t; return 0; }
    if (ty_fixed(v) && !(ty_isvar(ty[t].kind) && !ty_fixed(t))) {
        if (ty[v].sealed || (ty_isvar(ty[t].kind) && ty[t].sealed)) { snprintf(ty_why, sizeof ty_why, "this body must turn its inputs into one value and cannot reach the stack below them"); return 1; }
        if (ty[v].kind == K_SVAR && (ty[t].kind == K_SCONS || ty[t].kind == K_SNIL)) { snprintf(ty_why, sizeof ty_why, "the body takes or leaves a different number of values than the signature declares"); ty_rigid_rest = 1; return 1; }
        if (ty[v].kind == K_RVAR && (ty[t].kind == K_REXT || ty[t].kind == K_RNIL)) { char s[256]; ty_show(s, sizeof s, t, 0);
            snprintf(ty_why, sizeof ty_why, "the signature's open record ends in keys the caller chooses, but here the body needs them to be %s", ty[t].kind == K_RNIL ? "none" : s); return 1; }
        if (ty[v].kind == K_TVAR && (ty[t].kind == K_TEXT || ty[t].kind == K_TNIL)) { char s[256]; ty_show(s, sizeof s, t, 0);
            snprintf(ty_why, sizeof ty_why, "the signature's open tag set ends in tags the caller chooses, but here the body needs them to be %s", ty[t].kind == K_TNIL ? "none" : s); return 1; }
        char s[256]; ty_show(s, sizeof s, t, 0); snprintf(ty_why, sizeof ty_why, "a type the signature leaves open is %s here", s); return 1; }
    if (ty_fixed(v)) { int x = v; v = t; t = x; }
    int occ = ty_occurs(v, t, ty[v].level);
    if (occ == 2) { snprintf(ty_why, sizeof ty_why, "a type the signature leaves open would have to be a type from outside the body"); return 1; }
    if (occ) {
        int n = 0; for (int x = ty_find(t); ty[x].kind == K_SCONS && n < ty_n; x = ty_find(ty[x].b)) n++;
        if (ty[v].kind == K_SVAR && n) ty_depth_why = 1, snprintf(ty_why, sizeof ty_why, "one path leaves %d more value%s on the stack than the other, so a branch, clause, loop pass or recursive call changes the stack's depth", n, n == 1 ? "" : "s");
        else if (ty[v].kind == K_RVAR) snprintf(ty_why, sizeof ty_why, "a record would have to contain itself: one path adds a field the other has not");
        else { char s[256]; ty_show(s, sizeof s, t, 0); snprintf(ty_why, sizeof ty_why, "a value would have to contain itself, as %s", s); }
        return 1; }
    if ((ty[v].kind == K_VAR || ty[v].kind == K_TVAR) && ty[v].prot) {
        if (ty[t].kind == ty[v].kind && ty_fixed(t) && (ty[v].prot & ~ty[t].prot)) {
            snprintf(ty_why, sizeof ty_why, "the body needs a %s value where the signature allows any type", ty_prot_name(ty[v].prot & ~ty[t].prot)); return 1; }
        if (ty[t].kind == ty[v].kind) ty[t].prot |= ty[v].prot;
        else {
            int need = ty[v].prot;
            ty[v].link = t;
            if ((need & P_COPY) && (ty[t].kind == K_RES || ty[t].kind == K_TAG || ty[t].kind == K_TEXT || ty[t].kind == K_TNIL)) {
                if (ty_copy_parts(t)) return 1;
                need &= ~P_COPY; }
            if ((ty_prot_of(ty[t].kind) & need) != need) {
                char s[256]; ty_show(s, sizeof s, t, 0);
                if (ty[t].kind == K_BOX) snprintf(ty_why, sizeof ty_why, "%s is a box: it cannot be copied, dropped, bound or stored. Free it, or pass it to a word that takes it", s);
                else snprintf(ty_why, sizeof ty_why, "%s is not %s", s, ty_prot_name(need & ~ty_prot_of(ty[t].kind)));
                return 1; }
            return 0;
        }
    }
    ty[v].link = t; return 0;
}
/* t must be copyable: unify it with a variable that asks so. */
static int ty_need(int t, int prot) { int w = ty_new(K_VAR, 0, 0, 0); ty[w].prot = (uint8_t)prot; return ty_unify_at(t, w, 0); }
/* A result or tag is copyable when every payload it may hold is; an open tag set asks it of the tags it
   gains later. The term's own prot marks it checked, so a tag whose payload holds the same tag ends. */
static int ty_copy_parts(int t) {
    c_stack_check("while checking what a value holds");
    t = ty_find(t);
    if (ty[t].prot & P_COPY) return 0;
    ty[t].prot |= P_COPY;
    if (ty[t].kind == K_RES) return ty_need(ty[t].a, P_COPY) || ty_need(ty[t].b, P_COPY);
    int r = ty[t].kind == K_TAG ? ty_find(ty[t].a) : t;
    for (int hops = 0; ty[r].kind == K_TEXT; r = ty_find(ty[r].c), hops++) {
        if (hops == ty_n) die("type checker bug: a tag set links in a cycle");
        if (ty_need(ty_tag_payload(ty[r].sym), P_COPY)) return 1;
    }
    if (ty[r].kind == K_TVAR) {
        if (ty_fixed(r) && !(ty[r].prot & P_COPY)) { snprintf(ty_why, sizeof ty_why, "the body needs a copyable value where the signature allows any tagged value"); return 1; }
        ty[r].prot |= P_COPY; }
    return 0;
}
static int ty_row_take(int r, uint32_t label, int *field, int *rest, int depth);
static int ty_tag_take(int s, uint32_t tag, int *rest, int depth);
/* The variable or end a row or tag set finishes in. */
static int ty_tail(int r) {
    r = ty_find(r);
    for (int hops = 0; ty[r].kind == K_REXT || ty[r].kind == K_TEXT; hops++) { if (hops == ty_n) die("type checker bug: a row links in a cycle"); r = ty_find(ty[r].c); }
    return r;
}
/* Recursion follows nesting; a stack or a row is a chain, which the loop walks. A structure found equal
   to another links to it only once the whole chain is unified: an occurs check during the chain must
   still see the parts of both. */
static struct { int a, b; } *ty_links; static int ty_links_n, ty_links_cap;
static int ty_unify_chain(int a, int b, int depth);
static int ty_unify_at(int a, int b, int depth) {
    int base = ty_links_n, r = ty_unify_chain(a, b, depth);
    if (!r) for (int k = base; k < ty_links_n; k++) ty[ty_links[k].a].link = ty_links[k].b;
    ty_links_n = base; return r;
}
static int ty_unify_chain(int a, int b, int depth) {
  int row_tail = 0; /* the tail of the row this call walks: the same for every entry of it */
  for (;;) {
    if ((depth & 255) == 255) c_stack_check("while unifying types");
    a = ty_find(a); b = ty_find(b);
    if (a == b) return 0;
    if (ty_isvar(ty[a].kind)) return ty_bind(a, b);
    if (ty_isvar(ty[b].kind)) return ty_bind(b, a);
    if (ty[a].kind == K_RNIL && ty[b].kind == K_REXT) { int x = a; a = b; b = x; }
    /* two different symbols meeting leave plain sym: the value may be either */
    if (ty[a].kind == K_SYM && ty[b].kind == K_SYM) {
        int la = ty_label_var(a), lb = ty_label_var(b);
        if (la != lb) {
            int sa = ty[la].a, sb = ty[lb].a;
            if (!(sa && sb && ty[sa].sym == ty[sb].sym)) {
                if (ty[la].named || ty[lb].named) { int n = ty[la].named ? la : lb, o = n == la ? lb : la;
                    snprintf(ty_why, sizeof ty_why, "nth reads the list named '%s by this symbol, so it must be '%s, but it may be %s%s", sym_name(ty[ty[n].a].sym), sym_name(ty[ty[n].a].sym), ty[o].a ? "'" : "another symbol", ty[o].a ? sym_name(ty[ty[o].a].sym) : "");
                    return 1; }
                ty[la].a = ty[lb].a = 0; }
            ty[lb].named |= ty[la].named; if (ty[la].level < ty[lb].level) ty[lb].level = ty[la].level;
            ty[la].link = lb; }
        ty[a].link = b; return 0; }
    /* a closed tag set meets what is left of a set with more tags */
    if (ty[a].kind == K_TNIL && ty[b].kind == K_TEXT) { snprintf(ty_why, sizeof ty_why, "it may be tagged '%s, which is not one of the tags this takes", sym_name(ty[b].sym)); return 1; }
    if (ty[a].kind == K_TEXT && ty[b].kind == K_TNIL) { snprintf(ty_why, sizeof ty_why, "this value is never tagged '%s", sym_name(ty[a].sym)); return 1; }
    if (ty[a].kind != ty[b].kind && !(ty[a].kind == K_REXT && ty[b].kind == K_RNIL)) {
        char s1[256], s2[256]; ty_show(s1, sizeof s1, a, 0); ty_show(s2, sizeof s2, b, 0);
        if (ty[a].kind == K_SNIL || ty[b].kind == K_SNIL) ty_depth_why = 1, snprintf(ty_why, sizeof ty_why, "the stack is shorter than this needs");
        else snprintf(ty_why, sizeof ty_why, "%s is not %s%s", s2, ty_kind_noun(ty[a].kind) ? ty_kind_noun(ty[a].kind) : s1,
                      ty[a].kind == K_LIST && ty[b].kind == K_DICT ? ". Iterate a dict with dict-entries: it leaves the dict and a list of {'key k 'value v}" : "");
        return 1;
    }
    switch (ty[a].kind) {
    case K_REXT: case K_TEXT: {
        /* Take a's first entry out of b, then meet the rests. If taking it binds a's own tail, the two
           rows differ only in order around one shared tail, and no finite row satisfies both. */
        if (!row_tail) row_tail = ty_tail(a);
        int f = 0, rest, tail = row_tail;
        if (ty[a].kind == K_REXT) { if (ty_row_take(b, ty[ty[a].a].sym, &f, &rest, 0)) return 1; }
        else if (ty_tag_take(b, ty[a].sym, &rest, 0)) return 1;
        if (ty_isvar(ty[tail].kind) && ty_find(tail) != tail) { snprintf(ty_why, sizeof ty_why, "these two rows would have to contain each other"); return 1; }
        if (f) { int fa = ty_find(ty[a].b), fb = ty_find(f);
            if ((ty[fa].kind == K_ABS && ty[fb].kind == K_PRE) || (ty[fa].kind == K_PRE && ty[fb].kind == K_ABS)) {
                snprintf(ty_why, sizeof ty_why, "one record has '%s and the other has not", sym_name(ty[ty[a].a].sym)); return 1; }
            if (ty_unify_at(fa, fb, depth + 1)) return 1; }
        a = ty[a].c; b = rest; continue;
    }
    default: {
        /* once equal, a stands for b: a pair two types share is unified once */
        int na = ty[a].b, nb = ty[b].b;
        if (ty[a].a && ty_unify_at(ty[a].a, ty[b].a, depth + 1)) return 1;
        if (ty[a].c && ty_unify_at(ty[a].c, ty[b].c, depth + 1)) return 1;
        if (ty_links_n == ty_links_cap) { ty_links_cap = ty_links_cap ? 2*ty_links_cap : 1024; ty_links = realloc(ty_links, (size_t)ty_links_cap * sizeof *ty_links);
            if (!ty_links) die("type checker: out of memory for %d unified structures", ty_links_cap); }
        ty_links[ty_links_n].a = a; ty_links[ty_links_n++].b = b;
        if (!na) return 0;
        a = na; b = nb; continue;
    }
    }
  }
}
static int ty_unify(int a, int b) { ty_why[0] = 0; ty_depth_why = 0; return ty_unify_at(a, b, 0); }
/* Row r without its most recent field `label`: that field's type and the rest. An open row gains it. */
static int ty_row_take(int r, uint32_t label, int *field, int *rest, int depth) {
    if (depth > 4096) die("type checker: a record has more than %d fields", depth);
    r = ty_find(r);
    if (ty[r].kind == K_REXT) {
        if (ty[ty[r].a].sym == label) { *field = ty[r].b; *rest = ty[r].c; return 0; }
        int f, rr; if (ty_row_take(ty[r].c, label, &f, &rr, depth + 1)) return 1;
        *field = f; *rest = ty_new(K_REXT, ty[r].a, ty[r].b, rr); return 0;
    }
    if (ty[r].kind == K_RVAR) {
        int f = ty_new(K_VAR, 0, 0, 0), rr = ty_new(K_RVAR, 0, 0, 0), l = ty_sym(K_LSYM, label);
        ty[f].level = ty[rr].level = ty[r].level;
        if (ty_bind(r, ty_new(K_REXT, l, f, rr))) return 1;
        *field = f; *rest = rr; return 0;
    }
    if (ty[r].kind == K_RNIL) { *field = ty_new(K_ABS, 0, 0, 0); *rest = r; return 0; }
    snprintf(ty_why, sizeof ty_why, "this is not a record row"); return 1;
}

/* Tag set s without `tag`: an open set gains it. */
static int ty_tag_take(int s, uint32_t tag, int *rest, int depth) {
    if (depth > 4096) die("type checker: a tag set has more than %d tags", depth);
    s = ty_find(s);
    if (ty[s].kind == K_TEXT) {
        if (ty[s].sym == tag) { *rest = ty[s].c; return 0; }
        int rr; if (ty_tag_take(ty[s].c, tag, &rr, depth + 1)) return 1;
        *rest = ty_new(K_TEXT, 0, 0, rr); ty[*rest].sym = ty[s].sym; return 0;
    }
    if (ty[s].kind == K_TVAR) {
        int rr = ty_new(K_TVAR, 0, 0, 0), n = ty_new(K_TEXT, 0, 0, rr); ty[n].sym = tag; ty[rr].level = ty[s].level; ty[rr].prot = ty[s].prot;
        if (ty_bind(s, n)) return 1;
        *rest = rr; return 0;
    }
    snprintf(ty_why, sizeof ty_why, "this value is never tagged '%s", sym_name(tag)); return 1;
}

/* ---- generalizing and instantiating ----
   A word's type generalizes the variables made at a deeper level than its definition; each use of the
   word copies them fresh. */
static void ty_generalize(int t) {
    int n = 0, stamp = ++ty_stamp; ty_work_push(&n, t);
    while (n) {
        int x = ty_find(ty_work[--n]);
        if (ty_mark[x] == stamp) continue;
        ty_mark[x] = stamp;
        if (ty_isvar(ty[x].kind)) { if (ty[x].level > ty_level) ty[x].level = GENERIC; continue; }
        if (ty[x].a) ty_work_push(&n, ty[x].a);
        if (ty[x].b) ty_work_push(&n, ty[x].b);
        if (ty[x].c) ty_work_push(&n, ty[x].c);
    }
}
/* The variables the last instantiation made, for ty_rigid. */
static int *ty_memo_to, ty_memo_n, ty_memo_cap, ty_copy_stamp;
static int ty_copy(int t, int depth) {
    if ((depth & 255) == 255) c_stack_check("while copying a type");
    t = ty_find(t);
    if (ty_mark[t] == ty_copy_stamp) return ty_to[t];
    int r = t;
    if (ty_isvar(ty[t].kind)) {
        if (ty[t].level == GENERIC) {
            r = ty_new(ty[t].kind, 0, 0, 0); ty[r].prot = ty[t].prot; ty[r].sealed = ty[t].sealed;
            if (ty[t].kind == K_LVAR) { ty[r].a = ty[t].a; ty[r].named = ty[t].named; }
            if (ty_memo_n == ty_memo_cap) { ty_memo_cap = ty_memo_cap ? 2*ty_memo_cap : 256; ty_memo_to = realloc(ty_memo_to, (size_t)ty_memo_cap * sizeof(int));
                if (!ty_memo_to) die("type checker: out of memory for %d type variables", ty_memo_cap); }
            ty_memo_to[ty_memo_n++] = r;
        }
    } else if (ty[t].a || ty[t].b || ty[t].c) {
        int a = ty[t].a ? ty_copy(ty[t].a, depth + 1) : 0, b = ty[t].b ? ty_copy(ty[t].b, depth + 1) : 0, c = ty[t].c ? ty_copy(ty[t].c, depth + 1) : 0;
        if (a != ty[t].a || b != ty[t].b || c != ty[t].c) { r = ty_new(ty[t].kind, a, b, c); ty[r].sym = ty[t].sym; }
    }
    ty_mark[t] = ty_copy_stamp; ty_to[t] = r; return r;
}
static int ty_instantiate(int t) { ty_memo_n = 0; ty_copy_stamp = ++ty_stamp; return ty_copy(t, 0); }

/* The type of every primitive: 'name ( ins -> outs ), bottom first. A body's stack rest is `..s`;
   where a body runs on the stack below a word's inputs, the two rests are the same variable. `..!r` is
   a rest the body may not reach: the runtime keeps other values there. `at`, `into` and `edit` take
   their key from the program's text, so ty_range types them. */
static const char *TYPES =
    "'dup ( 'a -> 'a 'a ) 'drop ( 'a -> ) 'swap ( 'a 'b -> 'b 'a ) 'over ( 'a 'b -> 'a 'b 'a ) 'rot ( 'a 'b 'c -> 'b 'c 'a )\n"
    "'plus ( 'a num 'a num -> 'a num ) 'sub ( 'a num 'a num -> 'a num ) 'mul ( 'a num 'a num -> 'a num ) 'div ( 'a num 'a num -> 'a num )\n"
    "'mod ( int int -> int ) 'wrap ( int int -> int ) 'band ( int int -> int ) 'bor ( int int -> int ) 'bxor ( int int -> int )\n"
    "'shl ( int int -> int ) 'shr ( int int -> int ) 'and ( int int -> int ) 'or ( int int -> int ) 'bnot ( int -> int ) 'divmod ( int int -> int int )\n"
    "'eq ( 'a 'a -> int ) 'lt ( 'a ord 'a ord -> int )\n"
    "'itof ( int -> float ) 'ftoi ( float -> int ) 'fsqrt ( float -> float ) 'ffloor ( float -> float ) 'fround ( float -> float )\n"
    "'fexp ( float -> float ) 'flog ( float -> float ) 'fpow ( float float -> float ) 'fatan2 ( float float -> float )\n"
    "'print ( 'a -> ) 'assert ( int -> ) 'millis ( -> int ) 'datetime ( -> int list ) 'random ( int -> int ) 'isheadless ( -> int )\n"
    "'apply ( ..s ( ..s -> ..t ) -> ..t ) 'dip ( ..s 'x ( ..s -> ..t ) -> ..t 'x )\n"
    "'if ( ..s int ( ..s -> ..t ) ( ..s -> ..t ) -> ..t ) 'while ( ..a ( ..a -> ..b int ) ( ..b -> ..a ) -> ..b )\n"
    "'each ( ..s 'a list ( ..!r 'a -> ..!r 'b ) -> ..s 'b list ) 'fold ( ..s 'a list 'b ( ..!r 'b 'a -> ..!r 'b ) -> ..s 'b )\n"
    "'filter ( ..s 'a list ( ..!r 'a -> ..!r int ) -> ..s 'a list )\n"
    "'list ( -> 'a list ) 'len ( 'a list -> int ) 'push ( 'a list 'a -> 'a list ) 'pop ( 'a list -> 'a list {'ok 'a 'no ()} either )\n"
    "'get ( 'a list int -> {'ok 'a 'no ()} either ) 'peek ( 'a list int -> 'a list {'ok 'a 'no ()} either )\n"
    "'set ( 'a list int 'a -> {'ok 'a list 'no ()} either ) 'cat ( 'a list 'a list -> 'a list ) 'reverse ( 'a list -> 'a list )\n"
    "'take-n ( 'a list int -> 'a list ) 'drop-n ( 'a list int -> 'a list ) 'range ( int int -> int list ) 'sort ( 'a ord list -> 'a ord list )\n"
    "'index-of ( 'a list 'a -> {'ok int 'no ()} either ) 'zip ( 'a list 'a list -> 'a list list )\n"
    "'str-find ( str str -> {'ok int 'no ()} either ) 'str-split ( str str -> str list )\n"
    "'rec ( -> {} )\n"
    "'must ( {'ok 'a 'no 'b} either -> 'a ) 'fail ( ..a str -> ..b )\n"
    "'pthen ( ..s {'ok 'a 'no 'b} either 'd copy ( ..s 'a -> ..s 'd {'ok 'c 'no 'b} either ) -> ..s 'd {'ok 'c 'no 'b} either )\n"
    "'box ( 'a -> 'a box ) 'free ( 'a box -> ) 'mutate ( ..s 'a box ( ..!r 'a -> ..!r 'b ) -> ..s 'b box )\n"
    "'dict ( -> 'a dict ) 'insert ( 'a dict str 'a -> 'a dict ) 'of ( 'a dict str -> 'a dict {'ok 'a 'no str} either )\n"
    "'remove ( 'a dict str -> 'a dict ) 'dict-keys ( 'a dict -> 'a dict str list ) 'dict-entries ( 'a dict -> 'a dict {'key str 'value 'a} list )\n"
    "'read ( str -> {'ok str 'no str} either ) 'write ( str str -> {'ok int 'no str} either ) 'ls ( str -> {'ok str list 'no str} either )\n"
    "'args ( -> str list ) 'parse-http ( str -> {'ok {'status int 'headers {'key str 'value str} list 'body str} 'no str} either )\n"
    /* a socket is its own type: the runtime keeps it in a box, but free, lend and mutate must not reach it */
    "'tcp-connect ( str int -> {'ok socket 'no str} either ) 'tcp-send ( socket str -> socket {'ok int 'no str} either )\n"
    "'tcp-recv ( socket int -> socket {'ok str 'no str} either ) 'tcp-close ( socket -> ) 'tcp-listen ( int -> {'ok socket 'no str} either )\n"
    "'tcp-accept ( socket -> socket {'ok socket 'no str} either )\n"
    "'clear ( int -> ) 'pixel ( int int int -> ) 'fill-rect ( int int int int int -> )\n";

/* ---- signatures ----
   One syntax for the builtin table and for program signatures. Names are per signature: the same 'a
   is one variable, `..s` names a stack's rest. */
typedef struct { uint32_t name[64]; int term[64]; uint8_t kind[64]; int n; } TyNames;
static int ty_named(TyNames *nm, uint32_t name, int kind) {
    for (int k = 0; k < nm->n; k++) if (nm->name[k] == name && nm->kind[k] == kind) return nm->term[k];
    if (nm->n == 64) die("type annotation: more than 64 names in one signature");
    int t = ty_new(kind, 0, 0, 0); ty[t].sealed = kind == K_SVAR && sym_name(name)[2] == '!'; nm->name[nm->n] = name; nm->term[nm->n] = t; nm->kind[nm->n++] = (uint8_t)kind; return t;
}
static int ty_word_is(Token *t, const char *w) { return t->tag == TOK_WORD && strcmp(sym_name(t->as.sym), w) == 0; }
static int ty_prot_word(Token *t) {
    if (t->tag != TOK_WORD) return 0;
    const char *w = sym_name(t->as.sym);
    /* every num or ord value is copyable */
    int p = !strcmp(w, "num") ? P_NUM : !strcmp(w, "ord") ? P_ORD : !strcmp(w, "copy") ? P_COPY : 0;
    return p ? p | P_COPY : 0;
}
static int ty_parse_fn(Token *toks, int open, int close, TyNames *nm, int rest);
/* While a slot signature parses, the word's own stack rest: a body type in a slot that names no rest of
   its own runs on it, and a body type inside that one runs on its enclosing body's. 0 elsewhere. */
static int ty_slot_rest;
static void ty_mark_copy(int t, int line);
static int ty_parse(Token *toks, int *i, int end, TyNames *nm) {
    c_stack_check("while reading a type annotation");
    if (*i >= end) die("type annotation: a type is missing at line %d", toks[end-1].line);
    Token *t = &toks[*i]; int base = 0;
    if (t->tag == TOK_WORD) {
        const char *w = sym_name(t->as.sym); int p = ty_prot_word(t);
        if (!strcmp(w, "int")) base = ty_new(K_INT, 0, 0, 0);
        else if (!strcmp(w, "float")) base = ty_new(K_FLOAT, 0, 0, 0);
        else if (!strcmp(w, "sym")) base = ty_new(K_SYM, ty_new(K_LVAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "str")) base = ty_new(K_LIST, ty_new(K_INT, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "tagged")) base = ty_new(K_TAG, ty_new(K_TVAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "rec")) base = ty_new(K_REC, ty_new(K_RVAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "tuple")) base = ty_new(K_FN, ty_new(K_SVAR, 0, 0, 0), ty_new(K_SVAR, 0, 0, 0), 0);
        else if (!strcmp(w, "list") || !strcmp(w, "seq")) base = ty_new(K_LIST, ty_new(K_VAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "dict")) base = ty_new(K_DICT, ty_new(K_VAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "box")) base = ty_new(K_BOX, ty_new(K_VAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "socket")) base = ty_new(K_SOCK, 0, 0, 0);
        else if (p) { base = ty_new(K_VAR, 0, 0, 0); ty[base].prot = (uint8_t)p; }
        else die("type annotation: unknown type word '%s' at line %d", w, t->line);
        (*i)++;
    } else if (t->tag == TOK_SYM) {
        if (*i + 1 < end && ty_word_is(&toks[*i + 1], "sym")) die("type annotation: a symbol's type is sym; write sym, not '%s sym, at line %d", sym_name(t->as.sym), t->line);
        { base = ty_named(nm, t->as.sym, K_VAR); (*i)++;
            for (int p; *i < end && (p = ty_prot_word(&toks[*i])); (*i)++) ty[base].prot |= (uint8_t)p; }
    } else if (t->tag == TOK_LPAREN) {
        base = ty_parse_fn(toks, *i, *i + t->span, nm, ty_slot_rest); *i += t->span + 1;
    } else if (t->tag == TOK_LBRACE) {
        int close = *i + t->span, j = *i + 1; uint32_t keys[64], rest_sym = 0; int types[64], n = 0;
        while (j < close) {
            if (ty_word_is(&toks[j], "|")) { if (j + 1 >= close || toks[j+1].tag != TOK_SYM) die("type annotation: '|' needs a row name after it at line %d", toks[j].line);
                rest_sym = toks[j+1].as.sym; j += 2; continue; }
            if (toks[j].tag != TOK_SYM) die("type annotation: a record or either type takes 'name type pairs, at line %d", toks[j].line);
            if (n == 64) die("type annotation: more than 64 fields at line %d", toks[j].line);
            for (int m = 0; m < n; m++) if (keys[m] == toks[j].as.sym) die("type annotation: '%s appears twice in one record or either type, at line %d", sym_name(toks[j].as.sym), toks[j].line);
            keys[n] = toks[j].as.sym; j++; types[n++] = ty_parse(toks, &j, close, nm);
        }
        *i = close + 1;
        if (*i < end && ty_word_is(&toks[*i], "either")) {
            (*i)++; int okno = n > 0, ok = 0, no = 0;
            for (int k = 0; k < n; k++) { if (keys[k] == S_OK) ok = types[k]; else if (keys[k] == S_NO) no = types[k]; else okno = 0; }
            if (okno && rest_sym) die("type annotation: {'ok ... 'no ...} either is a result, which holds only 'ok and 'no: drop | '%s, at line %d", sym_name(rest_sym), t->line);
            if (okno) base = ty_new(K_RES, ok ? ok : ty_new(K_VAR, 0, 0, 0), no ? no : ty_new(K_VAR, 0, 0, 0), 0);
            else { int row = rest_sym ? ty_named(nm, rest_sym, K_TVAR) : ty_new(K_TNIL, 0, 0, 0);
                for (int k = n - 1; k >= 0; k--) { if (ty_unify(types[k], ty_tag_payload(keys[k]))) die("type annotation: tag '%s here conflicts with its payload elsewhere: %s", sym_name(keys[k]), ty_why);
                    row = ty_new(K_TEXT, 0, 0, row); ty[row].sym = keys[k]; }
                base = ty_new(K_TAG, row, 0, 0); }
        } else {
            int row = rest_sym ? ty_named(nm, rest_sym, K_RVAR) : ty_new(K_RNIL, 0, 0, 0);
            for (int k = 0; k < n; k++) row = ty_new(K_REXT, ty_sym(K_LSYM, keys[k]), ty_new(K_PRE, types[k], 0, 0), row);
            base = ty_new(K_REC, row, 0, 0);
        }
    } else die("type annotation: a type is expected at line %d", t->line);
    for (; *i < end && toks[*i].tag == TOK_WORD; (*i)++) {
        const char *w = sym_name(toks[*i].as.sym);
        if (!strcmp(w, "list") || !strcmp(w, "seq")) base = ty_new(K_LIST, base, 0, 0);
        else if (!strcmp(w, "dict")) base = ty_new(K_DICT, base, 0, 0);
        else if (!strcmp(w, "box")) base = ty_new(K_BOX, base, 0, 0);
        else break;
    }
    return base;
}
/* ( ins -> outs ): the rest is `..name` when written, else one fresh rest for both sides (or `rest`). */
static int ty_parse_fn(Token *toks, int open, int close, TyNames *nm, int rest) {
    int j = open + 1, in, out, dash = -1;
    for (int k = open + 1; k < close; k += toks[k].span + 1) if (ty_word_is(&toks[k], "->")) { dash = k; break; }
    if (dash < 0) { if (close == open + 1) { int r = rest ? rest : ty_new(K_SVAR, 0, 0, 0); return ty_new(K_FN, r, r, 0); }
        die("type annotation: a body type needs ->, as in ( int -> int ), at line %d", toks[open].line); }
    int r = rest ? rest : 0;
    for (int side = 0; side < 2; side++) {
        int end = side ? close : dash, s;
        if (j < end && toks[j].tag == TOK_WORD && sym_name(toks[j].as.sym)[0] == '.' && sym_name(toks[j].as.sym)[1] == '.') { s = ty_named(nm, toks[j].as.sym, K_SVAR); j++; }
        else { if (!r) r = ty_new(K_SVAR, 0, 0, 0); s = r; }
        int slot = ty_slot_rest; if (slot) ty_slot_rest = s;
        while (j < end) s = ty_new(K_SCONS, ty_parse(toks, &j, end, nm), s, 0);
        ty_slot_rest = slot;
        if (side) out = s; else in = s;
        j = dash + 1;
    }
    return ty_new(K_FN, in, out, 0);
}
/* A program signature: `[ type own in  type move out ... ]`, one slot per value, bottom first. */
static int ty_parse_slots(Token *toks, int open, int close, TyNames *nm) {
    int rest = ty_new(K_SVAR, 0, 0, 0), in = rest, out = rest, slot = ty_slot_rest;
    ty_slot_rest = rest;
    for (int j = open + 1; j < close; ) {
        int k = j;
        while (k < close && !(toks[k].tag == TOK_WORD && (ty_word_is(&toks[k], "own") || ty_word_is(&toks[k], "lent") || ty_word_is(&toks[k], "copy") || ty_word_is(&toks[k], "move") || ty_word_is(&toks[k], "auto")))) k += toks[k].span + 1;
        if (k + 1 > close) die("type annotation: each slot ends with own/lent/copy/move/auto and in/out, at line %d", toks[j].line);
        int t = j < k ? ty_parse(toks, &j, k, nm) : ty_new(K_VAR, 0, 0, 0);
        /* a lent or copy slot is copyable; an own, move or auto one may hold a box */
        if (ty_word_is(&toks[k], "lent") || ty_word_is(&toks[k], "copy")) ty_mark_copy(t, toks[k].line);
        if (j < k) die("type annotation: a slot holds one type, then own/lent/copy/move/auto and in/out, but %s%s follows the type, at line %d", toks[j].tag == TOK_SYM ? "'" : "", toks[j].tag == TOK_WORD || toks[j].tag == TOK_SYM ? sym_name(toks[j].as.sym) : "a value", toks[j].line);
        if (ty_word_is(&toks[k+1], "in")) in = ty_new(K_SCONS, t, in, 0);
        else if (ty_word_is(&toks[k+1], "out")) out = ty_new(K_SCONS, t, out, 0);
        else die("type annotation: a slot ends with in or out, at line %d", toks[k].line);
        j = k + 2;
    }
    ty_slot_rest = slot;
    return ty_new(K_FN, in, out, 0);
}
static void ty_held_copy(int t, int line);
/* A signature says this type is copyable: its variables and open tag sets are, and so are the payloads
   of the tags it names. */
static void ty_mark_copy(int t, int line) {
    t = ty_find(t);
    if (ty[t].kind == K_VAR || ty[t].kind == K_TVAR) { ty[t].prot |= P_COPY; return; }
    if (ty[t].kind == K_RES) { ty_mark_copy(ty[t].a, line); ty_mark_copy(ty[t].b, line); return; }
    if (ty[t].kind != K_TAG) return;
    int r = ty_find(ty[t].a);
    for (int hops = -ty_n; ty[r].kind == K_TEXT; r = ty_rest(r, &hops))
        if (ty_need(ty_tag_payload(ty[r].sym), P_COPY)) die("type annotation: this slot is copyable, but tag '%s holds %s, at line %d", sym_name(ty[r].sym), ty_why, line);
    if (ty[r].kind == K_TVAR) ty[r].prot |= P_COPY;
}
/* A scheme: a signature parsed at a deeper level and generalized. */
static int ty_scheme_slots(Token *toks, int open, int close) {
    TyNames nm = {0}; ty_level++;
    int t = ty_parse_slots(toks, open, close, &nm); ty_level--; ty_held_copy(t, toks[open].line); ty_generalize(t); return t;
}
/* A signature instance whose variables only stand for themselves: a body must work for all of them,
   including every stack a body type in it names. ty_unrigid frees them once the check is done. */
static int *ty_rigid_vars, ty_rigid_n, ty_rigid_cap;
/* Signatures nest (a word defined in a word's body), so the rigid variables form a stack: mark is where
   this signature's start, and ty_unrigid(mark) frees them. */
static int ty_rigid(int scheme, int *mark) {
    int t = ty_instantiate(scheme);
    if (ty_rigid_n + ty_memo_n > ty_rigid_cap) { ty_rigid_cap = 2*(ty_rigid_n + ty_memo_n); ty_rigid_vars = realloc(ty_rigid_vars, (size_t)ty_rigid_cap * sizeof(int)); if (!ty_rigid_vars) die("type checker: out of memory for %d signature variables", ty_rigid_cap); }
    *mark = ty_rigid_n;
    for (int k = 0; k < ty_memo_n; k++) { ty_rigid_vars[ty_rigid_n++] = ty_memo_to[k]; ty[ty_memo_to[k]].rigid = 1; }
    return ty_find(t);
}
static void ty_unrigid(int mark) { while (ty_rigid_n > mark) ty[ty_rigid_vars[--ty_rigid_n]].rigid = 0; }

/* ---- names ---- */
/* word: 0 a value, 1 a word, 2 the word whose body is being checked. declared: `'name [sig] effect`
   without its body yet. depth: how many bodies deep the name was bound (0: the top level). */
typedef struct { uint32_t sym; int ty, word, line, declared, depth; } TyBind;
static TyBind *tyb; static int tyb_n, tyb_cap, tyb_prelude;
static int ty_builtin[SYM_MAX], ty_tagpay[SYM_MAX];
static int ty_tag_payload(uint32_t tag) { if (!ty_tagpay[tag]) { int l = ty_level; ty_level = 0; ty_tagpay[tag] = ty_new(K_VAR, 0, 0, 0); ty_level = l; } return ty_tagpay[tag]; }
static int tyb_find(uint32_t sym) { for (int k = tyb_n - 1; k >= 0; k--) if (tyb[k].sym == sym) return k; return -1; }
static int ty_body_depth;
static void tyb_push(uint32_t sym, int t, int word, int line) {
    if (tyb_n == tyb_cap) { tyb_cap = tyb_cap ? 2*tyb_cap : 1024; tyb = realloc(tyb, (size_t)tyb_cap * sizeof(TyBind)); if (!tyb) die("type checker: out of memory for %d names", tyb_cap); }
    tyb[tyb_n++] = (TyBind){sym, t, word, line, 0, ty_body_depth};
}
/* A body inside a [...] or {...} literal is built when the program is read, so the names it sees are
   the top level's and its own: none bound in the bodies around the literal (ty_lit_depth deep). */
static int ty_lit_depth = -1;
static int tyb_visible(int b) { return ty_lit_depth < 0 || tyb[b].depth == 0 || tyb[b].depth > ty_lit_depth; }
static int ty_in_prelude;
/* The end of the program's tokens, for a look ahead past the range being checked. */
static int ty_tok_end;
static uint32_t S_LEND, S_EQ, S_NEQ;
static void ty_err(int line, const char *fmt, ...);
/* Words the runtime reads as forms, not bindings. */
static int ty_reserved(uint32_t sym) {
    return sym == S_LET || sym == S_EFFECT || sym == S_TAG || sym == S_CASE || sym == S_NTH
        || sym == S_LEND || sym == S_ON || sym == S_SHOW || sym == S_AT || sym == S_INTO || sym == S_EDIT;
}
/* Forward declarations waiting for their body, per body depth: code that runs in that scope before the
   body is bound may call the word. */
static int *ty_pending, ty_pending_cap;
static void ty_pending_add(int depth, int d) {
    if (depth >= ty_pending_cap) { int old = ty_pending_cap; ty_pending_cap = 2*depth + 16; ty_pending = realloc(ty_pending, (size_t)ty_pending_cap * sizeof(int));
        if (!ty_pending) die("type checker: out of memory for %d scopes", ty_pending_cap); memset(ty_pending + old, 0, (size_t)(ty_pending_cap - old) * sizeof(int)); }
    ty_pending[depth] += d;
}
static int ty_literal;
/* Code about to run a user word, or a body: refused inside a literal's own code, which runs when the
   program is read, and in a scope whose declared words have no body yet. */
static void ty_runs(const char *who, int line) {
    if (ty_literal) ty_err(line, "'%s' runs code, but a [...] or {...} literal is built when the program is read, so it cannot run code that may use names bound later. Build the value outside the literal.", who);
    if (ty_body_depth < ty_pending_cap && ty_pending[ty_body_depth] > 0)
        for (int k = tyb_n - 1; k >= 0; k--) if (tyb[k].declared && tyb[k].depth == ty_body_depth) {
            ty_err(line, "'%s' runs code here, but '%s' is declared on line %d and not defined yet, so that code may call it. Define '%s' first.", who, sym_name(tyb[k].sym), tyb[k].line, sym_name(tyb[k].sym)); break; }
}
/* User code never rebinds a name: a word or value already visible, a primitive, or a form. A declared
   word is defined by a body in the declaration's own scope. */
static int ty_redefined(uint32_t sym, int line, int define) {
    if (ty_in_prelude) return 0;
    if (ty_reserved(sym)) { ty_err(line, "'%s' is already defined: it is a built-in word. Pick another name.", sym_name(sym)); return 1; }
    int b = tyb_find(sym);
    if (b >= 0 && tyb[b].declared && (!define || tyb[b].depth != ty_body_depth)) {
        ty_err(line, "'%s' is declared as a word on line %d, so it is defined by a body written before its name in the same scope: `(...) '%s let`.", sym_name(sym), tyb[b].line, sym_name(sym)); return 1; }
    if (b >= 0 && tyb[b].declared) return 0;
    if (b >= 0 && b >= tyb_prelude) { ty_err(line, "'%s' is already defined (first defined on line %d). Names are never rebound; pick another name.", sym_name(sym), tyb[b].line); return 1; }
    if (b >= 0 || ty_builtin[sym]) { ty_err(line, "'%s' is already defined: it is a built-in word. Pick another name.", sym_name(sym)); return 1; }
    return 0;
}
/* Forward declarations still waiting for their body when their scope ends. */
static void ty_undefined(int from) {
    for (int k = from; k < tyb_n; k++) if (tyb[k].declared) ty_err(tyb[k].line, "'%s' is declared here but never defined. Write its body before the end of this scope: `(...) '%s let`.", sym_name(tyb[k].sym), sym_name(tyb[k].sym));
}

/* ---- inference ---- */
static int ty_cur, ty_errors;
static int ty_on[16], ty_on_line[16], ty_on_mouse[16], ty_on_n, ty_shown;
static void ty_err(int line, const char *fmt, ...) {
    ty_errors++; int fid = LOC_FID(current_loc); const char *f = src_files[fid];
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "\n-- TYPE ERROR %s:%d ", f, line);
    int hl = 15 + (int)strlen(f) + 10; for (int i = hl; i < 60; i++) fputc('-', stderr);
    fprintf(stderr, "\n\n    "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n\n"); va_end(ap);
    print_source_line(stderr, fid, line, 0); fprintf(stderr, "\n");
}
static void ty_push(int t) { ty_cur = ty_new(K_SCONS, t, ty_cur, 0); }
static const char *ty_at_word = "this";
static int ty_pop(void) {
    int v = ty_new(K_VAR, 0, 0, 0), r = ty_new(K_SVAR, 0, 0, 0);
    if (ty_unify(ty_cur, ty_new(K_SCONS, v, r, 0))) { ty_err(LOC_LINE(current_loc), "'%s' takes more values than the stack holds here.", ty_at_word); return v; }
    ty_cur = r; return v;
}
/* The stack variable a stack type ends in. */
static int ty_stack_tail(int s) {
    s = ty_find(s);
    for (int hops = 0; ty[s].kind == K_SCONS; hops++) { if (hops == ty_n) die("type checker bug: a stack links in a cycle"); s = ty_find(ty[s].b); }
    return s;
}
static void ty_apply(int scheme, const char *who, int line, int user) {
    int f = ty_find(ty_instantiate(scheme)), body = user;
    for (int x = ty_find(ty[f].a), hops = -ty_n; !body && ty[x].kind == K_SCONS; x = ty_rest(x, &hops)) body = ty[ty_find(ty[x].a)].kind == K_FN;
    if (body) ty_runs(who, line);
    if (ty_unify(ty[f].a, ty_cur)) {
        /* The message shows what the word takes, from a fresh copy, and as many values from the top of the stack. */
        char want[512], before[512]; int g = ty_find(ty_instantiate(scheme)), n = 0;
        for (int x = ty_find(ty[g].a); ty[x].kind == K_SCONS && n < ty_n; x = ty_find(ty[x].b)) n++;
        ty_print_count = 0; ty_show_top(want, sizeof want, ty[g].a, n); ty_show_top(before, sizeof before, ty_cur, n);
        /* A value bound with let has one type, so once a use fixes a bound body's stack depth, it runs only there. */
        char hint[512] = "";
        for (int x = ty_find(ty_cur), k = ty_depth_why ? n : 0; !hint[0] && k-- > 0 && ty[x].kind == K_SCONS; x = ty_find(ty[x].b)) {
            int v = ty_find(ty[x].a);
            if (ty[v].kind != K_FN || ty[ty_stack_tail(ty[v].a)].kind == K_SVAR) continue;
            for (int b = tyb_n - 1; b >= 0; b--) if (!tyb[b].word && ty_find(tyb[b].ty) == v) {
                snprintf(hint, sizeof hint, "\n    '%s' is a body bound with let, and a body bound with let runs at one stack depth. Bound with its body written in place, `(...) '%s let`, it is a word, which runs at any depth.", sym_name(tyb[b].sym), sym_name(tyb[b].sym)); break; }
        }
        ty_err(line, "'%s' takes %s\n    but the stack has %s\n    %s.%s", who, n ? want : "nothing", before, ty_why, hint);
        ty_cur = ty[f].b; return;
    }
    ty_cur = ty[f].b;
}
static void ty_range(Token *toks, int i, int end);
/* A word's own calls inside its body (TyBind.word == 2): each call gets a fresh type, checked against
   the body's once the body is known. */
/* A call belongs to the word it calls (its binding's index) and is made at that word's body level, so
   a word defined inside the body does not generalize it. */
static int *ty_rec_call, *ty_rec_line, *ty_rec_owner, ty_rec_n, ty_rec_cap;
static int ty_self_fn(int b, int line) {
    if (ty_rec_n == ty_rec_cap) { ty_rec_cap = ty_rec_cap ? 2*ty_rec_cap : 256;
        ty_rec_call = realloc(ty_rec_call, (size_t)ty_rec_cap * sizeof(int)); ty_rec_line = realloc(ty_rec_line, (size_t)ty_rec_cap * sizeof(int)); ty_rec_owner = realloc(ty_rec_owner, (size_t)ty_rec_cap * sizeof(int));
        if (!ty_rec_call || !ty_rec_line || !ty_rec_owner) die("type checker: out of memory for %d recursive calls", ty_rec_cap); }
    int level = ty_level; ty_level = tyb[b].ty;
    int fn = ty_new(K_FN, ty_new(K_SVAR, 0, 0, 0), ty_new(K_SVAR, 0, 0, 0), 0);
    ty_level = level;
    ty_rec_call[ty_rec_n] = fn; ty_rec_line[ty_rec_n] = line; ty_rec_owner[ty_rec_n++] = b; return fn;
}
/* t with variable v replaced by w; parts without v are shared. */
static int ty_subst_at(int t, int v, int w, int stamp, int depth) {
    if ((depth & 255) == 255) c_stack_check("while copying a type");
    t = ty_find(t);
    if (t == v) return w;
    if (ty_mark[t] == stamp) return ty_to[t];
    int r = t;
    if (!ty_isvar(ty[t].kind) && (ty[t].a || ty[t].b || ty[t].c)) {
        int a = ty[t].a ? ty_subst_at(ty[t].a, v, w, stamp, depth + 1) : 0, b = ty[t].b ? ty_subst_at(ty[t].b, v, w, stamp, depth + 1) : 0, c = ty[t].c ? ty_subst_at(ty[t].c, v, w, stamp, depth + 1) : 0;
        if (a != ty[t].a || b != ty[t].b || c != ty[t].c) { r = ty_new(ty[t].kind, a, b, c); ty[r].sym = ty[t].sym; }
    }
    ty_mark[t] = stamp; ty_to[t] = r; return r;
}
static int ty_subst(int t, int v, int w) { return ty_subst_at(t, v, w, ++ty_stamp, 0); }
/* in: the stack a signature says the body takes, or 0 for any. */
static int ty_body(Token *toks, int open, int close, int in) {
    /* a body inside a literal runs later, so it may use names bound when the program runs: the top level's */
    int saved = ty_cur, mark = tyb_n, lit = ty_literal, lit_depth = ty_lit_depth; ty_cur = in ? in : ty_new(K_SVAR, 0, 0, 0); in = ty_cur;
    c_stack_check("while checking nested bodies");
    if (lit) ty_lit_depth = ty_body_depth;
    ty_body_depth++; ty_literal = 0; ty_range(toks, open + 1, close); ty_literal = lit; ty_body_depth--; ty_lit_depth = lit_depth;
    ty_undefined(mark);
    int fn = ty_new(K_FN, in, ty_cur, 0); ty_cur = saved; tyb_n = mark; return fn;
}
/* `(body) [sig] effect 'name let`: a word. Inside its body the word has one type (or its declared
   one); after, the type generalizes. */
static void ty_define(Token *toks, int open, int close, int sig_open, int sig_close, uint32_t name, int line) {
    ty_redefined(name, line, 1);
    if (ty_literal) ty_err(line, "a [...] or {...} literal is built when the program is read, so it cannot bind names: '%s' would be bound for the whole program. Define it outside the literal.", sym_name(name));
    int fwd = tyb_find(name);
    if (fwd >= 0 && !tyb[fwd].declared) fwd = -1;
    if (fwd >= 0 && sig_open) { ty_err(line, "'%s' is declared on line %d with its signature, so its definition takes that one. Drop this second signature.", sym_name(name), tyb[fwd].line); sig_open = 0; }
    int scheme = sig_open ? ty_scheme_slots(toks, sig_open, sig_close) : fwd >= 0 ? tyb[fwd].ty : 0, mark = tyb_n;
    if (fwd >= 0) { tyb[fwd].declared = 0; ty_pending_add(tyb[fwd].depth, -1); }
    ty_level++;
    /* for word 2, ty holds the body's level, where its own calls are made */
    tyb_push(name, scheme ? scheme : ty_level, scheme ? 1 : 2, line);
    int rmark = ty_rigid_n, want = scheme ? ty_rigid(scheme, &rmark) : 0;
    int bt = ty_body(toks, open, close, want ? ty[want].a : 0);
    /* Each call of the word inside its own body uses the body's type with its own stack rest, since a
       call may sit above more values than the body started on. The rest is fresh only when it will
       generalize; every other part of the type is the same at every call. */
    int keep = 0;
    for (int k = 0; k < ty_rec_n; k++) {
        if (ty_rec_owner[k] != mark) { ty_rec_call[keep] = ty_rec_call[k]; ty_rec_line[keep] = ty_rec_line[k]; ty_rec_owner[keep++] = ty_rec_owner[k]; continue; }
        int inst = bt, tin = ty_stack_tail(ty[bt].a), tout = ty_stack_tail(ty[bt].b);
        if (ty[tin].kind == K_SVAR && ty[tin].level > ty_level - 1) inst = ty_subst(inst, tin, ty_new(K_SVAR, 0, 0, 0));
        if (tout != tin && ty[tout].kind == K_SVAR && ty[tout].level > ty_level - 1) inst = ty_subst(inst, tout, ty_new(K_SVAR, 0, 0, 0));
        /* A call must not bind the body's own stack rests: that would grow the body's type after inst was made.
           A body with a returning path ties its output rest to its input rest, and the occurs check refuses a call that changes the depth. */
        int ret = ty_find(tin) == ty_find(tout), call = ty_rec_call[k], bad;
        uint8_t rin = ty[tin].rigid, rout = ty[tout].rigid;
        if (ret) bad = ty_unify(call, inst);
        else {
            if (ty[tin].kind == K_SVAR) ty[tin].rigid = 1;
            if (ty[tout].kind == K_SVAR) ty[tout].rigid = 1;
            ty_rigid_rest = 0; bad = ty_unify(ty[call].a, ty[inst].a);
            if (bad && ty_rigid_rest) snprintf(ty_why, sizeof ty_why, "each call would need more values below it than the call before");
            else if (!bad && (ty_rigid_rest = 0, bad = ty_unify(ty[call].b, ty[inst].b)) && ty_rigid_rest) snprintf(ty_why, sizeof ty_why, "each call leaves a different number of values than the body leaves, so the body's output changes with every call");
            ty[tout].rigid = rout; ty[tin].rigid = rin; }
        if (bad) ty_err(ty_rec_line[k], "'%s' calls itself here with a stack its body does not take: %s.", sym_name(name), ty_why);
    }
    ty_rec_n = keep;
    if (want) { char a[512], b[512];
        /* shown before they meet, so the message names each variable once, in the types and in why */
        ty_print_count = 0; ty_show(a, sizeof a, want, 0); ty_show(b, sizeof b, bt, 0);
        if (ty_unify(want, bt)) {
            ty_err(line, "'%s' declares %s, but its body is %s: %s.", sym_name(name), a, b, ty_why); want = 0; } }
    ty_level--; tyb_n = mark;
    ty_unrigid(rmark);
    if (want) { ty_generalize(want); scheme = want; }
    else if (!scheme) { ty_generalize(bt); scheme = bt; }
    tyb_push(name, scheme, 1, line);
}
/* `x {'tag (…) … '_ (…)} case`: each clause runs on its tag's payload, and a last '_ clause on x itself.
   Without '_, every tag x may carry has a clause: an open tag set closes on the clause tags. */
static int ty_clause_mark[SYM_MAX], ty_clause_stamp;
static void ty_case(Token *toks, int open, int close, int line) {
    ty_runs("case", line); ty_at_word = "case";
    int s = ty_pop(), rest = ty_cur, out = ty_new(K_SVAR, 0, 0, 0), stamp = ++ty_clause_stamp, items = 0, tags = 0, wild = 0, res = 1, okc = 0, noc = 0, a = 0, b = 0;
    uint32_t other = 0;
    ty_cur = out;
    for (int j = open + 1; j < close; j += toks[j].span + 1, items++) {
        if (items % 2) continue;
        if (toks[j].tag != TOK_SYM) { ty_err(toks[j].line, "a case clause key is a 'tag or '_, not this. Branch on other values with if."); return; }
        uint32_t tg = toks[j].as.sym;
        if (wild) { ty_err(toks[j].line, "the '_ clause takes every tag the others do not name, so it comes last."); return; }
        if (tg == S_WILD) { wild = j; continue; }
        if (ty_clause_mark[tg] == stamp) { ty_err(toks[j].line, "this case has two clauses for '%s.", sym_name(tg)); return; }
        ty_clause_mark[tg] = stamp; tags++;
        if (tg == S_OK) okc = 1; else if (tg == S_NO) noc = 1; else { res = 0; if (!other) other = tg; }
    }
    if (items % 2) { ty_err(line, "case clauses come in pairs, a key and a body, but this list has %d items.", items); return; }
    if (!tags) { ty_err(line, "case needs a clause for at least one tag."); return; }
    if (other && (okc || noc)) { ty_err(line, "a result is tagged only 'ok or 'no, so this case cannot also name '%s. Match '%s in another case, or use '_.", sym_name(other), sym_name(other)); return; }
    /* dead: a clause key whose clause never runs, as a token index */
    uint32_t unnamed = 0; int dead = 0;
    if (res) { a = ty_new(K_VAR, 0, 0, 0); b = ty_new(K_VAR, 0, 0, 0);
        if (ty_unify(ty_new(K_RES, a, b, 0), s)) { ty_err(line, "case with 'ok/'no clauses takes a result, but this value is not one: %s.", ty_why); return; }
        if (!wild && !(okc && noc)) unnamed = okc ? S_NO : S_OK;
        if (wild && okc && noc) dead = wild; }
    else if (!wild) {
        int set = ty_new(K_TNIL, 0, 0, 0), tagged = ty[ty_find(s)].kind == K_TAG;
        for (int j = open + 1; j < close; j += toks[j].span + 1, j += toks[j].span + 1) { set = ty_new(K_TEXT, 0, 0, set); ty[set].sym = toks[j].as.sym; }
        if (ty_unify(ty_new(K_TAG, set, 0, 0), s)) { ty_err(line, "case cannot take this value: %s.%s", ty_why, tagged ? " A case without a last '_ clause names every tag the value may carry, and only those; a last '_ clause takes the rest." : ""); return; }
    }
    else {
        /* A clause's payload type is its tag's everywhere, so with '_ the value may carry any tags. On a closed
           set, a clause for a tag the set lacks never runs, and neither does '_ once every tag has a clause. */
        int v = ty_find(s);
        if (ty[v].kind == K_TAG && ty[ty_tail(ty[v].a)].kind == K_TNIL) {
            int missing = 0, set_stamp = ++ty_clause_stamp;
            for (int x = ty_find(ty[v].a), hops = -ty_n; ty[x].kind == K_TEXT; x = ty_rest(x, &hops)) { missing += ty_clause_mark[ty[x].sym] != stamp; ty_clause_mark[ty[x].sym] = set_stamp; }
            for (int j = open + 1; !dead && j < close; j += toks[j].span + 1, j += toks[j].span + 1) if (j != wild && ty_clause_mark[toks[j].as.sym] != set_stamp) dead = j;
            if (!dead && !missing) dead = wild; }
        else if (ty_unify(ty_new(K_TAG, ty_new(K_TVAR, 0, 0, 0), 0, 0), s)) { ty_err(line, "case cannot take this value: %s.", ty_why); return; }
    }
    if (unnamed) { ty_err(line, "the value may be tagged '%s, which no clause names. Add a clause for '%s, or a last '_ clause.", sym_name(unnamed), sym_name(unnamed)); return; }
    if (dead == wild && dead) { ty_err(toks[dead].line, "the clauses name every tag the value may carry, so the '_ clause never runs. Delete it."); return; }
    if (dead) { ty_err(toks[dead].line, "the value is never tagged '%s, so the clause for '%s never runs. Delete it.", sym_name(toks[dead].as.sym), sym_name(toks[dead].as.sym)); return; }
    for (int j = open + 1; j < close; ) {
        int key = j; j += toks[j].span + 1;
        if (j >= close || toks[j].tag != TOK_LPAREN) { ty_err(toks[key].line, "each case clause is a key and a body in parentheses."); break; }
        int body = ty_find(ty_body(toks, j, j + toks[j].span, 0)); j += toks[j].span + 1;
        uint32_t tg = toks[key].as.sym; int p = tg == S_WILD ? s : res ? (tg == S_OK ? a : b) : ty_tag_payload(tg);
        ty_print_count = 0;
        if (ty_unify(ty[body].a, ty_new(K_SCONS, p, rest, 0))) { char ps[256]; ty_show(ps, sizeof ps, p, 0);
            if (tg == S_WILD) ty_err(toks[key].line, "the '_ clause gets the tagged value, %s, but its body cannot take it: %s.", ps, ty_why);
            else ty_err(toks[key].line, "the clause for '%s gets its payload, %s, but its body cannot take it: %s.", sym_name(tg), ps, ty_why); }
        else if (ty_unify(ty[body].b, out)) ty_err(toks[key].line, "the clause for '%s does not leave what the other clauses leave: %s.", sym_name(tg), ty_why);
    }
}
/* Values collected from a stack type, for literals and lend. Nothing between filling and
   reading it collects again. */
static int *ty_items, ty_items_cap;
static void ty_item(int k, int t) {
    if (k >= ty_items_cap) { ty_items_cap = ty_items_cap ? 2*ty_items_cap : 1024; ty_items = realloc(ty_items, (size_t)ty_items_cap * sizeof(int)); if (!ty_items) die("type checker: out of memory for %d values", ty_items_cap); }
    ty_items[k] = t;
}
/* `box (body) lend`: the body runs on a copy of the contents and may not take what lies below it; the box
   goes back under what the body leaves. */
static void ty_lend(int line) {
    ty_runs("lend", line); ty_at_word = "lend";
    int e0 = ty_errors, body = ty_pop(), bx = ty_pop(), rest = ty_cur, a = ty_new(K_VAR, 0, 0, 0), below = ty_new(K_SVAR, 0, 0, 0), out = ty_new(K_SVAR, 0, 0, 0);
    if (ty_errors > e0) { ty_push(ty_new(K_VAR, 0, 0, 0)); ty_push(ty_new(K_VAR, 0, 0, 0)); return; }
    ty[below].sealed = 1;
    if (ty_unify(bx, ty_new(K_BOX, a, 0, 0))) { ty_err(line, "lend takes a box: %s.", ty_why); return; }
    if (ty_unify(body, ty_new(K_FN, ty_new(K_SCONS, a, below, 0), out, 0))) { ty_err(line, "lend's body does not fit the box's contents: %s.", ty_why); return; }
    int n = 0, s = ty_find(out);
    for (int hops = -ty_n; s != ty_find(below) && ty[s].kind == K_SCONS; s = ty_rest(s, &hops)) ty_item(n++, ty[s].a);
    if (s != ty_find(below)) { ty_err(line, "lend's body may not take values below the box's contents."); return; }
    ty_cur = rest; ty_push(bx); for (int k = n - 1; k >= 0; k--) ty_push(ty_items[k]);
}
static void ty_range(Token *toks, int i, int end) {
    for (; i < end; i++) {
        Token *t = &toks[i]; int line = t->line; current_loc = LOC_PACK(t->fid, t->line, t->col);
        switch (t->tag) {
        case TOK_INT: ty_push(ty_new(K_INT, 0, 0, 0)); break;
        case TOK_FLOAT: ty_push(ty_new(K_FLOAT, 0, 0, 0)); break;
        case TOK_STRING: ty_push(ty_new(K_LIST, ty_new(K_INT, 0, 0, 0), 0, 0)); break;
        case TOK_SYM: ty_push(ty_new(K_SYM, ty_new(K_LVAR, ty_sym(K_LSYM, t->as.sym), 0, 0), 0, 0)); break;
        case TOK_LPAREN: {
            int close = i + t->span, nm = close + 1, sig_open = 0, sig_close = 0;
            if (nm < end && toks[nm].tag == TOK_LBRACKET && nm + toks[nm].span + 1 < end && ty_word_is(&toks[nm + toks[nm].span + 1], "effect")) {
                sig_open = nm; sig_close = nm + toks[nm].span; nm = sig_close + 2; }
            if (nm + 1 < end && toks[nm].tag == TOK_SYM && toks[nm+1].tag == TOK_WORD && toks[nm+1].as.sym == S_LET) {
                ty_define(toks, i, close, sig_open, sig_close, toks[nm].as.sym, line); i = nm + 1; break; }
            if (sig_open) { ty_level++; int rmark, want = ty_rigid(ty_scheme_slots(toks, sig_open, sig_close), &rmark), bt = ty_body(toks, i, close, ty[want].a);
                if (ty_unify(want, bt)) ty_err(line, "this body does not have its declared type: %s.", ty_why);
                ty_unrigid(rmark); ty_level--; ty_occurs(-1, bt, ty_level); ty_push(bt); i = sig_close + 1; break; }
            ty_push(ty_body(toks, i, close, 0)); i = close; break;
        }
        case TOK_LBRACKET: {
            int close = i + t->span;
            if (close + 1 < end && ty_word_is(&toks[close+1], "effect")) {
                /* `'name [sig] effect`: the word is declared before its body is written */
                if (i == 0 || toks[i-1].tag != TOK_SYM) { ty_err(line, "a signature [...] effect needs a body before it or a 'name before it."); i = close + 1; break; }
                ty_pop(); ty_redefined(toks[i-1].as.sym, line, 0);
                if (ty_literal) ty_err(line, "a [...] or {...} literal is built when the program is read, so it cannot declare words.");
                tyb_push(toks[i-1].as.sym, ty_scheme_slots(toks, i, close), 1, line); tyb[tyb_n-1].declared = 1;
                ty_pending_add(ty_body_depth, 1);
                i = close + 1; break;
            }
            /* a list literal, built when the program is read: its elements have one type */
            int saved = ty_cur; ty_cur = ty_new(K_SNIL, 0, 0, 0); ty_literal++;
            c_stack_check("while checking nested literals");
            ty_range(toks, i + 1, close); ty_literal--;
            int el = ty_new(K_VAR, 0, 0, 0); ty[el].prot = P_COPY;
            for (int s = ty_find(ty_cur), k = 0, hops = -ty_n; ty[s].kind == K_SCONS; s = ty_rest(s, &hops), k++)
                if (ty_need(ty[s].a, P_COPY)) { ty_err(line, "a list literal holds only values that can be copied: %s.", ty_why); break; }
                else if (ty_unify(el, ty[s].a)) { ty_err(line, "a list holds values of one type, but element %d from the end is not like the others: %s.", k + 1, ty_why); break; }
            ty_cur = saved; ty_push(ty_new(K_LIST, el, 0, 0)); i = close; break;
        }
        case TOK_LBRACE: {
            int close = i + t->span;
            if (close + 1 < end && toks[close+1].tag == TOK_WORD && toks[close+1].as.sym == S_CASE) { ty_case(toks, i, close, line); i = close + 1; break; }
            /* a {...} literal, built when the program is read: a record, each value written after its 'key */
            int saved = ty_cur; ty_cur = ty_new(K_SNIL, 0, 0, 0); ty_literal++;
            c_stack_check("while checking nested literals");
            ty_range(toks, i + 1, close); ty_literal--;
            int n = 0;
            for (int s = ty_find(ty_cur), hops = -ty_n; ty[s].kind == K_SCONS; s = ty_rest(s, &hops)) ty_item(n++, ty[s].a);
            ty_cur = saved;
            for (int k = 0; k < n; k++) if (ty_need(ty_items[k], P_COPY)) { ty_err(line, "a {...} literal's values are copied each time it runs, so each is copyable: %s.", ty_why); break; }
            int rec = n % 2 == 0;
            for (int k = 1; rec && k < n; k += 2) if (ty[ty_find(ty_items[k])].kind != K_SYM) rec = 0;
            /* odd, with a symbol in every key place and a value that is not one: a record missing its last value */
            if (n % 2) { int keys = 1, vals = 0;
                for (int k = 0; k < n; k += 2) keys &= ty[ty_find(ty_items[k])].kind == K_SYM;
                for (int k = 1; k < n; k += 2) vals |= ty[ty_find(ty_items[k])].kind != K_SYM;
                int l = ty_sym_label(ty_items[0]);
                if (keys && vals && l) { ty_err(line, "record literal: key '%s has no value. Give it one, as in {'%s 0}.", sym_name(ty[l].sym), sym_name(ty[l].sym)); rec = -1; } }
            if (rec == 0) ty_err(line, "a {...} literal is a record, so each value follows its 'key, as in {'x 1 'y 2}. For code that pushes values, write a body: (1 2).");
            if (rec != 1) { ty_push(ty_new(K_VAR, 0, 0, 0)); i = close; break; }
            { int row = ty_new(K_RNIL, 0, 0, 0);
                for (int k = n - 1; k >= 1; k -= 2) {
                    int l = ty_sym_label(ty_items[k]), twice = 0;
                    if (!l) { ty_err(line, "this {...} literal pairs each value with a symbol, so it is a record, but key %d is a symbol this literal computes. Write each key in the literal, as in {'name 1}.", (n - k) / 2 + 1); continue; }
                    for (int m = 1; m < k; m += 2) { int lm = ty_sym_label(ty_items[m]); if (lm && ty[lm].sym == ty[l].sym) twice = 1; }
                    if (twice) { ty_err(line, "this record literal has '%s twice.", sym_name(ty[l].sym)); continue; }
                    row = ty_new(K_REXT, l, ty_new(K_PRE, ty_items[k-1], 0, 0), row);
                }
                ty_push(ty_new(K_REC, row, 0, 0)); i = close; break; }
        }
        case TOK_WORD: {
            uint32_t w = t->as.sym; ty_at_word = sym_name(w);
            if (w == S_LET) {
                if (i == 0 || toks[i-1].tag != TOK_SYM) { ty_err(line, "let needs its name written before it, as in `42 'x let`."); break; }
                if (ty_literal) ty_err(line, "a [...] or {...} literal is built when the program is read, so it cannot bind names: a let inside it would bind '%s for the whole program. Bind it outside the literal.", sym_name(toks[i-1].as.sym));
                ty_pop(); int v = ty_pop();
                if (ty_need(v, P_COPY)) ty_err(line, "'%s' cannot be let-bound: %s. Keep it on the stack.", sym_name(toks[i-1].as.sym), ty_why);
                ty_redefined(toks[i-1].as.sym, line, 0); tyb_push(toks[i-1].as.sym, v, 0, line); break;
            }
            if (w == S_TAG) {
                if (i == 0 || toks[i-1].tag != TOK_SYM) { ty_err(line, "tag needs its tag written before it, as in `5 'n tag`."); break; }
                ty_pop(); int p = ty_pop(); uint32_t tg = toks[i-1].as.sym;
                if (tg == S_OK) ty_push(ty_new(K_RES, p, ty_new(K_VAR, 0, 0, 0), 0));
                else if (tg == S_NO) ty_push(ty_new(K_RES, ty_new(K_VAR, 0, 0, 0), p, 0));
                else if (tg == S_WILD) { ty_err(line, "'_ is the key of case's catch-all clause, so it cannot be a tag."); ty_push(ty_new(K_VAR, 0, 0, 0)); }
                else { if (ty_unify(ty_tag_payload(tg), p)) ty_err(line, "'%s is tagged onto a value unlike its payload elsewhere: %s.", sym_name(tg), ty_why);
                    int row = ty_new(K_TEXT, 0, 0, ty_new(K_TVAR, 0, 0, 0)); ty[row].sym = tg; ty_push(ty_new(K_TAG, row, 0, 0)); }
                break;
            }
            if (w == S_NTH) {
                int ix = ty_pop(), nm = ty_find(ty_pop()), l = ty_sym_label(nm);
                if (ty_unify(ix, ty_new(K_INT, 0, 0, 0))) ty_err(line, "nth takes an int index: %s.", ty_why);
                if (!l) { ty_err(line, "nth reads a list by its name written before the index, as in `'xs i nth`."); ty_push(ty_new(K_VAR, 0, 0, 0)); break; }
                ty[ty_label_var(nm)].named = 1;
                int b = tyb_find(ty[l].sym), el = ty_new(K_VAR, 0, 0, 0);
                if (b >= 0 && ty_literal && b >= tyb_prelude) ty_err(line, "'%s' is bound when the program runs, but a [...] or {...} literal is built when it is read.", sym_name(ty[l].sym));
                if (b < 0 || tyb[b].word || !tyb_visible(b)) { ty_err(line, "nth reads a list bound to '%s, but '%s is not a bound list here.", sym_name(ty[l].sym), sym_name(ty[l].sym)); }
                else if (ty_unify(tyb[b].ty, ty_new(K_LIST, el, 0, 0))) ty_err(line, "nth reads a list, but '%s is not one: %s.", sym_name(ty[l].sym), ty_why);
                { int r = ty_new(K_SVAR, 0, 0, 0); ty_push(ty_new(K_RES, el, ty_new(K_FN, r, r, 0), 0)); } break;
            }
            if (w == S_CASE) { ty_err(line, "case needs its clauses written right before it, as in `x {'ok (…) 'no (…)} case`."); ty_pop(); ty_pop(); ty_push(ty_new(K_VAR, 0, 0, 0)); break; }
            if (w == S_LEND) { ty_lend(line); break; }
            /* eq and neq on two symbols compare them; they do not make them one type */
            if (w == S_EQ || w == S_NEQ) {
                int st = ty_find(ty_cur), nx = ty[st].kind == K_SCONS ? ty_find(ty[st].b) : 0;
                if (nx && ty[nx].kind == K_SCONS && ty[ty_find(ty[st].a)].kind == K_SYM && ty[ty_find(ty[nx].a)].kind == K_SYM) {
                    ty_pop(); ty_pop(); ty_push(ty_new(K_INT, 0, 0, 0)); break; }
            }
            if (w == S_AT || w == S_INTO || w == S_EDIT) {
                /* the key is the symbol written right before the word (before its body, for edit) */
                int kt = i - 1;
                if (w == S_EDIT && kt >= 0 && toks[kt].tag == TOK_RPAREN) kt += toks[kt].span - 1;
                if (kt < 0 || toks[kt].tag != TOK_SYM) {
                    if (w == S_EDIT) ty_err(line, "'edit' needs its key written right before its body, as in `'name (1 plus) edit`. For keys that are data, use a dict: `d key of`.");
                    else ty_err(line, "'%s' needs its key written right before it, as in `'name %s`. For keys that are data, use a dict: `d key of`.", sym_name(w), sym_name(w));
                    ty_cur = ty_new(K_SVAR, 0, 0, 0); break;
                }
                int key = ty_sym(K_LSYM, toks[kt].as.sym), s0 = ty_new(K_SVAR, 0, 0, 0), r = ty_new(K_RVAR, 0, 0, 0), v = ty_new(K_VAR, 0, 0, 0), in, out;
                ty[v].prot = P_COPY;
                int sym = ty_new(K_SYM, ty_new(K_LVAR, 0, 0, 0), 0, 0);
                if (w == S_AT) {
                    in = ty_new(K_SCONS, sym, ty_new(K_SCONS, ty_new(K_REC, ty_new(K_REXT, key, ty_new(K_PRE, v, 0, 0), r), 0, 0), s0, 0), 0);
                    out = ty_new(K_SCONS, v, s0, 0);
                } else if (w == S_INTO) {
                    int old = ty_new(K_REC, ty_new(K_REXT, key, ty_new(K_VAR, 0, 0, 0), r), 0, 0);
                    in = ty_new(K_SCONS, sym, ty_new(K_SCONS, v, ty_new(K_SCONS, old, s0, 0), 0), 0);
                    out = ty_new(K_SCONS, ty_new(K_REC, ty_new(K_REXT, key, ty_new(K_PRE, v, 0, 0), r), 0, 0), s0, 0);
                } else {
                    int u = ty_new(K_VAR, 0, 0, 0), below = ty_new(K_SVAR, 0, 0, 0); ty[below].sealed = 1; ty[u].prot = P_COPY;
                    int body = ty_new(K_FN, ty_new(K_SCONS, v, below, 0), ty_new(K_SCONS, u, below, 0), 0);
                    in = ty_new(K_SCONS, body, ty_new(K_SCONS, sym, ty_new(K_SCONS, ty_new(K_REC, ty_new(K_REXT, key, ty_new(K_PRE, v, 0, 0), r), 0, 0), s0, 0), 0), 0);
                    out = ty_new(K_SCONS, ty_new(K_REC, ty_new(K_REXT, key, ty_new(K_PRE, u, 0, 0), r), 0, 0), s0, 0);
                }
                ty_apply(ty_new(K_FN, in, out, 0), sym_name(w), line, 0); break;
            }
            if (w == S_ON) {
                int h = ty_pop(), ev = ty_find(ty_pop()), l = ty_sym_label(ev);
                if (!l) { ty_err(line, "on needs its event written before the handler, as in `'tick (…) on`."); break; }
                const char *en = sym_name(ty[l].sym);
                if (strcmp(en, "tick") && strcmp(en, "keydown") && strcmp(en, "keyup") && strcmp(en, "mousedown") && strcmp(en, "mouseup") && strcmp(en, "mousemove")) {
                    ty_err(line, "on has no event '%s. The events are 'tick 'keydown 'keyup 'mousedown 'mouseup 'mousemove.", en); break; }
                if (ty_body_depth) { ty_err(line, "on registers a handler for the whole program, so it runs at the top level, not inside a body."); break; }
                if (ty_shown) { ty_err(line, "this handler is registered after show starts the event loop, so it never runs. Register it before show."); break; }
                if (ty_on_n == 16) die("type checker: more than 16 'on' handlers");
                ty_on[ty_on_n] = h; ty_on_line[ty_on_n] = line; ty_on_mouse[ty_on_n++] = strncmp(en, "mouse", 5) == 0; break;
            }
            if (w == S_SHOW) {
                /* Every handler and render run on the stack below show: a handler takes the event's ints
                   and leaves the stack as it found it; render takes a copy of the top value. show never returns. */
                if (ty_body_depth) ty_err(line, "show starts the event loop and never returns, so it runs at the top level, after every handler.");
                ty_runs("show", line); ty_shown = 1;
                int render = ty_pop(), below = ty_cur, top = ty_pop(); ty_cur = below;
                for (int x = ty_find(below), hops = -ty_n; ty[x].kind == K_SCONS; x = ty_rest(x, &hops))
                    if (ty_need(ty[x].a, P_COPY)) { ty_err(line, "show ends the program when its window closes, so a value it leaves on the stack is never freed: %s.", ty_why); break; }
                for (int k = 0; k < ty_on_n; k++) {
                    int in = ty_new(K_SCONS, ty_new(K_INT, 0, 0, 0), below, 0);
                    if (ty_on_mouse[k]) in = ty_new(K_SCONS, ty_new(K_INT, 0, 0, 0), in, 0);
                    if (ty_unify(ty_on[k], ty_new(K_FN, in, below, 0))) ty_err(ty_on_line[k], "this handler must take the event's %s and leave the stack below show as it was: %s.", ty_on_mouse[k] ? "x and y" : "int", ty_why);
                }
                if (ty_unify(render, ty_new(K_FN, ty_new(K_SCONS, top, below, 0), below, 0))) ty_err(line, "show's render body takes a copy of the top value and leaves the stack as it was: %s.", ty_why);
                ty_cur = ty_new(K_SVAR, 0, 0, 0); break;
            }
            /* A field may hold a result for must to unwrap; anything else on top means must was written for at or edit. */
            if (w == S_MUST && i > 0 && toks[i-1].tag == TOK_WORD && (toks[i-1].as.sym == S_AT || toks[i-1].as.sym == S_EDIT)) {
                int st = ty_find(ty_cur);
                if (ty[st].kind == K_SCONS && ty_unify(ty_new(K_RES, ty_new(K_VAR, 0, 0, 0), ty_new(K_VAR, 0, 0, 0), 0), ty[st].a)) {
                    ty_err(line, "%s never fails: the checker proves the record has the key, so it gives %s, and %s. Drop must.", sym_name(toks[i-1].as.sym), toks[i-1].as.sym == S_AT ? "the value" : "the record", ty_why);
                    break; } }
            int b = tyb_find(w);
            if (b >= 0) {
                if (!tyb_visible(b)) { ty_err(line, "'%s' is bound in the word around this literal, but a body inside a [...] or {...} literal is built when the program is read, so it sees only top-level names and its own.", sym_name(w)); ty_cur = ty_new(K_SVAR, 0, 0, 0); break; }
                if (ty_literal && b >= tyb_prelude) ty_err(line, "'%s' is bound when the program runs, but a [...] or {...} literal is built when it is read.", sym_name(w));
                if (tyb[b].word == 2) ty_apply(ty_self_fn(b, line), sym_name(w), line, 1);
                else if (tyb[b].word) ty_apply(tyb[b].ty, sym_name(w), line, b >= tyb_prelude); else ty_push(tyb[b].ty);
                break;
            }
            if (ty_builtin[w]) { ty_apply(ty_builtin[w], sym_name(w), line, 0); break; }
            { int later = 0;
              for (int j = i + 1; j + 1 < ty_tok_end && !later; j++) if (toks[j].tag == TOK_SYM && toks[j].as.sym == w && toks[j+1].tag == TOK_WORD && toks[j+1].as.sym == S_LET) later = toks[j].line;
              if (later) ty_err(line, "unknown word '%s': it is defined later, on line %d. A word used before its definition needs `'%s [sig] effect` before its first use.", sym_name(w), later, sym_name(w));
              else ty_err(line, "unknown word '%s'.", sym_name(w)); }
            break;
        }
        default: break;
        }
    }
}
/* What a primitive's type asks to be copyable: an input it drops or duplicates (it appears a different
   number of times among the outputs, at the top level) and anything a list, dict, box or record holds. */
static void ty_table_copy(int fn, int line) {
    for (int x = ty_find(ty[fn].a), hx = -ty_n; ty[x].kind == K_SCONS; x = ty_rest(x, &hx)) {
        int v = ty_find(ty[x].a), in = 0, out = 0;
        if (ty[v].kind != K_VAR) continue;
        for (int y = ty_find(ty[fn].a), hy = -ty_n; ty[y].kind == K_SCONS; y = ty_rest(y, &hy)) in += ty_find(ty[y].a) == v;
        for (int y = ty_find(ty[fn].b), hy = -ty_n; ty[y].kind == K_SCONS; y = ty_rest(y, &hy)) out += ty_find(ty[y].a) == v;
        if (in != out) ty[v].prot |= P_COPY;
    }
    ty_held_copy(fn, line);
}
/* What a list, dict, box or record holds is copyable, in the table and in signatures alike. */
/* The walk collects what the containers hold, then marks it: marking walks terms too (ty_need), with the
   same work stack and stamps. */
static int *ty_held, ty_held_cap;
static void ty_held_copy(int t, int line) {
    int n = 0, h = 0, stamp = ++ty_stamp; ty_work_push(&n, t);
    while (n) {
        int x = ty_find(ty_work[--n]);
        if (ty_mark[x] == stamp) continue;
        ty_mark[x] = stamp;
        int held = ty[x].kind == K_LIST || ty[x].kind == K_DICT || ty[x].kind == K_BOX ? ty_find(ty[x].a)
                 : ty[x].kind == K_REXT && ty[ty_find(ty[x].b)].kind == K_PRE ? ty_find(ty[ty_find(ty[x].b)].a) : 0;
        if (held) {
            if (h == ty_held_cap) { ty_held_cap = ty_held_cap ? 2*ty_held_cap : 256; ty_held = realloc(ty_held, (size_t)ty_held_cap * sizeof(int)); if (!ty_held) die("type checker: out of memory for %d held types", ty_held_cap); }
            ty_held[h++] = held; }
        if (!ty_isvar(ty[x].kind)) { if (ty[x].a) ty_work_push(&n, ty[x].a); if (ty[x].b) ty_work_push(&n, ty[x].b); if (ty[x].c) ty_work_push(&n, ty[x].c); }
    }
    for (int k = 0; k < h; k++) ty_mark_copy(ty_held[k], line);
}
/* The builtin table: 'name ( ins -> outs ) pairs. */
static void ty_read_table(Token *toks, int n) {
    for (int i = 0; i + 1 < n; ) {
        if (toks[i].tag != TOK_SYM || toks[i+1].tag != TOK_LPAREN) die("type table: expected 'name ( ... -> ... ) at line %d", toks[i].line);
        TyNames nm = {0}; int close = i + 1 + toks[i+1].span;
        ty_level++; int t = ty_parse_fn(toks, i + 1, close, &nm, 0); ty_level--;
        ty_table_copy(t, toks[i].line); ty_generalize(t); ty_builtin[toks[i].as.sym] = t; i = close + 1;
    }
}
static int infer_program(Token *table, int table_n, Token *toks, int count, int user_start) {
    ty_read_table(table, table_n);
    S_LEND = sym_intern("lend"); S_EQ = sym_intern("eq"); S_NEQ = sym_intern("neq");
    ty_cur = ty_new(K_SNIL, 0, 0, 0);
    ty_tok_end = count;
    ty_in_prelude = 1; ty_range(toks, 0, user_start); ty_in_prelude = 0; tyb_prelude = tyb_n;
    ty_cur = ty_new(K_SNIL, 0, 0, 0);
    ty_range(toks, user_start, count);
    ty_undefined(tyb_prelude);
    /* after an error the stack's types may be made up, so the end is checked only when nothing failed */
    for (int x = ty_find(ty_cur), hops = -ty_n; !ty_errors && ty[x].kind == K_SCONS; x = ty_rest(x, &hops))
        if (ty_need(ty[x].a, P_COPY)) { ty_err(LOC_LINE(current_loc), "the program ends with a value left on the stack that is never freed: %s.", ty_why); break; }
    return ty_errors;
}
/* ---- PRIMITIVES ---- */
/* The aux stack holds what must step aside while code runs: bodies being
   executed, dip's saved value, case's clauses, each/fold's input. It never
   moves, so a body executes in place from it. eval_body releases whatever a
   primitive staged there when that primitive returns. */
static Value aux[STACK_MAX];
static int asp = 0;
static Value *aux_reserve(int n) {
    if (asp + n > STACK_MAX) die("aux stack overflow: %d of %d slots in use, %d more needed. It holds the bodies and values of every primitive still running, so deep nesting of dip/each/fold/case over large values fills it.", asp, STACK_MAX, n);
    Value *p = &aux[asp]; asp += n; return p;
}
static Value *aux_take(int n) {
    if (n > sp) die("stack underflow: need %d slots, have %d", n, sp);
    Value *p = aux_reserve(n); VCPY(p, &stack[sp-n], n); sp -= n; return p;
}
/* Element starts of a compound, staged on aux; NULL when every element is one slot. */
static int *elem_starts(Value *data, int slots, int len) {
    if (slots == len + 1) return NULL;
    int *st = (int*)aux_reserve((int)(((size_t)(len+1)*sizeof(int)+sizeof(Value)-1)/sizeof(Value)));
    st[len] = slots-1; for (int i = len-1; i >= 0; i--) st[i] = st[i+1] - val_slots(data[st[i+1]-1]);
    return st;
}
#define POP_VAL(name) \
    if (sp <= 0) die("stack underflow"); \
    Value name##_top = stack[sp-1]; int name##_s = val_slots(name##_top); Value *name##_buf = aux_take(name##_s)
/* The bodies a running primitive took, by aux index of their header: eval_run frees them when the
   primitive returns. */
static int *staged, staged_n, staged_cap;
static inline void stage_body(int at) {
    if (staged_n == staged_cap) { staged_cap = staged_cap ? 2*staged_cap : 256; staged = realloc(staged, (size_t)staged_cap*sizeof(int)); if (!staged) die("out of memory: %d staged bodies", staged_cap); }
    staged[staged_n++] = at;
}
#define POP_BODY(name, label) if (sp<=0) die(label ": stack underflow"); if (stack[sp-1].tag != VAL_TUPLE) die(label ": expected tuple, got %s", valtag_name(stack[sp-1].tag)); POP_VAL(name); stage_body(asp-1)
static void deep_copy_values(Value *dst, const Value *src, int slots);
static void prim_dup(Frame *e) { (void)e; if (sp<=0) die("dup: stack underflow"); Value top=stack[sp-1]; if(top.tag<=VAL_XT){spush(top);return;} int s=val_slots(top); stack_room(s,"dup"); deep_copy_values(&stack[sp],&stack[sp-s],s); sp+=s; }
static void prim_drop(Frame *e) { (void)e; if (sp<=0) die("drop: stack underflow"); Value top=stack[sp-1]; if(top.tag<=VAL_XT){sp--;return;} int s=val_slots(top); deep_free_values(&stack[sp-s],s); sp-=s; }
static void slot_reverse(Value *a,int n){for(int i=0,j=n-1;i<j;i++,j--){Value t=a[i];a[i]=a[j];a[j]=t;}}
/* Exchange the adjacent runs [base, base+n1) and [base+n1, base+n1+n2) in place. */
static void swap_blocks(int base,int n1,int n2){slot_reverse(&stack[base],n1+n2);slot_reverse(&stack[base],n2);slot_reverse(&stack[base+n2],n1);}
/* Where the value ending at slot `end` starts. */
static int val_start(int end, const char *who) {
    int b = end > 0 ? end - val_slots(stack[end-1]) : -1;
    if (b < 0) die("%s: stack underflow", who);
    return b;
}
static void prim_swap(Frame *e) {
    (void)e;
    if(sp>=2&&stack[sp-1].tag<=VAL_XT&&stack[sp-2].tag<=VAL_XT){Value t=stack[sp-1];stack[sp-1]=stack[sp-2];stack[sp-2]=t;return;}
    int b=val_start(sp,"swap"),a=val_start(b,"swap"); swap_blocks(a,b-a,sp-b);
}
static void prim_over(Frame *e) {
    (void)e; int b=val_start(sp,"over"),a=val_start(b,"over");
    stack_room(b-a,"over");
    deep_copy_values(&stack[sp],&stack[a],b-a); sp+=b-a;
}
/* a b c -- b c a */
static void prim_rot(Frame *e) {
    (void)e; int c=val_start(sp,"rot"),b=val_start(c,"rot"),a=val_start(b,"rot");
    swap_blocks(a,b-a,sp-b);
}
static void prim_dip(Frame *env) {
    if(sp<2)die("dip: need body and value"); POP_BODY(body,"dip");
    POP_VAL(saved); eval_body(body_buf,body_s,env);
    SPUSH(saved_buf,saved_s);
}
static void prim_apply(Frame *env) { POP_BODY(body,"apply"); eval_body(body_buf,body_s,env); }
/* Integer plus/sub/mul wrap at 64 bits: computed in uint64_t, where overflow is defined. */
#define ARITH2(nm,iop,fop) static void prim_##nm(Frame *e){(void)e;Value b=spop(),a=spop(); \
    if(a.tag==VAL_INT&&b.tag==VAL_INT) spush(val_int((int64_t)((uint64_t)a.as.i iop (uint64_t)b.as.i))); \
    else if(a.tag==VAL_FLOAT&&b.tag==VAL_FLOAT) spush(val_float(a.as.f fop b.as.f)); \
    else die(#nm ": type mismatch, got %s and %s", valtag_name(a.tag), valtag_name(b.tag));}
ARITH2(plus,+,+) ARITH2(sub,-,-) ARITH2(mul,*,*)
static void int_div_check(const char *who,int64_t a,int64_t b){
    if(b==0) die("%s: division by zero",who);
    if(a==INT64_MIN&&b==-1) die("%s: %lld / -1 overflows a 64-bit int",who,(long long)a);
}
static void prim_div(Frame *e) { (void)e; Value b=spop(),a=spop();
    if(a.tag==VAL_INT&&b.tag==VAL_INT){int_div_check("div",a.as.i,b.as.i);spush(val_int(a.as.i/b.as.i));}
    else if(a.tag==VAL_FLOAT&&b.tag==VAL_FLOAT)spush(val_float(a.as.f/b.as.f));
    else die("div: type mismatch, got %s and %s", valtag_name(a.tag), valtag_name(b.tag)); }
static void prim_mod(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();int_div_check("mod",a,b);spush(val_int(a%b));}
static void prim_divmod(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();int_div_check("divmod",a,b);spush(val_int(a%b));spush(val_int(a/b));}
/* Result takes the sign of m. */
static void prim_wrap(Frame *e){(void)e;int64_t m=pop_int(),v=pop_int();if(m==0)die("wrap: modulus must be non-zero");
    int64_t r=m==-1?0:v%m; if(r!=0&&((r<0)!=(m<0))) r+=m; spush(val_int(r));}
#define INTOP2(nm,expr) static void prim_##nm(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();spush(val_int(expr));}
#define SHIFT_OK(nm) (b<0||b>63?(die(#nm ": shift count %lld is outside 0-63",(long long)b),0):1)
INTOP2(band,a&b) INTOP2(bor,a|b) INTOP2(bxor,a^b) INTOP2(shl,SHIFT_OK(shl)?(int64_t)((uint64_t)a<<b):0) INTOP2(shr,SHIFT_OK(shr)?(int64_t)((uint64_t)a>>b):0)
INTOP2(and,(a&&b)?1:0) INTOP2(or,(a||b)?1:0)
static void prim_bnot(Frame *e){(void)e;int64_t a=pop_int();spush(val_int(~a));}
#define CMP2(nm,expr) static void prim_##nm(Frame *e){(void)e;int b=val_start(sp,#nm),a=val_start(b,#nm),r=(expr);deep_free_values(&stack[a],sp-a);sp=a;spush(val_int(r?1:0));}
CMP2(eq, val_equal(&stack[a],b-a,&stack[b],sp-b))
CMP2(lt, val_less(&stack[a],b-a,&stack[b],sp-b))
static void prim_print(Frame *e){(void)e;if(sp<=0)die("print: stack underflow");Value top=stack[sp-1];int s=val_slots(top);val_print(&stack[sp-s],s,stdout);printf("\n");deep_free_values(&stack[sp-s],s);sp-=s;
    if(ferror(stdout)) die("print: cannot write to stdout: %s", strerror(errno));}
/* A failed write to stdout must not exit 0. */
static void stdout_check(void){ if(fflush(stdout)||ferror(stdout)){ fprintf(stderr,"slap: cannot write to stdout: %s\n",strerror(errno)); _exit(1); } }
static void prim_assert(Frame *e){(void)e;if(!pop_int())die("assertion failed: expected a nonzero int, got 0");}
/* splitmix64 (Steele, Lea & Flood 2014): 64-bit output, where rand() stops at RAND_MAX. */
static uint64_t rng_state;
static uint64_t rng_next(void){ uint64_t z=(rng_state+=0x9E3779B97F4A7C15ull); z=(z^(z>>30))*0xBF58476D1CE4E5B9ull; z=(z^(z>>27))*0x94D049BB133111EBull; return z^(z>>31); }
static void prim_random(Frame *e){(void)e;int64_t max=pop_int();if(max<1)die("random: max must be at least 1, got %lld",(long long)max);
    uint64_t m=(uint64_t)max,lim=UINT64_MAX-UINT64_MAX%m,r; do r=rng_next(); while(r>=lim); spush(val_int((int64_t)(r%m)));}
static void push_ok(void) { spush(val_compound(VAL_TAGGED,S_OK,val_slots(stack[sp-1])+1)); }
static void push_no(void) { spush(val_compound(VAL_TAGGED,S_NO,val_slots(stack[sp-1])+1)); }
static void push_none(void) { spush(val_compound(VAL_TUPLE,0,1)); spush(val_compound(VAL_TAGGED,S_NO,2)); }
static void prim_if(Frame *env) {
    POP_BODY(el,"if"); POP_BODY(then,"if");
    Value cond=spop(); if(cond.tag!=VAL_INT) die("if: condition must be int, got %s",valtag_name(cond.tag));
    if(cond.as.i) eval_body(then_buf,then_s,env); else eval_body(el_buf,el_s,env);
}
/* Text of a value for a message that dies. */
static char *val_text(Value *buf, ElemRef r) {
    char *txt = NULL; size_t n = 0; FILE *f = open_memstream(&txt, &n);
    if (!f) die("out of memory formatting a value for an error message");
    val_print(&buf[r.base], r.slots, f); fclose(f); return txt;
}
/* A clause body must be code: a value would land above the payload instead of replacing it. */
static void case_body_check(Value *buf, ElemRef key, ElemRef body) {
    if (buf[body.base+body.slots-1].tag == VAL_TUPLE) return;
    die("case: the clause for %s is %s, not a body. A clause runs code on the payload: write (drop %s) instead",
        val_text(buf, key), valtag_name(buf[body.base+body.slots-1].tag), val_text(buf, body));
}
/* The clause list is a record literal, built when the program is read; its bodies are code at the case,
   so they run in the frame that runs case. A clause runs on its tag's payload, already in place under the
   tagged value's header; '_ runs on the tagged value. */
static void prim_case(Frame *env) {
    if (sp <= 0 || stack[sp-1].tag != VAL_RECORD) die("case: expected a record of clauses, got %s", sp > 0 ? valtag_name(stack[sp-1].tag) : "nothing");
    POP_VAL(clauses); stage_body(asp-1); int clauses_len=(int)clauses_top.as.compound.len, found=0;
    if (sp <= 0 || stack[sp-1].tag != VAL_TAGGED) die("case: expected a tagged value, got %s", sp > 0 ? valtag_name(stack[sp-1].tag) : "nothing");
    uint32_t tag_sym=stack[sp-1].as.compound.len;
    ElemRef br = record_field(clauses_buf,clauses_s,clauses_len,tag_sym,&found);
    if (found) sp--; else br = record_field(clauses_buf,clauses_s,clauses_len,S_WILD,&found);
    if (!found) die("case: no clause names '%s, but the checker proved one does. This is a bug in slap's checker: please report it with this program.", sym_name(tag_sym));
    case_body_check(clauses_buf,(ElemRef){br.base-1,1},br); eval_in(&clauses_buf[br.base],br.slots,env);
}
static void prim_tag(Frame *e) {
    (void)e;
    uint32_t tag_sym=pop_sym();
    if(sp<=0) die("tag: need a payload value");
    Value payload_top=stack[sp-1];
    int payload_s=val_slots(payload_top);
    spush(val_compound(VAL_TAGGED,tag_sym,payload_s+1));
}
static void prim_must(Frame *e) {
    (void)e; Value top=speek();
    if(top.tag!=VAL_TAGGED) die("must: expected tagged value, got %s", valtag_name(top.tag));
    if(top.as.compound.len==S_OK) { sp--; return; }
    die("must: expected 'ok tagged, got '%s tagged (its payload is on top of the stack below)", sym_name(top.as.compound.len));
}
/* tagged default (body) pthen: on 'ok run body on the payload, else leave default under the tagged, re-tagged 'no. */
static void prim_pthen(Frame *env) {
    POP_BODY(body,"pthen"); POP_VAL(def);
    if(sp<=0) die("pthen: stack underflow");
    Value top=stack[sp-1];
    if(top.tag!=VAL_TAGGED) die("pthen: expected tagged, got %s",valtag_name(top.tag));
    if(top.as.compound.len==S_OK){ deep_free_values(def_buf,def_s); sp--; eval_body(body_buf,body_s,env); return; }
    stack[sp-1].as.compound.len=S_NO;
    int ts=val_slots(top); SPUSH(def_buf,def_s); swap_blocks(sp-def_s-ts,ts,def_s);
}
static void prim_while(Frame *env) {
    POP_BODY(body,"while"); POP_BODY(pred,"while");
    for(;;){eval_body(pred_buf,pred_s,env);if(!pop_int())break;eval_body(body_buf,body_s,env);}
}
static void prim_itof(Frame *e){(void)e;spush(val_float((double)pop_int()));}
static void prim_ftoi(Frame *e){(void)e;double f=pop_float();
    if(!(f>=-9223372036854775808.0&&f<9223372036854775808.0)) die("ftoi: %g does not fit in an int",f);
    spush(val_int((int64_t)f));}
#define FLOAT1(nm,fn) static void prim_##nm(Frame *e){(void)e;spush(val_float(fn(pop_float())));}
FLOAT1(fsqrt,sqrt)
FLOAT1(ffloor,floor) FLOAT1(fround,round) FLOAT1(fexp,exp) FLOAT1(flog,log)
#define FLOAT2(nm,fn) static void prim_##nm(Frame *e){(void)e;double b=pop_float(),a=pop_float();spush(val_float(fn(a,b)));}
FLOAT2(fpow,pow) FLOAT2(fatan2,atan2)
static void dict_data_free(DictData *dd);
#define SEQ_GUARD(v, who) do{ if((v).tag!=VAL_LIST) die(who ": expected list, got %s",valtag_name((v).tag)); }while(0)
static void prim_size(Frame *e) {
    (void)e; Value top=speek(); SEQ_GUARD(top,"len");
    int s=val_slots(top); deep_free_values(&stack[sp-s],s); sp-=s; spush(val_int((int)top.as.compound.len));
}
static void prim_push_op(Frame *e) {
    (void)e; int vb=val_start(sp,"push"),vs=sp-vb; if(vb<1) die("push: stack underflow");
    Value h=stack[vb-1]; SEQ_GUARD(h,"push");
    memmove(&stack[vb-1],&stack[vb],(size_t)vs*sizeof(Value));
    h.as.compound.len++; h.as.compound.slots+=(uint32_t)vs; h.loc=0; stack[sp-1]=h;
}
#define MUST_PAIR(nm) static void prim_##nm(Frame *e) { prim_##nm##_impl(e,1); } \
    static void prim_##nm##_must(Frame *e) { prim_##nm##_impl(e,0); }
static inline void prim_pop_impl(Frame *e, int tagged) {
    (void)e; Value top=speek(); SEQ_GUARD(top,"pop");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    if(len==0) { if(tagged) push_none(); else die("pop: empty %s",valtag_name(top.tag)); return; }
    ElemRef last=compound_elem(&stack[base],s,len,len-1);
    memmove(&stack[base+last.base+1],&stack[base+last.base],(size_t)last.slots*sizeof(Value));
    top.as.compound.len--; top.as.compound.slots-=(uint32_t)last.slots; top.loc=0; stack[base+last.base]=top;
    if(tagged) push_ok();
}
MUST_PAIR(pop)
static inline void prim_get_impl(Frame *e, int tagged) {
    (void)e; int64_t idx=pop_int(); Value top=speek(); SEQ_GUARD(top,"get");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    ElemRef ref=compound_elem(&stack[base],s,len,idx);
    if(ref.base<0) { deep_free_values(&stack[base],s); sp=base; if(tagged) push_none(); else die("get: index %lld out of bounds (len %d)",(long long)idx,len); return; }
    deep_free_values(&stack[base],ref.base); deep_free_values(&stack[base+ref.base+ref.slots],s-ref.base-ref.slots);
    memmove(&stack[base],&stack[base+ref.base],(size_t)ref.slots*sizeof(Value)); sp=base+ref.slots;
    if(tagged) push_ok();
}
MUST_PAIR(get)
static inline void prim_peek_impl(Frame *e, int tagged) {
    (void)e; int64_t idx=pop_int(); Value top=speek(); SEQ_GUARD(top,"peek");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    if(base<0) die("peek: stack underflow: need %d slots, have %d", s, sp);
    ElemRef ref=compound_elem(&stack[base],s,len,idx);
    if(ref.base<0) { if(tagged) push_none(); else die("peek: index %lld out of bounds (len %d)",(long long)idx,len); return; }
    stack_room(ref.slots,"peek");
    /* A deep copy: the compound keeps its own boxes and dicts. */
    deep_copy_values(&stack[sp],&stack[base+ref.base],ref.slots); sp+=ref.slots;
    if(tagged) push_ok();
}
MUST_PAIR(peek)
/* [old][rest][new] at the top of the stack -> [new][rest]; old is dropped. */
static void replace_run(int old_base,int os,int vs){
    deep_free_values(&stack[old_base],os);
    int rest=sp-vs-old_base-os;
    memmove(&stack[old_base],&stack[old_base+os],(size_t)(rest+vs)*sizeof(Value)); sp-=os;
    swap_blocks(old_base,rest,vs);
}
static inline void prim_set_impl(Frame *e, int tagged) {
    (void)e; POP_VAL(v); int64_t idx=pop_int(); Value top=speek(); SEQ_GUARD(top,"set");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    ElemRef old=compound_elem(&stack[base],s,len,idx);
    if(old.base<0) { deep_free_values(v_buf,v_s); deep_free_values(&stack[base],s); sp=base; if(tagged) push_none(); else die("set: index %lld out of bounds (len %d)",(long long)idx,len); return; }
    if(old.slots==v_s){ deep_free_values(&stack[base+old.base],v_s); VCPY(&stack[base+old.base],v_buf,v_s); if(tagged) push_ok(); return; }
    SPUSH(v_buf,v_s); replace_run(base+old.base,old.slots,v_s);
    stack[sp-1].as.compound.slots=(uint32_t)(s-old.slots+v_s); stack[sp-1].loc=0;
    if(tagged) push_ok();
}
MUST_PAIR(set)
static void prim_concat(Frame *e) {
    (void)e; if(sp<2) die("cat: stack underflow");
    Value t2=stack[sp-1]; SEQ_GUARD(t2,"cat");
    int s2=val_slots(t2),b2=sp-s2; if(b2<1) die("cat: stack underflow");
    Value t1=stack[b2-1]; SEQ_GUARD(t1,"cat");
    memmove(&stack[b2-1],&stack[b2],(size_t)(s2-1)*sizeof(Value)); sp--;
    t2.as.compound.len+=t1.as.compound.len; t2.as.compound.slots=(uint32_t)(val_slots(t1)+s2-1); t2.loc=0;
    stack[sp-1]=t2;
}
static inline void prim_nth_impl(Frame *env, int tagged) {
    int64_t idx=pop_int(); uint32_t sym=pop_sym();
    Lookup lu=frame_lookup(env,sym); if(!lu.bind) die("nth: unknown word: %s",sym_name(sym));
    Value *data=lu.bind->vals; int s=lu.bind->slots;
    Value top=data[s-1]; if(!is_compound(top.tag)) die("nth: expected compound (tuple/list/record) bound to '%s, got %s", sym_name(sym), valtag_name(top.tag));
    int len=(int)top.as.compound.len;
    ElemRef ref=compound_elem(data,s,len,idx);
    if(ref.base<0) { if(tagged) push_none(); else die("nth: index %lld out of bounds (len %d)",(long long)idx,len); return; }
    /* A deep copy: the binding keeps its own boxes and dicts. */
    stack_room(ref.slots,"nth");
    deep_copy_values(&stack[sp],&data[ref.base],ref.slots); sp+=ref.slots; if(tagged) push_ok();
}
MUST_PAIR(nth)
static void prim_slice_n(int take) {
    int64_t n=pop_int(); Value top=speek();
    const char *label=take?"take-n":"drop-n";
    if(top.tag!=VAL_LIST) die("%s: expected list, got %s", label, valtag_name(top.tag));
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    if(base<0) die("%s: stack underflow: need %d slots, have %d", label, s, sp);
    if(n<0) die("%s: count %lld is negative", label, (long long)n);
    if(n>len) n=len;
    int start=take?0:(int)n, end_i=take?(int)n:len;
    int lo = start<len ? compound_elem(&stack[base],s,len,start).base : s-1;
    int hi = end_i<len ? compound_elem(&stack[base],s,len,end_i).base : s-1;
    int nslots = hi-lo;
    deep_free_values(&stack[base],lo); deep_free_values(&stack[base+hi],s-hi);
    if(lo>0&&nslots>0) memmove(&stack[base],&stack[base+lo],(size_t)nslots*sizeof(Value));
    sp=base+nslots;
    spush(val_compound(VAL_LIST,end_i-start,nslots+1));
}
/* Reverse every slot, which also reverses each element; then flip each element
   back. Its header, now first, gives its size. */
static void prim_reverse(Frame *e){
    (void)e; Value top=speek(); if(top.tag!=VAL_LIST) die("reverse: expected list, got %s",valtag_name(top.tag));
    int s=val_slots(top),len=(int)top.as.compound.len,p=sp-s;
    slot_reverse(&stack[p],s-1);
    for(int i=0;i<len;i++){ int n=val_slots(stack[p]); slot_reverse(&stack[p],n); p+=n; }
}
/* a b zip: [a0 b0] [a1 b1] ... as long as the shorter list. One pass over each;
   reading element i by index walks a list whose elements span several slots. */
static void prim_zip(Frame *e){
    (void)e;
    for(int k=1;k<=2;k++) if(sp<k||stack[sp-1].tag!=VAL_LIST) die("zip: expected two lists, got %s", sp>=1?valtag_name(stack[sp-1].tag):"an empty stack");
    POP_VAL(b);
    if(sp<=0||stack[sp-1].tag!=VAL_LIST) die("zip: expected two lists, got %s under the second list", sp>0?valtag_name(stack[sp-1].tag):"nothing");
    POP_VAL(a);
    int la=(int)a_top.as.compound.len,lb=(int)b_top.as.compound.len,n=la<lb?la:lb,p0=sp;
    int *sa=elem_starts(a_buf,a_s,la),*sb=elem_starts(b_buf,b_s,lb);
    for(int i=0;i<n;i++){
        int ao=sa?sa[i]:i,an=sa?sa[i+1]-sa[i]:1,bo=sb?sb[i]:i,bn=sb?sb[i+1]-sb[i]:1;
        SPUSH(&a_buf[ao],an); SPUSH(&b_buf[bo],bn); spush(val_compound(VAL_LIST,2,an+bn+1));
    }
    if(la>n) deep_free_values(&a_buf[sa?sa[n]:n],(sa?sa[la]:la)-(sa?sa[n]:n));
    if(lb>n) deep_free_values(&b_buf[sb?sb[n]:n],(sb?sb[lb]:lb)-(sb?sb[n]:n));
    spush(val_compound(VAL_LIST,n,sp-p0+1));
}
static void prim_range(Frame *e){(void)e;int64_t end=pop_int(),start=pop_int();int count=0;for(int64_t i=start;i<end;i++){spush(val_int(i));count++;}spush(val_compound(VAL_LIST,count,count+1));}
static void push_string_bytes(const char *buf, int len);
/* A body given to each/fold/mutate must leave exactly one value where its input began. */
static void one_value_above(int p0, const char *who, const char *what) {
    if(sp<=p0||sp-val_slots(stack[sp-1])!=p0)
        die("%s: the body must turn %s into one value, but the stack moved from %d slots to %d", who, what, p0, sp);
}
static void prim_each(Frame *env) {
    POP_BODY(fn,"each");
    Value top=speek(); if(top.tag!=VAL_LIST) die("each: expected list, got %s",valtag_name(top.tag));
    POP_VAL(list); int len=(int)list_top.as.compound.len,*st=elem_starts(list_buf,list_s,len),rb=sp;
    for(int i=0;i<len;i++){ int p0=sp; if(st) SPUSH(&list_buf[st[i]],st[i+1]-st[i]); else spush(list_buf[i]); eval_body(fn_buf,fn_s,env); one_value_above(p0,"each","one element"); }
    spush(val_compound(VAL_LIST,len,sp-rb+1));
}
static void prim_fold(Frame *env) {
    POP_BODY(fn,"fold"); POP_VAL(init);
    Value top=speek(); if(top.tag!=VAL_LIST) die("fold: expected list, got %s",valtag_name(top.tag));
    POP_VAL(list); int len=(int)list_top.as.compound.len,*st=elem_starts(list_buf,list_s,len);
    int p0=sp; SPUSH(init_buf,init_s);
    for(int i=0;i<len;i++){ if(st) SPUSH(&list_buf[st[i]],st[i+1]-st[i]); else spush(list_buf[i]); eval_body(fn_buf,fn_s,env); one_value_above(p0,"fold","the accumulator and one element"); }
}
/* The predicate takes a copy of each element; the list keeps the element when it leaves a nonzero int. */
static void prim_filter(Frame *env) {
    POP_BODY(fn,"filter");
    if(sp<=0||stack[sp-1].tag!=VAL_LIST) die("filter: expected list, got %s",sp>0?valtag_name(stack[sp-1].tag):"nothing");
    POP_VAL(list); int len=(int)list_top.as.compound.len,*st=elem_starts(list_buf,list_s,len),rb=sp,kept=0;
    for(int i=0;i<len;i++){
        int b=st?st[i]:i,n=st?st[i+1]-st[i]:1,p0=sp;
        stack_room(n,"filter"); deep_copy_values(&stack[sp],&list_buf[b],n); sp+=n;
        eval_body(fn_buf,fn_s,env); one_value_above(p0,"filter","one element");
        Value keep=spop(); if(keep.tag!=VAL_INT) die("filter: the body must leave an int, got %s",valtag_name(keep.tag));
        if(keep.as.i){ SPUSH(&list_buf[b],n); kept++; } else deep_free_values(&list_buf[b],n);
    }
    spush(val_compound(VAL_LIST,kept,sp-rb+1));
}
static int val_cmp(const Value *va, const Value *vb) {
    if(va->tag==VAL_INT&&vb->tag==VAL_INT) return(va->as.i>vb->as.i)-(va->as.i<vb->as.i);
    if(va->tag==VAL_FLOAT&&vb->tag==VAL_FLOAT) return(va->as.f>vb->as.f)-(va->as.f<vb->as.f);
    die("sort: mismatched or unsupported element types (got %s and %s)", valtag_name(va->tag), valtag_name(vb->tag)); return 0;
}
static int sort_cmp(const void *a,const void *b) { return val_cmp((const Value*)a,(const Value*)b); }
static void prim_sort(Frame *e){
    (void)e; Value top=speek(); if(top.tag!=VAL_LIST) die("sort: expected list, got %s",valtag_name(top.tag));
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    if(s!=len+1){ValTag bad=VAL_INT;for(int i=0;i<len;i++){ElemRef r=compound_elem(&stack[base],s,len,i);if(r.slots>1){bad=stack[base+r.base+r.slots-1].tag;break;}}
        die("sort: elements must be int or float, got %s",valtag_name(bad));}
    qsort(&stack[base],len,sizeof(Value),sort_cmp);
}
static inline void prim_indexof_impl(Frame *e, int tagged) {
    (void)e; POP_VAL(val); Value top=speek(); if(top.tag!=VAL_LIST) die("index-of: expected list, got %s",valtag_name(top.tag));
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s,r=-1,*st=elem_starts(&stack[base],s,len);
    for(int i=0;i<len&&r<0;i++){int b=st?st[i]:i,n=st?st[i+1]-st[i]:1;if(val_equal(&stack[base+b],n,val_buf,val_s))r=i;}
    deep_free_values(&stack[base],s); deep_free_values(val_buf,val_s); sp=base;
    if(r<0) { if(tagged) push_none(); else die("index-of: element not found"); }
    else { spush(val_int(r)); if(tagged) push_ok(); }
}
MUST_PAIR(indexof)
/* The checker proves every key `at` or `edit` reads, so a missing key is a checker bug. */
#define KEY_MISSING(who) die(who ": this record has no '%s, but the checker proved it has. This is a bug in slap's checker: please report it with this program.",sym_name(key))
static void prim_at(Frame *env) {
    (void)env; uint32_t key=pop_sym();
    if(sp<=0) die("at: stack underflow"); Value next=stack[sp-1];
    if(next.tag!=VAL_RECORD) die("at: expected record, got %s",valtag_name(next.tag));
    int s=val_slots(next),len=(int)next.as.compound.len,base=sp-s;
    int found; ElemRef ref=record_field(&stack[base],s,len,key,&found);
    if(!found) KEY_MISSING("at");
    deep_free_values(&stack[base],ref.base); deep_free_values(&stack[base+ref.base+ref.slots],s-ref.base-ref.slots);
    memmove(&stack[base],&stack[base+ref.base],ref.slots*sizeof(Value));
    sp=base+ref.slots;
}
#define REC_PREAMBLE(who) Value rec_top=speek();if(rec_top.tag!=VAL_RECORD)die(who ": expected record, got %s",valtag_name(rec_top.tag));int rec_s=val_slots(rec_top),rec_len=(int)rec_top.as.compound.len,rec_base=sp-rec_s
/* [rec][value] -> [rec'] with value under key. Appending turns the old header
   slot into the key; replacing moves the value over the old field. */
static void rec_put(uint32_t key, const char *who) {
    if(sp<=0) die("%s: stack underflow", who);
    int v_s=val_slots(stack[sp-1]),v_base=sp-v_s;
    if(v_base<1) die("%s: stack underflow: value needs %d slots, have %d",who,v_s,sp);
    Value rec_top=stack[v_base-1];
    if(rec_top.tag!=VAL_RECORD) die("%s: expected record, got %s",who,valtag_name(rec_top.tag));
    int rec_s=val_slots(rec_top),rec_len=(int)rec_top.as.compound.len,rec_base=v_base-rec_s;
    if(rec_base<0) die("%s: stack underflow: record needs %d slots, have %d",who,rec_s,v_base);
    int found; ElemRef ex=record_field(&stack[rec_base],rec_s,rec_len,key,&found);
    rec_top.loc=0;
    if(!found) {
        stack[v_base-1]=val_sym(key);
        rec_top.as.compound.len=(uint32_t)(rec_len+1); rec_top.as.compound.slots=(uint32_t)(rec_s+v_s+1);
        spush(rec_top); return;
    }
    int old_base=rec_base+ex.base,os=ex.slots;
    if(os==v_s) { deep_free_values(&stack[old_base],os); memmove(&stack[old_base],&stack[v_base],(size_t)v_s*sizeof(Value)); sp=v_base; return; }
    replace_run(old_base,os,v_s);
    rec_top.as.compound.slots=(uint32_t)(rec_s-os+v_s); stack[sp-1]=rec_top;
}
static void prim_into(Frame *e) { (void)e; rec_put(pop_sym(),"into"); }
static void prim_edit(Frame *env) {
    POP_BODY(fn,"edit"); uint32_t key=pop_sym(); REC_PREAMBLE("edit");
    int found; ElemRef ref=record_field(&stack[rec_base],rec_s,rec_len,key,&found);
    if(!found) KEY_MISSING("edit");
    stack_room(ref.slots,"edit");
    deep_copy_values(&stack[sp],&stack[rec_base+ref.base],ref.slots); sp+=ref.slots;
    eval_body(fn_buf,fn_s,env);
    rec_put(key,"edit");
}
typedef struct BoxData { Value *data; int slots; } BoxData;
static void prim_box(Frame *e){(void)e;Value top=speek();int s=val_slots(top);BoxData *bd=malloc(sizeof(BoxData));bd->data=malloc(s*sizeof(Value));bd->slots=s;VCPY(bd->data,&stack[sp-s],s);sp-=s;Value v;v.tag=VAL_BOX;v.loc=0;v.as.box=bd;spush(v);}
static void prim_free(Frame *e){
    (void)e;Value v=spop();
    if(v.tag==VAL_BOX){BoxData *bd=(BoxData*)v.as.box;deep_free_values(bd->data,bd->slots);free(bd->data);free(bd);return;}
    die("free: expected box, got %s", valtag_name(v.tag));
}
#define BOX_UNPACK(who) POP_BODY(fn,who); Value box_val=spop(); if(box_val.tag!=VAL_BOX) die(who ": expected box, got %s", valtag_name(box_val.tag)); \
    BoxData *bd=(BoxData*)box_val.as.box; stack_room(bd->slots,who)
/* The body reads a deep copy; the box keeps its own. */
static void prim_lend(Frame *env) {
    BOX_UNPACK("lend"); int sp0=sp;
    deep_copy_values(&stack[sp],bd->data,bd->slots); sp+=bd->slots;
    eval_body(fn_buf,fn_s,env);
    if(sp<sp0) die("lend: the body consumed %d value(s) from below the box's contents", sp0-sp);
    stack_room(1,"lend");
    memmove(&stack[sp0+1],&stack[sp0],(size_t)(sp-sp0)*sizeof(Value));
    stack[sp0]=box_val; sp++;
}
/* The body takes ownership of the contents and returns the replacement. */
static void prim_mutate(Frame *env) {
    BOX_UNPACK("mutate"); int p0=sp; SPUSH(bd->data,bd->slots); free(bd->data); bd->data=NULL;
    eval_body(fn_buf,fn_s,env); one_value_above(p0,"mutate","the contents");
    int ns=val_slots(stack[sp-1]);
    bd->data=malloc((size_t)ns*sizeof(Value)); if(!bd->data) die("mutate: out of memory for %d values", ns);
    bd->slots=ns; VCPY(bd->data,&stack[sp-ns],ns); sp-=ns; spush(box_val);
}
static DictData *dict_clone(DictData *orig);
/* mutate can make a box that contains itself, so both walks are depth-bounded. */
#define BOX_DEPTH_MAX 512
static int box_walk_depth = 0;
static void deep_copy_values(Value *dst, const Value *src, int slots) {
    VCPY(dst,src,slots);
    if(++box_walk_depth > BOX_DEPTH_MAX){ box_walk_depth=0;
        die("box nesting deeper than %d -- a box that contains itself cannot be copied", BOX_DEPTH_MAX); }
    for(int i=0;i<slots;i++){
        if(dst[i].tag==VAL_TUPLE) frame_ref(dst[i].as.compound.env);
        else if(dst[i].tag==VAL_BOX){
            BoxData *o=(BoxData*)dst[i].as.box; BoxData *c=malloc(sizeof(BoxData));
            c->slots=o->slots; c->data=malloc(o->slots*sizeof(Value));
            deep_copy_values(c->data,o->data,o->slots); dst[i].as.box=c;
        } else if(dst[i].tag==VAL_DICT){
            dst[i].as.box=dict_clone((DictData*)dst[i].as.box);
        }
    }
    box_walk_depth--;
}
static void deep_free_owned(Value *vals, int slots) {
    if(++box_walk_depth > BOX_DEPTH_MAX){ box_walk_depth=0;
        die("box nesting deeper than %d -- a box that contains itself cannot be freed", BOX_DEPTH_MAX); }
    for(int i=0;i<slots;i++){
        if(vals[i].tag==VAL_TUPLE) frame_drop(vals[i].as.compound.env);
        else if(vals[i].tag==VAL_DICT) dict_data_free((DictData*)vals[i].as.box);
        else if(vals[i].tag==VAL_BOX){
            BoxData *bd=(BoxData*)vals[i].as.box;
            deep_free_values(bd->data,bd->slots); free(bd->data); free(bd);
        }
    }
    box_walk_depth--;
}
/* ---- DICT ---- */
static uint32_t dict_hash(const char *key, int klen) {
    uint32_t h=2166136261u; for(int i=0;i<klen;i++){h^=(uint8_t)key[i]; h*=16777619u;} return h;
}
static int dict_probe(DictData *dd, const char *key, int klen) {
    /* returns index of matching entry, or first empty slot on the probe chain */
    if(dd->cap==0) return -1;
    uint32_t h=dict_hash(key,klen); int mask=dd->cap-1, i=(int)(h&mask);
    for(int n=0;n<dd->cap;n++){
        DictEntry *e=&dd->entries[i];
        if(!e->key) return i;
        if(e->klen==klen && memcmp(e->key,key,klen)==0) return i;
        i=(i+1)&mask;
    }
    return -1;
}
static void dict_grow(DictData *dd) {
    int old_cap=dd->cap; DictEntry *old=dd->entries;
    dd->cap = old_cap? old_cap*2 : 8;
    dd->entries = calloc(dd->cap, sizeof(DictEntry));
    dd->len = 0;
    for(int i=0;i<old_cap;i++){
        DictEntry *e=&old[i]; if(!e->key) continue;
        int j=dict_probe(dd,e->key,e->klen);
        dd->entries[j]=*e; dd->len++;
    }
    free(old);
}
/* Takes ownership of vals' boxes and dicts. */
static void dict_put(DictData *dd, const char *key, int klen, Value *vals, int nvals) {
    if(dd->cap==0 || (dd->len+1)*10 >= dd->cap*7) dict_grow(dd);
    int i=dict_probe(dd,key,klen); if(i<0) die("dict: probe failed (internal)");
    DictEntry *e=&dd->entries[i];
    if(e->key){ deep_free_values(e->vals,e->nvals); free(e->vals); }
    else { e->key=malloc(klen?klen:1); if(klen) memcpy(e->key,key,klen); e->klen=klen; dd->len++; }
    e->vals=malloc((size_t)nvals*sizeof(Value)); e->nvals=nvals; VCPY(e->vals,vals,nvals);
}
static DictEntry *dict_get(DictData *dd, const char *key, int klen) {
    int i=dict_probe(dd,key,klen); if(i<0) return NULL;
    return dd->entries[i].key ? &dd->entries[i] : NULL;
}
static void dict_free_entry_contents(DictEntry *e) {
    if(!e->key) return;
    deep_free_values(e->vals,e->nvals);
    free(e->key); free(e->vals); e->key=NULL; e->vals=NULL; e->klen=0; e->nvals=0;
}
static int dict_del(DictData *dd, const char *key, int klen) {
    if(dd->cap==0) return 0;
    int i=dict_probe(dd,key,klen); if(i<0 || !dd->entries[i].key) return 0;
    dict_free_entry_contents(&dd->entries[i]);
    dd->len--;
    /* rehash tail of probe chain */
    int mask=dd->cap-1, j=(i+1)&mask;
    while(dd->entries[j].key){
        DictEntry tmp=dd->entries[j]; dd->entries[j].key=NULL; dd->entries[j].vals=NULL;
        dd->len--;
        int k=dict_probe(dd,tmp.key,tmp.klen);
        dd->entries[k]=tmp; dd->len++;
        j=(j+1)&mask;
    }
    return 1;
}
/* Same capacity, so every entry keeps its slot. */
static DictData *dict_clone(DictData *orig) {
    DictData *c=calloc(1,sizeof(DictData)); c->cap=orig->cap; c->len=orig->len;
    if(orig->cap) c->entries=calloc(orig->cap,sizeof(DictEntry));
    for(int i=0;i<orig->cap;i++){
        DictEntry *o=&orig->entries[i],*n=&c->entries[i]; if(!o->key) continue;
        n->key=malloc(o->klen?o->klen:1); memcpy(n->key,o->key,o->klen); n->klen=o->klen;
        n->vals=malloc((size_t)o->nvals*sizeof(Value)); n->nvals=o->nvals; deep_copy_values(n->vals,o->vals,o->nvals);
    }
    return c;
}
static int pop_string_bytes(const char *who, const char *what, char **out, int *out_len) {
    Value top=speek();
    if(top.tag!=VAL_LIST) die("%s: expected string (list of int), got %s", who, valtag_name(top.tag));
    int s=val_slots(top), len=(int)top.as.compound.len, base=sp-s;
    if(s != len+1) die("%s: %s must be a simple string (list of int)", who, what);
    char *buf=malloc(len?len:1);
    for(int i=0;i<len;i++){
        if(stack[base+i].tag!=VAL_INT) die("%s: %s string contains non-int at position %d", who, what, i);
        int64_t c=stack[base+i].as.i; if(c<0||c>255) die("%s: %s byte %d is %lld, outside 0-255", who, what, i, (long long)c);
        buf[i]=(char)c;
    }
    sp=base; *out=buf; *out_len=len; return len;
}
static uint64_t user_loc;
static void prim_fail(Frame *e) {
    (void)e; char *msg; int n, s0=sp; pop_string_bytes("fail","text", &msg, &n);
    if(!n){ sp=s0; die("fail: the text is empty; expected a message"); }
    for(int i=0;i<n;i++) if(!msg[i]){ sp=s0; die("fail: the text holds a NUL byte at offset %d; expected text without NUL bytes", i); }
    if(LOC_FID(current_loc)==FID_PRELUDE) current_loc=user_loc;
    die("%.*s", n, msg);
}
static void push_byte_list(const unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) spush(val_int(buf[i]));
    spush(val_compound(VAL_LIST, (int)len, (int)len + 1));
}
static void push_string_bytes(const char *buf, int len) { push_byte_list((const unsigned char*)buf, (size_t)len); }
static void push_c_string(const char *s) { push_byte_list((const unsigned char*)s, strlen(s)); }
static void push_fail(const char *msg) { push_c_string(msg); push_no(); }
static Value dict_val(DictData *dd){Value v={0};v.tag=VAL_DICT;v.loc=0;v.as.box=dd;return v;}
static void prim_dict(Frame *e){(void)e;DictData *dd=calloc(1,sizeof(DictData));dicts_made=1;spush(dict_val(dd));}
static void prim_insert(Frame *e) {
    (void)e; POP_VAL(val); char *key; int klen; pop_string_bytes("insert","key",&key,&klen);
    Value dv=speek(); if(dv.tag!=VAL_DICT) die("insert: expected dict, got %s", valtag_name(dv.tag));
    DictData *dd=(DictData*)dv.as.box;
    dict_put(dd,key,klen,val_buf,val_s);
    free(key);
}
static void prim_of(Frame *e) {
    (void)e; char *key; int klen; pop_string_bytes("of","key",&key,&klen);
    Value dv=speek(); if(dv.tag!=VAL_DICT) die("of: expected dict, got %s", valtag_name(dv.tag));
    DictData *dd=(DictData*)dv.as.box;
    DictEntry *ent=dict_get(dd,key,klen);
    if(!ent){ push_string_bytes(key,klen); free(key); push_no(); return; }
    free(key);
    /* A deep copy: the dict keeps its own boxes and dicts. */
    stack_room(ent->nvals,"of");
    deep_copy_values(&stack[sp],ent->vals,ent->nvals); sp+=ent->nvals; push_ok();
}
static void prim_remove(Frame *e) {
    (void)e; char *key; int klen; pop_string_bytes("remove","key",&key,&klen);
    Value dv=speek(); if(dv.tag!=VAL_DICT) die("remove: expected dict, got %s", valtag_name(dv.tag));
    DictData *dd=(DictData*)dv.as.box;
    dict_del(dd,key,klen); free(key);
}
static void prim_keys(Frame *e) {
    (void)e; Value dv=speek(); if(dv.tag!=VAL_DICT) die("dict-keys: expected dict, got %s", valtag_name(dv.tag));
    DictData *dd=(DictData*)dv.as.box;
    int rb=sp, count=0;
    for(int i=0;i<dd->cap;i++){DictEntry *ent=&dd->entries[i]; if(!ent->key) continue;
        push_string_bytes(ent->key,ent->klen); count++;}
    spush(val_compound(VAL_LIST,count,sp-rb+1));
}
static void prim_entries(Frame *e) {
    (void)e; Value dv=speek(); if(dv.tag!=VAL_DICT) die("dict-entries: expected dict, got %s", valtag_name(dv.tag));
    DictData *dd=(DictData*)dv.as.box;
    int rb=sp, count=0;
    for(int i=0;i<dd->cap;i++){DictEntry *ent=&dd->entries[i]; if(!ent->key) continue;
        int p0=sp; spush(val_sym(S_KEY)); push_string_bytes(ent->key,ent->klen);
        stack_room(ent->nvals+2,"dict-entries"); spush(val_sym(S_VALUE));
        deep_copy_values(&stack[sp],ent->vals,ent->nvals); sp+=ent->nvals;
        spush(val_compound(VAL_RECORD,2,sp-p0+1)); count++;}
    spush(val_compound(VAL_LIST,count,sp-rb+1));
}
static void dict_data_free(DictData *dd) {
    for(int i=0;i<dd->cap;i++) dict_free_entry_contents(&dd->entries[i]);
    free(dd->entries); free(dd);
}
/* ---- EVAL ---- */
/* --profile: a call tree of words and primitives. Each node holds its self time; every enter and
   leave charges the time since the last one to the current node. An edge to a word already on the
   path points back at that ancestor, so recursion folds into one node. */
#define PROF_NODES (1<<18)
#define PROF_SLOTS (1<<19)
static int prof_on, prof_cur, prof_nodes = 1, prof_edges, prof_sp;
static int prof_parent[PROF_NODES], prof_stack[4*EVAL_DEPTH_MAX];
static uint32_t prof_sym[PROF_NODES]; static uint64_t prof_ns[PROF_NODES], prof_last;
static int slot_from[PROF_SLOTS], slot_to[PROF_SLOTS]; static uint32_t slot_sym[PROF_SLOTS];
static uint64_t prof_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec*1000000000u + (uint64_t)t.tv_nsec; }
static void prof_charge(void) { uint64_t now = prof_now(); prof_ns[prof_cur] += now - prof_last; prof_last = now; }
static void prof_enter(uint32_t sym) {
    prof_charge();
    if (prof_sp == (int)(sizeof prof_stack / sizeof prof_stack[0])) die("--profile: calls nested more than %d deep", prof_sp);
    prof_stack[prof_sp++] = prof_cur;
    uint32_t h = ((uint32_t)prof_cur * 2654435761u ^ sym) & (PROF_SLOTS-1);
    for (; slot_to[h]; h = (h+1) & (PROF_SLOTS-1))
        if (slot_from[h] == prof_cur && slot_sym[h] == sym) { prof_cur = slot_to[h]-1; return; }
    /* Only a word can recur; a primitive under itself, like `if` in a branch of `if`, is a new frame. */
    int to = prim_fns[sym] ? 0 : prof_cur; while (to && prof_sym[to] != sym) to = prof_parent[to];
    if (!to) {
        if (prof_nodes == PROF_NODES) die("--profile: more than %d distinct call paths", PROF_NODES);
        to = prof_nodes++; prof_parent[to] = prof_cur; prof_sym[to] = sym;
    }
    if (++prof_edges > PROF_SLOTS/2) die("--profile: more than %d call edges", PROF_SLOTS/2);
    slot_from[h] = prof_cur; slot_sym[h] = sym; slot_to[h] = to+1; prof_cur = to;
}
static void prof_leave(void) { prof_charge(); prof_cur = prof_stack[--prof_sp]; }
/* Folded stacks for flamegraph.pl: one `a;b;c nanoseconds` line per node that took time. */
static void prof_report(void) {
    prof_charge();
    int *path = malloc(sizeof(int) * (size_t)prof_nodes); if (!path) return;
    for (int n = 1; n < prof_nodes; n++) {
        if (!prof_ns[n]) continue;
        int d = 0; for (int a = n; a; a = prof_parent[a]) path[d++] = a;
        while (d--) { fputs(sym_name(prof_sym[path[d]]), stderr); fputc(d ? ';' : ' ', stderr); }
        fprintf(stderr, "%llu\n", (unsigned long long)prof_ns[n]);
    }
    free(path);
}
static inline __attribute__((always_inline)) void dispatch_word(uint32_t sym, Frame *env) {
    Lookup lu=frame_lookup(env,sym);
    if(!lu.bind) die("unknown word: %s",sym_name(sym));
    Binding *b=lu.bind; Value *v=b->vals; int s=b->slots;
    if(!b->word){ if(b->heap){ stack_room(s,sym_name(sym)); deep_copy_values(&stack[sp],v,s); sp+=s; } else { SPUSH(v,s); if(b->tuples) vals_retain(v,s); } return; }
    Frame *f=lu.frame; int bi=(int)(b-f->bindings);
    if(LOC_FID(current_loc)!=FID_PRELUDE) user_loc=current_loc;
    b->pinned++; eval_body(v,s,env); f->bindings[bi].pinned--;
}
/* Under --profile, build_tuple leaves primitives unresolved, so every word arrives here.
   A separate function keeps eval_in's hot loop as it is. */
__attribute__((noinline)) static void prof_dispatch(uint32_t sym, Frame *env) {
    /* The checker refuses a binding named like a primitive, so a primitive is never shadowed. */
    if(prim_fns[sym]){ prof_enter(sym); prim_fns[sym](env); prof_leave(); return; }
    Lookup lu=frame_lookup(env,sym);
    if(!lu.bind || !lu.bind->word){ dispatch_word(sym,env); return; }
    prof_enter(sym); dispatch_word(sym,env); prof_leave();
}
static void eval_body(Value *body, int slots, Frame *env) {
    Value hdr=body[slots-1]; if(hdr.tag!=VAL_TUPLE) die("eval_body: expected tuple, got %s (internal: evaluator received non-tuple header)", valtag_name(hdr.tag));
    eval_in(body, slots, hdr.as.compound.env?hdr.as.compound.env:env);
}
static void eval_run(Value *body, int slots, Frame *ee) {
    if(++eval_depth > EVAL_DEPTH_MAX) die("recursion depth exceeded (%d levels)", EVAL_DEPTH_MAX);
    c_stack_check("in a nested call");
    int len=(int)body[slots-1].as.compound.len;
    int a0=asp,*st=elem_starts(body,slots,len);
    for(int k=0;k<len;k++){
        int eo=k,es=1; if(st){eo=st[k];es=st[k+1]-st[k];}
        const Value *ep=&body[eo+es-1];
        if(ep->tag<=VAL_SYM){
            if(sp>=STACK_MAX){ current_loc=ep->loc; die("stack overflow: all %d value slots are in use", STACK_MAX); }
            stack[sp++]=*ep; continue;
        }
        if(ep->loc) current_loc=ep->loc;
        if(ep->tag==VAL_XT){
            int a1=asp,s0=staged_n;
            if(ep->as.xt.fn) ep->as.xt.fn(ee);
            else if(ep->as.xt.sym==S_LET){
                uint32_t n=pop_sym(); if(sp<=0) die("let: nothing to bind to '%s", sym_name(n));
                int ds=val_slots(stack[sp-1]); if(sp-ds<0) die("let: stack underflow: need %d slots, have %d", ds, sp);
                /* A body written right before the name makes a word. */
                int word = stack[sp-1].tag==VAL_TUPLE && k>=2 && body[st?st[k-1]-1:k-2].tag==VAL_TUPLE;
                sp-=ds; frame_bind(ee,n,&stack[sp],ds,word);
            }
            else if(__builtin_expect(prof_on,0)) prof_dispatch(ep->as.xt.sym,ee);
            else dispatch_word(ep->as.xt.sym,ee);
            /* the bodies the primitive took are used up */
            while(staged_n>s0){ int at=staged[--staged_n]; Value *h=&aux[at];
                if(h->flags&VF_DICT){ int hs=val_slots(*h); deep_free_values(&aux[at-hs+1],hs); }
                else if(h->tag==VAL_TUPLE) frame_drop(h->as.compound.env); }
            asp=a1;
        } else if(is_compound(ep->tag)){
            /* `(then) (else) if` written in place: run the chosen branch from this body
               rather than copy both branches to the stack and then to the aux stack. */
            if(ep->tag==VAL_TUPLE && k+2<len){
                int e2=st?st[k+2]:k+2, s2=st?st[k+3]-st[k+2]:1, e1=st?st[k+1]:k+1, s1=e2-e1;
                const Value *b2=&body[e2+s2-1];
                if(b2->tag==VAL_XT && b2->as.xt.fn==prim_if && body[e2-1].tag==VAL_TUPLE){
                    if(b2->loc) current_loc=b2->loc;
                    if(sp<=0) die("if: stack underflow");
                    Value c=spop(); if(c.tag!=VAL_INT) die("if: condition must be int, got %s",valtag_name(c.tag));
                    if(c.as.i) eval_in(&body[eo],es,ee); else eval_in(&body[e1],s1,ee);
                    k+=2; continue;
                }
            }
            /* a literal is built once; a dict in it belongs to each copy the program pushes */
            if(ep->flags&VF_DICT){ stack_room(es,"a literal"); deep_copy_values(&stack[sp],&body[eo],es); sp+=es; }
            else SPUSH(&body[eo],es);
            if(ep->tag==VAL_TUPLE){ stack[sp-1].as.compound.env=ee; frame_ref(ee); }
        } else if(ep->tag==VAL_DICT){ stack_room(1,"a literal"); deep_copy_values(&stack[sp],ep,1); sp++; }
        else spush(*ep);
    }
    asp=a0; eval_depth--;
}
static void build_tuple(Token *toks, int start, int end, int tc, Frame *env) {
    int eb=sp,ec=0,binds=0; Token *ft = (start < end) ? &toks[start] : NULL;
    for(int j=start;j<end;j++){
        Token *tt=&toks[j]; current_loc=LOC_PACK(tt->fid,tt->line,tt->col);
        switch(tt->tag){
        case TOK_INT:
            spush(with_tok(val_int(tt->as.i),tt)); ec++; break;
        case TOK_FLOAT: spush(with_tok(val_float(tt->as.f),tt)); ec++; break;
        case TOK_SYM: spush(with_tok(val_sym(tt->as.sym),tt)); ec++; break;
        case TOK_WORD:
            {
                uint32_t s=tt->as.sym; binds|=s==S_LET;
                /* A fused `X must` runs directly even under --profile, so a body holds the same values either way. */
                if(prim_must_fns[s] && j+1<end && toks[j+1].tag==TOK_WORD && toks[j+1].as.sym==S_MUST){spush(with_tok(val_xt(s,prim_must_fns[s]),tt));ec++;j++;break;}
                if(prof_on){spush(with_tok(val_xt(s,NULL),tt));ec++;break;}
                spush(with_tok(val_xt(s,prim_fns[s]),tt));ec++;
            }
            break;
        case TOK_STRING:
            for(int c=0;c<tt->as.str.len;c++) spush(with_tok(val_int(tt->as.str.codes[c]),tt));
            spush(with_tok(val_compound(VAL_LIST,tt->as.str.len,tt->as.str.len+1),tt)); ec++; break;
        case TOK_LPAREN:{int nc=(j+toks[j].span);build_tuple(toks,j+1,nc,tc,env);stack[sp-1].loc=LOC_PACK(tt->fid,tt->line,tt->col);ec++;j=nc;break;}
        case TOK_LBRACKET:{
            int bc=(j+toks[j].span);
            if(bc+1<tc&&toks[bc+1].tag==TOK_WORD&&toks[bc+1].as.sym==S_EFFECT){if(ec>0&&stack[sp-1].tag==VAL_SYM){sp--;ec--;}j=bc+1;break;}
            int lb=sp; eval(toks+j+1,bc-j-1,env);
            int n=0,p=sp; while(p>lb){p-=val_slots(stack[p-1]);n++;}
            spush(with_tok(val_compound(VAL_LIST,n,sp-lb+1),tt)); if(vals_hold_dict(&stack[lb],sp-lb)) stack[sp-1].flags|=VF_DICT; ec++; j=bc; break;
        }
        case TOK_LBRACE:{
            int bc=(j+toks[j].span);
            int lb=sp; eval(toks+j+1,bc-j-1,env); int ts=sp-lb,nf=0,ir=1,p=sp;
            while(p>lb){int vs=val_slots(stack[p-1]);p-=vs;if(ir&&p>lb&&stack[p-1].tag==VAL_SYM){p--;nf++;}else ir=0;}
            if(!ir) die("a {...} literal that does not pair each value with a 'key reached the runtime, but the checker refuses one. This is a bug in slap's checker: please report it with this program.");
            spush(with_tok(val_compound(VAL_RECORD,nf,ts+1),tt));
            if(vals_hold_dict(&stack[lb],ts)) stack[sp-1].flags|=VF_DICT;
            ec++; j=bc; break;
        }
        default: break;
        }
    }
    Value hdr=val_compound(VAL_TUPLE,ec,sp-eb+1); if(ft) hdr.loc=LOC_PACK(ft->fid,ft->line,ft->col);
    if(binds) hdr.flags|=VF_BINDS;
    if(vals_hold_dict(&stack[eb],sp-eb)) hdr.flags|=VF_DICT;
    spush(hdr); stack[sp-1].as.compound.env=env;
}
static void eval(Token *toks, int count, Frame *env) {
    int base=sp; build_tuple(toks,0,count,count,env);
    int s=val_slots(stack[sp-1]); Value *body=malloc(s*sizeof(Value));
    VCPY(body,&stack[base],s); sp=base; eval_run(body,s,env); free(body);
}
static void prim_millis(Frame *e){(void)e;struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);spush(val_int((int64_t)(ts.tv_sec*1000+ts.tv_nsec/1000000)));}
/* Local wall clock in Varvara's Datetime port order: year month(0-11) day hour
   minute second dotw doty isdst. Unknown isdst reads as 0. */
static void prim_datetime(Frame *e){
    (void)e; time_t t=time(NULL); struct tm lt;
    if(t==(time_t)-1) die("datetime: the system clock is unreadable -- time() returned -1.");
    if(!localtime_r(&t,&lt))
        die("datetime: localtime_r rejected the system clock (epoch %lld).\n"
            "  The TZ environment variable may name a timezone that does not exist.",(long long)t);
    int64_t f[9]={lt.tm_year+1900,lt.tm_mon,lt.tm_mday,lt.tm_hour,lt.tm_min,
                  lt.tm_sec,lt.tm_wday,lt.tm_yday,lt.tm_isdst>0};
    for(int i=0;i<9;i++) spush(val_int(f[i]));
    spush(val_compound(VAL_LIST,9,10));
}
static const char *PRELUDE =
    "(swap drop) 'nip let\n"
    "(0 eq) 'not let\n"
    "(eq not) 'neq let\n"
    "(swap lt) 'gt let\n"
    "(lt not) 'ge let\n"
    "(swap lt not) 'le let\n"
    "(1 plus) 'inc let\n"
    "(1 sub) 'dec let\n"
    "(0 swap sub) 'neg let\n"
    "(over over lt (nip) (drop) if) 'max let\n"
    "(over over lt (drop) (nip) if) 'min let\n"
    "(dup 0 lt (neg) (dup drop) if) 'abs let\n"
    "('f let (dup 0 gt) (1 sub (f apply) dip) while drop) 'repeat let\n"
    "(dup mul) 'sqr let\n"
    "(0 get must) 'first let\n"
    "(dup len 1 sub get must) 'last let\n"
    "(0 (plus) fold) 'sum let\n"
    "(index-of {'ok (drop 1) 'no (drop 0)} case) 'member let\n"
    "(list rot push swap push) 'couple let\n"
    "(list (cat) fold) 'flatten let\n"
    "(0.0 swap sub) 'fneg let\n"
    "(dup 0.0 lt (fneg) () if) 'fabs let\n"
    "(dup 0 lt (drop -1) (dup 0 eq (drop 0) (drop 1) if) if) 'sign let\n"
    "(rot swap min max) 'clamp let\n"
    "(2 mod 0 eq) 'iseven let\n"
    "('ok tag) 'ok let\n"
    "('no tag) 'no let\n"
    "(() no) 'none let\n"
    "('body let {'ok (body apply) 'no (no)} case) 'then let\n"
    "('fb let {'ok () 'no (drop fb)} case) 'default let\n"
    "(list ('dd-x let dup dd-x member (dd-x drop) (dd-x push) if) fold) 'dedup let\n"
    "3.14159265358979323846 'pi let\n"
    "6.28318530717958647692 'tau let\n"
    "(255 band) 'byte-mask let\n"
    "('b let 0 8 range (7 swap sub b swap shr 1 band) each) 'byte-bits let\n"
    "(0 (swap 1 shl bor) fold) 'bits-byte let\n"
    "('n let n 1 lt (n \"chunks: the size must be at least 1\" fail) () if list swap (dup len 0 eq not) (dup n take-n swap (push) dip n drop-n) while drop) 'chunks let\n"

;
/* ---- SDL ---- */
#ifdef SLAP_SDL
#include <SDL.h>
#include <SDL_syswm.h>
#ifdef __APPLE__
#include <objc/message.h>
#endif
#define CANVAS_W 640
#define CANVAS_H 480
static uint8_t canvas[CANVAS_W*CANVAS_H];
static SDL_Window *sdl_window=NULL; static SDL_Renderer *sdl_renderer=NULL; static SDL_Texture *sdl_texture=NULL;
#define MAX_HANDLERS 16
static struct{uint32_t event_sym;Value *handler_body;int handler_slots;} event_handlers[MAX_HANDLERS];
static int handler_count=0;
static Value *render_body=NULL; static int render_slots=0;
static uint8_t gray_lut[4]={0,85,170,255};
static void sdl_init(void) {
    if(sdl_window) return;
    if(SDL_Init(SDL_INIT_VIDEO)<0) die("SDL_Init: %s",SDL_GetError());
    /* RESIZABLE tells emscripten's SDL2 to track the canvas's CSS box, which is
       unsized in shell.html — the canvas collapses to 3x3. The web canvas is
       fixed at CANVAS_W x CANVAS_H anyway, so only ask for it on desktop. */
#ifdef __EMSCRIPTEN__
    Uint32 win_flags=SDL_WINDOW_BORDERLESS;
#else
    Uint32 win_flags=SDL_WINDOW_BORDERLESS|SDL_WINDOW_RESIZABLE;
#endif
    sdl_window=SDL_CreateWindow("slap",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,CANVAS_W,CANVAS_H,win_flags);
    if(!sdl_window) die("SDL_CreateWindow: %s",SDL_GetError());
#ifdef __APPLE__
    SDL_SysWMinfo wminfo; SDL_VERSION(&wminfo.version);
    if(SDL_GetWindowWMInfo(sdl_window,&wminfo)){
        id nsw=(id)wminfo.info.cocoa.window;
        ((void(*)(id,SEL,BOOL))objc_msgSend)(nsw,sel_getUid("setHasShadow:"),NO);
    }
#endif
    sdl_renderer=SDL_CreateRenderer(sdl_window,-1,SDL_RENDERER_ACCELERATED);
    if(!sdl_renderer) die("SDL_CreateRenderer: %s",SDL_GetError());
    SDL_RenderSetLogicalSize(sdl_renderer,CANVAS_W,CANVAS_H);
    sdl_texture=SDL_CreateTexture(sdl_renderer,SDL_PIXELFORMAT_RGB24,SDL_TEXTUREACCESS_STREAMING,CANVAS_W,CANVAS_H);
    if(!sdl_texture) die("SDL_CreateTexture: %s",SDL_GetError());
    memset(canvas,0,sizeof(canvas));
}
static void sdl_present(void) {
    static uint8_t pixels[CANVAS_W*CANVAS_H*3];
    for(int i=0;i<CANVAS_W*CANVAS_H;i++){uint8_t g=gray_lut[canvas[i]&3];pixels[i*3]=pixels[i*3+1]=pixels[i*3+2]=g;}
    SDL_UpdateTexture(sdl_texture,NULL,pixels,CANVAS_W*3);
    SDL_RenderClear(sdl_renderer);SDL_RenderCopy(sdl_renderer,sdl_texture,NULL,NULL);SDL_RenderPresent(sdl_renderer);
}
static void prim_clear(Frame *e){(void)e;memset(canvas,(int)(pop_int()&3),sizeof(canvas));}
static void prim_pixel(Frame *e){(void)e;int64_t color=pop_int(),y=pop_int(),x=pop_int();if(x>=0&&x<CANVAS_W&&y>=0&&y<CANVAS_H)canvas[y*CANVAS_W+x]=(uint8_t)(color&3);}
static void prim_fill_rect(Frame *e){(void)e;int64_t c=pop_int(),h=pop_int(),w=pop_int(),y0=pop_int(),x0=pop_int();uint8_t cv=(uint8_t)(c&3);for(int dy=0;dy<h;dy++)for(int dx=0;dx<w;dx++){int x=x0+dx,y=y0+dy;if(x>=0&&x<CANVAS_W&&y>=0&&y<CANVAS_H)canvas[y*CANVAS_W+x]=cv;}}
static uint32_t sym_tick=0,sym_keydown=0,sym_keyup=0,sym_mousedown=0,sym_mouseup=0,sym_mousemove=0;
static void show_intern_syms(void) {
    if(!sym_tick){sym_tick=sym_intern("tick");sym_keydown=sym_intern("keydown");sym_keyup=sym_intern("keyup");sym_mousedown=sym_intern("mousedown");sym_mouseup=sym_intern("mouseup");sym_mousemove=sym_intern("mousemove");}
}
static void prim_on(Frame *e) {
    (void)e; Value fn_top=speek(); if(fn_top.tag!=VAL_TUPLE) die("on: expected tuple handler, got %s", valtag_name(fn_top.tag));
    int fn_s=val_slots(fn_top); if(handler_count>=MAX_HANDLERS) die("on: too many event handlers");
    Value *hb=malloc((size_t)fn_s*sizeof(Value)); if(!hb) die("on: out of memory"); VCPY(hb,&stack[sp-fn_s],fn_s); event_handlers[handler_count].handler_body=hb;
    event_handlers[handler_count].handler_slots=fn_s; sp-=fn_s;
    uint32_t ev=pop_sym(); show_intern_syms();
    if(ev!=sym_tick&&ev!=sym_keydown&&ev!=sym_keyup&&ev!=sym_mousedown&&ev!=sym_mouseup&&ev!=sym_mousemove)
        die("on: unknown event '%s; the events are 'tick 'keydown 'keyup 'mousedown 'mouseup 'mousemove",sym_name(ev));
    event_handlers[handler_count].event_sym=ev; handler_count++;
}
static void show_dispatch_event(SDL_Event *ev, Frame *env) {
    if(ev->type==SDL_KEYDOWN||ev->type==SDL_KEYUP){uint32_t ksym=ev->type==SDL_KEYDOWN?sym_keydown:sym_keyup;for(int h=0;h<handler_count;h++)if(event_handlers[h].event_sym==ksym){spush(val_int((int64_t)ev->key.keysym.sym));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}}
    int is_mouse=0; float lx,ly;
    if(ev->type==SDL_MOUSEBUTTONDOWN||ev->type==SDL_MOUSEBUTTONUP||ev->type==SDL_MOUSEMOTION) is_mouse=1;
    if(is_mouse){
        int sx,sy; SDL_GetMouseState(&sx,&sy);
        SDL_RenderWindowToLogical(sdl_renderer,sx,sy,&lx,&ly);
        int64_t mx=(int64_t)lx, my=(int64_t)ly;
        uint32_t sym=ev->type==SDL_MOUSEBUTTONDOWN?sym_mousedown:ev->type==SDL_MOUSEBUTTONUP?sym_mouseup:sym_mousemove;
        for(int h=0;h<handler_count;h++)if(event_handlers[h].event_sym==sym){spush(val_int(mx));spush(val_int(my));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}}
}
static void show_tick_render(int64_t frame, Frame *env) {
    for(int h=0;h<handler_count;h++) if(event_handlers[h].event_sym==sym_tick){spush(val_int(frame));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}
    if(render_slots>0){int ms=val_slots(stack[sp-1]);stack_room(ms,"show");deep_copy_values(&stack[sp],&stack[sp-ms],ms);sp+=ms;eval_body(render_body,render_slots,env);}
    sdl_present();
}
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
static Frame *show_env=NULL; static int64_t show_frame=0;
static void show_one_frame(void) {
    SDL_Event ev;
    while(SDL_PollEvent(&ev)){
        if(ev.type==SDL_QUIT){emscripten_cancel_main_loop();return;}
        show_dispatch_event(&ev,show_env);
    }
    show_tick_render(show_frame++, show_env);
}
#endif
static void prim_show(Frame *env) {
    Value fn_top=speek(); if(fn_top.tag!=VAL_TUPLE) die("show: expected tuple render function, got %s", valtag_name(fn_top.tag));
    render_slots=val_slots(fn_top); render_body=malloc((size_t)render_slots*sizeof(Value)); if(!render_body) die("show: out of memory");
    VCPY(render_body,&stack[sp-render_slots],render_slots); sp-=render_slots;
    show_intern_syms();
    if(headless_mode){
        int64_t frame=0;
        for(;;){
            for(int h=0;h<handler_count;h++)
                if(event_handlers[h].event_sym==sym_tick){spush(val_int(frame));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}
            frame++;
            SDL_Delay(16);
        }
    }
    sdl_init();
#ifdef __EMSCRIPTEN__
    show_env=env; show_frame=0;
    emscripten_set_main_loop(show_one_frame,0,1);
#else
    int64_t frame=0; int running=1;
    while(running){
        SDL_Event ev;
        while(SDL_PollEvent(&ev)){
            if(ev.type==SDL_QUIT){running=0;break;}
            show_dispatch_event(&ev,env);
        }
        show_tick_render(frame++, env);
        SDL_Delay(16);
    }
    SDL_DestroyTexture(sdl_texture);SDL_DestroyRenderer(sdl_renderer);SDL_DestroyWindow(sdl_window);SDL_Quit();exit(0);
#endif
}
#endif
static unsigned char *pop_byte_list_buf(const char *who, int *out_len) {
    Value top=spop();if(top.tag!=VAL_LIST)die("%s: expected a byte list, got %s",who,valtag_name(top.tag));
    int len=(int)top.as.compound.len;if((int)top.as.compound.slots-1!=len)die("%s: list elements must all be single-slot (ints)",who);
    unsigned char *buf=malloc(len);
    for(int i=0;i<len;i++){Value v=stack[sp-len+i];if(v.tag!=VAL_INT)die("%s: byte element %d is not an int",who,i);
        if(v.as.i<0||v.as.i>255)die("%s: byte %d out of range (got %lld)",who,i,(long long)v.as.i);buf[i]=(unsigned char)v.as.i;}
    sp-=len;*out_len=len;return buf;
}
static char *pop_string_path(const char *who, int *len) {
    unsigned char *raw = pop_byte_list_buf(who, len);
    char *buf = realloc(raw, *len + 1); buf[*len] = '\0'; return buf;
}
/* A C string stops at a NUL, so a path with one cannot name a file. The 'no payload keeps every byte. */
static int path_has_nul(const char *path, int len) {
    if (!memchr(path, 0, len)) return 0;
    push_byte_list((const unsigned char*)path, len); push_no(); return 1;
}
static void prim_read(Frame *e) {
    (void)e; int plen;char *path=pop_string_path("read",&plen);
    if(path_has_nul(path,plen)){free(path);return;}
    FILE *f=fopen(path,"rb");
    if(!f) { push_fail(path); free(path); return; }
    size_t n=0,cap=65536,got; unsigned char *buf=malloc(cap);
    while(buf&&(got=fread(buf+n,1,cap-n,f))>0){ n+=got; if(n==cap){ cap*=2; buf=realloc(buf,cap); } }
    if(!buf) die("read: out of memory reading %s", path);
    int bad=ferror(f); fclose(f);
    if(bad) { free(buf); push_fail(path); free(path); return; }
    push_byte_list(buf,n);free(buf);free(path); push_ok();
}
static void prim_write(Frame *e) {
    (void)e; int len;unsigned char *buf=pop_byte_list_buf("write",&len);int plen;char *path=pop_string_path("write",&plen);
    if(path_has_nul(path,plen)){free(buf);free(path);return;}
    /* Opening /dev/stdout again would skip what print still buffers, and on Linux it truncates a
       file the shell redirected stdout to. Write through the process's own streams instead. */
    FILE *std=strcmp(path,"/dev/stdout")==0?stdout:strcmp(path,"/dev/stderr")==0?stderr:NULL;
    if(std==stderr) fflush(stdout);
    FILE *f=std?std:fopen(path,"wb");if(!f){free(buf);push_fail(path);free(path);return;}
    size_t n=fwrite(buf,1,len,f); int closed=std?fflush(f):fclose(f);
    if((int)n!=len||closed){free(buf);push_fail(path);free(path);return;}
    free(buf);free(path); spush(val_int(1)); push_ok();
}
static void prim_ls(Frame *e) {
    (void)e; int plen;char *path=pop_string_path("ls",&plen);
    if(path_has_nul(path,plen)){free(path);return;}
    DIR *d=opendir(path); if(!d) { push_fail(path); free(path); return; }
    struct dirent *ent; int base=sp,count=0;
    while((ent=readdir(d))!=NULL){if(strcmp(ent->d_name,".")==0||strcmp(ent->d_name,"..")==0)continue;
        push_c_string(ent->d_name);count++;}
    closedir(d);spush(val_compound(VAL_LIST,count,sp-base+1));free(path); push_ok();
}

#ifndef SLAP_WASM
/* tcp-send/recv/accept consume the socket box and push a fresh one. */
static int pop_socket_fd(const char *who) {
    Value v = spop();
    if (v.tag != VAL_BOX) die("%s: expected socket (box)", who);
    BoxData *bd = (BoxData*)v.as.box;
    if (bd->slots != 1 || bd->data[0].tag != VAL_INT) die("%s: expected a socket box, got a box that does not hold one int", who);
    int fd = (int)bd->data[0].as.i; struct stat st;
    if (fstat(fd, &st) || !S_ISSOCK(st.st_mode)) die("%s: %d is not a socket", who, fd);
    free(bd->data); free(bd);
    return fd;
}
/* A peer that hangs up must make tcp-send return 'no, not raise SIGPIPE. macOS
   has no MSG_NOSIGNAL, and Linux has no SO_NOSIGPIPE. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
static int nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
}
static void push_socket_box(int fd) {
    BoxData *bd=malloc(sizeof(BoxData));bd->data=malloc(sizeof(Value));bd->slots=1;bd->data[0]=val_int(fd);
    Value v;v.tag=VAL_BOX;v.loc=0;v.as.box=bd;spush(v);
}
static void prim_tcp_connect(Frame *e) {
    (void)e; int64_t port=pop_int(); int hlen;char *host=pop_string_path("tcp-connect",&hlen);
    if(port<0||port>65535) die("tcp-connect: port %lld is outside 0-65535",(long long)port);
    if(memchr(host,0,hlen)){free(host);push_fail("tcp-connect: the host contains a NUL byte");return;}
    struct addrinfo hints={0},*res; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    char ps[16]; snprintf(ps,sizeof(ps),"%lld",(long long)port);
    char msg[512]; int err=getaddrinfo(host,ps,&hints,&res);
    if(err){snprintf(msg,sizeof msg,"tcp-connect: cannot resolve %s: %s",host,gai_strerror(err));free(host);push_fail(msg);return;}
    int fd=-1;
    for(struct addrinfo *a=res;a&&fd<0;a=a->ai_next){
        fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol);
        if(fd>=0&&connect(fd,a->ai_addr,a->ai_addrlen)<0){close(fd);fd=-1;}
    }
    if(fd<0){snprintf(msg,sizeof msg,"tcp-connect: cannot connect to %s:%s: %s",host,ps,strerror(errno));freeaddrinfo(res);free(host);push_fail(msg);return;}
    freeaddrinfo(res);free(host);push_socket_box(nosigpipe(fd));push_ok();
}
static void prim_tcp_send(Frame *e) {
    (void)e; int len; unsigned char *buf = pop_byte_list_buf("tcp-send", &len);
    int fd = pop_socket_fd("tcp-send"); push_socket_box(fd);
    size_t sent = 0; while (sent < (size_t)len) { ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL); if (n <= 0) { free(buf); push_fail(strerror(errno)); return; } sent += n; }
    free(buf); spush(val_int(1)); push_ok();
}
static void prim_tcp_recv(Frame *e) {
    (void)e; int64_t maxlen = pop_int(); if (maxlen < 1) die("tcp-recv: length must be at least 1, got %lld", (long long)maxlen);
    int fd = pop_socket_fd("tcp-recv"); push_socket_box(fd);
    static unsigned char buf[65536]; ssize_t n = recv(fd, buf, maxlen < (int64_t)sizeof buf ? (size_t)maxlen : sizeof buf, 0);
    if (n < 0) { push_fail(strerror(errno)); return; }
    push_byte_list(buf, n); push_ok();
}
static void prim_tcp_close(Frame *e) { (void)e; int fd=pop_socket_fd("tcp-close"); if(close(fd)) die("tcp-close: closing socket %d failed: %s", fd, strerror(errno)); }
static void prim_tcp_listen(Frame *e) {
    (void)e; int64_t port=pop_int(); if(port<0||port>65535) die("tcp-listen: port %lld is outside 0-65535",(long long)port);
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0){push_fail(strerror(errno));return;}
    int opt=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=INADDR_ANY;addr.sin_port=htons((uint16_t)port);
    if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0||listen(fd,128)<0){char msg[128];snprintf(msg,sizeof msg,"tcp-listen: port %lld: %s",(long long)port,strerror(errno));close(fd);push_fail(msg);return;}
    push_socket_box(fd);push_ok();
}
static void prim_tcp_accept(Frame *e) {
    (void)e; int sfd=pop_socket_fd("tcp-accept"); struct sockaddr_in ca; socklen_t al=sizeof(ca);
    int cfd=accept(sfd,(struct sockaddr*)&ca,&al); if(cfd<0){push_socket_box(sfd);push_fail(strerror(errno));return;} push_socket_box(sfd); push_socket_box(nosigpipe(cfd)); push_ok();
}
#endif
static inline void prim_strfind_impl(Frame *e, int tagged) {
    (void)e; int nl,hl; unsigned char *needle=pop_byte_list_buf("str-find",&nl); unsigned char *hay=pop_byte_list_buf("str-find",&hl);
    int r=-1; for(int i=0;i<=hl-nl;i++) if(memcmp(hay+i,needle,nl)==0){r=i;break;}
    free(needle);free(hay);
    if(r<0) { if(tagged) push_none(); else die("str-find: not found"); }
    else { spush(val_int(r)); if(tagged) push_ok(); }
}
MUST_PAIR(strfind)
static void prim_str_split(Frame *e) {
    (void)e; int dl,sl; unsigned char *delim=pop_byte_list_buf("str-split",&dl); unsigned char *str=pop_byte_list_buf("str-split",&sl);
    if(dl==0) die("str-split: empty delimiter");
    int base=sp,count=0,pos=0;
    while(pos<=sl){int found=-1;if(dl>0)for(int i=pos;i<=sl-dl;i++)if(memcmp(str+i,delim,dl)==0){found=i;break;}
        if(found<0){push_byte_list(str+pos,sl-pos);count++;break;}
        push_byte_list(str+pos,found-pos);count++;pos=found+dl;}
    spush(val_compound(VAL_LIST,count,sp-base+1));free(delim);free(str);
}
static int memfind(const unsigned char *hay, int hlen, const char *needle, int nlen, int from) {
    for(int i=from;i<=hlen-nlen;i++) if(memcmp(hay+i,needle,nlen)==0) return i; return -1;
}
static void prim_parse_http(Frame *e) {
    (void)e; int rlen; unsigned char *raw=pop_byte_list_buf("parse-http",&rlen);
    int split=memfind(raw,rlen,"\r\n\r\n",4,0);
    if(split<0){free(raw);push_fail("no header/body separator");return;}
    int se=memfind(raw,split,"\r\n",2,0); if(se<0)se=split;
    int sp1=-1; for(int i=0;i<se;i++)if(raw[i]==' '){sp1=i;break;}
    int sc=0;
    for(int i=sp1+1;sp1>=0&&i<sp1+4;i++){ if(i>=se||raw[i]<'0'||raw[i]>'9'){sp1=-1;break;} sc=sc*10+(raw[i]-'0'); }
    if(sp1<0||(sp1+4<se&&raw[sp1+4]!=' ')){free(raw);push_fail("status line has no 3-digit code");return;}
    static uint32_t ks,vs,ss,hs,bs; if(!ks){ks=sym_intern("key");vs=sym_intern("value");ss=sym_intern("status");hs=sym_intern("headers");bs=sym_intern("body");}
    int r0=sp; spush(val_sym(ss)); spush(val_int(sc)); spush(val_sym(hs));
    int rb=sp,hc=0,pos=se+2;
    while(pos<split){
        int le=memfind(raw,split,"\r\n",2,pos); if(le<0)le=split;
        if(le==pos){pos+=2;continue;}
        int colon=-1;for(int i=pos;i<le;i++)if(raw[i]==':'){colon=i;break;}
        if(colon<0){sp=r0;free(raw);push_fail("header line has no colon");return;}
        int kl=colon-pos, vo=colon+1; while(vo<le&&(raw[vo]==' '||raw[vo]=='\t')) vo++;
        int vl=le-vo;
        spush(val_sym(ks));push_byte_list(raw+pos,kl);spush(val_sym(vs));push_byte_list(raw+vo,vl);
        spush(val_compound(VAL_RECORD,2,1+(kl+1)+1+(vl+1)+1));hc++;pos=le+2;
    }
    spush(val_compound(VAL_LIST,hc,sp-rb+1));
    spush(val_sym(bs)); push_byte_list(raw+split+4,rlen-split-4); free(raw);
    spush(val_compound(VAL_RECORD,3,sp-r0+1)); push_ok();
}
static void prim_args(Frame *e) {
    (void)e; int ts=0; for(int i=0;i<cli_argc;i++){push_c_string(cli_args[i]);ts+=(int)strlen(cli_args[i])+1;}
    spush(val_compound(VAL_LIST,cli_argc,ts+1));
}
static void prim_isheadless(Frame *e){(void)e;spush(val_int(headless_mode));}
#define PRIM(nm,body) static void prim_##nm(Frame *e){(void)e;body;}
PRIM(list, spush(val_compound(VAL_LIST,0,1)))
PRIM(rec, spush(val_compound(VAL_RECORD,0,1)))
PRIM(take_n, prim_slice_n(1)) PRIM(drop_n, prim_slice_n(0))
#undef PRIM
#define R(n,f) {#n,prim_##f,NULL}
#define M(n,f) {n,prim_##f,prim_##f##_must}
static void register_prims(void) {
    static struct{const char*n;PrimFn f,m;} t[]={
        R(dup,dup),R(drop,drop),R(swap,swap),R(over,over),R(rot,rot),R(dip,dip),R(apply,apply),
        R(plus,plus),R(sub,sub),R(mul,mul),R(div,div),R(mod,mod),R(divmod,divmod),R(wrap,wrap),
        R(band,band),R(bor,bor),R(bxor,bxor),R(bnot,bnot),R(shl,shl),R(shr,shr),
        R(eq,eq),R(lt,lt),R(and,and),R(or,or),
        R(print,print),R(assert,assert),R(random,random),
        R(if,if),R(case,case),R(while,while),
        R(itof,itof),R(ftoi,ftoi),R(fsqrt,fsqrt),
        R(ffloor,ffloor),R(fround,fround),R(fexp,fexp),R(flog,flog),R(fpow,fpow),R(fatan2,fatan2),
        R(list,list),R(len,size),R(push,push_op),M("pop",pop),
        M("get",get),M("peek",peek),M("nth",nth),M("set",set),R(cat,concat),
        R(reverse,reverse),R(zip,zip),{"take-n",prim_take_n,NULL},{"drop-n",prim_drop_n,NULL},R(range,range),
        R(fold,fold),R(each,each),R(filter,filter),R(sort,sort),M("index-of",indexof),
        R(at,at),R(rec,rec),R(into,into),R(edit,edit),
        R(millis,millis),R(datetime,datetime),R(box,box),R(free,free),R(lend,lend),R(mutate,mutate),
        R(dict,dict),R(insert,insert),R(of,of),R(remove,remove),
        {"dict-keys",prim_keys,NULL},{"dict-entries",prim_entries,NULL},
        R(tag,tag),R(must,must),R(pthen,pthen),
        R(read,read),R(write,write),R(ls,ls),
        M("str-find",strfind),{"str-split",prim_str_split,NULL},{"parse-http",prim_parse_http,NULL},
        R(args,args),R(fail,fail),R(isheadless,isheadless),
#ifndef SLAP_WASM
        {"tcp-connect",prim_tcp_connect,NULL},{"tcp-send",prim_tcp_send,NULL},{"tcp-recv",prim_tcp_recv,NULL},
        {"tcp-close",prim_tcp_close,NULL},{"tcp-listen",prim_tcp_listen,NULL},{"tcp-accept",prim_tcp_accept,NULL},
#endif
#ifdef SLAP_SDL
        R(clear,clear),R(pixel,pixel),{"fill-rect",prim_fill_rect,NULL},R(on,on),R(show,show),
#endif
        {NULL,NULL,NULL}};
    for(int i=0;t[i].n;i++){uint32_t s=sym_intern(t[i].n);prim_fns[s]=t[i].f;prim_must_fns[s]=t[i].m;}
}
#undef R
#undef M

int main(int argc, char **argv) {
    char stack_anchor; c_stack_base = &stack_anchor;
    rng_state=(uint64_t)time(NULL)^((uint64_t)getpid()<<32); atexit(stdout_check);
    int check_only=0, profile=0;
    cli_args=malloc(argc*sizeof(char*)); cli_argc=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--check")==0) check_only=1;
        else if(strcmp(argv[i],"--headless")==0) headless_mode=1;
        else if(strcmp(argv[i],"--profile")==0) profile=1;
        else if(argv[i][0]=='-'&&argv[i][1]=='-'){fprintf(stderr,"unknown flag: %s\nusage: slap [--check] [--headless] [--profile] [args...] < file.slap\n",argv[i]);free(cli_args);return 1;}
        else cli_args[cli_argc++]=argv[i];
    }
    prof_on=profile; syms_init(); register_prims();
    global_frame=frame_new(NULL); Frame *global=global_frame;
    store_source_lines(PRELUDE, FID_PRELUDE);
    lex(PRELUDE, FID_PRELUDE); eval(tokens,tok_count,global);
    current_loc=LOC_PACK(FID_STDIN,0,0);
#ifdef SLAP_WASM
    FILE *f=fopen("program.slap","r"); if(!f){fprintf(stderr,"error: cannot open 'program.slap'\n");return 1;}
#else
    FILE *f=stdin;
#endif
    long sz=0,cap=4096; char *src=malloc(cap); long n;
    while((n=fread(src+sz,1,cap-sz,f))>0){sz+=n;if(sz==cap){cap*=2;src=realloc(src,cap);}}
    src[sz]=0;
#ifdef SLAP_WASM
    fclose(f);
#endif
    if(sz==0){fprintf(stderr,"usage: slap [--check] [--headless] [--profile] [args...] < file.slap\n");return 1;}
    store_source_lines(src, FID_STDIN);
    lex(src, FID_STDIN); int user_tok_count=tok_count;
    static Token user_tokens[TOK_MAX]; memcpy(user_tokens,tokens,user_tok_count*sizeof(Token));
    static Token combined[TOK_MAX]; int cpos=0;
    #define COMBINE(n) if(cpos+(n)>TOK_MAX) die("program too long: prelude, builtins and program need %d tokens, max %d", cpos+(n), TOK_MAX)
    static Token table[TOK_MAX]; store_source_lines(TYPES, FID_BUILTIN); lex(TYPES, FID_BUILTIN);
    int table_count=tok_count; memcpy(table,tokens,table_count*sizeof(Token));
    lex(PRELUDE, FID_PRELUDE); COMBINE(tok_count); memcpy(&combined[cpos],tokens,tok_count*sizeof(Token)); cpos+=tok_count;
    int user_start=cpos;
    COMBINE(user_tok_count); memcpy(&combined[cpos],user_tokens,user_tok_count*sizeof(Token)); cpos+=user_tok_count;
    int errors=infer_program(table,table_count,combined,cpos,user_start);
    if(errors>0){fprintf(stderr,"%d type error(s)\n",errors);return 1;}
    if(check_only){fprintf(stderr,"type check passed\n");return 0;}
    for(int i=0;i<user_tok_count;i++){Token*t=&user_tokens[i]; uint32_t w=t->as.sym;
        if(t->tag!=TOK_WORD||prim_fns[w]||!(ty_builtin[w]||w==S_ON||w==S_SHOW)) continue;
        current_loc=LOC_PACK(t->fid,t->line,t->col);
#ifdef SLAP_WASM
        die("'%s' is not in the wasm build: a browser has no TCP sockets. Remove the tcp-* words from this program.",sym_name(w));
#else
        die("'%s' needs the SDL build, but this is the terminal build. Build it with make slap-sdl and run ./slap-sdl.",sym_name(w));
#endif
    }
    current_loc=LOC_PACK(FID_STDIN,0,0);
    /* Registered after stdout_check, so it runs first, on die too. */
    if(profile){ prof_last=prof_now(); atexit(prof_report); }
    eval(user_tokens,user_tok_count,global);
    return 0;
}
