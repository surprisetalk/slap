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
#include <sys/resource.h>
#include <signal.h>
#include <limits.h>
#include <setjmp.h>
#ifndef SLAP_WASM
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>
#include <fcntl.h>
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
                       VF_LIST or VF_REC on a tuple header: it is a [...] or {...} literal's code, which runs where it
                       is written and leaves its values in one list or record. It is never pushed as a body. */
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
static int val_slots(Value v) {
    if (is_compound(v.tag)) {
        int s = (int)v.as.compound.slots;
        /* The checker makes this unreachable, but deleting it slows the 600 KB feed by 12%:
           the compiler then lays out val_slots' hot callers differently. */
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
    free(src_text[fid]); free(src_lines[fid]); /* the shell stores its whole history again for every line */
    src_text[fid] = strdup(src); if (!src_text[fid]) { fprintf(stderr, "slap: out of memory storing %zu bytes of source\n", strlen(src)); exit(1); }
    int count = 1; for (const char *p = src_text[fid]; *p; p++) if (*p == '\n') count++;
    src_lines[fid] = malloc(count * sizeof(char *)); src_line_count[fid] = 0;
    char *p = src_text[fid];
    while (*p) { src_lines[fid][src_line_count[fid]++] = p; char *nl = strchr(p, '\n'); if (nl) { *nl = '\0'; p = nl + 1; } else break; }
}
static void print_source_line(FILE *out, int fid, int line, int col) {
    if (line==0) return;
    if (fid<0||fid>=SRC_MAX||!src_lines[fid]||line<1||line>src_line_count[fid]) { fprintf(out,"    (source unavailable)\n"); return; }
    const char *ln = src_lines[fid][line - 1]; size_t len = strlen(ln), from = 0, to = len, c0 = col > 0 ? (size_t)col - 1 : 0;
    if (len > 200) { from = c0 > 100 ? c0 - 100 : 0; if (from + 200 < len) to = from + 200; else from = len - 200; }
    fprintf(out, "    %4d| ", line); if (from) fprintf(out, "[%zu bytes cut] ", from);
    fwrite(ln + from, 1, to - from, out); if (to < len) fprintf(out, " [%zu bytes cut]", len - to); fputc('\n', out);
    if (col > 0) { fprintf(out, "          "); if (from) fprintf(out, "[%zu bytes cut] ", from); for (size_t i = from; i + 1 < (size_t)col; i++) fputc(' ', out); fprintf(out, "^^^\n"); }
}
static uint64_t user_loc;
/* Set while the shell runs a line: an error discards the line instead of ending the process. */
static jmp_buf *shell_jmp;
static int dying;
/* The sockets the shell's line opened (fd+1) and closed (-(fd+1)). A kept line closes what it closed
   then; a discarded line closes only what it opened, so each socket on the restored stack stays open. */
static int *shell_socks, shell_socks_n, shell_socks_cap;
static void shell_sock(int ev) {
    if (shell_socks_n == shell_socks_cap) { shell_socks_cap = shell_socks_cap ? 2*shell_socks_cap : 16;
        shell_socks = realloc(shell_socks, (size_t)shell_socks_cap * sizeof *shell_socks);
        if (!shell_socks) die("shell: out of memory noting %d sockets", shell_socks_cap); }
    shell_socks[shell_socks_n++] = ev;
}
__attribute__((noreturn))
static void die(const char *fmt, ...) {
    if (LOC_FID(current_loc) == FID_PRELUDE && user_loc) current_loc = user_loc;
    int fid = LOC_FID(current_loc), line = LOC_LINE(current_loc), col = LOC_COL(current_loc);
    const char *f = src_files[fid];
    va_list ap; va_start(ap, fmt);
    if (col > 0) fprintf(stderr, "\n-- ERROR %s:%d:%d ", f, line, col);
    else if (line) fprintf(stderr, "\n-- ERROR %s:%d ", f, line);
    else fprintf(stderr, "\n-- ERROR %s ", f);
    int hl = 10+(int)strlen(f)+10; for(int i=hl;i<60;i++) fputc('-',stderr);
    fprintf(stderr, "\n\n    "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n\n");
    print_source_line(stderr, fid, line, col); va_end(ap);
    if (!dying) { dying = 1; print_stack_summary(stderr); fprintf(stderr, "\n"); }
    if (shell_jmp) { dying = 0; longjmp(*shell_jmp, 1); }
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
#define VF_LIST 2u
#define VF_REC 4u
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
static int tok_limit = TOK_MAX; /* the program's share of TOK_MAX: the prelude takes the rest */
#define LEX_ADVANCE() do { if (*p == '\n') { line++; col = 1; } else { col++; } p++; } while (0)
static void lex(const char *src, int fid) {
    tok_count = 0; int line = 1; int col = 1; const char *p = src;
    static int open_at[TOK_MAX]; int depth = 0;
    while (*p) {
        if (*p == '\n') { line++; col = 1; p++; continue; }
        if (isspace((unsigned char)*p)) { col++; p++; continue; }
        if (p[0] == '-' && p[1] == '-') { while (*p && *p != '\n') { col++; p++; } continue; }
        if (tok_count >= tok_limit) { current_loc = LOC_PACK(fid, 0, 0); if (fid != FID_STDIN) die("a built-in source has more than %d tokens; the limit is TOK_MAX. Raise TOK_MAX.", tok_limit);
            if (shell_jmp) die("the shell's lines hold more than %d tokens, the most a program holds. Start a new session.", tok_limit);
            die("the program has more than %d tokens; the limit is %d (TOK_MAX minus %d for the prelude). Split the program.", tok_limit, tok_limit, TOK_MAX-tok_limit); }
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
                if (!isfinite(t->as.f)) die("float literal at line %d does not fit in 64 bits: its magnitude is past 1.8e308", t->line);
                if (t->as.f == 0) for (const char *q = start; q < p; q++) if (*q >= '1' && *q <= '9')
                    die("float literal at line %d is too small for 64 bits: it reads as 0.0. Write 0.0 if you mean zero.", t->line);
            } else { errno = 0; t->tag = TOK_INT; t->as.i = strtoll(start, NULL, 10);
                if (errno == ERANGE) die("integer literal at line %d does not fit in 64 bits", t->line); }
            if (*p && !isspace((unsigned char)*p) && !strchr("()[]{}", *p)) {
                const char *e = p; while (*e && !isspace((unsigned char)*e) && !strchr("()[]{}", *e)) e++;
                die("`%.*s` at line %d starts like a number, but a number ends at a space or a bracket, and slap has no hex, "
                    "exponent or digit-separator syntax. Write a space after the number, or start the word with a letter.", (int)(e - start), start, t->line);
            }
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
   frame f's own tuples, which a binding in f holds weakly. Boxes are left alone: the checker refuses a
   binding of a box. */
static inline __attribute__((always_inline)) void binding_release(Frame *f, Binding *b) {
    if (!b->tuples && !b->heap) { b->slots = 0; return; }
    for (int i = 0; i < b->slots; i++) {
        Value *v = &b->vals[i];
        if (v->tag == VAL_TUPLE && v->as.compound.env != f) frame_drop(v->as.compound.env);
        else if (v->tag == VAL_DICT && b->heap) deep_free_values(v, 1);
    }
    b->heap = 0; b->tuples = 0; b->slots = 0;
}
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
    if (slots == 1) b->vals[0] = vals[0]; else VCPY(b->vals, vals, slots);
    b->slots = slots; b->word = word; b->heap = 0; b->tuples = 0;
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
/* A run's frame that only the run holds goes straight back to the pool. Keep it noinline: inlined,
   it makes clang lay out eval_run's hot loop worse, and zoom.slap slows. */
__attribute__((noinline)) static void frame_end_run(Frame *f) {
    if(f->refs!=1||f->hash){ frame_drop(f); return; }
    Frame *p=f->parent;
    for(int i=f->bind_count-1;i>=0;i--) binding_release(f,&f->bindings[i]);
    f->bind_count=0; f->refs=0; f->parent=frame_pool; frame_pool=f;
    frame_drop(p);
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
    Frame *f=frame_acquire(ee); eval_run(body,slots,f); frame_end_run(f);
}
static inline void dispatch_word(uint32_t sym, Frame *env);
/* Primitives by symbol id; the second table holds the fused `X must` variant. */
static PrimFn prim_fns[SYM_MAX], prim_must_fns[SYM_MAX];
static int64_t pop_int(void) { return spop().as.i; }
static double pop_float(void) { return spop().as.f; }
static uint32_t pop_sym(void) { return spop().as.sym; }
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
        if (data[key_pos].as.sym == key) { ref.base = val_base; ref.slots = vsize; *found = 1; return ref; }
        elem_end = key_pos;
    }
    return ref;
}
static int eval_depth = 0;
#define EVAL_DEPTH_MAX 10000
/* Set at startup: the stack limit less 1 MiB, which leaves room for die() to report, and at most 7 MiB. */
static long c_stack_max;
static char *c_stack_base = NULL;
static void c_stack_check(const char *what) {
    char probe; long used = !c_stack_base ? 0 : &probe > c_stack_base ? &probe - c_stack_base : c_stack_base - &probe;
    if(used > c_stack_max)
        die("C stack exhausted %s -- %ld KB used, limit %ld KB.\n"
            "  Each nested word call or nesting level keeps a C frame alive. Rewrite deep recursion\n"
            "  as a `while` loop, or flatten the data.", what, used/1024, c_stack_max/1024);
}
static void val_print(Value *data, int slots, FILE *out);
/* Error reports show at most this many elements per compound and 200 bytes per string; 0 means all. */
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
/* The shortest of %.15g, %.16g and %.17g that reads back as f, with a point so it never reads as an int.
   NaN prints as nan whatever its sign: glibc prints a negative one as -nan, macOS as nan. */
static void float_text(char *b, size_t cap, double f) {
    if (isnan(f)) { snprintf(b, cap, "nan"); return; }
    for (int p = 15; p <= 17; p++) { snprintf(b, cap, "%.*g", p, f); if (strtod(b, NULL) == f) break; }
    if (!strpbrk(b, ".eni")) strncat(b, ".0", cap - strlen(b) - 1);
}
static void val_print_node(Value *data, int slots, FILE *out) {
    Value top = data[slots - 1];
    switch (top.tag) {
    case VAL_INT: fprintf(out, "%lld", (long long)top.as.i); break;
    case VAL_FLOAT: { char b[40]; float_text(b,sizeof b,top.as.f); fputs(b,out); break; }
    case VAL_SYM: fprintf(out, "'%s", sym_name(top.as.sym)); break;
    case VAL_XT: fprintf(out, "%s", sym_name(top.as.xt.sym)); break;
    case VAL_LIST: {
        int len=(int)top.as.compound.len, is_str=len>0&&slots==len+1;
        for(int i=0;is_str&&i<len;i++){Value v=data[i];if(v.tag!=VAL_INT||v.as.i<32||v.as.i>126)is_str=0;}
        if(is_str){int shown=print_max&&len>200?200:len; fputc('"',out);for(int i=0;i<shown;i++)fputc((char)data[i].as.i,out);fputc('"',out);
            if(shown<len) fprintf(out," ...%d more bytes",len-shown); break;}
        print_elems(data,slots,len,'[',']',out); break;
    }
    case VAL_TUPLE: { const char *br=top.flags&VF_LIST?"[]":top.flags&VF_REC?"{}":"()"; print_elems(data,slots,(int)top.as.compound.len,br[0],br[1],out); break; }
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
        if ((atop.flags ^ btop.flags) & (VF_LIST|VF_REC)) return 0;
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
    return atop.tag == VAL_INT ? atop.as.i < btop.as.i : atop.as.f < btop.as.f;
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
       K_SVAR, K_SNIL, K_SCONS, K_RVAR, K_RNIL, K_REXT, K_LSYM, K_TVAR, K_TNIL, K_TEXT };
/* Every symbol has the one type K_SYM, and two of them always unify. A K_SYM that a symbol literal made
   keeps the literal's name plus one in `sym`; `{...}` keys and `on` read it, and `nth` reads its name from
   the token before it. Record keys are K_LSYM. */
/* A row field is the type of its key's value. A closed row (K_RNIL) has no other key. A record has the
   keys its literal names: `into` replaces the value of a key the record has, and never adds one. */
/* What a value variable must be: protocols a word asks of its inputs. */
/* copy: the value may be copied, dropped, bound or stored. A box is not, nor a result or tag that
   holds one; the stack carries it from the word that makes it to the word that frees it. */
/* P_NOFN: holds no body (ty_no_body). */
enum { P_COPY = 1, P_NOFN = 2 };
/* rigid: a signature's variable while a body is checked against it. sealed: the stack below a body
   that must not reach it (`each`, `edit`); instances keep it. */
typedef struct { uint8_t kind, prot, rigid, sealed; int level, a, b, c, link; uint32_t sym; } Ty;
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
    ty[ty_n] = (Ty){(uint8_t)kind, 0, 0, 0, ty_level, a, b, c, 0, 0};
    return ty_n++;
}
static int ty_sym(int kind, uint32_t s) { int t = ty_new(kind, 0, 0, 0); ty[t].sym = s; return t; }
static int ty_isvar(int k) { return k == K_VAR || k == K_SVAR || k == K_RVAR || k == K_TVAR; }
/* The name a symbol literal wrote into a K_SYM, or -1. */
static int ty_find(int t);
static int ty_sym_name(int t) { t = ty_find(t); return ty[t].kind == K_SYM ? (int)ty[t].sym - 1 : -1; }
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
    case K_INT: case K_FLOAT: case K_SYM: case K_FN: return P_COPY;
    case K_LIST: case K_DICT: case K_REC: return P_COPY;
    default: return 0;
    }
}

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
    case K_VAR: { ty_print_var(out, cap, t, '\''); size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%s", ty[t].prot & P_COPY ? " copyable" : "", ty[t].prot & P_NOFN ? " no-body" : ""); return; }
    case K_INT: snprintf(out, cap, "int"); return;
    case K_FLOAT: snprintf(out, cap, "float"); return;
    case K_SYM: snprintf(out, cap, "sym"); return;
    case K_LIST: { int e = ty_find(ty[t].a); if (ty[t].sym && ty[e].kind == K_INT) { snprintf(out, cap, "str"); return; } ty_show(a, sizeof a, e, depth + 1); snprintf(out, cap, "%s list", a); return; }
    case K_DICT: ty_show(a, sizeof a, ty[t].a, depth + 1); snprintf(out, cap, "%s dict", a); return;
    case K_BOX: ty_show(a, sizeof a, ty[t].a, depth + 1); snprintf(out, cap, "%s box", a); return;
    case K_SOCK: snprintf(out, cap, "socket"); return;
    case K_TVAR: { char v[16]; ty_print_var(v, sizeof v, t, '.'); snprintf(out, cap, "tagged | .%s", v); return; }
    case K_RVAR: { char v[16]; ty_print_var(v, sizeof v, t, '.'); snprintf(out, cap, "{| .%s}", v); return; }
    case K_FN: ty_show_stack(a, sizeof a, ty[t].a, depth); ty_show_stack(b, sizeof b, ty[t].b, depth); snprintf(out, cap, "( %s%s->%s%s )", a, *a ? " " : "", *b ? " " : "", b); return;
    case K_RES: ty_show(a, sizeof a, ty[t].a, depth + 1); ty_show(b, sizeof b, ty[t].b, depth + 1); snprintf(out, cap, "{'ok %s 'no %s} either", a, b); return;
    case K_TAG: case K_TEXT: case K_TNIL: {
        size_t len = 0; int r = ty[t].kind == K_TAG ? ty_find(ty[t].a) : t; ty_put(out, cap, &len, "tagged");
        for (int hops = 0; ty[r].kind == K_TEXT; r = ty_find(ty[r].c), hops++) { if (hops == ty_n) die("type checker bug: a tag set links in a cycle"); ty_put(out, cap, &len, " '%s", sym_name(ty[r].sym)); }
        if (ty[r].kind == K_TVAR) { char v[16]; ty_print_var(v, sizeof v, r, '.'); ty_put(out, cap, &len, " | .%s", v); }
        return; }
    case K_REC: case K_REXT: case K_RNIL: {
        size_t len = 0; int r = ty[t].kind == K_REC ? ty_find(ty[t].a) : t, n = 0; ty_put(out, cap, &len, "{");
        for (int hops = 0; ty[r].kind == K_REXT; hops++) {
            if (hops == ty_n) die("type checker bug: a row links in a cycle");
            ty_show(a, sizeof a, ty[r].b, depth + 1);
            ty_put(out, cap, &len, "%s'%s %s", n ? " " : "", sym_name(ty[ty[r].a].sym), a); n++;
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
static int ty_no_body(int t);
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
        int n = 0, x = ty_find(t); for (; ty[x].kind == K_SCONS && n < ty_n; x = ty_find(ty[x].b)) n++;
        /* The stack does not end in v, so v is inside a value on it: a body's type that names this stack. */
        if (ty[v].kind == K_SVAR && n && x != v) snprintf(ty_why, sizeof ty_why, "a copy of the body lies below it on the stack. dup, or a second lookup of a body bound with let, gives both copies one type, so one copy cannot run while the other lies below it");
        else if (ty[v].kind == K_SVAR && n) ty_depth_why = 1, snprintf(ty_why, sizeof ty_why, "one path leaves %d more value%s on the stack than the other, so a branch, clause, loop pass or recursive call changes the stack's depth", n, n == 1 ? "" : "s");
        else if (ty[v].kind == K_RVAR) snprintf(ty_why, sizeof ty_why, "a record would have to contain itself: one path adds a field the other has not");
        else { char s[256]; ty_show(s, sizeof s, t, 0); snprintf(ty_why, sizeof ty_why, "a value would have to contain itself, as %s", s); }
        return 1; }
    if ((ty[v].kind == K_VAR || ty[v].kind == K_TVAR || ty[v].kind == K_RVAR) && ty[v].prot) {
        if (ty[t].kind == ty[v].kind && ty_fixed(t) && (ty[v].prot & ~ty[t].prot)) {
            snprintf(ty_why, sizeof ty_why, ty[v].prot & ~ty[t].prot & P_COPY ? "the body needs a copyable value where the signature allows any type"
                : "a value bound with let after a body was made in its scope cannot hold a body, but the signature allows one here"); return 1; }
        if (ty[t].kind == ty[v].kind) ty[t].prot |= ty[v].prot;
        else {
            int need = ty[v].prot;
            /* before the link, so a message shows the variable that refused t */
            if ((need & P_NOFN) && ty_no_body(t)) return 1;
            need &= ~P_NOFN;
            ty[v].link = t;
            if ((need & P_COPY) && (ty[t].kind == K_RES || ty[t].kind == K_TAG || ty[t].kind == K_TEXT || ty[t].kind == K_TNIL)) {
                if (ty_copy_parts(t)) return 1;
                need &= ~P_COPY; }
            if ((ty_prot_of(ty[t].kind) & need) != need) {
                char s[256]; ty_show(s, sizeof s, t, 0);
                if (ty[t].kind == K_BOX) snprintf(ty_why, sizeof ty_why, "%s is a box: it cannot be copied, dropped, bound or stored. Free it, or pass it to a word that takes it", s);
                else snprintf(ty_why, sizeof ty_why, "%s is not copyable", s);
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
/* t holds no body. A let binds such a value after its scope made a body (ty_taint): a body made in a
   scope keeps that scope's names alive, so a binding there that holds one could keep itself alive and
   never be freed. Variables, open rows and open tag sets take P_NOFN, so a later binding cannot add a
   body either. A term's own P_NOFN marks it checked, so a tag whose payload holds the same tag ends. */
static int ty_no_body(int t) {
    c_stack_check("while checking what a value holds");
    t = ty_find(t);
    if (ty[t].prot & P_NOFN) return 0;
    switch (ty[t].kind) {
    case K_FN: snprintf(ty_why, sizeof ty_why, "a value bound with let after a body was made in its scope cannot hold a body, since the binding could keep its own scope alive"); return 1;
    case K_VAR: case K_TVAR: case K_RVAR:
        if (ty_fixed(t)) { snprintf(ty_why, sizeof ty_why, "a value bound with let after a body was made in its scope cannot hold a body, but the signature allows one here"); return 1; }
        ty[t].prot |= P_NOFN; return 0;
    case K_LIST: case K_DICT: case K_BOX: case K_REC: case K_TAG: ty[t].prot |= P_NOFN; return ty_no_body(ty[t].a);
    case K_RES: ty[t].prot |= P_NOFN; return ty_no_body(ty[t].a) || ty_no_body(ty[t].b);
    case K_REXT: ty[t].prot |= P_NOFN; return ty_no_body(ty[t].b) || ty_no_body(ty[t].c);
    case K_TEXT: ty[t].prot |= P_NOFN; return ty_no_body(ty_tag_payload(ty[t].sym)) || ty_no_body(ty[t].c);
    default: return 0;
    }
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
    if (ty[a].kind == K_SYM && ty[b].kind == K_SYM) { if (ty[a].sym != ty[b].sym) ty[a].sym = ty[b].sym = 0; return 0; } /* two names met: the value may be either */
    /* a closed tag set meets what is left of a set with more tags */
    if (ty[a].kind == K_TNIL && ty[b].kind == K_TEXT) { snprintf(ty_why, sizeof ty_why, "it may be tagged '%s, which is not one of the tags this takes", sym_name(ty[b].sym)); return 1; }
    if (ty[a].kind == K_TEXT && ty[b].kind == K_TNIL) { snprintf(ty_why, sizeof ty_why, "this value is never tagged '%s", sym_name(ty[a].sym)); return 1; }
    if (ty[a].kind != ty[b].kind && !(ty[a].kind == K_REXT && ty[b].kind == K_RNIL)) {
        char s1[256], s2[256];
        if (ty[a].kind == K_SNIL || ty[b].kind == K_SNIL) ty_depth_why = 1, snprintf(ty_why, sizeof ty_why, "the stack is shorter than this needs");
        else {
            ty_show(s2, sizeof s2, b, 0);
            if (!ty_kind_noun(ty[a].kind)) ty_show(s1, sizeof s1, a, 0);
            snprintf(ty_why, sizeof ty_why, "%s is not %s%s", s2, ty_kind_noun(ty[a].kind) ? ty_kind_noun(ty[a].kind) : s1,
                      ty[a].kind == K_LIST && ty[b].kind == K_DICT ? ". Iterate a dict with dict-entries: it leaves the dict and a list of {'key k 'value v}" : "");
        }
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
        if (f && ty_unify_at(ty[a].b, f, depth + 1)) return 1;
        a = ty[a].c; b = rest; continue;
    }
    default: {
        /* once equal, a stands for b: a pair two types share is unified once */
        int na = ty[a].b, nb = ty[b].b;
        if (ty[a].a && ty_unify_at(ty[a].a, ty[b].a, depth + 1)) return 1;
        if (ty[a].c && ty_unify_at(ty[a].c, ty[b].c, depth + 1)) return 1;
        /* A str and an int list are one type; linking them would make one print as the other. */
        if (ty[a].kind == K_LIST && ty[a].sym != ty[b].sym) { if (!na) return 0; a = na; b = nb; continue; }
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
    if (ty[r].kind == K_RNIL) { snprintf(ty_why, sizeof ty_why, "one record has '%s and the other has not", sym_name(label)); return 1; }
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
    "'plus ( int int -> int ) 'sub ( int int -> int ) 'mul ( int int -> int ) 'div ( int int -> int )\n"
    "'fplus ( float float -> float ) 'fsub ( float float -> float ) 'fmul ( float float -> float ) 'fdiv ( float float -> float )\n"
    "'mod ( int int -> int ) 'band ( int int -> int ) 'bor ( int int -> int ) 'bxor ( int int -> int )\n"
    "'shl ( int int -> int ) 'shr ( int int -> int ) 'and ( int int -> int ) 'or ( int int -> int )\n"
    "'eq ( 'a 'a -> int ) 'lt ( int int -> int ) 'flt ( float float -> int )\n"
    "'itof ( int -> float ) 'ftoi ( float -> int ) 'float-str ( float -> str ) 'float-bits ( float -> int ) 'bits-float ( int -> float ) 'fsqrt ( float -> float ) 'ffloor ( float -> float ) 'fround ( float -> float )\n"
    "'fexp ( float -> float ) 'flog ( float -> float ) 'fpow ( float float -> float ) 'fatan2 ( float float -> float )\n"
    "'print ( 'a -> ) 'assert ( int -> ) 'millis ( -> int ) 'datetime ( -> int list ) 'random ( int -> int ) 'isheadless ( -> int )\n"
    "'apply ( ..s ( ..s -> ..t ) -> ..t ) 'dip ( ..s 'x ( ..s -> ..t ) -> ..t 'x )\n"
    "'if ( ..s int ( ..s -> ..t ) ( ..s -> ..t ) -> ..t ) 'while ( ..a ( ..a -> ..b int ) ( ..b -> ..a ) -> ..b )\n"
    "'each ( ..s 'a list ( ..!r 'a -> ..!r 'b ) -> ..s 'b list ) 'fold ( ..s 'a list 'b ( ..!r 'b 'a -> ..!r 'b ) -> ..s 'b )\n"
    "'filter ( ..s 'a list ( ..!r 'a -> ..!r int ) -> ..s 'a list )\n"
    "'len ( 'a list -> int ) 'push ( 'a list 'a -> 'a list ) 'pop ( 'a list -> 'a list {'ok 'a 'no ()} either )\n"
    "'get ( 'a list int -> {'ok 'a 'no ()} either ) 'peek ( 'a list int -> 'a list {'ok 'a 'no ()} either )\n"
    "'set ( 'a list int 'a -> {'ok 'a list 'no ()} either ) 'cat ( 'a list 'a list -> 'a list ) 'reverse ( 'a list -> 'a list )\n"
    "'take-n ( 'a list int -> 'a list ) 'drop-n ( 'a list int -> 'a list ) 'range ( int int -> int list ) 'sort ( int list -> int list )\n"
    "'index-of ( 'a list 'a -> {'ok int 'no ()} either ) 'zip ( 'a list 'a list -> 'a list list )\n"
    "'str-find ( str str -> {'ok int 'no ()} either ) 'str-split ( str str -> str list )\n"
    "'must ( {'ok 'a 'no 'b} either -> 'a ) 'fail ( ..a str -> ..b )\n"
    "'pthen ( ..s {'ok 'a 'no 'b} either 'd copy ( ..s 'a -> ..s 'd {'ok 'c 'no 'b} either ) -> ..s 'd {'ok 'c 'no 'b} either )\n"
    "'box ( 'a -> 'a box ) 'free ( 'a box -> ) 'mutate ( ..s 'a box ( ..!r 'a -> ..!r 'b ) -> ..s 'b box ) 'lend ( ..s 'a box ( ..!r 'a -> ..!r 'b ) -> ..s 'a box 'b )\n"
    "'dict ( -> 'a dict ) 'insert ( 'a dict str 'a -> 'a dict ) 'of ( 'a dict str -> 'a dict {'ok 'a 'no str} either )\n"
    "'remove ( 'a dict str -> 'a dict ) 'dict-entries ( 'a dict -> 'a dict {'key str 'value 'a} list )\n"
    "'read ( str -> {'ok str 'no str} either ) 'write ( str str -> {'ok int 'no str} either ) 'ls ( str -> {'ok str list 'no str} either )\n"
    "'args ( -> str list )\n"
    /* a socket is its own type: the runtime keeps it as a plain int fd, but free, lend and mutate must not reach it */
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
    return !strcmp(w, "copy") ? P_COPY : 0;
}
static int ty_parse_fn(Token *toks, int open, int close, TyNames *nm, int rest);
/* While a slot signature parses, the word's own stack rest: a body type in a slot that names no rest of
   its own runs on it, and a body type inside that one runs on its enclosing body's. 0 elsewhere. */
static int ty_slot_rest;
static void ty_mark_copy(int t, Token *tok);
#define TY_DIE(t, ...) do { current_loc = LOC_PACK((t)->fid, (t)->line, (t)->col); die(__VA_ARGS__); } while (0)
static int ty_parse(Token *toks, int *i, int end, TyNames *nm) {
    c_stack_check("while reading a type annotation");
    if (*i >= end) TY_DIE(&toks[end-1], "type annotation: a type is missing");
    Token *t = &toks[*i]; int base = 0;
    if (t->tag == TOK_WORD) {
        const char *w = sym_name(t->as.sym); int p = ty_prot_word(t);
        if (!strcmp(w, "int")) base = ty_new(K_INT, 0, 0, 0);
        else if (!strcmp(w, "float")) base = ty_new(K_FLOAT, 0, 0, 0);
        else if (!strcmp(w, "sym")) base = ty_new(K_SYM, 0, 0, 0);
        else if (!strcmp(w, "str")) { base = ty_new(K_LIST, ty_new(K_INT, 0, 0, 0), 0, 0); ty[base].sym = 1; }
        else if (!strcmp(w, "tagged")) base = ty_new(K_TAG, ty_new(K_TVAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "rec")) base = ty_new(K_REC, ty_new(K_RVAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "tuple")) base = ty_new(K_FN, ty_new(K_SVAR, 0, 0, 0), ty_new(K_SVAR, 0, 0, 0), 0);
        else if (!strcmp(w, "list") || !strcmp(w, "seq")) base = ty_new(K_LIST, ty_new(K_VAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "dict")) base = ty_new(K_DICT, ty_new(K_VAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "box")) base = ty_new(K_BOX, ty_new(K_VAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "socket")) base = ty_new(K_SOCK, 0, 0, 0);
        else if (p) { base = ty_new(K_VAR, 0, 0, 0); ty[base].prot = (uint8_t)p; }
        else if (ty_word_is(t, "|")) TY_DIE(t, "type annotation: a type is missing before '|'. Write a type after each field name, as in {'a int | 'r}.");
        else TY_DIE(t, "type annotation: unknown type word '%s'. A type is int, float, sym, str, tagged, rec, tuple, list, dict, box, socket, a 'name, or a ( ) or { } form.", w);
        (*i)++;
    } else if (t->tag == TOK_SYM) {
        if (*i + 1 < end && ty_word_is(&toks[*i + 1], "sym")) TY_DIE(t, "type annotation: a symbol's type is sym; write sym, not '%s sym", sym_name(t->as.sym));
        { base = ty_named(nm, t->as.sym, K_VAR); (*i)++;
            for (int p; *i < end && (p = ty_prot_word(&toks[*i])); (*i)++) ty[base].prot |= (uint8_t)p; }
    } else if (t->tag == TOK_LPAREN) {
        base = ty_parse_fn(toks, *i, *i + t->span, nm, ty_slot_rest); *i += t->span + 1;
    } else if (t->tag == TOK_LBRACE) {
        int close = *i + t->span, j = *i + 1; uint32_t keys[64], rest_sym = 0; int types[64], n = 0;
        while (j < close) {
            if (ty_word_is(&toks[j], "|")) { if (rest_sym) { TY_DIE(&toks[j], "type annotation: a record or either type takes one '|' row name, but a second '|' follows '%s. Drop one.", sym_name(rest_sym)); }
                if (j + 1 >= close || toks[j+1].tag != TOK_SYM) TY_DIE(&toks[j + (j + 1 < close)], "type annotation: '|' needs a 'row name after it");
                rest_sym = toks[j+1].as.sym; j += 2; continue; }
            if (toks[j].tag != TOK_SYM) TY_DIE(&toks[j], "type annotation: a record or either type takes 'name type pairs");
            if (n == 64) TY_DIE(&toks[j], "type annotation: more than 64 fields");
            for (int m = 0; m < n; m++) if (keys[m] == toks[j].as.sym) TY_DIE(&toks[j], "type annotation: '%s appears twice in one record or either type", sym_name(toks[j].as.sym));
            keys[n] = toks[j].as.sym; j++; types[n++] = ty_parse(toks, &j, close, nm);
        }
        *i = close + 1;
        if (*i < end && ty_word_is(&toks[*i], "either")) {
            (*i)++; int okno = n > 0, ok = 0, no = 0;
            for (int k = 0; k < n; k++) { if (keys[k] == S_OK) ok = types[k]; else if (keys[k] == S_NO) no = types[k]; else okno = 0; }
            if (okno && rest_sym) TY_DIE(t, "type annotation: {'ok ... 'no ...} either is a result, which holds only 'ok and 'no: drop | '%s", sym_name(rest_sym));
            if (okno) base = ty_new(K_RES, ok ? ok : ty_new(K_VAR, 0, 0, 0), no ? no : ty_new(K_VAR, 0, 0, 0), 0);
            else { int row = rest_sym ? ty_named(nm, rest_sym, K_TVAR) : ty_new(K_TNIL, 0, 0, 0);
                for (int k = n - 1; k >= 0; k--) { if (ty_unify(types[k], ty_tag_payload(keys[k]))) die("type annotation: tag '%s here conflicts with its payload elsewhere: %s", sym_name(keys[k]), ty_why);
                    row = ty_new(K_TEXT, 0, 0, row); ty[row].sym = keys[k]; }
                base = ty_new(K_TAG, row, 0, 0); }
        } else {
            int row = rest_sym ? ty_named(nm, rest_sym, K_RVAR) : ty_new(K_RNIL, 0, 0, 0);
            for (int k = 0; k < n; k++) row = ty_new(K_REXT, ty_sym(K_LSYM, keys[k]), types[k], row);
            base = ty_new(K_REC, row, 0, 0);
        }
    } else TY_DIE(t, "type annotation: a type is expected");
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
        TY_DIE(&toks[open], "type annotation: a body type needs ->, as in ( int -> int )"); }
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
        if (k + 1 > close) TY_DIE(&toks[j], "type annotation: each slot ends with own/lent/copy/move/auto and in/out");
        int t = j < k ? ty_parse(toks, &j, k, nm) : ty_new(K_VAR, 0, 0, 0);
        /* a lent or copy slot is copyable; an own, move or auto one may hold a box */
        if (ty_word_is(&toks[k], "lent") || ty_word_is(&toks[k], "copy")) ty_mark_copy(t, &toks[k]);
        /* A word after the type: reading it as a type names it when it is no type word. */
        if (j < k && toks[j].tag == TOK_WORD && !ty_word_is(&toks[j], "|")) { int q = j; ty_parse(toks, &q, k, nm); }
        if (j < k) TY_DIE(&toks[j], "type annotation: a slot holds one type, then own/lent/copy/move/auto and in/out, but %s%s follows the type", toks[j].tag == TOK_SYM ? "'" : "", toks[j].tag == TOK_WORD || toks[j].tag == TOK_SYM ? sym_name(toks[j].as.sym) : "a value");
        if (ty_word_is(&toks[k+1], "in")) in = ty_new(K_SCONS, t, in, 0);
        else if (ty_word_is(&toks[k+1], "out")) out = ty_new(K_SCONS, t, out, 0);
        else TY_DIE(&toks[k+1], "type annotation: a slot ends with in or out");
        j = k + 2;
    }
    ty_slot_rest = slot;
    return ty_new(K_FN, in, out, 0);
}
static void ty_held_copy(int t, Token *tok);
/* A signature says this type is copyable: its variables and open tag sets are, and so are the payloads
   of the tags it names. */
static void ty_mark_copy(int t, Token *tok) {
    t = ty_find(t);
    if (ty[t].kind == K_VAR || ty[t].kind == K_TVAR) { ty[t].prot |= P_COPY; return; }
    if (ty[t].kind == K_RES) { ty_mark_copy(ty[t].a, tok); ty_mark_copy(ty[t].b, tok); return; }
    if (ty[t].kind != K_TAG) return;
    int r = ty_find(ty[t].a);
    for (int hops = -ty_n; ty[r].kind == K_TEXT; r = ty_rest(r, &hops))
        if (ty_need(ty_tag_payload(ty[r].sym), P_COPY)) TY_DIE(tok, "type annotation: this slot is copyable, but tag '%s holds %s", sym_name(ty[r].sym), ty_why);
    if (ty[r].kind == K_TVAR) ty[r].prot |= P_COPY;
}
/* A scheme: a signature parsed at a deeper level and generalized. */
static int ty_scheme_slots(Token *toks, int open, int close) {
    TyNames nm = {0}; ty_level++;
    int t = ty_parse_slots(toks, open, close, &nm); ty_level--; ty_held_copy(t, &toks[open]); ty_generalize(t); return t;
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
static int ty_in_prelude;
/* The end of the program's tokens, for a look ahead past the range being checked. */
static int ty_tok_end;
static void ty_err(int line, const char *fmt, ...);
/* Words the runtime reads as forms, not bindings. */
static int ty_reserved(uint32_t sym) {
    return sym == S_LET || sym == S_EFFECT || sym == S_TAG || sym == S_CASE || sym == S_NTH
        || sym == S_ON || sym == S_SHOW || sym == S_AT || sym == S_INTO || sym == S_EDIT;
}
/* Forward declarations waiting for their body, per body depth: code that runs in that scope before the
   body is bound may call the word. */
static int *ty_pending, ty_pending_cap;
static void ty_pending_add(int depth, int d) {
    if (depth >= ty_pending_cap) { int old = ty_pending_cap; ty_pending_cap = 2*depth + 16; ty_pending = realloc(ty_pending, (size_t)ty_pending_cap * sizeof(int));
        if (!ty_pending) die("type checker: out of memory for %d scopes", ty_pending_cap); memset(ty_pending + old, 0, (size_t)(ty_pending_cap - old) * sizeof(int)); }
    ty_pending[depth] += d;
}
/* The outermost scope the code being checked runs in: a [...] or {...} literal's code runs where it is
   written, so it runs in the scopes around it too. */
static int ty_runs_floor;
/* Code about to run a user word, or a body: refused in a scope whose declared words have no body yet. */
static void ty_runs(const char *who, int line) {
    for (int d = ty_runs_floor; d <= ty_body_depth; d++) if (d < ty_pending_cap && ty_pending[d] > 0)
        for (int k = tyb_n - 1; k >= 0; k--) if (tyb[k].declared && tyb[k].depth == d) {
            ty_err(line, "'%s' runs code here, but '%s' is declared on line %d and not defined yet, so that code may call it. Define '%s' first.", who, sym_name(tyb[k].sym), tyb[k].line, sym_name(tyb[k].sym)); return; }
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
    int n0 = ty_n, f = ty_find(ty_instantiate(scheme)), body = user;
    for (int x = ty_find(ty[f].a), hops = -ty_n; !body && ty[x].kind == K_SCONS; x = ty_rest(x, &hops)) body = ty[ty_find(ty[x].a)].kind == K_FN;
    if (body) ty_runs(who, line);
    /* The instance is the nodes from n0 up. A failed unify binds some of them, so a copy of them lets the message show the word's type unbound, with the names ty_why used. */
    static Ty *snap; static int snap_cap; int n1 = ty_n - n0;
    if (n1 > snap_cap) { snap_cap = 2 * n1; snap = realloc(snap, (size_t)snap_cap * sizeof(Ty)); if (!snap) die("type checker: out of memory for %d types", snap_cap); }
    memcpy(snap, ty + n0, (size_t)n1 * sizeof(Ty)); ty_print_count = 0;
    if (ty_unify(ty[f].a, ty_cur)) {
        Ty *failed = malloc((size_t)(n1 + 1) * sizeof(Ty)); if (!failed) die("type checker: out of memory for %d types", n1);
        memcpy(failed, ty + n0, (size_t)n1 * sizeof(Ty)); memcpy(ty + n0, snap, (size_t)n1 * sizeof(Ty));
        char want[512], before[512]; int g = f, n = 0;
        for (int x = ty_find(ty[g].a); ty[x].kind == K_SCONS && n < ty_n; x = ty_find(ty[x].b)) n++;
        ty_show_top(want, sizeof want, ty[g].a, n);
        int tail = ty[ty_stack_tail(ty[f].a)].kind;
        memcpy(ty + n0, failed, (size_t)n1 * sizeof(Ty)); free(failed); ty_show_top(before, sizeof before, ty_cur, n);
        /* A value bound with let has one type, so once a use fixes a bound body's stack depth, it runs only there. */
        char hint[512] = "";
        for (int x = ty_find(ty_cur), k = ty_depth_why ? n : 0; !hint[0] && k-- > 0 && ty[x].kind == K_SCONS; x = ty_find(ty[x].b)) {
            int v = ty_find(ty[x].a);
            if (ty[v].kind != K_FN || ty[ty_stack_tail(ty[v].a)].kind == K_SVAR) continue;
            for (int b = tyb_n - 1; b >= 0; b--) if (!tyb[b].word && ty_find(tyb[b].ty) == v) {
                snprintf(hint, sizeof hint, "\n    '%s' is a body bound with let, and a body bound with let runs at one stack depth. Bound with its body written in place, `(...) '%s let`, it is a word, which runs at any depth.", sym_name(tyb[b].sym), sym_name(tyb[b].sym)); break; }
        }
        /* A word whose input ends in an empty stack takes exactly its inputs, so a longer stack fails it too. */
        int m = 0, exact = 0;
        for (int x = ty_find(ty_cur); ty[x].kind == K_SCONS && m < ty_n; x = ty_find(ty[x].b)) m++;
        if (tail == K_SNIL && m > n && !strncmp(ty_why, "the stack is shorter", 20)) { exact = 1;
            snprintf(ty_why, sizeof ty_why, "'%s' takes exactly %d value%s, but the stack holds %s%d", who, n, n == 1 ? "" : "s", ty[ty_stack_tail(ty_cur)].kind == K_SNIL ? "" : "at least ", m); }
        int lit = ty_stack_tail(ty_cur);
        if (ty[lit].kind == K_SNIL && ty[lit].sym && !strncmp(ty_why, "the stack is shorter", 20)) snprintf(ty_why, sizeof ty_why, "the code in a [...] or {...} literal starts from an empty stack, so it cannot take values from below the literal");
        if (n) ty_err(line, "'%s' takes %s\n    but the stack has %s\n    %s.%s", who, want, before, ty_why, hint);
        else if (exact) ty_err(line, "%s.%s", ty_why, hint);
        else ty_err(line, "'%s' takes nothing\n    %s.%s", who, ty_why, hint);
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
/* The line where the body being checked first pushed a body as a value, or 0: from there its frame
   may be kept by a closure, so a let there may not bind a value that holds a body (ty_no_body). An
   in-place body (if, while, dip, pthen, case clauses) is part of the body around it; any other body
   starts its own count (ty_taint_new). The top level's frame is never freed. */
static int ty_taint, ty_taint_new;
/* in: the stack a signature says the body takes, or 0 for any. lit: a [...] or {...} literal's code, which
   runs where it is written. */
static int ty_body(Token *toks, int open, int close, int in, int lit) {
    int own = ty_taint_new, taint = ty_taint; ty_taint_new = 0; if (own) ty_taint = 0;
    /* a literal's code starts on an empty stack; sym marks it for messages */
    if (lit) { in = ty_new(K_SNIL, 0, 0, 0); ty[in].sym = 1; }
    int saved = ty_cur, mark = tyb_n, floor = ty_runs_floor; ty_cur = in ? in : ty_new(K_SVAR, 0, 0, 0); in = ty_cur;
    c_stack_check("while checking nested bodies");
    ty_body_depth++; if (!lit) ty_runs_floor = ty_body_depth;
    ty_range(toks, open + 1, close); ty_body_depth--; ty_runs_floor = floor;
    ty_undefined(mark);
    int fn = ty_new(K_FN, in, ty_cur, 0); ty_cur = saved; tyb_n = mark;
    if (own) ty_taint = taint;
    return fn;
}
/* `(body) [sig] effect 'name let`: a word. Inside its body the word has one type (or its declared
   one); after, the type generalizes. */
static void ty_define(Token *toks, int open, int close, int sig_open, int sig_close, uint32_t name, int line) {
    ty_redefined(name, line, 1);
    int fwd = tyb_find(name);
    if (fwd >= 0 && !tyb[fwd].declared) fwd = -1;
    if (fwd >= 0 && sig_open) { ty_err(line, "'%s' is declared on line %d with its signature, so its definition takes that one. Drop this second signature.", sym_name(name), tyb[fwd].line); sig_open = 0; }
    int scheme = sig_open ? ty_scheme_slots(toks, sig_open, sig_close) : fwd >= 0 ? tyb[fwd].ty : 0, mark = tyb_n;
    if (fwd >= 0) { tyb[fwd].declared = 0; ty_pending_add(tyb[fwd].depth, -1); }
    ty_level++;
    /* for word 2, ty holds the body's level, where its own calls are made */
    tyb_push(name, scheme ? scheme : ty_level, scheme ? 1 : 2, line);
    int rmark = ty_rigid_n, want = scheme ? ty_rigid(scheme, &rmark) : 0;
    int bt = ty_body(toks, open, close, want ? ty[want].a : 0, 0);
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
        int body = ty_find(ty_body(toks, j, j + toks[j].span, 0, 0)); j += toks[j].span + 1;
        uint32_t tg = toks[key].as.sym; int p = tg == S_WILD ? s : res ? (tg == S_OK ? a : b) : ty_tag_payload(tg);
        ty_print_count = 0;
        if (ty_unify(ty[body].a, ty_new(K_SCONS, p, rest, 0))) { char ps[256]; ty_show(ps, sizeof ps, p, 0);
            if (tg == S_WILD) ty_err(toks[key].line, "the '_ clause gets the tagged value, %s, but its body cannot take it: %s.", ps, ty_why);
            else ty_err(toks[key].line, "the clause for '%s gets its payload, %s, but its body cannot take it: %s.", sym_name(tg), ps, ty_why); }
        else if (ty_unify(ty[body].b, out)) ty_err(toks[key].line, "the clause for '%s does not leave what the other clauses leave: %s.", sym_name(tg), ty_why);
    }
}
/* Values collected from a stack type, for literals. Nothing between filling and
   reading it collects again. */
static int *ty_items, ty_items_cap;
static void ty_item(int k, int t) {
    if (k >= ty_items_cap) { ty_items_cap = ty_items_cap ? 2*ty_items_cap : 1024; ty_items = realloc(ty_items, (size_t)ty_items_cap * sizeof(int)); if (!ty_items) die("type checker: out of memory for %d values", ty_items_cap); }
    ty_items[k] = t;
}
/* A literal holds what its code leaves above the empty stack it starts on. A stack that ends in a
   variable instead comes from a call whose effect is not known yet, so the count of values is not known.
   The caller skips this after an error in the literal's code, which also leaves a variable. */
static int ty_lit_open(int out, int line) {
    int s = ty_find(out);
    for (int hops = -ty_n; ty[s].kind == K_SCONS; s = ty_rest(s, &hops)) {}
    if (ty[s].kind == K_SNIL) return 0;
    ty_err(line, "the checker cannot count what this literal holds: its code ends in a call whose effect is not known yet (a recursive call or a body passed in), or in fail, which leaves any stack. Run the call before the literal and bind its result, as in `x f 'r let [r]`; write fail outside the literal.");
    return 1;
}
static void ty_range(Token *toks, int i, int end) {
    for (; i < end; i++) {
        Token *t = &toks[i]; int line = t->line; current_loc = LOC_PACK(t->fid, t->line, t->col);
        switch (t->tag) {
        case TOK_INT: ty_push(ty_new(K_INT, 0, 0, 0)); break;
        case TOK_FLOAT: ty_push(ty_new(K_FLOAT, 0, 0, 0)); break;
        case TOK_STRING: { int t = ty_new(K_LIST, ty_new(K_INT, 0, 0, 0), 0, 0); ty[t].sym = 1; ty_push(t); break; }
        case TOK_SYM: { int y = ty_new(K_SYM, 0, 0, 0); ty[y].sym = t->as.sym + 1; ty_push(y); break; }
        case TOK_LPAREN: {
            int close = i + t->span, nm = close + 1, sig_open = 0, sig_close = 0;
            /* runs in place, as eval_run decides: `(b) dip`, `(b) pthen`, `(t) (e) if`, `(p) (b) while` */
            int pair = close + 1 < end && toks[close+1].tag == TOK_LPAREN ? close + 1 + toks[close+1].span + 1 : -1;
            int inplace = (close + 1 < end && (ty_word_is(&toks[close+1], "dip") || ty_word_is(&toks[close+1], "pthen")))
                || (pair > 0 && pair < end && (ty_word_is(&toks[pair], "if") || ty_word_is(&toks[pair], "while")))
                || (close + 1 < end && i > 0 && toks[i-1].tag == TOK_RPAREN && (ty_word_is(&toks[close+1], "if") || ty_word_is(&toks[close+1], "while")));
            if (!inplace) { if (!ty_taint) ty_taint = line; ty_taint_new = 1; }
            if (nm < end && toks[nm].tag == TOK_LBRACKET && nm + toks[nm].span + 1 < end && ty_word_is(&toks[nm + toks[nm].span + 1], "effect")) {
                sig_open = nm; sig_close = nm + toks[nm].span; nm = sig_close + 2; }
            if (nm + 1 < end && toks[nm].tag == TOK_SYM && toks[nm+1].tag == TOK_WORD && toks[nm+1].as.sym == S_LET) {
                ty_define(toks, i, close, sig_open, sig_close, toks[nm].as.sym, line); i = nm + 1; break; }
            if (sig_open) { ty_level++; int rmark, want = ty_rigid(ty_scheme_slots(toks, sig_open, sig_close), &rmark), bt = ty_body(toks, i, close, ty[want].a, 0);
                if (ty_unify(want, bt)) ty_err(line, "this body does not have its declared type: %s.", ty_why);
                ty_unrigid(rmark); ty_level--; ty_occurs(-1, bt, ty_level); ty_push(bt); i = sig_close + 1; break; }
            ty_push(ty_body(toks, i, close, 0, 0)); i = close; break;
        }
        case TOK_LBRACKET: {
            int close = i + t->span;
            if (close + 1 < end && ty_word_is(&toks[close+1], "effect")) {
                /* `'name [sig] effect`: the word is declared before its body is written */
                if (i == 0 || toks[i-1].tag != TOK_SYM) { ty_err(line, "a signature [...] effect needs a body before it or a 'name before it."); i = close + 1; break; }
                ty_pop(); ty_redefined(toks[i-1].as.sym, line, 0);
                tyb_push(toks[i-1].as.sym, ty_scheme_slots(toks, i, close), 1, line); tyb[tyb_n-1].declared = 1;
                ty_pending_add(ty_body_depth, 1);
                i = close + 1; break;
            }
            /* a list literal: its elements have one type. ty_body may move the pool, so ty is read after it returns. */
            int e0 = ty_errors, fn = ty_body(toks, i, close, 0, 1), out = ty[fn].b;
            if (ty_errors == e0 && ty_lit_open(out, line)) { ty_push(ty_new(K_VAR, 0, 0, 0)); i = close; break; }
            int el = ty_new(K_VAR, 0, 0, 0); ty[el].prot = P_COPY;
            for (int s = ty_find(out), k = 0, hops = -ty_n; ty[s].kind == K_SCONS; s = ty_rest(s, &hops), k++)
                if (ty_need(ty[s].a, P_COPY)) { ty_err(line, "a list literal holds only values that can be copied: %s.", ty_why); break; }
                else if (ty_unify(el, ty[s].a)) { ty_err(line, "a list holds values of one type, but element %d from the end is not like the others: %s.", k + 1, ty_why); break; }
            ty_push(ty_new(K_LIST, el, 0, 0)); i = close; break;
        }
        case TOK_LBRACE: {
            int close = i + t->span;
            if (close + 1 < end && toks[close+1].tag == TOK_WORD && toks[close+1].as.sym == S_CASE) { ty_case(toks, i, close, line); i = close + 1; break; }
            /* a {...} literal: a record, each value written after its 'key */
            int e0 = ty_errors, fn = ty_body(toks, i, close, 0, 1), out = ty[fn].b, n = 0;
            if (ty_errors == e0 && ty_lit_open(out, line)) { ty_push(ty_new(K_VAR, 0, 0, 0)); i = close; break; }
            for (int s = ty_find(out), hops = -ty_n; ty[s].kind == K_SCONS; s = ty_rest(s, &hops)) ty_item(n++, ty[s].a);
            for (int k = 0; k < n; k++) if (ty_need(ty_items[k], P_COPY)) { ty_err(line, "a record holds only values that can be copied: %s.", ty_why); break; }
            int rec = n % 2 == 0;
            for (int k = 1; rec && k < n; k += 2) if (ty[ty_find(ty_items[k])].kind != K_SYM) rec = 0;
            /* odd, with a symbol in every key place and a value that is not one: a record missing its last value */
            if (n % 2) { int keys = 1, vals = 0;
                for (int k = 0; k < n; k += 2) keys &= ty[ty_find(ty_items[k])].kind == K_SYM;
                for (int k = 1; k < n; k += 2) vals |= ty[ty_find(ty_items[k])].kind != K_SYM;
                int l = ty_sym_name(ty_items[0]);
                if (keys && vals && l >= 0) { ty_err(line, "record literal: key '%s has no value. Give it one, as in {'%s 0}.", sym_name(l), sym_name(l)); rec = -1; } }
            if (rec == 0) ty_err(line, "a {...} literal is a record, so each value follows its 'key, as in {'x 1 'y 2}. For code that pushes values, write a body: (1 2).");
            if (rec != 1) { ty_push(ty_new(K_VAR, 0, 0, 0)); i = close; break; }
            { int row = ty_new(K_RNIL, 0, 0, 0);
                for (int k = n - 1; k >= 1; k -= 2) {
                    int l = ty_sym_name(ty_items[k]), twice = 0;
                    if (l < 0) { ty_err(line, "this {...} literal pairs each value with a symbol, so it is a record, but key %d is a symbol this literal computes. Write each key in the literal, as in {'name 1}.", (n - k) / 2 + 1); continue; }
                    for (int m = 1; m < k; m += 2) if (ty_sym_name(ty_items[m]) == l) twice = 1;
                    if (twice) { ty_err(line, "this record literal has '%s twice.", sym_name(l)); continue; }
                    row = ty_new(K_REXT, ty_sym(K_LSYM, (uint32_t)l), ty_items[k-1], row);
                }
                ty_push(ty_new(K_REC, row, 0, 0)); i = close; break; }
        }
        case TOK_WORD: {
            uint32_t w = t->as.sym; ty_at_word = sym_name(w);
            if (w == S_LET) {
                if (i == 0 || toks[i-1].tag != TOK_SYM) { ty_err(line, "let needs its name written before it, as in `42 'x let`."); break; }
                ty_pop(); int v = ty_pop();
                if (ty_need(v, P_COPY)) ty_err(line, "'%s' cannot be let-bound: %s. Keep it on the stack.", sym_name(toks[i-1].as.sym), ty_why);
                else if (ty_taint && ty_body_depth > 0 && ty_no_body(v))
                    ty_err(line, "'%s' is bound after the body on line %d, and it holds a body: %s. Bind it before that body is made, or in a word of its own.", sym_name(toks[i-1].as.sym), ty_taint, ty_why);
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
                int named = i > 0 && toks[i-1].tag == TOK_SYM; ty_pop(); int ix = ty_pop();
                if (!named) { ty_err(line, "nth needs the list's name written right before it, as in `1 'xs nth`."); ty_push(ty_new(K_VAR, 0, 0, 0)); break; }
                if (ty_unify(ty_new(K_INT, 0, 0, 0), ix)) ty_err(line, "nth takes an int index: %s.", ty_why);
                uint32_t nm = toks[i-1].as.sym;
                int el = ty_new(K_VAR, 0, 0, 0), b = tyb_find(nm);
                if (b < 0 || tyb[b].word) { ty_err(line, "nth reads a list bound to '%s, but '%s is not a bound list here.", sym_name(nm), sym_name(nm)); }
                else if (ty_unify(ty_new(K_LIST, el, 0, 0), tyb[b].ty)) ty_err(line, "nth reads a list, but '%s is not one: %s.", sym_name(nm), ty_why);
                { int r = ty_new(K_SVAR, 0, 0, 0); ty_push(ty_new(K_RES, el, ty_new(K_FN, r, r, 0), 0)); } break;
            }
            if (w == S_CASE) { ty_err(line, "case needs its clauses written right before it, as in `x {'ok (…) 'no (…)} case`."); ty_pop(); ty_pop(); ty_push(ty_new(K_VAR, 0, 0, 0)); break; }
            if (w == S_AT || w == S_INTO || w == S_EDIT) {
                /* the key is the symbol written right before the word */
                int kt = i - 1;
                if (kt < 0 || toks[kt].tag != TOK_SYM) {
                    ty_err(line, "'%s' needs its key written right before it, as in `%s'name %s`. For keys that are data, use a dict: `d key of`.", sym_name(w), w == S_EDIT ? "(1 plus) " : "", sym_name(w));
                    ty_cur = ty_new(K_SVAR, 0, 0, 0); break;
                }
                if (w == S_INTO) {
                    /* into on a closed record without the key: the fix is the literal that builds the record */
                    int st = ty_find(ty_cur), rec = 0, row = 0;
                    for (int k = 0; k < 2 && ty[st].kind == K_SCONS; k++) st = ty_find(ty[st].b);
                    if (ty[st].kind == K_SCONS && ty[rec = ty_find(ty[st].a)].kind == K_REC)
                        for (row = ty_find(ty[rec].a); ty[row].kind == K_REXT && ty[ty[row].a].sym != toks[kt].as.sym; row = ty_find(ty[row].c)) {}
                    if (row && ty[row].kind == K_RNIL) {
                        char rs[256], ex[64] = "…"; Token *vt = kt > 0 ? &toks[kt-1] : 0; ty_print_count = 0; ty_show(rs, sizeof rs, rec, 0);
                        if (vt && vt->tag == TOK_INT) snprintf(ex, sizeof ex, "%lld", (long long)vt->as.i);
                        else if (vt && vt->tag == TOK_FLOAT) snprintf(ex, sizeof ex, "%g", vt->as.f);
                        else if (vt && vt->tag == TOK_WORD && tyb_find(vt->as.sym) >= 0 && !tyb[tyb_find(vt->as.sym)].word) snprintf(ex, sizeof ex, "%s", sym_name(vt->as.sym));
                        else if (vt && vt->tag == TOK_SYM) snprintf(ex, sizeof ex, "'%s", sym_name(vt->as.sym));
                        const char *k = sym_name(toks[kt].as.sym);
                        ty_err(line, "into replaces the value of a key the record has, but this record, %s, has no '%s. A record has the keys its literal names: build it with '%s, as in {'%s %s}.", rs, k, k, k, ex);
                        ty_pop(); ty_pop(); break;
                    }
                }
                int key = ty_sym(K_LSYM, toks[kt].as.sym), s0 = ty_new(K_SVAR, 0, 0, 0), r = ty_new(K_RVAR, 0, 0, 0), v = ty_new(K_VAR, 0, 0, 0), in, out;
                ty[v].prot = P_COPY;
                int sym = ty_new(K_SYM, 0, 0, 0);
                if (w == S_AT) {
                    in = ty_new(K_SCONS, sym, ty_new(K_SCONS, ty_new(K_REC, ty_new(K_REXT, key, v, r), 0, 0), s0, 0), 0);
                    out = ty_new(K_SCONS, v, s0, 0);
                } else if (w == S_INTO) {
                    int old = ty_new(K_REC, ty_new(K_REXT, key, ty_new(K_VAR, 0, 0, 0), r), 0, 0);
                    in = ty_new(K_SCONS, sym, ty_new(K_SCONS, v, ty_new(K_SCONS, old, s0, 0), 0), 0);
                    out = ty_new(K_SCONS, ty_new(K_REC, ty_new(K_REXT, key, v, r), 0, 0), s0, 0);
                } else {
                    int u = ty_new(K_VAR, 0, 0, 0), below = ty_new(K_SVAR, 0, 0, 0); ty[below].sealed = 1; ty[u].prot = P_COPY;
                    int body = ty_new(K_FN, ty_new(K_SCONS, v, below, 0), ty_new(K_SCONS, u, below, 0), 0);
                    in = ty_new(K_SCONS, sym, ty_new(K_SCONS, body, ty_new(K_SCONS, ty_new(K_REC, ty_new(K_REXT, key, v, r), 0, 0), s0, 0), 0), 0);
                    out = ty_new(K_SCONS, ty_new(K_REC, ty_new(K_REXT, key, u, r), 0, 0), s0, 0);
                }
                ty_apply(ty_new(K_FN, in, out, 0), sym_name(w), line, 0); break;
            }
            if (w == S_ON) {
                int ev = ty_pop(), h = ty_pop(), l = ty_sym_name(ev);
                if (l < 0) { ty_err(line, "on needs its event written right before it, as in `(…) 'tick on`."); break; }
                const char *en = sym_name(l);
                if (strcmp(en, "tick") && strcmp(en, "keydown") && strcmp(en, "keyup") && strcmp(en, "mousedown") && strcmp(en, "mouseup") && strcmp(en, "mousemove") && strcmp(en, "resize")) {
                    ty_err(line, "on has no event '%s. The events are 'tick 'keydown 'keyup 'mousedown 'mouseup 'mousemove 'resize.", en); break; }
                if (ty_body_depth) { ty_err(line, "on registers a handler for the whole program, so it runs at the top level, not inside a body."); break; }
                if (ty_shown) { ty_err(line, "this handler is registered after show starts the event loop, so it never runs. Register it before show."); break; }
                if (ty_on_n == 16) die("type checker: more than 16 'on' handlers");
                ty_on[ty_on_n] = h; ty_on_line[ty_on_n] = line; ty_on_mouse[ty_on_n++] = !strcmp(en, "resize") ? 2 : strncmp(en, "mouse", 5) == 0; break;
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
                    if (ty_unify(ty_on[k], ty_new(K_FN, in, below, 0))) ty_err(ty_on_line[k], "this handler must take the event's %s and leave the stack below show as it was: %s.", ty_on_mouse[k] ? (ty_on_mouse[k] == 2 ? "w and h" : "x and y") : "int", ty_why);
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
static void ty_table_copy(int fn, Token *tok) {
    for (int x = ty_find(ty[fn].a), hx = -ty_n; ty[x].kind == K_SCONS; x = ty_rest(x, &hx)) {
        int v = ty_find(ty[x].a), in = 0, out = 0;
        if (ty[v].kind != K_VAR) continue;
        for (int y = ty_find(ty[fn].a), hy = -ty_n; ty[y].kind == K_SCONS; y = ty_rest(y, &hy)) in += ty_find(ty[y].a) == v;
        for (int y = ty_find(ty[fn].b), hy = -ty_n; ty[y].kind == K_SCONS; y = ty_rest(y, &hy)) out += ty_find(ty[y].a) == v;
        if (in != out) ty[v].prot |= P_COPY;
    }
    ty_held_copy(fn, tok);
}
/* What a list, dict, box or record holds is copyable, in the table and in signatures alike. */
/* The walk collects what the containers hold, then marks it: marking walks terms too (ty_need), with the
   same work stack and stamps. */
static int *ty_held, ty_held_cap;
static void ty_held_copy(int t, Token *tok) {
    int n = 0, h = 0, stamp = ++ty_stamp; ty_work_push(&n, t);
    while (n) {
        int x = ty_find(ty_work[--n]);
        if (ty_mark[x] == stamp) continue;
        ty_mark[x] = stamp;
        int held = ty[x].kind == K_LIST || ty[x].kind == K_DICT || ty[x].kind == K_BOX ? ty_find(ty[x].a)
                 : ty[x].kind == K_REXT ? ty_find(ty[x].b) : 0;
        if (held) {
            if (h == ty_held_cap) { ty_held_cap = ty_held_cap ? 2*ty_held_cap : 256; ty_held = realloc(ty_held, (size_t)ty_held_cap * sizeof(int)); if (!ty_held) die("type checker: out of memory for %d held types", ty_held_cap); }
            ty_held[h++] = held; }
        if (!ty_isvar(ty[x].kind)) { if (ty[x].a) ty_work_push(&n, ty[x].a); if (ty[x].b) ty_work_push(&n, ty[x].b); if (ty[x].c) ty_work_push(&n, ty[x].c); }
    }
    for (int k = 0; k < h; k++) ty_mark_copy(ty_held[k], tok);
}
/* The builtin table: 'name ( ins -> outs ) pairs. */
static void ty_read_table(Token *toks, int n) {
    for (int i = 0; i + 1 < n; ) {
        if (toks[i].tag != TOK_SYM || toks[i+1].tag != TOK_LPAREN) die("type table: expected 'name ( ... -> ... ) at line %d", toks[i].line);
        TyNames nm = {0}; int close = i + 1 + toks[i+1].span;
        ty_level++; int t = ty_parse_fn(toks, i + 1, close, &nm, 0); ty_level--;
        ty_table_copy(t, &toks[i]); ty_generalize(t); ty_builtin[toks[i].as.sym] = t; i = close + 1;
    }
}
/* The shell keeps values on the stack between lines, so its program may end on a box. */
static int ty_shell;
static int infer_program(Token *table, int table_n, Token *toks, int count, int user_start) {
    /* Each call starts clean: the shell checks every line again with the lines before it. */
    ty_n = 1; ty_level = 0; ty_links_n = 0; ty_memo_n = 0; ty_slot_rest = 0; ty_rigid_n = 0; ty_rec_n = 0;
    tyb_n = 0; tyb_prelude = 0; ty_body_depth = 0; ty_runs_floor = 0; ty_in_prelude = 0;
    ty_errors = 0; ty_on_n = 0; ty_shown = 0; ty_rigid_rest = 0; ty_depth_why = 0; ty_print_count = 0; ty_taint = 0; ty_taint_new = 0;
    memset(ty_builtin, 0, sizeof ty_builtin); memset(ty_tagpay, 0, sizeof ty_tagpay);
    if (ty_pending) memset(ty_pending, 0, (size_t)ty_pending_cap * sizeof *ty_pending);
    ty_read_table(table, table_n);
    ty_cur = ty_new(K_SNIL, 0, 0, 0);
    ty_tok_end = count;
    ty_in_prelude = 1; ty_range(toks, 0, user_start); ty_in_prelude = 0; tyb_prelude = tyb_n;
    ty_cur = ty_new(K_SNIL, 0, 0, 0);
    ty_range(toks, user_start, count);
    ty_undefined(tyb_prelude);
    /* after an error the stack's types may be made up, so the end is checked only when nothing failed */
    for (int x = ty_find(ty_cur), hops = -ty_n; !ty_errors && !ty_shell && ty[x].kind == K_SCONS; x = ty_rest(x, &hops))
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
    Value name##_top = stack[sp-1]; int name##_s = val_slots(name##_top); Value *name##_buf = aux_take(name##_s)
/* The bodies a running primitive took, by aux index of their header: eval_run frees them when the
   primitive returns. */
static int *staged, staged_n, staged_cap;
static inline void stage_body(int at) {
    if (staged_n == staged_cap) { staged_cap = staged_cap ? 2*staged_cap : 256; staged = realloc(staged, (size_t)staged_cap*sizeof(int)); if (!staged) die("out of memory: %d staged bodies", staged_cap); }
    staged[staged_n++] = at;
}
#define POP_BODY(name) POP_VAL(name); stage_body(asp-1)
static void deep_copy_values(Value *dst, const Value *src, int slots);
static void prim_dup(Frame *e) { (void)e; Value top=stack[sp-1]; if(top.tag<=VAL_XT){spush(top);return;} int s=val_slots(top); stack_room(s,"dup"); deep_copy_values(&stack[sp],&stack[sp-s],s); sp+=s; }
static void prim_drop(Frame *e) { (void)e; Value top=stack[sp-1]; if(top.tag<=VAL_XT){sp--;return;} int s=val_slots(top); deep_free_values(&stack[sp-s],s); sp-=s; }
static void slot_reverse(Value *a,int n){for(int i=0,j=n-1;i<j;i++,j--){Value t=a[i];a[i]=a[j];a[j]=t;}}
/* Exchange the adjacent runs [base, base+n1) and [base+n1, base+n1+n2): the shorter run waits on the
   aux stack while one memmove shifts the longer. With no room on the aux stack, three reversals do the
   same in place, more slowly. */
static void swap_blocks(int base,int n1,int n2){
    int a0=asp;
    if(asp+(n1<n2?n1:n2)>STACK_MAX){ slot_reverse(&stack[base],n1+n2); slot_reverse(&stack[base],n2); slot_reverse(&stack[base+n2],n1); return; }
    if(n1<=n2){ Value *t=aux_reserve(n1); VCPY(t,&stack[base],n1); memmove(&stack[base],&stack[base+n1],(size_t)n2*sizeof(Value)); VCPY(&stack[base+n2],t,n1); }
    else { Value *t=aux_reserve(n2); VCPY(t,&stack[base+n1],n2); memmove(&stack[base+n2],&stack[base],(size_t)n1*sizeof(Value)); VCPY(&stack[base],t,n2); }
    asp=a0;
}
/* Where the value ending at slot `end` starts. */
static int val_start(int end) {
    return end - val_slots(stack[end-1]);
}
static void prim_swap(Frame *e) {
    (void)e;
    if(sp>=2&&stack[sp-1].tag<=VAL_XT&&stack[sp-2].tag<=VAL_XT){Value t=stack[sp-1];stack[sp-1]=stack[sp-2];stack[sp-2]=t;return;}
    int b=val_start(sp),a=val_start(b); swap_blocks(a,b-a,sp-b);
}
static void prim_over(Frame *e) {
    (void)e;
    if(sp>=2&&sp<STACK_MAX&&stack[sp-1].tag<=VAL_XT&&stack[sp-2].tag<=VAL_XT){stack[sp]=stack[sp-2];sp++;return;}
    int b=val_start(sp),a=val_start(b);
    stack_room(b-a,"over");
    deep_copy_values(&stack[sp],&stack[a],b-a); sp+=b-a;
}
/* a b c -- b c a */
static void prim_rot(Frame *e) {
    (void)e;
    if(sp>=3&&stack[sp-1].tag<=VAL_XT&&stack[sp-2].tag<=VAL_XT&&stack[sp-3].tag<=VAL_XT){Value t=stack[sp-3];stack[sp-3]=stack[sp-2];stack[sp-2]=stack[sp-1];stack[sp-1]=t;return;}
    int c=val_start(sp),b=val_start(c),a=val_start(b);
    swap_blocks(a,b-a,sp-b);
}
static void prim_dip(Frame *env) {
    POP_BODY(body);
    POP_VAL(saved); eval_body(body_buf,body_s,env);
    SPUSH(saved_buf,saved_s);
}
/* `(body) dip` written in place. Keep it noinline: inlined, it makes clang lay out eval_run's hot
   loop worse, and zoom.slap slows. */
__attribute__((noinline)) static void dip_run(Value *body, int slots, Frame *ee) { POP_VAL(saved); eval_in(body,slots,ee); SPUSH(saved_buf,saved_s); }
static void prim_apply(Frame *env) { POP_BODY(body); eval_body(body_buf,body_s,env); }
/* Integer plus/sub/mul wrap at 64 bits: computed in uint64_t, where overflow is defined. */
#define ARITH2(nm,iop,fop) static void prim_##nm(Frame *e){(void)e;Value b=spop(),a=spop(); \
    if(a.tag==VAL_INT&&b.tag==VAL_INT) spush(val_int((int64_t)((uint64_t)a.as.i iop (uint64_t)b.as.i))); \
    else spush(val_float(a.as.f fop b.as.f));}
ARITH2(plus,+,+) ARITH2(sub,-,-) ARITH2(mul,*,*)
static void int_div_check(const char *who,int64_t a,int64_t b){
    if(b==0) die("%s: division by zero",who);
    if(a==INT64_MIN&&b==-1) die("%s: %lld / -1 overflows a 64-bit int",who,(long long)a);
}
static void prim_div(Frame *e) { (void)e; Value b=spop(),a=spop();
    if(a.tag==VAL_INT&&b.tag==VAL_INT){int_div_check("div",a.as.i,b.as.i);spush(val_int(a.as.i/b.as.i));}
    else spush(val_float(a.as.f/b.as.f)); }
static void prim_mod(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();int_div_check("mod",a,b);spush(val_int(a%b));}
#define INTOP2(nm,expr) static void prim_##nm(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();spush(val_int(expr));}
#define SHIFT_OK(nm) (b<0||b>63?(die(#nm ": shift count %lld is outside 0-63",(long long)b),0):1)
INTOP2(band,a&b) INTOP2(bor,a|b) INTOP2(bxor,a^b) INTOP2(shl,SHIFT_OK(shl)?(int64_t)((uint64_t)a<<b):0) INTOP2(shr,SHIFT_OK(shr)?(int64_t)((uint64_t)a>>b):0)
INTOP2(and,(a&&b)?1:0) INTOP2(or,(a||b)?1:0)
#define CMP2(nm,expr) static void prim_##nm(Frame *e){(void)e;int b=val_start(sp),a=val_start(b),r=(expr);deep_free_values(&stack[a],sp-a);sp=a;spush(val_int(r?1:0));}
CMP2(eq, val_equal(&stack[a],b-a,&stack[b],sp-b))
CMP2(lt, val_less(&stack[a],b-a,&stack[b],sp-b))
static void prim_print(Frame *e){(void)e;Value top=stack[sp-1];int s=val_slots(top);val_print(&stack[sp-s],s,stdout);printf("\n");deep_free_values(&stack[sp-s],s);sp-=s;
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
    POP_BODY(el); POP_BODY(then);
    Value cond=spop();
    if(cond.as.i) eval_body(then_buf,then_s,env); else eval_body(el_buf,el_s,env);
}
/* The clause list is a record literal, built when the program is read; its bodies are code at the case,
   so they run in the frame that runs case. A clause runs on its tag's payload, already in place under the
   tagged value's header; '_ runs on the tagged value. */
static void case_run(Value *clauses, int n, Frame *env) {
    int len=(int)clauses[n-1].as.compound.len, found=0; uint32_t tag_sym=stack[sp-1].as.compound.len;
    ElemRef br = record_field(clauses,n,len,tag_sym,&found);
    if (found) sp--; else br = record_field(clauses,n,len,S_WILD,&found);
    eval_in(&clauses[br.base],br.slots,env);
}
static void prim_case(Frame *env) { POP_VAL(clauses); stage_body(asp-1); case_run(clauses_buf,clauses_s,env); }
static void prim_tag(Frame *e) {
    (void)e;
    uint32_t tag_sym=pop_sym();
    Value payload_top=stack[sp-1];
    int payload_s=val_slots(payload_top);
    spush(val_compound(VAL_TAGGED,tag_sym,payload_s+1));
}
static void prim_must(Frame *e) {
    (void)e; Value top=speek();
    if(top.as.compound.len==S_OK) { sp--; return; }
    die("must: expected 'ok tagged, got '%s tagged (its payload is on top of the stack below)", sym_name(top.as.compound.len));
}
/* tagged default (body) pthen: on 'ok run body on the payload in env, else leave default under the tagged,
   re-tagged 'no. */
static void pthen_run(Value *body, int n, Frame *env) {
    POP_VAL(def);
    Value top=stack[sp-1];
    if(top.as.compound.len==S_OK){ deep_free_values(def_buf,def_s); sp--; eval_in(body,n,env); return; }
    stack[sp-1].as.compound.len=S_NO;
    int ts=val_slots(top); SPUSH(def_buf,def_s); swap_blocks(sp-def_s-ts,ts,def_s);
}
static void prim_pthen(Frame *env) { POP_BODY(body); Frame *f=body_buf[body_s-1].as.compound.env; pthen_run(body_buf,body_s,f?f:env); }
static void prim_while(Frame *env) {
    POP_BODY(body); POP_BODY(pred);
    for(;;){eval_body(pred_buf,pred_s,env);if(!pop_int())break;eval_body(body_buf,body_s,env);}
}
static void prim_itof(Frame *e){(void)e;spush(val_float((double)pop_int()));}
static void prim_ftoi(Frame *e){(void)e;double f=pop_float();
    if(!(f>=-9223372036854775808.0&&f<9223372036854775808.0)) die("ftoi: %g does not fit in an int",f);
    spush(val_int((int64_t)f));}
static void prim_float_bits(Frame *e){(void)e;double f=pop_float();int64_t b;memcpy(&b,&f,8);spush(val_int(b));}
static void prim_bits_float(Frame *e){(void)e;int64_t b=pop_int();double f;memcpy(&f,&b,8);spush(val_float(f));}
#define FLOAT1(nm,fn) static void prim_##nm(Frame *e){(void)e;spush(val_float(fn(pop_float())));}
FLOAT1(fsqrt,sqrt)
FLOAT1(ffloor,floor) FLOAT1(fround,round) FLOAT1(fexp,exp) FLOAT1(flog,log)
#define FLOAT2(nm,fn) static void prim_##nm(Frame *e){(void)e;double b=pop_float(),a=pop_float();spush(val_float(fn(a,b)));}
FLOAT2(fpow,pow) FLOAT2(fatan2,atan2)
static void dict_data_free(DictData *dd);
static void prim_size(Frame *e) {
    (void)e; Value top=speek();
    int s=val_slots(top); deep_free_values(&stack[sp-s],s); sp-=s; spush(val_int((int)top.as.compound.len));
}
static void prim_push_op(Frame *e) {
    (void)e; int vb=val_start(sp),vs=sp-vb;
    Value h=stack[vb-1];
    memmove(&stack[vb-1],&stack[vb],(size_t)vs*sizeof(Value));
    h.as.compound.len++; h.as.compound.slots+=(uint32_t)vs; h.loc=0; stack[sp-1]=h;
}
#define MUST_PAIR(nm) static void prim_##nm(Frame *e) { prim_##nm##_impl(e,1); } \
    static void prim_##nm##_must(Frame *e) { prim_##nm##_impl(e,0); }
static inline void prim_pop_impl(Frame *e, int tagged) {
    (void)e; Value top=speek();
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    if(len==0) { if(tagged) push_none(); else die("pop: empty list"); return; }
    ElemRef last=compound_elem(&stack[base],s,len,len-1);
    memmove(&stack[base+last.base+1],&stack[base+last.base],(size_t)last.slots*sizeof(Value));
    top.as.compound.len--; top.as.compound.slots-=(uint32_t)last.slots; top.loc=0; stack[base+last.base]=top;
    if(tagged) push_ok();
}
MUST_PAIR(pop)
static inline void prim_get_impl(Frame *e, int tagged) {
    (void)e; int64_t idx=pop_int(); Value top=speek();
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    ElemRef ref=compound_elem(&stack[base],s,len,idx);
    if(ref.base<0) { deep_free_values(&stack[base],s); sp=base; if(tagged) push_none(); else die("get: index %lld out of bounds (len %d)",(long long)idx,len); return; }
    deep_free_values(&stack[base],ref.base); deep_free_values(&stack[base+ref.base+ref.slots],s-ref.base-ref.slots);
    memmove(&stack[base],&stack[base+ref.base],(size_t)ref.slots*sizeof(Value)); sp=base+ref.slots;
    if(tagged) push_ok();
}
MUST_PAIR(get)
static inline void prim_peek_impl(Frame *e, int tagged) {
    (void)e; int64_t idx=pop_int(); Value top=speek();
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
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
    (void)e; POP_VAL(v); int64_t idx=pop_int(); Value top=speek();
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
    (void)e;
    Value t2=stack[sp-1];
    int s2=val_slots(t2),b2=sp-s2;
    Value t1=stack[b2-1];
    memmove(&stack[b2-1],&stack[b2],(size_t)(s2-1)*sizeof(Value)); sp--;
    t2.as.compound.len+=t1.as.compound.len; t2.as.compound.slots=(uint32_t)(val_slots(t1)+s2-1); t2.loc=0;
    stack[sp-1]=t2;
}
static inline void prim_nth_impl(Frame *env, int tagged) {
    uint32_t sym=pop_sym(); int64_t idx=pop_int();
    Lookup lu=frame_lookup(env,sym);
    Value *data=lu.bind->vals; int s=lu.bind->slots;
    Value top=data[s-1];
    int len=(int)top.as.compound.len;
    ElemRef ref=compound_elem(data,s,len,idx);
    if(ref.base<0) { if(tagged) push_none(); else die("nth: index %lld out of bounds (len %d)",(long long)idx,len); return; }
    /* A deep copy: the binding keeps its own boxes and dicts. */
    stack_room(ref.slots,"nth");
    if(ref.slots==1&&data[ref.base].tag<=VAL_SYM) stack[sp++]=data[ref.base];
    else { deep_copy_values(&stack[sp],&data[ref.base],ref.slots); sp+=ref.slots; }
    if(tagged) push_ok();
}
MUST_PAIR(nth)
static void prim_slice_n(int take) {
    int64_t n=pop_int(); Value top=speek();
    const char *label=take?"take-n":"drop-n";
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
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
    (void)e; Value top=speek();
    int s=val_slots(top),len=(int)top.as.compound.len,p=sp-s;
    slot_reverse(&stack[p],s-1);
    for(int i=0;i<len;i++){ int n=val_slots(stack[p]); slot_reverse(&stack[p],n); p+=n; }
}
/* a b zip: [a0 b0] [a1 b1] ... as long as the shorter list. One pass over each;
   reading element i by index walks a list whose elements span several slots. */
static void prim_zip(Frame *e){
    (void)e;
    POP_VAL(b);
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
static void prim_range(Frame *e){(void)e;int64_t end=pop_int(),start=pop_int();
    uint64_t n=end>start?(uint64_t)end-(uint64_t)start:0;
    if(n>=(uint64_t)(STACK_MAX-sp)) die("range: %lld to %lld makes %llu ints, but the stack has room for %d more: a list lives on the %d-slot stack. Make a smaller range.",(long long)start,(long long)end,(unsigned long long)n,STACK_MAX-sp-1,STACK_MAX);
    for(int64_t i=start;i<end;i++) stack[sp++]=val_int(i);
    spush(val_compound(VAL_LIST,(int)n,(int)n+1));}
static void push_string_bytes(const char *buf, int len);
static void prim_each(Frame *env) {
    POP_BODY(fn);
    POP_VAL(list); int len=(int)list_top.as.compound.len,*st=elem_starts(list_buf,list_s,len),rb=sp;
    for(int i=0;i<len;i++){ if(st) SPUSH(&list_buf[st[i]],st[i+1]-st[i]); else spush(list_buf[i]); eval_body(fn_buf,fn_s,env); }
    spush(val_compound(VAL_LIST,len,sp-rb+1));
}
static void prim_fold(Frame *env) {
    POP_BODY(fn); POP_VAL(init);
    POP_VAL(list); int len=(int)list_top.as.compound.len,*st=elem_starts(list_buf,list_s,len);
    SPUSH(init_buf,init_s);
    for(int i=0;i<len;i++){ if(st) SPUSH(&list_buf[st[i]],st[i+1]-st[i]); else spush(list_buf[i]); eval_body(fn_buf,fn_s,env); }
}
/* The predicate takes a copy of each element; the list keeps the element when it leaves a nonzero int. */
static void prim_filter(Frame *env) {
    POP_BODY(fn);
    POP_VAL(list); int len=(int)list_top.as.compound.len,*st=elem_starts(list_buf,list_s,len),rb=sp,kept=0;
    for(int i=0;i<len;i++){
        int b=st?st[i]:i,n=st?st[i+1]-st[i]:1;
        stack_room(n,"filter"); deep_copy_values(&stack[sp],&list_buf[b],n); sp+=n;
        eval_body(fn_buf,fn_s,env);
        Value keep=spop();
        if(keep.as.i){ SPUSH(&list_buf[b],n); kept++; } else deep_free_values(&list_buf[b],n);
    }
    spush(val_compound(VAL_LIST,kept,sp-rb+1));
}
static int val_cmp(const Value *va, const Value *vb) {
    if(va->tag==VAL_INT) return(va->as.i>vb->as.i)-(va->as.i<vb->as.i);
    return(va->as.f>vb->as.f)-(va->as.f<vb->as.f);
}
static int sort_cmp(const void *a,const void *b) { return val_cmp((const Value*)a,(const Value*)b); }
static void prim_sort(Frame *e){
    (void)e; Value top=speek();
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    qsort(&stack[base],len,sizeof(Value),sort_cmp);
}
static inline void prim_indexof_impl(Frame *e, int tagged) {
    (void)e; POP_VAL(val); Value top=speek();
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s,r=-1,*st=elem_starts(&stack[base],s,len);
    for(int i=0;i<len&&r<0;i++){int b=st?st[i]:i,n=st?st[i+1]-st[i]:1;if(val_equal(&stack[base+b],n,val_buf,val_s))r=i;}
    deep_free_values(&stack[base],s); deep_free_values(val_buf,val_s); sp=base;
    if(r<0) { if(tagged) push_none(); else die("index-of: element not found"); }
    else { spush(val_int(r)); if(tagged) push_ok(); }
}
MUST_PAIR(indexof)
static void prim_at(Frame *env) {
    (void)env; uint32_t key=pop_sym();
    Value next=stack[sp-1];
    int s=val_slots(next),len=(int)next.as.compound.len,base=sp-s;
    int found; ElemRef ref=record_field(&stack[base],s,len,key,&found);
    deep_free_values(&stack[base],ref.base); deep_free_values(&stack[base+ref.base+ref.slots],s-ref.base-ref.slots);
    memmove(&stack[base],&stack[base+ref.base],ref.slots*sizeof(Value));
    sp=base+ref.slots;
}
#define REC_PREAMBLE Value rec_top=speek();int rec_s=val_slots(rec_top),rec_len=(int)rec_top.as.compound.len,rec_base=sp-rec_s
/* [rec][value] -> [rec'] with value under key: the value moves over the old field. The checker proves
   the record has the key. */
static void rec_put(uint32_t key) {
    int v_s=val_slots(stack[sp-1]),v_base=sp-v_s;
    Value rec_top=stack[v_base-1];
    int rec_s=val_slots(rec_top),rec_len=(int)rec_top.as.compound.len,rec_base=v_base-rec_s;
    int found; ElemRef ex=record_field(&stack[rec_base],rec_s,rec_len,key,&found);
    rec_top.loc=0;
    int old_base=rec_base+ex.base,os=ex.slots;
    if(os==v_s) { deep_free_values(&stack[old_base],os); memmove(&stack[old_base],&stack[v_base],(size_t)v_s*sizeof(Value)); sp=v_base; return; }
    replace_run(old_base,os,v_s);
    rec_top.as.compound.slots=(uint32_t)(rec_s-os+v_s); stack[sp-1]=rec_top;
}
static void prim_into(Frame *e) { (void)e; rec_put(pop_sym()); }
static void prim_edit(Frame *env) {
    uint32_t key=pop_sym(); POP_BODY(fn); REC_PREAMBLE;
    int found; ElemRef ref=record_field(&stack[rec_base],rec_s,rec_len,key,&found);
    stack_room(ref.slots,"edit");
    deep_copy_values(&stack[sp],&stack[rec_base+ref.base],ref.slots); sp+=ref.slots;
    eval_body(fn_buf,fn_s,env);
    rec_put(key);
}
typedef struct BoxData { Value *data; int slots; } BoxData;
static void prim_box(Frame *e){(void)e;Value top=speek();int s=val_slots(top);BoxData *bd=malloc(sizeof(BoxData));bd->data=malloc(s*sizeof(Value));bd->slots=s;VCPY(bd->data,&stack[sp-s],s);sp-=s;Value v;v.tag=VAL_BOX;v.loc=0;v.as.box=bd;spush(v);}
static void prim_free(Frame *e){
    (void)e;Value v=spop();
    BoxData *bd=(BoxData*)v.as.box;deep_free_values(bd->data,bd->slots);free(bd->data);free(bd);
}
#define BOX_UNPACK(who) POP_BODY(fn); Value box_val=spop();  \
    BoxData *bd=(BoxData*)box_val.as.box; stack_room(bd->slots,who)
/* The body reads a deep copy; the box keeps its own. */
static void prim_lend(Frame *env) {
    BOX_UNPACK("lend"); int sp0=sp;
    deep_copy_values(&stack[sp],bd->data,bd->slots); sp+=bd->slots;
    eval_body(fn_buf,fn_s,env);
    stack_room(1,"lend");
    memmove(&stack[sp0+1],&stack[sp0],(size_t)(sp-sp0)*sizeof(Value));
    stack[sp0]=box_val; sp++;
}
/* The body takes ownership of the contents and returns the replacement. */
static void prim_mutate(Frame *env) {
    BOX_UNPACK("mutate"); SPUSH(bd->data,bd->slots); free(bd->data); bd->data=NULL;
    eval_body(fn_buf,fn_s,env);
    int ns=val_slots(stack[sp-1]);
    bd->data=malloc((size_t)ns*sizeof(Value)); if(!bd->data) die("mutate: out of memory for %d values", ns);
    bd->slots=ns; VCPY(bd->data,&stack[sp-ns],ns); sp-=ns; spush(box_val);
}
static DictData *dict_clone(DictData *orig);
/* Boxes and dicts nest through their contents, and both walks recurse in C, so both stop at a fixed depth. */
#define BOX_DEPTH_MAX 512
static int box_walk_depth = 0;
static void deep_copy_values(Value *dst, const Value *src, int slots) {
    VCPY(dst,src,slots);
    if(++box_walk_depth > BOX_DEPTH_MAX){ box_walk_depth=0;
        die("boxes and dicts nest more than %d deep here, so this value cannot be copied. Flatten the data: keep the items in one list or dict and refer to them by key.", BOX_DEPTH_MAX); }
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
        die("boxes and dicts nest more than %d deep here, so this value cannot be freed. Flatten the data: keep the items in one list or dict and refer to them by key.", BOX_DEPTH_MAX); }
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
    int i=dict_probe(dd,key,klen);
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
static unsigned char *pop_byte_list_buf(const char *who, int *out_len) {
    Value top=spop(); int len=(int)top.as.compound.len;
    unsigned char *buf=malloc(len?len:1);
    for(int i=0;i<len;i++){ int64_t c=stack[sp-len+i].as.i;
        if(c<0||c>255) die("%s: byte %d is %lld, outside 0-255",who,i,(long long)c);
        buf[i]=(unsigned char)c; }
    sp-=len; *out_len=len; return buf;
}
static void prim_fail(Frame *e) {
    (void)e; int n, s0=sp; char *msg=(char*)pop_byte_list_buf("fail",&n);
    if(!n){ sp=s0; die("fail: the text is empty; expected a message"); }
    for(int i=0;i<n;i++) if(!msg[i]){ sp=s0; die("fail: the text holds a NUL byte at offset %d; expected text without NUL bytes", i); }
    die("%.*s", n, msg);
}
static void push_byte_list(const unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) spush(val_int(buf[i]));
    spush(val_compound(VAL_LIST, (int)len, (int)len + 1));
}
static void push_string_bytes(const char *buf, int len) { push_byte_list((const unsigned char*)buf, (size_t)len); }
static void push_c_string(const char *s) { push_byte_list((const unsigned char*)s, strlen(s)); }
static void push_fail(const char *msg) { push_c_string(msg); push_no(); }
static void prim_float_str(Frame *e){(void)e;char b[40];float_text(b,sizeof b,pop_float());push_c_string(b);}
static Value dict_val(DictData *dd){Value v={0};v.tag=VAL_DICT;v.loc=0;v.as.box=dd;return v;}
static void prim_dict(Frame *e){(void)e;DictData *dd=calloc(1,sizeof(DictData));spush(dict_val(dd));}
static void prim_insert(Frame *e) {
    (void)e; POP_VAL(val); int klen; char *key=(char*)pop_byte_list_buf("insert",&klen);
    Value dv=speek();
    DictData *dd=(DictData*)dv.as.box;
    dict_put(dd,key,klen,val_buf,val_s);
    free(key);
}
static void prim_of(Frame *e) {
    (void)e; int klen; char *key=(char*)pop_byte_list_buf("of",&klen);
    Value dv=speek();
    DictData *dd=(DictData*)dv.as.box;
    DictEntry *ent=dict_get(dd,key,klen);
    if(!ent){ push_string_bytes(key,klen); free(key); push_no(); return; }
    free(key);
    /* A deep copy: the dict keeps its own boxes and dicts. */
    stack_room(ent->nvals,"of");
    deep_copy_values(&stack[sp],ent->vals,ent->nvals); sp+=ent->nvals; push_ok();
}
static void prim_remove(Frame *e) {
    (void)e; int klen; char *key=(char*)pop_byte_list_buf("remove",&klen);
    Value dv=speek();
    DictData *dd=(DictData*)dv.as.box;
    dict_del(dd,key,klen); free(key);
}
static void prim_entries(Frame *e) {
    (void)e; Value dv=speek();
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
    Binding *b=lu.bind; Value *v=b->vals; int s=b->slots;
    if(!b->word){
        if(s==1&&v->tag<=VAL_SYM&&sp<STACK_MAX){ stack[sp++]=*v; return; }
        if(b->heap){ stack_room(s,sym_name(sym)); deep_copy_values(&stack[sp],v,s); sp+=s; } else { SPUSH(v,s); if(b->tuples) vals_retain(v,s); } return; }
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
    Value hdr=body[slots-1];
    eval_in(body, slots, hdr.as.compound.env?hdr.as.compound.env:env);
}
/* A [...] or {...} literal's code runs in the running frame, and what it leaves becomes one list or record. */
__attribute__((noinline)) static void lit_run(Value *code, int slots, Frame *ee) {
    Value hdr=code[slots-1]; int base=sp,n=0,rec=!(hdr.flags&VF_LIST);
    eval_in(code,slots,ee); current_loc=hdr.loc;
    /* a record's key is one slot before its value */
    for(int p=sp;p>base;n++) p-=val_slots(stack[p-1])+rec;
    spush(val_compound(rec?VAL_RECORD:VAL_LIST,n,sp-base+1)); stack[sp-1].loc=hdr.loc;
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
                uint32_t n=pop_sym();
                int ds=val_slots(stack[sp-1]);
                /* A body written right before the name makes a word. */
                int word = stack[sp-1].tag==VAL_TUPLE && k>=2 && body[st?st[k-1]-1:k-2].tag==VAL_TUPLE;
                sp-=ds; frame_bind(ee,n,&stack[sp],ds,word);
            }
            else if(__builtin_expect(prof_on,0)) prof_dispatch(ep->as.xt.sym,ee);
            else dispatch_word(ep->as.xt.sym,ee);
            /* the bodies the primitive took are used up */
            while(staged_n>s0){ Value *h=&aux[staged[--staged_n]]; if(h->tag==VAL_TUPLE) frame_drop(h->as.compound.env); }
            asp=a1;
        } else if(is_compound(ep->tag)){
            /* `{clauses} case`, `(body) pthen`, `(body) dip`, `(then) (else) if` and `(pred) (body) while`
               written in place run from this body rather than copy their bodies to the stack and then to the aux
               stack. A body's header holds the frame it was built in, so these run in ee. */
            if(k+1<len){
                const Value *b1=&body[(st?st[k+2]:k+2)-1];
                if(b1->tag==VAL_XT && ((ep->tag==VAL_RECORD && b1->as.xt.fn==prim_case) || (ep->tag==VAL_TUPLE && (b1->as.xt.fn==prim_pthen || b1->as.xt.fn==prim_dip)))){
                    if(b1->loc) current_loc=b1->loc;
                    int a1=asp; if(ep->tag==VAL_RECORD) case_run(&body[eo],es,ee); else if(b1->as.xt.fn==prim_pthen) pthen_run(&body[eo],es,ee); else dip_run(&body[eo],es,ee); asp=a1;
                    k++; continue;
                }
            }
            if(ep->tag==VAL_TUPLE && k+2<len){
                int e2=st?st[k+2]:k+2, s2=st?st[k+3]-st[k+2]:1, e1=st?st[k+1]:k+1, s1=e2-e1;
                const Value *b2=&body[e2+s2-1];
                if(b2->tag==VAL_XT && body[e2-1].tag==VAL_TUPLE && (b2->as.xt.fn==prim_if || b2->as.xt.fn==prim_while)){
                    if(b2->loc) current_loc=b2->loc;
                    if(b2->as.xt.fn==prim_if){ Value c=spop(); if(c.as.i) eval_in(&body[eo],es,ee); else eval_in(&body[e1],s1,ee); }
                    else for(;;){ eval_in(&body[eo],es,ee); if(!pop_int()) break; eval_in(&body[e1],s1,ee); }
                    k+=2; continue;
                }
            }
            /* a literal's code is never one of the bodies above: the checker refuses `[x] dip` and `[x] (y) if` */
            if(ep->flags&(VF_LIST|VF_REC)){ lit_run(&body[eo],es,ee); continue; }
            SPUSH(&body[eo],es);
            if(ep->tag==VAL_TUPLE){ stack[sp-1].as.compound.env=ee; frame_ref(ee); }
        } else spush(*ep);
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
            stack_room(tt->as.str.len+1,"a string literal");
            for(int c=0;c<tt->as.str.len;c++) spush(with_tok(val_int(tt->as.str.codes[c]),tt));
            spush(with_tok(val_compound(VAL_LIST,tt->as.str.len,tt->as.str.len+1),tt)); ec++; break;
        case TOK_LPAREN:{int nc=(j+toks[j].span);build_tuple(toks,j+1,nc,tc,env);stack[sp-1].loc=LOC_PACK(tt->fid,tt->line,tt->col);ec++;j=nc;break;}
        case TOK_LBRACKET:{
            int bc=(j+toks[j].span);
            if(bc+1<tc&&toks[bc+1].tag==TOK_WORD&&toks[bc+1].as.sym==S_EFFECT){if(ec>0&&stack[sp-1].tag==VAL_SYM){sp--;ec--;}j=bc+1;break;}
            build_tuple(toks,j+1,bc,tc,env); stack[sp-1].loc=LOC_PACK(tt->fid,tt->line,tt->col); stack[sp-1].flags|=VF_LIST; ec++; j=bc; break;
        }
        case TOK_LBRACE:{
            int bc=(j+toks[j].span);
            if(!(bc+1<tc&&toks[bc+1].tag==TOK_WORD&&toks[bc+1].as.sym==S_CASE)){
                build_tuple(toks,j+1,bc,tc,env); stack[sp-1].loc=LOC_PACK(tt->fid,tt->line,tt->col); stack[sp-1].flags|=VF_REC; ec++; j=bc; break; }
            /* `{clauses} case` is built once, when the program is read, so case runs its clauses from this body */
            int lb=sp; eval(toks+j+1,bc-j-1,env); int ts=sp-lb,nf=0,p=sp;
            while(p>lb){p-=val_slots(stack[p-1])+1;nf++;}
            spush(with_tok(val_compound(VAL_RECORD,nf,ts+1),tt));
            ec++; j=bc; break;
        }
        default: break;
        }
    }
    Value hdr=val_compound(VAL_TUPLE,ec,sp-eb+1); if(ft) hdr.loc=LOC_PACK(ft->fid,ft->line,ft->col);
    if(binds) hdr.flags|=VF_BINDS;
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
    "([] rot push swap push) 'couple let\n"
    "([] (cat) fold) 'flatten let\n"
    "(0.0 swap fsub) 'fneg let\n"
    "(dup 0.0 flt (fneg) () if) 'fabs let\n"
    "(swap flt) 'fgt let\n"
    "(dup 0 lt (drop -1) (dup 0 eq (drop 0) (drop 1) if) if) 'sign let\n"
    "(rot swap min max) 'clamp let\n"
    "('ok tag) 'ok let\n"
    "('no tag) 'no let\n"
    "(() no) 'none let\n"
    "('body let {'ok (body apply) 'no (no)} case) 'then let\n"
    "('fb let {'ok () 'no (drop fb)} case) 'default let\n"
    "(over over div rot rot mod swap) 'divmod let\n"
    "(-1 bxor) 'bnot let\n"
    "(dict-entries ('key at) each) 'dict-keys let\n"
    "('wr-m let wr-m mod dup 0 eq not over 0 lt wr-m 0 lt eq not and (wr-m plus) () if) 'wrap let\n"
    "([] ('dd-x let dup dd-x member (dd-x drop) (dd-x push) if) fold) 'dedup let\n"
    "3.14159265358979323846 'pi let\n"
    "6.28318530717958647692 'tau let\n"
    "(255 band) 'byte-mask let\n"
    "('b let 0 8 range (7 swap sub b swap shr 1 band) each) 'byte-bits let\n"
    "(0 (swap 1 shl bor) fold) 'bits-byte let\n"
    "('n let n 1 lt (n \"chunks: the size must be at least 1\" fail) () if [] swap (dup len 0 eq not) (dup n take-n swap (push) dip n drop-n) while drop) 'chunks let\n"

;
/* ---- SDL ---- */
#ifdef SLAP_SDL
#include <SDL.h>
#include <SDL_syswm.h>
#ifdef __APPLE__
#include <objc/message.h>
#endif
static uint8_t *canvas=NULL,*rgb=NULL; static int canvas_w=-1,canvas_h=-1;
static SDL_Window *sdl_window=NULL; static SDL_Renderer *sdl_renderer=NULL; static SDL_Texture *sdl_texture=NULL;
#define MAX_HANDLERS 16
static struct{uint32_t event_sym;Value *handler_body;int handler_slots;} event_handlers[MAX_HANDLERS];
static int handler_count=0;
static Value *render_body=NULL; static int render_slots=0;
static uint8_t gray_lut[4]={0,85,170,255};
/* A window size of 0 in either axis keeps a 0-pixel canvas: pixel and fill-rect clip against it. */
static void canvas_alloc(int w,int h) {
    free(canvas);free(rgb);canvas=rgb=NULL; if(sdl_texture){SDL_DestroyTexture(sdl_texture);sdl_texture=NULL;}
    if(w<=0||h<=0){canvas_w=canvas_h=0;return;}
    size_t n=(size_t)w*(size_t)h;
    canvas=calloc(n,1); rgb=malloc(n*3);
    if(!canvas||!rgb) die("canvas: out of memory for a %dx%d canvas (%zu bytes)",w,h,n*4);
    canvas_w=w; canvas_h=h;
    if(sdl_renderer){
        sdl_texture=SDL_CreateTexture(sdl_renderer,SDL_PIXELFORMAT_RGB24,SDL_TEXTUREACCESS_STREAMING,w,h);
        if(!sdl_texture) die("SDL_CreateTexture: %dx%d canvas: %s",w,h,SDL_GetError());
    }
}
static void sdl_init(void) {
    if(sdl_window) return;
    if(SDL_Init(SDL_INIT_VIDEO)<0) die("SDL_Init: %s",SDL_GetError());
    SDL_Rect ub; if(SDL_GetDisplayUsableBounds(0,&ub)<0) die("SDL_GetDisplayUsableBounds: %s",SDL_GetError());
    sdl_window=SDL_CreateWindow("slap",ub.x,ub.y,ub.w,ub.h,SDL_WINDOW_BORDERLESS|SDL_WINDOW_RESIZABLE);
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
    int w,h; SDL_GetWindowSize(sdl_window,&w,&h); canvas_alloc(w,h);
}
static void sdl_present(void) {
    SDL_RenderClear(sdl_renderer);
    if(sdl_texture){
        for(int i=0;i<canvas_w*canvas_h;i++){uint8_t g=gray_lut[canvas[i]&3];rgb[i*3]=rgb[i*3+1]=rgb[i*3+2]=g;}
        SDL_UpdateTexture(sdl_texture,NULL,rgb,canvas_w*3);SDL_RenderCopy(sdl_renderer,sdl_texture,NULL,NULL);}
    SDL_RenderPresent(sdl_renderer);
}
static int64_t pop_color(const char *word){int64_t c=pop_int();if(c<0||c>3)die("%s: color %lld is not 0-3",word,(long long)c);return c;}
static void canvas_need(const char *w){if(canvas_w<0)die("%s: no canvas yet; the canvas exists once show starts, so draw in show's render body or in a handler",w);}
static void prim_clear(Frame *e){(void)e;int c=(int)pop_color("clear");canvas_need("clear");if(canvas)memset(canvas,c,(size_t)canvas_w*(size_t)canvas_h);}
static void prim_pixel(Frame *e){(void)e;int64_t color=pop_color("pixel"),y=pop_int(),x=pop_int();canvas_need("pixel");if(x>=0&&x<canvas_w&&y>=0&&y<canvas_h)canvas[y*canvas_w+x]=(uint8_t)color;}
/* Clipped in int64 before it draws, so a rect far off the canvas costs nothing. w and h are positive
   there, so x+w and y+h can only overflow upward. */
static void prim_fill_rect(Frame *e){(void)e;int64_t c=pop_color("fill-rect"),h=pop_int(),w=pop_int(),y=pop_int(),x=pop_int(),xe,ye;
    canvas_need("fill-rect");
    if(w<=0||h<=0) return;
    if(__builtin_add_overflow(x,w,&xe)) xe=INT64_MAX;
    if(__builtin_add_overflow(y,h,&ye)) ye=INT64_MAX;
    int64_t x0=x<0?0:x, x1=xe<canvas_w?xe:canvas_w, y0=y<0?0:y, y1=ye<canvas_h?ye:canvas_h;
    for(int64_t r=y0;r<y1&&x0<x1;r++) memset(&canvas[r*canvas_w+x0],(int)c,(size_t)(x1-x0));}
static uint32_t sym_tick=0,sym_keydown=0,sym_keyup=0,sym_mousedown=0,sym_mouseup=0,sym_mousemove=0,sym_resize=0;
static void show_intern_syms(void) {
    if(!sym_tick){sym_tick=sym_intern("tick");sym_keydown=sym_intern("keydown");sym_keyup=sym_intern("keyup");sym_mousedown=sym_intern("mousedown");sym_mouseup=sym_intern("mouseup");sym_mousemove=sym_intern("mousemove");sym_resize=sym_intern("resize");}
}
static void prim_on(Frame *e) {
    (void)e; uint32_t ev=pop_sym(); Value fn_top=speek();
    int fn_s=val_slots(fn_top); if(handler_count>=MAX_HANDLERS) die("on: too many event handlers");
    Value *hb=malloc((size_t)fn_s*sizeof(Value)); if(!hb) die("on: out of memory"); VCPY(hb,&stack[sp-fn_s],fn_s); event_handlers[handler_count].handler_body=hb;
    event_handlers[handler_count].handler_slots=fn_s; sp-=fn_s;
    show_intern_syms();
    if(ev!=sym_tick&&ev!=sym_keydown&&ev!=sym_keyup&&ev!=sym_mousedown&&ev!=sym_mouseup&&ev!=sym_mousemove&&ev!=sym_resize)
        die("on: unknown event '%s; the events are 'tick 'keydown 'keyup 'mousedown 'mouseup 'mousemove 'resize",sym_name(ev));
    event_handlers[handler_count].event_sym=ev; handler_count++;
}
static void run_resize(int64_t w,int64_t h,Frame *env){for(int k=0;k<handler_count;k++)if(event_handlers[k].event_sym==sym_resize){spush(val_int(w));spush(val_int(h));eval_body(event_handlers[k].handler_body,event_handlers[k].handler_slots,env);}}
static void show_dispatch_event(SDL_Event *ev, Frame *env) {
    if(ev->type==SDL_WINDOWEVENT&&ev->window.event==SDL_WINDOWEVENT_SIZE_CHANGED){
        int w,h; SDL_GetWindowSize(sdl_window,&w,&h);
        if(w!=canvas_w||h!=canvas_h){canvas_alloc(w,h);run_resize(w,h,env);}
        return;}
    if(ev->type==SDL_KEYDOWN||ev->type==SDL_KEYUP){uint32_t ksym=ev->type==SDL_KEYDOWN?sym_keydown:sym_keyup;for(int h=0;h<handler_count;h++)if(event_handlers[h].event_sym==ksym){spush(val_int((int64_t)ev->key.keysym.sym));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}}
    int is_mouse=0;
    if(ev->type==SDL_MOUSEBUTTONDOWN||ev->type==SDL_MOUSEBUTTONUP||ev->type==SDL_MOUSEMOTION) is_mouse=1;
    if(is_mouse){
        int sx,sy; SDL_GetMouseState(&sx,&sy);
        int64_t mx=sx, my=sy;
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
    Value fn_top=speek();
    render_slots=val_slots(fn_top); render_body=malloc((size_t)render_slots*sizeof(Value)); if(!render_body) die("show: out of memory");
    VCPY(render_body,&stack[sp-render_slots],render_slots); sp-=render_slots;
    show_intern_syms();
    if(headless_mode){
        canvas_alloc(640,480); run_resize(640,480,env);
        for(int64_t frame=0;;frame++){show_tick_render(frame,env);SDL_Delay(16);}
    }
    sdl_init();
#ifdef __EMSCRIPTEN__
    show_env=env; show_frame=0; run_resize(canvas_w,canvas_h,env);
    emscripten_set_main_loop(show_one_frame,0,1);
#else
    int64_t frame=0; int running=1; run_resize(canvas_w,canvas_h,env);
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
static char *pop_string_path(const char *who, int *len) {
    unsigned char *raw = pop_byte_list_buf(who, len);
    char *buf = realloc(raw, *len + 1); if (!buf) die("%s: out of memory for a %d-byte path", who, *len);
    buf[*len] = '\0'; return buf;
}
/* A failed read, write or ls gives "path: reason" no, with every byte of the path. */
static void push_path_fail(const char *path, int len, const char *reason) {
    int rl = (int)strlen(reason);
    for (int i = 0; i < len; i++) spush(val_int((unsigned char)path[i]));
    spush(val_int(':')); spush(val_int(' '));
    for (int i = 0; i < rl; i++) spush(val_int((unsigned char)reason[i]));
    spush(val_compound(VAL_LIST, len + 2 + rl, len + 3 + rl)); push_no();
}
/* A C string stops at a NUL, so a path with one cannot name a file. */
static int path_has_nul(const char *path, int len) {
    if (!memchr(path, 0, len)) return 0;
    push_path_fail(path, len, "contains a NUL byte"); return 1;
}
static void prim_read(Frame *e) {
    (void)e; int plen;char *path=pop_string_path("read",&plen);
    if(path_has_nul(path,plen)){free(path);return;}
    FILE *f=fopen(path,"rb");
    if(!f) { push_path_fail(path,plen,strerror(errno)); free(path); return; }
    size_t n=0,cap=65536,got; unsigned char *buf=malloc(cap); errno=0;
    while(buf&&(got=fread(buf+n,1,cap-n,f))>0){ n+=got;
        if(n>STACK_MAX-2) die("read: %s holds more than %d bytes, the most read returns: each byte, the list and its 'ok take one slot of the %d-slot stack. Split the file into smaller files.", path, STACK_MAX-2, STACK_MAX);
        if(n==cap){ cap*=2; buf=realloc(buf,cap); } }
    if(!buf) die("read: out of memory reading %s", path);
    int bad=ferror(f), er=errno; fclose(f);
    if(bad) { free(buf); push_path_fail(path,plen,er?strerror(er):"the read did not finish"); free(path); return; }
    stack_room((int)n+2,"read"); push_byte_list(buf,n);free(buf);free(path); push_ok();
}
/* Linux's own limit on symlinks met while resolving one path. */
#define LINK_HOPS_MAX 40
/* Read once in main: reading the umask means setting it, which races with SDL's threads. */
static mode_t file_umask;
/* As fopen "wb" writes: in place, so a failure leaves a short file. Returns 0, or -1 with errno set. */
static int write_in_place(const char *path, const unsigned char *buf, int len) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    errno = 0; int bad = (int)fwrite(buf, 1, len, f) != len; bad |= fclose(f) != 0;
    /* C does not require fwrite or fclose to set errno */
    if (bad && !errno) errno = EIO;
    return bad ? -1 : 0;
}
/* write replaces a regular file whole: the bytes go to a hidden temp file in its directory, which is
   renamed over it, so a crash, a full disk or a size limit leaves the old file. A symlink resolves to
   its target, so the link stays, and a dangling one creates its target. The replacement keeps the
   mode and group. Where it could not match the old file (another owner, a second hard link, a
   directory the user may not write, a group or mode the filesystem refuses) or the temp path passes
   PATH_MAX, the write goes in place,
   as it does for a device, a FIFO or a directory. Returns 0, or -1 with errno set. */
static int write_atomic(const char *path, const unsigned char *buf, int len) {
    char *target = strdup(path), *tmp = NULL, link[PATH_MAX]; struct stat st; int exists = 1, fd = -1, er = 0;
    if (!target) die("write: out of memory for the path %s", path);
    for (int hops = 0;; hops++) {
        if (lstat(target, &st) < 0) { if (errno != ENOENT) { er = errno; goto fail; } exists = 0; break; }
        if (!S_ISLNK(st.st_mode)) break;
        if (hops == LINK_HOPS_MAX) { er = ELOOP; goto fail; }
        ssize_t n = readlink(target, link, sizeof link - 1);
        if (n < 0 || n == (ssize_t)sizeof link - 1) { er = n < 0 ? errno : ENAMETOOLONG; goto fail; }
        link[n] = 0;
        char *slash = strrchr(target, '/'); size_t dl = link[0] == '/' || !slash ? 0 : (size_t)(slash - target) + 1;
        char *next = malloc(dl + (size_t)n + 1); if (!next) die("write: out of memory resolving the symlink %s", target);
        memcpy(next, target, dl); memcpy(next + dl, link, (size_t)n + 1); free(target); target = next;
    }
    if (exists && (!S_ISREG(st.st_mode) || st.st_nlink > 1 || st.st_uid != geteuid())) goto in_place;
    /* rename needs only the directory's permission: a file the user may not write stays as it is. */
    if (exists && access(target, W_OK)) { er = errno; goto fail; }
    char *slash = strrchr(target, '/'); size_t dl = slash ? (size_t)(slash - target) + 1 : 0;
    if (!(tmp = malloc(dl + 12))) die("write: out of memory for a temp name beside %s", target);
    memcpy(tmp, target, dl); memcpy(tmp + dl, ".slapXXXXXX", 12);
    /* A directory the user may not write, or a temp path past PATH_MAX: in place still works, or gives the reason. */
    if ((fd = mkstemp(tmp)) < 0) { er = errno; free(tmp); tmp = NULL; if (er == EACCES || er == EPERM || er == ENAMETOOLONG) goto in_place; goto fail; }
    if (fchmod(fd, exists ? st.st_mode & 0777 : 0666 & ~file_umask) || (exists && fchown(fd, (uid_t)-1, st.st_gid))) {
        er = errno; if (exists) { close(fd); unlink(tmp); free(tmp); goto in_place; } goto fail; }
    for (int off = 0; off < len; ) { ssize_t w = write(fd, buf + off, (size_t)(len - off)); if (w < 0 && errno != EINTR) { er = errno; goto fail; } if (w > 0) off += (int)w; }
    /* Data first, then the name. On macOS fsync leaves the drive's cache, so a power loss there can still lose the write. */
    if (fsync(fd)) { er = errno; goto fail; }
    int closed = close(fd); fd = -1; if (closed) { er = errno; goto fail; }
    if (rename(tmp, target)) { er = errno; goto fail; }
    free(tmp); free(target); return 0;
in_place:
    free(target);
    return write_in_place(path, buf, len);
fail:
    if (fd >= 0) close(fd);
    if (tmp) unlink(tmp);
    free(tmp); free(target); errno = er; return -1;
}
static void prim_write(Frame *e) {
    (void)e; int len;unsigned char *buf=pop_byte_list_buf("write",&len);int plen;char *path=pop_string_path("write",&plen);
    if(path_has_nul(path,plen)){free(buf);free(path);return;}
    /* Opening /dev/stdout again would skip what print still buffers, and on Linux it truncates a
       file the shell redirected stdout to. Write through the process's own streams instead. */
    FILE *std=strcmp(path,"/dev/stdout")==0?stdout:strcmp(path,"/dev/stderr")==0?stderr:NULL;
    int failed;
    if(std){ if(std==stderr) fflush(stdout); errno=0; failed=(int)fwrite(buf,1,len,std)!=len||fflush(std); if(failed&&!errno) errno=EIO; }
    else failed=write_atomic(path,buf,len)<0;
    if(failed){int er=errno;free(buf);push_path_fail(path,plen,strerror(er));free(path);return;}
    free(buf);free(path); spush(val_int(1)); push_ok();
}
static void prim_ls(Frame *e) {
    (void)e; int plen;char *path=pop_string_path("ls",&plen);
    if(path_has_nul(path,plen)){free(path);return;}
    DIR *d=opendir(path); if(!d) { push_path_fail(path,plen,strerror(errno)); free(path); return; }
    struct dirent *ent; int base=sp,count=0;
    /* readdir returns NULL at the end and on an error; only an error sets errno */
    for(;;){errno=0; if(!(ent=readdir(d))) break; if(strcmp(ent->d_name,".")==0||strcmp(ent->d_name,"..")==0)continue;
        push_c_string(ent->d_name);count++;}
    int er=errno; closedir(d);
    if(er){ sp=base; push_path_fail(path,plen,strerror(er)); free(path); return; }
    spush(val_compound(VAL_LIST,count,sp-base+1));free(path); push_ok();
}

#ifndef SLAP_WASM
/* A peer that hangs up must make tcp-send return 'no, not raise SIGPIPE. macOS
   has no MSG_NOSIGNAL, and Linux has no SO_NOSIGPIPE. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
/* recv on a connected socket gives up on a silent peer after 30 s; a listening socket must not (accept waits for clients). Returns -1 with errno set when an option fails. */
static int sock_setup(int fd) {
    struct timeval tv = {30, 0};
#ifdef SO_NOSIGPIPE
    int one = 1; if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0) return -1;
#endif
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0) return -1;
    return 0;
}
/* A non-blocking connect that waits at most ms; errno is ETIMEDOUT on expiry. */
static int connect_bounded(int fd, const struct sockaddr *sa, socklen_t sl, int ms) {
    int fl = fcntl(fd, F_GETFL, 0); if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    if (connect(fd, sa, sl) < 0) {
        if (errno != EINPROGRESS) return -1;
        struct pollfd p = {fd, POLLOUT, 0}; int r = poll(&p, 1, ms);
        if (r < 0) return -1;
        if (r == 0) { errno = ETIMEDOUT; return -1; }
        int so = 0; socklen_t l = sizeof so;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &l) < 0) return -1;
        if (so) { errno = so; return -1; }
    }
    return fcntl(fd, F_SETFL, fl);
}
static void prim_tcp_connect(Frame *e) {
    (void)e; int64_t port=pop_int(); int hlen;char *host=pop_string_path("tcp-connect",&hlen);
    if(port<0||port>65535) die("tcp-connect: port %lld is outside 0-65535",(long long)port);
    if(memchr(host,0,hlen)){free(host);push_fail("tcp-connect: the host contains a NUL byte");return;}
    struct addrinfo hints={0},*res; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    char ps[16]; snprintf(ps,sizeof(ps),"%lld",(long long)port);
    char msg[512]; int err=getaddrinfo(host,ps,&hints,&res);
    if(err){snprintf(msg,sizeof msg,"tcp-connect: cannot resolve %s: %s",host,gai_strerror(err));free(host);push_fail(msg);return;}
    /* One 10 s deadline covers every address. getaddrinfo waits as long as the system resolver does. */
    int fd=-1,last=0; struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    for(struct addrinfo *a=res;a&&fd<0;a=a->ai_next){
        clock_gettime(CLOCK_MONOTONIC,&t1); long left=10000-((t1.tv_sec-t0.tv_sec)*1000+(t1.tv_nsec-t0.tv_nsec)/1000000);
        if(left<=0){last=ETIMEDOUT;break;}
        fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol);
        if(fd<0){last=errno;continue;}
        if(connect_bounded(fd,a->ai_addr,a->ai_addrlen,(int)left)<0||sock_setup(fd)<0){last=errno;close(fd);fd=-1;}
    }
    freeaddrinfo(res);
    if(fd<0){if(last==ETIMEDOUT)snprintf(msg,sizeof msg,"tcp-connect: %s:%s timed out after 10 s",host,ps);else snprintf(msg,sizeof msg,"tcp-connect: cannot connect to %s:%s: %s",host,ps,strerror(last));free(host);push_fail(msg);return;}
    free(host); if(shell_jmp) shell_sock(fd+1); spush(val_int(fd));push_ok();
}
static void prim_tcp_send(Frame *e) {
    (void)e; int len; unsigned char *buf = pop_byte_list_buf("tcp-send", &len);
    int fd = (int)pop_int(); spush(val_int(fd));
    /* One 30 s deadline bounds the whole call. A blocking send restarts its timeout each time the peer frees some room,
       so a peer that reads a trickle would hold it forever: the socket sends without blocking, and poll waits for room. */
    int fl = fcntl(fd, F_GETFL, 0); const char *err = NULL;
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) { free(buf); push_fail(strerror(errno)); return; }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t sent = 0; sent < (size_t)len && !err; ) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += n; continue; }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) { err = strerror(errno); break; }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long left = 30000 - ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        struct pollfd p = {fd, POLLOUT, 0}; int r = left > 0 ? poll(&p, 1, (int)left) : 0;
        if (r < 0) err = strerror(errno); else if (r == 0) err = "timed out after 30 s";
    }
    if (fcntl(fd, F_SETFL, fl) < 0 && !err) err = strerror(errno);
    free(buf);
    if (err) { push_fail(err); return; }
    spush(val_int(1)); push_ok();
}
static void prim_tcp_recv(Frame *e) {
    (void)e; int64_t maxlen = pop_int(); if (maxlen < 1) die("tcp-recv: length must be at least 1, got %lld", (long long)maxlen);
    int fd = (int)pop_int(); spush(val_int(fd));
    static unsigned char buf[65536]; ssize_t n = recv(fd, buf, maxlen < (int64_t)sizeof buf ? (size_t)maxlen : sizeof buf, 0);
    if (n < 0) { push_fail(errno == EAGAIN || errno == EWOULDBLOCK ? "timed out after 30 s" : strerror(errno)); return; }
    push_byte_list(buf, n); push_ok();
}
static void prim_tcp_close(Frame *e) { (void)e; int fd=(int)pop_int(); if(shell_jmp){shell_sock(-(fd+1));return;} if(close(fd)) die("tcp-close: closing socket %d failed: %s", fd, strerror(errno)); }
static void prim_tcp_listen(Frame *e) {
    (void)e; int64_t port=pop_int(); if(port<0||port>65535) die("tcp-listen: port %lld is outside 0-65535",(long long)port);
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0){push_fail(strerror(errno));return;}
    int opt=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(0x7f000001);addr.sin_port=htons((uint16_t)port);
    if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0||listen(fd,128)<0){char msg[128];snprintf(msg,sizeof msg,"tcp-listen: port %lld: %s",(long long)port,strerror(errno));close(fd);push_fail(msg);return;}
    if(shell_jmp) shell_sock(fd+1); spush(val_int(fd));push_ok();
}
/* A client that resets before tcp-accept is done with it: accept fails with ECONNABORTED or EPROTO
   (Linux passes pending network errors through; see accept(2)) or EINVAL while the socket still
   listens, or accept succeeds and macOS then refuses socket options on it with EINVAL or ECONNRESET.
   That connection is gone, and the next accept waits for another, so tcp-accept takes the next one. */
