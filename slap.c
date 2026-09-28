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
typedef struct Binding { uint32_t sym; int slots, cap, pinned, word, heap; Value *vals; } Binding;
struct Frame {
    struct Frame *parent; int bind_count, bind_cap;
    int captured; /* a tuple made in this frame may refer to it, so bindings outlive the body that made them */
    Binding *bindings;
    int32_t *hash; uint32_t hash_mask; /* binding index+1 by symbol; 0 is empty */
};
/* The bindings a scoped body shadowed, detached so the scope can put them back. */
typedef struct { int bi, slots, cap, word, heap; Value *vals; } Saved;
static Saved *saves=NULL;
static int saves_cap=0, saves_sp=0;
static int frame_save_active=0;
static Frame *frame_save_target=NULL;
static int frame_save_sbc=0;
static int frame_save_sb0=0; /* where the current scope's records start in saves */
static Frame *frame_new(Frame *parent) {
    Frame *f = calloc(1, sizeof(Frame));
    if (!f) die("out of memory: cannot allocate a call frame");
    f->parent = parent; return f;
}
static int frame_find(Frame *f, uint32_t sym) {
    if (!f->hash) return -1;
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
    int32_t *h = calloc((size_t)cap*4, sizeof(int32_t));
    if (!b || !h) die("out of memory: cannot grow a frame to %d bindings", cap);
    memset(&b[f->bind_cap], 0, (size_t)(cap-f->bind_cap)*sizeof(Binding));
    free(f->hash); f->bindings = b; f->bind_cap = cap; f->hash = h; f->hash_mask = (uint32_t)cap*4-1;
    for (int i = 0; i < f->bind_count; i++) hash_put(f, i);
}
static void deep_free_values(Value *vals, int slots);
/* Dicts the program has made; with none, no binding can hold one and binds skip the scan. */
static int dicts_made;
static int vals_hold_dict(const Value *v, int n) { if (!dicts_made) return 0; for (int i = 0; i < n; i++) if (v[i].tag == VAL_DICT) return 1; return 0; }
/* Drop bindings [n, bind_count). Newest first, so no probe chain is cut short. */
static void frame_trim(Frame *f, int n) {
    for (int i = f->bind_count-1; i >= n; i--) {
        if (f->bindings[i].heap) { deep_free_values(f->bindings[i].vals, f->bindings[i].slots); f->bindings[i].heap = 0; f->bindings[i].slots = 0; }
        uint32_t s = f->bindings[i].sym & f->hash_mask;
        while (f->hash[s] != i+1) s = (s+1) & f->hash_mask;
        f->hash[s] = 0;
    }
    f->bind_count = n;
}
static void frame_bind(Frame *f, uint32_t sym, Value *vals, int slots, int word) {
    int bi = frame_find(f, sym);
    if (bi < 0) { if (f->bind_count == f->bind_cap) frame_grow(f); bi = f->bind_count++; f->bindings[bi].sym = sym; hash_put(f, bi); }
    Binding *b = &f->bindings[bi];
    int save = frame_save_active && f == frame_save_target && bi < frame_save_sbc;
    /* Save a caller's binding only on its first rebind in this scope. */
    for (int p = frame_save_sb0; save && p < saves_sp; p++) if (saves[p].bi == bi) save = 0;
    if (save) {
        if (saves_sp == saves_cap) {
            saves_cap = saves_cap ? saves_cap*2 : 64; saves = realloc(saves, (size_t)saves_cap*sizeof(Saved));
            if (!saves) die("out of memory: cannot save %d shadowed bindings", saves_cap);
        }
        saves[saves_sp++] = (Saved){bi, b->slots, b->cap, b->word, b->heap, b->vals};
        b->vals = NULL; b->cap = 0; b->heap = 0;
    } else if (b->heap) { deep_free_values(b->vals, b->slots); b->heap = 0; }
    if (slots > b->cap || b->pinned) {
        if (b->pinned) { b->vals = NULL; b->cap = 0; }
        b->vals = realloc(b->vals, (size_t)slots*sizeof(Value));
        if (!b->vals) die("out of memory: cannot bind %d values to '%s", slots, sym_name(sym));
        b->cap = slots;
    }
    VCPY(b->vals, vals, slots); b->slots = slots; b->word = word; b->heap = vals_hold_dict(vals, slots);
}
typedef struct { Binding *bind; Frame *frame; } Lookup;
static Lookup frame_lookup(Frame *f, uint32_t sym) {
    for (Frame *cur = f; cur; cur = cur->parent) {
        int i = frame_find(cur, sym);
        if (i >= 0) { Lookup r = {&cur->bindings[i], cur}; return r; }
    }
    Lookup r = {NULL, NULL}; return r;
}
static void eval(Token *toks, int count, Frame *env);
static void eval_body(Value *body, int slots, Frame *env);
static void eval_tuple_scoped(Value *body, int slots, Frame *env);
static void dispatch_word(uint32_t sym, Frame *env);
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
static int print_max = 0;
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
static void val_print(Value *data, int slots, FILE *out) {
    c_stack_check("printing a deeply nested value");
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
    print_max = 32;
    if (sp == 0) { fprintf(out, "\n    stack: (empty)\n"); return; }
    fprintf(out, "\n    stack (%d slot%s):\n", sp, sp==1?"":"s");
    int pos=sp, shown=0;
    while(pos>0&&shown<5){int s=val_slots(stack[pos-1]);pos-=s;fprintf(out,"      %d: ",shown);val_print(&stack[pos],s,out);fprintf(out,"\n");shown++;}
    if(pos>0){int rem=0;while(pos>0){pos-=val_slots(stack[pos-1]);rem++;}fprintf(out,"      ... %d more\n",rem);}
}
static int val_equal(Value *a, int aslots, Value *b, int bslots) {
    if (aslots != bslots) return 0;
    Value atop = a[aslots - 1], btop = b[bslots - 1];
    if (atop.tag != btop.tag) return 0;
    switch (atop.tag) {
    case VAL_INT: return atop.as.i == btop.as.i;
    case VAL_FLOAT: return atop.as.f == btop.as.f;
    case VAL_SYM: return atop.as.sym == btop.as.sym;
    case VAL_XT: return atop.as.xt.sym == btop.as.xt.sym;
    case VAL_TUPLE: case VAL_LIST: case VAL_RECORD: case VAL_TAGGED:
        if (atop.as.compound.len != btop.as.compound.len) return 0;
        for (int i = 0; i < aslots - 1; i++) if (!val_equal(&a[i], 1, &b[i], 1)) return 0;
        return 1;
    case VAL_BOX: case VAL_DICT: return atop.as.box == btop.as.box;
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
static uint32_t S_CAT, S_NTH, S_LET, S_IF, S_EFFECT, S_CHECK, S_OK, S_NO, S_NONE, S_HALT, S_TAG, S_PTHEN, S_CASE, S_MUST, S_QUOTE, S_THEN, S_AT, S_EDIT, S_REC, S_INTO,
    S_WHILE, S_EACH, S_FOLD, S_FILTER, S_REPEAT, S_ON, S_SHOW, S_PARSE_HTTP;
/* ---- TYPE SYSTEM ---- */
typedef enum { DIR_IN, DIR_OUT } SlotDir;
typedef enum { OWN_OWN, OWN_COPY, OWN_MOVE, OWN_LENT, OWN_AUTO } OwnMode;
typedef enum { TC_NONE=0, TC_INT, TC_FLOAT, TC_SYM, TC_NUM, TC_LIST, TC_TUPLE, TC_REC, TC_BOX, TC_TAGGED, TC_SEQ, TC_EQ, TC_ORD, TC_SEMIGROUP, TC_FUNCTOR, TC_DICT, TC_SIZED } TypeConstraint;
enum { HO_BODY_1TO1=1, HO_BRANCHES_AGREE=2, HO_SAVES_UNDER=4,
       HO_APPLY_EFFECT=16, HO_BOX_BORROW=32, HO_BOX_MUTATE=64 };
typedef struct { const char *name; uint32_t sym; int need; int out; TypeConstraint out_type; uint8_t flags; } HOEffect;
#define HO_OP_COUNT 12
static HOEffect ho_ops[HO_OP_COUNT] = {
    {"apply",0,1,0,TC_NONE,HO_APPLY_EFFECT},{"dip",0,2,1,TC_NONE,HO_APPLY_EFFECT|HO_SAVES_UNDER},
    {"if",0,3,1,TC_NONE,HO_BRANCHES_AGREE},
    {"fold",0,3,1,TC_NONE,0},{"each",0,2,1,TC_FUNCTOR,HO_BODY_1TO1},
    {"while",0,2,0,TC_NONE,0},
    {"lend",0,2,2,TC_BOX,HO_BOX_BORROW},{"mutate",0,2,1,TC_BOX,HO_BOX_MUTATE},
    {"case",0,3,1,TC_NONE,HO_BRANCHES_AGREE},
    {"on",0,2,0,TC_NONE,0},{"show",0,1,0,TC_NONE,0},
    {"edit",0,3,1,TC_TAGGED,HO_BODY_1TO1},
};
static HOEffect *ho_ops_find(uint32_t sym) {
    for (int i = 0; i < HO_OP_COUNT; i++) if (ho_ops[i].sym == sym) return &ho_ops[i];
    return NULL;
}
static void syms_init(void) {
    S_LET=sym_intern("let");
    S_IF=sym_intern("if"); S_EFFECT=sym_intern("effect"); S_CHECK=sym_intern("check");
    S_OK=sym_intern("ok"); S_NO=sym_intern("no");
    S_CASE=sym_intern("case"); S_MUST=sym_intern("must"); S_NONE=sym_intern("none"); S_HALT=sym_intern("halt"); S_TAG=sym_intern("tag"); S_PTHEN=sym_intern("pthen"); S_QUOTE=sym_intern("quote"); S_THEN=sym_intern("then");
    S_AT=sym_intern("at"); S_EDIT=sym_intern("edit"); S_REC=sym_intern("rec"); S_INTO=sym_intern("into");
    S_CAT=sym_intern("cat"); S_NTH=sym_intern("nth"); S_WHILE=sym_intern("while"); S_EACH=sym_intern("each"); S_FOLD=sym_intern("fold"); S_FILTER=sym_intern("filter"); S_REPEAT=sym_intern("repeat"); S_ON=sym_intern("on"); S_SHOW=sym_intern("show"); S_PARSE_HTTP=sym_intern("parse-http");
    for (int i = 0; i < HO_OP_COUNT; i++) ho_ops[i].sym = sym_intern(ho_ops[i].name);
}
typedef struct {
    uint32_t type_var; TypeConstraint constraint, elem_constraint; OwnMode ownership; SlotDir direction;
    uint32_t either_syms[8]; TypeConstraint either_types[8], either_elems[8]; uint32_t either_tvars[8]; int either_count;
} TypeSlot;
#define TYPE_SLOTS_MAX 16
typedef struct { TypeSlot slots[TYPE_SLOTS_MAX]; int slot_count; } TypeSig;
#define TYPESIG_MAX 512
static struct { uint32_t sym; TypeSig sig; } type_sigs[TYPESIG_MAX];
static int type_sig_count = 0;
static void typesig_register(uint32_t sym, TypeSig *sig) {
    for (int i = 0; i < type_sig_count; i++) { if (type_sigs[i].sym == sym) { type_sigs[i].sig = *sig; return; } }
    if (type_sig_count >= TYPESIG_MAX) die("more than %d type signatures; TYPESIG_MAX in slap.c sets the limit", TYPESIG_MAX);
    type_sigs[type_sig_count].sym = sym; type_sigs[type_sig_count].sig = *sig; type_sig_count++;
}
static TypeSig *typesig_find(uint32_t sym) {
    for (int i = 0; i < type_sig_count; i++) if (type_sigs[i].sym == sym) return &type_sigs[i].sig;
    return NULL;
}
static const struct { const char *name; TypeConstraint tc; } tc_names[] = {
    {"int",TC_INT},{"float",TC_FLOAT},{"sym",TC_SYM},{"num",TC_NUM},
    {"list",TC_LIST},{"tuple",TC_TUPLE},{"rec",TC_REC},{"box",TC_BOX},{"tagged",TC_TAGGED},
    {"seq",TC_SEQ},{"eq",TC_EQ},{"ord",TC_ORD},
    {"semigroup",TC_SEMIGROUP},{"functor",TC_FUNCTOR},{"dict",TC_DICT},{"sized",TC_SIZED},{NULL,TC_NONE}
};
static TypeConstraint parse_constraint(const char *tw) {
    for (int i=0; tc_names[i].name; i++) if (strcmp(tw,tc_names[i].name)==0) return tc_names[i].tc;
    return TC_NONE;
}
static int tc_is_container(TypeConstraint c) { return c == TC_LIST || c == TC_BOX || c == TC_TAGGED || c == TC_SEQ || c == TC_FUNCTOR || c == TC_DICT; }
static int tc_is_concrete(TypeConstraint c) {
    return c == TC_INT || c == TC_FLOAT || c == TC_SYM || c == TC_LIST || c == TC_TUPLE || c == TC_REC || c == TC_BOX || c == TC_TAGGED || c == TC_DICT;
}
static TypeSig parse_type_annotation(Token *toks, int start, int end) {
    TypeSig sig; memset(&sig, 0, sizeof(sig));
    int i = start;
    while (i < end) {
        if (sig.slot_count >= TYPE_SLOTS_MAX) die("type annotation: more than %d slots", TYPE_SLOTS_MAX);
        TypeSlot *slot = &sig.slots[sig.slot_count]; memset(slot, 0, sizeof(*slot));
        int slot_start = i;
        /* handle {... } either pattern: {'ok type 'no type} either move out */
        if (toks[i].tag == TOK_LBRACE) {
            int brace_start = i;
            i += toks[i].span + 1;
            /* parse variant pairs from brace: 'sym type 'sym type ... */
            int ec = 0;
            for (int b = brace_start + 1; b < i - 1; ) {
                if (toks[b].tag == TOK_SYM) {
                    if (ec == 8) die("type annotation: an either takes at most 8 variants");
                    slot->either_syms[ec] = toks[b].as.sym; b++;
                    if (b < i - 1 && toks[b].tag == TOK_LPAREN) {
                        /* () means tuple/unit */
                        slot->either_types[ec] = TC_TUPLE; b++;
                        if (b < i - 1 && toks[b].tag == TOK_RPAREN) b++;
                    } else if (b < i - 1 && toks[b].tag == TOK_SYM) {
                        slot->either_types[ec] = TC_NONE;
                        slot->either_tvars[ec] = toks[b].as.sym; b++;
                    } else if (b < i - 1 && toks[b].tag == TOK_WORD) {
                        const char *tn = sym_name(toks[b].as.sym);
                        if ((slot->either_types[ec] = parse_constraint(tn)) == TC_NONE) die("type annotation: unknown type word '%s'", tn);
                        b++;
                        /* `int list`: a container of the type before it. */
                        if (b < i - 1 && toks[b].tag == TOK_WORD && tc_is_container(parse_constraint(sym_name(toks[b].as.sym)))) {
                            slot->either_elems[ec] = slot->either_types[ec]; slot->either_types[ec] = parse_constraint(sym_name(toks[b].as.sym)); b++; }
                    } else die("type annotation: either variant '%s needs a type, a 'variable or ()", sym_name(slot->either_syms[ec]));
                    ec++;
                } else die("type annotation: either variants are 'symbols, as in {'ok int 'no ()}");
            }
            slot->either_count = ec;
            slot->constraint = TC_TAGGED;
            /* skip 'either' keyword if present */
            if (i < end && toks[i].tag == TOK_WORD && strcmp(sym_name(toks[i].as.sym), "either") == 0) i++;
            slot_start = i;
        }
        while (i < end) {
            if (toks[i].tag != TOK_WORD && toks[i].tag != TOK_SYM) break;
            const char *w = sym_name(toks[i].as.sym);
            if (strcmp(w,"in")==0 || strcmp(w,"out")==0) break;
            i++;
        }
        if (i >= end) {
            if (i > slot_start || slot->constraint == TC_TAGGED) die("type annotation: a slot needs in or out after its type");
            break;
        }
        if (toks[i].tag != TOK_WORD) die("type annotation: expected a type word, a 'variable, in or out");
        slot->direction = (strcmp(sym_name(toks[i].as.sym), "in") == 0) ? DIR_IN : DIR_OUT; i++;
        for (int j = slot_start; j < i - 1; j++) {
            const char *tw = sym_name(toks[j].as.sym);
            if(strcmp(tw,"own")==0){slot->ownership=OWN_OWN;continue;} if(strcmp(tw,"copy")==0){slot->ownership=OWN_COPY;continue;}
            if(strcmp(tw,"move")==0){slot->ownership=OWN_MOVE;continue;} if(strcmp(tw,"lent")==0){slot->ownership=OWN_LENT;continue;}
            if(strcmp(tw,"auto")==0){slot->ownership=OWN_AUTO;continue;}
            TypeConstraint c = parse_constraint(tw);
            if (c != TC_NONE) {
                if (tc_is_container(c) && slot->constraint != TC_NONE) slot->elem_constraint = slot->constraint;
                slot->constraint = c; continue;
            }
            if (toks[j].tag != TOK_SYM) die("type annotation: unknown type word '%s'", tw);
            if (!slot->type_var) slot->type_var = toks[j].as.sym;
        }
        sig.slot_count++;
    }
    return sig;
}
static const char *constraint_name(TypeConstraint c) {
    if (c == TC_NONE) return "any";
    for (int i=0; tc_names[i].name; i++) if (tc_names[i].tc == c) return tc_names[i].name;
    return "?";
}
typedef struct { int parent; TypeConstraint bound; int elem; int box_c; int tag_p; int union_id;
    int row;  /* a record's keys: an index into TypeChecker.rows, or 0 when they are unknown */
    int rest; /* a record whose keys this one has too, beyond its row */
    int need; /* keys read from this record while it stood for a caller's: every caller must pass them */
    int vrow; /* a tagged value's payload type per tag, as a Row keyed by tag */
    uint8_t open; /* stands for whatever record a caller passes: reading a key adds it to need */
    uint8_t param; /* a call's copy of an open input: it is the caller's value, once that meets it */
    uint8_t unknown; /* a value the checker has not seen, or whose type it lost: a value it meets loses its keys, payloads
                        and body too. Every fresh tvar starts so; a placeholder that takes the first type it meets clears it. */
    uint8_t input; /* a declared input: the caller's value, whose keys the body cannot see */
    uint8_t okno; /* a caller's value that `then` takes: every caller must pass one tagged only 'ok or 'no */
    int code; /* a body's value: 1 + the index of its effect, or -1 when it may be one of several bodies */
    uint32_t reads; /* a key some body this value may be reads from its input */ } TVarEntry;
/* A record's keys, each with the type of its value: key[i] holds a value of tvar tv[i]. */
typedef struct { int n, cap, line; uint32_t *key; int *tv; } Row;
#define UNION_MAX 2048
#define UNION_VARIANTS_MAX 16
/* inferred: the tags a value can carry, found by the checker. Only a declared `either` makes `case` exhaustive. */
typedef struct { uint32_t syms[UNION_VARIANTS_MAX]; TypeConstraint types[UNION_VARIANTS_MAX]; int count, inferred; } UnionDef;
#define EFFECT_MAX 4096
typedef struct {
    int consumed, produced;
    TypeConstraint out_type;
    int scheme_base, scheme_count;
    int in_tvars[16], out_tvars[16], in_count, out_count;
    int out_effect;
    int out_tags;    /* the tags its top output can carry: a union id, or 0 when unknown */
    int has_let;     /* body binds a name it reads back: a lend snapshot could alias it */
    int output_is_linear; /* body's sole output is itself a linear-capturing closure; apply should mark output AT_LINEAR */
    int diverges;    /* body never returns, so it agrees with any branch */
    int clauses;     /* a {...} clause list: the counts are its deciding clause's, payload included */
    int nclause, dflt_live; /* for a clause list: its clause bodies, and whether the default can be left */
    int clause_eff[16], clause_pred[16]; uint32_t clause_key[16]; /* body, predicate or -1, tag or 0 */
    int opaque;      /* body runs code of unknown effect, so running the body does too */
    uint32_t reads;  /* a key the body reads from an input with at or edit: only a checked call may run it */
    int placeholder; /* a word's effect while its own body is checked: calls to it are recursive */
    int unknown;     /* the body ran code of unknown effect, even behind a declared signature */
    int body_id;     /* which tc_tuple checked it: recursive calls inside it are recorded under this id */
} TupleEffect;
#define AT_LINEAR 1
#define AT_CONSUMED 2
#define AT_OPAQUE 4 /* a declared tuple input: code whose effect the signature does not state */
typedef struct {
    TypeConstraint type; int tvar_id; uint32_t sym_id;
    uint8_t flags; int source_line; int effect_idx;
} AbstractType;
#define ASTACK_MAX 65536
#define TC_BINDS_MAX 16384
/* word: bound from a body written right before `'name let`: a lookup runs it. Any other binding is a value. */
typedef struct { uint32_t sym; AbstractType atype; int def_line; int consumed_line; int word; } TCBinding;
typedef struct { uint32_t sym; int line; } TCUnknown;
#define TC_UNKNOWN_MAX 256
typedef struct {
    AbstractType data[ASTACK_MAX]; int sp, errors;
    TCBinding bindings[TC_BINDS_MAX]; int bind_count;
    int recur_pending; uint32_t recur_sym;
    TCUnknown unknowns[TC_UNKNOWN_MAX]; int unknown_count;
    TVarEntry *tvars; int tvar_count, tvar_cap;
    TupleEffect effects[EFFECT_MAX]; int effect_count;
    int user_start, prelude_sig_count, sp_floor, body_depth;
    int underflows; /* values taken from below sp_floor; see tc_take */
    int diverged; /* the current body has run a word that never returns */
    int opaque_at; /* where code of unknown effect last ran in this body, or -1; see tc_take */
    int unknown_code; /* this body ran code that may replace any value below it: not only its own recursive calls */
    int saw_linear_capture;  /* set by binding-lookup of a linear value; consumed by enclosing tuple-body inference */
    int prelude_binds; /* bindings below this index come from the prelude, which exists before the program is built */
    int literal_depth; /* inside a [...] or {...} literal, which build_tuple evaluates when the program is read */
    UnionDef unions[UNION_MAX]; int union_count;
    Row *rows; int row_count, row_cap;
    /* Recursive calls, checked against the word's inputs once its body is checked. */
#define RCALL_MAX 4096
    struct { int effect, n, line, body, nout; int arg[16], out[16]; } rcalls[RCALL_MAX]; int rcall_count;
    int body_ids, cur_body; /* the body being checked; a body's recursive calls move out to where it is applied */
    int *branch_used, branch_used_n; /* (binding, line) pairs the then-branch of the next `if` consumed */
    int *early, early_n, early_cap; /* (word, line) pairs: calls to a declared word before its body was checked */
    uint32_t prescan_self; /* the word whose effect the pre-scan is guessing, or 0 */
    uint32_t *unk; int unk_n, unk_cap; /* words whose body runs code of unknown effect and that are called before it is checked */
    int quiet; /* the first pass counts errors and prints none */
    #define FWD_MAX 1024
    uint32_t fwd[FWD_MAX]; int fwd_line[FWD_MAX], fwd_n; /* words the program declares with `'name [sig] effect` */
    /* `on` handlers, run by `show` on the state below it and their event's ints. */
    struct { int effect, nevent; } handlers[16]; int handler_count;
    AbstractType trial_out[16]; /* what the last tc_trial left, as values */
} TypeChecker;
static int tvar_fresh(TypeChecker *tc) {
    if (tc->tvar_count >= tc->tvar_cap) {
        tc->tvar_cap = tc->tvar_cap ? tc->tvar_cap*2 : 4096;
        tc->tvars = realloc(tc->tvars, (size_t)tc->tvar_cap*sizeof(TVarEntry));
        if (!tc->tvars) die("out of memory: %d type variables", tc->tvar_cap);
    }
    int id = tc->tvar_count++;
    tc->tvars[id].parent = id; tc->tvars[id].bound = TC_NONE; tc->tvars[id].elem = 0; tc->tvars[id].box_c = 0; tc->tvars[id].tag_p = 0; tc->tvars[id].union_id = 0;
    tc->tvars[id].row = 0; tc->tvars[id].rest = 0; tc->tvars[id].open = 0; tc->tvars[id].need = 0; tc->tvars[id].vrow = 0; tc->tvars[id].param = 0;
    tc->tvars[id].unknown = 1; tc->tvars[id].input = 0; tc->tvars[id].okno = 0; tc->tvars[id].code = 0; tc->tvars[id].reads = 0;
    return id;
}
static int row_new(TypeChecker *tc, int line) {
    if (tc->row_count >= tc->row_cap) {
        tc->row_cap = tc->row_cap ? tc->row_cap*2 : 1024;
        tc->rows = realloc(tc->rows, (size_t)tc->row_cap*sizeof(Row));
        if (!tc->rows) die("out of memory: %d record rows", tc->row_cap);
    }
    Row *r = &tc->rows[tc->row_count]; memset(r, 0, sizeof(*r)); r->line = line;
    return tc->row_count++;
}
static int row_find(TypeChecker *tc, int r, uint32_t key) {
    for (int i = tc->rows[r].n - 1; i >= 0; i--) if (tc->rows[r].key[i] == key) return tc->rows[r].tv[i];
    return 0;
}
static void row_put(TypeChecker *tc, int r, uint32_t key, int tv) {
    Row *w = &tc->rows[r];
    for (int i = 0; i < w->n; i++) if (w->key[i] == key) { w->tv[i] = tv; return; }
    if (w->n == w->cap) {
        w->cap = w->cap ? w->cap*2 : 8;
        w->key = realloc(w->key, (size_t)w->cap*sizeof(uint32_t)); w->tv = realloc(w->tv, (size_t)w->cap*sizeof(int));
        if (!w->key || !w->tv) die("out of memory: a record row with %d keys", w->cap);
    }
    w->key[w->n] = key; w->tv[w->n++] = tv;
}
static int row_copy(TypeChecker *tc, int r, int line) {
    int c = row_new(tc, line);
    for (int i = 0; i < tc->rows[r].n; i++) row_put(tc, c, tc->rows[r].key[i], tc->rows[r].tv[i]);
    return c;
}
static int tvar_unify(TypeChecker *tc, int a, int b);
static int tvar_find(TypeChecker *tc, int id);
static void tc_error(TypeChecker *tc, int line, int origin_line, const char *fmt, ...);
#define REST_MAX 65536
#define REST_DIE die("type checker: a record type extends another more than %d times", REST_MAX)
/* The checker follows this type's keys: it has keys, extends a record, or stands for a caller's value. */
static int rec_tracked(TypeChecker *tc, int r) { r = tvar_find(tc, r); return tc->tvars[r].row || tc->tvars[r].rest || tc->tvars[r].open || tc->tvars[r].need; }
/* This type is a record the checker knows keys of. A caller's value not yet used as a record is not. */
static int rec_known(TypeChecker *tc, int r) {
    for (int hops = 0; r > 0; hops++) {
        if (hops == REST_MAX) REST_DIE;
        r = tvar_find(tc, r);
        if (tc->tvars[r].row || tc->tvars[r].need) return 1;
        if (tc->tvars[r].open) return tc->tvars[r].bound == TC_REC;
        r = tc->tvars[r].rest;
    }
    return 0;
}
/* Whether a meet of two values' types keeps anything: record keys, a caller's value, a tagged value's
   payloads, or a body. */
static int tv_info(TypeChecker *tc, int r) {
    /* The type and the types it contains, each once. Past 64 of them it is taken to know something:
       a meet is then kept. */
    int seen[64], ns = 0, todo[64], nt = 0;
    if (r > 0) todo[nt++] = r;
    while (nt) {
        int t = tvar_find(tc, todo[--nt]), dup = 0;
        for (int k = 0; k < ns; k++) if (seen[k] == t) dup = 1;
        if (dup) continue;
        if (ns == 64) return 1;
        seen[ns++] = t; TVarEntry *e = &tc->tvars[t];
        if (rec_known(tc, t) || e->open || e->input || e->vrow || e->code > 0) return 1;
        int c[3] = {e->elem, e->box_c, e->tag_p};
        for (int k = 0; k < 3; k++) if (c[k] > 0) { if (nt == 64) return 1; todo[nt++] = c[k]; }
    }
    return 0;
}
/* The record type at the end of tv's rest links: the one the others extend. */
static int rec_base(TypeChecker *tc, int tv) {
    for (int hops = 0; ; hops++) {
        if (hops == REST_MAX) REST_DIE;
        int r = tvar_find(tc, tv);
        if (!tc->tvars[r].rest) return r;
        tv = tc->tvars[r].rest;
    }
}
/* Every key a record of type tv has, following rest but stopping before the root stop (0: at the
   end); the nearest key wins. A new row, or 0 when the keys are unknown. */
static int rec_flat_to(TypeChecker *tc, int tv, int stop) {
    if (tv > 0 && tvar_find(tc, tv) != stop && !rec_tracked(tc, tv)) return 0;
    int m = row_new(tc, 0);
    for (int hops = 0; tv > 0; hops++) {
        if (hops == REST_MAX) REST_DIE;
        int r = tvar_find(tc, tv), w = tc->tvars[r].row;
        if (r == stop) break;
        if (!rec_tracked(tc, r)) return 0;
        if (w) { if (!tc->rows[m].line) tc->rows[m].line = tc->rows[w].line;
            for (int i = 0; i < tc->rows[w].n; i++) if (!row_find(tc, m, tc->rows[w].key[i])) row_put(tc, m, tc->rows[w].key[i], tc->rows[w].tv[i]); }
        tv = tc->tvars[r].rest;
    }
    return m;
}
static int rec_flat(TypeChecker *tc, int tv) { return tv > 0 && rec_tracked(tc, tv) ? rec_flat_to(tc, tv, 0) : 0; }
/* The keys both rows have, which a value of either type has. */
static int row_meet(TypeChecker *tc, int a, int b) {
    int m = row_new(tc, tc->rows[a].line);
    for (int i = 0; i < tc->rows[a].n; i++) {
        uint32_t k = tc->rows[a].key[i]; int g = row_find(tc, b, k);
        if (g && !tvar_unify(tc, tc->rows[a].tv[i], g)) row_put(tc, m, k, tc->rows[a].tv[i]);
    }
    return m;
}
/* Every key of either row, with one type for a key in both. */
static int row_union(TypeChecker *tc, int a, int b) {
    if (!a || !b) return a ? a : b;
    int m = row_copy(tc, a, tc->rows[a].line);
    for (int i = 0; i < tc->rows[b].n; i++) { int g = row_find(tc, m, tc->rows[b].key[i]);
        if (g) tvar_unify(tc, g, tc->rows[b].tv[i]); else row_put(tc, m, tc->rows[b].key[i], tc->rows[b].tv[i]); }
    return m;
}
/* The tvar of the value under key k in a record of type tv, or 0 when the checker cannot show the
   key is there. An open record stands for what its caller passes, so reading k adds k to what
   every caller must pass: its need. */
static int rec_has(TypeChecker *tc, int tv, uint32_t k) {
    for (int hops = 0; tv > 0; hops++) {
        if (hops == REST_MAX) REST_DIE;
        int r = tvar_find(tc, tv), w = tc->tvars[r].row, f = w ? row_find(tc, w, k) : 0;
        if (f) return f;
        if (tc->tvars[r].rest) { tv = tc->tvars[r].rest; continue; }
        if (!tc->tvars[r].open) return 0;
        if (tc->tvars[r].bound == TC_NONE) tc->tvars[r].bound = TC_REC;
        if (!w) { w = row_new(tc, 0); tc->tvars[r].row = w; }
        if (!tc->tvars[r].need) tc->tvars[r].need = row_new(tc, 0);
        f = tvar_fresh(tc); tc->tvars[f].open = 1; tc->tvars[f].unknown = 0; row_put(tc, w, k, f); row_put(tc, tc->tvars[r].need, k, f);
        return f;
    }
    return 0;
}
/* The tvar of the value under key k if the checker already knows it is there; adds nothing. */
static int rec_find(TypeChecker *tc, int tv, uint32_t k) {
    for (int hops = 0; tv > 0; hops++) {
        if (hops == REST_MAX) REST_DIE;
        int r = tvar_find(tc, tv), w = tc->tvars[r].row, f = w ? row_find(tc, w, k) : 0;
        if (f) return f;
        tv = tc->tvars[r].rest;
    }
    return 0;
}
/* A record of type arg reaches param, which some body read keys from: arg must have every key in
   param's need. Without check_only, param then stands for arg. The first key arg lacks, or 0. */
/* A record nested this deep holds records of its own kind; its type has no end. */
#define REC_DEPTH_MAX 32
#define REC_TOO_DEEP UINT32_MAX
static uint32_t rec_flow_at(TypeChecker *tc, int arg, int param, int check_only, int depth) {
    int p = tvar_find(tc, param), w = tc->tvars[p].need;
    if (!w) return 0;
    if (depth == REC_DEPTH_MAX) return REC_TOO_DEEP;
    if (!check_only) { tc->tvars[p].open = 0; tc->tvars[p].row = 0; tc->tvars[p].need = 0; }
    for (int i = 0; i < tc->rows[w].n; i++) {
        uint32_t k = tc->rows[w].key[i]; int f = tc->rows[w].tv[i], g = rec_has(tc, arg, k);
        if (!g) return k;
        uint32_t miss = rec_flow_at(tc, g, f, check_only, depth + 1); if (miss) return miss;
        if (!check_only) tvar_unify(tc, f, g);
    }
    if (!check_only) { int a = tvar_find(tc, arg); if (a != p) tc->tvars[p].parent = a; }
    return 0;
}
static uint32_t rec_flow(TypeChecker *tc, int arg, int param, int check_only) { return rec_flow_at(tc, arg, param, check_only, 0); }
/* rb is about to be linked under ra, whose type then stands for both values: what it knows of their
   keys. What callers must pass (need) is never dropped. */
static int rec_find(TypeChecker *tc, int tv, uint32_t k);
static void row_join(TypeChecker *tc, int ra, int rb, TypeConstraint ca, TypeConstraint cb) {
    int need = row_union(tc, tc->tvars[ra].need, tc->tvars[rb].need);
    int ka = rec_tracked(tc, ra), kb = rec_tracked(tc, rb), ua = tc->tvars[ra].unknown, ub = tc->tvars[rb].unknown;
    int pa = tc->tvars[ra].param && tc->tvars[ra].open && !tc->tvars[ra].rest, pb = tc->tvars[rb].param && tc->tvars[rb].open && !tc->tvars[rb].rest;
    /* A call's copy of an input meets the caller's value, whose keys were checked: it is that value. */
    if (pa && !pb) { tc->tvars[ra].row = tc->tvars[rb].row; tc->tvars[ra].rest = tc->tvars[rb].rest; tc->tvars[ra].open = tc->tvars[rb].open; tc->tvars[ra].unknown = ub; }
    if (pa || pb) { tc->tvars[ra].param = 0; tc->tvars[ra].need = need; return; }
    /* A declared input that meets a value the checker knows something of, or a value of unknown
       type, has unknown keys: it may be either. */
    int ia = tc->tvars[ra].input && !ka, ib = tc->tvars[rb].input && !kb;
    if ((ia && (kb || ub || tc->tvars[rb].vrow)) || (ib && (ka || ua || tc->tvars[ra].vrow))) {
        tc->tvars[ra].row = 0; tc->tvars[ra].rest = 0; tc->tvars[ra].open = 0; tc->tvars[ra].unknown = 1; tc->tvars[ra].need = need; return; }
    if (ib) tc->tvars[ra].input = 1;
    /* A record whose keys are unknown, or a value whose type is, makes the keys of both unknown. */
    if ((!kb && (cb == TC_REC || ub)) || (!ka && (ca == TC_REC || ua))) { tc->tvars[ra].row = 0; tc->tvars[ra].rest = 0; tc->tvars[ra].open = 0; tc->tvars[ra].unknown = 1; }
    else if (!kb) {}
    else if (!ka) { tc->tvars[ra].row = tc->tvars[rb].row; tc->tvars[ra].rest = tc->tvars[rb].rest; tc->tvars[ra].open = tc->tvars[rb].open; tc->tvars[ra].param = tc->tvars[rb].param; }
    else {
        int ea = rec_base(tc, ra), eb = rec_base(tc, rb);
        int oa = tc->tvars[ra].open && !tc->tvars[ra].rest, ob = tc->tvars[rb].open && !tc->tvars[rb].rest;
        if (ea == eb) {
            /* Both extend one record: keep it, and the keys both have. A key either side adds or replaces
               has the value types of both sides. */
            int fa = ea != ra ? rec_flat_to(tc, ra, ea) : 0, fb = rec_flat_to(tc, rb, ea), m = row_new(tc, 0);
            for (int s = 0; s < 2; s++) { int w = s ? fb : fa;
                for (int i = 0; w && i < tc->rows[w].n; i++) { uint32_t k = tc->rows[w].key[i];
                    int ga = rec_find(tc, ra, k), gb = rec_find(tc, rb, k);
                    if (ga && gb && !row_find(tc, m, k) && !tvar_unify(tc, ga, gb)) row_put(tc, m, k, ga); } }
            if (ea != ra) { tc->tvars[ra].row = m; tc->tvars[ra].rest = ea; tc->tvars[ra].open = 0; }
        } else if (oa && ob) {
            /* Two inputs of one type: a caller passes both, so both have every key either was read with. */
            tc->tvars[ra].row = row_union(tc, tc->tvars[ra].row, tc->tvars[rb].row);
        } else if (oa || ob) {
            /* A known record meets an input: it must have every key the input was read with. */
            int o = oa ? ra : rb, c = oa ? rb : ra, w = tc->tvars[o].need, flat = rec_flat(tc, c);
            if (w) for (int i = 0; i < tc->rows[w].n; i++) { int g = rec_has(tc, c, tc->rows[w].key[i]);
                if (g) tvar_unify(tc, g, tc->rows[w].tv[i]);
                else tc_error(tc, LOC_LINE(current_loc), 0, "a record without '%s takes the place of a value that must have it: '%s is read from that value with at or edit. Give the record '%s, or keep the two values apart.", sym_name(tc->rows[w].key[i]), sym_name(tc->rows[w].key[i]), sym_name(tc->rows[w].key[i])); }
            /* The two are different values of one type now: it has only the keys both have. */
            int ow = tc->tvars[o].row;
            tc->tvars[ra].row = flat && ow ? row_meet(tc, ow, flat) : 0; tc->tvars[ra].rest = 0; tc->tvars[ra].open = 0;
        } else {
            /* Two known records of one type: it has the keys both have. */
            int fa = rec_flat(tc, ra), fb = rec_flat(tc, rb);
            tc->tvars[ra].row = fa && fb ? row_meet(tc, fa, fb) : 0; tc->tvars[ra].rest = 0; tc->tvars[ra].open = 0;
        }
        /* A meet that knows no keys of what either side had is a record of unknown keys, not a free type. */
        if (!rec_tracked(tc, ra)) tc->tvars[ra].unknown = 1;
    }
    tc->tvars[ra].need = need;
}
static int tvar_find(TypeChecker *tc, int id) {
    if (id <= 0 || id >= tc->tvar_count) die("type checker bug: type variable %d does not exist (there are %d)", id, tc->tvar_count);
    while (tc->tvars[id].parent != id) { tc->tvars[id].parent = tc->tvars[tc->tvars[id].parent].parent; id = tc->tvars[id].parent; }
    return id;
}
static TypeConstraint tvar_resolve(TypeChecker *tc, int id) { return tc->tvars[tvar_find(tc, id)].bound; }
/* Bitmask per TypeConstraint: bit N set means "compatible with TC whose enum == N" */
#define B(x) (1u<<(x))
static const uint32_t tc_compat[] = {
    [TC_NONE]=0xFFFFFFFF,
    [TC_INT]=B(TC_INT)|B(TC_NUM)|B(TC_ORD)|B(TC_EQ),
    [TC_FLOAT]=B(TC_FLOAT)|B(TC_NUM)|B(TC_ORD)|B(TC_EQ),
    [TC_SYM]=B(TC_SYM)|B(TC_EQ),
    [TC_NUM]=B(TC_NUM)|B(TC_INT)|B(TC_FLOAT)|B(TC_EQ)|B(TC_ORD),
    [TC_LIST]=B(TC_LIST)|B(TC_SEQ)|B(TC_SEMIGROUP)|B(TC_FUNCTOR)|B(TC_EQ)|B(TC_SIZED),
    [TC_TUPLE]=B(TC_TUPLE)|B(TC_SEMIGROUP)|B(TC_EQ)|B(TC_SIZED),
    [TC_REC]=B(TC_REC)|B(TC_SEMIGROUP)|B(TC_EQ)|B(TC_SIZED),
    [TC_BOX]=B(TC_BOX),
    [TC_TAGGED]=B(TC_TAGGED)|B(TC_FUNCTOR)|B(TC_EQ),
    [TC_SEQ]=B(TC_SEQ)|B(TC_LIST)|B(TC_SEMIGROUP)|B(TC_FUNCTOR)|B(TC_EQ)|B(TC_SIZED),
    /* EQ is the most general stackable constraint. A value typed only as EQ
       cannot be narrowed to something stricter (ORD, NUM, SEQ, ...) without
       concrete evidence — that would silently tighten a function's declared
       signature beyond what was written. Keep this row minimal: EQ and the
       concrete stackable types that inherently satisfy Eq. */
    [TC_EQ]=B(TC_EQ)|B(TC_INT)|B(TC_FLOAT)|B(TC_SYM)|B(TC_LIST)|B(TC_TUPLE)|B(TC_REC)|B(TC_TAGGED),
    [TC_ORD]=B(TC_ORD)|B(TC_INT)|B(TC_FLOAT)|B(TC_EQ)|B(TC_NUM),
    [TC_SEMIGROUP]=B(TC_SEMIGROUP)|B(TC_LIST)|B(TC_TUPLE)|B(TC_REC)|B(TC_SEQ),
    [TC_FUNCTOR]=B(TC_FUNCTOR)|B(TC_LIST)|B(TC_TAGGED)|B(TC_SEQ)|B(TC_DICT),
    [TC_DICT]=B(TC_DICT)|B(TC_FUNCTOR)|B(TC_SIZED),
    [TC_SIZED]=B(TC_SIZED)|B(TC_LIST)|B(TC_TUPLE)|B(TC_REC)|B(TC_DICT)|B(TC_SEQ)|B(TC_SEMIGROUP),
};
#undef B
static int tc_constraint_matches(TypeConstraint a, TypeConstraint b) {
    if (a == TC_NONE || a == b) return 1;
    return (a <= TC_SIZED && b <= TC_SIZED) ? (tc_compat[a] >> b) & 1 : 0;
}
static int tc_should_narrow(TypeConstraint cur, TypeConstraint c) {
    if (tc_is_concrete(c) && !tc_is_concrete(cur)) return 1;
    if (cur == TC_NUM && (c == TC_INT || c == TC_FLOAT)) return 1;
    if (cur == TC_EQ && (c == TC_ORD || c == TC_NUM || c == TC_SEQ || c == TC_SEMIGROUP || c == TC_FUNCTOR)) return 1;
    if (cur == TC_SEMIGROUP && c == TC_SEQ) return 1;
    return 0;
}
static int tvar_bind(TypeChecker *tc, int id, TypeConstraint c) {
    int root = tvar_find(tc, id); TypeConstraint cur = tc->tvars[root].bound;
    if (cur == TC_NONE) { tc->tvars[root].bound = c; return 0; }
    if (tc_constraint_matches(cur, c)) { if (tc_should_narrow(cur, c)) tc->tvars[root].bound = c; return 0; }
    if (tc_constraint_matches(c, cur)) {
        /* c is stricter than cur. Narrow to the intersection (c) since we have
           evidence at the call site that the tvar is at least as tight as c.
           Without this, a body annotated `'a eq` using `sort` (wants ord) would
           leave the tvar at EQ and silently lie about the sig it presents to
           callers. Narrowing here forces the sig mismatch to surface later. */
        tc->tvars[root].bound = c; return 0;
    }
    return 1;
}
static int tvar_unify_1(TypeChecker *tc, int a, int b);
static int tc_tags_merge(TypeChecker *tc, int a, int b);
/* A value `then` takes must carry only 'ok and 'no. A caller's value passes the requirement on to its callers. */
static void tc_check_okno(TypeChecker *tc, int r) {
    r = tvar_find(tc, r); TVarEntry *e = &tc->tvars[r];
    if (e->open || e->param) return;
    int uid = e->union_id, line = LOC_LINE(current_loc);
    if (!uid) { tc_error(tc, line, 0, "'then' takes a value tagged 'ok or 'no, but the checker cannot see which tags this value carries. Case on it first, or declare the either its word leaves."); e->okno = 0; return; }
    for (int g = 0; g < tc->unions[uid-1].count; g++) { uint32_t tg = tc->unions[uid-1].syms[g];
        if (tg != S_OK && tg != S_NO) { tc_error(tc, line, 0, "'then' takes a value tagged 'ok or 'no, but this value may be tagged '%s. Case on it first.", sym_name(tg)); e->okno = 0; return; } }
}
/* A value the checker has not seen, and everything in it: no keys, no payloads, no body it can name. */
static void tc_mark_unknown(TypeChecker *tc, int r) {
    int seen[64], ns = 0, todo[64], nt = 0;
    if (r > 0) todo[nt++] = r;
    while (nt) {
        int t = tvar_find(tc, todo[--nt]), dup = 0;
        for (int k = 0; k < ns; k++) if (seen[k] == t) dup = 1;
        if (dup) continue;
        if (ns == 64) die("type checker: a value's type holds more than %d types while marking it unknown", ns);
        seen[ns++] = t; TVarEntry *e = &tc->tvars[t];
        e->unknown = 1; e->row = 0; e->rest = 0; e->open = 0; e->param = 0; e->vrow = 0; if (e->code > 0) e->code = -1;
        int c[3] = {e->elem, e->box_c, e->tag_p};
        for (int k = 0; k < 3; k++) if (c[k] > 0) { if (nt == 64) die("type checker: a value's type holds more than %d types while marking it unknown", nt); todo[nt++] = c[k]; }
    }
}
/* Unifying record types unifies their fields, which may hold records in turn. */
#define UNIFY_DEPTH_MAX 10000
static int tvar_unify(TypeChecker *tc, int a, int b) {
    static int depth;
    if (++depth > UNIFY_DEPTH_MAX) die("type checker: record types nest more than %d deep while unifying", UNIFY_DEPTH_MAX);
    int r = tvar_unify_1(tc, a, b); depth--; return r;
}
static int tvar_unify_1(TypeChecker *tc, int a, int b) {
    int ra = tvar_find(tc, a), rb = tvar_find(tc, b);
    if (ra == rb) return 0;
    TypeConstraint ca = tc->tvars[ra].bound, cb = tc->tvars[rb].bound;
    /* Values of conflicting types met: whichever it is, the checker knows nothing of it. The caller reports it. */
    if (ca != TC_NONE && cb != TC_NONE && !tc_constraint_matches(ca, cb) && !tc_constraint_matches(cb, ca)) {
        tc_mark_unknown(tc, ra); tc_mark_unknown(tc, rb); return 1; }
    /* A record type that extends the other one links under it, so the base stands for both. */
    if (tc->tvars[ra].rest && rec_base(tc, ra) == rb) { int t = ra; ra = rb; rb = t; TypeConstraint c = ca; ca = cb; cb = c; }
    int una = tc->tvars[ra].unknown, unb = tc->tvars[rb].unknown, va0 = tc->tvars[ra].vrow, vb0 = tc->tvars[rb].vrow;
    int okno = tc->tvars[ra].okno || tc->tvars[rb].okno;
    /* One type holds both bodies: it runs the one body both are, or one the checker cannot name. A value of
       unknown type, or an input, may be any body. */
    { int ka = tc->tvars[ra].code, kb = tc->tvars[rb].code;
      int wa = tc->tvars[ra].unknown || tc->tvars[ra].open || tc->tvars[ra].param, wb = tc->tvars[rb].unknown || tc->tvars[rb].open || tc->tvars[rb].param;
      tc->tvars[ra].code = ka == kb ? ka : !ka ? (wa ? -1 : kb) : !kb ? (wb ? -1 : ka) : -1;
      tc->tvars[ra].reads = tc->tvars[ra].reads ? tc->tvars[ra].reads : tc->tvars[rb].reads; }
    row_join(tc, ra, rb, ca, cb);
    /* A tagged value of either type: each tag carries a payload of both types. */
    { int va = tc->tvars[ra].vrow, vb = tc->tvars[rb].vrow;
      int ua = tc->tvars[ra].unknown, ub = tc->tvars[rb].unknown;
      tc->tvars[ra].vrow = va && vb ? row_union(tc, va, vb) : va ? (cb == TC_TAGGED || ub ? 0 : va) : (ca == TC_TAGGED || ua ? 0 : vb); }
    tc->tvars[rb].parent = ra;
#define PROP(f) if (!tc->tvars[ra].f && tc->tvars[rb].f) tc->tvars[ra].f = tc->tvars[rb].f
    /* One type holds both values' contents: elements, box contents and payloads are one type too,
       and the tags either can carry. A content that cannot unify keeps the first. */
    int ce[3] = {tc->tvars[ra].elem, tc->tvars[ra].box_c, tc->tvars[ra].tag_p}, cf[3] = {tc->tvars[rb].elem, tc->tvars[rb].box_c, tc->tvars[rb].tag_p};
    int ua = tc->tvars[ra].union_id, ub = tc->tvars[rb].union_id;
    PROP(elem); PROP(box_c); PROP(tag_p); PROP(union_id);
#undef PROP
    for (int k = 0; k < 3; k++) if (ce[k] > 0 && cf[k] > 0) {
        /* Tags carry payloads of different types, each tag's own in vrow: a merge of the payloads there is no conflict. */
        if (k == 2 && (va0 || vb0)) continue;
        tvar_unify(tc, ce[k], cf[k]); }
    /* A value of unknown type holds contents of unknown type. */
    for (int k = 0; k < 3; k++) if ((una && ce[k] <= 0 && cf[k] > 0) || (unb && cf[k] <= 0 && ce[k] > 0)) tc_mark_unknown(tc, ce[k] > 0 ? ce[k] : cf[k]);
    if (ua && ub && ua != ub) tc->tvars[ra].union_id = tc_tags_merge(tc, ua, ub);
    if (ca == TC_NONE) tc->tvars[ra].bound = cb;
    else if (tc_constraint_matches(ca, cb) && tc_should_narrow(ca, cb)) tc->tvars[ra].bound = cb;
    tc->tvars[ra].okno = okno;
    if (okno) tc_check_okno(tc, ra);
    return 0;
}
static int tvar_content(TypeChecker *tc, int tvar_id, TypeConstraint c) {
    int root = tvar_find(tc, tvar_id);
    if (c == TC_LIST || c == TC_SEQ || c == TC_DICT) return tc->tvars[root].elem;
    if (c == TC_BOX) return tc->tvars[root].box_c;
    if (c == TC_TAGGED) return tc->tvars[root].tag_p;
    return 0;
}
/* Resolve the content type of the top-of-stack value, viewed as a container
   of the given kind (TC_BOX, TC_LIST, TC_TAGGED, etc). Returns TC_NONE if
   the stack top has no tvar or the content slot is unbound. */
static inline TypeConstraint tc_top_content(TypeChecker *tc, TypeConstraint kind) {
    if (tc->sp <= 0 || tc->data[tc->sp-1].tvar_id <= 0) return TC_NONE;
    int sub = tvar_content(tc, tc->data[tc->sp-1].tvar_id, kind);
    return sub > 0 ? tvar_resolve(tc, sub) : TC_NONE;
}
/* Copy a tuple's scheme into fresh tvars for one call site: the tvars its inputs
   and outputs reach, with links between them remapped. A link out of the scheme
   gets a fresh tvar with the same bound, so call sites never constrain each
   other. map[i] is the copy of tvar base+i, or 0 if nothing reaches it. */
static void tvar_instantiate(TypeChecker *tc, TupleEffect *eff, int *map, int *extra, int nextra) {
    int base = eff->scheme_base, count = eff->scheme_count, n = 0;
    int *list = malloc((size_t)(count ? count : 1)*sizeof(int));
    if (!list) die("out of memory instantiating %d type variables", count);
#define REACH(tv) do { int t_ = (tv), o_ = t_ - base; if (t_ > 0 && o_ >= 0 && o_ < count && !map[o_]) { map[o_] = -1; list[n++] = o_; } } while (0)
    for (int j = 0; j < eff->in_count; j++) REACH(eff->in_tvars[j]);
    for (int j = 0; j < eff->out_count; j++) REACH(eff->out_tvars[j]);
    for (int j = 0; j < nextra; j++) REACH(extra[j]);
    for (int k = 0; k < n; k++) {
        int r = tvar_find(tc, base + list[k]), w = tc->tvars[r].row;
        REACH(r); REACH(tc->tvars[r].elem); REACH(tc->tvars[r].box_c); REACH(tc->tvars[r].tag_p); REACH(tc->tvars[r].rest);
        if (w) for (int f = 0; f < tc->rows[w].n; f++) REACH(tc->rows[w].tv[f]);
        int nw = tc->tvars[r].need, vw = tc->tvars[r].vrow;
        if (nw) for (int f = 0; f < tc->rows[nw].n; f++) REACH(tc->rows[nw].tv[f]);
        if (vw) for (int f = 0; f < tc->rows[vw].n; f++) REACH(tc->rows[vw].tv[f]);
    }
#undef REACH
    for (int k = 0; k < n; k++) map[list[k]] = tvar_fresh(tc);
    for (int k = 0; k < n; k++) {
        int i = list[k], root = tvar_find(tc, base + i), ro = root - base;
        /* A copy linked to its root's copy below takes its type from it: a bound of its own would
           read as a record whose keys are unknown. */
        int own = ro == i || ro < 0 || ro >= count;
        tc->tvars[map[i]].bound = own ? tc->tvars[root].bound : TC_NONE;
        if (!own) tc->tvars[map[i]].unknown = 0;
#define REMAP(f) if (tc->tvars[root].f > 0) { int off = tc->tvars[root].f - base; \
    if (off >= 0 && off < count) tc->tvars[map[i]].f = map[off]; \
    else { int fresh = tvar_fresh(tc); tc->tvars[fresh].bound = tc->tvars[tvar_find(tc, tc->tvars[root].f)].bound; tc->tvars[map[i]].f = fresh; } }
        REMAP(elem) REMAP(box_c) REMAP(tag_p)
#undef REMAP
        tc->tvars[map[i]].union_id = tc->tvars[root].union_id;
        if (own) {
#define COPY_OF(t) ((t) - base >= 0 && (t) - base < count && map[(t) - base] > 0 ? map[(t) - base] : (t))
            int w = tc->tvars[root].row;
            if (w) { int c = row_new(tc, tc->rows[w].line);
                for (int f = 0; f < tc->rows[w].n; f++) row_put(tc, c, tc->rows[w].key[f], COPY_OF(tc->rows[w].tv[f]));
                tc->tvars[map[i]].row = c; }
            if (tc->tvars[root].rest) tc->tvars[map[i]].rest = COPY_OF(tc->tvars[root].rest);
            int nw = tc->tvars[root].need;
            if (nw) { int c = row_new(tc, tc->rows[nw].line);
                for (int f = 0; f < tc->rows[nw].n; f++) row_put(tc, c, tc->rows[nw].key[f], COPY_OF(tc->rows[nw].tv[f]));
                tc->tvars[map[i]].need = c; }
            int vw = tc->tvars[root].vrow;
            if (vw) { int c = row_new(tc, tc->rows[vw].line);
                for (int f = 0; f < tc->rows[vw].n; f++) row_put(tc, c, tc->rows[vw].key[f], COPY_OF(tc->rows[vw].tv[f]));
                tc->tvars[map[i]].vrow = c; }
            tc->tvars[map[i]].open = tc->tvars[root].open; tc->tvars[map[i]].param = tc->tvars[root].open;
            tc->tvars[map[i]].unknown = tc->tvars[root].unknown; tc->tvars[map[i]].input = tc->tvars[root].input; tc->tvars[map[i]].okno = tc->tvars[root].okno;
            tc->tvars[map[i]].code = tc->tvars[root].code; tc->tvars[map[i]].reads = tc->tvars[root].reads;
#undef COPY_OF
        }
    }
    for (int k = 0; k < n; k++) { int i = list[k], off = tvar_find(tc, base + i) - base; if (off >= 0 && off < count && off != i) tvar_unify(tc, map[off], map[i]); }
    free(list);
}
static int tc_find_brace_before(Token *toks, int pos) {
    for (int j = pos - 1; j >= 0; j--) {
        TokTag t = toks[j].tag;
        if (t == TOK_RBRACE || t == TOK_RPAREN || t == TOK_RBRACKET) {
            if (t == TOK_RBRACE) return j + toks[j].span;
            j += toks[j].span; continue;
        }
        if (t==TOK_SYM||t==TOK_INT||t==TOK_FLOAT||t==TOK_STRING) continue;
        break;
    }
    return -1;
}
static void tc_push(TypeChecker *tc, TypeConstraint type, int line) {
    if (tc->sp >= ASTACK_MAX) die("type checker stack overflow: more than %d values on the stack", ASTACK_MAX);
    AbstractType *at = &tc->data[tc->sp++]; memset(at, 0, sizeof(*at));
    at->type = type; at->flags = (type == TC_BOX) ? AT_LINEAR : 0; at->source_line = line; at->effect_idx = -1;
    if (tc_is_container(type) || type == TC_SEQ) {
        at->tvar_id = tvar_fresh(tc); tc->tvars[at->tvar_id].bound = type; int sub = tvar_fresh(tc);
        if (type == TC_LIST || type == TC_SEQ || type == TC_DICT) tc->tvars[at->tvar_id].elem = sub;
        else if (type == TC_BOX) tc->tvars[at->tvar_id].box_c = sub;
        else tc->tvars[at->tvar_id].tag_p = sub;
    }
}
/* Record an operation taking n values: any beyond the body's floor are an underflow.
   After code of unknown effect runs, that code may have left them, so they become
   untyped values at the place where it ran. */
static void tc_take(TypeChecker *tc, int n, int line) {
    int avail = tc->sp - tc->sp_floor, m = n - avail;
    if (m <= 0) return;
    /* At the top of the program no caller supplies the missing values. */
    if (tc->opaque_at < 0 && tc->body_depth == 0 && tc->sp_floor == 0) {
        tc_error(tc, line, 0, "this takes %d value(s) from the stack, but the stack has %d here. Push the missing value(s) first.", n, avail); return; }
    if (tc->opaque_at < 0) { tc->underflows += m; return; }
    if (tc->sp + m > ASTACK_MAX) die("type checker stack overflow: more than %d values on the stack", ASTACK_MAX);
    int at = tc->opaque_at < tc->sp_floor ? tc->sp_floor : tc->opaque_at > tc->sp ? tc->sp : tc->opaque_at, top = tc->sp;
    memmove(&tc->data[at + m], &tc->data[at], (size_t)(top - at) * sizeof(AbstractType));
    tc->sp = at; for (int k = 0; k < m; k++) tc_push(tc, TC_NONE, line);
    tc->sp = top + m;
}
/* The tags a value can carry: a union id, or 0 when they are unknown. */
static int tc_tags(TypeChecker *tc, AbstractType *at) {
    return at->type == TC_TAGGED && at->tvar_id > 0 ? tc->tvars[tvar_find(tc, at->tvar_id)].union_id : 0;
}
static void tc_set_tags(TypeChecker *tc, AbstractType *at, int uid) {
    if (uid && at->type == TC_TAGGED && at->tvar_id > 0) tc->tvars[tvar_find(tc, at->tvar_id)].union_id = uid;
}
/* The inferred union with exactly these tags, made once. */
static int tc_tags_of(TypeChecker *tc, const uint32_t *syms, int n) {
    for (int u = 0; u < tc->union_count; u++) {
        UnionDef *ud = &tc->unions[u]; int same = ud->inferred && ud->count == n;
        for (int k = 0; same && k < n; k++) { same = 0; for (int m = 0; m < n; m++) if (ud->syms[m] == syms[k]) same = 1; }
        if (same) return u + 1;
    }
    if (tc->union_count == UNION_MAX) die("type checker: more than %d distinct tag sets", UNION_MAX);
    UnionDef *ud = &tc->unions[tc->union_count++]; memset(ud, 0, sizeof(*ud));
    ud->count = n; ud->inferred = 1; memcpy(ud->syms, syms, (size_t)n * sizeof(uint32_t));
    return tc->union_count;
}
/* A value that is either a or b carries the tags of both; unknown if either is. */
static int tc_tags_merge(TypeChecker *tc, int a, int b) {
    if (!a || !b) return 0;
    if (a == b) return a;
    uint32_t syms[UNION_VARIANTS_MAX]; int n = 0;
    for (int u = 0; u < 2; u++) { UnionDef *ud = &tc->unions[(u ? b : a) - 1];
        for (int k = 0; k < ud->count; k++) {
            int seen = 0; for (int m = 0; m < n; m++) if (syms[m] == ud->syms[k]) seen = 1;
            if (seen) continue;
            if (n == UNION_VARIANTS_MAX) die("type checker: a value carries more than %d different tags", UNION_VARIANTS_MAX);
            syms[n++] = ud->syms[k];
        } }
    return tc_tags_of(tc, syms, n);
}
static int tc_alloc_effect(TypeChecker *tc) {
    if (tc->effect_count >= EFFECT_MAX) die("type checker: more than %d tuple bodies at once; EFFECT_MAX in slap.c sets the limit", EFFECT_MAX);
    int idx = tc->effect_count++; memset(&tc->effects[idx], 0, sizeof(TupleEffect));
    return idx;
}
static TCBinding *tc_lookup(TypeChecker *tc, uint32_t sym) {
    for (int i = tc->bind_count - 1; i >= 0; i--) if (tc->bindings[i].sym == sym) return &tc->bindings[i];
    return NULL;
}
static void tc_error(TypeChecker *tc, int line, int origin_line, const char *fmt, ...) {
    tc->errors++; if (tc->quiet) return;
    int fid = LOC_FID(current_loc); const char *f = src_files[fid];
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "\n-- TYPE ERROR %s:%d ", f, line);
    int hl=15+(int)strlen(f)+10; for(int i=hl;i<60;i++) fputc('-',stderr);
    fprintf(stderr, "\n\n    "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n\n"); va_end(ap);
    print_source_line(stderr, fid, line, 0);
    if (origin_line > 0 && origin_line != line) print_source_line(stderr, fid, origin_line, 0);
    fprintf(stderr, "\n");
}
#define EFF_CONSUME(vsp,consumed,need) do{if(vsp<(need)){consumed+=(need)-vsp;vsp=0;}else vsp-=(need);}while(0)
static TypeConstraint tc_infer_effect(Token *toks, int start, int end,
                            int *out_consumed, int *out_produced, TypeChecker *ctx,
                            const uint32_t *outer_binds, int outer_count) {
    c_stack_check("while checking nested bodies");
    int vsp = 0, consumed = 0; TypeConstraint tt = TC_NONE;
    /* local bind table: sym + inferred (consume, produce). For `let` and for
       defs whose body we can't peek into, counts are (0, 1) — treat the name
       as producing one stack slot when referenced. For let-bound tuples whose
       body we infer via `(body) 'name let`, we record real counts. */
    #define LOCAL_BINDS_MAX 1024
    uint32_t local_binds[LOCAL_BINDS_MAX]; int local_bc[LOCAL_BINDS_MAX]; int local_bp[LOCAL_BINDS_MAX]; int local_count = 0;
    if (outer_count > LOCAL_BINDS_MAX) die("type checker: more than %d names in scope", LOCAL_BINDS_MAX);
    for (int i = 0; i < outer_count; i++) {
        local_binds[local_count] = outer_binds[i]; local_bc[local_count] = 0; local_bp[local_count] = 1; local_count++;
    }
    for (int i = start; i < end; i++) {
        switch (toks[i].tag) {
        case TOK_INT: vsp++; tt = TC_INT; break;
        case TOK_FLOAT: vsp++; tt = TC_FLOAT; break;
        case TOK_SYM: vsp++; tt = TC_SYM; break;
        case TOK_STRING: vsp++; tt = TC_LIST; break;
        case TOK_LPAREN: i = (i+toks[i].span); vsp++; tt = TC_TUPLE; break;
        case TOK_LBRACKET: {
            int close = i + toks[i].span;
            /* `[sig] effect` leaves nothing, and takes the 'name before it. */
            if (close+1 < end && toks[close+1].tag == TOK_WORD && toks[close+1].as.sym == S_EFFECT) {
                if (i-1 >= start && toks[i-1].tag == TOK_SYM) vsp--;
                i = close+1; break;
            }
            i = close; vsp++; tt = TC_LIST; break;
        }
        case TOK_LBRACE: i = (i+toks[i].span); vsp++; tt = TC_REC; break;
        case TOK_WORD: {
            uint32_t sym = toks[i].as.sym;
            if (sym == S_LET) {
                /* `(body) 'name let`: the name is at i-1, the body ends at i-2. */
                int dc = 0, dp = 1;
                if (i-2 >= start && toks[i-2].tag == TOK_RPAREN) {
                    int bs = i-2 + toks[i-2].span, bc = 0, bp = 0;
                    tc_infer_effect(toks, bs+1, i-2, &bc, &bp, ctx, local_binds, local_count);
                    dc = bc; dp = bp;
                } else if (i-3 >= start && toks[i-2].tag == TOK_WORD && toks[i-2].as.sym == S_EFFECT && toks[i-3].tag == TOK_RBRACKET) {
                    TypeSig sig = parse_type_annotation(toks, i-3 + toks[i-3].span + 1, i-3);
                    dc = 0; dp = 0;
                    for (int j = 0; j < sig.slot_count; j++) { if (sig.slots[j].direction == DIR_IN) dc++; else dp++; }
                }
                if (i >= start + 1 && toks[i-1].tag == TOK_SYM) {
                    if (local_count == LOCAL_BINDS_MAX) die("type checker: more than %d names in scope", LOCAL_BINDS_MAX);
                    local_binds[local_count] = toks[i-1].as.sym;
                    local_bc[local_count] = dc; local_bp[local_count] = dp; local_count++;
                }
                EFF_CONSUME(vsp,consumed,2);
            }
            else {
                TypeSig *sig = typesig_find(sym);
                /* A bare at leaves the value itself, which may be of any type. */
                int bare_at = sym == S_AT && !(i+1 < end && toks[i+1].tag == TOK_WORD && toks[i+1].as.sym == S_MUST);
                if (sig) {
                    int ni = 0, no = 0; TypeConstraint lo = TC_NONE;
                    for (int j = 0; j < sig->slot_count; j++) { if (sig->slots[j].direction == DIR_IN) ni++; else { no++; lo = sig->slots[j].constraint; } }
                    EFF_CONSUME(vsp,consumed,ni); vsp += no; if (no > 0) tt = bare_at ? TC_NONE : lo; else if (ni > 0) tt = TC_NONE;
                } else {
                    HOEffect *ho = ho_ops_find(sym);
                    if (ho) { int need = ho->need, out = ho->out;
                        if (ctx && sym == S_IF && i >= start + 2 && i-1 >= start && toks[i-1].tag == TOK_RPAREN) {
                            int bp = i-1, ep = bp + toks[bp].span;
                            int bc = 0, bp2 = 0; tc_infer_effect(toks, ep+1, bp, &bc, &bp2, ctx, local_binds, local_count);
                            /* Either branch may run: the if takes what the hungrier one takes. */
                            if (ep-1 >= start && toks[ep-1].tag == TOK_RPAREN) {
                                int tp = ep-1, ts = tp + toks[tp].span, tc1 = 0, tp1 = 0;
                                tc_infer_effect(toks, ts+1, tp, &tc1, &tp1, ctx, local_binds, local_count);
                                /* A branch that calls the word being guessed has the guess's effect: the other branch's is the real one. */
                                int self_then = 0, self_else = 0;
                                for (int k = ts+1; k < tp && ctx->prescan_self; k++) if (toks[k].tag == TOK_WORD && toks[k].as.sym == ctx->prescan_self) self_then = 1;
                                for (int k = ep+1; k < bp && ctx->prescan_self; k++) if (toks[k].tag == TOK_WORD && toks[k].as.sym == ctx->prescan_self) self_else = 1;
                                int net = self_else && !self_then ? tp1 - tc1 : bp2 - bc;
                                if (tc1 > bc) bc = tc1;
                                bp2 = bc + net;
                            }
                            need = ho->need + bc; out = bp2;
                        } else if (sym == S_WHILE && i-1 >= start && toks[i-1].tag == TOK_RPAREN) {
                            /* A loop body that reaches below the values it passes along takes them too. */
                            int bp = i-1, ep = bp + toks[bp].span, bc = 0, bp2 = 0;
                            tc_infer_effect(toks, ep+1, bp, &bc, &bp2, ctx, local_binds, local_count);
                            if (ep-1 >= start && toks[ep-1].tag == TOK_RPAREN) {
                                int cp = ep-1, cs = cp + toks[cp].span, cc = 0, cp2 = 0;
                                tc_infer_effect(toks, cs+1, cp, &cc, &cp2, ctx, local_binds, local_count);
                                if (cc > bc) bc = cc;
                            }
                            need = ho->need + bc; out = ho->out + bc;
                        } else if ((ho->flags & HO_APPLY_EFFECT) && i-1 >= start && toks[i-1].tag == TOK_RPAREN) {
                            int bp = i-1, ep = bp + toks[bp].span;
                            int bc = 0, bp2 = 0; tc_infer_effect(toks, ep+1, bp, &bc, &bp2, ctx, local_binds, local_count); need = ho->need + bc; out = ho->out + bp2;
                        }
                        EFF_CONSUME(vsp,consumed,need); vsp += out; if (out > 0) tt = ho->out_type; else if (need > 0) tt = TC_NONE;
                        if (sym == S_EDIT && !(i+1 < end && toks[i+1].tag == TOK_WORD && toks[i+1].as.sym == S_MUST)) tt = TC_REC;
                    }
                }
                if (!sig && !ho_ops_find(sym)) {
                    int lidx = -1; for (int j = 0; j < local_count; j++) if (local_binds[j] == sym) { lidx = j; break; }
                    if (lidx >= 0) {
                        EFF_CONSUME(vsp,consumed,local_bc[lidx]); vsp += local_bp[lidx]; tt = TC_NONE;
                    } else if (ctx) { TCBinding *ub = tc_lookup(ctx, sym);
                        if (ub && ub->atype.type == TC_TUPLE && ub->atype.effect_idx >= 0) { TupleEffect *e = &ctx->effects[ub->atype.effect_idx];
                            int bc = 0, bp2 = 0;
                            /* `n (body) repeat` takes what the body reaches below the values it passes along. */
                            if (sym == S_REPEAT && i-1 >= start && toks[i-1].tag == TOK_RPAREN) tc_infer_effect(toks, i-1 + toks[i-1].span + 1, i-1, &bc, &bp2, ctx, local_binds, local_count);
                            EFF_CONSUME(vsp,consumed,e->consumed + bc); vsp += e->produced + bc; if (e->produced > 0) tt = e->out_type; }
                        else if (ub) { vsp++; tt = ub->atype.type; }
                    }
                }
            }
            break;
        }
        default: break;
        }
    }
    *out_consumed = consumed; *out_produced = vsp; return tt;
}
static void tc_apply_effect(TypeChecker *tc, int consumed, int produced, TypeConstraint out_type, int line) {
    tc_take(tc, consumed, line); int avail = tc->sp - tc->sp_floor; tc->sp -= (consumed <= avail) ? consumed : avail;
    for (int i = 0; i < produced; i++) tc_push(tc, (i == produced - 1) ? out_type : TC_NONE, line);
}
static int tc_is_copyable(AbstractType *t) { return !(t->flags & AT_LINEAR) && t->type != TC_BOX; }
static int tc_is_builtin(uint32_t sym, int prelude_sig_count) {
    for (int i = 0; i < prelude_sig_count; i++) if (type_sigs[i].sym == sym) return 1;
    return ho_ops_find(sym) != NULL;
}
static void tc_bind(TypeChecker *tc, uint32_t sym, AbstractType *atype, int line, int word) {
    int i = 0; while (i < tc->bind_count && tc->bindings[i].sym != sym) i++;
    if (i == TC_BINDS_MAX) die("type checker: more than %d names bound at once", TC_BINDS_MAX);
    if (i == tc->bind_count) tc->bind_count++;
    tc->bindings[i] = (TCBinding){sym, *atype, line, 0, word};
}
static int tc_value_tvar(TypeChecker *tc, AbstractType *at);
static void tc_push_tvar(TypeChecker *tc, int tv, int line);
/* Code that may take any value below top and leave another has run: the records, payloads and
   literal symbols there are no longer known. */
static void tc_forget_range(TypeChecker *tc, int lo, int top) {
    for (int k = lo > tc->sp_floor ? lo : tc->sp_floor; k < top && k < tc->sp; k++) {
        AbstractType *v = &tc->data[k]; v->sym_id = 0;
        if (tv_info(tc, v->tvar_id)) { int n = tvar_fresh(tc); tc->tvars[n].bound = tvar_resolve(tc, v->tvar_id); v->tvar_id = n; }
    }
}
static void tc_forget_below(TypeChecker *tc, int top) { tc_forget_range(tc, tc->sp_floor, top); }
static void tc_forget(TypeChecker *tc) { tc_forget_below(tc, tc->sp); }
/* Code of unknown effect ran at top: the values below it may be others now. */
static void tc_unknown_ran(TypeChecker *tc, int top) { tc->opaque_at = top; tc->unknown_code = 1; tc_forget_below(tc, top); }
/* Code of unknown effect runs at the top of the stack. */
static void tc_opaque(TypeChecker *tc) { tc_unknown_ran(tc, tc->sp); }
/* A key read from a value of type tv or from what it holds (elements, box contents, payload), or 0. */
static uint32_t tc_need_key(TypeChecker *tc, int tv) {
    if (tv <= 0) return 0;
    int r = tvar_find(tc, tv), sub[4] = {r, tc->tvars[r].elem, tc->tvars[r].box_c, tc->tvars[r].tag_p};
    for (int k = 0; k < 4; k++) { int w = sub[k] > 0 ? tc->tvars[tvar_find(tc, sub[k])].need : 0; if (w && tc->rows[w].n) return tc->rows[w].key[0]; }
    return 0;
}
static void tc_flow_input(TypeChecker *tc, int arg, int param, int check_only, const char *who, int j, int line);
/* A caller's value reaches an input whose elements, box contents or payload a body read keys from:
   the value's must have them too. */
static void tc_flow_contents(TypeChecker *tc, int param, int arg, const char *who, int j, int line, int check_only) {
    int p = tvar_find(tc, param), a = arg > 0 ? tvar_find(tc, arg) : 0;
    int pf[3] = {tc->tvars[p].elem, tc->tvars[p].box_c, tc->tvars[p].tag_p};
    int af[3] = {a ? tc->tvars[a].elem : 0, a ? tc->tvars[a].box_c : 0, a ? tc->tvars[a].tag_p : 0};
    for (int k = 0; k < 3; k++) {
        int w = pf[k] > 0 ? tc->tvars[tvar_find(tc, pf[k])].need : 0;
        if (!w || !tc->rows[w].n) continue;
        if (af[k] > 0) tc_flow_input(tc, af[k], pf[k], check_only, who, j, line);
        else tc_error(tc, line, 0, "'%s' reads '%s from what its input %d holds, but the checker cannot see which keys the value passed here holds", who, sym_name(tc->rows[w].key[0]), j+1);
    }
}
/* A body whose input needs keys reaches a place that runs it without showing which record it gets. */
static void tc_escape(TypeChecker *tc, uint32_t key, const char *who, int line) {
    tc_error(tc, line, 0, "'%s' runs a body that reads '%s from its input with at or edit, but the checker cannot see which record reaches that input there.\n    Read the key where the record is built, and pass the value to the body instead.", who, sym_name(key));
}
/* The record of type arg reaches input j of `who`, which reads the keys param stands for. */
static void tc_flow_input(TypeChecker *tc, int arg, int param, int check_only, const char *who, int j, int line) {
    uint32_t miss = rec_flow(tc, arg, param, check_only);
    if (!miss) return;
    if (miss == REC_TOO_DEEP) { tc_error(tc, line, 0, "'%s' passes its input's records back to itself, nested past %d records deep; the checker cannot follow a record that holds a record of its own kind. Read such fields with `at must`.", who, REC_DEPTH_MAX); return; }
    if (rec_flat(tc, arg)) tc_error(tc, line, 0, "'%s' reads '%s from its input %d, but the record passed here has no '%s", who, sym_name(miss), j+1, sym_name(miss));
    else tc_error(tc, line, 0, "'%s' reads '%s from its input %d, but the checker cannot see which keys the record passed here has", who, sym_name(miss), j+1);
}
static void tc_apply_scheme(TypeChecker *tc, TupleEffect *eff, int consumed, int produced,
                            TypeConstraint body_out, const char *name, int line, int fu) {
    int sc = eff->scheme_count, *map = calloc(sc ? sc : 1, sizeof(int));
    if (!map) die("out of memory instantiating %d type variables", sc);
    tc_take(tc, consumed, line);
    /* The recursive calls inside this body pass records built from its inputs: copy them with the
       scheme, so they refer to what this call passes, and hand them to the body being checked. */
    int extra[64], nextra = 0;
    for (int c = 0; c < tc->rcall_count && eff->body_id; c++) if (tc->rcalls[c].body == eff->body_id)
        for (int j = 0; j < tc->rcalls[c].n + tc->rcalls[c].nout; j++) { int a = j < tc->rcalls[c].n ? tc->rcalls[c].arg[j] : tc->rcalls[c].out[j - tc->rcalls[c].n];
            if (a <= 0) continue;
            if (nextra == 64) die("type checker: a body passes more than 64 values to or from calls of its own word");
            extra[nextra++] = a; }
    tvar_instantiate(tc, eff, map, extra, nextra);
    for (int c = 0; c < tc->rcall_count && eff->body_id; c++) if (tc->rcalls[c].body == eff->body_id) {
        for (int j = 0; j < tc->rcalls[c].n; j++) { int a = tc->rcalls[c].arg[j], o = a - eff->scheme_base;
            if (a > 0 && o >= 0 && o < sc && map[o] > 0) tc->rcalls[c].arg[j] = map[o]; }
        for (int j = 0; j < tc->rcalls[c].nout; j++) { int a = tc->rcalls[c].out[j], o = a - eff->scheme_base;
            if (a > 0 && o >= 0 && o < sc && map[o] > 0) tc->rcalls[c].out[j] = map[o]; }
        tc->rcalls[c].body = tc->cur_body;
    }
    /* Two inputs may share a type: check each argument against its keys before any links to it. */
    for (int j = 0; j < eff->in_count && j < tc->sp; j++) {
        int stv = eff->in_tvars[j] - eff->scheme_base, idx = tc->sp - eff->in_count + j;
        if (stv < 0 || stv >= sc || idx < tc->sp_floor) continue;
        int ftv = map[stv]; AbstractType *inp = &tc->data[idx];
        if (tc->tvars[tvar_find(tc, ftv)].need) tc_flow_input(tc, tc_value_tvar(tc, inp), ftv, 1, name, j, line);
        tc_flow_contents(tc, ftv, inp->tvar_id, name, j, line, 1);
    }
    for (int j = 0; j < eff->in_count && j < tc->sp; j++) {
        int stv = eff->in_tvars[j] - eff->scheme_base;
        if (stv >= 0 && stv < sc) { int ftv = map[stv], idx = tc->sp - eff->in_count + j;
            if (idx < tc->sp_floor) continue;
            AbstractType *inp = &tc->data[idx];
            if (inp->type == TC_TUPLE && inp->effect_idx >= 0 && tc->effects[inp->effect_idx].reads) tc_escape(tc, (&tc->effects[inp->effect_idx])->reads, name, line);
            if (tc->tvars[tvar_find(tc, ftv)].need) rec_flow(tc, tc_value_tvar(tc, inp), ftv, 0);
            { int p = tvar_find(tc, ftv), a = inp->tvar_id > 0 ? tvar_find(tc, inp->tvar_id) : 0;
              if (a) { int pf[3] = {tc->tvars[p].elem, tc->tvars[p].box_c, tc->tvars[p].tag_p}, af[3] = {tc->tvars[a].elem, tc->tvars[a].box_c, tc->tvars[a].tag_p};
                for (int k = 0; k < 3; k++) if (pf[k] > 0 && af[k] > 0 && tc->tvars[tvar_find(tc, pf[k])].need) rec_flow(tc, af[k], pf[k], 0); } }
            /* An argument of unknown shape makes the input's copy unknown too, not an open input. */
            if (tvar_unify(tc, ftv, tc_value_tvar(tc, inp)))
                tc_error(tc, line, inp->source_line, "'%s' input type mismatch: expected %s, got %s (value from line %d)", name, constraint_name(tvar_resolve(tc, ftv)), constraint_name(inp->type != TC_NONE ? inp->type : tvar_resolve(tc, inp->tvar_id)), inp->source_line);
            if (fu && inp->tvar_id > 0) {
#define UNIFY_SUB(f) { int fa=tc->tvars[tvar_find(tc,ftv)].f, fb=tc->tvars[tvar_find(tc,inp->tvar_id)].f; if(fa>0&&fb>0)tvar_unify(tc,fa,fb); }
                UNIFY_SUB(elem) UNIFY_SUB(box_c) UNIFY_SUB(tag_p)
#undef UNIFY_SUB
            }
        }
    }
    int avail = tc->sp - tc->sp_floor; tc->sp -= (consumed <= avail) ? consumed : avail;
    for (int j = 0; j < eff->out_count; j++) {
        int stv = eff->out_tvars[j] - eff->scheme_base, ftv = (stv >= 0 && stv < sc) ? map[stv] : 0;
        if (ftv > 0) { TypeConstraint r = tvar_resolve(tc, ftv);
            tc_push(tc, TC_NONE, line); if (r != TC_NONE) tc->data[tc->sp-1].type = r;
            tc->data[tc->sp-1].tvar_id = ftv; if (fu && r == TC_BOX) tc->data[tc->sp-1].flags |= AT_LINEAR;
        } else tc_push(tc, TC_NONE, line);
    }
    for (int j = eff->out_count; j < produced; j++) tc_push(tc, (j == produced - 1) ? body_out : TC_NONE, line);
    free(map);
}
/* What `who` passes to a body: a record as it is, so the body's reads are checked against its keys;
   anything else only by its type, so the trial narrows nothing the program did not ask for. */
static AbstractType tc_trial_arg(TypeChecker *tc, AbstractType v) {
    if (v.tvar_id > 0) { TVarEntry *e = &tc->tvars[tvar_find(tc, v.tvar_id)];
        if (rec_tracked(tc, v.tvar_id) || e->code || e->open || e->param || e->input) return v; }
    /* The value's type stays; what it holds is not followed into the trial. */
    if (v.type == TC_NONE && v.tvar_id > 0) v.type = tvar_resolve(tc, v.tvar_id);
    v.tvar_id = 0; return v;
}
/* Run body te on args as `who` would, and leave the stack as it was: every record there must have
   the keys te reads. The tvars of te's top outputs go to outs; returns how many (at most max). */
static int tc_trial(TypeChecker *tc, TupleEffect *te, AbstractType *args, int n, int *outs, int max, const char *who, int line) {
    /* A body that takes more than it is given reads values the trial cannot show it. */
    if (te->consumed > n && te->reads) tc_escape(tc, (te)->reads, who, line);
    int s0 = tc->sp, f0 = tc->sp_floor, u0 = tc->underflows, oa0 = tc->opaque_at, dv0 = tc->diverged, uc0 = tc->unknown_code;
    for (int j = 0; j < n; j++) { tc_push(tc, TC_NONE, line); tc->data[tc->sp-1] = tc_trial_arg(tc, args[j]); }
    tc->sp_floor = s0; tc->opaque_at = -1;
    if (te->scheme_count > 0) tc_apply_scheme(tc, te, te->consumed, te->produced, te->out_type, who, line, 1);
    else tc_apply_effect(tc, te->consumed, te->produced, te->out_type, line);
    if (te->out_effect >= 0 && tc->sp > s0 && tc->data[tc->sp-1].type == TC_TUPLE) tc->data[tc->sp-1].effect_idx = te->out_effect;
    int k = tc->sp - s0; if (k > max) k = max; if (k > 16) k = 16; if (k < 0) k = 0;
    for (int j = 0; j < k; j++) { outs[j] = tc_value_tvar(tc, &tc->data[tc->sp - k + j]); tc->trial_out[j] = tc->data[tc->sp - k + j]; }
    /* Code of unknown effect leaves values the checker only guessed. */
    if (te->unknown) for (int j = 0; j < k; j++) {
        /* Its outputs are guesses from here on: a body among them that reads keys goes where no trial runs it. */
        AbstractType *o = &tc->trial_out[j];
        uint32_t r = o->type == TC_TUPLE && o->effect_idx >= 0 ? tc->effects[o->effect_idx].reads : outs[j] > 0 ? tc->tvars[tvar_find(tc, outs[j])].reads : 0;
        if (r) tc_escape(tc, r, who, line);
        int u = tvar_fresh(tc); tc->tvars[u].bound = tvar_resolve(tc, outs[j]); tc->tvars[u].unknown = 1;
        outs[j] = u; tc->trial_out[j].tvar_id = u; tc->trial_out[j].sym_id = 0; tc->trial_out[j].effect_idx = -1;
    }
    tc->sp = s0; tc->sp_floor = f0; tc->underflows = u0; tc->opaque_at = oa0; tc->diverged = dv0; tc->unknown_code = uc0;
    return k;
}
/* A body the checker cannot run on what it takes: the values are out of its sight or too many. Its
   reads cannot be checked, and it may take or leave anything. */
static void tc_unseen(TypeChecker *tc, TupleEffect *te, const char *who, int line) {
    if (te->reads) tc_escape(tc, (te)->reads, who, line);
    tc_opaque(tc);
}
/* A loop runs cond (if any) and body on its state again and again. From the second run on, each
   value there has only what it and every run's output have: meet them, then check every run's reads
   on the met state. A loop that changes the stack's depth, or runs on values out of sight, is code
   of unknown effect. */
static void tc_loop_records(TypeChecker *tc, TupleEffect *cond, TupleEffect *body, const char *who, int line) {
    TupleEffect *runs[2] = {cond, body}; int avail = tc->sp - tc->sp_floor, outs[17];
    /* The stack's depth after the loop must not depend on how often it runs. The prelude's repeat runs
       a let-bound body the checker takes for a value. */
    int user = tc->prelude_sig_count != 0, known = !body->opaque && !body->diverges && (!cond || (!cond->opaque && !cond->diverges));
    int net = body->produced - body->consumed + (cond ? cond->produced - 1 - cond->consumed : 0);
    if (user && known && net && !cond)
        tc_error(tc, line, 0, "'%s' runs its body again and again, so the body must leave as many values as it takes. This one takes %d and leaves %d.", who, body->consumed, body->produced);
    if (user && known && net && cond)
        tc_error(tc, line, 0, "'%s' runs its body again and again, so one run of cond and body must leave as many values as they take. The cond takes %d and leaves %d, one of them the flag; the body takes %d and leaves %d.", who, cond->consumed, cond->produced, body->consumed, body->produced);
    if (body->consumed != body->produced || (cond && cond->produced != cond->consumed + 1)) { tc_unseen(tc, body, who, line); if (cond) tc_unseen(tc, cond, who, line); return; }
    for (int r = 0; r < 2; r++) if (runs[r] && (runs[r]->consumed > avail || runs[r]->consumed > 16)) { tc_unseen(tc, runs[r], who, line); return; }
    for (int r = 0; r < 2; r++) {
        if (!runs[r]) continue;
        int m = runs[r]->consumed, k = tc_trial(tc, runs[r], &tc->data[tc->sp - m], m, outs, 17, who, line);
        for (int j = 0; j < m && j < k; j++) {
            AbstractType *v = &tc->data[tc->sp - m + j]; AbstractType *o = &tc->trial_out[j];
            /* From the second run on, a value here is the run's: a new body or symbol makes it unknown. */
            if (v->sym_id != o->sym_id) v->sym_id = 0;
            if (v->effect_idx != o->effect_idx) { if (o->type == TC_TUPLE && o->effect_idx >= 0 && tc->effects[o->effect_idx].reads) tc_escape(tc, (&tc->effects[o->effect_idx])->reads, who, line); v->effect_idx = -1; }
            if (tv_info(tc, v->tvar_id) || tv_info(tc, outs[j])) tvar_unify(tc, tc_value_tvar(tc, v), outs[j]);
        }
    }
    for (int r = 0; r < 2; r++) if (runs[r]) { int m = runs[r]->consumed; tc_trial(tc, runs[r], &tc->data[tc->sp - m], m, outs, 17, who, line); }
    for (int r = 0; r < 2; r++) if (runs[r] && runs[r]->unknown) tc_unknown_ran(tc, tc->sp - body->consumed);
}
static void tc_apply_ho(TypeChecker *tc, HOEffect *ho, int line) {
    int eff_c = 0, eff_p = 0, bk = 0, boe = -1; TypeConstraint bo = TC_NONE; TupleEffect *bteff = NULL;
    TypeConstraint bouts[8] = {0}; int bc = 0;
    int barr_c[8] = {0}, barr_p[8] = {0}, barr_has[8] = {0};
    AbstractType saved = {0}; int had_saved = 0;
    tc_take(tc, ho->need, line);
    if ((tc->sp - tc->sp_floor) < ho->need) {
        tc_error(tc, line, 0, "'%s' needs %d input(s), stack has %d", ho->name, ho->need, tc->sp - tc->sp_floor);
        tc->sp = tc->sp_floor;
        for (int j = 0; j < ho->out; j++) tc_push(tc, ho->out_type, line);
        return;
    }
    if (ho->flags & (HO_BOX_BORROW|HO_BOX_MUTATE)) {
        int body_unseen = 0;
        if (tc->sp > tc->sp_floor && (tc->data[tc->sp-1].type == TC_TUPLE || tc->data[tc->sp-1].type == TC_NONE)) {
            AbstractType *top = &tc->data[tc->sp-1];
            if (top->effect_idx < 0 && top->tvar_id > 0 && tc->tvars[tvar_find(tc, top->tvar_id)].code > 0) { top->type = TC_TUPLE; top->effect_idx = tc->tvars[tvar_find(tc, top->tvar_id)].code - 1; }
            else if (top->effect_idx < 0 && top->tvar_id > 0 && tc->tvars[tvar_find(tc, top->tvar_id)].reads) tc_escape(tc, tc->tvars[tvar_find(tc, top->tvar_id)].reads, ho->name, line);
            if (top->effect_idx < 0) body_unseen = 1;
            if (tc->data[tc->sp-1].effect_idx >= 0) { TupleEffect *te = &tc->effects[tc->data[tc->sp-1].effect_idx]; if (te->opaque && (ho->flags & HO_BOX_BORROW)) tc->opaque_at = tc->sp - 1; eff_c = te->consumed; eff_p = te->produced; bo = te->out_type; bk = 1; bteff = te; }
            tc->sp--;
        }
        /* The body runs on the box's contents: a copy for lend, the value itself for mutate, which keeps what the body leaves. */
        int outs[16], k = 0;
        if (bteff && tc->sp > tc->sp_floor && (tc->data[tc->sp-1].type == TC_BOX || tc->data[tc->sp-1].type == TC_NONE) && tc->data[tc->sp-1].tvar_id > 0) {
            tvar_bind(tc, tc->data[tc->sp-1].tvar_id, TC_BOX);
            int bc = tvar_content(tc, tc->data[tc->sp-1].tvar_id, TC_BOX);
            AbstractType a = {0}; a.type = bc > 0 ? tvar_resolve(tc, bc) : TC_NONE; a.tvar_id = bc; a.effect_idx = -1; a.source_line = line;
            k = tc_trial(tc, bteff, &a, 1, outs, 16, ho->name, line);
            /* The body may take the values under the box's contents and leave others. */
            if (bteff->consumed > 1) tc_forget_below(tc, tc->sp - 1);
            if (bteff->unknown) tc_unknown_ran(tc, tc->sp - 1);
            /* mutate replaces the contents: the box holds what the body left, or a value the checker has not seen. */
            if (ho->flags & HO_BOX_MUTATE) { int nb = tvar_fresh(tc), c = k == 1 ? outs[0] : tvar_fresh(tc); tc->tvars[nb].unknown = 0;
                if (k != 1) tc->tvars[c].unknown = 1;
                tc->tvars[nb].bound = TC_BOX; tc->tvars[nb].box_c = c; tc->data[tc->sp-1].tvar_id = nb;
                /* A box binding names the one cell: later lookups see the new contents. */
                if (tc->data[tc->sp-1].sym_id) { TCBinding *bb = tc_lookup(tc, tc->data[tc->sp-1].sym_id); if (bb && bb->atype.type == TC_BOX) bb->atype.tvar_id = nb; } }
        } else if (bteff && bteff->reads) tc_escape(tc, (bteff)->reads, ho->name, line);
        if (tc->sp > 0 && tc->data[tc->sp-1].type != TC_BOX && tc->data[tc->sp-1].type != TC_NONE)
            tc_error(tc, line, 0, "'%s' expected box, got %s", ho->name, constraint_name(tc->data[tc->sp-1].type));
        if ((ho->flags & HO_BOX_BORROW) && tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
            TypeConstraint ct = tc_top_content(tc, TC_BOX);
            /* A snapshot shares the box's inner boxes and dicts by pointer, and a later
               mutate frees them, so binding one is refused. */
            if (bk && bteff && bteff->has_let && (ct == TC_BOX || ct == TC_DICT))
                tc_error(tc, line, 0, "'lend' body may not 'let'-bind the snapshot when the box contains a %s — the snapshot copies the pointer, not the contents, so a later 'mutate' would free it while the binding still refers to it. Read it out with `k peek` instead of binding it.", constraint_name(ct));
            int r = bk ? (1 - eff_c + eff_p) : 1; if (r < 0) r = 0;
            /* What the body left, when the trial above saw it; otherwise values the checker has not seen. */
            if (k == r) for (int j = 0; j < r; j++) tc_push_tvar(tc, outs[j], line);
            else for (int j = 0; j < r; j++) { tc_push(tc, (j==r-1&&bo!=TC_NONE)?bo:(j==0&&ct!=TC_NONE)?ct:TC_NONE, line);
                AbstractType *o = &tc->data[tc->sp-1]; o->tvar_id = 0; tc->tvars[tc_value_tvar(tc, o)].unknown = 1; }
        } else if ((ho->flags & HO_BOX_MUTATE) && tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
            TypeConstraint ct = tc_top_content(tc, TC_BOX);
            if (ct != TC_NONE && bo != TC_NONE && !tc_constraint_matches(ct, bo) && !tc_constraint_matches(bo, ct))
                tc_error(tc, line, 0, "'mutate' body produces %s but box contains %s", constraint_name(bo), constraint_name(ct));
        }
        if (body_unseen) tc_opaque(tc);
        return;
    }
    /* `case` with tagged scrutinee: tagged box payloads can't be discarded
       by clause dispatch, and a linear default would leak on any match. */
    if (ho->sym == S_CASE && tc->sp >= 3 && tc->data[tc->sp-3].type == TC_TAGGED) {
        if (tc->data[tc->sp-3].tvar_id > 0) {
            int tid = tc->data[tc->sp-3].tvar_id;
            int tp = tvar_content(tc, tid, TC_TAGGED);
            if (tp > 0) { TypeConstraint pt = tvar_resolve(tc, tp);
                if (pt == TC_BOX)
                    tc_error(tc, line, 0, "'case' branches cannot safely discard linear payload; use a typed handler that consumes the box"); }
        }
        if ((tc->data[tc->sp-2].flags & AT_LINEAR) || tc->data[tc->sp-2].type == TC_BOX)
            tc_error(tc, line, 0, "'case' default must not be a linear value (would leak on any matched branch)");
    }
    TupleEffect *clauses = ho->sym == S_CASE && tc->sp > 0 && tc->data[tc->sp-1].effect_idx >= 0
                           && tc->effects[tc->data[tc->sp-1].effect_idx].clauses ? &tc->effects[tc->data[tc->sp-1].effect_idx] : NULL;
    /* Clauses that did not come from a literal right before `case` run code the checker cannot see. */
    int unknown_clauses = ho->sym == S_CASE && !clauses;
    TypeConstraint lpt = TC_NONE; int tptv = 0, lptv = 0, if_tags = 0, if_returns = 0;
    int branch_linear = 0; /* any popped body tuple captures or outputs linear */
    TupleEffect *branch[2]; int nbranch = 0; /* if branches that read keys from their inputs */
    int unknown_branch = 0; /* an if branch that is a body of unknown effect, or a value that may be one */
    /* What the op took, top first; ops with flows run their bodies on the right records below. */
    AbstractType pv[4]; TupleEffect *pte[4] = {0}; int npv = 0;
    int flows = ho->sym == S_WHILE || ho->sym == S_EACH || ho->sym == S_FOLD || ho->sym == S_ON || ho->sym == S_SHOW;
    int body_captures_linear = 0; /* primary body tuple captures a linear outer binding */
    for (int n = ho->need; n > 0 && tc->sp > tc->sp_floor; n--) {
        AbstractType *top = &tc->data[tc->sp - 1];
        if ((ho->flags & HO_SAVES_UNDER) && n == 1) { saved = *top; had_saved = 1; tc->sp--; continue; }
        /* The top of these is the body they run. */
        if (n == ho->need && ho->sym != S_IF && ho->sym != S_CASE && top->type != TC_TUPLE && top->type != TC_NONE)
            tc_error(tc, line, top->source_line, "'%s' expected tuple, got %s (value from line %d): it runs a body. Write the body in parentheses: (...) %s.", ho->name, constraint_name(top->type), top->source_line, ho->name);
        /* A body that lost its effect on the way here still has it in its type. */
        if (top->effect_idx < 0 && top->tvar_id > 0 && (top->type == TC_TUPLE || top->type == TC_NONE)) {
            int r = tvar_find(tc, top->tvar_id);
            if (tc->tvars[r].code > 0) { top->type = TC_TUPLE; top->effect_idx = tc->tvars[r].code - 1; }
            else if (tc->tvars[r].code < 0 && tc->tvars[r].reads) tc_escape(tc, tc->tvars[r].reads, ho->name, line);
        }
        lpt = top->type; lptv = top->tvar_id;
        if (npv < 4) { pv[npv] = *top; pte[npv++] = top->type == TC_TUPLE && top->effect_idx >= 0 ? &tc->effects[top->effect_idx] : NULL; }
        if (top->type == TC_TAGGED && top->tvar_id > 0 && !tptv) tptv = tvar_content(tc, top->tvar_id, TC_TAGGED);
        /* A case default that cannot run leaves nothing to compare with the clauses. */
        int ib = n > 1 && !(ho->sym == S_CASE && n == 2 && clauses && !clauses->dflt_live);
        if (top->type == TC_TUPLE && top->effect_idx >= 0) {
            TupleEffect *te = &tc->effects[top->effect_idx];
            if (te->reads && !(ho->flags & HO_APPLY_EFFECT) && !flows) {
                if (ho->sym == S_IF && ib && nbranch < 2) branch[nbranch++] = te; else tc_escape(tc, (te)->reads, ho->name, line);
            }
            /* A branch that recurses has only a guessed effect and one that never returns has none,
               so a branch that returns with a known effect decides. */
            if (!bk || (ib && ho->sym == S_IF && (te->diverges ? 2 : te->opaque) < (bteff->diverges ? 2 : bteff->opaque))) {
                eff_c = te->consumed; eff_p = te->produced; bo = te->out_type; bk = 1; boe = te->out_effect; bteff = te;
                if (top->flags & AT_LINEAR) body_captures_linear = 1;
            }
            if (te->output_is_linear || (top->flags & AT_LINEAR)) branch_linear = 1;
            if (ib && ho->sym == S_IF && !te->diverges) if_tags = if_returns++ ? tc_tags_merge(tc, if_tags, te->out_tags) : te->out_tags;
            if (ib && bc < 8) { barr_c[bc]=te->consumed; barr_p[bc]=te->produced; barr_has[bc]=!te->opaque && !te->diverges && ho->sym == S_IF; bouts[bc++] = te->out_type; }
        } else {
            if (ib && ho->sym == S_IF) { if_tags = 0; if_returns++; if (top->type == TC_TUPLE || top->type == TC_NONE) unknown_branch = 1; }
            if (ib && ho->sym == S_IF && top->type != TC_NONE && top->type != TC_TUPLE)
                tc_error(tc, line, top->source_line, "'if' expected tuple, got %s (value from line %d): it takes two bodies, `cond (then) (else) if`. Write the branch in parentheses.", constraint_name(top->type), top->source_line);
            if (ib && top->type != TC_NONE && bc < 8) bouts[bc++] = top->type;
        }
        tc->sp--;
    }
    if (unknown_clauses) { tc_opaque(tc); bc = 0; }
    if (ho->sym == S_IF && npv >= 3) {
        TypeConstraint c = pv[2].type != TC_NONE ? pv[2].type : pv[2].tvar_id > 0 ? tvar_resolve(tc, pv[2].tvar_id) : TC_NONE;
        if (c != TC_NONE && !tc_constraint_matches(c, TC_INT) && !tc_constraint_matches(TC_INT, c))
            tc_error(tc, line, pv[2].source_line, "'if' condition must be int, got %s (value from line %d). The condition goes under the two branches: `x 0 gt (then) (else) if`", constraint_name(c), pv[2].source_line);
    }
    int each_out = 0, fold_acc = 0;
    /* A loop whose body or cond the checker cannot see runs code of unknown effect, again and again. */
    if (ho->sym == S_WHILE && !(pte[0] && pte[1])) { for (int r = 0; r < 2; r++) if (pte[r]) tc_unseen(tc, pte[r], "while", line); tc_opaque(tc); }
    else if ((ho->sym == S_EACH || ho->sym == S_FOLD) && !pte[0]) tc_opaque(tc);
    else if (ho->sym == S_WHILE && pte[0] && pte[1]) {
        /* state (cond) (body) while: both run on the state, which body keeps the same size. */
        tc_loop_records(tc, pte[1], pte[0], "while", line);
    } else if (ho->sym == S_EACH && pte[0]) {
        /* list (body) each: body runs on each element, and the output holds what it leaves. */
        if ((pv[1].type == TC_LIST || pv[1].type == TC_SEQ || pv[1].type == TC_NONE) && pv[1].tvar_id > 0 && tvar_content(tc, pv[1].tvar_id, TC_LIST) > 0) {
            int el = tvar_content(tc, pv[1].tvar_id, TC_LIST), nb = pte[0]->consumed - 1; if (nb < 0) nb = 0;
            if (nb > tc->sp - tc->sp_floor || nb > 15) tc_unseen(tc, pte[0], "each", line);
            else {
                AbstractType a[16]; for (int j = 0; j < nb; j++) a[j] = tc->data[tc->sp - nb + j];
                memset(&a[nb], 0, sizeof(AbstractType)); a[nb].type = tvar_resolve(tc, el); a[nb].tvar_id = el; a[nb].effect_idx = -1; a[nb].source_line = line;
                int outs[16], k = tc_trial(tc, pte[0], a, nb + 1, outs, 16, "each", line);
                if (pte[0]->unknown) tc_unknown_ran(tc, tc->sp - nb);
                if (k >= 1) { each_out = outs[k-1]; AbstractType *o = &tc->trial_out[k-1];
                    if (o->type == TC_TUPLE && o->effect_idx >= 0 && tc->effects[o->effect_idx].reads) tc_escape(tc, (&tc->effects[o->effect_idx])->reads, "each", line); }
            }
        } else if (pte[0]->reads) tc_escape(tc, (pte[0])->reads, "each", line);
    } else if (ho->sym == S_FOLD && pte[0]) {
        /* list init (body) fold: body runs on the accumulator and each element. */
        if ((pv[2].type == TC_LIST || pv[2].type == TC_SEQ || pv[2].type == TC_NONE) && pv[2].tvar_id > 0 && tvar_content(tc, pv[2].tvar_id, TC_LIST) > 0) {
            int el = tvar_content(tc, pv[2].tvar_id, TC_LIST), nb = pte[0]->consumed - 2; if (nb < 0) nb = 0;
            if (nb > tc->sp - tc->sp_floor || nb > 14) tc_unseen(tc, pte[0], "fold", line);
            else {
                AbstractType a[16]; for (int j = 0; j < nb; j++) a[j] = tc->data[tc->sp - nb + j];
                if (pv[1].type == TC_TUPLE) tc_value_tvar(tc, &pv[1]);
                a[nb] = pv[1]; memset(&a[nb+1], 0, sizeof(AbstractType));
                a[nb+1].type = tvar_resolve(tc, el); a[nb+1].tvar_id = el; a[nb+1].effect_idx = -1; a[nb+1].source_line = line;
                int outs[16], k = tc_trial(tc, pte[0], a, nb + 2, outs, 16, "fold", line);
                if (pte[0]->unknown) tc_unknown_ran(tc, tc->sp - nb);
                /* The accumulator is the initial value or what the body leaves. */
                if (k >= 1) {
                    fold_acc = tc_value_tvar(tc, &a[nb]); tvar_unify(tc, fold_acc, outs[k-1]);
                    if (tv_info(tc, fold_acc)) tc_trial(tc, pte[0], a, nb + 2, outs, 16, "fold", line);
                }
                /* The body replaces the values below the accumulator it reaches. */
                if (nb) tc_forget(tc);
            }
        } else { if (pte[0]->reads) tc_escape(tc, (pte[0])->reads, "fold", line); if (pte[0]->consumed > 2) tc_forget(tc); }
    } else if (ho->sym == S_ON && pte[0]) {
        if (tc->handler_count == 16) die("type checker: more than 16 'on' handlers");
        uint32_t ev = pv[1].sym_id; const char *en = ev ? sym_name(ev) : "";
        tc->handlers[tc->handler_count].effect = (int)(pte[0] - tc->effects);
        tc->handlers[tc->handler_count++].nevent = strncmp(en, "mouse", 5) == 0 ? 2 : 1;
    } else if (ho->sym == S_SHOW && pte[0]) {
        /* Every handler and the render body run on the state below `show`. */
        for (int h = 0; h < tc->handler_count; h++) {
            TupleEffect *he = &tc->effects[tc->handlers[h].effect]; int ne = tc->handlers[h].nevent, n = he->consumed - ne;
            if (n < 0 || he->produced != n || n > tc->sp - tc->sp_floor || n + ne > 16) { tc_unseen(tc, he, "on", line); continue; }
            AbstractType a[16]; int outs[16];
            for (int j = 0; j < n; j++) a[j] = tc->data[tc->sp - n + j];
            for (int j = 0; j < ne; j++) { memset(&a[n+j], 0, sizeof(AbstractType)); a[n+j].type = TC_INT; a[n+j].effect_idx = -1; }
            if (tc_trial(tc, he, a, n + ne, outs, 16, "on", line) == n)
                for (int j = 0; j < n; j++) { AbstractType *v = &tc->data[tc->sp - n + j];
                    if (tv_info(tc, v->tvar_id) || tv_info(tc, outs[j])) tvar_unify(tc, tc_value_tvar(tc, v), outs[j]); }
        }
        for (int h = 0; h < tc->handler_count; h++) {
            TupleEffect *he = &tc->effects[tc->handlers[h].effect]; int ne = tc->handlers[h].nevent, n = he->consumed - ne, outs[16];
            if (n < 0 || he->produced != n || n > tc->sp - tc->sp_floor || n + ne > 16) continue;
            AbstractType a[16];
            for (int j = 0; j < n; j++) a[j] = tc->data[tc->sp - n + j];
            for (int j = 0; j < ne; j++) { memset(&a[n+j], 0, sizeof(AbstractType)); a[n+j].type = TC_INT; a[n+j].effect_idx = -1; }
            tc_trial(tc, he, a, n + ne, outs, 16, "on", line);
        }
        /* render runs on a copy of the top value, above the rest of the stack, which it may read too. */
        int m = pte[0]->consumed < 1 ? 1 : pte[0]->consumed;
        if (tc->sp > tc->sp_floor && m - 1 <= tc->sp - tc->sp_floor && m <= 16) {
            AbstractType a[16]; int outs[16];
            for (int j = 0; j < m - 1; j++) a[j] = tc->data[tc->sp - (m - 1) + j];
            a[m-1] = tc->data[tc->sp - 1];
            tc_trial(tc, pte[0], a, m, outs, 16, "show", line);
            if (m > 1) tc_forget(tc);
        }
        else tc_unseen(tc, pte[0], "show", line);
    } else if (ho->sym == S_SHOW) {
        /* A render body the checker cannot see: nor can it run the handlers against the state. */
        for (int h = 0; h < tc->handler_count; h++) tc_unseen(tc, &tc->effects[tc->handlers[h].effect], "on", line);
        tc_opaque(tc);
    }
    /* The rest of the ops leave a count that does not depend on the body. */
    int forget_after = 0; /* the body runs code of unknown effect: forget what lies below what it leaves */
    if ((ho->flags & HO_APPLY_EFFECT) || ho->sym == S_IF) {
        if (!bk || bteff->opaque) tc->opaque_at = tc->sp;
        forget_after = !bk || bteff->unknown;
        if (bk && bteff->diverges) tc->diverged = 1; }
    else if ((ho->flags & HO_BODY_1TO1) && bk && !bteff->opaque && (eff_c != 1 || eff_p != 1) && (eff_c + eff_p > 0))
        tc_error(tc, line, 0, "'%s' body must be 1->1, got %d->%d", ho->name, eff_c, eff_p);
    /* HO ops that aggregate body outputs into a container (each, fold)
       would alias a linear closure across iterations or package it into a list.
       Apply/dip execute the body directly and are fine. */
    if (bteff && bteff->output_is_linear && !(ho->flags & HO_APPLY_EFFECT))
        tc_error(tc, line, 0, "'%s' body may not produce a linear-capturing closure (would alias it across iterations or package it into a container)", ho->name);
    /* An iterated body (each, fold, while) that captures a linear
       outer binding would consume that binding N times. Apply-style ops
       (apply/dip/loop) run the body once; if/case dispatch one branch, so
       branch_linear is handled separately. */
    if (body_captures_linear && !(ho->flags & HO_APPLY_EFFECT) && !(ho->flags & HO_BRANCHES_AGREE)
        && !(ho->flags & HO_BOX_BORROW) && !(ho->flags & HO_BOX_MUTATE))
        tc_error(tc, line, 0, "'%s' body captures a linear value; running it more than once would double-consume that value", ho->name);
    /* Branch agreement splits into two checks:
       - Output *type* agreement: safe to enforce everywhere. A branch returning
         list vs int is always a bug, regardless of inferred pop counts.
       - Net stack-effect agreement: only enforced at top level, because inferring
         body effects from an empty stack undercounts pops from outer scope inside
         nested/recursive bodies, producing false positives on shape-preserving
         branches. */
    if ((ho->flags & HO_BRANCHES_AGREE) && bc >= 2) {
        int nested = tc->body_depth;
        /* Inside nested/recursive scopes, a TC_TUPLE output often means the
           effect of an empty body `()` couldn't be inferred (it's pushed as a
           value rather than analyzed as a branch body). Exclude those from the
           type comparison to avoid false positives on shape-preserving
           identity branches, but still catch concrete type mismatches. */
        TypeConstraint ref = TC_NONE;
        for (int i = 0; i < bc; i++)
            if (bouts[i] != TC_NONE && !(nested && bouts[i] == TC_TUPLE)) { ref = bouts[i]; break; }
        for (int i = 0; i < bc; i++) {
            if (bouts[i] == TC_NONE || (nested && bouts[i] == TC_TUPLE)) continue;
            if (ref != TC_NONE && bouts[i] != ref && !tc_constraint_matches(ref, bouts[i]) && !tc_constraint_matches(bouts[i], ref))
                tc_error(tc, line, 0, "'%s' branches produce different types: %s vs %s", ho->name, constraint_name(ref), constraint_name(bouts[i]));
        }
        {
            int rnet = 0, rset = 0, rci = 0;
            for (int i = 0; i < bc; i++) if (barr_has[i]) { rnet = barr_p[i] - barr_c[i]; rci = i; rset = 1; break; }
            if (rset) for (int i = 0; i < bc; i++)
                if (barr_has[i] && (barr_p[i] - barr_c[i]) != rnet)
                    tc_error(tc, line, 0, "'%s' branches have different stack effects: net %+d vs net %+d (%d->%d vs %d->%d)", ho->name, rnet, barr_p[i]-barr_c[i], barr_c[rci], barr_p[rci], barr_c[i], barr_p[i]);
        }
    }
    if (ho->flags & HO_APPLY_EFFECT) {
        if (bk) {
            if (bteff->scheme_count > 0) tc_apply_scheme(tc, bteff, eff_c, eff_p, bo, ho->name, line, 1);
            else tc_apply_effect(tc, eff_c, eff_p, bo, line);
            if (forget_after) tc_unknown_ran(tc, tc->sp - eff_p);
            if (boe >= 0 && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE) tc->data[tc->sp-1].effect_idx = boe;
            /* apply of a body whose output is a linear-capturing closure must
               propagate AT_LINEAR onto the result — otherwise subsequent let+apply
               would lose the single-use property. */
            if (bteff && bteff->output_is_linear && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE)
                tc->data[tc->sp-1].flags |= AT_LINEAR;
        }
        else tc_opaque(tc);
        if (had_saved) { tc_push(tc, TC_NONE, line); tc->data[tc->sp-1] = saved; }
        return;
    }
    if (ho->sym == S_IF && bk) {
        /* Either branch may run on the inputs: each must find the keys it reads there. */
        for (int b = 0; b < nbranch; b++) if (branch[b] != bteff)
            for (int j = 0; j < branch[b]->in_count; j++) { int idx = tc->sp - branch[b]->in_count + j;
                if (idx >= tc->sp_floor) tc_flow_input(tc, tc_value_tvar(tc, &tc->data[idx]), branch[b]->in_tvars[j], 1, "if", j, line); }
        /* The other branch runs on the same inputs, so a record the if leaves has only the keys both branches give it. */
        /* Over the values either branch takes, the other branch leaves what it leaves, or what it passes through. */
        int oi = pte[0] == bteff ? 1 : 0; TupleEffect *other = pte[oi]; int oo[32], no = 0, deep = 0, met = 0; AbstractType ov[32];
        if (other && other != bteff && !other->diverges) { met = 1;
            int avail = tc->sp - tc->sp_floor, m = other->consumed < avail ? other->consumed : avail;
            int w = eff_c > m ? eff_c : m; if (w > avail) { w = avail; deep = 1; } if (w > 16) { w = 16; deep = 1; } if (m > w) m = w;
            for (int k = 0; k < w - m; k++) { ov[no] = tc->data[tc->sp - w + k]; oo[no] = ov[no].tvar_id > 0 ? ov[no].tvar_id : 0; no++; }
            int k = tc_trial(tc, other, &tc->data[tc->sp - m], m, &oo[no], 16, "if", line);
            memcpy(&ov[no], tc->trial_out, sizeof(AbstractType) * (size_t)k); no += k;
        }
        if (bteff && bteff->scheme_count > 0) tc_apply_scheme(tc, bteff, eff_c, eff_p, bo, "if", line, 1);
        else tc_apply_effect(tc, eff_c, eff_p, bo, line);
        for (int t = 0; t < no && tc->sp - 1 - t >= tc->sp_floor; t++) {
            AbstractType *v = &tc->data[tc->sp - 1 - t], *w = &ov[no - 1 - t]; int o = oo[no - 1 - t];
            /* Which branch left a value is unknown: a body there runs code nobody checks, and a symbol there is not one literal. */
            if (w->type == TC_TUPLE && w->effect_idx >= 0 && tc->effects[w->effect_idx].reads) tc_escape(tc, (&tc->effects[w->effect_idx])->reads, "if", line);
            if (v->sym_id != w->sym_id) v->sym_id = 0;
            if (tv_info(tc, v->tvar_id) || tv_info(tc, o)) tvar_unify(tc, tc_value_tvar(tc, v), o ? o : tc_value_tvar(tc, w));
        }
        /* Where the meet could not follow both branches (past 16 values, or below what this body sees),
           either may have left something else. */
        if (met && deep) tc_forget_below(tc, tc->sp - no);
        else if (met && no < eff_p) tc_forget_range(tc, tc->sp - eff_p, tc->sp - no);
        if (bteff && bteff->out_effect >= 0 && tc->effects[bteff->out_effect].reads) tc_escape(tc, (&tc->effects[bteff->out_effect])->reads, "if", line);
        if (forget_after || (other && other->unknown)) tc_unknown_ran(tc, tc->sp - eff_p);
        if (unknown_branch) tc_unknown_ran(tc, tc->sp);
        if (tc->sp > 0 && !tc_tags(tc, &tc->data[tc->sp-1])) tc_set_tags(tc, &tc->data[tc->sp-1], if_tags);
        /* If either branch body produces a linear-capturing closure, mark the
           result linear. Runtime dispatches one branch at a time, but static
           TC can't know which branch wins — both must be treated as tainted. */
        if (branch_linear && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE)
            tc->data[tc->sp-1].flags |= AT_LINEAR;
        return;
    }
    /* `case` has popped the scrutinee, the default and the clauses. A matching clause takes
       the rest of its inputs from below; an unmatched tag or predicate leaves the default. */
    if (clauses) {
        if (clauses->diverges) tc->diverged = 1;
        /* Each clause runs on its tag's payload, or a predicate's on the scrutinee, above the values
           below the scrutinee. The case leaves what any clause leaves, or the default. */
        int met[16], nmet = -1;
        for (int c = 0; c < clauses->nclause; c++) {
            TupleEffect *ce = &tc->effects[clauses->clause_eff[c]];
            AbstractType pay = pv[2], a[16]; int outs[16];
            if (clauses->clause_key[c]) {
                int st = pv[2].tvar_id, vw = st > 0 ? tc->tvars[tvar_find(tc, st)].vrow : 0, g = vw ? row_find(tc, vw, clauses->clause_key[c]) : 0;
                memset(&pay, 0, sizeof pay); pay.effect_idx = -1; pay.source_line = line;
                if (g) { pay.tvar_id = g; pay.type = tvar_resolve(tc, g); }
            } else if (clauses->clause_pred[c] >= 0) {
                /* A predicate runs on a copy of the scrutinee: one that takes more reaches the values below. */
                TupleEffect *pe = &tc->effects[clauses->clause_pred[c]];
                tc_trial(tc, pe, &pay, 1, outs, 16, "case", line);
                if (pe->consumed > 1) tc_forget(tc);
            }
            int nb = ce->consumed - 1; if (nb < 0) nb = 0;
            if (nb > tc->sp - tc->sp_floor || nb > 15) { tc_unseen(tc, ce, "case", line); continue; }
            for (int j = 0; j < nb; j++) a[j] = tc->data[tc->sp - nb + j];
            a[nb] = pay;
            int k = tc_trial(tc, ce, a, nb + 1, outs, 16, "case", line);
            for (int j = 0; j < k; j++) { AbstractType *o = &tc->trial_out[j];
                if (o->type == TC_TUPLE && o->effect_idx >= 0 && tc->effects[o->effect_idx].reads) tc_escape(tc, (&tc->effects[o->effect_idx])->reads, "case", line); }
            if (ce->diverges) continue;
            if (nmet < 0) { nmet = k; for (int t = 0; t < k; t++) met[t] = outs[k - 1 - t]; }
            else for (int t = 0; t < k && t < nmet; t++) if (tv_info(tc, met[t]) || tv_info(tc, outs[k - 1 - t])) tvar_unify(tc, met[t], outs[k - 1 - t]);
        }
        /* The default replaces the scrutinee and passes the values below it through. */
        if (clauses->dflt_live && nmet > 0) { AbstractType d = pv[1];
            if (tv_info(tc, d.tvar_id)) tvar_unify(tc, met[0], d.tvar_id); else if (tv_info(tc, met[0])) met[0] = 0;
            for (int t = 1; t < nmet; t++) { AbstractType *v = tc->sp - t >= tc->sp_floor ? &tc->data[tc->sp - t] : NULL;
                if (!v || clauses->produced != clauses->consumed) met[t] = 0;
                else if (tv_info(tc, v->tvar_id) || tv_info(tc, met[t])) tvar_unify(tc, met[t], tc_value_tvar(tc, v)); } }
        int s_before = tc->sp, same = 1;
        for (int c = 0; c < clauses->nclause; c++) { TupleEffect *ce = &tc->effects[clauses->clause_eff[c]];
            if (!ce->diverges && ce->consumed - ce->produced != clauses->consumed - clauses->produced) same = 0;
            if (!ce->diverges && (ce->consumed > 0 ? ce->consumed : 1) != clauses->consumed) same = 0; }
        tc_apply_effect(tc, clauses->consumed - 1, clauses->produced, clauses->out_type, line);
        /* Clauses that take different counts replace different values below: the checker cannot say which. */
        if (!same) { tc_opaque(tc); nmet = 0; }
        else if (clauses->unknown) tc_unknown_ran(tc, tc->sp - clauses->produced);
        for (int t = 0; t < nmet && t < clauses->produced && tc->sp - 1 - t >= 0 && tc->sp - 1 - t >= s_before - (clauses->consumed - 1); t++)
            if (tv_info(tc, met[t])) { AbstractType *v = &tc->data[tc->sp - 1 - t]; v->tvar_id = met[t]; v->type = tvar_resolve(tc, met[t]); }
        if (tc->sp > 0) tc_set_tags(tc, &tc->data[tc->sp-1], clauses->out_tags);
        return;
    }
    TypeConstraint out = ho->out_type;
    if (out == TC_NONE && (ho->flags & HO_BRANCHES_AGREE))
        for (int i = 0; i < bc; i++) if (bouts[i] != TC_NONE) { out = bouts[i]; break; }
    /* A functor op (each) outputs the input container type. */
    if (out == TC_FUNCTOR && lpt != TC_NONE && tc_is_concrete(lpt)) out = lpt;
    else if (out == TC_FUNCTOR && lptv > 0) {
        TypeConstraint r = tvar_resolve(tc, lptv); if (tc_is_concrete(r)) out = r;
    }
    for (int j = 0; j < ho->out; j++) tc_push(tc, (j == ho->out - 1) ? out : TC_NONE, line);
    if (ho->sym == S_IF && unknown_branch) tc_unknown_ran(tc, tc->sp);
    /* each leaves what its body leaves; without a trial the checker has not seen those values. */
    if (ho->sym == S_EACH) { int r = tvar_find(tc, tc_value_tvar(tc, &tc->data[tc->sp-1]));
        if (each_out > 0) { tc->tvars[r].elem = each_out; tc->tvars[r].unknown = 0; }
        else { if (!tc->tvars[r].elem) tc->tvars[r].elem = tvar_fresh(tc); tc_mark_unknown(tc, tc->tvars[r].elem); } }
    if (fold_acc) { tc->data[tc->sp-1].tvar_id = fold_acc; tc->data[tc->sp-1].type = tvar_resolve(tc, fold_acc); }
    /* Without a trial that followed the accumulator, fold leaves a value the checker has not seen. */
    else if (ho->sym == S_FOLD) tc_mark_unknown(tc, tc_value_tvar(tc, &tc->data[tc->sp-1]));
    if ((ho->flags & HO_BODY_1TO1) && out == TC_TAGGED && tc->sp > 0 && tc->data[tc->sp-1].tvar_id > 0) {
        int otp = tvar_content(tc, tc->data[tc->sp-1].tvar_id, TC_TAGGED);
        if (otp > 0) { if (bo != TC_NONE) tvar_bind(tc, otp, bo); else if (tptv > 0) tvar_unify(tc, otp, tptv); }
        /* The body changes the payload, so the tags survive but a declared union's payload types do not. */
        if (lptv > 0) { int uid = tc->tvars[tvar_find(tc, lptv)].union_id;
            if (uid > 0) tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].union_id = tc_tags_of(tc, tc->unions[uid-1].syms, tc->unions[uid-1].count); }
    }
}
/* The tvar of a value's type, made when the value has none. */
static int tc_value_tvar(TypeChecker *tc, AbstractType *at) {
    if (at->tvar_id > 0) {
        /* A body whose type says nothing of it yet: its type names this body now. */
        int r = tvar_find(tc, at->tvar_id); TVarEntry *e = &tc->tvars[r];
        if (at->type == TC_TUPLE && at->effect_idx >= 0 && !e->code && !e->unknown && !e->open && !e->param) {
            e->code = at->effect_idx + 1; e->reads = tc->effects[at->effect_idx].reads; if (e->bound == TC_NONE) e->bound = TC_TUPLE; }
        return at->tvar_id;
    }
    int t = at->tvar_id = tvar_fresh(tc); tc->tvars[t].bound = at->type;
    /* A value with no type variable carries nothing the checker knows of its keys or payloads. */
    tc->tvars[t].unknown = !(at->type == TC_INT || at->type == TC_FLOAT || at->type == TC_SYM || at->type == TC_NUM || at->type == TC_ORD);
    if (at->type == TC_TUPLE && at->effect_idx >= 0) { tc->tvars[t].code = at->effect_idx + 1; tc->tvars[t].reads = tc->effects[at->effect_idx].reads; tc->tvars[t].unknown = 0; }
    else if (at->type == TC_TUPLE) tc->tvars[t].code = -1;
    return t;
}
/* Push a value whose type is tvar tv. */
static void tc_push_tvar(TypeChecker *tc, int tv, int line) {
    tc_push(tc, TC_NONE, line); AbstractType *at = &tc->data[tc->sp-1];
    at->type = tvar_resolve(tc, tv); at->tvar_id = tv;
    if (at->type == TC_BOX) at->flags |= AT_LINEAR;
}
/* Push a record whose keys are row w (0: unknown). */
static void tc_push_rec(TypeChecker *tc, int w, int line) {
    tc_push(tc, TC_REC, line); int t = tc_value_tvar(tc, &tc->data[tc->sp-1]); tc->tvars[t].row = w; tc->tvars[t].unknown = !w;
}
/* The tvar of the value under key in record r, for `at` and `edit`, which never fail: the key must
   be written as a literal, and the record must be known to have it. 0 after reporting why not. */
static int tc_row_read(TypeChecker *tc, AbstractType *r, AbstractType *key, const char *who, int line) {
    TypeConstraint rt = r->type != TC_NONE ? r->type : r->tvar_id > 0 ? tvar_resolve(tc, r->tvar_id) : TC_NONE;
    if (rt != TC_NONE && rt != TC_REC) { tc_error(tc, line, r->source_line, "'%s' expected rec, got %s (value from line %d)", who, constraint_name(rt), r->source_line); return 0; }
    if (!key->sym_id) {
        tc_error(tc, line, key->source_line, "'%s' needs its key written as a literal, like 'name; this one is a value from line %d.\n    The checker proves the key is there, so it must see which key you read.\n    For keys that are data, use a dict: `d key of` gives `value ok` or `key no`.", who, key->source_line);
        return 0;
    }
    const char *k = sym_name(key->sym_id);
    int ftv = r->tvar_id > 0 ? rec_has(tc, r->tvar_id, key->sym_id) : 0;
    int w = ftv || r->tvar_id <= 0 ? 0 : rec_flat(tc, r->tvar_id);
    if (!ftv && !w) {
        tc_error(tc, line, r->source_line, "'%s' reads '%s, but the checker cannot see which keys this record has (value from line %d).\n    It sees the keys of records built with {...}, rec and into, and follows them through let, stack words and the words that pass them on.", who, k, r->source_line);
        return 0;
    }
    if (!ftv) {
        char has[256] = ""; int hl = 0;
        for (int i = 0; i < tc->rows[w].n && hl < (int)sizeof has - 40; i++) hl += snprintf(has + hl, sizeof has - hl, " '%s", sym_name(tc->rows[w].key[i]));
        tc_error(tc, line, tc->rows[w].line, "'%s' reads '%s, but this record has no '%s.\n    It has:%s\n    Made on: line %d\n    `at` and `edit` never fail, so every path that builds the record must add the key.", who, k, k, tc->rows[w].n ? has : " no keys", tc->rows[w].line);
    }
    return ftv;
}
/* Push the record r with key k holding a value of type vtv. A record that stands for what a caller
   passes keeps standing for it: the result has k, and every key of r through rest. */
static void tc_push_rec_with(TypeChecker *tc, int rtv, uint32_t k, int vtv, int line) {
    int r = rtv > 0 ? tvar_find(tc, rtv) : 0, w = 0, rest = 0;
    if (r && rec_tracked(tc, r)) {
        if (tc->tvars[r].open || tc->tvars[r].rest) { if (k) { w = row_new(tc, line); row_put(tc, w, k, vtv); rest = rtv; } }
        else if (k) { w = row_copy(tc, tc->tvars[r].row, line); row_put(tc, w, k, vtv); }
        /* A key that is data could replace any value, so every value's type is unknown after it. */
        else { int o = tc->tvars[r].row; w = row_new(tc, line); for (int f = 0; f < tc->rows[o].n; f++) row_put(tc, w, tc->rows[o].key[f], tvar_fresh(tc)); }
    }
    tc_push_rec(tc, w, line);
    if (rest) tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].rest = rest;
}
/* The field `at must` or `edit must` reads: those die on a missing key, so they prove nothing and
   need nothing, but a key the checker knows still gives its value's type. */
static int tc_row_read_must(TypeChecker *tc, AbstractType *r, AbstractType *key, const char *who, int line) {
    TypeConstraint rt = r->type != TC_NONE ? r->type : r->tvar_id > 0 ? tvar_resolve(tc, r->tvar_id) : TC_NONE;
    if (rt != TC_NONE && rt != TC_REC) { tc_error(tc, line, r->source_line, "'%s' expected rec, got %s (value from line %d)", who, constraint_name(rt), r->source_line); return 0; }
    return key->sym_id && r->tvar_id > 0 ? rec_find(tc, r->tvar_id, key->sym_id) : 0;
}
/* A tvar for a list whose elements have type tvar el. */
static int tc_list_of(TypeChecker *tc, int el) { int t = tvar_fresh(tc); tc->tvars[t].bound = TC_LIST; tc->tvars[t].elem = el; tc->tvars[t].unknown = 0; return t; }
static int tc_tvar_of(TypeChecker *tc, TypeConstraint c) { int t = tvar_fresh(tc); tc->tvars[t].bound = c; tc->tvars[t].unknown = 0; return t; }
/* What prim_parse_http builds: {'status int 'headers [{'key str 'value str}...] 'body str}. */
static int tc_http_rec(TypeChecker *tc, int line) {
    int h = tc_tvar_of(tc, TC_REC), hw = row_new(tc, line), w = row_new(tc, line), r = tc_tvar_of(tc, TC_REC);
    row_put(tc, hw, sym_intern("key"), tc_list_of(tc, tc_tvar_of(tc, TC_INT)));
    row_put(tc, hw, sym_intern("value"), tc_list_of(tc, tc_tvar_of(tc, TC_INT)));
    tc->tvars[h].row = hw;
    row_put(tc, w, sym_intern("status"), tc_tvar_of(tc, TC_INT));
    row_put(tc, w, sym_intern("headers"), tc_list_of(tc, h));
    row_put(tc, w, sym_intern("body"), tc_list_of(tc, tc_tvar_of(tc, TC_INT)));
    tc->tvars[r].row = w;
    return r;
}
/* rec 'k at: the value under 'k. */
static void tc_at(TypeChecker *tc, int line, int must) {
    tc_take(tc, 2, line);
    if (tc->sp - tc->sp_floor < 2) { tc_error(tc, line, 0, "'at' needs 2 input(s), stack has %d", tc->sp - tc->sp_floor); tc->sp = tc->sp_floor; tc_push(tc, TC_NONE, line); return; }
    AbstractType key = tc->data[tc->sp-1], r = tc->data[tc->sp-2]; tc->sp -= 2;
    int ftv = must ? tc_row_read_must(tc, &r, &key, "at", line) : tc_row_read(tc, &r, &key, "at", line);
    if (ftv) tc_push_tvar(tc, ftv, line); else tc_push(tc, TC_NONE, line);
}
/* rec 'k (body) edit: the record with the body run on the value under 'k. */
static void tc_edit(TypeChecker *tc, int line, int must) {
    tc_take(tc, 3, line);
    if (tc->sp - tc->sp_floor < 3) { tc_error(tc, line, 0, "'edit' needs 3 input(s), stack has %d", tc->sp - tc->sp_floor); tc->sp = tc->sp_floor; tc_push_rec(tc, 0, line); return; }
    AbstractType body = tc->data[tc->sp-1], key = tc->data[tc->sp-2], r = tc->data[tc->sp-3]; tc->sp -= 3;
    int ftv = must ? tc_row_read_must(tc, &r, &key, "edit", line) : tc_row_read(tc, &r, &key, "edit", line);
    if (ftv) tc_push_tvar(tc, ftv, line); else tc_push(tc, TC_NONE, line);
    TupleEffect *te = body.type == TC_TUPLE && body.effect_idx >= 0 ? &tc->effects[body.effect_idx] : NULL;
    if (!te || te->opaque) { tc->sp--; tc_push(tc, TC_NONE, line); if (!te || te->unknown) tc_opaque(tc); }
    /* A 0->0 body changes the value in place, below what the checker sees it take. */
    else if (te->consumed == 0 && te->produced == 0) {}
    else if (te->consumed != 1 || te->produced != 1) { tc_error(tc, line, 0, "'edit' body must be 1->1, got %d->%d", te->consumed, te->produced); tc->sp--; tc_push(tc, TC_NONE, line); }
    else if (te->scheme_count > 0) tc_apply_scheme(tc, te, 1, 1, te->out_type, "edit", line, 1);
    else tc_apply_effect(tc, 1, 1, te->out_type, line);
    if (te && te->diverges) tc->diverged = 1;
    int vtv = tc_value_tvar(tc, &tc->data[tc->sp-1]); tc->sp--;
    /* A key that must dies without is in the result either way. */
    tc_push_rec_with(tc, ftv || must ? r.tvar_id : 0, key.sym_id, vtv, line);
}
static void tc_check_word(TypeChecker *tc, uint32_t sym, int line) {
    /* quote pushes a binding's raw value (no auto-exec). If that value is a
       linear-capturing closure, applying it twice double-consumes the captured
       linear. Mirror the consumed_line check done for ordinary lookup at the
       b->atype path below. */
    TCBinding *quoted = NULL;
    if (sym == S_QUOTE && tc->sp > 0 && tc->data[tc->sp-1].type == TC_SYM && tc->data[tc->sp-1].sym_id) {
        uint32_t target = tc->data[tc->sp-1].sym_id;
        TCBinding *qb = tc_lookup(tc, target); quoted = qb;
        /* Quoting a value that does not run pushes what looking it up pushes. */
        if (qb && qb->atype.type != TC_TUPLE && qb->atype.type != TC_BOX && !(qb->atype.flags & (AT_LINEAR | AT_OPAQUE))) {
            tc->sp--; tc_check_word(tc, target, line); return; }
        if (qb && (qb->atype.flags & AT_LINEAR) && qb->atype.type == TC_TUPLE) {
            if (qb->consumed_line > 0)
                tc_error(tc, line, qb->consumed_line, "linear-capturing closure '%s' has already been consumed (previous use on line %d) — 'quote' on a linear closure consumes it just like applying it", sym_name(target), qb->consumed_line);
            else qb->consumed_line = line;
        }
    }
    TypeSig *sig = typesig_find(sym);
    /* A declared word whose body runs code of unknown effect may take or leave more than it declares. */
    TCBinding *db = tc_lookup(tc, sym);
    int early_unknown = 0; for (int k = 0; k < tc->unk_n; k++) if (tc->unk[k] == sym) early_unknown = 1;
    /* then, filter and pthen run their body on one value they give it: the body decides (apply_sig). */
    int gives_one = sym == S_THEN || sym == S_FILTER || sym == S_PTHEN;
    int declared_unknown = sig && !gives_one && (early_unknown || (db && db->atype.type == TC_TUPLE && db->atype.effect_idx >= 0
        && tc->effects[db->atype.effect_idx].unknown));
    /* Before its body is checked, a word is taken to leave what lies below its inputs alone;
       the body's check confirms it. The first pass finds the words it does not. */
    if (!early_unknown && ((sig && !db && !tc_is_builtin(sym, tc->prelude_sig_count)) || (db && db->atype.type == TC_TUPLE && db->atype.effect_idx >= 0 && tc->effects[db->atype.effect_idx].placeholder))) {
        if (tc->early_n + 2 > tc->early_cap) { tc->early_cap = tc->early_cap ? 2*tc->early_cap : 64;
            tc->early = realloc(tc->early, (size_t)tc->early_cap * sizeof(int)); if (!tc->early) die("type checker: out of memory for %d early calls", tc->early_n/2); }
        tc->early[tc->early_n++] = (int)sym; tc->early[tc->early_n++] = line;
    }
    if (sig) goto apply_sig;
    { HOEffect *ho = ho_ops_find(sym); if (ho) { tc_apply_ho(tc, ho, line); return; } }
    { TCBinding *b = tc_lookup(tc, sym);
      if (b && tc->literal_depth && b - tc->bindings >= tc->prelude_binds)
          tc_error(tc, line, 0, "'%s' is bound when the program runs, but a [...] or {...} literal is built when the program is read, before anything runs. Build the value at runtime instead, as in `list %s push` or `rec %s 'key into`.", sym_name(sym), sym_name(sym), sym_name(sym));
      if (b) {
        /* A linear-capturing closure (tuple whose body referenced linear outer bindings)
           must be applied at most once — each application would re-consume the captured
           linear resource. Bare Box bindings are mutable references: mutate/lend thread
           the box back onto the stack, so they can be looked up many times. Runtime
           still catches bare-box double-free/double-consume. */
        int is_linear_closure = (b->atype.flags & AT_LINEAR) && b->atype.type == TC_TUPLE;
        if (is_linear_closure && b->consumed_line > 0) {
            tc_error(tc, line, b->consumed_line, "linear-capturing closure '%s' has already been consumed (previous use on line %d) — a closure that captures a linear value can only be applied once", sym_name(sym), b->consumed_line);
            tc_push(tc, b->atype.type, line); tc->data[tc->sp-1].flags |= AT_CONSUMED; return;
        }
        if (is_linear_closure) b->consumed_line = line;
        if (b->word && b->atype.type == TC_TUPLE && b->atype.effect_idx >= 0) {
            TupleEffect *eff = &tc->effects[b->atype.effect_idx];
            int rc = -1;
            if (eff->placeholder) {
                if (tc->rcall_count == RCALL_MAX) die("type checker: more than %d recursive calls in one word", RCALL_MAX);
                int n = eff->consumed < 16 ? eff->consumed : 16, c = rc = tc->rcall_count++;
                tc->rcalls[c].effect = b->atype.effect_idx; tc->rcalls[c].n = n; tc->rcalls[c].nout = 0; tc->rcalls[c].line = line; tc->rcalls[c].body = tc->cur_body;
                for (int j = 0; j < n; j++) { int idx = tc->sp - n + j; tc->rcalls[c].arg[j] = idx >= tc->sp_floor ? tc_value_tvar(tc, &tc->data[idx]) : 0; }
            }
            if (eff->opaque) tc->opaque_at = tc->sp;
            if (eff->diverges) tc->diverged = 1;
            if (eff->scheme_count > 0) tc_apply_scheme(tc, eff, eff->consumed, eff->produced, eff->out_type, sym_name(sym), line, 1);
            else tc_apply_effect(tc, eff->consumed, eff->produced, eff->out_type, line);
            /* A recursive call leaves what the word leaves: its outputs join the word's once the body is checked. */
            if (rc >= 0) {
                for (int k = tc->sp - eff->produced; k < tc->sp && tc->rcalls[rc].nout < 16; k++) if (k >= 0) {
                    /* It stands for what the word leaves, known once the body is checked: like an input, a key read
                       from it is a need, which the word's real output must meet. */
                    if (tc->data[k].tvar_id <= 0) { int t = tvar_fresh(tc); tc->tvars[t].bound = tc->data[k].type; tc->data[k].tvar_id = t;
                        tc->tvars[t].unknown = 0; tc->tvars[t].open = 1; }
                    tc->rcalls[rc].out[tc->rcalls[rc].nout++] = tc->data[k].tvar_id; } }
            /* Code of unknown effect ran on its arguments: what lay below them may be gone. */
            if (eff->unknown || early_unknown) tc_unknown_ran(tc, tc->sp - eff->produced);
            /* Def returns a linear-capturing closure → mark output AT_LINEAR so
               the linear-closure single-use check catches second application. */
            if (eff->output_is_linear && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE)
                tc->data[tc->sp-1].flags |= AT_LINEAR;
            if (eff->out_effect >= 0 && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE)
                tc->data[tc->sp-1].effect_idx = eff->out_effect;
            return;
        } else {
            tc_push(tc, b->atype.type, line);
            /* The name stands for one record: a lookup has its keys, through rest, and nothing else of its type. */
            /* A list, box or tagged value keeps what is known of its elements' keys and its payloads. */
            if (tc_is_container(b->atype.type) && b->atype.tvar_id > 0 && tc->data[tc->sp-1].tvar_id > 0) {
                int bt = tvar_find(tc, b->atype.tvar_id), nt = tvar_find(tc, tc->data[tc->sp-1].tvar_id);
                int be = tvar_content(tc, bt, b->atype.type), ne = tvar_content(tc, nt, b->atype.type);
                int bb = be > 0 ? tvar_find(tc, be) : 0;
                if (bb && ne > 0 && (tc->tvars[bb].row || tc->tvars[bb].rest || tc->tvars[bb].need || (tc->tvars[bb].open && tc->tvars[bb].bound == TC_REC))) {
                    int r = tvar_find(tc, ne); tc->tvars[r].rest = be; tc->tvars[r].unknown = 0; if (tc->tvars[r].bound == TC_NONE) tc->tvars[r].bound = TC_REC; }
                if (tc->tvars[bt].vrow) tc->tvars[nt].vrow = tc->tvars[bt].vrow;
            }
            if ((b->atype.type == TC_REC || b->atype.type == TC_NONE) && b->atype.tvar_id > 0 && rec_tracked(tc, b->atype.tvar_id)) {
                int a = tvar_fresh(tc); tc->tvars[a].bound = tvar_resolve(tc, b->atype.tvar_id); tc->tvars[a].rest = b->atype.tvar_id; tc->tvars[a].unknown = 0;
                tc->data[tc->sp-1].tvar_id = a;
            }
            tc_set_tags(tc, &tc->data[tc->sp-1], tc_tags(tc, &b->atype));
            /* Box bindings: copy the content-type binding from the original
               tvar onto the fresh one so `lend`'s compound-aliasing guard can
               see what the box contains. Uses a fresh tvar for isolation. */
            if (b->atype.type == TC_BOX && b->atype.tvar_id > 0 && tc->data[tc->sp-1].tvar_id > 0) {
                int src_bc = tc->tvars[tvar_find(tc, b->atype.tvar_id)].box_c;
                if (src_bc > 0) {
                    TypeConstraint bct = tvar_resolve(tc, src_bc);
                    int dst_bc = tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].box_c;
                    if (bct != TC_NONE && dst_bc > 0) tvar_bind(tc, dst_bc, bct);
                }
            }
            if (b->atype.flags & AT_LINEAR) { tc->data[tc->sp-1].flags |= AT_LINEAR; tc->saw_linear_capture = 1; }
            /* Tag the pushed value with the binding it came from. Looking a Box
               binding up is free -- `lend` and `mutate` leave the box in place,
               so the same name is read many times in ordinary code -- but every
               lookup names the SAME heap cell, so only one of them may reach a
               consuming word. apply_sig checks this below. TC_BOX is a
               container, so the src_sym plumbing there won't copy this onto
               `clone`'s outputs, which are genuinely two different cells. */
            if (b->atype.type == TC_BOX) tc->data[tc->sp-1].sym_id = sym;
            return;
        }
      }
    }
    if (tc->unknown_count == TC_UNKNOWN_MAX) die("type checker: more than %d unknown words", TC_UNKNOWN_MAX);
    { tc->unknowns[tc->unknown_count].sym = sym; tc->unknowns[tc->unknown_count].line = line; tc->unknown_count++; }
    tc_opaque(tc);
    return;
apply_sig:;
    /* A word the program defines: its declared tags are checked against its body, not trusted. */
    int user_word = tc->prelude_sig_count && !tc_is_builtin(sym, tc->prelude_sig_count);
    if (sym == S_THEN && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE && tc->data[tc->sp-1].effect_idx >= 0) {
        TypeConstraint bo = tc->effects[tc->data[tc->sp-1].effect_idx].out_type;
        if (bo != TC_NONE && !tc_constraint_matches(bo, TC_TAGGED) && !tc_constraint_matches(TC_TAGGED, bo))
            tc_error(tc, line, 0, "then: the body must produce a tagged value (ok or no), got %s", constraint_name(bo));
    }
    int ni = 0;
    for (int i = 0; i < sig->slot_count; i++) if (sig->slots[i].direction == DIR_IN) ni++;
    tc_take(tc, ni, line);
    if (tc->sp < ni) {
        tc_error(tc, line, 0, "'%s' needs %d input(s), stack has %d", sym_name(sym), ni, tc->sp);
        if (tc->sp > 0 && !tc->quiet) { fprintf(stderr, "    stack (top first):"); for(int d=tc->sp-1;d>=0&&d>=tc->sp-5;d--) fprintf(stderr," %s",constraint_name(tc->data[d].type)); fprintf(stderr,"\n"); }
        return;
    }
    #define MAX_TVARS 16
    struct { uint32_t var; int tvar; uint32_t src_sym; int src_effect_idx, seen; } tm[MAX_TVARS]; int tmc = 0;
    for (int i = 0; i < sig->slot_count; i++) {
        uint32_t tv = sig->slots[i].type_var; if (!tv) continue;
        int found = 0; for (int j = 0; j < tmc; j++) if (tm[j].var == tv) { found = 1; break; }
        if (!found) {
            if (tmc == MAX_TVARS) die("type checker: a signature uses more than %d type variables", MAX_TVARS);
            int id = tvar_fresh(tc); TypeConstraint c = sig->slots[i].constraint; tc->tvars[id].unknown = 0;
            if (!tc_is_container(c) && c != TC_NONE) tc->tvars[id].bound = c;
            if (tc_is_container(c) && sig->slots[i].elem_constraint != TC_NONE) tc->tvars[id].bound = sig->slots[i].elem_constraint;
            tm[tmc].var = tv; tm[tmc].tvar = id; tm[tmc].src_sym = 0; tm[tmc].src_effect_idx = -1; tm[tmc].seen = 0; tmc++;
        }
    }
    #define FIND_TVAR(tv_name) ({ int _tv = 0; for (int _j = 0; _j < tmc; _j++) if (tm[_j].var == (tv_name)) { _tv = tm[_j].tvar; break; } _tv; })
    AbstractType passthrough[8] = {0}; int pt_count = 0;
    /* Track whether any input tuple carries AT_LINEAR, so that sig-declared
       tuple outputs (e.g. `compose`) can propagate linear-capture downstream.
       Without this, `(b free) (1 plus) compose apply apply` double-consumes. */
    int any_input_linear_tuple = 0;
    /* Same idea one level up: a tagged output built from a linear input is
       itself linear. `tag` is the only builtin shaped that way, and without
       this `42 box 'x tag` hands back a plain stackable that drop/dup/push/
       insert all accept -- laundering the box past every linear check. */
    int any_input_linear = 0;
    for (int i = 0; i < ni && i < tc->sp - tc->sp_floor; i++) {
        AbstractType *at = &tc->data[tc->sp-1-i];
        if (at->flags & AT_LINEAR) {
            any_input_linear = 1;
            if (at->type == TC_TUPLE) { any_input_linear_tuple = 1; break; }
        }
    }
    int sp2 = tc->sp - 1, runs_unknown = 0, then_tags = 0;
    for (int i = sig->slot_count - 1; i >= 0; i--) {
        TypeSlot *s = &sig->slots[i]; if (s->direction != DIR_IN || sp2 < tc->sp_floor) { if (s->direction == DIR_IN) sp2--; continue; }
        AbstractType *at = &tc->data[sp2];
        /* A body that moves through a type variable keeps its effect in its type; one a `tuple` slot takes goes
           where the checker cannot follow it. */
        if (!s->type_var && at->type == TC_TUPLE && at->effect_idx >= 0 && tc->effects[at->effect_idx].reads) tc_escape(tc, (&tc->effects[at->effect_idx])->reads, sym_name(sym), line);
        if (s->type_var && at->type == TC_TUPLE && at->tvar_id <= 0) tc_value_tvar(tc, at);
        /* then passes 'no on and runs its body on 'ok; any other tag would leave (). */
        if (sym == S_THEN && s->constraint == TC_TAGGED && at->tvar_id > 0 && tc->tvars[tvar_find(tc, at->tvar_id)].open) tc->tvars[tvar_find(tc, at->tvar_id)].okno = 1;
        else if (sym == S_THEN && s->constraint == TC_TAGGED) {
            int uid = at->tvar_id > 0 ? tc->tvars[tvar_find(tc, at->tvar_id)].union_id : 0;
            if (!uid) tc_error(tc, line, at->source_line, "'then' takes a value tagged 'ok or 'no, but the checker cannot see which tags this value carries (value from line %d). Case on it first, or declare the either its word leaves.", at->source_line);
            else for (int g = 0; g < tc->unions[uid-1].count; g++) { uint32_t tg = tc->unions[uid-1].syms[g];
                if (tg != S_OK && tg != S_NO) tc_error(tc, line, at->source_line, "'then' takes a value tagged 'ok or 'no, but this value may be tagged '%s (value from line %d). Case on it first.", sym_name(tg), at->source_line); }
        }
        if (sym == S_THEN && s->constraint == TC_TUPLE) then_tags = at->effect_idx >= 0 ? tc->effects[at->effect_idx].out_tags : 0;
        /* A body the word runs on the one value it gives it: unknown code, or one that reaches below, may replace anything. */
        if (gives_one && s->constraint == TC_TUPLE) { int e = at->effect_idx;
            if (e < 0 && at->tvar_id > 0 && tc->tvars[tvar_find(tc, at->tvar_id)].code > 0) e = tc->tvars[tvar_find(tc, at->tvar_id)].code - 1;
            if (e < 0 || tc->effects[e].unknown || tc->effects[e].consumed > 1) runs_unknown = 1;
            /* pthen's body leaves a value and the tagged result, as its 'no path does. */
            else { int want = sym == S_PTHEN ? 2 : 1;
                if (!tc->effects[e].opaque && !tc->effects[e].diverges && (tc->effects[e].consumed != 1 || tc->effects[e].produced != want))
                    tc_error(tc, line, at->source_line, "'%s' runs its body on one value and takes %d back, but this body takes %d and leaves %d.", sym_name(sym), want, tc->effects[e].consumed, tc->effects[e].produced); } }
        if (s->ownership == OWN_AUTO && (at->flags & AT_LINEAR)) { if (pt_count == 8) die("type checker: more than 8 auto slots"); passthrough[pt_count++] = *at; at->flags |= AT_CONSUMED; }
        if (s->ownership == OWN_COPY && !tc_is_copyable(at))
            tc_error(tc, line, at->source_line, "'%s' requires copyable value, got linear type (value from line %d)", sym_name(sym), at->source_line);
        if (s->constraint != TC_NONE && at->type != TC_NONE && !tc_constraint_matches(s->constraint, at->type))
            tc_error(tc, line, at->source_line, "'%s' expected %s, got %s (value from line %d)", sym_name(sym), constraint_name(s->constraint), constraint_name(at->type), at->source_line);
        if (s->constraint != TC_NONE && at->type == TC_NONE && at->tvar_id > 0) tvar_bind(tc, at->tvar_id, s->constraint);
        if (s->type_var) { int tv = FIND_TVAR(s->type_var); if (tv > 0) {
            if (tc_is_container(s->constraint)) {
                /* A container whose contents the checker has not seen holds values of unknown type. */
                if (at->tvar_id <= 0) { tc_value_tvar(tc, at); tvar_bind(tc, at->tvar_id, s->constraint); }
                int ef = tvar_content(tc, at->tvar_id, s->constraint);
                if (ef <= 0) { int r = tvar_find(tc, at->tvar_id), base = rec_base(tc, r);
                    /* A lookup of an input links to it: its contents are the input's. */
                    if (base != r && (tc->tvars[base].open || tc->tvars[base].input)) { tvar_bind(tc, base, s->constraint); ef = tvar_content(tc, base, s->constraint); }
                    if (ef <= 0) { ef = tvar_fresh(tc); tc->tvars[ef].unknown = 1;
                        if (tc->tvars[base].open || tc->tvars[base].input) { tc->tvars[ef].unknown = 0; tc->tvars[ef].open = tc->tvars[base].open; tc->tvars[ef].input = tc->tvars[base].input;
                            if (base != r) { if (s->constraint == TC_BOX) tc->tvars[base].box_c = ef; else if (s->constraint == TC_TAGGED) tc->tvars[base].tag_p = ef; else tc->tvars[base].elem = ef; } } }
                    if (s->constraint == TC_BOX) tc->tvars[r].box_c = ef; else if (s->constraint == TC_TAGGED) tc->tvars[r].tag_p = ef; else tc->tvars[r].elem = ef; }
                if (ef > 0 && tvar_unify(tc, tv, ef))
                    tc_error(tc, line, at->source_line, "'%s' type variable '%s' mismatch: expected %s, got %s", sym_name(sym), sym_name(s->type_var), constraint_name(tvar_resolve(tc, tv)), constraint_name(tvar_resolve(tc, ef)));
            } else if (!tc_is_container(s->constraint)) {
                if (at->tvar_id <= 0 && (at->type == TC_NONE || at->type == TC_TUPLE)) tc_value_tvar(tc, at);
                int fail = at->tvar_id > 0 ? tvar_unify(tc, tv, at->tvar_id) : (at->type != TC_NONE ? tvar_bind(tc, tv, at->type) : 0);
                if (fail) tc_error(tc, line, at->source_line, "'%s' type variable '%s' mismatch: expected %s, got %s", sym_name(sym), sym_name(s->type_var), constraint_name(tvar_resolve(tc, tv)), constraint_name(at->type != TC_NONE ? at->type : tvar_resolve(tc, at->tvar_id)));
            }
            /* Values of one type variable: the output is the one body or symbol all of them are, or none known. */
            for (int j = 0; j < tmc; j++) if (tm[j].var == s->type_var) {
                if (!tm[j].seen) { tm[j].src_sym = at->sym_id; tm[j].src_effect_idx = at->effect_idx; tm[j].seen = 1; }
                else { if (tm[j].src_sym != at->sym_id) tm[j].src_sym = 0; if (tm[j].src_effect_idx != at->effect_idx) tm[j].src_effect_idx = -1; }
                break; }
        }}
        /* An `either` INPUT slot names each variant's payload type. Bind those
           vars from the incoming union, so a later slot reusing the same var
           (default's fallback) must agree with the payload. Without this the
           fallback silently wins and `default`'s output takes its type. */
        /* A word of the program handles only the tags its either input names. */
        if (s->either_count > 0 && user_word) {
            int uid = tc_tags(tc, at);
            if (!uid) tc_error(tc, line, at->source_line, "'%s' takes an either of the tags its type names, but the checker cannot see which tags this value carries (value from line %d). Tag it where it is passed, or case on it first.", sym_name(sym), at->source_line);
            else for (int v = 0; v < tc->unions[uid-1].count; v++) { uint32_t g = tc->unions[uid-1].syms[v]; int named = 0;
                for (int e = 0; e < s->either_count; e++) if (s->either_syms[e] == g) named = 1;
                if (!named) tc_error(tc, line, at->source_line, "'%s' takes an either without '%s, but this value may be tagged '%s (value from line %d). Add '%s to its type, or case on the value first.", sym_name(sym), sym_name(g), sym_name(g), at->source_line, sym_name(g)); }
        }
        if (s->either_count > 0 && at->tvar_id > 0) {
            int vw = tc->tvars[tvar_find(tc, at->tvar_id)].vrow, uid = tc->tvars[tvar_find(tc, at->tvar_id)].union_id;
            for (int e = 0; e < s->either_count; e++) {
                int tv = s->either_tvars[e] && s->either_types[e] == TC_NONE ? FIND_TVAR(s->either_tvars[e]) : 0, g = vw ? row_find(tc, vw, s->either_syms[e]) : 0;
                /* A tag the value may carry, with a payload the checker has not seen: the payload is of unknown type. */
                int may = !uid; for (int k = 0; uid && k < tc->unions[uid-1].count; k++) if (tc->unions[uid-1].syms[k] == s->either_syms[e]) may = 1;
                if (tv > 0 && !g && may) { g = tvar_fresh(tc); tc->tvars[g].unknown = 1; }
                if (tv > 0 && g) tvar_unify(tc, tv, g);
                /* The result may be the payload: no one symbol or body is known for the variable. */
                for (int j = 0; tv > 0 && j < tmc; j++) if (tm[j].tvar == tv) { tm[j].seen = 1; tm[j].src_sym = 0; tm[j].src_effect_idx = -1; }
            }
            if (uid > 0) { UnionDef *ud = &tc->unions[uid-1];
                for (int e = 0; e < s->either_count; e++) {
                    if (!s->either_tvars[e] || s->either_types[e] != TC_NONE) continue;
                    int tv = FIND_TVAR(s->either_tvars[e]); if (tv <= 0) continue;
                    for (int v = 0; v < ud->count; v++) {
                        if (ud->syms[v] != s->either_syms[e] || ud->types[v] == TC_NONE) continue;
                        if (tvar_bind(tc, tv, ud->types[v]))
                            tc_error(tc, line, at->source_line, "'%s' type variable '%s is %s, but the '%s variant carries %s — they must agree (value from line %d)",
                                     sym_name(sym), sym_name(s->either_tvars[e]), constraint_name(tvar_resolve(tc, tv)), sym_name(ud->syms[v]), constraint_name(ud->types[v]), at->source_line);
                        break;
                    }
                }
            }
        }
        if ((at->flags & AT_LINEAR) && s->ownership == OWN_OWN) {
            const char *wn = sym_name(sym);
            if (strcmp(wn,"push")==0 || strcmp(wn,"into")==0 || strcmp(wn,"set")==0 || strcmp(wn,"insert")==0)
                tc_error(tc, line, at->source_line, "'%s' cannot embed a linear value into a stackable container — linearity would be lost (value from line %d)", wn, at->source_line);
            /* Only the words that actually retire the cell mark the binding.
               `swap` and the tcp-* pair also declare `box own in`, but they hand
               the box straight back out -- treating those as consumption fires
               on correct code (euler/10's `lend ... swap free`). `clone` counts:
               it returns the original alongside the copy, so the name is live
               only until then. Identity rides along on sym_id, which the tvar
               plumbing above already carries through `swap`. */
            if (at->type == TC_BOX && at->sym_id &&
                (strcmp(wn,"free")==0 || strcmp(wn,"tcp-close")==0)) {
                TCBinding *bb = tc_lookup(tc, at->sym_id);
                if (bb && bb->atype.type == TC_BOX) {
                    if (bb->consumed_line > 0)
                        tc_error(tc, line, bb->consumed_line, "box '%s' was already consumed on line %d — a Box binding names one heap cell no matter how many times it is looked up, so consuming through it twice is a double free. 'lend' and 'mutate' hand the same box back, so freeing what they return consumes the binding too", sym_name(at->sym_id), bb->consumed_line);
                    else bb->consumed_line = line;
                }
            }
            at->flags |= AT_CONSUMED;
        }
        sp2--;
    }
    tc->sp -= ni; if (tc->sp < tc->sp_floor) tc->sp = tc->sp_floor;
    /* Its declared counts hold; what lies below its inputs may be other values now. */
    if (declared_unknown) { tc->unknown_code = 1; tc_forget(tc); }
    /* An output type variable no input binds stands for a value the checker has not seen. */
    for (int j = 0; j < tmc; j++) if (!tm[j].seen) tc->tvars[tvar_find(tc, tm[j].tvar)].unknown = 1;
    for (int i = pt_count - 1; i >= 0; i--) {
        tc_push(tc, passthrough[i].type, line);
        AbstractType *o = &tc->data[tc->sp-1];
        o->tvar_id = passthrough[i].tvar_id; o->flags |= AT_LINEAR;
    }
    for (int i = 0; i < sig->slot_count; i++) {
        TypeSlot *s = &sig->slots[i]; if (s->direction != DIR_OUT) continue;
        tc_push(tc, s->constraint, line); AbstractType *at = &tc->data[tc->sp - 1];
        /* A word that takes nothing and leaves a container makes an empty one: it holds nothing yet. */
        if (ni == 0 && !user_word && !s->type_var && at->tvar_id > 0) { int r = tvar_find(tc, at->tvar_id); tc->tvars[r].unknown = 0;
            int c[3] = {tc->tvars[r].elem, tc->tvars[r].box_c, tc->tvars[r].tag_p}; for (int k = 0; k < 3; k++) if (c[k] > 0) tc->tvars[tvar_find(tc, c[k])].unknown = 0; }
        /* Propagate AT_LINEAR onto a sig-declared tuple output when an input
           tuple was AT_LINEAR. Covers `compose`: merging two closures where
           one captures linear must yield a linear-capturing closure. */
        if (any_input_linear_tuple && s->constraint == TC_TUPLE) at->flags |= AT_LINEAR;
        if (any_input_linear && s->constraint == TC_TAGGED) at->flags |= AT_LINEAR;
        if (s->type_var) { int tv = FIND_TVAR(s->type_var); if (tv > 0) {
            if (tc_is_container(s->constraint) && at->tvar_id > 0) { int r = tvar_find(tc, at->tvar_id);
                if (s->constraint == TC_BOX) tc->tvars[r].box_c = tv; else if (s->constraint == TC_TAGGED) tc->tvars[r].tag_p = tv; else tc->tvars[r].elem = tv; }
            else if (!tc_is_container(s->constraint)) { TypeConstraint r = tvar_resolve(tc, tv); if (r != TC_NONE) at->type = r; at->tvar_id = tv; if (r == TC_BOX) at->flags |= AT_LINEAR; }
        }}
        if (tc_is_container(s->constraint) && s->elem_constraint != TC_NONE && at->tvar_id > 0) { int ef = tvar_content(tc, at->tvar_id, s->constraint); if (ef > 0) tvar_bind(tc, ef, s->elem_constraint); }
        if (s->type_var) for (int j = 0; j < tmc; j++) if (tm[j].var == s->type_var) { if (tm[j].src_sym && !tc_is_container(s->constraint)) at->sym_id = tm[j].src_sym; if (tm[j].src_effect_idx >= 0 && !tc_is_container(s->constraint)) at->effect_idx = tm[j].src_effect_idx; break; }
        /* quote pushes the binding's value as it is: a body keeps its effect, so applying it is checked. */
        if (quoted && quoted->atype.type == TC_TUPLE && quoted->atype.effect_idx >= 0) { at->type = TC_TUPLE; at->effect_idx = quoted->atype.effect_idx; at->tvar_id = 0; tc_value_tvar(tc, at); }
        else if (sym == S_QUOTE && !quoted && at->tvar_id > 0) tc->tvars[tvar_find(tc, at->tvar_id)].unknown = 1;
        /* A word of the program leaves the tags its body was seen to leave; unseen, the checker knows none. */
        int last_out = 1; for (int k = i+1; k < sig->slot_count; k++) if (sig->slots[k].direction == DIR_OUT) last_out = 0;
        int trust_tags = !user_word || (last_out && db && db->atype.type == TC_TUPLE && db->atype.effect_idx >= 0
            && !tc->effects[db->atype.effect_idx].placeholder && tc->effects[db->atype.effect_idx].out_tags);
        if (s->either_count > 0 && at->tvar_id > 0) {
            if (tc->union_count == UNION_MAX) die("type checker: more than %d either results", UNION_MAX);
            int uid = ++tc->union_count; UnionDef *ud = &tc->unions[uid-1]; ud->count = s->either_count;
            for (int e = 0; e < s->either_count; e++) {
                ud->syms[e] = s->either_syms[e];
                if (s->either_tvars[e] && s->either_types[e] == TC_NONE) {
                    int tv = FIND_TVAR(s->either_tvars[e]);
                    ud->types[e] = tv > 0 ? tvar_resolve(tc, tv) : TC_NONE;
                } else ud->types[e] = s->either_types[e];
            }
            if (trust_tags) tc->tvars[tvar_find(tc, at->tvar_id)].union_id = uid;
            /* Each variant's payload type, where the signature names it with a variable. */
            int vw = 0;
            for (int e = 0; e < s->either_count; e++) { int tv = s->either_tvars[e] && s->either_types[e] == TC_NONE ? FIND_TVAR(s->either_tvars[e]) : 0;
                /* A variant of a written type carries a payload of that type, of contents the checker has not seen. */
                if (!tv && s->either_types[e] != TC_NONE) { tv = tvar_fresh(tc); tc->tvars[tv].bound = s->either_types[e];
                    tc->tvars[tv].unknown = !(s->either_types[e] == TC_INT || s->either_types[e] == TC_FLOAT || s->either_types[e] == TC_SYM);
                    if (tc_is_container(s->either_types[e])) { int c = tvar_fresh(tc); if (s->either_elems[e] != TC_NONE) tc->tvars[c].bound = s->either_elems[e];
                        if (s->either_types[e] == TC_BOX) tc->tvars[tv].box_c = c; else if (s->either_types[e] == TC_TAGGED) tc->tvars[tv].tag_p = c; else tc->tvars[tv].elem = c; } }
                if (tv > 0) { if (!vw) vw = row_new(tc, line); row_put(tc, vw, s->either_syms[e], tv); } }
            if (vw) tc->tvars[tvar_find(tc, at->tvar_id)].vrow = vw;
        }
    }
    if (runs_unknown) tc_unknown_ran(tc, tc->sp);
    /* then leaves the body's tags, or the 'no it passes on. */
    if (sym == S_THEN && tc->sp > 0 && tc->data[tc->sp-1].tvar_id > 0) { uint32_t no = S_NO;
        tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].union_id = then_tags ? tc_tags_merge(tc, then_tags, tc_tags_of(tc, &no, 1)) : 0; }
    #undef MAX_TVARS
}
static int tc_word_produces_linear(uint32_t sym) {
    const char *n = sym_name(sym);
    return strcmp(n,"box")==0;
}
static TypeConstraint tc_check_list_elements(TypeChecker *tc, Token *toks, int start, int end, int line) {
    TypeConstraint et = TC_NONE; int ec = 0;
    for (int i = start; i < end; i++) { TypeConstraint tt = TC_NONE;
        switch (toks[i].tag) {
        case TOK_INT: tt=TC_INT; break; case TOK_FLOAT: tt=TC_FLOAT; break; case TOK_SYM: tt=TC_SYM; break; case TOK_STRING: tt=TC_LIST; break;
        case TOK_LPAREN: tt=TC_TUPLE;i=(i+toks[i].span);break;
        case TOK_LBRACKET: tt=TC_LIST;i=(i+toks[i].span);break;
        case TOK_LBRACE: tt=TC_REC;i=(i+toks[i].span);break;
        case TOK_WORD: return TC_NONE;
        default: continue; }
        if (tt == TC_NONE) continue;
        if (++ec == 1) et = tt;
        else if (tt != et) { tc_error(tc, line, 0, "list elements have inconsistent types: element 1 is %s, element %d is %s", constraint_name(et), ec, constraint_name(tt)); return TC_NONE; }
    }
    return et;
}
static void tc_process_range(TypeChecker *tc, Token *toks, int start, int end, int total_count);
/* Some concrete type fits what was declared but not what the body needs. */
static int tc_stricter(TypeConstraint declared, TypeConstraint need) {
    static const TypeConstraint concrete[] = {TC_INT, TC_FLOAT, TC_SYM, TC_LIST, TC_TUPLE, TC_REC, TC_BOX, TC_TAGGED, TC_DICT};
    for (int k = 0; k < (int)(sizeof concrete / sizeof concrete[0]); k++)
        if (tc_constraint_matches(declared, concrete[k]) && !tc_constraint_matches(need, concrete[k])) return 1;
    return 0;
}
/* The source text of token j, or of the group it opens, cut to fit buf. */
static const char *tok_src(Token *toks, int j, char *buf, int n) {
    Token *a = &toks[j], *b = a->span > 0 ? &toks[j + a->span] : a;
    const char *line = src_lines[a->fid][a->line-1];
    int start = a->col - 1, end = start, cut = 0;
    if (b != a) { if (b->line == a->line) end = b->col; else { end = (int)strlen(line); cut = 1; } }
    else if (line[end] == '"') { end++; while (line[end] && line[end] != '"') end += line[end] == '\\' && line[end+1] ? 2 : 1; if (line[end]) end++; }
    else while (line[end] && !isspace((unsigned char)line[end]) && !strchr("()[]{}", line[end])) end++;
    if (end - start > n - 4) { end = start + n - 4; cut = 1; }
    snprintf(buf, n, "%.*s%s", end - start, line + start, cut ? "..." : "");
    return buf;
}
/* Check the inside of a [...] or {...} literal at toks[i..close]. build_tuple evaluates it when the
   program is read: it sees only what is written inside it and the prelude. Its values stay on the
   abstract stack above the returned base; the caller reads them and resets sp. */
static int tc_literal(TypeChecker *tc, Token *toks, int i, int close, int total_count, const char *kind) {
    c_stack_check("while checking nested literals");
    for (int j = i+1; j < close; j++) {
        if (toks[j].span > 0) { j += toks[j].span; continue; }
        if (toks[j].tag == TOK_WORD && tc_word_produces_linear(toks[j].as.sym))
            tc_error(tc, toks[i].line, 0, "%s literal cannot contain linear values produced by '%s'", kind, sym_name(toks[j].as.sym));
    }
    int s0 = tc->sp, u0 = tc->underflows, oa0 = tc->opaque_at;
    tc->sp_floor = tc->sp; tc->opaque_at = -1; tc->literal_depth++;
    tc_process_range(tc, toks, i+1, close, total_count);
    tc->literal_depth--; tc->opaque_at = oa0;
    for (int k = s0; k < tc->sp; k++)
        if (tc->data[k].type == TC_TUPLE && tc->data[k].effect_idx >= 0 && tc->effects[tc->data[k].effect_idx].reads)
            tc_escape(tc, tc->effects[tc->data[k].effect_idx].reads, kind[0] == 'l' ? "a list literal" : "a record literal", toks[i].line);
    if (tc->underflows > u0) {
        char src[64];
        tc_error(tc, toks[i].line, 0, "%s literal %s takes %d value(s) from outside it, but it is built when the program is read, from only what is written inside it.",
                 kind, tok_src(toks, i, src, sizeof src), tc->underflows - u0);
    }
    tc->underflows = u0;
    return s0;
}
/* Check a (...) body at toks[i..close] in its own scope and push the tuple it makes. */
static void tc_tuple(TypeChecker *tc, Token *toks, int i, int close, int total_count) {
    c_stack_check("while checking nested bodies");
    Token *t = &toks[i];
    int eff_c = 0, eff_p = 0;
    TypeConstraint eff_out = tc_infer_effect(toks, i+1, close, &eff_c, &eff_p, tc, NULL, 0);
    /* Auto-visible: if this tuple is followed by `'name let`, or by `[sig] effect 'name let`,
       make name visible inside the body so recursive references typecheck. */
    int nm = close+1;
    if (nm < total_count && toks[nm].tag == TOK_LBRACKET) { nm += toks[nm].span + 1;
        nm = nm < total_count && toks[nm].tag == TOK_WORD && toks[nm].as.sym == S_EFFECT ? nm + 1 : total_count; }
    if (!tc->recur_pending && nm+1 < total_count
        && toks[nm].tag == TOK_SYM
        && toks[nm+1].tag == TOK_WORD && toks[nm+1].as.sym == S_LET) {
        uint32_t name_sym = toks[nm].as.sym;
        for (int j = i+1; j < close; j++) {
            if (toks[j].tag == TOK_WORD && toks[j].as.sym == name_sym) {
                tc->recur_pending = 1; tc->recur_sym = name_sym;
                /* A call to itself has the word's own effect: guess again, with the last guess for the call, until the guess holds. */
                if (tc->bind_count == TC_BINDS_MAX) die("type checker: more than %d names bound at once", TC_BINDS_MAX);
                int g = tc_alloc_effect(tc); AbstractType ga = {0}; ga.type = TC_TUPLE; ga.effect_idx = g;
                tc->bindings[tc->bind_count++] = (TCBinding){name_sym, ga, t->line, 0, 1};
                uint32_t self0 = tc->prescan_self; tc->prescan_self = name_sym;
                for (int round = 0; ; round++) {
                    tc->effects[g].consumed = eff_c; tc->effects[g].produced = eff_p; tc->effects[g].out_type = eff_out;
                    int c2 = 0, p2 = 0; TypeConstraint o2 = tc_infer_effect(toks, i+1, close, &c2, &p2, tc, NULL, 0);
                    if (c2 == eff_c && p2 == eff_p) break;
                    if (round == 8) { tc_error(tc, t->line, 0, "'%s' calls itself, and after %d guesses the checker still cannot settle what it takes and leaves (last guesses: %d->%d, then %d->%d). Declare its type: '%s [...] effect before the body.", sym_name(name_sym), round + 1, eff_c, eff_p, c2, p2, sym_name(name_sym)); break; }
                    eff_c = c2; eff_p = p2; eff_out = o2;
                }
                tc->bind_count--; tc->prescan_self = self0;
                break;
            }
        }
    }
    /* A declared signature, `(body) [sig] effect` or an earlier `'name [sig] effect`,
       is checked against the body itself: it runs on exactly the declared inputs,
       and must leave exactly the declared outputs. */
    TypeSig dsig; int has_sig = 0, n_in = 0, n_out = 0; const char *who = "body"; uint32_t def_sym = 0;
    TypeConstraint in_c[TYPE_SLOTS_MAX], in_elem[TYPE_SLOTS_MAX]; int in_tags[TYPE_SLOTS_MAX]; uint32_t in_tv[TYPE_SLOTS_MAX];
    if (close+2 < total_count && toks[close+1].tag == TOK_LBRACKET) {
        int be = close+1+toks[close+1].span;
        if (be+1 < total_count && toks[be+1].tag == TOK_WORD && toks[be+1].as.sym == S_EFFECT) {
            dsig = parse_type_annotation(toks, close+2, be); has_sig = 1;
            if (be+3 < total_count && toks[be+2].tag == TOK_SYM && toks[be+3].tag == TOK_WORD && toks[be+3].as.sym == S_LET) { def_sym = toks[be+2].as.sym; who = sym_name(def_sym); }
        }
    } else if (i >= tc->user_start && close+2 < total_count && toks[close+1].tag == TOK_SYM
               && toks[close+2].tag == TOK_WORD && toks[close+2].as.sym == S_LET) {
        TypeSig *f = typesig_find(toks[close+1].as.sym);
        if (f && !tc_is_builtin(toks[close+1].as.sym, tc->prelude_sig_count)) { dsig = *f; has_sig = 1; def_sym = toks[close+1].as.sym; who = sym_name(def_sym); }
    }
    if (has_sig) {
        for (int k = 0; k < dsig.slot_count; k++)
            if (dsig.slots[k].direction == DIR_IN) {
                in_tags[n_in] = dsig.slots[k].either_count ? tc_tags_of(tc, dsig.slots[k].either_syms, dsig.slots[k].either_count) : 0;
                in_c[n_in] = dsig.slots[k].constraint; in_tv[n_in] = dsig.slots[k].type_var; in_elem[n_in++] = dsig.slots[k].elem_constraint;
            }
            else n_out++;
        eff_c = n_in; eff_p = n_out;
    }
    int scheme_base = tc->tvar_count, ic = 0, oc = 0, out_eff = -1;
    int itv[16] = {0}, otv[16] = {0}, sc = 0;
    { struct { int sp, bind_count, recur_pending, type_sig_count, sp_floor, saw_linear_capture; } _s = {
            tc->sp, tc->bind_count, tc->recur_pending, type_sig_count, tc->sp_floor, tc->saw_linear_capture };
        /* Note: effect_count is intentionally NOT saved — effects allocated during
           body processing must persist so the outer TupleEffect's fields stay valid. */
        /* A scheme records at most 16 inputs and outputs; a wider body is checked without one. */
        int wide = !has_sig && (eff_c > 16 || eff_p > 16);
        tc->saw_linear_capture = 0;
        tc->sp_floor = tc->sp;
        ic = wide ? 0 : eff_c;
        /* One tvar per signature variable. On a container slot the variable names the element, as in apply_sig. */
        uint32_t var_name[TYPE_SLOTS_MAX]; int var_tv[TYPE_SLOTS_MAX], nvar = 0;
        for (int j = 0; j < ic; j++) {
            itv[j] = tvar_fresh(tc); tc->tvars[itv[j]].elem = tvar_fresh(tc); tc->tvars[itv[j]].box_c = tvar_fresh(tc); tc->tvars[itv[j]].tag_p = tvar_fresh(tc);
            /* An input is the caller's value: open or input, not unknown. */
            tc->tvars[itv[j]].unknown = 0; tc->tvars[tc->tvars[itv[j]].elem].unknown = 0; tc->tvars[tc->tvars[itv[j]].box_c].unknown = 0; tc->tvars[tc->tvars[itv[j]].tag_p].unknown = 0;
            tc_push(tc, TC_NONE, t->line); tc->data[tc->sp-1].tvar_id = itv[j];
            /* Without a declared signature, an input stands for whatever record a caller passes. */
            if (!has_sig) { int t = itv[j]; tc->tvars[t].open = 1;
                tc->tvars[tc->tvars[t].elem].open = 1; tc->tvars[tc->tvars[t].box_c].open = 1; tc->tvars[tc->tvars[t].tag_p].open = 1; }
            /* A tuple input is code whose effect the signature does not state. */
            if (has_sig && in_c[j] == TC_TUPLE) tc->data[tc->sp-1].flags |= AT_OPAQUE;
            if (has_sig) { int t = itv[j]; tc->tvars[t].input = 1;
                tc->tvars[tc->tvars[t].elem].input = 1; tc->tvars[tc->tvars[t].box_c].input = 1; tc->tvars[tc->tvars[t].tag_p].input = 1; }
            if (has_sig && in_c[j] != TC_NONE && in_c[j] != TC_TUPLE) {
                tc->data[tc->sp-1].type = in_c[j]; tc->tvars[itv[j]].bound = in_c[j];
                if (in_c[j] == TC_BOX) tc->data[tc->sp-1].flags |= AT_LINEAR;
                if (in_elem[j] != TC_NONE) tc->tvars[tc->tvars[itv[j]].elem].bound = in_elem[j];
                tc_set_tags(tc, &tc->data[tc->sp-1], in_tags[j]);
            }
            if (has_sig && in_tv[j]) {
                int target = tc_is_container(in_c[j]) ? tc->tvars[itv[j]].elem : itv[j], k = 0;
                while (k < nvar && var_name[k] != in_tv[j]) k++;
                if (k < nvar) tvar_unify(tc, var_tv[k], target); else { var_name[nvar] = in_tv[j]; var_tv[nvar++] = target; }
            }
        }
        if (wide) for (int j = 0; j < eff_c; j++) tc_push(tc, TC_NONE, t->line);
        int pe = -1, rc0 = tc->rcall_count; uint32_t early_sym = def_sym;
        if (tc->recur_pending && tc->recur_sym) {
            /* The name is bound for the body before its let runs: a name bound already is redefined. */
            if (i >= tc->user_start) {
                TCBinding *prev = tc_lookup(tc, tc->recur_sym);
                if (tc_is_builtin(tc->recur_sym, tc->prelude_sig_count)) tc_error(tc, t->line, 0, "'%s' is already defined", sym_name(tc->recur_sym));
                else if (prev) tc_error(tc, t->line, 0, "'%s' is already defined (first defined on line %d)", sym_name(tc->recur_sym), prev->def_line);
            }
            early_sym = tc->recur_sym;
            pe = tc_alloc_effect(tc); tc->effects[pe].consumed = eff_c; tc->effects[pe].produced = eff_p; tc->effects[pe].out_type = eff_out;
            tc->effects[pe].placeholder = 1;
            /* Without a signature, the effect is the pre-scan's guess until the body is checked. */
            tc->effects[pe].opaque = !has_sig;
            AbstractType pa = {0}; pa.type = TC_TUPLE; pa.effect_idx = pe; tc_bind(tc, tc->recur_sym, &pa, t->line, 1);
        }
        /* Inside the body, recur_pending must be 0 so nested defs don't
           mis-attribute to recur_sym. _s.recur_pending restores it at exit
           so the outer def post-body still consumes it. */
        tc->recur_pending = 0;
        int u0 = tc->underflows, oa0 = tc->opaque_at, dv0 = tc->diverged, uc0 = tc->unknown_code; tc->opaque_at = -1; tc->diverged = 0; tc->unknown_code = 0;
        int ld0 = tc->literal_depth, my_body = ++tc->body_ids, outer_body = tc->cur_body; tc->literal_depth = 0; tc->cur_body = my_body;
        /* `(then) (else) if`: each branch starts from the bindings the if starts from, and a binding
           either branch consumes stays consumed after it. */
        int nb0 = tc->bind_count, *used0 = NULL, *used = NULL, nused = 0;
        int then_at = close+1 < total_count && toks[close+1].tag == TOK_LPAREN ? close+2 + toks[close+1].span : -1;
        int is_then = then_at >= 0 && then_at < total_count && toks[then_at].tag == TOK_WORD && toks[then_at].as.sym == S_IF;
        int is_else = i > 0 && toks[i-1].tag == TOK_RPAREN && close+1 < total_count && toks[close+1].tag == TOK_WORD && toks[close+1].as.sym == S_IF;
        if (is_then) { used0 = malloc((size_t)(nb0 + 1) * sizeof(int)); if (!used0) die("type checker: out of memory for %d bindings", nb0);
            for (int k = 0; k < nb0; k++) used0[k] = tc->bindings[k].consumed_line; }
        if (is_else) { used = tc->branch_used; nused = tc->branch_used_n; tc->branch_used = NULL; tc->branch_used_n = 0; }
        tc->body_depth++; tc_process_range(tc, toks, i+1, close, total_count); tc->body_depth--;
        if (is_then) {
            free(tc->branch_used); tc->branch_used = malloc((size_t)(2*nb0 + 1) * sizeof(int)); tc->branch_used_n = 0;
            if (!tc->branch_used) die("type checker: out of memory for %d bindings", nb0);
            for (int k = 0; k < nb0; k++) if (!used0[k] && tc->bindings[k].consumed_line) {
                tc->branch_used[tc->branch_used_n++] = k; tc->branch_used[tc->branch_used_n++] = tc->bindings[k].consumed_line;
                tc->bindings[k].consumed_line = 0; }
            free(used0);
        }
        if (is_else) { for (int k = 0; k < nused; k += 2) if (!tc->bindings[used[k]].consumed_line) tc->bindings[used[k]].consumed_line = used[k+1]; free(used); }
        tc->literal_depth = ld0; tc->cur_body = outer_body;
        int under = tc->underflows - u0, opaque = tc->opaque_at >= 0, diverges = tc->diverged, unknown_code = tc->unknown_code;
        tc->underflows = u0; tc->opaque_at = oa0; tc->diverged = dv0; tc->unknown_code = uc0;
        /* A recursive call passes records to the inputs whose keys are known only now. */
        int word_needs = 0; for (int j = 0; j < ic; j++) if (tc_need_key(tc, itv[j])) word_needs = 1;
        for (int c = rc0; c < tc->rcall_count; c++) {
            if (tc->rcalls[c].effect != pe) continue;
            /* A call inside a body that never ran where the checker could see it passes records it cannot follow. */
            if (tc->rcalls[c].body != my_body) { if (word_needs) tc_error(tc, tc->rcalls[c].line, 0, "'%s' calls itself inside a body that the checker cannot follow to where it runs; call it directly, or from an if, case or loop body", sym_name(tc->recur_sym)); continue; }
            for (int k = 0; k < tc->rcalls[c].n && k < ic; k++) {
                int arg = tc->rcalls[c].arg[tc->rcalls[c].n-1-k], j = ic-1-k;
                if (arg > 0) { tc_flow_input(tc, arg, itv[j], 1, sym_name(tc->recur_sym), j, tc->rcalls[c].line); tc_flow_contents(tc, itv[j], arg, sym_name(tc->recur_sym), j, tc->rcalls[c].line, 1); }
            }
        }
        uint32_t reads = 0;
        for (int j = 0; j < ic && !reads; j++) reads = tc_need_key(tc, itv[j]);
        if (!wide) {
            int ao = tc->sp - _s.sp; oc = ao > 16 ? 16 : (ao > 0 ? ao : 0);
            for (int j = 0; j < oc; j++) { int idx = tc->sp - oc + j; if (idx < 0) continue;
                otv[j] = tc_value_tvar(tc, &tc->data[idx]); }
            sc = tc->tvar_count - scheme_base;
            if (ao == 1 && tc->data[tc->sp-1].type == TC_TUPLE && tc->data[tc->sp-1].effect_idx >= 0) out_eff = tc->data[tc->sp-1].effect_idx;
            /* The observed top type is more precise than the pre-scan's. */
            if (ao > 0 && tc->data[tc->sp-1].type != TC_NONE) eff_out = tc->data[tc->sp-1].type;
        }
        /* A recursive call leaves what the word leaves for its own arguments: a copy of the word's type
           takes the call's arguments, and its outputs are the call's. */
        if (!wide) { TupleEffect sch = {0}; sch.scheme_base = scheme_base; sch.scheme_count = sc; sch.in_count = ic; sch.out_count = oc;
            for (int j = 0; j < ic; j++) sch.in_tvars[j] = itv[j];
            for (int j = 0; j < oc; j++) sch.out_tvars[j] = otv[j];
            for (int c = rc0; c < tc->rcall_count; c++) {
                if (tc->rcalls[c].effect != pe || !sc || tc->rcalls[c].nout != oc) continue;
                int *map = calloc((size_t)sc, sizeof(int)); if (!map) die("out of memory instantiating %d type variables", sc);
                tvar_instantiate(tc, &sch, map, NULL, 0);
                int n = tc->rcalls[c].n;
                for (int j = 0; j < ic && n >= ic; j++) { int ar = tc->rcalls[c].arg[n - ic + j], o = itv[j] - scheme_base;
                    if (ar > 0 && o >= 0 && o < sc && map[o] > 0) tvar_unify(tc, map[o], ar); }
                for (int j = 0; j < oc; j++) { int bo = tc->rcalls[c].out[j], o = otv[j] - scheme_base;
                    if (bo <= 0 || o < 0 || o >= sc || map[o] <= 0) continue;
                    /* Every key read from the call's output must be in what the word leaves. */
                    uint32_t miss = tc->tvars[tvar_find(tc, bo)].need ? rec_flow(tc, map[o], bo, 1) : 0;
                    if (miss) tc_error(tc, tc->rcalls[c].line, t->line, "'%s' reads '%s from what its call to itself leaves, but the word may leave a value without '%s.", sym_name(early_sym), miss == REC_TOO_DEEP ? "?" : sym_name(miss), miss == REC_TOO_DEEP ? "?" : sym_name(miss));
                    tvar_unify(tc, map[o], bo); }
                free(map);
            }
        }
        /* What the body really took and left, rather than the pre-scan's guess. */
        if (!has_sig) { eff_c = (wide ? eff_c : ic) + under; eff_p = tc->sp - _s.sp; }
        else {
            int left = tc->sp - _s.sp;
            if (under) tc_error(tc, t->line, 0, "'%s' reaches below the %d input(s) its type declares", who, n_in);
            /* After code of unknown effect runs, only the caller knows what the body leaves; so the body
               cannot show that an output of an input's type variable is that input. */
            else if (opaque || diverges) {
                for (int k = 0, o = 0; opaque && i >= tc->user_start && k < dsig.slot_count; k++) {
                    if (dsig.slots[k].direction != DIR_OUT) continue; o++;
                    for (int e = -1; e < dsig.slots[k].either_count; e++) {
                        uint32_t v = e < 0 ? dsig.slots[k].type_var : dsig.slots[k].either_types[e] == TC_NONE ? dsig.slots[k].either_tvars[e] : 0; int j = 0;
                        while (v && j < ic && in_tv[j] != v) j++;
                        if (v && j < ic && in_c[j] != TC_INT && in_c[j] != TC_FLOAT && in_c[j] != TC_NUM && in_c[j] != TC_ORD && in_c[j] != TC_SYM)
                            tc_error(tc, t->line, 0, "'%s' runs code whose effect the checker cannot see (a tuple input), so it cannot check that output %d, declared '%s like input %d, is that input. Give the output a type of its own.", who, o, sym_name(v), j+1);
                    }
                }
            }
            else if (left != n_out) tc_error(tc, t->line, 0, "'%s' leaves %d value(s) but its type declares %d output(s)", who, left, n_out);
            else for (int k = 0, o = 0; k < dsig.slot_count; k++) {
                if (dsig.slots[k].direction != DIR_OUT) continue;
                AbstractType *at = &tc->data[_s.sp + o++];
                TypeConstraint want = dsig.slots[k].constraint, got = at->type != TC_NONE ? at->type : at->tvar_id > 0 ? tvar_resolve(tc, at->tvar_id) : TC_NONE;
                if (want != TC_NONE && got != TC_NONE && !tc_constraint_matches(want, got) && !tc_constraint_matches(got, want))
                    tc_error(tc, t->line, 0, "'%s' output %d is %s but its type declares %s", who, o, constraint_name(got), constraint_name(want));
                /* A declared body output hides the body's effect from callers. */
                int ot = tc_value_tvar(tc, at), oreads = at->effect_idx >= 0 ? (int)tc->effects[at->effect_idx].reads : (int)tc->tvars[tvar_find(tc, ot)].reads;
                if (!dsig.slots[k].type_var && (want == TC_TUPLE || want == TC_NONE) && oreads) tc_escape(tc, (uint32_t)oreads, who, t->line);
                /* A declared either names every tag the output can carry. */
                if (dsig.slots[k].either_count > 0 && i >= tc->user_start && tc_tags(tc, at)) {
                    UnionDef *ud = &tc->unions[tc_tags(tc, at)-1];
                    for (int g = 0; g < ud->count; g++) { int named = 0;
                        for (int e = 0; e < dsig.slots[k].either_count; e++) if (dsig.slots[k].either_syms[e] == ud->syms[g]) named = 1;
                        if (!named) tc_error(tc, t->line, 0, "'%s' declares output %d as an either without '%s, but its body can leave a value tagged '%s. Add '%s to the either, or tag the value with one it names.", who, o, sym_name(ud->syms[g]), sym_name(ud->syms[g]), sym_name(ud->syms[g])); }
                }
                /* An output of an input's type variable, or an either payload of one, is that input to callers,
                   keys and all: it must be the input. */
                for (int e = -1; e < dsig.slots[k].either_count && i >= tc->user_start; e++) {
                    uint32_t v = e < 0 ? dsig.slots[k].type_var : dsig.slots[k].either_types[e] == TC_NONE ? dsig.slots[k].either_tvars[e] : 0; int j = 0;
                    while (v && j < ic && in_tv[j] != v) j++;
                    if (!v || j == ic || in_c[j] == TC_INT || in_c[j] == TC_FLOAT || in_c[j] == TC_NUM || in_c[j] == TC_ORD) continue;
                    int vw = tc->tvars[tvar_find(tc, ot)].vrow, uid = tc_tags(tc, at), a = tc_is_container(in_c[j]) ? tc->tvars[itv[j]].elem : itv[j];
                    int b = e >= 0 ? (vw ? row_find(tc, vw, dsig.slots[k].either_syms[e]) : 0) : tc_is_container(want) ? tvar_content(tc, ot, want) : ot;
                    /* A tag the body never leaves has no payload to check. */
                    if (e >= 0 && !b && uid) { int has = 0; for (int g = 0; g < tc->unions[uid-1].count; g++) if (tc->unions[uid-1].syms[g] == dsig.slots[k].either_syms[e]) has = 1; if (!has) continue; }
                    int same = a > 0 && b > 0 && (tvar_find(tc, a) == tvar_find(tc, b) || rec_base(tc, a) == rec_base(tc, b));
                    /* An input of another type, or a join with one, is not this input. */
                    /* A declared input shows nothing of its own: a type that holds keys, payloads or a body, or is
                       unknown, has met another value on the way. */
                    if (same) { TVarEntry *ea = &tc->tvars[tvar_find(tc, a)]; if (ea->row || ea->rest || ea->vrow || ea->code > 0 || ea->unknown || ea->need) same = 0; }
                    int another = 0;
                    for (int q = 0; q < ic && b > 0; q++) if (in_tv[q] != v) {
                        int iq = tc_is_container(in_c[q]) ? tc->tvars[itv[q]].elem : itv[q];
                        if (iq > 0 && tvar_find(tc, iq) == tvar_find(tc, b)) another = 1; }
                    TypeConstraint ra = a > 0 ? tvar_resolve(tc, a) : TC_NONE, rb = b > 0 ? tvar_resolve(tc, b) : TC_NONE;
                    /* The body cannot always link a list's elements or a payload to its input; a value that
                       shows nothing of its own, of the input's type, is taken to be the input. */
                    /* A symbol written in the body is not the caller's symbol. */
                    int other = another || (e < 0 && at->sym_id) || b <= 0 || rb == TC_REC || tv_info(tc, b) || tc->tvars[tvar_find(tc, b)].unknown || (rb != TC_NONE && rb != ra && tc_stricter(ra, rb));
                    if ((!same || another) && other) tc_error(tc, t->line, 0, "'%s' declares %s%s of output %d as '%s, the type of its input %d, so a caller takes it to be that input, keys and all. But the body leaves another value there. Leave the input, or give it a type of its own.",
                                                 who, e < 0 ? "the value" : "the payload of '", e < 0 ? "" : sym_name(dsig.slots[k].either_syms[e]), o, sym_name(v), j+1);
                }
            }
            /* A declared input is rigid: the body must work for every value its type admits. */
            for (int j = 0; j < ic; j++) {
                TypeConstraint need = tvar_resolve(tc, itv[j]);
                if (in_c[j] != TC_TUPLE && tc_stricter(in_c[j], need))
                    tc_error(tc, t->line, 0, "'%s' declares input %d as %s, but its body only works when it is %s. Declare %s, or change the body so it accepts %s%s.",
                             who, j+1, in_c[j] == TC_NONE ? "any value" : constraint_name(in_c[j]), constraint_name(need), constraint_name(need),
                             in_c[j] == TC_NONE ? "any value" : "every ", in_c[j] == TC_NONE ? "" : constraint_name(in_c[j]));
                else if (tc_is_container(in_c[j]) && tc_stricter(in_elem[j], tvar_resolve(tc, tc->tvars[itv[j]].elem))) {
                    TypeConstraint en = tvar_resolve(tc, tc->tvars[itv[j]].elem);
                    tc_error(tc, t->line, 0, "'%s' declares input %d as a %s of %s, but its body only works when the elements are %s. Declare %s elements, or change the body.",
                             who, j+1, constraint_name(in_c[j]), constraint_name(in_elem[j]), constraint_name(en), constraint_name(en));
                }
            }
        }
        int out_tags = tc->sp > _s.sp ? tc_tags(tc, &tc->data[tc->sp-1]) : 0;
        int body_captured = tc->saw_linear_capture;
        /* output_is_linear: the body's sole output is itself a linear-capturing
           closure (e.g. `(42 box 'b let (b free))` or a def chain thereof).
           Detected by checking that the one residual stack value is a tuple
           carrying AT_LINEAR. */
        int output_captures_linear = (tc->sp - _s.sp) == 1
            && tc->data[tc->sp-1].type == TC_TUPLE
            && (tc->data[tc->sp-1].flags & AT_LINEAR);
        tc->sp = _s.sp; tc->bind_count = _s.bind_count;
        tc->recur_pending = _s.recur_pending;
        type_sig_count = _s.type_sig_count; tc->sp_floor = _s.sp_floor;
        tc->saw_linear_capture = _s.saw_linear_capture;
        tc_push(tc, TC_TUPLE, t->line);
        int eidx = tc_alloc_effect(tc); TupleEffect *eff = &tc->effects[eidx];
        eff->consumed = eff_c; eff->produced = eff_p; eff->out_type = eff_out; eff->out_effect = out_eff;
        eff->scheme_base = scheme_base; eff->scheme_count = sc; eff->in_count = ic; eff->out_count = oc;
        /* Recursive calls ran with the pre-scan's guess of the word's effect: a wrong guess leaves its model of them wrong. */
        if (pe >= 0 && (tc->effects[pe].consumed != eff_c || tc->effects[pe].produced != eff_p)) unknown_code = 1;
        eff->output_is_linear = output_captures_linear; eff->opaque = opaque && !has_sig; eff->diverges = diverges; eff->out_tags = out_tags; eff->reads = reads; eff->unknown = unknown_code; eff->body_id = my_body;
        /* Calls made before this body was checked took it to leave what lies below its inputs alone:
           the next pass treats them as the unknown code they are. */
        int early_call = 0; for (int k = 0; k < tc->early_n; k += 2) if ((uint32_t)tc->early[k] == early_sym) early_call = 1;
        if (early_sym && unknown_code && early_call) {
            if (tc->unk_n == tc->unk_cap) { tc->unk_cap = tc->unk_cap ? 2*tc->unk_cap : 16;
                tc->unk = realloc(tc->unk, (size_t)tc->unk_cap * sizeof(uint32_t)); if (!tc->unk) die("type checker: out of memory for %d words", tc->unk_n); }
            tc->unk[tc->unk_n++] = early_sym;
        }
        for (int j = 0; j < ic; j++) eff->in_tvars[j] = itv[j];
        for (int j = 0; j < oc; j++) eff->out_tvars[j] = otv[j];
        { int kept = rc0; for (int c = rc0; c < tc->rcall_count; c++) if (tc->rcalls[c].effect != pe) tc->rcalls[kept++] = tc->rcalls[c]; tc->rcall_count = kept; }
        /* has_let tracks only `let`-bindings whose value is read back as a
           WORD later in the same body. A binding referenced only as a SYM
           literal (e.g. `'data k nth`) accesses the value via `nth` without
           pulling it to the stack, so it can't create an aliasing snapshot. */
        for (int k = i+1; k < close; k++) {
            if (toks[k].tag == TOK_WORD && toks[k].as.sym == S_LET
                && k >= i+2 && toks[k-1].tag == TOK_SYM) {
                uint32_t bn = toks[k-1].as.sym;
                for (int m = k+1; m < close; m++) {
                    if (toks[m].tag == TOK_WORD && toks[m].as.sym == bn) { eff->has_let = 1; break; }
                    if (toks[m].tag == TOK_LPAREN) m = (m+toks[m].span);
                }
            }
            if (toks[k].tag == TOK_LPAREN) k = (k+toks[k].span);
            if (eff->has_let) break;
        }
        tc->data[tc->sp-1].effect_idx = eidx;
        if (body_captured) { tc->data[tc->sp-1].flags |= AT_LINEAR; tc->saw_linear_capture = 1; }
    }
}
static void tc_process_range(TypeChecker *tc, Token *toks, int start, int end, int total_count) {
    for (int i = start; i < end; i++) {
        if (i == tc->user_start && !tc->prelude_sig_count) { tc->prelude_sig_count = type_sig_count; tc->prelude_binds = tc->bind_count; }
        Token *t = &toks[i]; current_loc = LOC_PACK(t->fid, t->line, t->col);
        switch (t->tag) {
        case TOK_INT: tc_push(tc, TC_INT, t->line); break;
        case TOK_FLOAT: tc_push(tc, TC_FLOAT, t->line); break;
        case TOK_SYM: tc_push(tc, TC_SYM, t->line); tc->data[tc->sp-1].sym_id = t->as.sym; break;
        case TOK_STRING: {
            tc_push(tc, TC_LIST, t->line);
            if (tc->data[tc->sp-1].tvar_id > 0) {
                int ev = tvar_content(tc, tc->data[tc->sp-1].tvar_id, TC_LIST);
                if (ev > 0) tvar_bind(tc, ev, TC_INT);
            }
            break;
        }
        case TOK_LPAREN: {
            int close = (i+toks[i].span);
            tc_tuple(tc, toks, i, close, total_count);
            i = close; break;
        }
        case TOK_LBRACKET: {
            int close = (i+toks[i].span);
            int is_type_annot = (close+1 < total_count && toks[close+1].tag == TOK_WORD && toks[close+1].as.sym == S_EFFECT);
            /* A list of records has the keys all its elements have. */
            int recs = 0, others = 0, empty = 1;
            if (!is_type_annot) {
                int f0 = tc->sp_floor, s0 = tc_literal(tc, toks, i, close, total_count, "list");
                for (int k = s0; k < tc->sp; k++) {
                    int v = tc->data[k].type == TC_TUPLE ? tc_value_tvar(tc, &tc->data[k]) : tc->data[k].tvar_id;
                    if (tv_info(tc, v)) { if (!recs) { recs = tvar_fresh(tc); tc->tvars[recs].unknown = 0; } tvar_unify(tc, recs, v); } else others = 1;
                }
                empty = tc->sp == s0;
                tc->sp = s0; tc->sp_floor = f0;
            }
            TypeConstraint elem = tc_check_list_elements(tc, toks, i+1, close, t->line);
            tc_push(tc, TC_LIST, t->line);
            /* The list holds its elements' join; one with no elements holds nothing yet. */
            if (recs && !others) tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].elem = recs;
            else if (empty) tc->tvars[tvar_find(tc, tvar_content(tc, tc->data[tc->sp-1].tvar_id, TC_LIST))].unknown = 0;
            if (elem != TC_NONE && tc->data[tc->sp-1].tvar_id > 0) {
                int ev = tvar_content(tc, tc->data[tc->sp-1].tvar_id, TC_LIST);
                if (ev > 0) tvar_bind(tc, ev, elem);
            }
            i = close; break;
        }
        case TOK_LBRACE: {
            int close = (i+toks[i].span);
            TypeConstraint veffout = TC_NONE;
            int has_eff = 0;
            /* The clause that decides what `case` leaves: each body runs with the payload
               (or scrutinee) on top, so that value counts as its first input. */
            int cl_c = 0, cl_p = 0, cl_rank = 3, cl_out = TC_NONE, cl_tags = 0, cl_returns = 0, nk = 0;
            int is_case = close+1 < total_count && toks[close+1].tag == TOK_WORD && toks[close+1].as.sym == S_CASE && tc->sp >= 2;
            if (!is_case) {
                /* build_tuple makes a record when the values pair up as 'key value, and a tuple otherwise. */
                int f0 = tc->sp_floor, s0 = tc_literal(tc, toks, i, close, total_count, "record"), n = tc->sp - s0;
                int keyed = 1, known = 1, valued = 0;
                for (int k = 0; k < n; k++) {
                    AbstractType *at = &tc->data[s0+k];
                    TypeConstraint c = at->type != TC_NONE ? at->type : at->tvar_id > 0 ? tvar_resolve(tc, at->tvar_id) : TC_NONE;
                    int sym = c == TC_SYM, not_sym = c != TC_NONE && !tc_constraint_matches(c, TC_SYM);
                    if (k % 2 == 0) { if (not_sym) keyed = 0; else if (!sym) known = 0; }
                    else if (not_sym) valued = 1;
                }
                uint32_t last = n ? tc->data[s0+n-1].sym_id : 0; TypeConstraint top = n ? tc->data[s0+n-1].type : TC_NONE;
                int w = 0;
                if (n % 2 == 0 && keyed && known) {
                    w = row_new(tc, t->line);
                    for (int k = 0; k < n && w; k += 2)
                        if (tc->data[s0+k].sym_id) row_put(tc, w, tc->data[s0+k].sym_id, tc_value_tvar(tc, &tc->data[s0+k+1])); else w = 0;
                }
                tc->sp = s0; tc->sp_floor = f0;
                if (n % 2 && keyed && known && valued) {
                    char src[64];
                    tc_error(tc, t->line, 0, "record literal: key '%s has no value in %s. A record is 'key value pairs, as in {'a 1 'b 2}.", last ? sym_name(last) : "?", tok_src(toks, i, src, sizeof src));
                }
                if (n % 2 == 0 && keyed && known) tc_push_rec(tc, w, t->line);
                else tc_push(tc, n % 2 || !keyed ? TC_TUPLE : TC_NONE, t->line);
                /* A tuple runs when it is applied or looked up by name: it pushes the values it holds. */
                if (tc->data[tc->sp-1].type == TC_TUPLE) {
                    int e = tc_alloc_effect(tc); tc->effects[e].produced = n; tc->effects[e].out_type = top; tc->data[tc->sp-1].effect_idx = e;
                }
                i = close; break;
            }
            AbstractType *scr0 = &tc->data[tc->sp-2];
            TypeConstraint st = scr0->type != TC_NONE ? scr0->type : scr0->tvar_id > 0 ? tvar_resolve(tc, scr0->tvar_id) : TC_NONE;
            int untagged = st != TC_NONE && !tc_constraint_matches(st, TC_TAGGED);
            uint32_t keys[UNION_VARIANTS_MAX];
            int ceff[16], cpred[16], nce = 0; uint32_t ckey[16];
            for (int j = i+1; j < close; ) {
                char kt[64], vt[64]; int kj = j, pred = -1;
                /* A predicate runs on the scrutinee: check it as a body. */
                if (toks[j].tag == TOK_LPAREN) { tc_tuple(tc, toks, j, j + toks[j].span, total_count); pred = tc->data[tc->sp-1].effect_idx; tc->sp--; }
                if (toks[j].tag == TOK_SYM) {
                    if (nk == UNION_VARIANTS_MAX) die("case: more than %d tag clauses", UNION_VARIANTS_MAX);
                    keys[nk++] = toks[j].as.sym;
                    if (untagged && nk == 1)
                        tc_error(tc, toks[j].line, 0, "'case' clause '%s matches a tag, but the value is %s, not tagged. Tag keys match only a tagged value; test a %s with (predicate) keys, as in {(5 lt) (2 mul)}.",
                                 sym_name(toks[j].as.sym), constraint_name(st), constraint_name(st));
                } else if (toks[j].tag != TOK_LPAREN)
                    tc_error(tc, toks[j].line, 0, "'case' clause key %s is not a 'tag or a (predicate). Tag keys match a tagged value, as in {'ok (1 plus)}; predicate keys test the value, as in {(5 lt) (2 mul)}.", tok_src(toks, j, kt, sizeof kt));
                j += toks[j].span > 0 ? toks[j].span + 1 : 1;
                if (j >= close) { tc_error(tc, toks[kj].line, 0, "case: need even number of clauses: %s has no body. Clauses are key and body pairs, as in {'ok (1 plus) 'no (drop 0)}.", tok_src(toks, kj, kt, sizeof kt)); break; }
                if (toks[j].tag != TOK_LPAREN) {
                    tc_error(tc, toks[j].line, 0, "'case' clause %s is %s, not a body. A clause runs code on the payload: write (drop %s) to replace it.", tok_src(toks, kj, kt, sizeof kt), tok_src(toks, j, vt, sizeof vt), vt);
                    j += toks[j].span > 0 ? toks[j].span + 1 : 1;
                    continue;
                }
                {
                    int vc2 = (j+toks[j].span);
                    int vc = 0, vp = 0; TypeConstraint vo = tc_infer_effect(toks, j+1, vc2, &vc, &vp, tc, NULL, 0);
                    if (!has_eff) { veffout = vo; has_eff = 1; }
                    if (vo != TC_NONE && veffout != TC_NONE && vo != veffout && !tc_constraint_matches(veffout, vo) && !tc_constraint_matches(vo, veffout))
                        tc_error(tc, t->line, 0, "clause bodies produce different types: %s vs %s", constraint_name(veffout), constraint_name(vo));
                    /* A case clause body that creates a linear closure
                       escapes conditional dispatch — the resulting value can be
                       applied twice without detection. Ban box creation nested in
                       clause bodies. Runtime catches double-free but we'd rather
                       surface this at TC. */
                    tc_tuple(tc, toks, j, vc2, total_count);
                    TupleEffect *ce = &tc->effects[tc->data[tc->sp-1].effect_idx];
                    if (nce == 16) die("case: more than 16 clause bodies; the checker follows at most 16. Split the case.");
                    ceff[nce] = tc->data[tc->sp-1].effect_idx; cpred[nce] = pred; ckey[nce] = toks[kj].tag == TOK_SYM ? toks[kj].as.sym : 0; nce++;
                    tc->sp--;
                    int rank = ce->diverges ? 2 : ce->opaque;
                    if (rank == 0 && cl_rank == 0 && ce->produced - ce->consumed != cl_p - cl_c)
                        tc_error(tc, toks[j].line, 0, "'case' clauses leave different counts: net %+d vs net %+d", cl_p - cl_c, ce->produced - ce->consumed);
                    if (rank < 2) cl_tags = cl_returns++ ? tc_tags_merge(tc, cl_tags, ce->out_tags) : ce->out_tags;
                    if (rank < cl_rank) {
                        cl_rank = rank; cl_c = ce->consumed > 0 ? ce->consumed : 1;
                        cl_p = ce->produced + cl_c - ce->consumed; cl_out = ce->out_type;
                    }
                    for (int k = j+1; k < vc2; k++) {
                        if (toks[k].tag == TOK_LPAREN) { k = (k+toks[k].span); continue; }
                        if (toks[k].tag == TOK_LBRACKET) { k = (k+toks[k].span); continue; }
                        if (toks[k].tag == TOK_LBRACE) { k = (k+toks[k].span); continue; }
                        if (toks[k].tag == TOK_WORD && tc_word_produces_linear(toks[k].as.sym))
                            tc_error(tc, t->line, 0, "clause body may not produce a linear value (via '%s') — linear-capturing closures from conditional branches cannot be tracked for single-use", sym_name(toks[k].as.sym));
                    }
                    j = vc2 + 1;
                }
            }
            {
                tc_push(tc, TC_TUPLE, t->line);
                /* The default is pushed, not run, when no clause matches. That cannot happen when
                   the scrutinee's tags are known and each has a clause. */
                AbstractType *scr = &tc->data[tc->sp-3], *dfl = &tc->data[tc->sp-2];
                int live = scr->type != TC_TAGGED || !tc_tags(tc, scr); uint32_t missing = 0;
                if (!live) { UnionDef *ud = &tc->unions[tc_tags(tc, scr)-1];
                    for (int v = 0; v < ud->count; v++) { int found = 0;
                        for (int k = 0; k < nk; k++) if (keys[k] == ud->syms[v]) found = 1;
                        if (!found) { live = 1; if (!missing) missing = ud->syms[v]; } } }
                if (live) {
                    int dtags = tc_tags(tc, dfl);
                    if (cl_rank == 0 && cl_p != cl_c) {
                        const char *why = scr->type != TC_TAGGED ? "no predicate holds, which can always happen"
                            : !tc_tags(tc, scr) ? "a tag it has no clause for arrives, and the checker cannot see which tags arrive"
                            : "the scrutinee is tagged '%s, which has no clause";
                        const char *fix = scr->type != TC_TAGGED ? "Make each clause leave one value in place of the scrutinee."
                            : !tc_tags(tc, scr) ? "Declare the tags where the value comes from, as in [{'ok int 'no int list} either lent in ...] effect, or make each clause leave one value in place of the scrutinee."
                            : "Add a clause for '%s, or make each clause leave one value in place of the scrutinee.";
                        char msg[512]; snprintf(msg, sizeof msg, "'case' clauses leave net %+d, but it pushes its default when %s, and that leaves net +0.\n    %s", cl_p - cl_c, why, fix);
                        const char *m = missing ? sym_name(missing) : "";
                        tc_error(tc, t->line, 0, msg, m, m);
                    }
                    cl_tags = cl_returns++ ? tc_tags_merge(tc, cl_tags, dtags) : dtags;
                    if (cl_rank >= 2) { cl_rank = 0; cl_c = 1; cl_p = 1; cl_out = dfl->type; }
                }
                {
                    int eidx = tc_alloc_effect(tc); TupleEffect *e = &tc->effects[eidx];
                    e->out_type = cl_rank < 3 ? (TypeConstraint)cl_out : veffout;
                    e->consumed = cl_c; e->produced = cl_p; e->opaque = cl_rank == 1; e->diverges = cl_rank == 2; e->clauses = cl_rank < 3;
                    e->out_tags = cl_returns ? cl_tags : 0;
                    e->nclause = nce; e->dflt_live = live;
                    for (int c = 0; c < nce; c++) if (tc->effects[ceff[c]].unknown) e->unknown = 1;
                    for (int c = 0; c < nce; c++) { e->clause_eff[c] = ceff[c]; e->clause_pred[c] = cpred[c]; e->clause_key[c] = ckey[c]; }
                    tc->data[tc->sp-1].effect_idx = eidx;
                }
            }
            i = close; break;
        }
        case TOK_WORD: {
            uint32_t sym = t->as.sym;
            if (sym == S_HALT || (sym == S_MUST && i > start && toks[i-1].tag == TOK_WORD && (toks[i-1].as.sym == S_NO || toks[i-1].as.sym == S_NONE)))
                tc->diverged = 1;
            if (sym == S_LET) {
                if (tc->sp > tc->sp_floor && tc->data[tc->sp-1].type == TC_SYM) tc_take(tc, 2, t->line);
                if (tc->sp < 2 || tc->data[tc->sp-1].type != TC_SYM || !tc->data[tc->sp-1].sym_id) {
                    tc_error(tc, t->line, 0, "let: expected a value and then a 'name, as in `42 'x let`");
                    tc->sp = tc->sp_floor;
                } else {
                    uint32_t ns = tc->data[tc->sp-1].sym_id; AbstractType vt = tc->data[tc->sp-2]; tc->sp -= 2;
                    int is_recur = tc->recur_pending && tc->recur_sym == ns;
                    if (tc->recur_pending) tc->recur_pending = 0;
                    /* A declared type is checked against a body written right before `'name let`; callers trust it. */
                    if (ns && i >= tc->user_start && typesig_find(ns) && !tc_is_builtin(ns, tc->prelude_sig_count)
                        && !(i >= 2 && (toks[i-2].tag == TOK_RPAREN || (toks[i-2].tag == TOK_WORD && toks[i-2].as.sym == S_EFFECT))))
                        tc_error(tc, t->line, vt.source_line, "'%s' has a declared type, so it must be bound to a body written right before `'%s let`, where the checker checks the body against the type. This value comes from line %d.", sym_name(ns), sym_name(ns), vt.source_line);
                    if (ns) {
                        if (tc->body_depth > 0 && ((vt.flags & AT_LINEAR) || vt.type == TC_BOX)) {
                            int seen = 0;
                            for (int k = i+1; k < end; k++) if (toks[k].tag == TOK_WORD && toks[k].as.sym == ns) { seen = 1; break; }
                            if (!seen) tc_error(tc, t->line, vt.source_line, "linear value bound as '%s' is never referenced in the enclosing quotation — it will be captured and leaked", sym_name(ns));
                        }
                        /* A linear-capturing closure that recursively references itself
                           would double-consume on each recursion. Scan the tuple body
                           preceding this `'name let` for a self-reference. */
                        if ((vt.flags & AT_LINEAR) && vt.type == TC_TUPLE && i >= 2 && toks[i-2].tag == TOK_RPAREN) {
                            int b2 = i-2;
                            for (int m = b2 + toks[b2].span + 1; m < b2; m++)
                                if (toks[m].tag == TOK_WORD && toks[m].as.sym == ns) {
                                    tc_error(tc, t->line, 0, "linear-capturing closure '%s' cannot recurse on itself — each recursive call would re-consume the captured linear value", sym_name(ns));
                                    break;
                                }
                        }
                        /* A bound dict would alias the one on the stack: dropping either frees both. */
                        if (vt.type == TC_DICT) {
                            tc_error(tc, t->line, vt.source_line, "cannot 'let'-bind a dict as '%s' — a dict is a heap object and the binding would alias it, so dropping either copy leaves the other reading freed memory. Thread the dict on the stack instead (see examples/kv-server.slap), or use a record if you want a value you can bind.", sym_name(ns));
                            break;
                        }
                        if (!is_recur && i >= tc->user_start) {
                            TCBinding *old = tc_lookup(tc, ns);
                            if (tc_is_builtin(ns, tc->prelude_sig_count)) tc_error(tc, t->line, 0, "'%s' is already defined", sym_name(ns));
                            else if (old) tc_error(tc, t->line, 0, "'%s' is already defined (first defined on line %d)", sym_name(ns), old->def_line);
                        }
                        /* Reconcile with prior forward `effect` declaration: if
                           `'name [sig] effect` registered a sig in user code,
                           validate the body against it. Only runs in user code —
                           the builtin/prelude handoff already relies on trust. */
                        tc_bind(tc, ns, &vt, t->line, vt.type == TC_TUPLE && i >= 2 && (toks[i-2].tag == TOK_RPAREN || toks[i-2].tag == TOK_RBRACE || (toks[i-2].tag == TOK_WORD && toks[i-2].as.sym == S_EFFECT)));
                    }
                }
            } else if (sym == S_EFFECT) {
                if (tc->sp > 0) tc->sp--;
                int be = i - 1;
                if (be >= start && toks[be].tag == TOK_RBRACKET) {
                    int bs = be + toks[be].span;
                    TypeSig sig = parse_type_annotation(toks, bs+1, be);
                    /* validate either-schema variant types: anything longer than two
                       characters must resolve as a type keyword or be referenced as a
                       type-variable elsewhere in the signature. Catches typos like
                       `{'ok 'ist 'no ()}` while still permitting single-letter 'a/'b/'k/'v
                       conventions used by the builtin polymorphic sigs. */
                    for (int si = 0; si < sig.slot_count; si++) {
                        TypeSlot *sl = &sig.slots[si];
                        for (int e = 0; e < sl->either_count; e++) {
                            uint32_t tv = sl->either_tvars[e];
                            if (!tv || sl->either_types[e] != TC_NONE) continue;
                            const char *nm = sym_name(tv); int nl = (int)strlen(nm);
                            if (nl <= 2) continue;
                            int seen = 0;
                            for (int sj = 0; sj < sig.slot_count && !seen; sj++) {
                                if (sig.slots[sj].type_var == tv) seen = 1;
                                for (int e2 = 0; e2 < sig.slots[sj].either_count && !seen; e2++)
                                    if (sj != si || e2 != e) if (sig.slots[sj].either_tvars[e2] == tv) seen = 1;
                            }
                            if (!seen) tc_error(tc, toks[bs].line, 0, "unknown type '%s' in 'either' schema — expected a type keyword (int, list, …) or a type variable ('a, 'b) referenced elsewhere in the signature", nm);
                        }
                    }
                    if (tc->sp >= 1 && tc->data[tc->sp-1].type == TC_TUPLE) {
                        /* `(body) [sig] effect 'name let` registers sig for name. Only a body written right before
                           the signature was checked against it. */
                        if (i+2 < total_count && toks[i+1].tag == TOK_SYM
                            && toks[i+2].tag == TOK_WORD && toks[i+2].as.sym == S_LET) {
                            if (i >= tc->user_start && !(bs-1 >= start && toks[bs-1].tag == TOK_RPAREN))
                                tc_error(tc, t->line, 0, "the type of '%s' must follow the body it describes, written in place: `(body) [...] effect '%s let`. Here the body comes from elsewhere, so the checker cannot check it against the type.", sym_name(toks[i+1].as.sym), sym_name(toks[i+1].as.sym));
                            typesig_register(toks[i+1].as.sym, &sig);
                        }
                    } else if (tc->sp >= 1 && tc->data[tc->sp-1].type == TC_SYM) {
                        /* BUILTIN_TYPES prim registration: `'name [sig] effect` (no def). */
                        typesig_register(tc->data[tc->sp-1].sym_id, &sig);
                        if (i >= tc->user_start) { if (tc->fwd_n == FWD_MAX) die("type checker: more than %d declared words", FWD_MAX);
                            tc->fwd[tc->fwd_n] = tc->data[tc->sp-1].sym_id; tc->fwd_line[tc->fwd_n++] = t->line; }
                        tc->sp--;
                    }
                }
            } else if (sym == S_CHECK) {
                if (i >= 1 && toks[i-1].tag == TOK_WORD) {
                    TypeConstraint exp = parse_constraint(sym_name(toks[i-1].as.sym));
                    if (exp != TC_NONE && tc->sp > 0 && tc->data[tc->sp-1].type != exp && tc->data[tc->sp-1].type != TC_NONE)
                        tc_error(tc, t->line, 0, "'check' expected %s, got %s", constraint_name(exp), constraint_name(tc->data[tc->sp-1].type));
                    if (tc->unknown_count > 0 && tc->unknowns[tc->unknown_count-1].sym == toks[i-1].as.sym) tc->unknown_count--;
                }
            } else if (sym == S_CASE) {
                /* Exhaustiveness: when scrutinee has a declared union, require every variant to appear as a clause. */
                if (tc->sp >= 3 && tc->data[tc->sp-3].type == TC_TAGGED && tc->data[tc->sp-3].tvar_id > 0) {
                    int uid = tc->tvars[tvar_find(tc, tc->data[tc->sp-3].tvar_id)].union_id;
                    if (uid > 0 && !tc->unions[uid-1].inferred) {
                        int brace = tc_find_brace_before(toks, i);
                        if (brace >= 0) {
                            uint32_t csyms[UNION_VARIANTS_MAX]; int cc = 0;
                            for (int j = brace+1, cl = brace+toks[brace].span, key = 1; j < cl; key = !key) {
                                if (key && toks[j].tag == TOK_SYM) {
                                    if (cc == UNION_VARIANTS_MAX) die("case: more than %d clauses", UNION_VARIANTS_MAX);
                                    csyms[cc++] = toks[j].as.sym;
                                }
                                j += toks[j].span > 0 ? toks[j].span + 1 : 1;
                            }
                            UnionDef *ud = &tc->unions[uid-1];
                            int has_linear = 0;
                            for (int v = 0; v < ud->count; v++)
                                if (ud->types[v] == TC_BOX) { has_linear = 1; break; }
                            for (int v = 0; v < ud->count; v++) {
                                int found = 0;
                                for (int k = 0; k < cc; k++) if (csyms[k] == ud->syms[v]) { found = 1; break; }
                                if (!found) {
                                    if (has_linear)
                                        tc_error(tc, t->line, 0, "'case' missing clause for '%s variant — union carries a linear variant, exhaustiveness is required", sym_name(ud->syms[v]));
                                    else
                                        tc_error(tc, t->line, 0, "'case' missing clause for '%s variant", sym_name(ud->syms[v]));
                                }
                            }
                        }
                    }
                }
                tc_check_word(tc, sym, t->line);
            } else if (sym == S_THEN && tc->sp - tc->sp_floor >= 2 && tc->data[tc->sp-1].type == TC_TUPLE && tc->data[tc->sp-1].effect_idx >= 0
                       && tc->data[tc->sp-2].tvar_id > 0
                       && tc->tvars[tvar_find(tc, tc->data[tc->sp-2].tvar_id)].vrow) {
                /* tagged (body) then runs body on the 'ok payload. */
                TupleEffect *te = &tc->effects[tc->data[tc->sp-1].effect_idx];
                int g = row_find(tc, tc->tvars[tvar_find(tc, tc->data[tc->sp-2].tvar_id)].vrow, S_OK), outs[16];
                if (!g) { g = tvar_fresh(tc); tc->tvars[g].unknown = 1; }
                AbstractType pay = {0}; pay.tvar_id = g; pay.type = tvar_resolve(tc, g); pay.effect_idx = -1; pay.source_line = t->line;
                int k = tc_trial(tc, te, &pay, 1, outs, 16, "then", t->line);
                int iv = tc->tvars[tvar_find(tc, tc->data[tc->sp-2].tvar_id)].vrow, no = row_find(tc, iv, S_NO);
                if (te->consumed > 1) { tc->sp -= 2; tc_forget(tc); tc->sp += 2; }
                uint32_t reads = te->reads; te->reads = 0;
                tc_check_word(tc, sym, t->line);
                te->reads = reads;
                /* The result carries the body's payloads, and the 'no payload it passes through. */
                if (k == 1 && outs[0] > 0 && tc->tvars[tvar_find(tc, outs[0])].vrow && tc->data[tc->sp-1].tvar_id > 0) {
                    int m = row_copy(tc, tc->tvars[tvar_find(tc, outs[0])].vrow, t->line), g2 = no ? row_find(tc, m, S_NO) : 0;
                    if (no && g2) tvar_unify(tc, g2, no); else if (no) row_put(tc, m, S_NO, no);
                    tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].vrow = m;
                }
            } else if (sym == S_CAT && tc->sp - tc->sp_floor >= 2 && tc_value_tvar(tc, &tc->data[tc->sp-1]) && tc_value_tvar(tc, &tc->data[tc->sp-2])
                       && tvar_resolve(tc, tc->data[tc->sp-1].tvar_id) == TC_REC && tvar_resolve(tc, tc->data[tc->sp-2].tvar_id) == TC_REC) {
                /* Two records: every pair of both, and `at` reads a key's last one, so the right record wins. It may
                   hold keys the checker does not see, so a key only the left one shows is there, of unknown type. */
                int fa = rec_flat(tc, tc->data[tc->sp-2].tvar_id), fb = rec_flat(tc, tc->data[tc->sp-1].tvar_id), m = 0;
                if (fa && fb) { m = row_new(tc, t->line);
                    for (int k = 0; k < tc->rows[fa].n; k++) row_put(tc, m, tc->rows[fa].key[k], tvar_fresh(tc));
                    for (int k = 0; k < tc->rows[fb].n; k++) row_put(tc, m, tc->rows[fb].key[k], tc->rows[fb].tv[k]); }
                tc->sp -= 2; tc_push_rec(tc, m, t->line);
            } else if (sym == S_NTH && tc->sp - tc->sp_floor >= 2 && tc->data[tc->sp-2].sym_id) {
                /* 'name i nth reads an element of the list bound to name. */
                TCBinding *b = tc_lookup(tc, tc->data[tc->sp-2].sym_id);
                int el = b && b->atype.tvar_id > 0 && (b->atype.type == TC_LIST || tvar_resolve(tc, b->atype.tvar_id) == TC_LIST) ? tvar_content(tc, b->atype.tvar_id, TC_LIST) : 0;
                tc_check_word(tc, sym, t->line);
                if (el > 0 && tc->sp > 0 && tc->data[tc->sp-1].tvar_id > 0) {
                    int r = tvar_find(tc, tc->data[tc->sp-1].tvar_id), g = tc->tvars[r].vrow ? row_find(tc, tc->tvars[r].vrow, S_OK) : 0;
                    if (g) tvar_unify(tc, g, el);
                    else { if (!tc->tvars[r].vrow) tc->tvars[r].vrow = row_new(tc, t->line); row_put(tc, tc->tvars[r].vrow, S_OK, el); }
                }
            } else if ((sym == S_REPEAT || sym == S_FILTER) && tc->sp - tc->sp_floor >= 2 && tc->data[tc->sp-1].type == TC_TUPLE && tc->data[tc->sp-1].effect_idx >= 0) {
                /* x n (body) repeat runs body on x n times; list (p) filter runs p on each element. */
                TupleEffect *te = &tc->effects[tc->data[tc->sp-1].effect_idx];
                AbstractType top2[2] = {tc->data[tc->sp-2], tc->data[tc->sp-1]};
                if (sym == S_REPEAT) {
                    /* x n (body) repeat leaves x as the loop leaves it: the flow is the whole effect. */
                    tc->sp -= 2;
                    tc_loop_records(tc, NULL, te, "repeat", t->line);
                    break;
                } else if ((top2[0].type == TC_LIST || top2[0].type == TC_NONE) && top2[0].tvar_id > 0 && tvar_content(tc, top2[0].tvar_id, TC_LIST) > 0) {
                    int el = tvar_content(tc, top2[0].tvar_id, TC_LIST), outs[2]; AbstractType a = {0};
                    a.type = tvar_resolve(tc, el); a.tvar_id = el; a.effect_idx = -1; a.source_line = t->line;
                    tc_trial(tc, te, &a, 1, outs, 2, "filter", t->line);
                    if (te->consumed > 1) { tc->sp -= 2; tc_forget(tc); tc->sp += 2; }
                } else if (te->reads) tc_escape(tc, (te)->reads, "filter", t->line);
                /* The flow above checked the body's reads; the call itself only passes it on. */
                uint32_t reads = te->reads; te->reads = 0;
                tc_check_word(tc, sym, t->line);
                te->reads = reads;
            } else if (sym == S_AT || sym == S_EDIT) {
                /* `at must` and `edit must` run as one word, as build_tuple fuses them. */
                int must = i+1 < end && toks[i+1].tag == TOK_WORD && toks[i+1].as.sym == S_MUST;
                if (sym == S_AT) tc_at(tc, t->line, must); else tc_edit(tc, t->line, must);
                i += must;
            } else if (sym == S_REC || sym == S_INTO) {
                /* rec has no keys; into adds one, or replaces its value. */
                int rtv = 0, vtv = 0; uint32_t k = 0;
                if (sym == S_INTO && tc->sp - tc->sp_floor >= 3) {
                    rtv = tc->data[tc->sp-3].tvar_id; k = tc->data[tc->sp-1].sym_id; vtv = tc_value_tvar(tc, &tc->data[tc->sp-2]);
                }
                tc_check_word(tc, sym, t->line);
                if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_REC) {
                    tc->sp--;
                    if (sym == S_REC) tc_push_rec(tc, row_new(tc, t->line), t->line); else tc_push_rec_with(tc, rtv, k, vtv, t->line);
                }
            } else {
                /* The tags a word's output can carry: its body's, or the literal `'x tag` names,
                   or for `then` and `pthen` its body's plus the 'no they pass on. */
                TCBinding *wb = tc_lookup(tc, sym); uint32_t no = S_NO;
                int tags = wb && wb->atype.type == TC_TUPLE && wb->atype.effect_idx >= 0 ? tc->effects[wb->atype.effect_idx].out_tags : 0;
                if (sym == S_TAG && tc->sp > 0 && tc->data[tc->sp-1].type == TC_SYM) tags = tc_tags_of(tc, &tc->data[tc->sp-1].sym_id, 1);
                if (sym == S_THEN || sym == S_PTHEN) tags = tc->sp > 0 && tc->data[tc->sp-1].effect_idx >= 0
                    ? tc_tags_merge(tc, tc->effects[tc->data[tc->sp-1].effect_idx].out_tags, tc_tags_of(tc, &no, 1)) : 0;
                /* A tagged value carries its payload's type under its tag; must gives the 'ok one back. */
                int ptv = 0, mtv = 0; uint32_t ptag = 0;
                if (sym == S_TAG && tc->sp - tc->sp_floor >= 2 && tc->data[tc->sp-1].sym_id) { ptag = tc->data[tc->sp-1].sym_id; ptv = tc_value_tvar(tc, &tc->data[tc->sp-2]); }
                else if ((sym == S_OK || sym == S_NO) && tc->sp > tc->sp_floor) { ptag = sym; ptv = tc_value_tvar(tc, &tc->data[tc->sp-1]); }
                else if (sym == S_MUST && tc->sp > tc->sp_floor) mtv = tc->data[tc->sp-1].tvar_id;
                tc_check_word(tc, sym, t->line);
                if (ptag && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TAGGED && tc->data[tc->sp-1].tvar_id > 0) {
                    int w = row_new(tc, t->line); row_put(tc, w, ptag, ptv); tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].vrow = w;
                }
                if (mtv > 0) { int vw = tc->tvars[tvar_find(tc, mtv)].vrow, g = vw ? row_find(tc, vw, S_OK) : 0;
                    if (g && tc->sp > 0) { tc->sp--; tc_push_tvar(tc, g, t->line); } }
                if (sym == S_PARSE_HTTP && tc->sp > 0 && tc->data[tc->sp-1].tvar_id > 0) {
                    int w = row_new(tc, t->line); row_put(tc, w, S_OK, tc_http_rec(tc, t->line));
                    tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].vrow = w;
                }
                if (tc->sp > 0 && !tc_tags(tc, &tc->data[tc->sp-1])) tc_set_tags(tc, &tc->data[tc->sp-1], tags);
            }
            break;
        }
        default: break;
        }
        if (tc->body_depth == 0 && tc->effect_count > EFFECT_MAX/2) {
            int ml = 0;
            for (int s = 0; s < tc->sp; s++) if (tc->data[s].effect_idx >= ml) ml = tc->data[s].effect_idx + 1;
            for (int b = 0; b < tc->bind_count; b++) if (tc->bindings[b].atype.effect_idx >= ml) ml = tc->bindings[b].atype.effect_idx + 1;
            for (int h = 0; h < tc->handler_count; h++) if (tc->handlers[h].effect >= ml) ml = tc->handlers[h].effect + 1;
            int grew = 1; while (grew) { grew = 0;
                for (int e = 0; e < ml; e++) { int oe = tc->effects[e].out_effect;
                    if (oe >= ml && oe < tc->effect_count) { ml = oe + 1; grew = 1; } } }
            /* A type that names a body being freed now names some body the checker cannot name. */
            if (tc->effect_count - ml >= 256) {
                for (int t = 1; t < tc->tvar_count; t++) if (tc->tvars[t].code > ml) tc->tvars[t].code = -1;
                tc->effect_count = ml;
            }
        }
    }
}
#ifdef SLAP_NEXT
/* ==== The new checker (todo.md step 4), built with -DSLAP_NEXT beside the old one until the switch. ==== */
/* ==== TYPES: inference by unification ====
   A type is a term in one pool: a value, a stack (its top and the rest), a record row, a label or a
   tag set. A variable is a term that union-find binds to another. A body's type is its stack effect:
   a function from the stack it takes to the stack it leaves, whose untouched rest is a variable, so a
   word works on any stack below what it touches. Levels decide what a word's type generalizes. */
enum { K_VAR, K_INT, K_FLOAT, K_SYM, K_LIST, K_DICT, K_BOX, K_SOCK, K_FN, K_REC, K_RES, K_TAG,
       K_SVAR, K_SNIL, K_SCONS, K_RVAR, K_RNIL, K_REXT, K_LSYM, K_TVAR, K_TNIL, K_TEXT, K_PRE, K_ABS };
/* A symbol's type is K_SYM. Its `a` is the K_LSYM of the literal it came from, or 0 once two different
   symbols meet; only `{...}` keys and `nth` read it, and they refuse 0. Record keys are K_LSYM. */
/* A row field is K_PRE (the record has the key, of type a) or K_ABS (it has not), or a variable for
   either. A closed row (K_RNIL) has no other key. `into` sets a key whether or not the record had it,
   as the runtime replaces a key it finds. */
/* What a value variable must be: protocols a word asks of its inputs. */
/* copy: the value may be copied, dropped, bound or stored. A box is not, nor a result or tag that
   holds one; the stack carries it from the word that makes it to the word that frees it. */
enum { P_NUM = 1, P_ORD = 2, P_SEQ = 4, P_SIZED = 8, P_SEMI = 16, P_COPY = 32 };
/* rigid: a signature's variable while a body is checked against it. sealed: the stack below a body
   that must not reach it (`each`, `edit`); instances keep it. */
/* named: a K_SYM whose name nth read a list by; it may not meet another symbol. */
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
static int ty_isvar(int k) { return k == K_VAR || k == K_SVAR || k == K_RVAR || k == K_TVAR; }
/* A variable links to what it is bound to; a structure links to one it was unified with. */
static int ty_find(int t) {
    for (int hops = 0; ty[t].link; hops++) {
        if (hops == ty_n) die("type checker bug: a type links in a cycle (%d hops)", hops);
        t = ty[t].link;
    }
    return t;
}
/* The next term along a stack (b) or a row or tag set (c); a chain longer than the pool is a cycle. */
static int ty_rest(int x, int *hops) {
    if (++*hops > ty_n) die("type checker bug: a stack or row links in a cycle (%d hops)", *hops);
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
    case K_LIST: return P_SEQ | P_SIZED | P_SEMI | P_COPY;
    case K_DICT: return P_SIZED | P_COPY;
    case K_REC: return P_SIZED | P_SEMI | P_COPY;
    default: return 0;
    }
}
static const char *ty_prot_name(int p) { return p & P_NUM ? "num" : p & P_ORD ? "ord" : p & P_SEQ ? "seq" : p & P_SIZED ? "sized" : p & P_SEMI ? "semigroup" : "copyable"; }

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
    case K_SYM: if (ty[t].a) snprintf(out, cap, "'%s", sym_name(ty[ty[t].a].sym)); else snprintf(out, cap, "sym"); return;
    case K_LIST: { int e = ty_find(ty[t].a); if (ty[e].kind == K_INT) { snprintf(out, cap, "str"); return; } ty_show(a, sizeof a, e, depth + 1); snprintf(out, cap, "%s list", a); return; }
    case K_DICT: ty_show(a, sizeof a, ty[t].a, depth + 1); snprintf(out, cap, "%s dict", a); return;
    case K_BOX: ty_show(a, sizeof a, ty[t].a, depth + 1); snprintf(out, cap, "%s box", a); return;
    case K_SOCK: snprintf(out, cap, "socket"); return;
    case K_TVAR: snprintf(out, cap, "tagged .."); return;
    case K_RVAR: { char v[16]; ty_print_var(v, sizeof v, t, '\''); snprintf(out, cap, "{| %s}", v); return; }
    case K_FN: ty_show_stack(a, sizeof a, ty[t].a, depth); ty_show_stack(b, sizeof b, ty[t].b, depth); snprintf(out, cap, "( %s -> %s )", a, b); return;
    case K_RES: ty_show(a, sizeof a, ty[t].a, depth + 1); ty_show(b, sizeof b, ty[t].b, depth + 1); snprintf(out, cap, "{'ok %s 'no %s} either", a, b); return;
    case K_TAG: case K_TEXT: case K_TNIL: {
        size_t len = 0; int r = ty[t].kind == K_TAG ? ty_find(ty[t].a) : t; ty_put(out, cap, &len, "tagged");
        for (int hops = 0; ty[r].kind == K_TEXT; r = ty_find(ty[r].c), hops++) { if (hops == ty_n) die("type checker bug: a tag set links in a cycle"); ty_put(out, cap, &len, " '%s", sym_name(ty[r].sym)); }
        if (ty[r].kind == K_TVAR) ty_put(out, cap, &len, " ..");
        return; }
    case K_REC: {
        size_t len = 0; int r = ty_find(ty[t].a), n = 0; ty_put(out, cap, &len, "{");
        for (int hops = 0; ty[r].kind == K_REXT; hops++) {
            if (hops == ty_n) die("type checker bug: a row links in a cycle");
            int f = ty_find(ty[r].b);
            if (ty[f].kind != K_ABS) {
                if (ty[f].kind == K_PRE) ty_show(a, sizeof a, ty[f].a, depth + 1); else { ty_show(a, sizeof a, f, depth + 1); strncat(a, "?", sizeof a - strlen(a) - 1); }
                ty_put(out, cap, &len, "%s'%s %s", n ? " " : "", sym_name(ty[ty[r].a].sym), a); n++;
            }
            r = ty_find(ty[r].c);
        }
        if (ty[r].kind == K_RVAR) { char v[16]; ty_print_var(v, sizeof v, r, '\''); ty_put(out, cap, &len, "%s| %s", n ? " " : "", v); }
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
static int ty_bind(int v, int t) {
    if (ty[v].sealed && ty[t].kind == K_SVAR && ty[t].sealed && !ty[v].rigid && !ty[t].rigid) { ty[v].link = t; return 0; }
    if (ty_fixed(v) && !(ty_isvar(ty[t].kind) && !ty_fixed(t))) {
        if (ty[v].sealed || (ty_isvar(ty[t].kind) && ty[t].sealed)) { snprintf(ty_why, sizeof ty_why, "the body may use only the values it is given, not the stack below them"); return 1; }
        char s[256]; ty_show(s, sizeof s, t, 0); snprintf(ty_why, sizeof ty_why, "a type the signature leaves open is %s here", s); return 1; }
    if (ty_fixed(v)) { int x = v; v = t; t = x; }
    int occ = ty_occurs(v, t, ty[v].level);
    if (occ == 2) { snprintf(ty_why, sizeof ty_why, "a type the signature leaves open would have to be a type from outside the body"); return 1; }
    if (occ) {
        int n = 0; for (int x = ty_find(t); ty[x].kind == K_SCONS && n < ty_n; x = ty_find(ty[x].b)) n++;
        if (ty[v].kind == K_SVAR && n) snprintf(ty_why, sizeof ty_why, "one path leaves %d more value%s on the stack than the other, so a branch, clause, loop pass or recursive call changes the stack's depth", n, n == 1 ? "" : "s");
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
/* Recursion follows nesting; a stack or a row is a chain, which the loop walks. */
static int ty_unify_at(int a, int b, int depth) {
  for (;;) {
    if ((depth & 255) == 255) c_stack_check("while unifying types");
    a = ty_find(a); b = ty_find(b);
    if (a == b) return 0;
    if (ty_isvar(ty[a].kind)) return ty_bind(a, b);
    if (ty_isvar(ty[b].kind)) return ty_bind(b, a);
    if (ty[a].kind == K_RNIL && ty[b].kind == K_REXT) { int x = a; a = b; b = x; }
    /* two different symbols meeting leave plain sym: the value may be either */
    if (ty[a].kind == K_SYM && ty[b].kind == K_SYM) {
        if (!(ty[a].a && ty[b].a && ty[ty[a].a].sym == ty[ty[b].a].sym)) {
            if (ty[a].named || ty[b].named) { int n = ty[a].named ? a : b, o = n == a ? b : a;
                snprintf(ty_why, sizeof ty_why, "nth reads the list named '%s by this symbol, so it must be '%s, but it may be %s%s", sym_name(ty[ty[n].a].sym), sym_name(ty[ty[n].a].sym), ty[o].a ? "'" : "another symbol", ty[o].a ? sym_name(ty[ty[o].a].sym) : "");
                return 1; }
            ty[a].a = ty[b].a = 0; }
        ty[b].named |= ty[a].named; ty[a].link = b; return 0; }
    if (ty[a].kind != ty[b].kind && !(ty[a].kind == K_REXT && ty[b].kind == K_RNIL)) {
        char s1[256], s2[256]; ty_show(s1, sizeof s1, a, 0); ty_show(s2, sizeof s2, b, 0);
        if (ty[a].kind == K_SNIL || ty[b].kind == K_SNIL) snprintf(ty_why, sizeof ty_why, "the stack is shorter than this needs");
        else snprintf(ty_why, sizeof ty_why, "%s is not %s", s2, s1);
        return 1;
    }
    switch (ty[a].kind) {
    case K_REXT: case K_TEXT: {
        /* Take a's first entry out of b, then meet the rests. If taking it binds a's own tail, the two
           rows differ only in order around one shared tail, and no finite row satisfies both. */
        int f = 0, rest, tail = ty_tail(a);
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
        ty[a].link = b;
        if (!na) return 0;
        a = na; b = nb; continue;
    }
    }
  }
}
static int ty_unify(int a, int b) { ty_why[0] = 0; return ty_unify_at(a, b, 0); }
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
            if (ty_memo_n == ty_memo_cap) { ty_memo_cap = ty_memo_cap ? 2*ty_memo_cap : 256; ty_memo_to = realloc(ty_memo_to, (size_t)ty_memo_cap * sizeof(int));
                if (!ty_memo_to) die("type checker: out of memory for %d type variables", ty_memo_cap); }
            ty_memo_to[ty_memo_n++] = r;
        }
    } else if (ty[t].kind == K_SYM) { r = ty_new(K_SYM, ty[t].a, 0, 0); ty[r].named = ty[t].named; }
    else if (ty[t].a || ty[t].b || ty[t].c) {
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
    "'print ( 'a -> ) 'assert ( int -> ) 'millis ( -> int ) 'datetime ( -> int list ) 'random ( int -> int ) 'halt ( ..s -> ..t ) 'isheadless ( -> int )\n"
    "'apply ( ..s ( ..s -> ..t ) -> ..t ) 'dip ( ..s 'x ( ..s -> ..t ) -> ..t 'x )\n"
    "'if ( ..s int ( ..s -> ..t ) ( ..s -> ..t ) -> ..t ) 'while ( ..a ( ..a -> ..b int ) ( ..b -> ..a ) -> ..b )\n"
    "'each ( ..s 'a list ( ..!r 'a -> ..!r 'b ) -> ..s 'b list ) 'fold ( ..s 'a list 'b ( ..!r 'b 'a -> ..!r 'b ) -> ..s 'b )\n"
    "'list ( -> 'a list ) 'len ( 'a sized -> int ) 'push ( 'a list 'a -> 'a list ) 'pop ( 'a list -> 'a list {'ok 'a 'no ()} either )\n"
    "'get ( 'a list int -> {'ok 'a 'no ()} either ) 'peek ( 'a list int -> 'a list {'ok 'a 'no ()} either )\n"
    "'set ( 'a list int 'a -> {'ok 'a list 'no ()} either ) 'cat ( 'a semigroup 'a semigroup -> 'a semigroup ) 'reverse ( 'a list -> 'a list )\n"
    "'take-n ( 'a list int -> 'a list ) 'drop-n ( 'a list int -> 'a list ) 'range ( int int -> int list ) 'sort ( 'a ord list -> 'a ord list )\n"
    "'index-of ( 'a list 'a -> {'ok int 'no ()} either ) 'zip ( 'a list 'a list -> 'a list list )\n"
    "'str-find ( str str -> {'ok int 'no ()} either ) 'str-split ( str str -> str list )\n"
    "'rec ( -> {} )\n"
    "'must ( {'ok 'a 'no 'b} either -> 'a )\n"
    "'pthen ( ..s {'ok 'a 'no 'b} either 'd copy ( ..s 'a -> ..s 'd {'ok 'c 'no 'b} either ) -> ..s 'd {'ok 'c 'no 'b} either )\n"
    "'box ( 'a -> 'a box ) 'free ( 'a box -> ) 'mutate ( ..s 'a box ( ..!r 'a -> ..!r 'b ) -> ..s 'b box )\n"
    "'dict ( -> 'a dict ) 'insert ( 'a dict str 'a -> 'a dict ) 'of ( 'a dict str -> 'a dict {'ok 'a 'no str} either )\n"
    "'remove ( 'a dict str -> 'a dict ) 'dict-keys ( 'a dict -> 'a dict str list )\n"
    "'read ( str -> {'ok str 'no str} either ) 'write ( str str -> {'ok int 'no str} either ) 'ls ( str -> {'ok str list 'no str} either )\n"
    "'args ( -> str list ) 'parse-http ( str -> {'ok {'status int 'headers {'key str 'value str} list 'body str} 'no str} either )\n"
    /* a socket is its own type: the runtime keeps it in a box, but free, lend and mutate must not reach it */
    "'tcp-connect ( str int -> {'ok socket 'no str} either ) 'tcp-send ( socket str -> socket {'ok int 'no str} either )\n"
    "'tcp-recv ( socket int -> socket {'ok str 'no str} either ) 'tcp-close ( socket -> ) 'tcp-listen ( int -> {'ok socket 'no str} either )\n"
    "'tcp-accept ( socket -> socket {'ok socket 'no str} either )\n"
    "'clear ( int -> ) 'pixel ( int int int -> ) 'fill-rect ( int int int int int -> )\n"
    /* each and fold on a dict: the checker picks these when it sees a dict below the body */
    "'each-dict ( ..s 'a dict ( ..!r {'key str 'value 'a} -> ..!r 'b ) -> ..s 'b dict )\n"
    "'fold-dict ( ..s 'a dict 'b ( ..!r 'b {'key str 'value 'a} -> ..!r 'b ) -> ..s 'b )\n";

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
    return !strcmp(w, "num") ? P_NUM : !strcmp(w, "ord") ? P_ORD : !strcmp(w, "seq") ? P_SEQ : !strcmp(w, "sized") ? P_SIZED : !strcmp(w, "semigroup") ? P_SEMI : !strcmp(w, "copy") ? P_COPY : 0;
}
static int ty_parse_fn(Token *toks, int open, int close, TyNames *nm, int rest);
static int ty_parse(Token *toks, int *i, int end, TyNames *nm) {
    if (*i >= end) die("type annotation: a type is missing at line %d", toks[end-1].line);
    Token *t = &toks[*i]; int base = 0;
    if (t->tag == TOK_WORD) {
        const char *w = sym_name(t->as.sym); int p = ty_prot_word(t);
        if (!strcmp(w, "int")) base = ty_new(K_INT, 0, 0, 0);
        else if (!strcmp(w, "float")) base = ty_new(K_FLOAT, 0, 0, 0);
        else if (!strcmp(w, "sym")) base = ty_new(K_SYM, 0, 0, 0);
        else if (!strcmp(w, "str")) base = ty_new(K_LIST, ty_new(K_INT, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "tagged")) base = ty_new(K_TAG, ty_new(K_TVAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "rec")) base = ty_new(K_REC, ty_new(K_RVAR, 0, 0, 0), 0, 0);
        else if (!strcmp(w, "tuple")) base = ty_new(K_FN, ty_new(K_SVAR, 0, 0, 0), ty_new(K_SVAR, 0, 0, 0), 0);
        else if (!strcmp(w, "list")) base = ty_new(K_LIST, ty_new(K_VAR, 0, 0, 0), 0, 0);
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
        base = ty_parse_fn(toks, *i, *i + t->span, nm, 0); *i += t->span + 1;
    } else if (t->tag == TOK_LBRACE) {
        int close = *i + t->span, j = *i + 1, rest = 0; uint32_t keys[64]; int types[64], n = 0;
        while (j < close) {
            if (ty_word_is(&toks[j], "|")) { if (j + 1 >= close || toks[j+1].tag != TOK_SYM) die("type annotation: '|' needs a row name after it at line %d", toks[j].line);
                rest = ty_named(nm, toks[j+1].as.sym, K_RVAR); j += 2; continue; }
            if (toks[j].tag != TOK_SYM) die("type annotation: a record or either type takes 'name type pairs, at line %d", toks[j].line);
            if (n == 64) die("type annotation: more than 64 fields at line %d", toks[j].line);
            keys[n] = toks[j].as.sym; j++; types[n] = ty_parse(toks, &j, close, nm);
            /* `'p field`: a record field that may be present or absent (the table's `into`) */
            if (j < close && ty_word_is(&toks[j], "field")) j++; else types[n] = ty_new(K_PRE, types[n], 0, 0);
            n++;
        }
        *i = close + 1;
        if (*i < end && ty_word_is(&toks[*i], "either")) {
            (*i)++; int okno = 1, ok = 0, no = 0;
            for (int k = 0; k < n; k++) { types[k] = ty[types[k]].a; if (keys[k] == S_OK) ok = types[k]; else if (keys[k] == S_NO) no = types[k]; else okno = 0; }
            if (okno) base = ty_new(K_RES, ok ? ok : ty_new(K_VAR, 0, 0, 0), no ? no : ty_new(K_VAR, 0, 0, 0), 0);
            else { int row = ty_new(K_TNIL, 0, 0, 0);
                for (int k = n - 1; k >= 0; k--) { if (ty_unify(types[k], ty_tag_payload(keys[k]))) die("type annotation: tag '%s here conflicts with its payload elsewhere: %s", sym_name(keys[k]), ty_why);
                    row = ty_new(K_TEXT, 0, 0, row); ty[row].sym = keys[k]; }
                base = ty_new(K_TAG, row, 0, 0); }
        } else {
            int row = rest ? rest : ty_new(K_RNIL, 0, 0, 0);
            for (int k = 0; k < n; k++) row = ty_new(K_REXT, ty_sym(K_LSYM, keys[k]), types[k], row);
            base = ty_new(K_REC, row, 0, 0);
        }
    } else die("type annotation: a type is expected at line %d", t->line);
    for (; *i < end && toks[*i].tag == TOK_WORD; (*i)++) {
        const char *w = sym_name(toks[*i].as.sym);
        if (!strcmp(w, "list")) base = ty_new(K_LIST, base, 0, 0);
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
        while (j < end) s = ty_new(K_SCONS, ty_parse(toks, &j, end, nm), s, 0);
        if (side) out = s; else in = s;
        j = dash + 1;
    }
    return ty_new(K_FN, in, out, 0);
}
/* A program signature: `[ type own in  type move out ... ]`, one slot per value, bottom first. */
static int ty_parse_slots(Token *toks, int open, int close, TyNames *nm) {
    int rest = ty_new(K_SVAR, 0, 0, 0), in = rest, out = rest;
    for (int j = open + 1; j < close; ) {
        int k = j;
        while (k < close && !(toks[k].tag == TOK_WORD && (ty_word_is(&toks[k], "own") || ty_word_is(&toks[k], "lent") || ty_word_is(&toks[k], "copy") || ty_word_is(&toks[k], "move") || ty_word_is(&toks[k], "auto")))) k += toks[k].span + 1;
        if (k + 1 > close) die("type annotation: each slot ends with own/lent/copy/move/auto and in/out, at line %d", toks[j].line);
        int t = j < k ? ty_parse(toks, &j, k, nm) : ty_new(K_VAR, 0, 0, 0);
        /* a lent or copy slot is copyable; an own, move or auto one may hold a box */
        if ((ty_word_is(&toks[k], "lent") || ty_word_is(&toks[k], "copy")) && ty[ty_find(t)].kind == K_VAR) ty[ty_find(t)].prot |= P_COPY;
        if (j < k) die("type annotation: '%s' is not part of a type, at line %d", toks[j].tag == TOK_WORD ? sym_name(toks[j].as.sym) : "this", toks[j].line);
        if (ty_word_is(&toks[k+1], "in")) in = ty_new(K_SCONS, t, in, 0);
        else if (ty_word_is(&toks[k+1], "out")) out = ty_new(K_SCONS, t, out, 0);
        else die("type annotation: a slot ends with in or out, at line %d", toks[k].line);
        j = k + 2;
    }
    return ty_new(K_FN, in, out, 0);
}
static void ty_held_copy(int t);
/* A scheme: a signature parsed at a deeper level and generalized. */
static int ty_scheme_slots(Token *toks, int open, int close) {
    TyNames nm = {0}; ty_level++;
    int t = ty_parse_slots(toks, open, close, &nm); ty_level--; ty_held_copy(t); ty_generalize(t); return t;
}
/* A signature instance whose variables only stand for themselves: a body must work for all of them,
   including every stack a body type in it names. ty_unrigid frees them once the check is done. */
static int *ty_rigid_vars, ty_rigid_n, ty_rigid_cap;
static int ty_rigid(int scheme) {
    int t = ty_instantiate(scheme);
    if (ty_memo_n > ty_rigid_cap) { ty_rigid_cap = ty_memo_n; ty_rigid_vars = realloc(ty_rigid_vars, (size_t)ty_rigid_cap * sizeof(int)); if (!ty_rigid_vars) die("type checker: out of memory for %d signature variables", ty_rigid_cap); }
    for (int k = 0; k < ty_memo_n; k++) { ty_rigid_vars[k] = ty_memo_to[k]; ty[ty_memo_to[k]].rigid = 1; }
    ty_rigid_n = ty_memo_n; return t;
}
static void ty_unrigid(void) { for (int k = 0; k < ty_rigid_n; k++) ty[ty_rigid_vars[k]].rigid = 0; ty_rigid_n = 0; }

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
static uint32_t S_LEND;
static void ty_err(int line, const char *fmt, ...);
/* Words the runtime reads as forms, not bindings. */
static int ty_reserved(uint32_t sym) {
    return sym == S_LET || sym == S_EFFECT || sym == S_CHECK || sym == S_TAG || sym == S_CASE || sym == S_QUOTE || sym == S_NTH
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
static int ty_on[16], ty_on_line[16], ty_on_mouse[16], ty_on_n, ty_shown, ty_each_dict, ty_fold_dict;
static void ty_cases_settle(int level);
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
static void ty_apply(int scheme, const char *who, int line, int user) {
    int f = ty_find(ty_instantiate(scheme)), body = user;
    for (int x = ty_find(ty[f].a), hops = 0; !body && ty[x].kind == K_SCONS; x = ty_rest(x, &hops)) body = ty[ty_find(ty[x].a)].kind == K_FN;
    if (body) ty_runs(who, line);
    if (ty_unify(ty[f].a, ty_cur)) {
        /* The message shows what the word takes, from a fresh copy, and as many values from the top of the stack. */
        char want[512], before[512]; int g = ty_find(ty_instantiate(scheme)), n = 0;
        for (int x = ty_find(ty[g].a); ty[x].kind == K_SCONS && n < ty_n; x = ty_find(ty[x].b)) n++;
        ty_print_count = 0; ty_show_top(want, sizeof want, ty[g].a, n); ty_show_top(before, sizeof before, ty_cur, n);
        ty_err(line, "'%s' takes %s\n    but the stack has %s\n    %s.", who, n ? want : "nothing", before, ty_why);
        ty_cur = ty[f].b; return;
    }
    ty_cur = ty[f].b;
}
static void ty_range(Token *toks, int i, int end);
/* A word's own calls inside its body (TyBind.word == 2): each call gets a fresh type, checked against
   the body's once the body is known. */
/* A call belongs to the word it calls (its binding's index) and is made at that word's body level, so
   a word or {...} defined inside the body does not generalize it. */
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
/* The stack variable a stack type ends in. */
static int ty_stack_tail(int s) {
    s = ty_find(s);
    for (int hops = 0; ty[s].kind == K_SCONS; hops++) { if (hops == ty_n) die("type checker bug: a stack links in a cycle"); s = ty_find(ty[s].b); }
    return s;
}
static int ty_body(Token *toks, int open, int close) {
    /* a body inside a literal runs later, so it may use names bound when the program runs: the top level's */
    int saved = ty_cur, mark = tyb_n, lit = ty_literal, lit_depth = ty_lit_depth; ty_cur = ty_new(K_SVAR, 0, 0, 0); int in = ty_cur;
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
    int bt = ty_body(toks, open, close);
    /* Each call of the word inside its own body uses the body's type with its own stack rest, since a
       call may sit above more values than the body started on. The rest is fresh only when it will
       generalize; every other part of the type is the same at every call. */
    int keep = 0;
    for (int k = 0; k < ty_rec_n; k++) {
        if (ty_rec_owner[k] != mark) { ty_rec_call[keep] = ty_rec_call[k]; ty_rec_line[keep] = ty_rec_line[k]; ty_rec_owner[keep++] = ty_rec_owner[k]; continue; }
        int inst = bt, tin = ty_stack_tail(ty[bt].a), tout = ty_stack_tail(ty[bt].b);
        if (ty[tin].kind == K_SVAR && ty[tin].level > ty_level - 1) inst = ty_subst(inst, tin, ty_new(K_SVAR, 0, 0, 0));
        if (tout != tin && ty[tout].kind == K_SVAR && ty[tout].level > ty_level - 1) inst = ty_subst(inst, tout, ty_new(K_SVAR, 0, 0, 0));
        if (ty_unify(ty_rec_call[k], inst)) ty_err(ty_rec_line[k], "'%s' calls itself here with a stack its body does not take: %s.", sym_name(name), ty_why);
    }
    ty_rec_n = keep;
    ty_cases_settle(ty_level - 1);
    int want = 0;
    if (scheme) { char a[512], b[512]; want = ty_rigid(scheme);
        if (ty_unify(want, bt)) { ty_print_count = 0; ty_show(a, sizeof a, ty_instantiate(scheme), 0); ty_show(b, sizeof b, bt, 0);
            ty_err(line, "'%s' declares %s, but its body is %s: %s.", sym_name(name), a, b, ty_why); want = 0; } }
    ty_level--; tyb_n = mark;
    ty_unrigid();
    if (want) { ty_generalize(want); scheme = want; }
    else if (!scheme) { ty_generalize(bt); scheme = bt; }
    tyb_push(name, scheme, 1, line);
}
/* `x default {'tag (…) …} case`: each clause runs on its payload, or on x for a (predicate); clauses
   leave the same stack; the default takes x's place when no clause matches and some tag may miss. */
typedef struct { int tags, dflt, clauses, line; Token *toks; int open, close, scrut; } TyLater;
static TyLater ty_later[4096]; static int ty_later_n;
static void ty_case_live(int dflt, int clauses, int line, const char *why, int scrut);
static void ty_case_later(int tags, int dflt, int clauses, int line, Token *toks, int open, int close, int scrut) {
    if (ty_later_n == 4096) die("type checker: more than 4096 case forms wait on their tags");
    ty_later[ty_later_n++] = (TyLater){tags, dflt, clauses, line, toks, open, close, scrut};
}
static void ty_case(Token *toks, int open, int close, int line) {
    ty_runs("case", line); ty_at_word = "case";
    int d = ty_pop(), s = ty_pop(), rest = ty_cur, out = ty_new(K_SVAR, 0, 0, 0), tags = 0, preds = 0, okc = 0, noc = 0, res = 0, a = 0, b = 0;
    int items = 0;
    for (int j = open + 1; j < close; j += toks[j].span + 1) { if (items % 2 == 0) { if (toks[j].tag == TOK_SYM) tags++; else preds++; } items++; }
    if (items % 2) { ty_err(line, "case clauses come in pairs, a key and a body, but this list has %d items.", items); ty_cur = out; return; }
    if (tags && preds) { ty_err(line, "case clauses are all 'tags or all (predicates), not both."); ty_cur = out; return; }
    if (tags) {
        res = 1; for (int j = open + 1; j < close; j += toks[j].span + 1, j += toks[j].span + 1) if (toks[j].as.sym != S_OK && toks[j].as.sym != S_NO) res = 0;
        if (res) { a = ty_new(K_VAR, 0, 0, 0); b = ty_new(K_VAR, 0, 0, 0);
            if (ty_unify(s, ty_new(K_RES, a, b, 0))) ty_err(line, "case with 'ok/'no clauses takes a result, but this value is not one: %s.", ty_why); }
        else { int row = ty_new(K_TVAR, 0, 0, 0);
            for (int j = open + 1; j < close; j += toks[j].span + 1, j += toks[j].span + 1) { int r = ty_new(K_TEXT, 0, 0, row); ty[r].sym = toks[j].as.sym; row = r; }
            if (ty_unify(s, ty_new(K_TAG, row, 0, 0))) ty_err(line, "case with tag clauses takes a tagged value: %s.", ty_why); }
    }
    for (int j = open + 1; j < close; ) {
        int key = j; j += toks[j].span + 1;
        if (j >= close || toks[j].tag != TOK_LPAREN) { ty_err(toks[key].line, "each case clause is a key and a body in parentheses."); break; }
        int body = ty_body(toks, j, j + toks[j].span); j += toks[j].span + 1;
        if (toks[key].tag == TOK_SYM) {
            uint32_t tg = toks[key].as.sym; int p = res ? (tg == S_OK ? a : b) : ty_tag_payload(tg);
            if (tg == S_OK) okc = 1; if (tg == S_NO) noc = 1;
            int bf = ty_find(body);
            if (ty_unify(ty[bf].a, ty_new(K_SCONS, p, rest, 0))) { char ps[256]; ty_print_count = 0; ty_show(ps, sizeof ps, p, 0);
                ty_err(toks[key].line, "the clause for '%s gets its payload, %s, but its body cannot take it: %s.", sym_name(tg), ps, ty_why); }
            else if (ty_unify(ty[bf].b, out)) ty_err(toks[key].line, "the clause for '%s does not leave what the other clauses leave: %s.", sym_name(tg), ty_why);
        } else if (toks[key].tag != TOK_LPAREN) { ty_err(toks[key].line, "a case clause key is a 'tag or a (predicate), not this."); break;
        } else {
            int pred = ty_body(toks, key, key + toks[key].span);
            if (ty_unify(pred, ty_new(K_FN, ty_new(K_SCONS, s, rest, 0), ty_new(K_SCONS, ty_new(K_INT, 0, 0, 0), rest, 0), 0))) ty_err(toks[key].line, "a case predicate takes the value and leaves a flag: %s.", ty_why);
            if (ty_unify(body, ty_new(K_FN, ty_new(K_SCONS, s, rest, 0), out, 0))) ty_err(toks[key].line, "this clause does not leave what the other clauses leave: %s.", ty_why);
        }
    }
    int dflt = ty_new(K_FN, rest, ty_new(K_SCONS, d, rest, 0), 0), clauses = ty_new(K_FN, rest, out, 0);
    if (preds && ty_need(s, P_COPY)) ty_err(line, "each predicate clause gets a copy of the value, so it is copyable: %s.", ty_why);
    if (ty_need(d, P_COPY)) ty_err(line, "a clause that matches drops case's default, so the default is copyable: %s.", ty_why);
    if (tags && !res && ty[ty_find(s)].kind == K_TAG) ty_case_later(ty[ty_find(s)].a, dflt, clauses, line, toks, open, close, s);
    else if (!(res && okc && noc)) ty_case_live(dflt, clauses, line, "no clause may match", s);
    ty_cur = out;
}
/* The default of a case on the program's own tags runs only if the value may carry a tag no clause
   names. That is known once the tags the value can carry are: when the word that holds the case is
   inferred (an open set there generalizes, so callers may pass any tag), or at the end of the program. */
static void ty_case_live(int dflt, int clauses, int line, const char *why, int scrut) {
    if (ty_need(scrut, P_COPY)) ty_err(line, "case drops the value it looks at when %s and the default runs, so the value is copyable: %s. Name every tag, or use must.", why, ty_why);
    if (ty_unify(dflt, clauses)) { char a[512], b[512]; ty_print_count = 0; ty_show(a, sizeof a, dflt, 0); ty_show(b, sizeof b, clauses, 0);
        ty_err(line, "case pushes its default when %s, so the default must leave what the clauses leave.\n    The default: %s\n    The clauses: %s\n    %s.", why, a, b, ty_why); }
}
/* A closed tag set: the default runs when the set holds a tag no clause names. */
static void ty_case_closed(TyLater *c) {
    int r = ty_find(c->tags);
    for (int hops = 0; ty[r].kind == K_TEXT; r = ty_find(ty[r].c), hops++) {
        if (hops == ty_n) die("type checker bug: a tag set links in a cycle");
        int named = 0;
        for (int j = c->open + 1; j < c->close; j += c->toks[j].span + 1, j += c->toks[j].span + 1) if (c->toks[j].as.sym == ty[r].sym) named = 1;
        if (!named) { char why[128]; snprintf(why, sizeof why, "the value may be tagged '%s, which no clause names", sym_name(ty[r].sym));
            ty_case_live(c->dflt, c->clauses, c->line, why, c->scrut); return; }
    }
}
static void ty_cases_settle(int level) {
    int keep = 0;
    for (int k = 0; k < ty_later_n; k++) {
        int tail = ty_tail(ty_later[k].tags);
        if (ty[tail].kind == K_TVAR && ty[tail].level <= level) {
            /* it waits on an outer set, so its types belong to the outer level, not to this word */
            ty_occurs(-1, ty_later[k].dflt, level); ty_occurs(-1, ty_later[k].clauses, level); ty_occurs(-1, ty_later[k].scrut, level);
            ty_later[keep++] = ty_later[k]; continue; }
        if (ty[tail].kind == K_TVAR) ty_case_live(ty_later[k].dflt, ty_later[k].clauses, ty_later[k].line, "the value comes from a caller, who may pass a tag no clause names", ty_later[k].scrut);
        else ty_case_closed(&ty_later[k]);
    }
    ty_later_n = keep;
}
/* Values collected from a stack type, for literals, lend and record cat. Nothing between filling and
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
    int body = ty_pop(), bx = ty_pop(), rest = ty_cur, a = ty_new(K_VAR, 0, 0, 0), out = ty_new(K_SVAR, 0, 0, 0);
    if (ty_unify(bx, ty_new(K_BOX, a, 0, 0))) { ty_err(line, "lend takes a box: %s.", ty_why); return; }
    if (ty_unify(body, ty_new(K_FN, ty_new(K_SCONS, a, rest, 0), out, 0))) { ty_err(line, "lend's body takes the contents: %s.", ty_why); return; }
    int n = 0, s = ty_find(out);
    for (int hops = 0; s != ty_find(rest) && ty[s].kind == K_SCONS; s = ty_rest(s, &hops)) ty_item(n++, ty[s].a);
    if (s != ty_find(rest)) { ty_err(line, "lend's body may not take values below the box's contents."); return; }
    ty_cur = rest; ty_push(bx); for (int k = n - 1; k >= 0; k--) ty_push(ty_items[k]);
}
static void ty_range(Token *toks, int i, int end) {
    for (; i < end; i++) {
        Token *t = &toks[i]; int line = t->line; current_loc = LOC_PACK(t->fid, t->line, t->col);
        switch (t->tag) {
        case TOK_INT: ty_push(ty_new(K_INT, 0, 0, 0)); break;
        case TOK_FLOAT: ty_push(ty_new(K_FLOAT, 0, 0, 0)); break;
        case TOK_STRING: ty_push(ty_new(K_LIST, ty_new(K_INT, 0, 0, 0), 0, 0)); break;
        case TOK_SYM: ty_push(ty_new(K_SYM, ty_sym(K_LSYM, t->as.sym), 0, 0)); break;
        case TOK_LPAREN: {
            int close = i + t->span, nm = close + 1, sig_open = 0, sig_close = 0;
            if (nm < end && toks[nm].tag == TOK_LBRACKET && nm + toks[nm].span + 1 < end && ty_word_is(&toks[nm + toks[nm].span + 1], "effect")) {
                sig_open = nm; sig_close = nm + toks[nm].span; nm = sig_close + 2; }
            if (nm + 1 < end && toks[nm].tag == TOK_SYM && toks[nm+1].tag == TOK_WORD && toks[nm+1].as.sym == S_LET) {
                /* The prelude's signatures are the old checker's; its words are inferred here. */
                if (ty_in_prelude) sig_open = sig_close = 0;
                ty_define(toks, i, close, sig_open, sig_close, toks[nm].as.sym, line); i = nm + 1; break; }
            if (sig_open) { ty_level++; int bt = ty_body(toks, i, close), want = ty_rigid(ty_scheme_slots(toks, sig_open, sig_close));
                if (ty_unify(want, bt)) ty_err(line, "this body does not have its declared type: %s.", ty_why);
                ty_unrigid(); ty_level--; ty_occurs(-1, bt, ty_level); ty_push(bt); i = sig_close + 1; break; }
            ty_push(ty_body(toks, i, close)); i = close; break;
        }
        case TOK_LBRACKET: {
            int close = i + t->span;
            if (close + 1 < end && ty_word_is(&toks[close+1], "effect")) {
                /* `'name [sig] effect`: the word is declared before its body is written */
                if (i == 0 || toks[i-1].tag != TOK_SYM) { ty_err(line, "a signature [...] effect needs a body before it or a 'name before it."); i = close + 1; break; }
                ty_pop(); ty_redefined(toks[i-1].as.sym, line, 0);
                if (ty_literal) ty_err(line, "a [...] or {...} literal is built when the program is read, so it cannot declare words.");
                tyb_push(toks[i-1].as.sym, ty_scheme_slots(toks, i, close), 1, line); tyb[tyb_n-1].declared = !ty_in_prelude;
                if (!ty_in_prelude) ty_pending_add(ty_body_depth, 1);
                i = close + 1; break;
            }
            /* a list literal, built when the program is read: its elements have one type */
            int saved = ty_cur; ty_cur = ty_new(K_SNIL, 0, 0, 0); ty_literal++;
            c_stack_check("while checking nested literals");
            ty_range(toks, i + 1, close); ty_literal--;
            int el = ty_new(K_VAR, 0, 0, 0); ty[el].prot = P_COPY;
            for (int s = ty_find(ty_cur), k = 0, hops = 0; ty[s].kind == K_SCONS; s = ty_rest(s, &hops), k++)
                if (ty_unify(el, ty[s].a)) { ty_err(line, "a list holds values of one type, but element %d from the end is not like the others: %s.", k + 1, ty_why); break; }
            ty_cur = saved; ty_push(ty_new(K_LIST, el, 0, 0)); i = close; break;
        }
        case TOK_LBRACE: {
            int close = i + t->span;
            if (close + 1 < end && toks[close+1].tag == TOK_WORD && toks[close+1].as.sym == S_CASE) { ty_case(toks, i, close, line); i = close + 1; break; }
            /* a {...} literal, built when the program is read: a record when its values pair up with
               symbols below them, as the runtime decides, and a tuple otherwise */
            int word = close + 2 < end && toks[close+1].tag == TOK_SYM && toks[close+2].tag == TOK_WORD && toks[close+2].as.sym == S_LET;
            if (word) ty_level++;
            int saved = ty_cur; ty_cur = ty_new(K_SNIL, 0, 0, 0); ty_literal++;
            c_stack_check("while checking nested literals");
            ty_range(toks, i + 1, close); ty_literal--;
            int n = 0;
            for (int s = ty_find(ty_cur), hops = 0; ty[s].kind == K_SCONS; s = ty_rest(s, &hops)) ty_item(n++, ty[s].a);
            ty_cur = saved;
            for (int k = 0; k < n; k++) if (ty_need(ty_items[k], P_COPY)) { ty_err(line, "a {...} literal's values are copied each time it runs, so each is copyable: %s.", ty_why); break; }
            int rec = n % 2 == 0;
            for (int k = 1; rec && k < n; k += 2) if (ty[ty_find(ty_items[k])].kind != K_SYM) rec = 0;
            if (rec) { int row = ty_new(K_RNIL, 0, 0, 0);
                for (int k = n - 1; k >= 1; k -= 2) {
                    int l = ty[ty_find(ty_items[k])].a, twice = 0;
                    if (!l) { ty_err(line, "this {...} literal pairs each value with a symbol, so it is a record, but key %d is a symbol this literal computes. Write each key in the literal, as in {'name 1}.", (n - k) / 2 + 1); continue; }
                    for (int m = 1; m < k; m += 2) { int lm = ty[ty_find(ty_items[m])].a; if (lm && ty[lm].sym == ty[l].sym) twice = 1; }
                    if (twice) { ty_err(line, "this record literal has '%s twice.", sym_name(ty[l].sym)); continue; }
                    row = ty_new(K_REXT, l, ty_new(K_PRE, ty_items[k-1], 0, 0), row);
                }
                int rt = ty_new(K_REC, row, 0, 0);
                if (word) { ty_level--; ty_occurs(-1, rt, ty_level); }
                ty_push(rt); i = close; break; }
            /* a tuple of values: a body that pushes them. Written right before its 'name let, it is a word. */
            int r = ty_new(K_SVAR, 0, 0, 0), s = r; for (int k = n - 1; k >= 0; k--) s = ty_new(K_SCONS, ty_items[k], s, 0);
            int fn = ty_new(K_FN, r, s, 0);
            if (word) {
                if (ty_literal) ty_err(line, "a [...] or {...} literal is built when the program is read, so it cannot bind names: '%s' would be bound for the whole program. Define it outside the literal.", sym_name(toks[close+1].as.sym));
                ty_cases_settle(ty_level - 1); ty_level--; ty_generalize(fn);
                ty_redefined(toks[close+1].as.sym, line, 0); tyb_push(toks[close+1].as.sym, fn, 1, line); i = close + 2; break; }
            ty_push(fn); i = close; break;
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
            if (w == S_QUOTE && i > 0 && toks[i-1].tag == TOK_SYM) {
                int b = tyb_find(toks[i-1].as.sym); ty_pop();
                if (b >= 0 && ty_literal && b >= tyb_prelude) ty_err(line, "'%s' is bound when the program runs, but a [...] or {...} literal is built when it is read.", sym_name(toks[i-1].as.sym));
                if (b >= 0 && tyb[b].declared && tyb[b].depth == ty_body_depth) ty_err(line, "quote reads '%s here, but '%s is declared on line %d and not defined yet. Define it first.", sym_name(toks[i-1].as.sym), sym_name(toks[i-1].as.sym), tyb[b].line);
                if (b < 0 || !tyb_visible(b)) { ty_err(line, b < 0 ? "quote reads a name, but '%s is not bound here." : "quote reads '%s, but a body inside a [...] or {...} literal is built when the program is read, so it sees only top-level names and its own.", sym_name(toks[i-1].as.sym)); ty_push(ty_new(K_VAR, 0, 0, 0)); break; }
                ty_push(tyb[b].word == 2 ? ty_self_fn(b, line) : tyb[b].word ? ty_instantiate(tyb[b].ty) : tyb[b].ty); break;
            }
            if (w == S_TAG) {
                if (i == 0 || toks[i-1].tag != TOK_SYM) { ty_err(line, "tag needs its tag written before it, as in `5 'n tag`."); break; }
                ty_pop(); int p = ty_pop(); uint32_t tg = toks[i-1].as.sym;
                if (tg == S_OK) ty_push(ty_new(K_RES, p, ty_new(K_VAR, 0, 0, 0), 0));
                else if (tg == S_NO) ty_push(ty_new(K_RES, ty_new(K_VAR, 0, 0, 0), p, 0));
                else { if (ty_unify(ty_tag_payload(tg), p)) ty_err(line, "'%s is tagged onto a value unlike its payload elsewhere: %s.", sym_name(tg), ty_why);
                    int row = ty_new(K_TEXT, 0, 0, ty_new(K_TVAR, 0, 0, 0)); ty[row].sym = tg; ty_push(ty_new(K_TAG, row, 0, 0)); }
                break;
            }
            if (w == S_NTH) {
                int ix = ty_pop(), nm = ty_find(ty_pop()), l = ty[nm].kind == K_SYM ? ty[nm].a : 0;
                if (ty_unify(ix, ty_new(K_INT, 0, 0, 0))) ty_err(line, "nth takes an int index: %s.", ty_why);
                if (!l) { ty_err(line, "nth reads a list by its name written before the index, as in `'xs i nth`."); ty_push(ty_new(K_VAR, 0, 0, 0)); break; }
                ty[nm].named = 1;
                int b = tyb_find(ty[l].sym), el = ty_new(K_VAR, 0, 0, 0);
                if (b >= 0 && ty_literal && b >= tyb_prelude) ty_err(line, "'%s' is bound when the program runs, but a [...] or {...} literal is built when it is read.", sym_name(ty[l].sym));
                if (b < 0 || tyb[b].word || !tyb_visible(b)) { ty_err(line, "nth reads a list bound to '%s, but '%s is not a bound list here.", sym_name(ty[l].sym), sym_name(ty[l].sym)); }
                else if (ty_unify(tyb[b].ty, ty_new(K_LIST, el, 0, 0))) ty_err(line, "nth reads a list, but '%s is not one: %s.", sym_name(ty[l].sym), ty_why);
                { int r = ty_new(K_SVAR, 0, 0, 0); ty_push(ty_new(K_RES, el, ty_new(K_FN, r, r, 0), 0)); } break;
            }
            if (w == S_CASE) { ty_err(line, "case needs its clauses written right before it, as in `x 0 {'ok (…) 'no (…)} case`."); ty_pop(); ty_pop(); ty_pop(); ty_push(ty_new(K_VAR, 0, 0, 0)); break; }
            if (w == S_LEND) { ty_lend(line); break; }
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
                int sym = ty_new(K_SYM, 0, 0, 0);
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
            if (w == S_CAT) {
                /* two bodies compose: the first's output is the second's input */
                int y = ty_find(ty_pop()), x = ty_find(ty_pop());
                if (ty[x].kind == K_FN || ty[y].kind == K_FN) {
                    int a = ty_new(K_SVAR, 0, 0, 0), b = ty_new(K_SVAR, 0, 0, 0), c = ty_new(K_SVAR, 0, 0, 0);
                    if (ty_unify(x, ty_new(K_FN, a, b, 0)) || ty_unify(y, ty_new(K_FN, b, c, 0))) ty_err(line, "cat joins two bodies into one when the first leaves what the second takes: %s.", ty_why);
                    ty_push(ty_new(K_FN, a, c, 0)); break;
                }
                /* two records: the right one's fields over the left's, as `into` sets them one by one */
                if (ty[x].kind == K_REC || ty[y].kind == K_REC) {
                    int row = ty[y].kind == K_REC ? ty_find(ty[y].a) : 0, n = 0;
                    if (ty[y].kind == K_REC) for (int hops = 0; ty[row].kind == K_REXT; row = ty_rest(row, &hops)) { ty_item(2*n, ty[row].a); ty_item(2*n+1, ty[row].b); n++; }
                    if (ty[y].kind != K_REC || ty[row].kind != K_RNIL) { ty_err(line, "cat joins two records only when it knows every field of the right one, but %s.", ty[y].kind == K_REC ? "the right one may have more fields than it names" : ty[y].kind == K_VAR ? "the right one is a value whose fields this word cannot see, such as an input" : "the right one is not a record"); ty_push(ty_new(K_VAR, 0, 0, 0)); break; }
                    int r = ty_new(K_RVAR, 0, 0, 0);
                    if (ty_unify(x, ty_new(K_REC, r, 0, 0))) { ty_err(line, "cat joins a record with a record: %s.", ty_why); ty_push(ty_new(K_VAR, 0, 0, 0)); break; }
                    for (int k = n - 1; k >= 0; k--) {
                        int f, rest; if (ty_row_take(r, ty[ty_items[2*k]].sym, &f, &rest, 0)) { ty_err(line, "cat: %s.", ty_why); break; }
                        r = ty_new(K_REXT, ty_items[2*k], ty_items[2*k+1], rest);
                    }
                    ty_push(ty_new(K_REC, r, 0, 0)); break;
                }
                ty_push(x); ty_push(y);
            }
            if (w == S_ON) {
                int h = ty_pop(), ev = ty_find(ty_pop()), l = ty[ev].kind == K_SYM ? ty[ev].a : 0;
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
                if (ty_need(top, P_COPY)) ty_err(line, "show's render body gets a copy of the top value each frame, so it is copyable: %s.", ty_why);
                for (int k = 0; k < ty_on_n; k++) {
                    int in = ty_new(K_SCONS, ty_new(K_INT, 0, 0, 0), below, 0);
                    if (ty_on_mouse[k]) in = ty_new(K_SCONS, ty_new(K_INT, 0, 0, 0), in, 0);
                    if (ty_unify(ty_on[k], ty_new(K_FN, in, below, 0))) ty_err(ty_on_line[k], "this handler must take the event's %s and leave the stack below show as it was: %s.", ty_on_mouse[k] ? "x and y" : "int", ty_why);
                }
                if (ty_unify(render, ty_new(K_FN, ty_new(K_SCONS, top, below, 0), below, 0))) ty_err(line, "show's render body takes a copy of the top value and leaves the stack as it was: %s.", ty_why);
                ty_cur = ty_new(K_SVAR, 0, 0, 0); break;
            }
            /* `'k at must` and `edit must` are the old spelling of the total `at`/`edit`; the runtime fuses them. */
            if (w == S_MUST && i > 0 && toks[i-1].tag == TOK_WORD && (toks[i-1].as.sym == S_AT || toks[i-1].as.sym == S_EDIT)) break;
            /* `x no must` and `none must` always die, so the code after them never runs: any stack may follow. */
            if (w == S_MUST && i > 0 && toks[i-1].tag == TOK_WORD && (toks[i-1].as.sym == S_NO || toks[i-1].as.sym == S_NONE) && tyb_find(toks[i-1].as.sym) < tyb_prelude) { ty_pop(); ty_cur = ty_new(K_SVAR, 0, 0, 0); break; }
            int b = tyb_find(w);
            if (b >= 0) {
                if (!tyb_visible(b)) { ty_err(line, "'%s' is bound in the word around this literal, but a body inside a [...] or {...} literal is built when the program is read, so it sees only top-level names and its own.", sym_name(w)); ty_cur = ty_new(K_SVAR, 0, 0, 0); break; }
                if (ty_literal && b >= tyb_prelude) ty_err(line, "'%s' is bound when the program runs, but a [...] or {...} literal is built when it is read.", sym_name(w));
                if (tyb[b].word == 2) ty_apply(ty_self_fn(b, line), sym_name(w), line, 1);
                else if (tyb[b].word) ty_apply(tyb[b].ty, sym_name(w), line, b >= tyb_prelude); else ty_push(tyb[b].ty);
                break;
            }
            if (w == S_EACH || w == S_FOLD) {
                /* the collection sits below the body (and below fold's start value) */
                int st = ty_find(ty_cur);
                for (int k = w == S_EACH ? 1 : 2; k > 0 && ty[st].kind == K_SCONS; k--) st = ty_find(ty[st].b);
                if (ty[st].kind == K_SCONS && ty[ty_find(ty[st].a)].kind == K_DICT) { ty_apply(w == S_EACH ? ty_each_dict : ty_fold_dict, sym_name(w), line, 0); break; }
            }
            if (ty_builtin[w]) { ty_apply(ty_builtin[w], sym_name(w), line, 0); break; }
            ty_err(line, "unknown word '%s'.", sym_name(w));
            break;
        }
        default: break;
        }
    }
}
/* What a primitive's type asks to be copyable: an input it drops or duplicates (it appears a different
   number of times among the outputs, at the top level) and anything a list, dict, box or record holds. */
static void ty_table_copy(int fn) {
    for (int x = ty_find(ty[fn].a), hx = 0; ty[x].kind == K_SCONS; x = ty_rest(x, &hx)) {
        int v = ty_find(ty[x].a), in = 0, out = 0;
        if (ty[v].kind != K_VAR) continue;
        for (int y = ty_find(ty[fn].a), hy = 0; ty[y].kind == K_SCONS; y = ty_rest(y, &hy)) in += ty_find(ty[y].a) == v;
        for (int y = ty_find(ty[fn].b), hy = 0; ty[y].kind == K_SCONS; y = ty_rest(y, &hy)) out += ty_find(ty[y].a) == v;
        if (in != out) ty[v].prot |= P_COPY;
    }
    ty_held_copy(fn);
}
/* What a list, dict, box or record holds is copyable, in the table and in signatures alike. */
static void ty_held_copy(int t) {
    int n = 0, stamp = ++ty_stamp; ty_work_push(&n, t);
    while (n) {
        int x = ty_find(ty_work[--n]);
        if (ty_mark[x] == stamp) continue;
        ty_mark[x] = stamp;
        int held = ty[x].kind == K_LIST || ty[x].kind == K_DICT || ty[x].kind == K_BOX ? ty_find(ty[x].a)
                 : ty[x].kind == K_REXT && ty[ty_find(ty[x].b)].kind == K_PRE ? ty_find(ty[ty_find(ty[x].b)].a) : 0;
        if (held && ty[held].kind == K_VAR) ty[held].prot |= P_COPY;
        if (!ty_isvar(ty[x].kind)) { if (ty[x].a) ty_work_push(&n, ty[x].a); if (ty[x].b) ty_work_push(&n, ty[x].b); if (ty[x].c) ty_work_push(&n, ty[x].c); }
    }
}
/* The builtin table: 'name ( ins -> outs ) pairs. */
static void ty_read_table(Token *toks, int n) {
    for (int i = 0; i + 1 < n; ) {
        if (toks[i].tag != TOK_SYM || toks[i+1].tag != TOK_LPAREN) die("type table: expected 'name ( ... -> ... ) at line %d", toks[i].line);
        TyNames nm = {0}; int close = i + 1 + toks[i+1].span;
        ty_level++; int t = ty_parse_fn(toks, i + 1, close, &nm, 0); ty_level--;
        ty_table_copy(t); ty_generalize(t); ty_builtin[toks[i].as.sym] = t; i = close + 1;
    }
}
static int infer_program(Token *table, int table_n, Token *toks, int count, int user_start) {
    ty_read_table(table, table_n);
    S_LEND = sym_intern("lend");
    uint32_t ed = sym_intern("each-dict"), fd = sym_intern("fold-dict");
    ty_each_dict = ty_builtin[ed]; ty_fold_dict = ty_builtin[fd]; ty_builtin[ed] = ty_builtin[fd] = 0;
    if (!ty_each_dict || !ty_fold_dict) die("type table: each-dict and fold-dict are missing");
    ty_cur = ty_new(K_SNIL, 0, 0, 0);
    ty_in_prelude = 1; ty_range(toks, 0, user_start); ty_in_prelude = 0; tyb_prelude = tyb_n;
    ty_cur = ty_new(K_SNIL, 0, 0, 0);
    ty_range(toks, user_start, count);
    ty_undefined(tyb_prelude);
    for (int x = ty_find(ty_cur), hops = 0; ty[x].kind == K_SCONS; x = ty_rest(x, &hops))
        if (ty_need(ty[x].a, P_COPY)) { ty_err(LOC_LINE(current_loc), "the program ends with a value left on the stack that is never freed: %s.", ty_why); break; }
    /* What is still open at the end reached every place it can: close it. */
    for (int k = 0; k < ty_later_n; k++) { int tail = ty_tail(ty_later[k].tags); if (ty[tail].kind == K_TVAR) { int nil = ty_new(K_TNIL, 0, 0, 0); ty[tail].link = nil; } }
    for (int k = 0; k < ty_later_n; k++) ty_case_closed(&ty_later[k]);
    ty_later_n = 0;
    return ty_errors;
}
#endif
static int typecheck_tokens(Token *toks, int count, int user_start) {
    /* static: the TypeChecker is megabytes, and -flto inlines this into main(),
       whose frame lives for the whole run. */
    static TypeChecker tc; int sigs0 = type_sig_count, quiet = 1;
    uint32_t *unk = NULL; int unk_n = 0;
    /* A pass learns which words, called before their bodies are checked, run code of unknown effect;
       the next pass treats those calls as such code, which may find more. Passes stay quiet until
       the set holds; then a program with errors runs once more to print them. */
    for (int pass = 0; ; pass++) {
    if (pass > 0) {
        int grew = tc.unk_n > unk_n;
        if (!grew && (!tc.errors || !quiet)) break;
        if (!grew) quiet = 0;
        /* Each pass finds at least one more word, so there are no more passes than words the program binds. */
        int lets = 2; for (int k = 0; k < count; k++) if (toks[k].tag == TOK_WORD && toks[k].as.sym == S_LET) lets++;
        if (pass > lets) die("type checker: still finding words that run code of unknown effect after %d passes, with %d words bound", pass, lets - 2);
        free(unk); unk = tc.unk; unk_n = tc.unk_n; tc.unk = NULL;
    }
    for (int r = 1; r < tc.row_count; r++) { free(tc.rows[r].key); free(tc.rows[r].tv); }
    free(tc.tvars); free(tc.rows); free(tc.early); free(tc.branch_used); free(tc.unk);
    memset(&tc, 0, sizeof(tc)); tc.tvar_count = 1; tc.row_count = 1; tc.opaque_at = -1; tc.user_start = user_start;
    tc.quiet = quiet; type_sig_count = sigs0;
    if (unk_n) { tc.unk = malloc((size_t)unk_n * sizeof(uint32_t)); if (!tc.unk) die("type checker: out of memory for %d words", unk_n);
        memcpy(tc.unk, unk, (size_t)unk_n * sizeof(uint32_t)); }
    tc.unk_n = tc.unk_cap = unk_n;
    tc_process_range(&tc, toks, 0, count, count);
    /* A declared word that is used needs a body somewhere: `(body) 'name let`. */
    for (int f = 0; f < tc.fwd_n; f++) { int defined = 0, used = 0;
        if (tc_is_builtin(tc.fwd[f], tc.prelude_sig_count)) continue;
        for (int k = user_start; k < count; k++) {
            if (toks[k].tag == TOK_WORD && toks[k].as.sym == tc.fwd[f]) used = 1;
            if (k + 1 < count && toks[k].tag == TOK_SYM && toks[k].as.sym == tc.fwd[f] && toks[k+1].tag == TOK_WORD && toks[k+1].as.sym == S_LET) defined = 1; }
        if (used && !defined) tc_error(&tc, tc.fwd_line[f], 0, "'%s' is declared here but never defined. Write its body: (...) '%s let.", sym_name(tc.fwd[f]), sym_name(tc.fwd[f])); }

    for (int i = 0; i < tc.sp; i++) {
        if ((tc.data[i].flags & AT_LINEAR) && !(tc.data[i].flags & AT_CONSUMED))
            tc_error(&tc, tc.data[i].source_line, 0, "linear value created here was never consumed (must free, lend, mutate, or clone)");
        if (tc.data[i].type == TC_TAGGED && tc.data[i].tvar_id > 0) {
            int tp = tvar_content(&tc, tc.data[i].tvar_id, TC_TAGGED);
            if (tp > 0) { TypeConstraint pt = tvar_resolve(&tc, tp);
                if (pt == TC_BOX)
                    tc_error(&tc, tc.data[i].source_line, 0, "tagged value contains a linear payload (%s) that was never consumed", constraint_name(pt)); }
        }
    }
    for (int i = 0; i < tc.unknown_count; i++) {
        uint32_t sym = tc.unknowns[i].sym; int def_line = 0;
        for (int j = 0; j < tc.bind_count; j++) if (tc.bindings[j].sym == sym) { def_line = tc.bindings[j].def_line; break; }
        if (!def_line) tc_error(&tc, tc.unknowns[i].line, 0, "unknown word '%s'", sym_name(sym));
        else tc_error(&tc, tc.unknowns[i].line, 0, "'%s' is used on line %d before it is defined on line %d, so the checker cannot know what it takes and leaves there.\n"
                      "    Declare its type before the first use: '%s [...] effect, listing what it takes and leaves (e.g. int lent in  int move out).", sym_name(sym), tc.unknowns[i].line, def_line, sym_name(sym));
    }
    }
    free(unk);
    return tc.errors;
}
/* ---- PRIMITIVES ---- */
/* The aux stack holds what must step aside while code runs: bodies being
   executed, dip's saved value, case's scrutinee, each/fold's input. It never
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
#define POP_BODY(name, label) if (sp<=0) die(label ": stack underflow"); if (stack[sp-1].tag != VAL_TUPLE) die(label ": expected tuple, got %s", valtag_name(stack[sp-1].tag)); POP_VAL(name)
static void deep_copy_values(Value *dst, const Value *src, int slots);
static void deep_free_values(Value *vals, int slots);
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
    POP_VAL(saved); eval_tuple_scoped(body_buf,body_s,env);
    SPUSH(saved_buf,saved_s);
}
static void prim_apply(Frame *env) { POP_BODY(body,"apply"); eval_tuple_scoped(body_buf,body_s,env); }
static void prim_quote(Frame *env) {
    uint32_t sym=pop_sym(); Lookup lu=frame_lookup(env,sym);
    if(!lu.bind) die("quote: unknown binding '%s'",sym_name(sym));
    Binding *b=lu.bind;
    if(b->heap){ stack_room(b->slots,"quote"); deep_copy_values(&stack[sp],b->vals,b->slots); sp+=b->slots; } else SPUSH(b->vals,b->slots);
}
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
#define CMP2(nm,expr) static void prim_##nm(Frame *e){(void)e;int b=val_start(sp,#nm),a=val_start(b,#nm),r=(expr);sp=a;spush(val_int(r?1:0));}
CMP2(eq, val_equal(&stack[a],b-a,&stack[b],sp-b))
CMP2(lt, val_less(&stack[a],b-a,&stack[b],sp-b))
static void prim_print(Frame *e){(void)e;if(sp<=0)die("print: stack underflow");Value top=stack[sp-1];int s=val_slots(top);val_print(&stack[sp-s],s,stdout);printf("\n");sp-=s;
    if(ferror(stdout)) die("print: cannot write to stdout: %s", strerror(errno));}
/* A failed write to stdout must not exit 0. */
static void stdout_check(void){ if(fflush(stdout)||ferror(stdout)){ fprintf(stderr,"slap: cannot write to stdout: %s\n",strerror(errno)); _exit(1); } }
static void prim_assert(Frame *e){(void)e;if(!pop_int())die("assertion failed: expected a nonzero int, got 0");}
static void prim_halt(Frame *e){(void)e;exit(0);}
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
/* Tagged scrutinee: find the clause keyed by its tag (record or 'sym (body)
   pairs); the payload is already in place under the header. Otherwise run
   (pred) (body) pairs, each predicate on its own copy of the scrutinee. */
static void prim_case(Frame *env) {
    if (sp <= 0) die("case: stack underflow");
    if (stack[sp-1].tag!=VAL_TUPLE && stack[sp-1].tag!=VAL_RECORD) die("case: expected tuple or record of clauses, got %s", valtag_name(stack[sp-1].tag));
    POP_VAL(clauses); int clauses_len=(int)clauses_top.as.compound.len;
    POP_VAL(def);
    if (sp <= 0) die("case: stack underflow");
    Value top = stack[sp-1];
    if (top.tag == VAL_TAGGED) {
        ElemRef br = {0}, kr = {0}; uint32_t tag_sym=top.as.compound.len; int found = 0;
        if (clauses_top.tag == VAL_RECORD) { br = record_field(clauses_buf,clauses_s,clauses_len,tag_sym,&found); kr = (ElemRef){br.base-1,1}; }
        else {
            if(clauses_len%2!=0) die("case: need even number of clauses");
            for(int i=0;i<clauses_len&&!found;i+=2){
                ElemRef pr=compound_elem(clauses_buf,clauses_s,clauses_len,i);
                if(clauses_buf[pr.base].tag==VAL_SYM&&clauses_buf[pr.base].as.sym==tag_sym){ br=compound_elem(clauses_buf,clauses_s,clauses_len,i+1); kr=pr; found=1; }
            }
        }
        if (found) { case_body_check(clauses_buf,kr,br); deep_free_values(def_buf,def_s); sp--; eval_body(&clauses_buf[br.base],br.slots,env); return; }
        int ts=val_slots(top); deep_free_values(&stack[sp-ts],ts); sp-=ts;
        SPUSH(def_buf,def_s); return;
    }
    if(clauses_top.tag==VAL_RECORD && clauses_len>0)
        die("case: clause key '%s matches a tag, but the value is %s, not tagged. Tag keys match only a tagged value; test other values with (predicate) keys, as in {(5 lt) (2 mul)}",
            sym_name(clauses_buf[0].as.sym), valtag_name(top.tag));
    if(clauses_len%2!=0) die("case: need even number of clauses (pred/body pairs)");
    POP_VAL(scrut);
    for(int i=0;i<clauses_len;i+=2){
        ElemRef pred_ref=compound_elem(clauses_buf,clauses_s,clauses_len,i);
        if(clauses_buf[pred_ref.base+pred_ref.slots-1].tag!=VAL_TUPLE)
            die("case: clause key %s is %s, not a (predicate). Tag keys like 'ok match only a tagged value, and this one is %s",
                val_text(clauses_buf,pred_ref), valtag_name(clauses_buf[pred_ref.base+pred_ref.slots-1].tag), valtag_name(scrut_buf[scrut_s-1].tag));
        stack_room(scrut_s,"case");
        deep_copy_values(&stack[sp],scrut_buf,scrut_s); sp+=scrut_s;
        eval_body(&clauses_buf[pred_ref.base],pred_ref.slots,env);
        if(pop_int()){ElemRef br=compound_elem(clauses_buf,clauses_s,clauses_len,i+1);
            case_body_check(clauses_buf,pred_ref,br); deep_free_values(def_buf,def_s); SPUSH(scrut_buf,scrut_s); eval_body(&clauses_buf[br.base],br.slots,env); return;}
    }
    deep_free_values(scrut_buf,scrut_s); SPUSH(def_buf,def_s);
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
#define COMPOUND_GUARD(v, who, tagmsg) do{ if((v).tag==VAL_TAGGED) die(who ": " tagmsg); \
    if(!is_compound((v).tag)) die(who ": expected compound, got %s",valtag_name((v).tag)); }while(0)
#define SEQ_GUARD(v, who) do{ if((v).tag!=VAL_LIST&&(v).tag!=VAL_TUPLE) die(who ": expected list or tuple, got %s",valtag_name((v).tag)); }while(0)
static void prim_size(Frame *e) {
    (void)e; Value top=speek();
    if(top.tag==VAL_DICT){ int n=((DictData*)top.as.box)->len; dict_data_free((DictData*)top.as.box); sp--; spush(val_int(n)); return; }
    COMPOUND_GUARD(top, "len", "tagged values have no length (case-match first)");
    sp-=val_slots(top); spush(val_int((int)top.as.compound.len));
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
    if(ref.base<0) { sp-=s; if(tagged) push_none(); else die("get: index %lld out of bounds (len %d)",(long long)idx,len); return; }
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
    Value t2=stack[sp-1]; COMPOUND_GUARD(t2, "cat", "cannot cat tagged values");
    int s2=val_slots(t2),b2=sp-s2; if(b2<1) die("cat: stack underflow");
    Value t1=stack[b2-1]; if(t1.tag!=t2.tag) die("cat: cannot join a %s and a %s",valtag_name(t1.tag),valtag_name(t2.tag));
    Frame *e1=t1.as.compound.env,*e2=t2.as.compound.env;
    if(e1&&e2&&e1!=e2) die("cat: these two tuples close over different scopes, so the joined code would have nowhere to look its words up");
    memmove(&stack[b2-1],&stack[b2],(size_t)(s2-1)*sizeof(Value)); sp--;
    t2.as.compound.len+=t1.as.compound.len; t2.as.compound.slots=(uint32_t)(val_slots(t1)+s2-1); t2.as.compound.env=e2?e2:e1; t2.loc=0; stack[sp-1]=t2;
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
static void dict_put(DictData *dd, const char *key, int klen, Value *vals, int nvals);
static Value dict_val(DictData *dd);
static void dict_push_kv_tuple(DictEntry *e, const char *who) {
    int key_s = e->klen + 1;
    push_string_bytes(e->key, e->klen);
    stack_room(e->nvals,who);
    deep_copy_values(&stack[sp], e->vals, e->nvals); sp+=e->nvals;
    spush(val_compound(VAL_TUPLE, 2, key_s + e->nvals + 1));
}
/* A body given to each/fold/mutate must leave exactly one value where its input began. */
static void one_value_above(int p0, const char *who, const char *what) {
    if(sp<=p0||sp-val_slots(stack[sp-1])!=p0)
        die("%s: the body must turn %s into one value, but the stack moved from %d slots to %d", who, what, p0, sp);
}
static void prim_each(Frame *env) {
    POP_BODY(fn,"each");
    Value top=speek();
    if(top.tag==VAL_DICT){
        Value dv=spop(); DictData *dd=(DictData*)dv.as.box;
        DictData *nd=calloc(1,sizeof(DictData));
        for(int i=0;i<dd->cap;i++){DictEntry *e=&dd->entries[i]; if(!e->key) continue;
            int p0=sp; dict_push_kv_tuple(e,"each");
            eval_body(fn_buf,fn_s,env); one_value_above(p0,"each","one entry");
            Value nt=stack[sp-1]; int ns=val_slots(nt);
            dict_put(nd,e->key,e->klen,&stack[sp-ns],ns); sp-=ns;
        }
        dict_data_free(dd); spush(dict_val(nd)); return;
    }
    if(top.tag==VAL_LIST){
        POP_VAL(list); int len=(int)list_top.as.compound.len,*st=elem_starts(list_buf,list_s,len),rb=sp;
        for(int i=0;i<len;i++){ int p0=sp; if(st) SPUSH(&list_buf[st[i]],st[i+1]-st[i]); else spush(list_buf[i]); eval_body(fn_buf,fn_s,env); one_value_above(p0,"each","one element"); }
        spush(val_compound(VAL_LIST,len,sp-rb+1));
    } else if(top.tag==VAL_TAGGED){
        if(top.as.compound.len==S_OK){
            sp--; int p0=sp-val_slots(stack[sp-1]);
            eval_body(fn_buf,fn_s,env); one_value_above(p0,"each","the payload"); push_ok();
        }
    } else {
        die("each: expected list or tagged, got %s", valtag_name(top.tag));
    }
}
static void prim_fold(Frame *env) {
    POP_BODY(fn,"fold"); POP_VAL(init);
    Value top=speek();
    if(top.tag==VAL_DICT){
        Value dv=spop(); DictData *dd=(DictData*)dv.as.box;
        int p0=sp; SPUSH(init_buf,init_s);
        for(int i=0;i<dd->cap;i++){DictEntry *e=&dd->entries[i]; if(!e->key) continue;
            dict_push_kv_tuple(e,"fold"); eval_body(fn_buf,fn_s,env); one_value_above(p0,"fold","the accumulator and one entry");
        }
        dict_data_free(dd); return;
    }
    if(top.tag!=VAL_LIST) die("fold: expected list or dict, got %s",valtag_name(top.tag));
    POP_VAL(list); int len=(int)list_top.as.compound.len,*st=elem_starts(list_buf,list_s,len);
    int p0=sp; SPUSH(init_buf,init_s);
    for(int i=0;i<len;i++){ if(st) SPUSH(&list_buf[st[i]],st[i+1]-st[i]); else spush(list_buf[i]); eval_body(fn_buf,fn_s,env); one_value_above(p0,"fold","the accumulator and one element"); }
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
    sp-=s;
    if(r<0) { if(tagged) push_none(); else die("index-of: element not found"); }
    else { spush(val_int(r)); if(tagged) push_ok(); }
}
MUST_PAIR(indexof)
/* The checker proves every key a bare `at` or `edit` reads, so a missing key there is a checker bug.
   `at must` and `edit must` are not checked yet; they die with the plain message. */
#define KEY_MISSING(who) do{ if(checked) die(who ": this record has no '%s, but the checker proved it has. This is a bug in slap's checker: please report it with this program.",sym_name(key)); \
    die(who ": key '%s' not found in record",sym_name(key)); }while(0)
static void prim_at_impl(Frame *env, int checked) {
    (void)env; uint32_t key=pop_sym();
    if(sp<=0) die("at: stack underflow"); Value next=stack[sp-1];
    if(next.tag!=VAL_RECORD) die("at: expected record, got %s",valtag_name(next.tag));
    int s=val_slots(next),len=(int)next.as.compound.len,base=sp-s;
    int found; ElemRef ref=record_field(&stack[base],s,len,key,&found);
    if(!found) KEY_MISSING("at");
    memmove(&stack[base],&stack[base+ref.base],ref.slots*sizeof(Value));
    sp=base+ref.slots;
}
static void prim_at(Frame *e) { prim_at_impl(e,1); }
static void prim_at_must(Frame *e) { prim_at_impl(e,0); }
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
static void prim_edit_impl(Frame *env, int checked) {
    POP_BODY(fn,"edit"); uint32_t key=pop_sym(); REC_PREAMBLE("edit");
    int found; ElemRef ref=record_field(&stack[rec_base],rec_s,rec_len,key,&found);
    if(!found) KEY_MISSING("edit");
    stack_room(ref.slots,"edit");
    deep_copy_values(&stack[sp],&stack[rec_base+ref.base],ref.slots); sp+=ref.slots;
    eval_body(fn_buf,fn_s,env);
    rec_put(key,"edit");
}
static void prim_edit(Frame *e) { prim_edit_impl(e,1); }
static void prim_edit_must(Frame *e) { prim_edit_impl(e,0); }
typedef struct BoxData { Value *data; int slots; } BoxData;
static void prim_box(Frame *e){(void)e;Value top=speek();int s=val_slots(top);BoxData *bd=malloc(sizeof(BoxData));bd->data=malloc(s*sizeof(Value));bd->slots=s;deep_copy_values(bd->data,&stack[sp-s],s);sp-=s;Value v;v.tag=VAL_BOX;v.loc=0;v.as.box=bd;spush(v);}
static void prim_free(Frame *e){
    (void)e;Value v=spop();
    if(v.tag==VAL_BOX){BoxData *bd=(BoxData*)v.as.box;if(!bd->data)die("free: double-free detected (box already freed, likely captured by a closure that ran twice)");free(bd->data);bd->data=NULL;bd->slots=-1;return;}
    die("free: expected box, got %s", valtag_name(v.tag));
}
#define BOX_UNPACK(who) POP_BODY(fn,who); Value box_val=spop(); if(box_val.tag!=VAL_BOX) die(who ": expected box, got %s", valtag_name(box_val.tag)); \
    BoxData *bd=(BoxData*)box_val.as.box; if(!bd->data) die(who ": the box was already freed"); stack_room(bd->slots,who)
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
        if(dst[i].tag==VAL_BOX){
            BoxData *o=(BoxData*)dst[i].as.box; BoxData *c=malloc(sizeof(BoxData));
            c->slots=o->slots; c->data=malloc(o->slots*sizeof(Value));
            deep_copy_values(c->data,o->data,o->slots); dst[i].as.box=c;
        } else if(dst[i].tag==VAL_DICT){
            dst[i].as.box=dict_clone((DictData*)dst[i].as.box);
        }
    }
    box_walk_depth--;
}
static void deep_free_values(Value *vals, int slots) {
    if(++box_walk_depth > BOX_DEPTH_MAX){ box_walk_depth=0;
        die("box nesting deeper than %d -- a box that contains itself cannot be freed", BOX_DEPTH_MAX); }
    for(int i=0;i<slots;i++){
        if(vals[i].tag==VAL_DICT) dict_data_free((DictData*)vals[i].as.box);
        else if(vals[i].tag==VAL_BOX){
            BoxData *bd=(BoxData*)vals[i].as.box;
            if(bd->data){ deep_free_values(bd->data,bd->slots); free(bd->data); bd->data=NULL; bd->slots=-1; }
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
static int pop_string_bytes(const char *who, char **out, int *out_len) {
    Value top=speek();
    if(top.tag!=VAL_LIST) die("%s: expected string (list of int), got %s", who, valtag_name(top.tag));
    int s=val_slots(top), len=(int)top.as.compound.len, base=sp-s;
    if(s != len+1) die("%s: key must be a simple string (list of int)", who);
    char *buf=malloc(len?len:1);
    for(int i=0;i<len;i++){
        if(stack[base+i].tag!=VAL_INT) die("%s: key string contains non-int at position %d", who, i);
        int64_t c=stack[base+i].as.i; if(c<0||c>255) die("%s: key byte %d is %lld, outside 0-255", who, i, (long long)c);
        buf[i]=(char)c;
    }
    sp=base; *out=buf; *out_len=len; return len;
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
    (void)e; POP_VAL(val); char *key; int klen; pop_string_bytes("insert",&key,&klen);
    Value dv=speek(); if(dv.tag!=VAL_DICT) die("insert: expected dict, got %s", valtag_name(dv.tag));
    DictData *dd=(DictData*)dv.as.box;
    dict_put(dd,key,klen,val_buf,val_s);
    free(key);
}
static void prim_of(Frame *e) {
    (void)e; char *key; int klen; pop_string_bytes("of",&key,&klen);
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
    (void)e; char *key; int klen; pop_string_bytes("remove",&key,&klen);
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
static void dict_data_free(DictData *dd) {
    for(int i=0;i<dd->cap;i++) dict_free_entry_contents(&dd->entries[i]);
    free(dd->entries); free(dd);
}
/* ---- EVAL ---- */
/* Run a body in its own scope: bindings it makes are trimmed afterwards, and
   caller bindings it rebinds are put back. Bindings that closures it returns
   refer to move into a fresh child frame those closures point at. */
static void eval_tuple_scoped(Value *body, int slots, Frame *env) {
    Frame *ee=body[slots-1].as.compound.env?body[slots-1].as.compound.env:env;
    int sbc=ee->bind_count,sp0=sp,sv0=saves_sp;
    int prev_active=frame_save_active; Frame *prev_target=frame_save_target; int prev_sbc=frame_save_sbc, prev_sb0=frame_save_sb0;
    frame_save_active=1; frame_save_target=ee; frame_save_sbc=sbc; frame_save_sb0=sv0;
    eval_body(body,slots,env);
    frame_save_active=prev_active; frame_save_target=prev_target; frame_save_sbc=prev_sbc; frame_save_sb0=prev_sb0;
    if(ee->bind_count==sbc&&saves_sp==sv0) return;
    Frame *cf=NULL;
    for(int i=sp;i>sp0;i-=val_slots(stack[i-1])){
        if(stack[i-1].tag!=VAL_TUPLE||stack[i-1].as.compound.env!=ee) continue;
        /* the child frame takes over the bindings, and any dict they own */
        if(!cf){ cf=frame_new(ee); for(int j=sbc;j<ee->bind_count;j++){ frame_bind(cf,ee->bindings[j].sym,ee->bindings[j].vals,ee->bindings[j].slots,ee->bindings[j].word); ee->bindings[j].heap=0; } }
        stack[i-1].as.compound.env=cf;
    }
    for(int p=saves_sp-1;p>=sv0;p--){
        Binding *b=&ee->bindings[saves[p].bi];
        if(b->heap) deep_free_values(b->vals,b->slots);
        free(b->vals); b->vals=saves[p].vals; b->slots=saves[p].slots; b->cap=saves[p].cap; b->word=saves[p].word; b->heap=saves[p].heap;
    }
    saves_sp=sv0;
    frame_trim(ee,sbc);
}
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
static void dispatch_word(uint32_t sym, Frame *env) {
    Lookup lu=frame_lookup(env,sym);
    if(!lu.bind) die("unknown word: %s",sym_name(sym));
    Binding *b=lu.bind; Value *v=b->vals; int s=b->slots;
    if(!b->word){ if(b->heap){ stack_room(s,sym_name(sym)); deep_copy_values(&stack[sp],v,s); sp+=s; } else SPUSH(v,s); return; }
    Frame *f=lu.frame; int bi=(int)(b-f->bindings);
    b->pinned++; eval_tuple_scoped(v,s,env); f->bindings[bi].pinned--;
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
static void eval_in(Value *body, int slots, Frame *ee);
static void eval_body(Value *body, int slots, Frame *env) {
    Value hdr=body[slots-1]; if(hdr.tag!=VAL_TUPLE) die("eval_body: expected tuple, got %s (internal: evaluator received non-tuple header)", valtag_name(hdr.tag));
    eval_in(body, slots, hdr.as.compound.env?hdr.as.compound.env:env);
}
/* Runs a tuple body in frame ee. */
static void eval_in(Value *body, int slots, Frame *ee) {
    if(++eval_depth > EVAL_DEPTH_MAX) die("recursion depth exceeded (%d levels)", EVAL_DEPTH_MAX);
    c_stack_check("in a nested call");
    int len=(int)body[slots-1].as.compound.len;
    int sbc=ee->bind_count,a0=asp,*st=elem_starts(body,slots,len);
    for(int k=0;k<len;k++){
        int eo=k,es=1; if(st){eo=st[k];es=st[k+1]-st[k];}
        const Value *ep=&body[eo+es-1];
        if(ep->tag<=VAL_SYM){
            if(sp>=STACK_MAX){ current_loc=ep->loc; die("stack overflow: all %d value slots are in use", STACK_MAX); }
            stack[sp++]=*ep; continue;
        }
        if(ep->loc) current_loc=ep->loc;
        if(ep->tag==VAL_XT){
            int a1=asp;
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
                    ee->captured=1; /* as pushing the branches would, so bindings are kept, not remade */
                    if(c.as.i) eval_in(&body[eo],es,ee); else eval_in(&body[e1],s1,ee);
                    k+=2; continue;
                }
            }
            /* a literal is built once; a dict in it belongs to each copy the program pushes */
            if(vals_hold_dict(&body[eo],es)){ stack_room(es,"a literal"); deep_copy_values(&stack[sp],&body[eo],es); sp+=es; }
            else SPUSH(&body[eo],es);
            if(ep->tag==VAL_TUPLE){ee->captured=1;stack[sp-1].as.compound.env=ee;}
        } else if(ep->tag==VAL_DICT){ stack_room(1,"a literal"); deep_copy_values(&stack[sp],ep,1); sp++; }
        else spush(*ep);
    }
    if(!ee->captured) frame_trim(ee,sbc);
    asp=a0; eval_depth--;
}
static void build_tuple(Token *toks, int start, int end, int tc, Frame *env) {
    int eb=sp,ec=0; Token *ft = (start < end) ? &toks[start] : NULL;
    for(int j=start;j<end;j++){
        Token *tt=&toks[j]; current_loc=LOC_PACK(tt->fid,tt->line,tt->col);
        switch(tt->tag){
        case TOK_INT:
            spush(with_tok(val_int(tt->as.i),tt)); ec++; break;
        case TOK_FLOAT: spush(with_tok(val_float(tt->as.f),tt)); ec++; break;
        case TOK_SYM: spush(with_tok(val_sym(tt->as.sym),tt)); ec++; break;
        case TOK_WORD:
            if(tt->as.sym==S_CHECK){if(ec>0&&stack[sp-1].tag==VAL_XT){sp--;ec--;}else die("check: expected preceding type word, got %s",ec>0?valtag_name(stack[sp-1].tag):"empty stack");}
            else{
                uint32_t s=tt->as.sym;
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
            spush(with_tok(val_compound(VAL_LIST,n,sp-lb+1),tt)); ec++; j=bc; break;
        }
        case TOK_LBRACE:{
            int bc=(j+toks[j].span);
            int lb=sp; eval(toks+j+1,bc-j-1,env); int ts=sp-lb,nf=0,ir=1,p=sp;
            while(p>lb){int vs=val_slots(stack[p-1]);p-=vs;if(ir&&p>lb&&stack[p-1].tag==VAL_SYM){p--;nf++;}else ir=0;}
            if(ir) spush(with_tok(val_compound(VAL_RECORD,nf,ts+1),tt));
            else{int n=0;p=sp;while(p>lb){p-=val_slots(stack[p-1]);n++;}spush(with_tok(val_compound(VAL_TUPLE,n,ts+1),tt));}
            ec++; j=bc; break;
        }
        default: break;
        }
    }
    Value hdr=val_compound(VAL_TUPLE,ec,sp-eb+1); if(ft) hdr.loc=LOC_PACK(ft->fid,ft->line,ft->col);
    spush(hdr); if(env) env->captured=1; stack[sp-1].as.compound.env=env;
}
static void eval(Token *toks, int count, Frame *env) {
    int base=sp; build_tuple(toks,0,count,count,env);
    int s=val_slots(stack[sp-1]); Value *body=malloc(s*sizeof(Value));
    VCPY(body,&stack[base],s); sp=base; eval_body(body,s,env); free(body);
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
#define A2E " ['a num lent in  'a num lent in  'a num move out] effect\n"
#define I2E " [int lent in  int lent in  int move out] effect\n"
#define F1E " [float lent in  float move out] effect\n"
#define F2E " [float lent in  float lent in  float move out] effect\n"
#define LNE " ['a list own in  int lent in  'a list move out] effect\n"
#define MO " move out] effect\n"
static const char *BUILTIN_TYPES =
    "'dup ['a copy in  'a copy out  'a copy out] effect\n"
    "'drop ['a copy in] effect\n'swap ['a own in  'b own in  'b own out  'a own out] effect\n"
    "'over ['a copy in  'b own in  'a copy out  'b own out  'a copy out] effect\n'rot ['a own in  'b own in  'c own in  'b own out  'c own out  'a own out] effect\n"
    "'plus" A2E "'sub" A2E "'mul" A2E "'div" A2E
    "'mod" I2E "'wrap" I2E "'band" I2E "'bor" I2E "'bxor" I2E "'shl" I2E "'shr" I2E
    "'bnot [int lent in  int" MO "'divmod [int lent in  int lent in  int move out  int" MO
    "'eq [lent in  lent in  int" MO "'lt ['a ord lent in  'a ord lent in  int" MO
    "'and" I2E "'or" I2E
    "'print [own in] effect\n'assert [int own in] effect\n'millis [int" MO
    "'datetime [int list" MO
    "'itof [int lent in  float" MO "'ftoi [float lent in  int" MO
    "'fsqrt" F1E "'ffloor" F1E "'fround" F1E "'fexp" F1E "'flog" F1E
    "'fpow" F2E "'fatan2" F2E
    "'list [list" MO "'len [sized auto in  int" MO "'push ['a seq own in  'a own in  'a seq" MO "'pop ['a seq own in  'a seq move out  {'ok 'a 'no ()} either move out] effect\n"
    "'get ['a seq own in  int lent in  {'ok 'a 'no ()} either move out] effect\n'peek ['a seq own in  int lent in  'a seq move out  {'ok 'a 'no ()} either move out] effect\n'nth [sym lent in  int lent in  {'ok 'a 'no ()} either move out] effect\n'set ['a seq own in  int lent in  'a own in  {'ok list 'no ()} either move out] effect\n'cat ['a semigroup own in  'a semigroup own in  'a semigroup" MO
    "'reverse ['a list own in  'a list move out] effect\n'take-n" LNE "'drop-n" LNE "'range [int lent in  int lent in  int list" MO "'sort ['a ord list own in  'a ord list" MO
    "'index-of ['a list own in  'a lent in  {'ok int 'no ()} either move out] effect\n"
    "'rec [rec" MO
    "'random [int lent in  int" MO "'halt [] effect\n'box ['a own in  'a box" MO "'free ['a box own in] effect\n"
    "'at [rec own in  sym lent in  {'ok 'a 'no ()} either move out] effect\n'into [rec own in  own in  sym lent in  rec" MO
    "'clear [int lent in] effect\n'pixel [int lent in  int lent in  int lent in] effect\n'fill-rect [int lent in  int lent in  int lent in  int lent in  int lent in] effect\n"
    "'zip ['a list own in  'a list own in  list list" MO
    "'read [list own in  {'ok int list 'no int list} either move out] effect\n'write [list own in  int list own in  {'ok int 'no int list} either move out] effect\n'ls [list own in  {'ok list list 'no int list} either move out] effect\n"
    "'str-find [int list own in  int list own in  {'ok int 'no ()} either move out] effect\n"
    "'str-split [int list own in  int list own in  list list" MO "'parse-http [int list own in  {'ok rec 'no int list} either move out] effect\n'args [list list" MO "'isheadless [int" MO
#ifndef SLAP_WASM
    "'tcp-connect [int list own in  int lent in  {'ok box 'no int list} either move out] effect\n'tcp-send [int box own in  int list own in  int box move out  {'ok int 'no int list} either move out] effect\n"
    "'tcp-recv [int box own in  int lent in  int box move out  {'ok int list 'no int list} either move out] effect\n'tcp-close [int box own in] effect\n'tcp-listen [int lent in  {'ok box 'no int list} either move out] effect\n'tcp-accept [int box own in  int box move out  {'ok box 'no int list} either move out] effect\n"
#endif
    "'tag ['a own in  sym lent in  'a tagged" MO "'must [tagged own in " MO
    "'dict ['a dict" MO "'insert ['a dict own in  list lent in  'a own in  'a dict" MO
    "'of ['a dict own in  list lent in  'a dict move out  {'ok 'a 'no list} either move out] effect\n"
    "'remove ['a dict own in  list lent in  'a dict" MO "'dict-keys ['a dict own in  'a dict move out  list list" MO
    "'pthen [tagged own in  own in  tuple own in  move out  tagged" MO
    "'quote [sym own in  'a own out] effect\n"
;
#undef A2E
#undef I2E
#undef F1E
#undef F2E
#undef LNE
#undef MO
static const char *PRELUDE =
    "(swap drop) 'nip let\n"
    "(0 eq) [int lent in  int move out] effect 'not let\n"
    "(eq not) [lent in  lent in  int move out] effect 'neq let\n"
    "(swap lt) ['a ord lent in  'a ord lent in  int move out] effect 'gt let\n"
    "(lt not) ['a ord lent in  'a ord lent in  int move out] effect 'ge let\n"
    "(swap lt not) ['a ord lent in  'a ord lent in  int move out] effect 'le let\n"
    "(1 plus) [int lent in  int move out] effect 'inc let\n"
    "(1 sub) [int lent in  int move out] effect 'dec let\n"
    "(0 swap sub) [int lent in  int move out] effect 'neg let\n"
    "(over over lt (nip) (drop) if) ['a ord lent in  'a ord lent in  'a ord move out] effect 'max let\n"
    "(over over lt (drop) (nip) if) ['a ord lent in  'a ord lent in  'a ord move out] effect 'min let\n"
    "(dup 0 lt (neg) (dup drop) if) 'abs let\n"
    "('f let (dup 0 gt) (1 sub (f apply) dip) while drop) 'repeat let\n"
    "('p let list (dup p apply (push) (drop) if) fold) ['a list own in  tuple own in  'a list move out] effect 'filter let\n"
    "(dup mul) 'sqr let\n"
    "(0 get must) 'first let\n"
    "(dup len 1 sub get must) 'last let\n"
    "(0 (plus) fold) 'sum let\n"
    "(index-of 0 {'ok (drop 1) 'no (drop 0)} case) 'member let\n"
    "(list rot push swap push) 'couple let\n"
    "(list (cat) fold) 'flatten let\n"
    "(0.0 swap sub) 'fneg let\n"
    "(dup 0.0 lt (fneg) () if) 'fabs let\n"
    "(dup 0 lt (drop -1) (dup 0 eq (drop 0) (drop 1) if) if) 'sign let\n"
    "(rot swap min max) 'clamp let\n"
    "(2 mod 0 eq) [int lent in  int move out] effect 'iseven let\n"
    "('ok tag) ['a own in  'a tagged move out] effect 'ok let\n"
    "('no tag) ['a own in  'a tagged move out] effect 'no let\n"
    "(() no) [tagged move out] effect 'none let\n"
    "('body let () {'ok (body apply) 'no (no)} case) [tagged own in  tuple own in  tagged move out] effect 'then let\n"
    "('fb let fb {'ok () 'no (drop fb)} case) [{'ok 'a 'no 'b} either own in  'a own in  'a move out] effect 'default let\n"
    "(list ('dd-x let dup dd-x member (dd-x drop) (dd-x push) if) fold) ['a list own in  'a list move out] effect 'dedup let\n"
    "3.14159265358979323846 'pi let\n"
    "6.28318530717958647692 'tau let\n"
    "(255 band) 'byte-mask let\n"
    "('b let 0 8 range (7 swap sub b swap shr 1 band) each) 'byte-bits let\n"
    "(0 (swap 1 shl bor) fold) 'bits-byte let\n"
    "('n let list swap (dup len 0 eq not) (dup n take-n swap (push) dip n drop-n) while drop) 'chunks let\n"

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
static char *pop_string_path(const char *who) {
    int len; unsigned char *raw = pop_byte_list_buf(who, &len);
    if (memchr(raw, 0, len)) die("%s: the path contains a NUL byte", who);
    char *buf = realloc(raw, len + 1); buf[len] = '\0'; return buf;
}
static void prim_read(Frame *e) {
    (void)e; char *path=pop_string_path("read");
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
    (void)e; int len;unsigned char *buf=pop_byte_list_buf("write",&len);char *path=pop_string_path("write");
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
    (void)e; char *path=pop_string_path("ls");
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
    if (!bd->data) die("%s: the socket box was already freed", who);
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
    (void)e; int64_t port=pop_int(); char *host=pop_string_path("tcp-connect");
    struct addrinfo hints={0},*res; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    char ps[16]; snprintf(ps,sizeof(ps),"%lld",(long long)port);
    if(port<0||port>65535) die("tcp-connect: port %lld is outside 0-65535",(long long)port);
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
    unsigned char *buf = malloc(maxlen); ssize_t n = recv(fd, buf, maxlen, 0);
    if (n < 0) { free(buf); push_fail(strerror(errno)); return; }
    push_byte_list(buf, n); free(buf); push_ok();
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
        R(dup,dup),R(drop,drop),R(swap,swap),R(over,over),R(rot,rot),R(dip,dip),R(apply,apply),R(quote,quote),
        R(plus,plus),R(sub,sub),R(mul,mul),R(div,div),R(mod,mod),R(divmod,divmod),R(wrap,wrap),
        R(band,band),R(bor,bor),R(bxor,bxor),R(bnot,bnot),R(shl,shl),R(shr,shr),
        R(eq,eq),R(lt,lt),R(and,and),R(or,or),
        R(print,print),R(assert,assert),R(halt,halt),R(random,random),
        R(if,if),R(case,case),R(while,while),
        R(itof,itof),R(ftoi,ftoi),R(fsqrt,fsqrt),
        R(ffloor,ffloor),R(fround,fround),R(fexp,fexp),R(flog,flog),R(fpow,fpow),R(fatan2,fatan2),
        R(list,list),R(len,size),R(push,push_op),M("pop",pop),
        M("get",get),M("peek",peek),M("nth",nth),M("set",set),R(cat,concat),
        R(reverse,reverse),R(zip,zip),{"take-n",prim_take_n,NULL},{"drop-n",prim_drop_n,NULL},R(range,range),
        R(fold,fold),R(each,each),R(sort,sort),M("index-of",indexof),
        M("at",at),R(rec,rec),R(into,into),M("edit",edit),
        R(millis,millis),R(datetime,datetime),R(box,box),R(free,free),R(lend,lend),R(mutate,mutate),
        R(dict,dict),R(insert,insert),R(of,of),R(remove,remove),
        {"dict-keys",prim_keys,NULL},
        R(tag,tag),R(must,must),R(pthen,pthen),
        R(read,read),R(write,write),R(ls,ls),
        M("str-find",strfind),{"str-split",prim_str_split,NULL},{"parse-http",prim_parse_http,NULL},
        R(args,args),R(isheadless,isheadless),
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
    Frame *global=frame_new(NULL);
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
#ifdef SLAP_NEXT
    static Token table[TOK_MAX]; store_source_lines(TYPES, FID_BUILTIN); lex(TYPES, FID_BUILTIN);
    int table_count=tok_count; memcpy(table,tokens,table_count*sizeof(Token));
#else
    store_source_lines(BUILTIN_TYPES, FID_BUILTIN);
    lex(BUILTIN_TYPES, FID_BUILTIN); COMBINE(tok_count); memcpy(combined,tokens,tok_count*sizeof(Token)); cpos=tok_count;
#endif
    lex(PRELUDE, FID_PRELUDE); COMBINE(tok_count); memcpy(&combined[cpos],tokens,tok_count*sizeof(Token)); cpos+=tok_count;
    int user_start=cpos;
    COMBINE(user_tok_count); memcpy(&combined[cpos],user_tokens,user_tok_count*sizeof(Token)); cpos+=user_tok_count;
#ifdef SLAP_NEXT
    (void)typecheck_tokens; (void)BUILTIN_TYPES;
    int errors=infer_program(table,table_count,combined,cpos,user_start);
#else
    int errors=typecheck_tokens(combined,cpos,user_start);
#endif
    if(errors>0){fprintf(stderr,"%d type error(s)\n",errors);return 1;}
    if(check_only){fprintf(stderr,"type check passed\n");return 0;}
    current_loc=LOC_PACK(FID_STDIN,0,0);
    /* Registered after stdout_check, so it runs first, on halt and die too. */
    if(profile){ prof_last=prof_now(); atexit(prof_report); }
    eval(user_tokens,user_tok_count,global);
    return 0;
}
