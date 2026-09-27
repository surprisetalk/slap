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
typedef struct Binding { uint32_t sym; int slots, cap, pinned; Value *vals; } Binding;
struct Frame {
    struct Frame *parent; int bind_count, bind_cap, refcount;
    Binding *bindings;
    int32_t *hash; uint32_t hash_mask; /* binding index+1 by symbol; 0 is empty */
};
/* The bindings a scoped body shadowed, detached so the scope can put them back. */
typedef struct { int bi, slots, cap; Value *vals; } Saved;
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
/* Drop bindings [n, bind_count). Newest first, so no probe chain is cut short. */
static void frame_trim(Frame *f, int n) {
    for (int i = f->bind_count-1; i >= n; i--) {
        uint32_t s = f->bindings[i].sym & f->hash_mask;
        while (f->hash[s] != i+1) s = (s+1) & f->hash_mask;
        f->hash[s] = 0;
    }
    f->bind_count = n;
}
static void frame_bind(Frame *f, uint32_t sym, Value *vals, int slots) {
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
        saves[saves_sp++] = (Saved){bi, b->slots, b->cap, b->vals};
        b->vals = NULL; b->cap = 0;
    }
    if (slots > b->cap || b->pinned) {
        if (b->pinned) { b->vals = NULL; b->cap = 0; }
        b->vals = realloc(b->vals, (size_t)slots*sizeof(Value));
        if (!b->vals) die("out of memory: cannot bind %d values to '%s", slots, sym_name(sym));
        b->cap = slots;
    }
    VCPY(b->vals, vals, slots); b->slots = slots;
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
static uint32_t S_LET, S_IF, S_EFFECT, S_CHECK, S_OK, S_NO, S_CASE, S_MUST, S_QUOTE, S_THEN;
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
    S_CASE=sym_intern("case"); S_MUST=sym_intern("must"); S_QUOTE=sym_intern("quote"); S_THEN=sym_intern("then");
    for (int i = 0; i < HO_OP_COUNT; i++) ho_ops[i].sym = sym_intern(ho_ops[i].name);
}
typedef struct {
    uint32_t type_var; TypeConstraint constraint, elem_constraint; OwnMode ownership; SlotDir direction;
    uint32_t either_syms[8]; TypeConstraint either_types[8]; uint32_t either_tvars[8]; int either_count;
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
typedef struct { int parent; TypeConstraint bound; int elem; int box_c; int tag_p; int union_id; } TVarEntry;
#define UNION_MAX 2048
#define UNION_VARIANTS_MAX 16
typedef struct { uint32_t syms[UNION_VARIANTS_MAX]; TypeConstraint types[UNION_VARIANTS_MAX]; int count; } UnionDef;
#define EFFECT_MAX 4096
typedef struct {
    int consumed, produced;
    TypeConstraint out_type;
    int scheme_base, scheme_count;
    int in_tvars[16], out_tvars[16], in_count, out_count;
    int out_effect;
    int has_let;     /* body binds a name it reads back: a lend snapshot could alias it */
    int output_is_linear; /* body's sole output is itself a linear-capturing closure; apply should mark output AT_LINEAR */
    int opaque;      /* body runs code of unknown effect, so running the body does too */
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
typedef struct { uint32_t sym; AbstractType atype; int def_line; int consumed_line; } TCBinding;
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
    int opaque_runs; /* runs of code whose effect is unknown; see tc_tuple */
    int saw_linear_capture;  /* set by binding-lookup of a linear value; consumed by enclosing tuple-body inference */
    UnionDef unions[UNION_MAX]; int union_count;
} TypeChecker;
static int tvar_fresh(TypeChecker *tc) {
    if (tc->tvar_count >= tc->tvar_cap) {
        tc->tvar_cap = tc->tvar_cap ? tc->tvar_cap*2 : 4096;
        tc->tvars = realloc(tc->tvars, (size_t)tc->tvar_cap*sizeof(TVarEntry));
        if (!tc->tvars) die("out of memory: %d type variables", tc->tvar_cap);
    }
    int id = tc->tvar_count++;
    tc->tvars[id].parent = id; tc->tvars[id].bound = TC_NONE; tc->tvars[id].elem = 0; tc->tvars[id].box_c = 0; tc->tvars[id].tag_p = 0; tc->tvars[id].union_id = 0;
    return id;
}
static int tvar_find(TypeChecker *tc, int id) {
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
static int tvar_unify(TypeChecker *tc, int a, int b) {
    int ra = tvar_find(tc, a), rb = tvar_find(tc, b);
    if (ra == rb) return 0;
    TypeConstraint ca = tc->tvars[ra].bound, cb = tc->tvars[rb].bound;
    if (ca != TC_NONE && cb != TC_NONE && !tc_constraint_matches(ca, cb) && !tc_constraint_matches(cb, ca))
        return 1;
    tc->tvars[rb].parent = ra;
#define PROP(f) if (!tc->tvars[ra].f && tc->tvars[rb].f) tc->tvars[ra].f = tc->tvars[rb].f
    PROP(elem); PROP(box_c); PROP(tag_p); PROP(union_id);
#undef PROP
    if (ca == TC_NONE) tc->tvars[ra].bound = cb;
    else if (tc_constraint_matches(ca, cb) && tc_should_narrow(ca, cb)) tc->tvars[ra].bound = cb;
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
static int tvar_unify_at(TypeChecker *tc, int tvar, AbstractType *at) {
    if (at->tvar_id > 0) return tvar_unify(tc, tvar, at->tvar_id);
    if (at->type != TC_NONE) return tvar_bind(tc, tvar, at->type);
    return 0;
}
/* Copy a tuple's scheme into fresh tvars for one call site: the tvars its inputs
   and outputs reach, with links between them remapped. A link out of the scheme
   gets a fresh tvar with the same bound, so call sites never constrain each
   other. map[i] is the copy of tvar base+i, or 0 if nothing reaches it. */
static void tvar_instantiate(TypeChecker *tc, TupleEffect *eff, int *map) {
    int base = eff->scheme_base, count = eff->scheme_count, n = 0;
    int *list = malloc((size_t)(count ? count : 1)*sizeof(int));
    if (!list) die("out of memory instantiating %d type variables", count);
#define REACH(tv) do { int t_ = (tv), o_ = t_ - base; if (t_ > 0 && o_ >= 0 && o_ < count && !map[o_]) { map[o_] = -1; list[n++] = o_; } } while (0)
    for (int j = 0; j < eff->in_count; j++) REACH(eff->in_tvars[j]);
    for (int j = 0; j < eff->out_count; j++) REACH(eff->out_tvars[j]);
    for (int k = 0; k < n; k++) {
        int r = tvar_find(tc, base + list[k]);
        REACH(r); REACH(tc->tvars[r].elem); REACH(tc->tvars[r].box_c); REACH(tc->tvars[r].tag_p);
    }
#undef REACH
    for (int k = 0; k < n; k++) map[list[k]] = tvar_fresh(tc);
    for (int k = 0; k < n; k++) {
        int i = list[k], root = tvar_find(tc, base + i);
        tc->tvars[map[i]].bound = tc->tvars[root].bound;
#define REMAP(f) if (tc->tvars[root].f > 0) { int off = tc->tvars[root].f - base; \
    if (off >= 0 && off < count) tc->tvars[map[i]].f = map[off]; \
    else { int fresh = tvar_fresh(tc); tc->tvars[fresh].bound = tc->tvars[tvar_find(tc, tc->tvars[root].f)].bound; tc->tvars[map[i]].f = fresh; } }
        REMAP(elem) REMAP(box_c) REMAP(tag_p)
#undef REMAP
        tc->tvars[map[i]].union_id = tc->tvars[root].union_id;
    }
    for (int k = 0; k < n; k++) { int i = list[k], off = tvar_find(tc, base + i) - base; if (off >= 0 && off < count && off != i) tvar_unify(tc, map[i], map[off]); }
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
/* Record an operation taking n values: any beyond the body's floor are an underflow. */
static void tc_take(TypeChecker *tc, int n) {
    int avail = tc->sp - tc->sp_floor;
    if (n > avail) tc->underflows += n - avail;
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
    tc->errors++; int fid = LOC_FID(current_loc); const char *f = src_files[fid];
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
        case TOK_LBRACKET: i = (i+toks[i].span); vsp++; tt = TC_LIST; break;
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
                if (sig) {
                    int ni = 0, no = 0; TypeConstraint lo = TC_NONE;
                    for (int j = 0; j < sig->slot_count; j++) { if (sig->slots[j].direction == DIR_IN) ni++; else { no++; lo = sig->slots[j].constraint; } }
                    EFF_CONSUME(vsp,consumed,ni); vsp += no; if (no > 0) tt = lo; else if (ni > 0) tt = TC_NONE;
                } else {
                    HOEffect *ho = ho_ops_find(sym);
                    if (ho) { int need = ho->need, out = ho->out;
                        if (ctx && sym == S_IF && i >= start + 2 && i-1 >= start && toks[i-1].tag == TOK_RPAREN) {
                            int bp = i-1, ep = bp + toks[bp].span;
                            int bc = 0, bp2 = 0; tc_infer_effect(toks, ep+1, bp, &bc, &bp2, ctx, local_binds, local_count); need = ho->need + bc; out = bp2;
                        }
                        EFF_CONSUME(vsp,consumed,need); vsp += out; if (out > 0) tt = ho->out_type; else if (need > 0) tt = TC_NONE;
                    }
                }
                if (!sig && !ho_ops_find(sym)) {
                    int lidx = -1; for (int j = 0; j < local_count; j++) if (local_binds[j] == sym) { lidx = j; break; }
                    if (lidx >= 0) {
                        EFF_CONSUME(vsp,consumed,local_bc[lidx]); vsp += local_bp[lidx]; tt = TC_NONE;
                    } else if (ctx) { TCBinding *ub = tc_lookup(ctx, sym);
                        if (ub && ub->atype.type == TC_TUPLE && ub->atype.effect_idx >= 0) { TupleEffect *e = &ctx->effects[ub->atype.effect_idx]; EFF_CONSUME(vsp,consumed,e->consumed); vsp += e->produced; if (e->produced > 0) tt = e->out_type; }
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
    tc_take(tc, consumed); int avail = tc->sp - tc->sp_floor; tc->sp -= (consumed <= avail) ? consumed : avail;
    for (int i = 0; i < produced; i++) tc_push(tc, (i == produced - 1) ? out_type : TC_NONE, line);
}
static int tc_check_either_tags(Token *toks, int start, int end, TypeSig *sig) {
    int errors = 0;
    /* either-schema validation: collect declared output variant symbols and
       reject any literal `'sym tag` / bare `ok`/`no`/`none` whose symbol isn't
       in the schema. Flat scan — false positives on intermediate tags are
       possible but rare; fix by dropping the either declaration or routing
       through a tag-variable if you need non-schema intermediates. */
    uint32_t allowed[TYPE_SLOTS_MAX * 4]; int na = 0; int have_either = 0;
    for (int s = 0; s < sig->slot_count; s++) {
        if (sig->slots[s].direction != DIR_OUT) continue;
        for (int e = 0; e < sig->slots[s].either_count; e++) {
            if (na < (int)(sizeof allowed / sizeof allowed[0])) allowed[na++] = sig->slots[s].either_syms[e];
            have_either = 1;
        }
    }
    if (have_either) {
        uint32_t s_tag = sym_intern("tag"), s_none = sym_intern("none");
        for (int i = start; i < end; i++) {
            uint32_t found = 0; int line = toks[i].line;
            if (toks[i].tag == TOK_WORD) {
                if (toks[i].as.sym == s_tag && i > start && toks[i-1].tag == TOK_SYM) found = toks[i-1].as.sym;
                else if (toks[i].as.sym == S_OK) found = S_OK;
                else if (toks[i].as.sym == S_NO || toks[i].as.sym == s_none) found = S_NO;
            }
            if (!found) continue;
            int ok = 0;
            for (int a = 0; a < na; a++) if (allowed[a] == found) { ok = 1; break; }
            if (!ok) {
                fprintf(stderr, "%s:%d: type error: body emits '%s tagged but declared either only allows {", src_files[LOC_FID(current_loc)], line, sym_name(found));
                for (int a = 0; a < na; a++) fprintf(stderr, "%s'%s", a ? " " : "", sym_name(allowed[a]));
                fprintf(stderr, "}\n");
                errors++;
            }
        }
    }
    return errors;
}
static int tc_is_copyable(AbstractType *t) { return !(t->flags & AT_LINEAR) && t->type != TC_BOX; }
static int tc_is_builtin(uint32_t sym, int prelude_sig_count) {
    for (int i = 0; i < prelude_sig_count; i++) if (type_sigs[i].sym == sym) return 1;
    return ho_ops_find(sym) != NULL;
}
static void tc_bind(TypeChecker *tc, uint32_t sym, AbstractType *atype, int line) {
    int i = 0; while (i < tc->bind_count && tc->bindings[i].sym != sym) i++;
    if (i == TC_BINDS_MAX) die("type checker: more than %d names bound at once", TC_BINDS_MAX);
    if (i == tc->bind_count) tc->bind_count++;
    tc->bindings[i] = (TCBinding){sym, *atype, line, 0};
}
static void tc_apply_scheme(TypeChecker *tc, TupleEffect *eff, int consumed, int produced,
                            TypeConstraint body_out, const char *name, int line, int fu) {
    int sc = eff->scheme_count, *map = calloc(sc ? sc : 1, sizeof(int));
    if (!map) die("out of memory instantiating %d type variables", sc);
    tvar_instantiate(tc, eff, map);
    for (int j = 0; j < eff->in_count && j < tc->sp; j++) {
        int stv = eff->in_tvars[j] - eff->scheme_base;
        if (stv >= 0 && stv < sc) { int ftv = map[stv], idx = tc->sp - eff->in_count + j;
            if (idx < 0) continue;
            AbstractType *inp = &tc->data[idx];
            if (tvar_unify_at(tc, ftv, inp))
                tc_error(tc, line, inp->source_line, "'%s' input type mismatch: expected %s, got %s (value from line %d)", name, constraint_name(tvar_resolve(tc, ftv)), constraint_name(inp->type != TC_NONE ? inp->type : tvar_resolve(tc, inp->tvar_id)), inp->source_line);
            if (fu && inp->tvar_id > 0) {
#define UNIFY_SUB(f) { int fa=tc->tvars[tvar_find(tc,ftv)].f, fb=tc->tvars[tvar_find(tc,inp->tvar_id)].f; if(fa>0&&fb>0)tvar_unify(tc,fa,fb); }
                UNIFY_SUB(elem) UNIFY_SUB(box_c) UNIFY_SUB(tag_p)
#undef UNIFY_SUB
            }
        }
    }
    tc_take(tc, consumed); int avail = tc->sp - tc->sp_floor; tc->sp -= (consumed <= avail) ? consumed : avail;
    for (int j = 0; j < eff->out_count; j++) {
        int stv = eff->out_tvars[j] - eff->scheme_base, ftv = (stv >= 0 && stv < sc) ? map[stv] : 0;
        if (ftv > 0) { TypeConstraint r = tvar_resolve(tc, ftv);
            if (tc_is_container(r)) {
                tc_push(tc, r, line);
                int fc = tvar_content(tc, ftv, r), oc = tvar_content(tc, tc->data[tc->sp-1].tvar_id, r);
                if (fc > 0 && oc > 0) tvar_unify(tc, oc, fc);
                int uid = tc->tvars[tvar_find(tc, ftv)].union_id;
                if (uid > 0) tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].union_id = uid;
            } else {
                tc_push(tc, TC_NONE, line); if (r != TC_NONE) tc->data[tc->sp-1].type = r;
                tc->data[tc->sp-1].tvar_id = ftv; if (fu && r == TC_BOX) tc->data[tc->sp-1].flags |= AT_LINEAR;
            }
        } else tc_push(tc, TC_NONE, line);
    }
    for (int j = eff->out_count; j < produced; j++) tc_push(tc, (j == produced - 1) ? body_out : TC_NONE, line);
    free(map);
}
static void tc_apply_ho(TypeChecker *tc, HOEffect *ho, int line) {
    int eff_c = 0, eff_p = 0, bk = 0, boe = -1; TypeConstraint bo = TC_NONE; TupleEffect *bteff = NULL;
    TypeConstraint bouts[8] = {0}; int bc = 0;
    int barr_c[8] = {0}, barr_p[8] = {0}, barr_has[8] = {0};
    AbstractType saved = {0}; int had_saved = 0;
    if ((tc->sp - tc->sp_floor) < ho->need) {
        tc_take(tc, ho->need);
        tc_error(tc, line, 0, "'%s' needs %d input(s), stack has %d", ho->name, ho->need, tc->sp - tc->sp_floor);
        tc->sp = tc->sp_floor;
        for (int j = 0; j < ho->out; j++) tc_push(tc, ho->out_type, line);
        return;
    }
    if (ho->flags & (HO_BOX_BORROW|HO_BOX_MUTATE)) {
        if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE) {
            if (tc->data[tc->sp-1].effect_idx >= 0) { TupleEffect *te = &tc->effects[tc->data[tc->sp-1].effect_idx]; if (te->opaque) tc->opaque_runs++; eff_c = te->consumed; eff_p = te->produced; bo = te->out_type; bk = 1; bteff = te; }
            tc->sp--;
        }
        if (tc->sp > 0 && tc->data[tc->sp-1].type != TC_BOX && tc->data[tc->sp-1].type != TC_NONE)
            tc_error(tc, line, 0, "'%s' expected box, got %s", ho->name, constraint_name(tc->data[tc->sp-1].type));
        if ((ho->flags & HO_BOX_BORROW) && tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
            TypeConstraint ct = tc_top_content(tc, TC_BOX);
            /* A snapshot shares the box's inner boxes and dicts by pointer, and a later
               mutate frees them, so binding one is refused. */
            if (bk && bteff && bteff->has_let && (ct == TC_BOX || ct == TC_DICT))
                tc_error(tc, line, 0, "'lend' body may not 'let'-bind the snapshot when the box contains a %s — the snapshot copies the pointer, not the contents, so a later 'mutate' would free it while the binding still refers to it. Read it out with `k peek` instead of binding it.", constraint_name(ct));
            int r = bk ? (1 - eff_c + eff_p) : 1; if (r < 0) r = 0;
            for (int j = 0; j < r; j++) tc_push(tc, (j==r-1&&bo!=TC_NONE)?bo:(j==0&&ct!=TC_NONE)?ct:TC_NONE, line);
        } else if ((ho->flags & HO_BOX_MUTATE) && tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
            TypeConstraint ct = tc_top_content(tc, TC_BOX);
            if (ct != TC_NONE && bo != TC_NONE && !tc_constraint_matches(ct, bo) && !tc_constraint_matches(bo, ct))
                tc_error(tc, line, 0, "'mutate' body produces %s but box contains %s", constraint_name(bo), constraint_name(ct));
        }
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
    TypeConstraint lpt = TC_NONE; int tptv = 0, lptv = 0;
    int branch_linear = 0; /* any popped body tuple captures or outputs linear */
    int body_captures_linear = 0; /* primary body tuple captures a linear outer binding */
    for (int n = ho->need; n > 0 && tc->sp > tc->sp_floor; n--) {
        AbstractType *top = &tc->data[tc->sp - 1];
        if ((ho->flags & HO_SAVES_UNDER) && n == 1) { saved = *top; had_saved = 1; tc->sp--; continue; }
        lpt = top->type; lptv = top->tvar_id;
        if (top->type == TC_TAGGED && top->tvar_id > 0 && !tptv) tptv = tvar_content(tc, top->tvar_id, TC_TAGGED);
        int ib = n > 1;
        if (top->type == TC_TUPLE && top->effect_idx >= 0) {
            TupleEffect *te = &tc->effects[top->effect_idx];
            if (te->opaque) tc->opaque_runs++;
            if (!bk) {
                eff_c = te->consumed; eff_p = te->produced; bo = te->out_type; bk = 1; boe = te->out_effect; bteff = te;
                if (top->flags & AT_LINEAR) body_captures_linear = 1;
            }
            if (te->output_is_linear || (top->flags & AT_LINEAR)) branch_linear = 1;
            if (ib && bc < 8) { barr_c[bc]=te->consumed; barr_p[bc]=te->produced; barr_has[bc]=1; bouts[bc++] = te->out_type; }
        } else if (ib && top->type != TC_NONE && bc < 8) bouts[bc++] = top->type;
        tc->sp--;
    }
    if ((ho->flags & HO_BODY_1TO1) && bk && (eff_c != 1 || eff_p != 1) && (eff_c + eff_p > 0))
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
        if (!nested) {
            int rnet = 0, rset = 0, rci = 0;
            for (int i = 0; i < bc; i++) if (barr_has[i]) { rnet = barr_p[i] - barr_c[i]; rci = i; rset = 1; break; }
            if (rset) for (int i = 0; i < bc; i++)
                if (barr_has[i] && (barr_p[i] - barr_c[i]) != rnet)
                    tc_error(tc, line, 0, "'%s' branches have different stack effects: net %+d vs net %+d (%d->%d vs %d->%d)", ho->name, rnet, barr_p[i]-barr_c[i], barr_c[rci], barr_p[rci], barr_c[i], barr_p[i]);
        }
    }
    if (ho->flags & HO_APPLY_EFFECT) {
        if (!bk) tc->opaque_runs++;
        if (bk) {
            tc_apply_effect(tc, eff_c, eff_p, bo, line);
            if (boe >= 0 && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE) tc->data[tc->sp-1].effect_idx = boe;
            /* apply of a body whose output is a linear-capturing closure must
               propagate AT_LINEAR onto the result — otherwise subsequent let+apply
               would lose the single-use property. */
            if (bteff && bteff->output_is_linear && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE)
                tc->data[tc->sp-1].flags |= AT_LINEAR;
        }
        if (had_saved) { tc_push(tc, TC_NONE, line); tc->data[tc->sp-1] = saved; }
        return;
    }
    if (ho->sym == S_IF && bk) {
        if (bteff && bteff->scheme_count > 0 && bteff->in_count > 0) tc_apply_scheme(tc, bteff, eff_c, eff_p, bo, "if", line, 0);
        else tc_apply_effect(tc, eff_c, eff_p, bo, line);
        /* If either branch body produces a linear-capturing closure, mark the
           result linear. Runtime dispatches one branch at a time, but static
           TC can't know which branch wins — both must be treated as tainted. */
        if (branch_linear && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE)
            tc->data[tc->sp-1].flags |= AT_LINEAR;
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
    if ((ho->flags & HO_BODY_1TO1) && out == TC_TAGGED && tc->sp > 0 && tc->data[tc->sp-1].tvar_id > 0) {
        int otp = tvar_content(tc, tc->data[tc->sp-1].tvar_id, TC_TAGGED);
        if (otp > 0) { if (bo != TC_NONE) tvar_bind(tc, otp, bo); else if (tptv > 0) tvar_unify(tc, otp, tptv); }
        if (lptv > 0) { int uid = tc->tvars[tvar_find(tc, lptv)].union_id; if (uid > 0) tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].union_id = uid; }
    }
}
static void tc_check_word(TypeChecker *tc, uint32_t sym, int line) {
    /* quote pushes a binding's raw value (no auto-exec). If that value is a
       linear-capturing closure, applying it twice double-consumes the captured
       linear. Mirror the consumed_line check done for ordinary lookup at the
       b->atype path below. */
    if (sym == S_QUOTE && tc->sp > 0 && tc->data[tc->sp-1].type == TC_SYM && tc->data[tc->sp-1].sym_id) {
        uint32_t target = tc->data[tc->sp-1].sym_id;
        TCBinding *qb = tc_lookup(tc, target);
        if (qb && (qb->atype.flags & AT_LINEAR) && qb->atype.type == TC_TUPLE) {
            if (qb->consumed_line > 0)
                tc_error(tc, line, qb->consumed_line, "linear-capturing closure '%s' has already been consumed (previous use on line %d) — 'quote' on a linear closure consumes it just like applying it", sym_name(target), qb->consumed_line);
            else qb->consumed_line = line;
        }
    }
    TypeSig *sig = typesig_find(sym);
    if (sig) goto apply_sig;
    { HOEffect *ho = ho_ops_find(sym); if (ho) { tc_apply_ho(tc, ho, line); return; } }
    { TCBinding *b = tc_lookup(tc, sym);
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
        if (b->atype.type == TC_TUPLE && b->atype.effect_idx >= 0) {
            TupleEffect *eff = &tc->effects[b->atype.effect_idx];
            if (eff->opaque) tc->opaque_runs++;
            if (eff->scheme_count > 0) tc_apply_scheme(tc, eff, eff->consumed, eff->produced, eff->out_type, sym_name(sym), line, 1);
            else tc_apply_effect(tc, eff->consumed, eff->produced, eff->out_type, line);
            /* Def returns a linear-capturing closure → mark output AT_LINEAR so
               the linear-closure single-use check catches second application. */
            if (eff->output_is_linear && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE)
                tc->data[tc->sp-1].flags |= AT_LINEAR;
            if (eff->out_effect >= 0 && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE)
                tc->data[tc->sp-1].effect_idx = eff->out_effect;
            return;
        } else {
            if (b->atype.flags & AT_OPAQUE) tc->opaque_runs++;
            tc_push(tc, b->atype.type, line);
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
    return;
apply_sig:;
    if (sym == S_THEN && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE && tc->data[tc->sp-1].effect_idx >= 0) {
        TypeConstraint bo = tc->effects[tc->data[tc->sp-1].effect_idx].out_type;
        if (bo != TC_NONE && !tc_constraint_matches(bo, TC_TAGGED) && !tc_constraint_matches(TC_TAGGED, bo))
            tc_error(tc, line, 0, "then: the body must produce a tagged value (ok or no), got %s", constraint_name(bo));
    }
    int ni = 0;
    for (int i = 0; i < sig->slot_count; i++) if (sig->slots[i].direction == DIR_IN) ni++;
    if (tc->sp < ni) {
        tc_take(tc, ni);
        tc_error(tc, line, 0, "'%s' needs %d input(s), stack has %d", sym_name(sym), ni, tc->sp);
        if (tc->sp > 0) { fprintf(stderr, "    stack (top first):"); for(int d=tc->sp-1;d>=0&&d>=tc->sp-5;d--) fprintf(stderr," %s",constraint_name(tc->data[d].type)); fprintf(stderr,"\n"); }
        return;
    }
    #define MAX_TVARS 16
    struct { uint32_t var; int tvar; uint32_t src_sym; int src_effect_idx; } tm[MAX_TVARS]; int tmc = 0;
    for (int i = 0; i < sig->slot_count; i++) {
        uint32_t tv = sig->slots[i].type_var; if (!tv) continue;
        int found = 0; for (int j = 0; j < tmc; j++) if (tm[j].var == tv) { found = 1; break; }
        if (!found) {
            if (tmc == MAX_TVARS) die("type checker: a signature uses more than %d type variables", MAX_TVARS);
            int id = tvar_fresh(tc); TypeConstraint c = sig->slots[i].constraint;
            if (!tc_is_container(c) && c != TC_NONE) tc->tvars[id].bound = c;
            if (tc_is_container(c) && sig->slots[i].elem_constraint != TC_NONE) tc->tvars[id].bound = sig->slots[i].elem_constraint;
            tm[tmc].var = tv; tm[tmc].tvar = id; tm[tmc].src_sym = 0; tm[tmc].src_effect_idx = -1; tmc++;
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
    for (int i = 0; i < ni && i < tc->sp; i++) {
        AbstractType *at = &tc->data[tc->sp-1-i];
        if (at->flags & AT_LINEAR) {
            any_input_linear = 1;
            if (at->type == TC_TUPLE) { any_input_linear_tuple = 1; break; }
        }
    }
    int sp2 = tc->sp - 1;
    for (int i = sig->slot_count - 1; i >= 0; i--) {
        TypeSlot *s = &sig->slots[i]; if (s->direction != DIR_IN || sp2 < 0) { if (s->direction == DIR_IN) sp2--; continue; }
        AbstractType *at = &tc->data[sp2];
        if (s->ownership == OWN_AUTO && (at->flags & AT_LINEAR)) { if (pt_count == 8) die("type checker: more than 8 auto slots"); passthrough[pt_count++] = *at; at->flags |= AT_CONSUMED; }
        if (s->ownership == OWN_COPY && !tc_is_copyable(at))
            tc_error(tc, line, at->source_line, "'%s' requires copyable value, got linear type (value from line %d)", sym_name(sym), at->source_line);
        if (s->constraint != TC_NONE && at->type != TC_NONE && !tc_constraint_matches(s->constraint, at->type))
            tc_error(tc, line, at->source_line, "'%s' expected %s, got %s (value from line %d)", sym_name(sym), constraint_name(s->constraint), constraint_name(at->type), at->source_line);
        if (s->constraint != TC_NONE && at->type == TC_NONE && at->tvar_id > 0) tvar_bind(tc, at->tvar_id, s->constraint);
        if (s->type_var) { int tv = FIND_TVAR(s->type_var); if (tv > 0) {
            if (tc_is_container(s->constraint) && at->tvar_id > 0) {
                int ef = tvar_content(tc, at->tvar_id, s->constraint);
                if (ef > 0 && tvar_unify(tc, tv, ef))
                    tc_error(tc, line, at->source_line, "'%s' type variable '%s' mismatch: expected %s, got %s", sym_name(sym), sym_name(s->type_var), constraint_name(tvar_resolve(tc, tv)), constraint_name(tvar_resolve(tc, ef)));
            } else if (!tc_is_container(s->constraint)) {
                int fail = at->tvar_id > 0 ? tvar_unify(tc, tv, at->tvar_id) : (at->type != TC_NONE ? tvar_bind(tc, tv, at->type) : 0);
                if (fail) tc_error(tc, line, at->source_line, "'%s' type variable '%s' mismatch: expected %s, got %s", sym_name(sym), sym_name(s->type_var), constraint_name(tvar_resolve(tc, tv)), constraint_name(at->type != TC_NONE ? at->type : tvar_resolve(tc, at->tvar_id)));
            }
            for (int j = 0; j < tmc; j++) if (tm[j].var == s->type_var) { if (at->sym_id) tm[j].src_sym = at->sym_id; if (at->effect_idx >= 0) tm[j].src_effect_idx = at->effect_idx; break; }
        }}
        /* An `either` INPUT slot names each variant's payload type. Bind those
           vars from the incoming union, so a later slot reusing the same var
           (default's fallback) must agree with the payload. Without this the
           fallback silently wins and `default`'s output takes its type. */
        if (s->either_count > 0 && at->tvar_id > 0) {
            int uid = tc->tvars[tvar_find(tc, at->tvar_id)].union_id;
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
    tc_take(tc, ni); tc->sp -= ni; if (tc->sp < tc->sp_floor) tc->sp = tc->sp_floor;
    for (int i = pt_count - 1; i >= 0; i--) {
        tc_push(tc, passthrough[i].type, line);
        AbstractType *o = &tc->data[tc->sp-1];
        o->tvar_id = passthrough[i].tvar_id; o->flags |= AT_LINEAR;
    }
    for (int i = 0; i < sig->slot_count; i++) {
        TypeSlot *s = &sig->slots[i]; if (s->direction != DIR_OUT) continue;
        tc_push(tc, s->constraint, line); AbstractType *at = &tc->data[tc->sp - 1];
        /* Propagate AT_LINEAR onto a sig-declared tuple output when an input
           tuple was AT_LINEAR. Covers `compose`: merging two closures where
           one captures linear must yield a linear-capturing closure. */
        if (any_input_linear_tuple && s->constraint == TC_TUPLE) at->flags |= AT_LINEAR;
        if (any_input_linear && s->constraint == TC_TAGGED) at->flags |= AT_LINEAR;
        if (s->type_var) { int tv = FIND_TVAR(s->type_var); if (tv > 0) {
            if (tc_is_container(s->constraint) && at->tvar_id > 0) { int ef = tvar_content(tc, at->tvar_id, s->constraint); if (ef > 0) tvar_unify(tc, ef, tv); }
            else if (!tc_is_container(s->constraint)) { TypeConstraint r = tvar_resolve(tc, tv); if (r != TC_NONE) at->type = r; at->tvar_id = tv; if (r == TC_BOX) at->flags |= AT_LINEAR; }
        }}
        if (tc_is_container(s->constraint) && s->elem_constraint != TC_NONE && at->tvar_id > 0) { int ef = tvar_content(tc, at->tvar_id, s->constraint); if (ef > 0) tvar_bind(tc, ef, s->elem_constraint); }
        if (s->type_var) for (int j = 0; j < tmc; j++) if (tm[j].var == s->type_var) { if (tm[j].src_sym && !tc_is_container(s->constraint)) at->sym_id = tm[j].src_sym; if (tm[j].src_effect_idx >= 0 && !tc_is_container(s->constraint)) at->effect_idx = tm[j].src_effect_idx; break; }
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
            tc->tvars[tvar_find(tc, at->tvar_id)].union_id = uid;
        }
    }
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
/* Check a (...) body at toks[i..close] in its own scope and push the tuple it makes. */
static void tc_tuple(TypeChecker *tc, Token *toks, int i, int close, int total_count) {
    Token *t = &toks[i];
    int eff_c = 0, eff_p = 0;
    TypeConstraint eff_out = tc_infer_effect(toks, i+1, close, &eff_c, &eff_p, tc, NULL, 0);
    /* Auto-visible: if this tuple is followed by `'name let`, make
       name visible inside the body so recursive references
       typecheck. Binding order: `(body) 'name let`. */
    if (!tc->recur_pending && close+2 < total_count
        && toks[close+1].tag == TOK_SYM
        && toks[close+2].tag == TOK_WORD && toks[close+2].as.sym == S_LET) {
        uint32_t name_sym = toks[close+1].as.sym;
        for (int j = i+1; j < close; j++) {
            if (toks[j].tag == TOK_WORD && toks[j].as.sym == name_sym) {
                tc->recur_pending = 1; tc->recur_sym = name_sym; break;
            }
        }
    }
    /* A declared signature, `(body) [sig] effect` or an earlier `'name [sig] effect`,
       is checked against the body itself: it runs on exactly the declared inputs,
       and must leave exactly the declared outputs. */
    TypeSig dsig; int has_sig = 0, n_in = 0, n_out = 0; const char *who = "body";
    TypeConstraint in_c[TYPE_SLOTS_MAX], in_elem[TYPE_SLOTS_MAX];
    if (close+2 < total_count && toks[close+1].tag == TOK_LBRACKET) {
        int be = close+1+toks[close+1].span;
        if (be+1 < total_count && toks[be+1].tag == TOK_WORD && toks[be+1].as.sym == S_EFFECT) {
            dsig = parse_type_annotation(toks, close+2, be); has_sig = 1;
            if (be+3 < total_count && toks[be+2].tag == TOK_SYM && toks[be+3].tag == TOK_WORD && toks[be+3].as.sym == S_LET) who = sym_name(toks[be+2].as.sym);
        }
    } else if (i >= tc->user_start && close+2 < total_count && toks[close+1].tag == TOK_SYM
               && toks[close+2].tag == TOK_WORD && toks[close+2].as.sym == S_LET) {
        TypeSig *f = typesig_find(toks[close+1].as.sym);
        if (f && !tc_is_builtin(toks[close+1].as.sym, tc->prelude_sig_count)) { dsig = *f; has_sig = 1; who = sym_name(toks[close+1].as.sym); }
    }
    if (has_sig) {
        for (int k = 0; k < dsig.slot_count; k++)
            if (dsig.slots[k].direction == DIR_IN) { in_c[n_in] = dsig.slots[k].constraint; in_elem[n_in++] = dsig.slots[k].elem_constraint; }
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
        for (int j = 0; j < ic; j++) {
            itv[j] = tvar_fresh(tc); tc->tvars[itv[j]].elem = tvar_fresh(tc); tc->tvars[itv[j]].box_c = tvar_fresh(tc); tc->tvars[itv[j]].tag_p = tvar_fresh(tc);
            tc_push(tc, TC_NONE, t->line); tc->data[tc->sp-1].tvar_id = itv[j];
            /* A tuple input is code whose effect the signature does not state. */
            if (has_sig && in_c[j] == TC_TUPLE) tc->data[tc->sp-1].flags |= AT_OPAQUE;
            if (has_sig && in_c[j] != TC_NONE && in_c[j] != TC_TUPLE) {
                tc->data[tc->sp-1].type = in_c[j]; tc->tvars[itv[j]].bound = in_c[j];
                if (in_c[j] == TC_BOX) tc->data[tc->sp-1].flags |= AT_LINEAR;
                if (in_elem[j] != TC_NONE) tc->tvars[tc->tvars[itv[j]].elem].bound = in_elem[j];
            }
        }
        if (wide) for (int j = 0; j < eff_c; j++) tc_push(tc, TC_NONE, t->line);
        if (tc->recur_pending && tc->recur_sym) {
            int pe = tc_alloc_effect(tc); tc->effects[pe].consumed = eff_c; tc->effects[pe].produced = eff_p; tc->effects[pe].out_type = eff_out;
            AbstractType pa = {0}; pa.type = TC_TUPLE; pa.effect_idx = pe; tc_bind(tc, tc->recur_sym, &pa, t->line);
        }
        /* Inside the body, recur_pending must be 0 so nested defs don't
           mis-attribute to recur_sym. _s.recur_pending restores it at exit
           so the outer def post-body still consumes it. */
        tc->recur_pending = 0;
        int u0 = tc->underflows, o0 = tc->opaque_runs;
        tc->body_depth++; tc_process_range(tc, toks, i+1, close, total_count); tc->body_depth--;
        int under = tc->underflows - u0, opaque = tc->opaque_runs > o0; tc->underflows = u0; tc->opaque_runs = o0;
        /* What the body really took and left, rather than the pre-scan's guess. */
        if (!has_sig) { eff_c = (wide ? eff_c : ic) + under; eff_p = tc->sp - _s.sp; }
        else {
            int left = tc->sp - _s.sp;
            if (under) tc_error(tc, t->line, 0, "'%s' reaches below the %d input(s) its type declares", who, n_in);
            /* After code of unknown effect runs, only the caller knows what the body leaves. */
            else if (opaque) {}
            else if (left != n_out) tc_error(tc, t->line, 0, "'%s' leaves %d value(s) but its type declares %d output(s)", who, left, n_out);
            else for (int k = 0, o = 0; k < dsig.slot_count; k++) {
                if (dsig.slots[k].direction != DIR_OUT) continue;
                AbstractType *at = &tc->data[_s.sp + o++];
                TypeConstraint want = dsig.slots[k].constraint, got = at->type != TC_NONE ? at->type : at->tvar_id > 0 ? tvar_resolve(tc, at->tvar_id) : TC_NONE;
                if (want != TC_NONE && got != TC_NONE && !tc_constraint_matches(want, got) && !tc_constraint_matches(got, want))
                    tc_error(tc, t->line, 0, "'%s' output %d is %s but its type declares %s", who, o, constraint_name(got), constraint_name(want));
            }
        }
        if (!wide) {
            int ao = tc->sp - _s.sp; oc = ao > 16 ? 16 : (ao > 0 ? ao : 0);
            for (int j = 0; j < oc; j++) { int idx = tc->sp - oc + j; if (idx < 0) continue;
                otv[j] = tc->data[idx].tvar_id;
                if (!otv[j]) { otv[j] = tvar_fresh(tc); if (tc->data[idx].type != TC_NONE) tc->tvars[otv[j]].bound = tc->data[idx].type; } }
            sc = tc->tvar_count - scheme_base;
            if (ao == 1 && tc->data[tc->sp-1].type == TC_TUPLE && tc->data[tc->sp-1].effect_idx >= 0) out_eff = tc->data[tc->sp-1].effect_idx;
            /* The observed top type is more precise than the pre-scan's. */
            if (ao > 0 && tc->data[tc->sp-1].type != TC_NONE) eff_out = tc->data[tc->sp-1].type;
        }
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
        eff->output_is_linear = output_captures_linear; eff->opaque = opaque && !has_sig;
        for (int j = 0; j < ic; j++) eff->in_tvars[j] = itv[j];
        for (int j = 0; j < oc; j++) eff->out_tvars[j] = otv[j];
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
        if (i == tc->user_start && !tc->prelude_sig_count) tc->prelude_sig_count = type_sig_count;
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
            if (!is_type_annot) {
                for (int j = i+1; j < close; j++) {
                    if (toks[j].tag == TOK_LPAREN) { j = (j+toks[j].span); continue; }
                    if (toks[j].tag == TOK_LBRACKET) { j = (j+toks[j].span); continue; }
                    if (toks[j].tag == TOK_LBRACE) { j = (j+toks[j].span); continue; }
                    if (toks[j].tag == TOK_WORD && tc_word_produces_linear(toks[j].as.sym))
                        tc_error(tc, t->line, 0, "list literal cannot contain linear values produced by '%s'", sym_name(toks[j].as.sym));
                }
            }
            if (!is_type_annot) {
                int s0 = tc->sp, f0 = tc->sp_floor; tc->sp_floor = tc->sp;
                tc_process_range(tc, toks, i+1, close, total_count);
                tc->sp = s0; tc->sp_floor = f0;
            }
            TypeConstraint elem = tc_check_list_elements(tc, toks, i+1, close, t->line);
            tc_push(tc, TC_LIST, t->line);
            if (elem != TC_NONE && tc->data[tc->sp-1].tvar_id > 0) {
                int ev = tvar_content(tc, tc->data[tc->sp-1].tvar_id, TC_LIST);
                if (ev > 0) tvar_bind(tc, ev, elem);
            }
            i = close; break;
        }
        case TOK_LBRACE: {
            int close = (i+toks[i].span);
            TypeConstraint vtype = TC_NONE, veffout = TC_NONE;
            int pairs = 0, has_eff = 0;
            for (int j = i+1; j < close; ) {
                if (toks[j].tag == TOK_LPAREN) j = (j+toks[j].span) + 1;
                else j++;
                if (j >= close) break;
                TypeConstraint tv = TC_NONE;
                if (toks[j].tag == TOK_LPAREN) {
                    int vc2 = (j+toks[j].span);
                    int vc = 0, vp = 0; TypeConstraint vo = tc_infer_effect(toks, j+1, vc2, &vc, &vp, tc, NULL, 0);
                    tv = TC_TUPLE; if (!pairs) { veffout = vo; has_eff = 1; }
                    if (has_eff && vo != TC_NONE && veffout != TC_NONE && vo != veffout && !tc_constraint_matches(veffout, vo) && !tc_constraint_matches(vo, veffout))
                        tc_error(tc, t->line, 0, "clause bodies produce different types: %s vs %s", constraint_name(veffout), constraint_name(vo));
                    /* A case clause body that creates a linear closure
                       escapes conditional dispatch — the resulting value can be
                       applied twice without detection. Ban box creation nested in
                       clause bodies. Runtime catches double-free but we'd rather
                       surface this at TC. */
                    tc_tuple(tc, toks, j, vc2, total_count); tc->sp--;
                    for (int k = j+1; k < vc2; k++) {
                        if (toks[k].tag == TOK_LPAREN) { k = (k+toks[k].span); continue; }
                        if (toks[k].tag == TOK_LBRACKET) { k = (k+toks[k].span); continue; }
                        if (toks[k].tag == TOK_LBRACE) { k = (k+toks[k].span); continue; }
                        if (toks[k].tag == TOK_WORD && tc_word_produces_linear(toks[k].as.sym))
                            tc_error(tc, t->line, 0, "clause body may not produce a linear value (via '%s') — linear-capturing closures from conditional branches cannot be tracked for single-use", sym_name(toks[k].as.sym));
                    }
                    j = vc2 + 1;
                } else {
                    if (toks[j].tag == TOK_INT) tv = TC_INT; else if (toks[j].tag == TOK_FLOAT) tv = TC_FLOAT;
                    else if (toks[j].tag == TOK_SYM) tv = TC_SYM; else if (toks[j].tag == TOK_STRING) tv = TC_LIST;
                    else if (toks[j].tag == TOK_WORD && tc_word_produces_linear(toks[j].as.sym))
                        tc_error(tc, t->line, 0, "record literal cannot contain linear values produced by '%s'", sym_name(toks[j].as.sym));
                    j++;
                }
                if (!pairs && tv != TC_NONE) vtype = tv;
                else if (tv != TC_NONE && vtype != TC_NONE && tv != vtype && !tc_constraint_matches(vtype, tv) && !tc_constraint_matches(tv, vtype))
                    tc_error(tc, t->line, 0, "clause values have inconsistent types: %s vs %s", constraint_name(vtype), constraint_name(tv));
                pairs++;
            }
            if (vtype != TC_TUPLE && !has_eff) tc_push(tc, TC_REC, t->line);
            else {
                tc_push(tc, TC_TUPLE, t->line);
                if (has_eff) { int eidx = tc_alloc_effect(tc); tc->effects[eidx].out_type = veffout; tc->data[tc->sp-1].effect_idx = eidx; }
            }
            i = close; break;
        }
        case TOK_WORD: {
            uint32_t sym = t->as.sym;
            if (sym == S_LET) {
                if (tc->sp < 2 || tc->data[tc->sp-1].type != TC_SYM || !tc->data[tc->sp-1].sym_id) {
                    tc_error(tc, t->line, 0, "let: expected a value and then a 'name, as in `42 'x let`");
                    tc->sp = tc->sp_floor;
                } else {
                    tc_take(tc, 2);
                    uint32_t ns = tc->data[tc->sp-1].sym_id; AbstractType vt = tc->data[tc->sp-2]; tc->sp -= 2;
                    int is_recur = tc->recur_pending && tc->recur_sym == ns;
                    if (tc->recur_pending) tc->recur_pending = 0;
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
                        if (i >= tc->user_start && vt.type == TC_TUPLE && i-2 >= start && toks[i-2].tag == TOK_RPAREN) {
                            TypeSig *fsig = typesig_find(ns);
                            if (fsig && !tc_is_builtin(ns, tc->prelude_sig_count)) {
                                int b2 = i-2;
                                tc->errors += tc_check_either_tags(toks, b2 + toks[b2].span + 1, b2, fsig);
                            }
                        }
                        tc_bind(tc, ns, &vt, t->line);
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
                        /* `(body) [sig] effect 'name let` registers sig for name. */
                        if (i+2 < total_count && toks[i+1].tag == TOK_SYM
                            && toks[i+2].tag == TOK_WORD && toks[i+2].as.sym == S_LET)
                            typesig_register(toks[i+1].as.sym, &sig);
                        /* Body is the RPAREN-terminated tuple immediately before the
                           [sig] brackets, which start at `bs`. Step from bs-1 backward
                           for RPAREN, then find matching LPAREN, skipping any nested
                           brackets/braces so we don't latch onto `()` inside the sig's
                           either schema. */
                        int b2 = bs - 1;
                        if (b2 >= 0 && toks[b2].tag == TOK_RPAREN)
                            tc->errors += tc_check_either_tags(toks, b2 + toks[b2].span + 1, b2, &sig);
                    } else if (tc->sp >= 1 && tc->data[tc->sp-1].type == TC_SYM) {
                        /* BUILTIN_TYPES prim registration: `'name [sig] effect` (no def). */
                        typesig_register(tc->data[tc->sp-1].sym_id, &sig);
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
                /* Predicate-mode lint: when the scrutinee isn't tagged, clause bodies
                   run with the scrutinee on top of stack. An empty body `()` preserves
                   the scrutinee as output; a body like `(42)` replaces it. Mixing the
                   two produces branches with different output types and breaks the
                   HO_BRANCHES_AGREE check (because the outer `{...}` effect hides
                   per-clause shape). Require all bodies to be consistently empty or
                   consistently non-empty. */
                if (tc->sp >= 3 && tc->data[tc->sp-3].type != TC_TAGGED) {
                    int brace = tc_find_brace_before(toks, i);
                    if (brace >= 0) {
                        int cl = (brace+toks[brace].span);
                        int pair = 0, empty_body = 0, nonempty_body = 0;
                        for (int j = brace + 1; j < cl; ) {
                            if (toks[j].tag == TOK_LPAREN) {
                                int close = (j+toks[j].span);
                                if (pair % 2 == 1) { /* body slot */
                                    if (close == j + 1) empty_body = 1; else nonempty_body = 1;
                                }
                                j = close + 1; pair++;
                            } else j++;
                        }
                        if (empty_body && nonempty_body)
                            tc_error(tc, t->line, 0, "'case' clause bodies have inconsistent stack effects: empty body `()` preserves scrutinee as output while non-empty bodies replace it. Make all bodies empty, or rewrite empty bodies to return the scrutinee explicitly (e.g., `(dup)` or a typed identity).");
                    }
                }
                /* Exhaustiveness: when scrutinee has a declared union, require every variant to appear as a clause. */
                if (tc->sp >= 3 && tc->data[tc->sp-3].type == TC_TAGGED && tc->data[tc->sp-3].tvar_id > 0) {
                    int uid = tc->tvars[tvar_find(tc, tc->data[tc->sp-3].tvar_id)].union_id;
                    if (uid > 0) {
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
            } else tc_check_word(tc, sym, t->line);
            break;
        }
        default: break;
        }
        if (tc->body_depth == 0 && tc->effect_count > EFFECT_MAX/2) {
            int ml = 0;
            for (int s = 0; s < tc->sp; s++) if (tc->data[s].effect_idx >= ml) ml = tc->data[s].effect_idx + 1;
            for (int b = 0; b < tc->bind_count; b++) if (tc->bindings[b].atype.effect_idx >= ml) ml = tc->bindings[b].atype.effect_idx + 1;
            int grew = 1; while (grew) { grew = 0;
                for (int e = 0; e < ml; e++) { int oe = tc->effects[e].out_effect;
                    if (oe >= ml && oe < tc->effect_count) { ml = oe + 1; grew = 1; } } }
            if (ml < tc->effect_count) tc->effect_count = ml;
        }
    }
}
static int typecheck_tokens(Token *toks, int count, int user_start) {
    /* static: the TypeChecker is megabytes, and -flto inlines this into main(),
       whose frame lives for the whole run. */
    static TypeChecker tc; memset(&tc, 0, sizeof(tc)); tc.tvar_count = 1; tc.user_start = user_start;
    tc_process_range(&tc, toks, 0, count, count);
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
        uint32_t sym = tc.unknowns[i].sym; int defined = 0;
        for (int j = 0; j < tc.bind_count; j++) if (tc.bindings[j].sym == sym) { defined = 1; break; }
        if (!defined) tc_error(&tc, tc.unknowns[i].line, 0, "unknown word '%s'", sym_name(sym));
    }
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
static void prim_dup(Frame *e) { (void)e; if (sp<=0) die("dup: stack underflow"); Value top=stack[sp-1]; if(top.tag<=VAL_XT){spush(top);return;} int s=val_slots(top); if(sp+s>STACK_MAX) die("dup: stack overflow"); deep_copy_values(&stack[sp],&stack[sp-s],s); sp+=s; }
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
    if(sp+b-a>STACK_MAX) die("over: stack overflow");
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
    Binding *b=lu.bind; SPUSH(b->vals,b->slots);
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
    POP_VAL(el); POP_BODY(then,"if");
    Value cond=spop(); if(cond.tag!=VAL_INT) die("if: condition must be int, got %s",valtag_name(cond.tag));
    if(cond.as.i) { if(el_top.tag!=VAL_TUPLE) deep_free_values(el_buf,el_s); eval_body(then_buf,then_s,env); }
    else if(el_top.tag==VAL_TUPLE) eval_body(el_buf,el_s,env);
    else SPUSH(el_buf,el_s);
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
        ElemRef br = {0}; uint32_t tag_sym=top.as.compound.len; int found = 0;
        if (clauses_top.tag == VAL_RECORD) br = record_field(clauses_buf,clauses_s,clauses_len,tag_sym,&found);
        else {
            if(clauses_len%2!=0) die("case: need even number of clauses");
            for(int i=0;i<clauses_len&&!found;i+=2){
                ElemRef pr=compound_elem(clauses_buf,clauses_s,clauses_len,i);
                if(clauses_buf[pr.base].tag==VAL_SYM&&clauses_buf[pr.base].as.sym==tag_sym){ br=compound_elem(clauses_buf,clauses_s,clauses_len,i+1); found=1; }
            }
        }
        if (found) { deep_free_values(def_buf,def_s); sp--; eval_body(&clauses_buf[br.base],br.slots,env); return; }
        int ts=val_slots(top); deep_free_values(&stack[sp-ts],ts); sp-=ts;
        SPUSH(def_buf,def_s); return;
    }
    if(clauses_len%2!=0) die("case: need even number of clauses (pred/body pairs)");
    POP_VAL(scrut);
    for(int i=0;i<clauses_len;i+=2){
        ElemRef pred_ref=compound_elem(clauses_buf,clauses_s,clauses_len,i);
        if(sp+scrut_s>STACK_MAX) die("case: stack overflow");
        deep_copy_values(&stack[sp],scrut_buf,scrut_s); sp+=scrut_s;
        eval_body(&clauses_buf[pred_ref.base],pred_ref.slots,env);
        if(pop_int()){ElemRef br=compound_elem(clauses_buf,clauses_s,clauses_len,i+1);
            deep_free_values(def_buf,def_s); SPUSH(scrut_buf,scrut_s); eval_body(&clauses_buf[br.base],br.slots,env); return;}
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
/* tagged default (body) pthen: on 'ok run body on the payload, else leave default under the tagged. */
static void prim_pthen(Frame *env) {
    POP_BODY(body,"pthen"); POP_VAL(def);
    if(sp<=0) die("pthen: stack underflow");
    Value top=stack[sp-1];
    if(top.tag!=VAL_TAGGED) die("pthen: expected tagged, got %s",valtag_name(top.tag));
    if(top.as.compound.len==S_OK){ deep_free_values(def_buf,def_s); sp--; eval_body(body_buf,body_s,env); return; }
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
    if(sp+ref.slots>STACK_MAX) die("peek: stack overflow");
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
    if(sp+ref.slots>STACK_MAX) die("nth: stack overflow");
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
static void prim_range(Frame *e){(void)e;int64_t end=pop_int(),start=pop_int();int count=0;for(int64_t i=start;i<end;i++){spush(val_int(i));count++;}spush(val_compound(VAL_LIST,count,count+1));}
static void push_string_bytes(const char *buf, int len);
static void dict_put(DictData *dd, const char *key, int klen, Value *vals, int nvals);
static Value dict_val(DictData *dd);
static void dict_push_kv_tuple(DictEntry *e) {
    int key_s = e->klen + 1;
    push_string_bytes(e->key, e->klen);
    if(sp+e->nvals>STACK_MAX) die("stack overflow");
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
            int p0=sp; dict_push_kv_tuple(e);
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
            dict_push_kv_tuple(e); eval_body(fn_buf,fn_s,env); one_value_above(p0,"fold","the accumulator and one entry");
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
static inline void prim_at_impl(Frame *env, int tagged) {
    (void)env; uint32_t key=pop_sym();
    if(sp<=0) die("at: stack underflow"); Value next=stack[sp-1];
    if(next.tag!=VAL_RECORD) die("at: expected record, got %s",valtag_name(next.tag));
    int s=val_slots(next),len=(int)next.as.compound.len,base=sp-s;
    int found; ElemRef ref=record_field(&stack[base],s,len,key,&found);
    if(!found) { sp-=s; if(tagged) push_none(); else die("at: key '%s' not found in record",sym_name(key)); return; }
        memmove(&stack[base],&stack[base+ref.base],ref.slots*sizeof(Value));
    sp=base+ref.slots; if(tagged) push_ok();
}
MUST_PAIR(at)
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
static inline void prim_edit_impl(Frame *env, int tagged) {
    POP_BODY(fn,"edit"); uint32_t key=pop_sym(); REC_PREAMBLE("edit");
    int found; ElemRef ref=record_field(&stack[rec_base],rec_s,rec_len,key,&found);
    if(!found) { deep_free_values(&stack[rec_base],rec_s); sp=rec_base; if(tagged) push_none(); else die("edit: key '%s' not found in record",sym_name(key)); return; }
    if(sp+ref.slots>STACK_MAX) die("edit: stack overflow");
    deep_copy_values(&stack[sp],&stack[rec_base+ref.base],ref.slots); sp+=ref.slots;
    eval_body(fn_buf,fn_s,env);
    rec_put(key,"edit");
    if(tagged) push_ok();
}
MUST_PAIR(edit)
typedef struct BoxData { Value *data; int slots; } BoxData;
static void prim_box(Frame *e){(void)e;Value top=speek();int s=val_slots(top);BoxData *bd=malloc(sizeof(BoxData));bd->data=malloc(s*sizeof(Value));bd->slots=s;deep_copy_values(bd->data,&stack[sp-s],s);sp-=s;Value v;v.tag=VAL_BOX;v.loc=0;v.as.box=bd;spush(v);}
static void prim_free(Frame *e){
    (void)e;Value v=spop();
    if(v.tag==VAL_BOX){BoxData *bd=(BoxData*)v.as.box;if(!bd->data)die("free: double-free detected (box already freed, likely captured by a closure that ran twice)");free(bd->data);bd->data=NULL;bd->slots=-1;return;}
    die("free: expected box, got %s", valtag_name(v.tag));
}
#define BOX_UNPACK(who) POP_BODY(fn,who); Value box_val=spop(); if(box_val.tag!=VAL_BOX) die(who ": expected box, got %s", valtag_name(box_val.tag)); \
    BoxData *bd=(BoxData*)box_val.as.box; if(!bd->data) die(who ": the box was already freed"); if(sp+bd->slots>STACK_MAX) die(who ": stack overflow")
/* The body reads a deep copy; the box keeps its own. */
static void prim_lend(Frame *env) {
    BOX_UNPACK("lend"); int sp0=sp;
    deep_copy_values(&stack[sp],bd->data,bd->slots); sp+=bd->slots;
    eval_body(fn_buf,fn_s,env);
    if(sp<sp0) die("lend: the body consumed %d value(s) from below the box's contents", sp0-sp);
    if(sp+1>STACK_MAX) die("lend: stack overflow");
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
static void prim_dict(Frame *e){(void)e;DictData *dd=calloc(1,sizeof(DictData));spush(dict_val(dd));}
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
    if(sp+ent->nvals>STACK_MAX) die("of: stack overflow");
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
        if(!cf){ cf=frame_new(ee); for(int j=sbc;j<ee->bind_count;j++) frame_bind(cf,ee->bindings[j].sym,ee->bindings[j].vals,ee->bindings[j].slots); }
        stack[i-1].as.compound.env=cf;
    }
    for(int p=saves_sp-1;p>=sv0;p--){
        Binding *b=&ee->bindings[saves[p].bi];
        free(b->vals); b->vals=saves[p].vals; b->slots=saves[p].slots; b->cap=saves[p].cap;
    }
    saves_sp=sv0;
    frame_trim(ee,sbc);
}
static void dispatch_word(uint32_t sym, Frame *env) {
    Lookup lu=frame_lookup(env,sym);
    if(!lu.bind) die("unknown word: %s",sym_name(sym));
    Binding *b=lu.bind; Value *v=b->vals; int s=b->slots;
    if(v[s-1].tag!=VAL_TUPLE){ SPUSH(v,s); return; }
    Frame *f=lu.frame; int bi=(int)(b-f->bindings);
    b->pinned++; eval_tuple_scoped(v,s,env); f->bindings[bi].pinned--;
}
static void eval_body(Value *body, int slots, Frame *env) {
    if(++eval_depth > EVAL_DEPTH_MAX) die("recursion depth exceeded (%d levels)", EVAL_DEPTH_MAX);
    c_stack_check("in a nested call");
    Value hdr=body[slots-1]; if(hdr.tag!=VAL_TUPLE) die("eval_body: expected tuple, got %s (internal: evaluator received non-tuple header)", valtag_name(hdr.tag));
    int len=(int)hdr.as.compound.len;
    Frame *ee=hdr.as.compound.env?hdr.as.compound.env:env;
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
                sp-=ds; frame_bind(ee,n,&stack[sp],ds);
            }
            else dispatch_word(ep->as.xt.sym,ee);
            asp=a1;
        } else if(is_compound(ep->tag)){
            SPUSH(&body[eo],es);
            if(ep->tag==VAL_TUPLE){ee->refcount++;stack[sp-1].as.compound.env=ee;}
        } else spush(*ep);
    }
    if(ee->refcount==0) frame_trim(ee,sbc);
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
                if(prim_must_fns[s] && j+1<end && toks[j+1].tag==TOK_WORD && toks[j+1].as.sym==S_MUST){spush(with_tok(val_xt(s,prim_must_fns[s]),tt));ec++;j++;break;}
                spush(with_tok(val_xt(s,prim_fns[s]),tt));ec++;
            }
            break;
        case TOK_STRING:
            for(int c=0;c<tt->as.str.len;c++) spush(with_tok(val_int(tt->as.str.codes[c]),tt));
            spush(with_tok(val_compound(VAL_LIST,tt->as.str.len,tt->as.str.len+1),tt)); ec++; break;
        case TOK_LPAREN:{int nc=(j+toks[j].span);build_tuple(toks,j+1,nc,tc,env);stack[sp-1].loc=LOC_PACK(tt->fid,tt->line,tt->col);ec++;j=nc;break;}
        case TOK_LBRACKET:{
            int bc=(j+toks[j].span);
            if(bc+1<tc&&toks[bc+1].tag==TOK_WORD&&toks[bc+1].as.sym==S_EFFECT){j=bc+1;break;}
            int lb=sp; eval(toks+j+1,bc-j-1,env);
            int n=0,p=sp; while(p>lb){p-=val_slots(stack[p-1]);n++;}
            spush(with_tok(val_compound(VAL_LIST,n,sp-lb+1),tt)); ec++; j=bc; break;
        }
        case TOK_LBRACE:{
            int bc=(j+toks[j].span);
            int lb=sp; eval(toks+j+1,bc-j-1,env); int ts=sp-lb,nf=0,ir=1,p=sp;
            while(p>lb){int vs=val_slots(stack[p-1]);p-=vs;if(ir&&p>lb&&stack[p-1].tag==VAL_SYM){p--;nf++;}else ir=0;}
            if(ir&&nf>0) spush(with_tok(val_compound(VAL_RECORD,nf,ts+1),tt));
            else{int n=0;p=sp;while(p>lb){p-=val_slots(stack[p-1]);n++;}spush(with_tok(val_compound(VAL_TUPLE,n,ts+1),tt));}
            ec++; j=bc; break;
        }
        default: break;
        }
    }
    Value hdr=val_compound(VAL_TUPLE,ec,sp-eb+1); if(ft) hdr.loc=LOC_PACK(ft->fid,ft->line,ft->col);
    spush(hdr); if(env) env->refcount++; stack[sp-1].as.compound.env=env;
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
    "'zip ['a list own in  'a list own in  list" MO
    "'read [list own in  {'ok list 'no list} either move out] effect\n'write [list own in  int list own in  {'ok int 'no list} either move out] effect\n'ls [list own in  {'ok list 'no list} either move out] effect\n"
    "'str-find [int list own in  int list own in  {'ok int 'no ()} either move out] effect\n"
    "'str-split [int list own in  int list own in  list" MO "'parse-http [int list own in  {'ok rec 'no list} either move out] effect\n'args [list" MO "'isheadless [int" MO
#ifndef SLAP_WASM
    "'tcp-connect [int list own in  int lent in  {'ok box 'no list} either move out] effect\n'tcp-send [int box own in  int list own in  int box move out  {'ok int 'no list} either move out] effect\n"
    "'tcp-recv [int box own in  int lent in  int box move out  {'ok list 'no list} either move out] effect\n'tcp-close [int box own in] effect\n'tcp-listen [int lent in  {'ok box 'no list} either move out] effect\n'tcp-accept [int box own in  int box move out  {'ok box 'no list} either move out] effect\n"
#endif
    "'tag ['a own in  sym lent in  'a tagged" MO "'must [tagged own in " MO
    "'dict ['a dict" MO "'insert ['a dict own in  list lent in  'a own in  'a dict" MO
    "'of ['a dict own in  list lent in  'a dict move out  {'ok 'a 'no list} either move out] effect\n"
    "'remove ['a dict own in  list lent in  'a dict" MO "'dict-keys ['a dict own in  'a dict move out  list" MO
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
    "(swap lt) [lent in  lent in  int move out] effect 'gt let\n"
    "(lt not) [lent in  lent in  int move out] effect 'ge let\n"
    "(swap lt not) [lent in  lent in  int move out] effect 'le let\n"
    "(1 plus) [num lent in  num move out] effect 'inc let\n"
    "(1 sub) [num lent in  num move out] effect 'dec let\n"
    "(0 swap sub) [num lent in  num move out] effect 'neg let\n"
    "(over over lt (nip) (drop) if) ['a ord lent in  'a ord lent in  'a ord move out] effect 'max let\n"
    "(over over lt (drop) (nip) if) ['a ord lent in  'a ord lent in  'a ord move out] effect 'min let\n"
    "(dup 0 lt (neg) (dup drop) if) 'abs let\n"
    "('f let (dup 0 gt) (1 sub (f) dip) while drop) 'repeat let\n"
    "('p let list (dup p (push) (drop) if) fold) ['a list own in  tuple own in  'a list move out] effect 'filter let\n"
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
    "('body let () {'ok (body) 'no (no)} case) [tagged own in  tuple own in  tagged move out] effect 'then let\n"
    "('fb let fb {'ok () 'no (drop fb)} case) [{'ok 'a 'no 'b} either own in  'a own in  'a move out] effect 'default let\n"
    "(list ('dd-x let dup dd-x member (dd-x drop) (dd-x push) if) fold) ['a list own in  'a list move out] effect 'dedup let\n"
    "3.14159265358979323846 'pi let\n"
    "6.28318530717958647692 'tau let\n"
    "('b let 'a let a len b len min 'n let 0 n range (dup a swap get must swap b swap get must couple) each) 'zip let\n"
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
    if(render_slots>0){int ms=val_slots(stack[sp-1]);if(sp+ms>STACK_MAX)die("show: stack overflow");deep_copy_values(&stack[sp],&stack[sp-ms],ms);sp+=ms;eval_body(render_body,render_slots,env);}
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
    FILE *f=fopen(path,"wb");if(!f){free(buf);push_fail(path);free(path);return;}
    size_t n=fwrite(buf,1,len,f); int closed=fclose(f);
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
        R(reverse,reverse),{"take-n",prim_take_n,NULL},{"drop-n",prim_drop_n,NULL},R(range,range),
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
    int check_only=0;
    cli_args=malloc(argc*sizeof(char*)); cli_argc=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--check")==0) check_only=1;
        else if(strcmp(argv[i],"--headless")==0) headless_mode=1;
        else if(argv[i][0]=='-'&&argv[i][1]=='-'){fprintf(stderr,"unknown flag: %s\nusage: slap [--check] [--headless] [args...] < file.slap\n",argv[i]);free(cli_args);return 1;}
        else cli_args[cli_argc++]=argv[i];
    }
    syms_init(); register_prims();
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
    if(sz==0){fprintf(stderr,"usage: slap [--check] [--headless] [args...] < file.slap\n");return 1;}
    store_source_lines(src, FID_STDIN);
    lex(src, FID_STDIN); int user_tok_count=tok_count;
    static Token user_tokens[TOK_MAX]; memcpy(user_tokens,tokens,user_tok_count*sizeof(Token));
    static Token combined[TOK_MAX]; int cpos=0;
    #define COMBINE(n) if(cpos+(n)>TOK_MAX) die("program too long: prelude, builtins and program need %d tokens, max %d", cpos+(n), TOK_MAX)
    store_source_lines(BUILTIN_TYPES, FID_BUILTIN);
    lex(BUILTIN_TYPES, FID_BUILTIN); COMBINE(tok_count); memcpy(combined,tokens,tok_count*sizeof(Token)); cpos=tok_count;
    lex(PRELUDE, FID_PRELUDE); COMBINE(tok_count); memcpy(&combined[cpos],tokens,tok_count*sizeof(Token)); cpos+=tok_count;
    int user_start=cpos;
    COMBINE(user_tok_count); memcpy(&combined[cpos],user_tokens,user_tok_count*sizeof(Token)); cpos+=user_tok_count;
    int errors=typecheck_tokens(combined,cpos,user_start);
    if(errors>0){fprintf(stderr,"%d type error(s)\n",errors);return 1;}
    if(check_only){fprintf(stderr,"type check passed\n");return 0;}
    current_loc=LOC_PACK(FID_STDIN,0,0);
    eval(user_tokens,user_tok_count,global);
    return 0;
}