static int accept_aborted(int sfd, int er, int accepted) {
    if (accepted) return er == EINVAL || er == ECONNRESET;
    if (er == ECONNABORTED || er == EPROTO) return 1;
    int on = 0; socklen_t l = sizeof on;
    return er == EINVAL && getsockopt(sfd, SOL_SOCKET, SO_ACCEPTCONN, &on, &l) == 0 && on;
}
/* More aborted clients in a row than a flood of hours makes: only a fault of the host reaches it. */
#define ACCEPT_ABORTS_MAX 1000000L
static void prim_tcp_accept(Frame *e) {
    (void)e; int sfd=(int)pop_int(); spush(val_int(sfd));
    for(long aborts=0;;){
        struct sockaddr_in ca; socklen_t al=sizeof(ca);
        int cfd=accept(sfd,(struct sockaddr*)&ca,&al), er=errno;
        if(cfd>=0){ if(sock_setup(cfd)==0){ if(shell_jmp) shell_sock(cfd+1); spush(val_int(cfd)); push_ok(); return; } er=errno; close(cfd); }
        if(!accept_aborted(sfd,er,cfd>=0)){ push_fail(strerror(er)); return; }
        if(++aborts==ACCEPT_ABORTS_MAX){ char msg[160]; snprintf(msg,sizeof msg,"%ld clients in a row reset before accept; the last: %s",aborts,strerror(er)); push_fail(msg); return; }
    }
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
static void prim_args(Frame *e) {
    (void)e; int ts=0; for(int i=0;i<cli_argc;i++){push_c_string(cli_args[i]);ts+=(int)strlen(cli_args[i])+1;}
    spush(val_compound(VAL_LIST,cli_argc,ts+1));
}
static void prim_isheadless(Frame *e){(void)e;spush(val_int(headless_mode));}
#define PRIM(nm,body) static void prim_##nm(Frame *e){(void)e;body;}
PRIM(take_n, prim_slice_n(1)) PRIM(drop_n, prim_slice_n(0))
#undef PRIM
#define R(n,f) {#n,prim_##f,NULL}
#define M(n,f) {n,prim_##f,prim_##f##_must}
static void register_prims(void) {
    static struct{const char*n;PrimFn f,m;} t[]={
        R(dup,dup),R(drop,drop),R(swap,swap),R(over,over),R(rot,rot),R(dip,dip),R(apply,apply),
        R(plus,plus),R(sub,sub),R(mul,mul),R(div,div),R(mod,mod),
        {"fplus",prim_plus,NULL},{"fsub",prim_sub,NULL},{"fmul",prim_mul,NULL},{"fdiv",prim_div,NULL},
        R(band,band),R(bor,bor),R(bxor,bxor),R(shl,shl),R(shr,shr),
        R(eq,eq),R(lt,lt),{"flt",prim_lt,NULL},R(and,and),R(or,or),
        R(print,print),R(assert,assert),R(random,random),
        R(if,if),R(case,case),R(while,while),
        R(itof,itof),R(ftoi,ftoi),{"float-str",prim_float_str,NULL},{"float-bits",prim_float_bits,NULL},{"bits-float",prim_bits_float,NULL},R(fsqrt,fsqrt),
        R(ffloor,ffloor),R(fround,fround),R(fexp,fexp),R(flog,flog),R(fpow,fpow),R(fatan2,fatan2),
        R(len,size),R(push,push_op),M("pop",pop),
        M("get",get),M("peek",peek),M("nth",nth),M("set",set),R(cat,concat),
        R(reverse,reverse),R(zip,zip),{"take-n",prim_take_n,NULL},{"drop-n",prim_drop_n,NULL},R(range,range),
        R(fold,fold),R(each,each),R(filter,filter),R(sort,sort),M("index-of",indexof),
        R(at,at),R(into,into),R(edit,edit),
        R(millis,millis),R(datetime,datetime),R(box,box),R(free,free),R(lend,lend),R(mutate,mutate),
        R(dict,dict),R(insert,insert),R(of,of),R(remove,remove),
        {"dict-entries",prim_entries,NULL},
        R(tag,tag),R(must,must),R(pthen,pthen),
        R(read,read),R(write,write),R(ls,ls),
        M("str-find",strfind),{"str-split",prim_str_split,NULL},
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

/* A word the checker knows but this build lacks (SDL words in ./slap, tcp-* in wasm) stops the
   program before anything runs. */
static void refuse_missing_words(Token *toks, int n) {
    for(int i=0;i<n;i++){Token*t=&toks[i]; uint32_t w=t->as.sym;
        if(t->tag!=TOK_WORD||prim_fns[w]||!(ty_builtin[w]||w==S_ON||w==S_SHOW)) continue;
        current_loc=LOC_PACK(t->fid,t->line,t->col);
#ifdef SLAP_WASM
        die("'%s' is not in the wasm build: a browser has no TCP sockets. Remove the tcp-* words from this program.",sym_name(w));
#else
        die("'%s' needs the SDL build, but this is the terminal build. Build it with make slap-sdl and run ./slap-sdl.",sym_name(w));
#endif
    }
}
#ifndef SLAP_WASM
/* The stack on one line, bottom first, or (empty). */
static void shell_show_stack(void) {
    if (!sp) { puts("(empty)"); return; }
    int n = 0; for (int p = sp; p > 0; p -= val_slots(stack[p-1])) n++;
    int *ends = malloc((size_t)n * sizeof *ends); if (!ends) die("shell: out of memory listing %d values", n);
    for (int p = sp, k = n; p > 0; p -= val_slots(stack[p-1])) ends[--k] = p;
    for (int k = 0, start = 0; k < n; start = ends[k++]) { if (k) putchar(' '); val_print(&stack[start], ends[k] - start, stdout); }
    putchar('\n'); free(ends);
}
/* The shell: reads a line at a time from a terminal, checks it after the lines it accepted before,
   runs it, and prints the stack. A line that fails to check or to run is discarded: the stack, the
   global bindings and the sockets on the stack go back to what they were before it; files and
   network I/O it did stay done. Names are never bound twice, so a line only adds global bindings,
   and trimming the frame undoes it. A discarded line stays in the history as an empty line, so
   messages give line numbers as typed. Returns the last line's exit status. */
static int shell(Token *table, int table_count, Token *combined, int prelude_n) {
    char *line = NULL, *hist = strdup(""); size_t lcap = 0, hlen = 0; int hist_toks = 0, failed = 0;
    if (!hist) die("shell: out of memory");
    ty_shell = 1;
    for (;;) {
        fputs("> ", stdout); fflush(stdout);
        ssize_t n = getline(&line, &lcap, stdin);
        if (n < 0) { if (ferror(stdin)) die("shell: cannot read the terminal: %s", strerror(errno)); break; }
        const char *bad = memchr(line, 0, (size_t)n) ? "the line holds a NUL byte; a slap program is text"
            : hlen + (size_t)n + 1 > (size_t)16 << 20 ? "the session passed 16 MiB of input, the most a program holds; start a new one" : NULL;
        /* a line ended by ^D, not a newline, still ends here */
        int nl = n == 0 || line[n-1] != '\n';
        char *src = malloc(hlen + (size_t)n + 2); if (!src) die("shell: out of memory for %zu bytes of input", hlen + (size_t)n);
        memcpy(src, hist, hlen); memcpy(src + hlen, line, (size_t)n); if (nl) src[hlen + (size_t)n] = '\n'; src[hlen + (size_t)n + (size_t)nl] = 0;
        int sp0 = sp, binds0 = global_frame->bind_count;
        Value *snap = malloc((size_t)(sp0 ? sp0 : 1) * sizeof(Value)); if (!snap) die("shell: out of memory copying the stack");
        deep_copy_values(snap, stack, sp0);
        volatile int ok = 0, died = 0, toks = 0; jmp_buf jb; shell_socks_n = 0;
        if (bad) fprintf(stderr, "shell: %s\n", bad);
        else if (!setjmp(jb)) {
            shell_jmp = &jb;
            store_source_lines(src, FID_STDIN);
            tok_limit = TOK_MAX - prelude_n; lex(src, FID_STDIN); toks = tok_count;
            memcpy(&combined[prelude_n], tokens, (size_t)tok_count * sizeof(Token));
            int errors = infer_program(table, table_count, combined, prelude_n + toks, prelude_n);
            if (errors) fprintf(stderr, "%d type error(s)\n", errors);
            else {
                refuse_missing_words(&combined[prelude_n + hist_toks], toks - hist_toks);
                current_loc = LOC_PACK(FID_STDIN, 0, 0);
                eval(&combined[prelude_n + hist_toks], toks - hist_toks, global_frame);
                ok = 1;
            }
        } else died = 1;
        shell_jmp = NULL;
        if (ok) {
            for (int k = 0; k < shell_socks_n; k++) if (shell_socks[k] < 0 && close(-shell_socks[k] - 1)) die("tcp-close: closing socket %d failed: %s", -shell_socks[k] - 1, strerror(errno));
            deep_free_values(snap, sp0); free(snap); free(hist); hist = src; hlen = strlen(src); hist_toks = toks;
        } else {
            if (died) {
                /* What the line left on the stack and the aux stack is dropped unfreed: after an error it may be half moved. */
                for (int k = 0; k < shell_socks_n; k++) if (shell_socks[k] > 0) close(shell_socks[k] - 1);
                asp = 0; staged_n = 0; eval_depth = 0; user_loc = 0;
                VCPY(stack, snap, sp0); sp = sp0;
                frame_trim(global_frame, binds0);
                /* a word the error left running stays pinned; nothing runs between lines */
                for (int b = 0; b < global_frame->bind_cap; b++) global_frame->bindings[b].pinned = 0;
            } else deep_free_values(snap, sp0);
            free(snap); free(src);
            char *h = realloc(hist, hlen + 2); if (!h) die("shell: out of memory");
            hist = h; hist[hlen++] = '\n'; hist[hlen] = 0; store_source_lines(hist, FID_STDIN);
        }
        failed = !ok;
        shell_show_stack(); fflush(stdout);
    }
    putchar('\n');
    return failed;
}
#endif
int main(int argc, char **argv) {
    char stack_anchor; c_stack_base = &stack_anchor;
#ifdef __EMSCRIPTEN__
    long stack_lim=4L<<20; /* -sSTACK_SIZE in the Makefile */
#else
    struct rlimit rl; if(getrlimit(RLIMIT_STACK,&rl)) die("getrlimit: cannot read the C stack limit: %s", strerror(errno));
    /* Past 8 MiB, RLIM_INFINITY included, the limit changes nothing: c_stack_max stops at 7 MiB. */
    long stack_lim=rl.rlim_cur>(rlim_t)(8L<<20)?8L<<20:(long)rl.rlim_cur;
#endif
    if(stack_lim<2L<<20) die("the C stack limit is %ld KB, and slap needs at least 2048 KB. Raise it with: ulimit -s 8192", stack_lim/1024);
    c_stack_max=stack_lim-(1L<<20)<7L<<20?stack_lim-(1L<<20):7L<<20;
    rng_state=(uint64_t)time(NULL)^((uint64_t)getpid()<<32); atexit(stdout_check);
#ifdef SIGXFSZ
    /* A file size limit (ulimit -f) then fails a write with EFBIG, which write reports, instead of
       killing the process in the middle of the write. */
    signal(SIGXFSZ, SIG_IGN);
#endif
    file_umask=umask(0); umask(file_umask);
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
    static Token combined[TOK_MAX], table[TOK_MAX]; int cpos=0;
    lex(PRELUDE, FID_PRELUDE); memcpy(combined,tokens,tok_count*sizeof(Token)); cpos=tok_count; eval(combined,cpos,global);
    store_source_lines(TYPES, FID_BUILTIN); lex(TYPES, FID_BUILTIN);
    int table_count=tok_count; memcpy(table,tokens,table_count*sizeof(Token));
    current_loc=LOC_PACK(FID_STDIN,0,0);
#ifndef SLAP_WASM
    if(isatty(0)&&!check_only) return shell(table,table_count,combined,cpos);
#endif
#ifdef SLAP_WASM
    FILE *f=fopen("program.slap","r"); if(!f){fprintf(stderr,"error: cannot open 'program.slap'\n");return 1;}
#else
    FILE *f=stdin;
#endif
    /* stdin may never end (`./slap < /dev/zero`), so the program has a size bound: 16 MiB. */
    long sz=0,cap=4096; char *src=malloc(cap); long n;
    while(src&&(n=fread(src+sz,1,cap-sz,f))>0){ sz+=n;
        if(sz>16L<<20) die("the program is larger than 16 MiB; expected a slap source file. Check what stdin is redirected from.");
        if(sz==cap){cap*=2;src=realloc(src,cap);} }
    if(!src) die("out of memory reading the program: %ld bytes", cap);
    if(ferror(f)) die("cannot read the program from stdin: %s. Check what stdin is redirected from.", strerror(errno));
    src[sz]=0;
    { char *nul=memchr(src,0,sz); if(nul) die("the program holds a NUL byte at offset %ld; a slap program is text. Remove the byte, or write \\0 inside a string literal.", (long)(nul-src)); }
#ifdef SLAP_WASM
    fclose(f);
#endif
    if(sz==0){fprintf(stderr,"usage: slap [--check] [--headless] [--profile] [args...] < file.slap\n");return 1;}
    store_source_lines(src, FID_STDIN);
    static Token user_tokens[TOK_MAX];
    int user_start=cpos;
    tok_limit=TOK_MAX-cpos; lex(src, FID_STDIN); int user_tok_count=tok_count;
    memcpy(user_tokens,tokens,user_tok_count*sizeof(Token));
    current_loc=LOC_PACK(FID_STDIN,0,0);
    memcpy(&combined[cpos],user_tokens,user_tok_count*sizeof(Token)); cpos+=user_tok_count;
    int errors=infer_program(table,table_count,combined,cpos,user_start);
    if(errors>0){fprintf(stderr,"%d type error(s)\n",errors);return 1;}
    if(check_only){fprintf(stderr,"type check passed\n");return 0;}
    refuse_missing_words(user_tokens,user_tok_count);
    current_loc=LOC_PACK(FID_STDIN,0,0);
    /* Registered after stdout_check, so it runs first, on die too. */
    if(profile){ prof_last=prof_now(); atexit(prof_report); }
    eval(user_tokens,user_tok_count,global);
    return 0;
}
