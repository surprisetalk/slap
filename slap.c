#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>

#define STACK_MAX  65536
#define SYM_MAX   4096
#define FRAME_MAX 256
#define FRAME_VALS_MAX 65536
#define TOK_MAX   65536
#define LOCAL_MAX 4096

typedef enum { VAL_INT, VAL_FLOAT, VAL_SYM, VAL_WORD, VAL_XT, VAL_TUPLE, VAL_LIST, VAL_RECORD, VAL_BOX } ValTag;
typedef struct Frame Frame;
typedef void (*PrimFn)(Frame *env);
typedef struct Value {
    ValTag tag;
    union {
        int64_t i; double f; uint32_t sym;
        struct { uint32_t sym; PrimFn fn; } xt;
        struct { uint32_t len; uint32_t slots; Frame *env; } compound;
        void *box;
    } as;
} Value;

__attribute__((noreturn)) static void die(const char *fmt, ...);

static int is_compound(ValTag tag) { return tag == VAL_TUPLE || tag == VAL_LIST || tag == VAL_RECORD; }

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
    if (sym_count >= SYM_MAX) { fprintf(stderr, "error: symbol table full\n"); exit(1); }
    sym_names[sym_count] = strdup(name);
    return (uint32_t)sym_count++;
}
static const char *sym_name(uint32_t id) { return sym_names[id]; }

static int current_line = 0;
static const char *current_file = "<input>";
static void print_stack_summary(FILE *out);

static char *source_text = NULL;
static const char **source_lines = NULL;
static int source_line_count = 0;

static void store_source_lines(const char *src) {
    source_text = strdup(src);
    int count = 1;
    for (const char *p = source_text; *p; p++) if (*p == '\n') count++;
    source_lines = malloc(count * sizeof(char *));
    source_line_count = 0;
    char *p = source_text;
    while (*p) {
        source_lines[source_line_count++] = p;
        char *nl = strchr(p, '\n');
        if (nl) { *nl = '\0'; p = nl + 1; } else break;
    }
}

static void print_source_line(FILE *out, int line) {
    if (!source_lines || line < 1 || line > source_line_count) return;
    fprintf(out, "    %4d| %s\n", line, source_lines[line - 1]);
}

__attribute__((noreturn))
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "\n-- ERROR %s:%d ", current_file, current_line);
    int hdr_len = 10 + (int)strlen(current_file) + 10;
    for (int i = hdr_len; i < 60; i++) fprintf(stderr, "-");
    fprintf(stderr, "\n\n    ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n\n");
    print_source_line(stderr, current_line);
    va_end(ap);
    print_stack_summary(stderr);
    fprintf(stderr, "\n");
    exit(1);
}

static Value stack[STACK_MAX];
static int sp = 0;
static void spush(Value v) { if (sp >= STACK_MAX) die("stack overflow"); stack[sp++] = v; }
static Value spop(void) { if (sp <= 0) die("stack underflow"); return stack[--sp]; }
static Value speek(void) { if (sp <= 0) die("stack underflow on peek"); return stack[sp - 1]; }

static Value val_int(int64_t i) { Value v; v.tag = VAL_INT; v.as.i = i; return v; }
static Value val_float(double f) { Value v; v.tag = VAL_FLOAT; v.as.f = f; return v; }
static Value val_sym(uint32_t s) { Value v; v.tag = VAL_SYM; v.as.sym = s; return v; }
static Value val_word(uint32_t s) { Value v; v.tag = VAL_WORD; v.as.sym = s; return v; }
static Value val_xt(uint32_t s, PrimFn fn) { Value v; v.tag = VAL_XT; v.as.xt.sym = s; v.as.xt.fn = fn; return v; }
static Value val_compound(ValTag tag, uint32_t len, uint32_t slots) {
    Value v; v.tag = tag; v.as.compound.len = len; v.as.compound.slots = slots; v.as.compound.env = NULL; return v;
}

typedef enum {
    TOK_INT, TOK_FLOAT, TOK_SYM, TOK_WORD, TOK_STRING,
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACKET, TOK_RBRACKET, TOK_LBRACE, TOK_RBRACE, TOK_EOF
} TokTag;
typedef struct {
    TokTag tag;
    union { int64_t i; double f; uint32_t sym; struct { int *codes; int len; } str; } as;
    int line;
} Token;
static Token tokens[TOK_MAX];
static int tok_count = 0;

static void lex(const char *src) {
    tok_count = 0; int line = 1; const char *p = src;
    while (*p) {
        if (*p == '\n') { line++; p++; continue; }
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (p[0] == '-' && p[1] == '-') { while (*p && *p != '\n') p++; continue; }
        if (tok_count >= TOK_MAX) die("too many tokens");
        Token *t = &tokens[tok_count]; t->line = line;
        if (*p == '(') { t->tag = TOK_LPAREN; p++; tok_count++; continue; }
        if (*p == ')') { t->tag = TOK_RPAREN; p++; tok_count++; continue; }
        if (*p == '[') { t->tag = TOK_LBRACKET; p++; tok_count++; continue; }
        if (*p == ']') { t->tag = TOK_RBRACKET; p++; tok_count++; continue; }
        if (*p == '{') { t->tag = TOK_LBRACE; p++; tok_count++; continue; }
        if (*p == '}') { t->tag = TOK_RBRACE; p++; tok_count++; continue; }
        if (*p == '"') {
            p++; int *codes = NULL; int len = 0, cap = 0;
            while (*p && *p != '"') {
                int ch;
                if (*p == '\\') { p++; switch (*p) { case 'n': ch='\n'; break; case 't': ch='\t'; break; case '\\': ch='\\'; break; case '"': ch='"'; break; case '0': ch=0; break; default: ch=*p; break; } }
                else ch = (unsigned char)*p;
                if (*p == '\n') line++;
                p++;
                if (len >= cap) { cap = cap ? cap*2 : 16; codes = realloc(codes, cap * sizeof(int)); }
                codes[len++] = ch;
            }
            if (*p == '"') p++;
            t->tag = TOK_STRING; t->as.str.codes = codes; t->as.str.len = len; tok_count++; continue;
        }
        if (*p == '\'') {
            p++; const char *start = p;
            while (*p && !isspace((unsigned char)*p) && *p!='(' && *p!=')' && *p!='[' && *p!=']' && *p!='{' && *p!='}') p++;
            int len = (int)(p - start);
            if (len == 0) die("empty symbol literal");
            char buf[256]; if (len >= (int)sizeof(buf)) die("symbol too long");
            memcpy(buf, start, len); buf[len] = 0;
            t->tag = TOK_SYM; t->as.sym = sym_intern(buf); tok_count++; continue;
        }
        if (isdigit((unsigned char)*p) || (*p == '-' && isdigit((unsigned char)p[1]))) {
            const char *start = p; if (*p == '-') p++;
            while (isdigit((unsigned char)*p)) p++;
            if (*p == '.' && isdigit((unsigned char)p[1])) {
                p++; while (isdigit((unsigned char)*p)) p++;
                t->tag = TOK_FLOAT; t->as.f = strtod(start, NULL);
            } else { t->tag = TOK_INT; t->as.i = strtoll(start, NULL, 10); }
            tok_count++; continue;
        }
        { const char *start = p;
          while (*p && !isspace((unsigned char)*p) && *p!='(' && *p!=')' && *p!='[' && *p!=']' && *p!='{' && *p!='}') p++;
          int len = (int)(p - start); char buf[256];
          if (len >= (int)sizeof(buf)) die("word too long");
          memcpy(buf, start, len); buf[len] = 0;
          if (strcmp(buf, "true") == 0) { t->tag = TOK_INT; t->as.i = 1; tok_count++; continue; }
          if (strcmp(buf, "false") == 0) { t->tag = TOK_INT; t->as.i = 0; tok_count++; continue; }
          t->tag = TOK_WORD; t->as.sym = sym_intern(buf); tok_count++;
        }
    }
    if (tok_count < TOK_MAX) { tokens[tok_count].tag = TOK_EOF; tokens[tok_count].line = line; }
}

typedef enum { BIND_DEF, BIND_LET } BindKind;
typedef struct Binding { uint32_t sym; int offset; int slots; BindKind kind; int recur; } Binding;
struct Frame {
    struct Frame *parent; int bind_count; int vals_used; int refcount;
    Binding bindings[FRAME_MAX]; Value vals[FRAME_VALS_MAX];
};

static Frame *frame_new(Frame *parent) {
    Frame *f = malloc(sizeof(Frame));
    f->parent = parent; f->bind_count = 0; f->vals_used = 0; f->refcount = 0; return f;
}
static void frame_free(Frame *f) { free(f); }

static void frame_bind(Frame *f, uint32_t sym, Value *vals, int slots, BindKind kind, int recur) {
    for (int i = 0; i < f->bind_count; i++) {
        if (f->bindings[i].sym == sym) {
            if (f->bindings[i].slots == slots) { memcpy(&f->vals[f->bindings[i].offset], vals, slots * sizeof(Value)); }
            else {
                int off = f->vals_used;
                if (off + slots > FRAME_VALS_MAX) die("frame value storage full");
                memcpy(&f->vals[off], vals, slots * sizeof(Value));
                f->vals_used += slots; f->bindings[i].offset = off; f->bindings[i].slots = slots;
            }
            f->bindings[i].kind = kind; f->bindings[i].recur = recur; return;
        }
    }
    if (f->bind_count >= FRAME_MAX) die("too many bindings in frame");
    int off = f->vals_used;
    if (off + slots > FRAME_VALS_MAX) die("frame value storage full");
    memcpy(&f->vals[off], vals, slots * sizeof(Value));
    f->vals_used += slots;
    Binding *b = &f->bindings[f->bind_count++];
    b->sym = sym; b->offset = off; b->slots = slots; b->kind = kind; b->recur = recur;
}

typedef struct { int found; int offset; int slots; BindKind kind; int recur; Frame *frame; } Lookup;
static Lookup frame_lookup(Frame *f, uint32_t sym) {
    Lookup r = {0};
    for (Frame *cur = f; cur; cur = cur->parent)
        for (int i = cur->bind_count - 1; i >= 0; i--)
            if (cur->bindings[i].sym == sym) {
                r.found = 1; r.offset = cur->bindings[i].offset; r.slots = cur->bindings[i].slots;
                r.kind = cur->bindings[i].kind; r.recur = cur->bindings[i].recur; r.frame = cur; return r;
            }
    return r;
}

static void eval(Token *toks, int count, Frame *env);
static void eval_body(Value *body, int slots, Frame *env);
static void build_tuple(Token *toks, int start, int end, int total_count, Frame *exec_env);
static int find_matching(Token *toks, int start, int count, TokTag open, TokTag close);

#define PRIM_HASH_SIZE 512
static struct { uint32_t sym; PrimFn fn; int used; } prim_hash[PRIM_HASH_SIZE];

static void prim_register(const char *name, PrimFn fn) {
    uint32_t sym = sym_intern(name), h = sym % PRIM_HASH_SIZE;
    for (int i = 0; i < PRIM_HASH_SIZE; i++) {
        uint32_t idx = (h + i) % PRIM_HASH_SIZE;
        if (!prim_hash[idx].used) { prim_hash[idx].sym = sym; prim_hash[idx].fn = fn; prim_hash[idx].used = 1; return; }
    }
    die("primitive hash table full");
}

static PrimFn prim_lookup(uint32_t sym) {
    uint32_t h = sym % PRIM_HASH_SIZE;
    for (int i = 0; i < PRIM_HASH_SIZE; i++) {
        uint32_t idx = (h + i) % PRIM_HASH_SIZE;
        if (!prim_hash[idx].used) return NULL;
        if (prim_hash[idx].sym == sym) return prim_hash[idx].fn;
    }
    return NULL;
}

static int64_t pop_int(void) { Value v = spop(); if (v.tag != VAL_INT) die("expected int, got %s", v.tag == VAL_FLOAT ? "float" : "non-int"); return v.as.i; }
static double pop_float(void) { Value v = spop(); if (v.tag != VAL_FLOAT) die("expected float, got non-float"); return v.as.f; }
static uint32_t pop_sym(void) { Value v = spop(); if (v.tag != VAL_SYM) die("expected symbol, got non-symbol"); return v.as.sym; }

typedef struct { int base; int slots; } ElemRef;
static ElemRef compound_elem(Value *data, int total_slots, int len, int index) {
    if (index < 0 || index >= len) die("index %d out of bounds (len %d)", index, len);
    if (total_slots == len + 1) { ElemRef ref = { index, 1 }; return ref; }
    int elem_end = total_slots - 1, off = 0, sz = 0;
    for (int i = len - 1; i >= 0; i--) {
        int lp = elem_end - 1; Value last = data[lp];
        int esize = val_slots(last);
        if (i == index) { off = elem_end - esize; sz = esize; }
        elem_end -= esize;
    }
    ElemRef ref = { off, sz }; return ref;
}

static ElemRef record_field(Value *data, int total_slots, int len, uint32_t key, int *found) {
    int elem_end = total_slots - 1; *found = 0; ElemRef ref = {0, 0};
    for (int i = len - 1; i >= 0; i--) {
        int lp = elem_end - 1; Value last = data[lp];
        int vsize = val_slots(last);
        int val_base = elem_end - vsize, key_pos = val_base - 1;
        if (key_pos < 0) die("malformed record");
        if (data[key_pos].tag != VAL_SYM) die("record key must be symbol");
        if (data[key_pos].as.sym == key) { ref.base = val_base; ref.slots = vsize; *found = 1; return ref; }
        elem_end = key_pos;
    }
    return ref;
}

static void compute_offsets(Value *data, int total_slots, int len, int *offsets, int *sizes) {
    int elem_end = total_slots - 1;
    for (int i = len - 1; i >= 0; i--) {
        int lp = elem_end - 1; Value l = data[lp];
        int sz = val_slots(l);
        offsets[i] = elem_end - sz; sizes[i] = sz; elem_end = offsets[i];
    }
}

static void record_offsets(Value *data, int total_slots, int len, int *kpos, int *voff, int *vsz) {
    int elem_end = total_slots - 1;
    for (int i = len - 1; i >= 0; i--) {
        int lp = elem_end - 1; int sz = val_slots(data[lp]);
        voff[i] = elem_end - sz; vsz[i] = sz; kpos[i] = voff[i] - 1; elem_end = kpos[i];
    }
}

static void val_print(Value *data, int slots, FILE *out) {
    Value top = data[slots - 1];
    switch (top.tag) {
    case VAL_INT: fprintf(out, "%lld", (long long)top.as.i); break;
    case VAL_FLOAT: fprintf(out, "%g", top.as.f); break;
    case VAL_SYM: fprintf(out, "'%s", sym_name(top.as.sym)); break;
    case VAL_WORD: fprintf(out, "%s", sym_name(top.as.sym)); break;
    case VAL_XT: fprintf(out, "%s", sym_name(top.as.xt.sym)); break;
    case VAL_LIST: {
        int len = (int)top.as.compound.len;
        int is_string = 1, elem_end = slots - 1;
        for (int i = len - 1; i >= 0; i--) {
            int lp = elem_end - 1; Value last = data[lp];
            if (last.tag != VAL_INT || last.as.i < 32 || last.as.i > 126) { is_string = 0; break; }
            elem_end = lp;
        }
        if (is_string && len > 0) {
            fprintf(out, "\"");
            for (int i = 0; i < len; i++) fprintf(out, "%c", (char)data[i].as.i);
            fprintf(out, "\""); break;
        }
    } /* fall through */
    case VAL_TUPLE: {
        int len = (int)top.as.compound.len;
        char open = top.tag == VAL_LIST ? '[' : '(', close = top.tag == VAL_LIST ? ']' : ')';
        fprintf(out, "%c", open);
        if (len > LOCAL_MAX) die("compound too large to print (%d elements)", len);
        int offsets[LOCAL_MAX], sizes[LOCAL_MAX];
        compute_offsets(data, slots, len, offsets, sizes);
        for (int i = 0; i < len; i++) { if (i > 0) fprintf(out, " "); val_print(&data[offsets[i]], sizes[i], out); }
        fprintf(out, "%c", close); break;
    }
    case VAL_RECORD: {
        int len = (int)top.as.compound.len;
        fprintf(out, "{");
        if (len > LOCAL_MAX) die("record too large to print (%d fields)", len);
        int kpos[LOCAL_MAX], voff[LOCAL_MAX], vsz[LOCAL_MAX];
        record_offsets(data, slots, len, kpos, voff, vsz);
        for (int i = 0; i < len; i++) {
            if (i > 0) fprintf(out, " ");
            fprintf(out, "'%s ", sym_name(data[kpos[i]].as.sym));
            val_print(&data[voff[i]], vsz[i], out);
        }
        fprintf(out, "}");
        break;
    }
    case VAL_BOX: fprintf(out, "<box>"); break;
    }
}

static void print_stack_summary(FILE *out) {
    if (sp == 0) { fprintf(out, "\n    stack: (empty)\n"); return; }
    fprintf(out, "\n    stack (%d slot%s):\n", sp, sp == 1 ? "" : "s");
    int pos = sp, shown = 0;
    while (pos > 0 && shown < 5) {
        Value v = stack[pos - 1]; int s = val_slots(v); pos -= s;
        fprintf(out, "      %d: ", shown); val_print(&stack[pos], s, out); fprintf(out, "\n"); shown++;
    }
    if (pos > 0) {
        int remaining = 0;
        while (pos > 0) { Value v = stack[pos - 1]; pos -= val_slots(v); remaining++; }
        fprintf(out, "      ... %d more\n", remaining);
    }
}

static int val_equal(Value *a, int aslots, Value *b, int bslots) {
    if (aslots != bslots) return 0;
    Value atop = a[aslots - 1], btop = b[bslots - 1];
    if (atop.tag != btop.tag) return 0;
    switch (atop.tag) {
    case VAL_INT: return atop.as.i == btop.as.i;
    case VAL_FLOAT: return atop.as.f == btop.as.f;
    case VAL_SYM: case VAL_WORD: case VAL_XT: return atop.as.sym == btop.as.sym;
    case VAL_TUPLE: case VAL_LIST: case VAL_RECORD:
        if (atop.as.compound.len != btop.as.compound.len) return 0;
        for (int i = 0; i < aslots - 1; i++) if (!val_equal(&a[i], 1, &b[i], 1)) return 0;
        return 1;
    case VAL_BOX: return a == b;
    }
    return 0;
}

static int val_less(Value *a, int aslots, Value *b, int bslots) {
    Value atop = a[aslots - 1], btop = b[bslots - 1];
    if (atop.tag != btop.tag) die("lt: type mismatch");
    switch (atop.tag) {
    case VAL_INT: return atop.as.i < btop.as.i;
    case VAL_FLOAT: return atop.as.f < btop.as.f;
    case VAL_SYM: return atop.as.sym < btop.as.sym;
    default: die("lt: unsupported type"); return 0;
    }
}

static uint32_t recur_sym = 0;
static int recur_pending = 0;
static uint32_t S_DEF, S_LET, S_RECUR, S_IF, S_EFFECT, S_CHECK;
static void syms_init(void) {
    S_DEF=sym_intern("def"); S_LET=sym_intern("let"); S_RECUR=sym_intern("recur");
    S_IF=sym_intern("if"); S_EFFECT=sym_intern("effect"); S_CHECK=sym_intern("check");
}

/* ---- TYPE SYSTEM ---- */

typedef enum { DIR_IN, DIR_OUT } SlotDir;
typedef enum { OWN_OWN, OWN_COPY, OWN_MOVE, OWN_LENT } OwnMode;
typedef enum { TC_NONE=0, TC_INT, TC_FLOAT, TC_SYM, TC_NUM, TC_LIST, TC_TUPLE, TC_REC, TC_BOX, TC_STACK } TypeConstraint;

enum { HO_BODY_1TO1=1, HO_BRANCHES_AGREE=2, HO_SAVES_UNDER=4, HO_SCRUTINEE_SYM=8,
       HO_APPLY_EFFECT=16, HO_BOX_BORROW=32, HO_BOX_MUTATE=64 };
typedef struct { const char *name; uint32_t sym; int need; int out; TypeConstraint out_type; uint8_t flags; } HOEffect;
#define HO_OP_COUNT 23
static HOEffect ho_ops[HO_OP_COUNT] = {
    {"apply",0,1,0,TC_NONE,HO_APPLY_EFFECT},{"dip",0,2,1,TC_NONE,HO_APPLY_EFFECT|HO_SAVES_UNDER},
    {"if",0,3,1,TC_NONE,HO_BRANCHES_AGREE},
    {"map",0,2,1,TC_LIST,HO_BODY_1TO1},{"filter",0,2,1,TC_LIST,HO_BODY_1TO1},
    {"fold",0,3,1,TC_NONE,0},{"reduce",0,2,1,TC_NONE,0},{"each",0,2,0,TC_NONE,0},
    {"while",0,2,0,TC_NONE,0},{"loop",0,1,0,TC_NONE,HO_APPLY_EFFECT},
    {"lend",0,2,2,TC_BOX,HO_BOX_BORROW},{"mutate",0,2,1,TC_BOX,HO_BOX_MUTATE},
    {"cond",0,3,1,TC_NONE,HO_BRANCHES_AGREE},{"match",0,3,1,TC_NONE,HO_BRANCHES_AGREE|HO_SCRUTINEE_SYM},
    {"where",0,2,1,TC_LIST,0},{"find",0,2,1,TC_NONE,0},{"table",0,2,1,TC_LIST,0},
    {"scan",0,3,1,TC_LIST,0},
    {"repeat",0,2,0,TC_NONE,0},{"bi",0,3,2,TC_NONE,0},{"keep",0,1,1,TC_NONE,0},
    {"on",0,1,0,TC_NONE,0},{"show",0,1,0,TC_NONE,0},
};
static int ho_ops_init = 0;
static void ho_ops_ensure_init(void) { if (ho_ops_init) return; ho_ops_init = 1; for (int i = 0; i < HO_OP_COUNT; i++) ho_ops[i].sym = sym_intern(ho_ops[i].name); }
static HOEffect *ho_ops_find(uint32_t sym) { ho_ops_ensure_init(); for (int i = 0; i < HO_OP_COUNT; i++) if (ho_ops[i].sym == sym) return &ho_ops[i]; return NULL; }

typedef struct {
    uint32_t type_var; TypeConstraint constraint, elem_constraint; OwnMode ownership; SlotDir direction;
} TypeSlot;
#define TYPE_SLOTS_MAX 16
typedef struct { TypeSlot slots[TYPE_SLOTS_MAX]; int slot_count; } TypeSig;

#define TYPESIG_MAX 512
static struct { uint32_t sym; TypeSig sig; } type_sigs[TYPESIG_MAX];
static int type_sig_count = 0;
static void typesig_register(uint32_t sym, TypeSig *sig) {
    for (int i = 0; i < type_sig_count; i++) { if (type_sigs[i].sym == sym) { type_sigs[i].sig = *sig; return; } }
    if (type_sig_count >= TYPESIG_MAX) die("type signature table full");
    type_sigs[type_sig_count].sym = sym; type_sigs[type_sig_count].sig = *sig; type_sig_count++;
}
static TypeSig *typesig_find(uint32_t sym) {
    for (int i = 0; i < type_sig_count; i++) if (type_sigs[i].sym == sym) return &type_sigs[i].sig;
    return NULL;
}

static TypeConstraint parse_constraint(const char *tw) {
    if (strcmp(tw,"int")==0) return TC_INT; if (strcmp(tw,"float")==0) return TC_FLOAT;
    if (strcmp(tw,"sym")==0) return TC_SYM; if (strcmp(tw,"num")==0) return TC_NUM;
    if (strcmp(tw,"list")==0) return TC_LIST; if (strcmp(tw,"tuple")==0) return TC_TUPLE;
    if (strcmp(tw,"rec")==0) return TC_REC; if (strcmp(tw,"box")==0) return TC_BOX;
    if (strcmp(tw,"stack")==0) return TC_STACK; return TC_NONE;
}

static TypeSig parse_type_annotation(Token *toks, int start, int end) {
    TypeSig sig; memset(&sig, 0, sizeof(sig));
    int i = start;
    while (i < end) {
        if (sig.slot_count >= TYPE_SLOTS_MAX) die("too many type slots");
        TypeSlot *slot = &sig.slots[sig.slot_count]; memset(slot, 0, sizeof(*slot));
        int slot_start = i;
        while (i < end) {
            if (toks[i].tag != TOK_WORD && toks[i].tag != TOK_SYM) break;
            const char *w = sym_name(toks[i].as.sym);
            if (strcmp(w,"in")==0 || strcmp(w,"out")==0) break;
            i++;
        }
        if (i >= end) break;
        slot->direction = (strcmp(sym_name(toks[i].as.sym), "in") == 0) ? DIR_IN : DIR_OUT; i++;
        for (int j = slot_start; j < i - 1; j++) {
            const char *tw = sym_name(toks[j].as.sym);
            if (strcmp(tw,"own")==0) { slot->ownership=OWN_OWN; continue; }
            if (strcmp(tw,"copy")==0) { slot->ownership=OWN_COPY; continue; }
            if (strcmp(tw,"move")==0) { slot->ownership=OWN_MOVE; continue; }
            if (strcmp(tw,"lent")==0) { slot->ownership=OWN_LENT; continue; }
            TypeConstraint c = parse_constraint(tw);
            if (c != TC_NONE) {
                if ((c == TC_LIST || c == TC_BOX) && slot->constraint != TC_NONE) slot->elem_constraint = slot->constraint;
                slot->constraint = c; continue;
            }
            if (!slot->type_var) slot->type_var = toks[j].as.sym;
        }
        sig.slot_count++;
    }
    return sig;
}

static const char *constraint_name(TypeConstraint c) {
    switch (c) {
    case TC_NONE: return "any"; case TC_INT: return "int"; case TC_FLOAT: return "float";
    case TC_SYM: return "sym"; case TC_NUM: return "num"; case TC_LIST: return "list";
    case TC_TUPLE: return "tuple"; case TC_REC: return "rec"; case TC_BOX: return "box";
    case TC_STACK: return "stack";
    }
    return "?";
}

#define TVAR_MAX 8192
typedef struct { int parent; TypeConstraint bound; int elem; int box_c; } TVarEntry;

#define EFFECT_MAX 256
typedef struct {
    int consumed, produced;
    TypeConstraint out_type;
    int scheme_base, scheme_count;
    int in_tvars[8], out_tvars[8], in_count, out_count;
    int out_effect;
} TupleEffect;

#define AT_LINEAR 1
#define AT_CONSUMED 2
typedef struct {
    TypeConstraint type; int tvar_id; uint32_t sym_id;
    uint8_t flags; int8_t borrowed; int source_line; int effect_idx;
} AbstractType;

#define ASTACK_MAX 256
#define TC_BINDS_MAX 256
typedef struct { uint32_t sym; AbstractType atype; int is_def; int def_line; } TCBinding;
typedef struct { uint32_t sym; int line; } TCUnknown;
#define TC_UNKNOWN_MAX 256

typedef struct {
    AbstractType data[ASTACK_MAX]; int sp, errors;
    TCBinding bindings[TC_BINDS_MAX]; int bind_count;
    int recur_pending; uint32_t recur_sym;
    TCUnknown unknowns[TC_UNKNOWN_MAX]; int unknown_count;
    TVarEntry tvars[TVAR_MAX]; int tvar_count;
    TupleEffect effects[EFFECT_MAX]; int effect_count;
    int user_start, prelude_sig_count, suppress_errors, sp_floor, body_depth;
} TypeChecker;

static int tvar_fresh(TypeChecker *tc) {
    if (tc->tvar_count >= TVAR_MAX) die("type variable overflow");
    int id = tc->tvar_count++;
    tc->tvars[id].parent = id; tc->tvars[id].bound = TC_NONE; tc->tvars[id].elem = 0; tc->tvars[id].box_c = 0;
    return id;
}
static int tvar_find(TypeChecker *tc, int id) {
    while (tc->tvars[id].parent != id) { tc->tvars[id].parent = tc->tvars[tc->tvars[id].parent].parent; id = tc->tvars[id].parent; }
    return id;
}
static TypeConstraint tvar_resolve(TypeChecker *tc, int id) { return tc->tvars[tvar_find(tc, id)].bound; }
static int tc_constraint_matches(TypeConstraint constraint, TypeConstraint actual) {
    if (constraint == TC_NONE || constraint == actual) return 1;
    if (constraint == TC_NUM && (actual == TC_INT || actual == TC_FLOAT)) return 1;
    if (actual == TC_NUM && (constraint == TC_INT || constraint == TC_FLOAT)) return 1;
    return 0;
}
static int tvar_bind(TypeChecker *tc, int id, TypeConstraint c, int line) {
    int root = tvar_find(tc, id); TypeConstraint cur = tc->tvars[root].bound;
    if (cur == TC_NONE) { tc->tvars[root].bound = c; return 0; }
    if (tc_constraint_matches(cur, c)) { if (cur == TC_NUM && c != TC_NUM) tc->tvars[root].bound = c; return 0; }
    if (tc_constraint_matches(c, cur)) return 0;
    (void)line; return 1;
}
static int tvar_unify(TypeChecker *tc, int a, int b, int line) {
    int ra = tvar_find(tc, a), rb = tvar_find(tc, b);
    if (ra == rb) return 0;
    TypeConstraint ca = tc->tvars[ra].bound, cb = tc->tvars[rb].bound;
    if (ca != TC_NONE && cb != TC_NONE && !tc_constraint_matches(ca, cb) && !tc_constraint_matches(cb, ca))
        { (void)line; return 1; }
    tc->tvars[rb].parent = ra;
    if (tc->tvars[ra].elem == 0 && tc->tvars[rb].elem != 0) tc->tvars[ra].elem = tc->tvars[rb].elem;
    if (tc->tvars[ra].box_c == 0 && tc->tvars[rb].box_c != 0) tc->tvars[ra].box_c = tc->tvars[rb].box_c;
    if (ca == TC_NONE) tc->tvars[ra].bound = cb;
    else if (tc_constraint_matches(ca, cb) && ca == TC_NUM && cb != TC_NUM) tc->tvars[ra].bound = cb;
    return 0;
}
static int tvar_unify_at(TypeChecker *tc, int tvar, AbstractType *at, int line) {
    if (at->tvar_id > 0) return tvar_unify(tc, tvar, at->tvar_id, line);
    if (at->type != TC_NONE) return tvar_bind(tc, tvar, at->type, line);
    return 0;
}

static void tvar_instantiate(TypeChecker *tc, int base, int count, int *map) {
    for (int i = 0; i < count; i++) map[i] = tvar_fresh(tc);
    for (int i = 0; i < count; i++) {
        int src = base + i, root = tvar_find(tc, src);
        tc->tvars[map[i]].bound = tc->tvars[root].bound;
        if (tc->tvars[root].elem > 0) {
            int off = tc->tvars[root].elem - base;
            tc->tvars[map[i]].elem = (off >= 0 && off < count) ? map[off] : tc->tvars[root].elem;
        }
        if (tc->tvars[root].box_c > 0) {
            int off = tc->tvars[root].box_c - base;
            tc->tvars[map[i]].box_c = (off >= 0 && off < count) ? map[off] : tc->tvars[root].box_c;
        }
    }
    for (int i = 0; i < count; i++) {
        int src_root = tvar_find(tc, base + i), off = src_root - base;
        if (off >= 0 && off < count && off != i) tvar_unify(tc, map[i], map[off], 0);
    }
}

static void tc_push(TypeChecker *tc, TypeConstraint type, int line) {
    if (tc->sp >= ASTACK_MAX) return;
    AbstractType *at = &tc->data[tc->sp++]; memset(at, 0, sizeof(*at));
    at->type = type; at->flags = (type == TC_BOX) ? AT_LINEAR : 0;
    at->source_line = line; at->effect_idx = -1;
    if (type == TC_LIST || type == TC_BOX) {
        at->tvar_id = tvar_fresh(tc); tc->tvars[at->tvar_id].bound = type;
        int sub = tvar_fresh(tc);
        if (type == TC_LIST) tc->tvars[at->tvar_id].elem = sub; else tc->tvars[at->tvar_id].box_c = sub;
    }
}

static int tc_alloc_effect(TypeChecker *tc) {
    if (tc->effect_count >= EFFECT_MAX) die("too many tuple effects");
    int idx = tc->effect_count++; memset(&tc->effects[idx], 0, sizeof(TupleEffect));
    return idx;
}

static TCBinding *tc_lookup(TypeChecker *tc, uint32_t sym) {
    for (int i = tc->bind_count - 1; i >= 0; i--) if (tc->bindings[i].sym == sym) return &tc->bindings[i];
    return NULL;
}

static void tc_error(TypeChecker *tc, int line, int origin_line, const char *fmt, ...) {
    if (tc->suppress_errors) { va_list ap; va_start(ap, fmt); va_end(ap); return; }
    tc->errors++;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "\n-- TYPE ERROR %s:%d ", current_file, line);
    int hdr_len = 15 + (int)strlen(current_file) + 10;
    for (int i = hdr_len; i < 60; i++) fprintf(stderr, "-");
    fprintf(stderr, "\n\n    ");
    vfprintf(stderr, fmt, ap); fprintf(stderr, "\n\n"); va_end(ap);
    print_source_line(stderr, line);
    if (origin_line > 0 && origin_line != line)
        print_source_line(stderr, origin_line);
    fprintf(stderr, "\n");
}

static void tc_dump_stack(TypeChecker *tc) {
    fprintf(stderr, "    stack (top first):");
    for (int d = tc->sp - 1; d >= 0 && d >= tc->sp - 5; d--)
        fprintf(stderr, " %s", constraint_name(tc->data[d].type));
    fprintf(stderr, "\n");
}

static TypeConstraint tc_infer_effect_ctx(Token *toks, int start, int end, int total_count,
                            int *out_consumed, int *out_produced, TypeChecker *ctx) {
    int vsp = 0, consumed = 0; TypeConstraint top_type = TC_NONE;
    for (int i = start; i < end; i++) {
        Token *t = &toks[i];
        switch (t->tag) {
        case TOK_INT: vsp++; top_type = TC_INT; break;
        case TOK_FLOAT: vsp++; top_type = TC_FLOAT; break;
        case TOK_SYM: vsp++; top_type = TC_SYM; break;
        case TOK_STRING: vsp++; top_type = TC_LIST; break;
        case TOK_LPAREN: { int close = find_matching(toks, i+1, total_count, TOK_LPAREN, TOK_RPAREN); vsp++; top_type = TC_TUPLE; i = close; break; }
        case TOK_LBRACKET: { int close = find_matching(toks, i+1, total_count, TOK_LBRACKET, TOK_RBRACKET); vsp++; top_type = TC_LIST; i = close; break; }
        case TOK_LBRACE: { int close = find_matching(toks, i+1, total_count, TOK_LBRACE, TOK_RBRACE); vsp++; top_type = TC_REC; i = close; break; }
        case TOK_WORD: {
            uint32_t sym = t->as.sym;
            if (sym == S_DEF || sym == S_LET) {
                int need = 2; if (vsp < need) { consumed += need - vsp; vsp = 0; } else vsp -= need;
            } else if (sym == S_RECUR) {
                int need = 1; if (vsp < need) { consumed += need - vsp; vsp = 0; } else vsp -= need;
            } else {
                TypeSig *sig = typesig_find(sym);
                if (sig) {
                    int inputs = 0, outputs = 0; TypeConstraint last_out = TC_NONE;
                    for (int j = 0; j < sig->slot_count; j++) {

                        if (sig->slots[j].direction == DIR_IN) inputs++;
                        else { outputs++; last_out = sig->slots[j].constraint; }
                    }
                    if (vsp < inputs) { consumed += inputs - vsp; vsp = 0; } else vsp -= inputs;
                    vsp += outputs; if (outputs > 0) top_type = last_out;
                } else {
                    HOEffect *ho = ho_ops_find(sym);
                    if (ho) {
                        int need = ho->need, out = ho->out;
                        if (ctx && sym == S_IF && i >= start + 2) {
                            int bp = i - 1, depth;
                            if (bp >= start && toks[bp].tag == TOK_RPAREN) {
                                int ep = bp; depth = 1;
                                for (int k = bp-1; k >= start; k--) { if (toks[k].tag == TOK_RPAREN) depth++; else if (toks[k].tag == TOK_LPAREN && --depth == 0) { ep = k; break; } }
                                int bc = 0, bprod = 0;
                                tc_infer_effect_ctx(toks, ep+1, bp, total_count, &bc, &bprod, ctx);
                                need = ho->need + bc;
                                out = bprod;
                            }
                        }
                        if (vsp < need) { consumed += need - vsp; vsp = 0; } else vsp -= need;
                        vsp += out; if (out > 0) top_type = ho->out_type;
                    }
                }
                if (ctx && !sig && !ho_ops_find(sym)) { TCBinding *ub = tc_lookup(ctx, sym);
                    if (ub && ub->is_def && ub->atype.effect_idx >= 0) {
                        TupleEffect *e = &ctx->effects[ub->atype.effect_idx];
                        if (vsp < e->consumed) { consumed += e->consumed - vsp; vsp = 0; } else vsp -= e->consumed;
                        vsp += e->produced; if (e->produced > 0) top_type = e->out_type;
                    } else if (ub && !ub->is_def) { vsp++; top_type = ub->atype.type; }
                }
            }
            break;
        }
        default: break;
        }
    }
    *out_consumed = consumed; *out_produced = vsp; return top_type;
}

static void tc_apply_effect(TypeChecker *tc, int consumed, int produced, TypeConstraint out_type, int line) {
    int avail = tc->sp - tc->sp_floor;
    tc->sp -= (consumed <= avail) ? consumed : avail;
    for (int i = 0; i < produced; i++) {
        TypeConstraint t = (i == produced - 1) ? out_type : TC_NONE;
        tc_push(tc, t, line);
    }
}

static int tc_check_body_against_sig(Token *toks, int start, int end, int total_count, TypeSig *sig) {
    int errors = 0, n_in = 0, n_out = 0;
    for (int i = 0; i < sig->slot_count; i++) {

        if (sig->slots[i].direction == DIR_IN) n_in++; else n_out++;
    }
    int eff_consumed, eff_produced;
    tc_infer_effect_ctx(toks, start, end, total_count, &eff_consumed, &eff_produced, NULL);
    if (eff_consumed != n_in) { fprintf(stderr, "%s:%d: type error: function body consumes %d value(s) but type declares %d input(s)\n", current_file, toks[start].line, eff_consumed, n_in); errors++; }
    if (eff_produced != n_out) { fprintf(stderr, "%s:%d: type error: function body produces %d value(s) but type declares %d output(s)\n", current_file, toks[start].line, eff_produced, n_out); errors++; }
    return errors;
}

static int tc_is_copyable(AbstractType *t) { return !(t->flags & AT_LINEAR) && t->type != TC_BOX; }
static int tc_is_builtin(uint32_t sym, int prelude_sig_count) {
    for (int i = 0; i < prelude_sig_count; i++) if (type_sigs[i].sym == sym) return 1;
    return ho_ops_find(sym) != NULL;
}
static void tc_bind(TypeChecker *tc, uint32_t sym, AbstractType *atype, int is_def, int line) {
    for (int i = 0; i < tc->bind_count; i++) {
        if (tc->bindings[i].sym == sym) { tc->bindings[i].atype = *atype; tc->bindings[i].is_def = is_def; tc->bindings[i].def_line = line; return; }
    }
    if (tc->bind_count < TC_BINDS_MAX) {
        tc->bindings[tc->bind_count].sym = sym; tc->bindings[tc->bind_count].atype = *atype;
        tc->bindings[tc->bind_count].is_def = is_def; tc->bindings[tc->bind_count].def_line = line; tc->bind_count++;
    }
}

static void tc_apply_scheme(TypeChecker *tc, TupleEffect *eff, int consumed, int produced,
                            TypeConstraint body_out, const char *name, int line, int full_unify) {
    int map[64] = {0}, sc = eff->scheme_count; if (sc > 64) sc = 64;
    tvar_instantiate(tc, eff->scheme_base, sc, map);
    for (int j = 0; j < eff->in_count && j < tc->sp; j++) {
        int stv = eff->in_tvars[j] - eff->scheme_base;
        if (stv >= 0 && stv < sc) {
            int ftv = map[stv];
            AbstractType *input = &tc->data[tc->sp - eff->in_count + j];
            if (tvar_unify_at(tc, ftv, input, line))
                tc_error(tc, line, input->source_line, "'%s' input type mismatch: expected %s, got %s (value from line %d)", name, constraint_name(tvar_resolve(tc, ftv)), constraint_name(input->type != TC_NONE ? input->type : tvar_resolve(tc, input->tvar_id)), input->source_line);
            if (full_unify && input->tvar_id > 0) {
                int fe = tc->tvars[tvar_find(tc, ftv)].elem, ie = tc->tvars[tvar_find(tc, input->tvar_id)].elem;
                if (fe > 0 && ie > 0) tvar_unify(tc, fe, ie, line);
                int fb = tc->tvars[tvar_find(tc, ftv)].box_c, ib = tc->tvars[tvar_find(tc, input->tvar_id)].box_c;
                if (fb > 0 && ib > 0) tvar_unify(tc, fb, ib, line);
            }
        }
    }
    int avail = tc->sp - tc->sp_floor;
    tc->sp -= (consumed <= avail) ? consumed : avail;
    for (int j = 0; j < eff->out_count; j++) {
        int stv = eff->out_tvars[j] - eff->scheme_base;
        int ftv = (stv >= 0 && stv < sc) ? map[stv] : 0;
        tc_push(tc, TC_NONE, line);
        if (ftv > 0) {
            TypeConstraint resolved = tvar_resolve(tc, ftv);
            if (resolved != TC_NONE) tc->data[tc->sp-1].type = resolved;
            tc->data[tc->sp-1].tvar_id = ftv;
            if (full_unify && resolved == TC_BOX) tc->data[tc->sp-1].flags |= AT_LINEAR;
        }
    }
    for (int j = eff->out_count; j < produced; j++)
        tc_push(tc, (j == produced - 1) ? body_out : TC_NONE, line);
}

static void tc_apply_ho(TypeChecker *tc, HOEffect *ho, int line) {
    int eff_c = 0, eff_p = 0, body_known = 0, body_out_eff = -1; TypeConstraint body_out = TC_NONE; TupleEffect *body_teff = NULL;
    TypeConstraint branch_outs[8] = {0}; int branch_count = 0;
    AbstractType saved = {0}; int had_saved = 0;

    if (ho->flags & (HO_BOX_BORROW|HO_BOX_MUTATE)) {
        if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE) {
            if (tc->data[tc->sp-1].effect_idx >= 0) {
                TupleEffect *teff = &tc->effects[tc->data[tc->sp-1].effect_idx];
                eff_c = teff->consumed; eff_p = teff->produced; body_out = teff->out_type; body_known = 1;
            }
            tc->sp--;
        }
        if (tc->sp > 0 && tc->data[tc->sp-1].type != TC_BOX && tc->data[tc->sp-1].type != TC_NONE)
            tc_error(tc, line, 0, "'%s' expected box, got %s", ho->name, constraint_name(tc->data[tc->sp-1].type));
        if ((ho->flags & HO_BOX_BORROW) && tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
            TypeConstraint contents = TC_NONE;
            if (tc->data[tc->sp-1].tvar_id > 0) {
                int bc = tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].box_c;
                if (bc > 0) contents = tvar_resolve(tc, bc);
            }
            tc->data[tc->sp-1].borrowed++;
            int results = body_known ? (1 - eff_c + eff_p) : 1; if (results < 0) results = 0;
            for (int j = 0; j < results; j++) {
                TypeConstraint rt = (j == results-1 && body_out != TC_NONE) ? body_out : (j == 0 && contents != TC_NONE) ? contents : TC_NONE;
                tc_push(tc, rt, line);
            }
            int box_idx = tc->sp - results - 1;
            if (box_idx >= 0) tc->data[box_idx].borrowed--;
        } else if ((ho->flags & HO_BOX_MUTATE) && tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
            TypeConstraint contents = TC_NONE;
            if (tc->data[tc->sp-1].tvar_id > 0) {
                int bc = tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].box_c;
                if (bc > 0) contents = tvar_resolve(tc, bc);
            }
            if (contents != TC_NONE && body_out != TC_NONE && !tc_constraint_matches(contents, body_out) && !tc_constraint_matches(body_out, contents))
                tc_error(tc, line, 0, "'mutate' body produces %s but box contains %s", constraint_name(body_out), constraint_name(contents));
        }
        return;
    }

    TypeConstraint last_popped_type = TC_NONE;
    for (int n = ho->need; n > 0 && tc->sp > tc->sp_floor; n--) {
        AbstractType *top = &tc->data[tc->sp - 1];
        if ((ho->flags & HO_SAVES_UNDER) && n == 1) { saved = *top; had_saved = 1; tc->sp--; continue; }
        last_popped_type = top->type;
        if (top->type == TC_TUPLE) {
            if (top->effect_idx >= 0) {
                TupleEffect *teff = &tc->effects[top->effect_idx];
                if (!body_known) { eff_c = teff->consumed; eff_p = teff->produced; body_out = teff->out_type; body_known = 1; body_out_eff = teff->out_effect; body_teff = teff; }
                if (n > 1 && branch_count < 8) branch_outs[branch_count++] = teff->out_type;
            }
        } else if (n > 1 && top->type != TC_NONE) {
            if (branch_count < 8) branch_outs[branch_count++] = top->type;
        }
        tc->sp--;
    }

    if ((ho->flags & HO_BODY_1TO1) && body_known && (eff_c != 1 || eff_p != 1) && (eff_c + eff_p > 0))
        tc_error(tc, line, 0, "'%s' body must be 1->1, got %d->%d", ho->name, eff_c, eff_p);
    if ((ho->flags & HO_BRANCHES_AGREE) && branch_count >= 2 && !tc->sp_floor) {
        TypeConstraint ref = TC_NONE;
        for (int i = 0; i < branch_count; i++) if (branch_outs[i] != TC_NONE) { ref = branch_outs[i]; break; }
        for (int i = 0; i < branch_count; i++)
            if (branch_outs[i] != TC_NONE && ref != TC_NONE && branch_outs[i] != ref)
                if (!tc_constraint_matches(ref, branch_outs[i]) && !tc_constraint_matches(branch_outs[i], ref))
                    tc_error(tc, line, 0, "'%s' branches produce different types: %s vs %s", ho->name, constraint_name(ref), constraint_name(branch_outs[i]));
    }
    if ((ho->flags & HO_SCRUTINEE_SYM) && last_popped_type != TC_SYM && last_popped_type != TC_NONE)
        tc_error(tc, line, 0, "'match' scrutinee must be a symbol, got %s", constraint_name(last_popped_type));

    if (ho->flags & HO_APPLY_EFFECT) {
        if (body_known) {
            tc_apply_effect(tc, eff_c, eff_p, body_out, line);
            if (body_out_eff >= 0 && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE)
                tc->data[tc->sp-1].effect_idx = body_out_eff;
        }
        if (had_saved && tc->sp < ASTACK_MAX) tc->data[tc->sp++] = saved;
        return;
    }

    { if (ho->sym == S_IF && body_known) {
        if (body_teff && body_teff->scheme_count > 0 && body_teff->in_count > 0)
            tc_apply_scheme(tc, body_teff, eff_c, eff_p, body_out, "if", line, 0);
        else tc_apply_effect(tc, eff_c, eff_p, body_out, line);
        return;
    } }

    TypeConstraint out = ho->out_type;
    if (out == TC_NONE && (ho->flags & HO_BRANCHES_AGREE))
        for (int i = 0; i < branch_count; i++) if (branch_outs[i] != TC_NONE) { out = branch_outs[i]; break; }
    for (int j = 0; j < ho->out; j++) tc_push(tc, (j == ho->out - 1) ? out : TC_NONE, line);
}

static void tc_check_word(TypeChecker *tc, uint32_t sym, int line) {
    TypeSig *sig = typesig_find(sym);
    if (sig) goto apply_sig;
    { HOEffect *ho = ho_ops_find(sym);
      if (ho) { tc_apply_ho(tc, ho, line); return; }
    }
    { TCBinding *b = tc_lookup(tc, sym);
      if (b) {
        if (b->is_def && b->atype.type == TC_TUPLE) {
            if (b->atype.effect_idx >= 0) {
                TupleEffect *eff = &tc->effects[b->atype.effect_idx];
                if (eff->scheme_count > 0)
                    tc_apply_scheme(tc, eff, eff->consumed, eff->produced, eff->out_type, sym_name(sym), line, 1);
                else tc_apply_effect(tc, eff->consumed, eff->produced, eff->out_type, line);
                return;
            }
        } else { tc_push(tc, b->atype.type, line); if (b->atype.flags & AT_LINEAR) tc->data[tc->sp-1].flags |= AT_LINEAR; return; }
      }
    }
    if (tc->unknown_count < TC_UNKNOWN_MAX) { tc->unknowns[tc->unknown_count].sym = sym; tc->unknowns[tc->unknown_count].line = line; tc->unknown_count++; }
    return;
apply_sig:;
    int inputs = 0;
    for (int i = 0; i < sig->slot_count; i++) if (sig->slots[i].direction == DIR_IN) inputs++;
    if (tc->sp < inputs) {
        tc_error(tc, line, 0, "'%s' needs %d input(s), stack has %d", sym_name(sym), inputs, tc->sp);
        if (tc->sp > 0) tc_dump_stack(tc);
        return;
    }
    #define MAX_TVARS 16
    struct { uint32_t var; int tvar; } tvar_map[MAX_TVARS]; int tvar_map_count = 0;
    for (int i = 0; i < sig->slot_count; i++) {
        uint32_t tv = sig->slots[i].type_var;
        if (!tv) continue;
        int found = 0;
        for (int j = 0; j < tvar_map_count; j++) if (tvar_map[j].var == tv) { found = 1; break; }
        if (!found && tvar_map_count < MAX_TVARS) {
            int id = tvar_fresh(tc);
            TypeConstraint c = sig->slots[i].constraint;
            int is_container = (c == TC_LIST || c == TC_BOX);
            if (!is_container && c != TC_NONE) tc->tvars[id].bound = c;
            if (is_container && sig->slots[i].elem_constraint != TC_NONE) tc->tvars[id].bound = sig->slots[i].elem_constraint;
            tvar_map[tvar_map_count].var = tv; tvar_map[tvar_map_count].tvar = id; tvar_map_count++;
        }
    }
    #define FIND_TVAR(tv_name) ({ int _tv = 0; for (int _j = 0; _j < tvar_map_count; _j++) if (tvar_map[_j].var == (tv_name)) { _tv = tvar_map[_j].tvar; break; } _tv; })
    int stack_pos = tc->sp - 1;
    for (int i = sig->slot_count - 1; i >= 0; i--) {
        TypeSlot *slot = &sig->slots[i];
        if (slot->direction != DIR_IN) continue;
        if (stack_pos < 0) break;
        AbstractType *at = &tc->data[stack_pos];
        if (slot->ownership == OWN_COPY && !tc_is_copyable(at))
            tc_error(tc, line, at->source_line, "'%s' requires copyable value, got linear type (value from line %d)", sym_name(sym), at->source_line);
        if (slot->ownership == OWN_OWN && at->borrowed > 0)
            tc_error(tc, line, at->source_line, "'%s' cannot consume value that is currently borrowed (lent, value from line %d)", sym_name(sym), at->source_line);
        if (slot->constraint != TC_NONE && at->type != TC_NONE && !tc_constraint_matches(slot->constraint, at->type))
            tc_error(tc, line, at->source_line, "'%s' expected %s, got %s (value from line %d)", sym_name(sym), constraint_name(slot->constraint), constraint_name(at->type), at->source_line);
        if (slot->type_var) {
            int tv = FIND_TVAR(slot->type_var);
            if (tv > 0) {
                int is_container = (slot->constraint == TC_LIST || slot->constraint == TC_BOX);
                if (is_container && at->tvar_id > 0) {
                    int ef = (slot->constraint == TC_LIST) ? tc->tvars[tvar_find(tc, at->tvar_id)].elem : tc->tvars[tvar_find(tc, at->tvar_id)].box_c;
                    if (ef > 0 && tvar_unify(tc, tv, ef, line))
                        tc_error(tc, line, at->source_line, "'%s' type variable '%s' mismatch: expected %s, got %s", sym_name(sym), sym_name(slot->type_var), constraint_name(tvar_resolve(tc, tv)), constraint_name(tvar_resolve(tc, ef)));
                } else if (!is_container) {
                    int fail = at->tvar_id > 0 ? tvar_unify(tc, tv, at->tvar_id, line) : (at->type != TC_NONE ? tvar_bind(tc, tv, at->type, line) : 0);
                    if (fail) tc_error(tc, line, at->source_line, "'%s' type variable '%s' mismatch: expected %s, got %s", sym_name(sym), sym_name(slot->type_var), constraint_name(tvar_resolve(tc, tv)), constraint_name(at->type != TC_NONE ? at->type : tvar_resolve(tc, at->tvar_id)));
                }
            }
        }
        if ((at->flags & AT_LINEAR) && slot->ownership == OWN_OWN) at->flags |= AT_CONSUMED;
        stack_pos--;
    }
    tc->sp -= inputs; if (tc->sp < tc->sp_floor) tc->sp = tc->sp_floor;
    for (int i = 0; i < sig->slot_count; i++) {
        TypeSlot *slot = &sig->slots[i];
        if (slot->direction != DIR_OUT) continue;
        if (tc->sp >= ASTACK_MAX) { tc->errors++; return; }
        tc_push(tc, slot->constraint, line);
        AbstractType *at = &tc->data[tc->sp - 1];
        int is_container = (slot->constraint == TC_LIST || slot->constraint == TC_BOX);
        if (slot->type_var) {
            int tv = FIND_TVAR(slot->type_var);
            if (tv > 0) {
                if (is_container && at->tvar_id > 0) {
                    int elem_field = (slot->constraint == TC_LIST) ? tc->tvars[tvar_find(tc, at->tvar_id)].elem
                                                                    : tc->tvars[tvar_find(tc, at->tvar_id)].box_c;
                    if (elem_field > 0) tvar_unify(tc, elem_field, tv, line);
                } else if (!is_container) {
                    TypeConstraint resolved = tvar_resolve(tc, tv);
                    if (resolved != TC_NONE) at->type = resolved;
                    at->tvar_id = tv;
                    if (resolved == TC_BOX) at->flags |= AT_LINEAR;
                }
            }
        }
        if (is_container && slot->elem_constraint != TC_NONE && at->tvar_id > 0) {
            int ef = (slot->constraint == TC_LIST) ? tc->tvars[tvar_find(tc, at->tvar_id)].elem
                                                    : tc->tvars[tvar_find(tc, at->tvar_id)].box_c;
            if (ef > 0) tvar_bind(tc, ef, slot->elem_constraint, line);
        }
    }
    #undef MAX_TVARS
}

static TypeConstraint tc_check_list_elements(TypeChecker *tc, Token *toks, int start, int end, int total_count, int line) {
    TypeConstraint elem_type = TC_NONE; int elem_count = 0;
    for (int i = start; i < end; i++) {
        TypeConstraint this_type = TC_NONE;
        switch (toks[i].tag) {
        case TOK_INT: this_type = TC_INT; break; case TOK_FLOAT: this_type = TC_FLOAT; break;
        case TOK_SYM: this_type = TC_SYM; break; case TOK_STRING: this_type = TC_LIST; break;
        case TOK_LPAREN: this_type = TC_TUPLE; i = find_matching(toks, i+1, total_count, TOK_LPAREN, TOK_RPAREN); break;
        case TOK_LBRACKET: this_type = TC_LIST; i = find_matching(toks, i+1, total_count, TOK_LBRACKET, TOK_RBRACKET); break;
        case TOK_LBRACE: this_type = TC_REC; i = find_matching(toks, i+1, total_count, TOK_LBRACE, TOK_RBRACE); break;
        case TOK_WORD: return TC_NONE;
        default: continue;
        }
        if (this_type == TC_NONE) continue;
        if (++elem_count == 1) elem_type = this_type;
        else if (this_type != elem_type) {
            tc_error(tc, line, 0, "list elements have inconsistent types: element 1 is %s, element %d is %s", constraint_name(elem_type), elem_count, constraint_name(this_type));
            return TC_NONE;
        }
    }
    return elem_type;
}

static void tc_process_range(TypeChecker *tc, Token *toks, int start, int end, int total_count) {
    for (int i = start; i < end; i++) {
        if (i == tc->user_start && !tc->prelude_sig_count) tc->prelude_sig_count = type_sig_count;
        Token *t = &toks[i]; current_line = t->line;
        switch (t->tag) {
        case TOK_INT: tc_push(tc, TC_INT, t->line); break;
        case TOK_FLOAT: tc_push(tc, TC_FLOAT, t->line); break;
        case TOK_SYM: tc_push(tc, TC_SYM, t->line); tc->data[tc->sp-1].sym_id = t->as.sym; break;
        case TOK_STRING: {
            tc_push(tc, TC_LIST, t->line);
            if (tc->data[tc->sp-1].tvar_id > 0) {
                int ev = tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].elem;
                if (ev > 0) tvar_bind(tc, ev, TC_INT, t->line);
            }
            break;
        }
        case TOK_LPAREN: {
            int close = find_matching(toks, i+1, total_count, TOK_LPAREN, TOK_RPAREN);
            int eff_c = 0, eff_p = 0;
            TypeConstraint eff_out = tc_infer_effect_ctx(toks, i+1, close, total_count, &eff_c, &eff_p, tc);
            int is_simple = 1;
            for (int j = i+1; j < close; j++) if (toks[j].tag == TOK_LPAREN || toks[j].tag == TOK_LBRACKET || toks[j].tag == TOK_LBRACE) { is_simple = 0; break; }
            int scheme_base = tc->tvar_count, in_count = 0, out_count = 0, out_eff = -1;
            int in_tvars[8] = {0}, out_tvars[8] = {0}, scheme_count = 0;
            if (eff_c <= 8 && eff_p <= 8) {
                int saved_sp = tc->sp, saved_binds = tc->bind_count, saved_unknowns = tc->unknown_count;
                int saved_recur = tc->recur_pending, saved_suppress = tc->suppress_errors;
                int saved_sigs = type_sig_count, saved_effects = tc->effect_count, saved_floor = tc->sp_floor;
                if (!is_simple) {
                    tc->sp_floor = tc->sp;
                    if (i < tc->user_start) tc->suppress_errors = 1;
                }
                in_count = eff_c;
                for (int j = 0; j < in_count; j++) {
                    in_tvars[j] = tvar_fresh(tc);
                    tc->tvars[in_tvars[j]].elem = tvar_fresh(tc); tc->tvars[in_tvars[j]].box_c = tvar_fresh(tc);
                    tc_push(tc, TC_NONE, t->line);
                    tc->data[tc->sp-1].tvar_id = in_tvars[j];
                }
                if (tc->recur_pending && tc->recur_sym) {
                    int pre_eidx = tc_alloc_effect(tc);
                    TupleEffect *pre_eff = &tc->effects[pre_eidx];
                    pre_eff->consumed = eff_c; pre_eff->produced = eff_p; pre_eff->out_type = eff_out;
                    AbstractType pre_at = {0}; pre_at.type = TC_TUPLE; pre_at.effect_idx = pre_eidx;
                    tc_bind(tc, tc->recur_sym, &pre_at, 1, t->line);
                }
                tc->body_depth++;
                tc_process_range(tc, toks, i+1, close, total_count);
                tc->body_depth--;
                int actual_out = tc->sp - saved_sp;
                {
                    out_count = actual_out > 8 ? 8 : (actual_out > 0 ? actual_out : 0);
                    for (int j = 0; j < out_count; j++) {
                        int idx = tc->sp - out_count + j;
                        if (idx >= 0) {
                            out_tvars[j] = tc->data[idx].tvar_id;
                            if (out_tvars[j] == 0 && tc->data[idx].type != TC_NONE) { out_tvars[j] = tvar_fresh(tc); tc->tvars[out_tvars[j]].bound = tc->data[idx].type; }
                            else if (out_tvars[j] == 0) out_tvars[j] = tvar_fresh(tc);
                        }
                    }
                    scheme_count = tc->tvar_count - scheme_base;
                }
                if (actual_out == 1 && tc->data[tc->sp-1].type == TC_TUPLE && tc->data[tc->sp-1].effect_idx >= 0)
                    out_eff = tc->data[tc->sp-1].effect_idx;
                tc->sp = saved_sp; tc->bind_count = saved_binds; tc->unknown_count = saved_unknowns;
                tc->recur_pending = saved_recur; tc->suppress_errors = saved_suppress;
                type_sig_count = saved_sigs; tc->effect_count = saved_effects; tc->sp_floor = saved_floor;
            }
            tc_push(tc, TC_TUPLE, t->line);
            int eidx = tc_alloc_effect(tc);
            TupleEffect *eff = &tc->effects[eidx];
            eff->consumed = eff_c; eff->produced = eff_p; eff->out_type = eff_out; eff->out_effect = out_eff;
            eff->scheme_base = scheme_base; eff->scheme_count = scheme_count;
            eff->in_count = in_count; eff->out_count = out_count;
            for (int j = 0; j < in_count; j++) eff->in_tvars[j] = in_tvars[j];
            for (int j = 0; j < out_count; j++) eff->out_tvars[j] = out_tvars[j];
            tc->data[tc->sp-1].effect_idx = eidx;
            i = close; break;
        }
        case TOK_LBRACKET: {
            int close = find_matching(toks, i+1, total_count, TOK_LBRACKET, TOK_RBRACKET);
            TypeConstraint elem = tc_check_list_elements(tc, toks, i+1, close, total_count, t->line);
            tc_push(tc, TC_LIST, t->line);
            if (elem != TC_NONE && tc->data[tc->sp-1].tvar_id > 0) {
                int ev = tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].elem;
                if (ev > 0) tvar_bind(tc, ev, elem, t->line);
            }
            i = close; break;
        }
        case TOK_LBRACE: {
            int close = find_matching(toks, i+1, total_count, TOK_LBRACE, TOK_RBRACE);
            TypeConstraint vtype = TC_NONE, veffout = TC_NONE;
            int pairs = 0, has_eff = 0;
            for (int j = i+1; j < close; ) {
                if (toks[j].tag == TOK_LPAREN) j = find_matching(toks, j+1, total_count, TOK_LPAREN, TOK_RPAREN) + 1;
                else j++;
                if (j >= close) break;
                TypeConstraint tv = TC_NONE;
                if (toks[j].tag == TOK_LPAREN) {
                    int vc2 = find_matching(toks, j+1, total_count, TOK_LPAREN, TOK_RPAREN);
                    int vc = 0, vp = 0; TypeConstraint vo = tc_infer_effect_ctx(toks, j+1, vc2, total_count, &vc, &vp, tc);
                    tv = TC_TUPLE; if (!pairs) { veffout = vo; has_eff = 1; }
                    if (has_eff && vo != TC_NONE && veffout != TC_NONE && vo != veffout && !tc_constraint_matches(veffout, vo) && !tc_constraint_matches(vo, veffout))
                        tc_error(tc, t->line, 0, "clause bodies produce different types: %s vs %s", constraint_name(veffout), constraint_name(vo));
                    j = vc2 + 1;
                } else {
                    if (toks[j].tag == TOK_INT) tv = TC_INT; else if (toks[j].tag == TOK_FLOAT) tv = TC_FLOAT;
                    else if (toks[j].tag == TOK_SYM) tv = TC_SYM; else if (toks[j].tag == TOK_STRING) tv = TC_LIST;
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
            if (sym == S_DEF) {
                if (tc->recur_pending) {
                    if (tc->sp >= 1) {
                        AbstractType vt = tc->data[tc->sp-1]; tc->sp--;
                        if (i >= tc->user_start) {
                            if (tc_is_builtin(tc->recur_sym, tc->prelude_sig_count)) tc_error(tc, t->line, 0, "'%s' is already defined", sym_name(tc->recur_sym));
                            else { TCBinding *existing = tc_lookup(tc, tc->recur_sym); if (existing) tc_error(tc, t->line, 0, "'%s' is already defined (first defined on line %d)", sym_name(tc->recur_sym), existing->def_line); }
                        }
                        tc_bind(tc, tc->recur_sym, &vt, 1, t->line);
                    }
                    tc->recur_pending = 0;
                } else {
                    if (tc->sp >= 2) {
                        AbstractType vt = tc->data[tc->sp-1]; uint32_t name_sym = tc->data[tc->sp-2].sym_id;
                        tc->sp -= 2;
                        if (name_sym) {
                            if (i >= tc->user_start && tc->sp_floor == 0) {
                                if (tc_is_builtin(name_sym, tc->prelude_sig_count)) tc_error(tc, t->line, 0, "'%s' is already defined", sym_name(name_sym));
                                else { TCBinding *existing = tc_lookup(tc, name_sym); if (existing) tc_error(tc, t->line, 0, "'%s' is already defined (first defined on line %d)", sym_name(name_sym), existing->def_line); }
                            }
                            tc_bind(tc, name_sym, &vt, 1, t->line);
                        }
                    } else if (tc->sp_floor == 0) tc->sp = 0;
                }
            } else if (sym == S_LET) {
                if (tc->sp >= 2) {
                    uint32_t name_sym = tc->data[tc->sp-1].sym_id; tc->sp--;
                    AbstractType val_t = tc->data[tc->sp-1]; tc->sp--;
                    if (name_sym) {
                        if (i >= tc->user_start && tc->body_depth == 0) {
                            if (tc_is_builtin(name_sym, tc->prelude_sig_count)) tc_error(tc, t->line, 0, "'%s' shadows existing definition", sym_name(name_sym));
                            else {
                                TCBinding *existing = tc_lookup(tc, name_sym);
                                if (existing) {
                                    if (existing->is_def) tc_error(tc, t->line, 0, "'%s' shadows existing definition (defined on line %d)", sym_name(name_sym), existing->def_line);
                                    else tc_error(tc, t->line, 0, "'%s' is already bound (bound on line %d)", sym_name(name_sym), existing->def_line);
                                }
                            }
                        }
                        tc_bind(tc, name_sym, &val_t, 0, t->line);
                    }
                } else if (tc->sp_floor == 0) tc->sp = 0;
            } else if (sym == S_RECUR) {
                if (tc->sp >= 1) { uint32_t name_sym = tc->data[tc->sp-1].sym_id; tc->recur_pending = 1; tc->sp--; if (name_sym) tc->recur_sym = name_sym; }
            } else if (sym == S_EFFECT) {
                if (tc->sp > 0) tc->sp--;
                int be = i - 1;
                if (be >= start && toks[be].tag == TOK_RBRACKET) {
                    int d = 1, bs = be;
                    for (int bi = be-1; bi >= start; bi--) { if (toks[bi].tag == TOK_RBRACKET) d++; else if (toks[bi].tag == TOK_LBRACKET && --d == 0) { bs = bi; break; } }
                    TypeSig sig = parse_type_annotation(toks, bs+1, be);
                    if (tc->sp >= 1 && tc->data[tc->sp-1].type == TC_TUPLE) {
                        if (tc->sp >= 2 && tc->data[tc->sp-2].type == TC_SYM) typesig_register(tc->data[tc->sp-2].sym_id, &sig);
                        for (int b2 = i-2; b2 >= 0; b2--) if (toks[b2].tag == TOK_RPAREN) {
                            int d2 = 1;
                            for (int k = b2-1; k >= 0; k--) { if (toks[k].tag == TOK_RPAREN) d2++; else if (toks[k].tag == TOK_LPAREN && --d2 == 0) { tc->errors += tc_check_body_against_sig(toks, k+1, b2, total_count, &sig); break; } }
                            break;
                        }
                    } else if (tc->sp >= 1 && tc->data[tc->sp-1].type == TC_SYM) {
                        typesig_register(tc->data[tc->sp-1].sym_id, &sig);
                        if (!(i+1 < end && toks[i+1].tag == TOK_WORD && toks[i+1].as.sym == S_DEF)) tc->sp--;
                    }
                }
            } else if (sym == S_CHECK) {
                if (i >= 1 && toks[i-1].tag == TOK_WORD) {
                    TypeConstraint exp = parse_constraint(sym_name(toks[i-1].as.sym));
                    if (exp != TC_NONE && tc->sp > 0 && tc->data[tc->sp-1].type != exp && tc->data[tc->sp-1].type != TC_NONE)
                        tc_error(tc, t->line, 0, "'check' expected %s, got %s", constraint_name(exp), constraint_name(tc->data[tc->sp-1].type));
                    if (tc->unknown_count > 0 && tc->unknowns[tc->unknown_count-1].sym == toks[i-1].as.sym) tc->unknown_count--;
                }
            } else tc_check_word(tc, sym, t->line);
            break;
        }
        default: break;
        }
    }
}

static int typecheck_tokens(Token *toks, int count, int user_start) {
    TypeChecker tc; memset(&tc, 0, sizeof(tc)); tc.tvar_count = 1; tc.user_start = user_start;
    tc_process_range(&tc, toks, 0, count, count);
    for (int i = 0; i < tc.sp; i++)
        if ((tc.data[i].flags & AT_LINEAR) && !(tc.data[i].flags & AT_CONSUMED))
            tc_error(&tc, tc.data[i].source_line, 0, "linear value (box) created here was never consumed (must free, lend, mutate, or clone)");
    for (int i = 0; i < tc.unknown_count; i++) {
        uint32_t sym = tc.unknowns[i].sym; int defined = 0;
        for (int j = 0; j < tc.bind_count; j++) if (tc.bindings[j].sym == sym) { defined = 1; break; }
        if (!defined) tc_error(&tc, tc.unknowns[i].line, 0, "unknown word '%s'", sym_name(sym));
    }
    return tc.errors;
}

/* ---- PRIMITIVES ---- */

#define POP_VAL(name) \
    Value name##_top = stack[sp-1]; int name##_s = val_slots(name##_top); \
    if (name##_s > LOCAL_MAX) die("value too large (%d slots, max %d)", name##_s, LOCAL_MAX); \
    if (name##_s > sp) die("stack underflow: need %d slots, have %d", name##_s, sp); \
    Value name##_buf[LOCAL_MAX]; memcpy(name##_buf, &stack[sp-name##_s], name##_s*sizeof(Value)); sp -= name##_s
#define POP_BODY(name, label) if (stack[sp-1].tag != VAL_TUPLE) die(label ": expected tuple"); POP_VAL(name)
#define POP_LIST_BUF(name, label) \
    if (stack[sp-1].tag != VAL_LIST) die(label ": expected list"); \
    Value name##_top = stack[sp-1]; int name##_s = val_slots(name##_top); \
    if (name##_s > LOCAL_MAX) die(label ": list too large (%d slots, max %d)", name##_s, LOCAL_MAX); \
    if (name##_s > sp) die(label ": stack underflow: need %d slots, have %d", name##_s, sp); \
    int name##_len = (int)name##_top.as.compound.len; \
    int name##_base __attribute__((unused)) = sp - name##_s; \
    Value name##_buf[LOCAL_MAX]; memcpy(name##_buf, &stack[name##_base], name##_s*sizeof(Value)); sp = name##_base

static void prim_dup(Frame *e) { (void)e; if (sp<=0) die("dup: stack underflow"); Value top=stack[sp-1]; int s=val_slots(top); if(sp+s>STACK_MAX) die("dup: stack overflow"); memcpy(&stack[sp],&stack[sp-s],s*sizeof(Value)); sp+=s; }
static void prim_drop(Frame *e) { (void)e; if (sp<=0) die("drop: stack underflow"); sp-=val_slots(stack[sp-1]); }
static void prim_swap(Frame *e) {
    (void)e; if (sp<=0) die("swap: stack underflow");
    Value top=stack[sp-1]; int top_s=val_slots(top); if(sp<top_s) die("swap: stack underflow");
    int bp=sp-top_s-1; if(bp<0) die("swap: stack underflow");
    int below_s=val_slots(stack[bp]); int total=top_s+below_s; if(sp<total) die("swap: stack underflow");
    Value tmp[LOCAL_MAX]; int base=sp-total;
    memcpy(tmp,&stack[base],below_s*sizeof(Value));
    memmove(&stack[base],&stack[base+below_s],top_s*sizeof(Value));
    memcpy(&stack[base+top_s],tmp,below_s*sizeof(Value));
}
static void prim_dip(Frame *env) { if(sp<2) die("dip: need body and value"); POP_BODY(body,"dip"); POP_VAL(saved); eval_body(body_buf,body_s,env); memcpy(&stack[sp],saved_buf,saved_s*sizeof(Value)); sp+=saved_s; }
static void prim_apply(Frame *env) { if(sp<=0) die("apply: stack underflow"); POP_BODY(body,"apply"); eval_body(body_buf,body_s,env); }

#define ARITH2(nm,iop,fop) static void prim_##nm(Frame *e){(void)e;Value b=spop(),a=spop(); \
    if(a.tag==VAL_INT&&b.tag==VAL_INT) spush(val_int(a.as.i iop b.as.i)); \
    else if(a.tag==VAL_FLOAT&&b.tag==VAL_FLOAT) spush(val_float(a.as.f fop b.as.f)); \
    else die(#nm ": type mismatch");}
ARITH2(plus,+,+) ARITH2(sub,-,-) ARITH2(mul,*,*)

static void prim_div(Frame *e) { (void)e; Value b=spop(),a=spop();
    if(a.tag==VAL_INT&&b.tag==VAL_INT){if(b.as.i==0)die("div: division by zero");spush(val_int(a.as.i/b.as.i));}
    else if(a.tag==VAL_FLOAT&&b.tag==VAL_FLOAT)spush(val_float(a.as.f/b.as.f)); else die("div: type mismatch"); }
static void prim_mod(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();if(b==0)die("mod: division by zero");spush(val_int(a%b));}
static void prim_divmod(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();if(b==0)die("divmod: division by zero");spush(val_int(a%b));spush(val_int(a/b));}
static void prim_wrap(Frame *e){(void)e;int64_t m=pop_int(),v=pop_int();if(m==0)die("wrap: modulus must be non-zero");spush(val_int(((v%m)+m)%m));}

#define CMP2(nm,expr) static void prim_##nm(Frame *e){(void)e;Value bt=stack[sp-1];int bs=val_slots(bt);int as=val_slots(stack[sp-1-bs]);int r=(expr);sp-=as+bs;spush(val_int(r?1:0));}
CMP2(eq, val_equal(&stack[sp-bs-as],as,&stack[sp-bs],bs))
CMP2(lt, val_less(&stack[sp-bs-as],as,&stack[sp-bs],bs))

static void prim_and(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();spush(val_int((a&&b)?1:0));}
static void prim_or(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();spush(val_int((a||b)?1:0));}

static void prim_print(Frame *e){(void)e;if(sp<=0)die("print: stack underflow");Value top=stack[sp-1];int s=val_slots(top);val_print(&stack[sp-s],s,stdout);printf("\n");sp-=s;}
static void prim_assert(Frame *e){(void)e;if(!pop_int())die("assertion failed");}
static void prim_halt(Frame *e){(void)e;exit(0);}
static void prim_random(Frame *e){(void)e;int64_t max=pop_int();if(max<=0)die("random: max must be positive");spush(val_int(rand()%max));}

static void eval_default(Value *buf, int s, Value top, Value *scrut, int scrut_s, Frame *env) {
    if (top.tag == VAL_TUPLE) {
        if (scrut) { memcpy(&stack[sp], scrut, scrut_s*sizeof(Value)); sp += scrut_s; }
        eval_body(buf, s, env);
    } else { memcpy(&stack[sp], buf, s*sizeof(Value)); sp += s; }
}

static void prim_if(Frame *env) {
    POP_VAL(el); POP_BODY(then,"if");
    Value cond=spop(); if(cond.tag!=VAL_INT) die("if: condition must be int, got tag %d",cond.tag);
    if(cond.as.i) eval_body(then_buf,then_s,env);
    else eval_default(el_buf,el_s,el_top,NULL,0,env);
}

static void prim_cond(Frame *env) {
    POP_VAL(def);
    Value clauses_top=stack[sp-1];
    if(clauses_top.tag!=VAL_TUPLE&&clauses_top.tag!=VAL_RECORD) die("cond: expected tuple or record of clauses");
    int clauses_s=val_slots(clauses_top),clauses_len=(int)clauses_top.as.compound.len;
    Value clauses_buf[LOCAL_MAX]; memcpy(clauses_buf,&stack[sp-clauses_s],clauses_s*sizeof(Value)); sp-=clauses_s;
    POP_VAL(scrut);
    if(clauses_len%2!=0) die("cond: need even number of clauses (pred/body pairs)");
    for(int i=0;i<clauses_len;i+=2){
        ElemRef pred_ref=compound_elem(clauses_buf,clauses_s,clauses_len,i);
        ElemRef body_ref=compound_elem(clauses_buf,clauses_s,clauses_len,i+1);
        memcpy(&stack[sp],scrut_buf,scrut_s*sizeof(Value)); sp+=scrut_s;
        eval_body(&clauses_buf[pred_ref.base],pred_ref.slots,env);
        if(pop_int()){memcpy(&stack[sp],scrut_buf,scrut_s*sizeof(Value));sp+=scrut_s;eval_body(&clauses_buf[body_ref.base],body_ref.slots,env);return;}
    }
    eval_default(def_buf,def_s,def_top,scrut_buf,scrut_s,env);
}

static void prim_match(Frame *env) {
    POP_VAL(def);
    Value clauses_top=stack[sp-1];
    if(clauses_top.tag!=VAL_TUPLE&&clauses_top.tag!=VAL_RECORD) die("match: expected tuple of clauses");
    int clauses_s=val_slots(clauses_top),clauses_len=(int)clauses_top.as.compound.len;
    Value clauses_buf[LOCAL_MAX]; memcpy(clauses_buf,&stack[sp-clauses_s],clauses_s*sizeof(Value)); sp-=clauses_s;
    Value scrut=spop(); if(scrut.tag!=VAL_SYM) die("match: scrutinee must be a symbol");
    if(clauses_top.tag==VAL_RECORD){
        int found; ElemRef body_ref=record_field(clauses_buf,clauses_s,clauses_len,scrut.as.sym,&found);
        if(found){eval_body(&clauses_buf[body_ref.base],body_ref.slots,env);return;}
    } else {
        if(clauses_len%2!=0) die("match: need even number of clauses (pattern/body pairs)");
        for(int i=0;i<clauses_len;i+=2){
            ElemRef pat_ref=compound_elem(clauses_buf,clauses_s,clauses_len,i);
            ElemRef body_ref=compound_elem(clauses_buf,clauses_s,clauses_len,i+1);
            Value pat=clauses_buf[pat_ref.base];
            if(pat.tag==VAL_SYM&&pat.as.sym==scrut.as.sym){eval_body(&clauses_buf[body_ref.base],body_ref.slots,env);return;}
        }
    }
    eval_default(def_buf,def_s,def_top,NULL,0,env);
}

static void prim_loop(Frame *env) {
    if(stack[sp-1].tag!=VAL_TUPLE) die("loop: expected tuple");
    int body_s=val_slots(stack[sp-1]); if(body_s>LOCAL_MAX) die("loop: body too large"); if(body_s>sp) die("loop: stack underflow");
    Value body_buf[body_s]; memcpy(body_buf,&stack[sp-body_s],body_s*sizeof(Value)); sp-=body_s;
    for(;;){eval_body(body_buf,body_s,env);if(!pop_int())break;}
}

static void prim_while(Frame *env) {
    if(stack[sp-1].tag!=VAL_TUPLE) die("while: expected tuple");
    int body_s=val_slots(stack[sp-1]); if(body_s>LOCAL_MAX||body_s>sp) die("while: stack underflow");
    Value body_buf[body_s]; memcpy(body_buf,&stack[sp-body_s],body_s*sizeof(Value)); sp-=body_s;
    if(stack[sp-1].tag!=VAL_TUPLE) die("while: expected tuple");
    int pred_s=val_slots(stack[sp-1]); if(pred_s>LOCAL_MAX||pred_s>sp) die("while: stack underflow");
    Value pred_buf[pred_s]; memcpy(pred_buf,&stack[sp-pred_s],pred_s*sizeof(Value)); sp-=pred_s;
    for(;;){eval_body(pred_buf,pred_s,env);if(!pop_int())break;eval_body(body_buf,body_s,env);}
}

static void prim_itof(Frame *e){(void)e;spush(val_float((double)pop_int()));}
static void prim_ftoi(Frame *e){(void)e;spush(val_int((int64_t)pop_float()));}
#define FLOAT1(nm,fn) static void prim_##nm(Frame *e){(void)e;spush(val_float(fn(pop_float())));}
FLOAT1(fsqrt,sqrt) FLOAT1(fsin,sin) FLOAT1(fcos,cos) FLOAT1(ftan,tan)
FLOAT1(ffloor,floor) FLOAT1(fceil,ceil) FLOAT1(fround,round) FLOAT1(fexp,exp) FLOAT1(flog,log)
static void prim_fpow(Frame *e){(void)e;double b=pop_float(),a=pop_float();spush(val_float(pow(a,b)));}
static void prim_fatan2(Frame *e){(void)e;double b=pop_float(),a=pop_float();spush(val_float(atan2(a,b)));}

static void prim_stack(Frame *e){(void)e;spush(val_compound(VAL_TUPLE,0,1));}

static void prim_size(Frame *e) {
    (void)e; Value top=speek();
    if(!is_compound(top.tag)) die("size: expected compound");
    sp-=val_slots(top); spush(val_int((int)top.as.compound.len));
}

static void prim_push_op(Frame *e) {
    (void)e; POP_VAL(v); Value ct=stack[sp-1];
    if(!is_compound(ct.tag)) die("push: expected compound");
    ValTag tag=ct.tag; int cs=val_slots(ct),cl=(int)ct.as.compound.len; sp--;
    memcpy(&stack[sp],v_buf,v_s*sizeof(Value)); sp+=v_s;
    spush(val_compound(tag,cl+1,cs+v_s));
}

static void prim_pop_impl(const char *label) {
    Value top=speek();
    if(!is_compound(top.tag)) die("%s: expected compound", label);
    ValTag tag=top.tag; int s=val_slots(top),len=(int)top.as.compound.len;
    if(len==0) die("%s: empty %s", label, tag==VAL_LIST?"list":tag==VAL_TUPLE?"tuple":"record");
    int base=sp-s; ElemRef last=compound_elem(&stack[base],s,len,len-1);
    Value elem_buf[LOCAL_MAX]; memcpy(elem_buf,&stack[base+last.base],last.slots*sizeof(Value));
    sp--; sp-=last.slots; spush(val_compound(tag,len-1,s-last.slots));
    memcpy(&stack[sp],elem_buf,last.slots*sizeof(Value)); sp+=last.slots;
}
static void prim_pop_op(Frame *e){(void)e;prim_pop_impl("pop");}

static void prim_elem(int consume) {
    int64_t idx=pop_int(); Value top=speek();
    if(!is_compound(top.tag)) die(consume?"get: expected compound":"pull: expected compound");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    ElemRef ref=compound_elem(&stack[base],s,len,(int)idx);
    Value eb[LOCAL_MAX]; memcpy(eb,&stack[base+ref.base],ref.slots*sizeof(Value));
    if(consume) sp-=s;
    memcpy(&stack[sp],eb,ref.slots*sizeof(Value)); sp+=ref.slots;
}
static void prim_pull(Frame *e){(void)e;prim_elem(0);}

static void prim_replace_at(Frame *e) {
    (void)e; POP_VAL(v); int64_t idx=pop_int(); Value top=speek();
    if(!is_compound(top.tag)) die("put: expected compound");
    ValTag tag=top.tag; int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    ElemRef old_ref=compound_elem(&stack[base],s,len,(int)idx);
    if(old_ref.slots==v_s) memcpy(&stack[base+old_ref.base],v_buf,v_s*sizeof(Value));
    else {
        Value tmp[LOCAL_MAX]; int tmp_sp=0;
        for(int i=0;i<len;i++){
            ElemRef r=compound_elem(&stack[base],s,len,i);
            if(i==(int)idx){memcpy(&tmp[tmp_sp],v_buf,v_s*sizeof(Value));tmp_sp+=v_s;}
            else{memcpy(&tmp[tmp_sp],&stack[base+r.base],r.slots*sizeof(Value));tmp_sp+=r.slots;}
        }
        sp=base; memcpy(&stack[sp],tmp,tmp_sp*sizeof(Value)); sp+=tmp_sp;
        spush(val_compound(tag,len,tmp_sp+1));
    }
}

static void prim_concat(Frame *e) {
    (void)e; Value top2=stack[sp-1];
    if(!is_compound(top2.tag)) die("concat: expected compound");
    ValTag tag=top2.tag; int s2=val_slots(top2),len2=(int)top2.as.compound.len,base2=sp-s2;
    Value below=stack[base2-1];
    if(!is_compound(below.tag)) die("concat: expected compound");
    int s1=val_slots(below),len1=(int)below.as.compound.len,base1=base2-s1;
    int new_elem_slots=(s1-1)+(s2-1); Value tmp[LOCAL_MAX];
    memcpy(tmp,&stack[base1],(s1-1)*sizeof(Value));
    memcpy(&tmp[s1-1],&stack[base2],(s2-1)*sizeof(Value));
    sp=base1; memcpy(&stack[sp],tmp,new_elem_slots*sizeof(Value)); sp+=new_elem_slots;
    spush(val_compound(tag,len1+len2,new_elem_slots+1));
}

static void prim_list(Frame *e){(void)e;spush(val_compound(VAL_LIST,0,1));}
static void prim_grab(Frame *e){(void)e;prim_pop_impl("grab");}

static void prim_get(Frame *e){(void)e;prim_elem(1);}

static void prim_nth(Frame *env) {
    int64_t idx=pop_int(); uint32_t sym=pop_sym();
    Lookup lu=frame_lookup(env,sym); if(!lu.found) die("nth: unknown word: %s",sym_name(sym));
    Value *data=&lu.frame->vals[lu.offset]; int s=lu.slots;
    Value top=data[s-1]; if(!is_compound(top.tag)) die("nth: expected compound");
    int len=(int)top.as.compound.len;
    ElemRef ref=compound_elem(data,s,len,(int)idx);
    memcpy(&stack[sp],&data[ref.base],ref.slots*sizeof(Value)); sp+=ref.slots;
}

static void prim_slice_n(int take) {
    int64_t n=pop_int(); Value top=speek(); if(top.tag!=VAL_LIST) die(take?"take-n: expected list":"drop-n: expected list");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    if(n<0||n>len) die(take?"take-n: n out of range":"drop-n: n out of range");
    Value tmp[LOCAL_MAX]; int tmp_sp=0;
    int start=take?0:(int)n, end_i=take?(int)n:len;
    for(int i=start;i<end_i;i++){ElemRef r=compound_elem(&stack[base],s,len,i);memcpy(&tmp[tmp_sp],&stack[base+r.base],r.slots*sizeof(Value));tmp_sp+=r.slots;}
    sp=base; memcpy(&stack[sp],tmp,tmp_sp*sizeof(Value)); sp+=tmp_sp;
    spush(val_compound(VAL_LIST,end_i-start,tmp_sp+1));
}
static void prim_take_n(Frame *e){(void)e;prim_slice_n(1);}
static void prim_drop_n(Frame *e){(void)e;prim_slice_n(0);}

static void prim_range(Frame *e){(void)e;int64_t end=pop_int(),start=pop_int();int count=0;for(int64_t i=start;i<end;i++){spush(val_int(i));count++;}spush(val_compound(VAL_LIST,count,count+1));}

static void prim_map(Frame *env) {
    POP_BODY(fn,"map"); POP_LIST_BUF(list,"map");
    int offs[LOCAL_MAX],szs[LOCAL_MAX]; compute_offsets(list_buf,list_s,list_len,offs,szs);
    int rb=sp;
    for(int i=0;i<list_len;i++){memcpy(&stack[sp],&list_buf[offs[i]],szs[i]*sizeof(Value));sp+=szs[i];eval_body(fn_buf,fn_s,env);}
    spush(val_compound(VAL_LIST,list_len,sp-rb+1));
}

static void prim_filter(Frame *env) {
    POP_BODY(fn,"filter"); POP_LIST_BUF(list,"filter");
    int offs[LOCAL_MAX],szs[LOCAL_MAX]; compute_offsets(list_buf,list_s,list_len,offs,szs);
    int rb=sp,rc=0;
    for(int i=0;i<list_len;i++){
        memcpy(&stack[sp],&list_buf[offs[i]],szs[i]*sizeof(Value));sp+=szs[i];
        memcpy(&stack[sp],&list_buf[offs[i]],szs[i]*sizeof(Value));sp+=szs[i];
        eval_body(fn_buf,fn_s,env); if(pop_int())rc++; else sp-=szs[i];
    }
    spush(val_compound(VAL_LIST,rc,sp-rb+1));
}

static void prim_fold(Frame *env) {
    POP_BODY(fn,"fold"); POP_VAL(init); POP_LIST_BUF(list,"fold");
    int offs[LOCAL_MAX],szs[LOCAL_MAX]; compute_offsets(list_buf,list_s,list_len,offs,szs);
    memcpy(&stack[sp],init_buf,init_s*sizeof(Value));sp+=init_s;
    for(int i=0;i<list_len;i++){memcpy(&stack[sp],&list_buf[offs[i]],szs[i]*sizeof(Value));sp+=szs[i];eval_body(fn_buf,fn_s,env);}
}
static void prim_each(Frame *env) {
    POP_BODY(fn,"each"); POP_LIST_BUF(list,"each");
    int offs[LOCAL_MAX],szs[LOCAL_MAX]; compute_offsets(list_buf,list_s,list_len,offs,szs);
    for(int i=0;i<list_len;i++){memcpy(&stack[sp],&list_buf[offs[i]],szs[i]*sizeof(Value));sp+=szs[i];eval_body(fn_buf,fn_s,env);}
}

static int sort_cmp(const void *a,const void *b) {
    const Value *va=(const Value*)a,*vb=(const Value*)b;
    if(va->tag==VAL_INT&&vb->tag==VAL_INT) return(va->as.i>vb->as.i)-(va->as.i<vb->as.i);
    if(va->tag==VAL_FLOAT&&vb->tag==VAL_FLOAT) return(va->as.f>vb->as.f)-(va->as.f<vb->as.f);
    die("sort: unsupported element type"); return 0;
}
static void prim_sort(Frame *e){(void)e;Value top=speek();if(top.tag!=VAL_LIST)die("sort: expected list");int s=val_slots(top),len=(int)top.as.compound.len;if(s!=len+1)die("sort: only single-slot elements (int/float) supported");qsort(&stack[sp-s],len,sizeof(Value),sort_cmp);}

static void prim_index_of(Frame *e) {
    (void)e; Value val=spop(); Value top=speek();
    if(top.tag!=VAL_LIST) die("index-of: expected list");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s,found=-1;
    for(int i=0;i<len;i++){ElemRef r=compound_elem(&stack[base],s,len,i);if(val_equal(&stack[base+r.base],r.slots,&val,1)){found=i;break;}}
    sp-=s; spush(val_int(found));
}

static void prim_scan(Frame *env) {
    POP_BODY(fn,"scan"); POP_VAL(init); POP_LIST_BUF(list,"scan");
    int offs[LOCAL_MAX],szs[LOCAL_MAX]; compute_offsets(list_buf,list_s,list_len,offs,szs);
    Value acc_buf[LOCAL_MAX]; int acc_s=init_s; memcpy(acc_buf,init_buf,init_s*sizeof(Value));
    int rb=sp;
    for(int i=0;i<list_len;i++){
        memcpy(&stack[sp],acc_buf,acc_s*sizeof(Value));sp+=acc_s;
        memcpy(&stack[sp],&list_buf[offs[i]],szs[i]*sizeof(Value));sp+=szs[i];
        eval_body(fn_buf,fn_s,env);
        Value new_acc_top=stack[sp-1]; acc_s=val_slots(new_acc_top); memcpy(acc_buf,&stack[sp-acc_s],acc_s*sizeof(Value));
    }
    spush(val_compound(VAL_LIST,list_len,sp-rb+1));
}
static void prim_at(Frame *env) {
    (void)env; uint32_t key=pop_sym();
    if(sp<=0) die("at: stack underflow"); Value next=stack[sp-1];
    if(next.tag!=VAL_RECORD) die("at: expected record, got tag %d",next.tag);
    int s=val_slots(next),len=(int)next.as.compound.len,base=sp-s;
    int found; ElemRef ref=record_field(&stack[base],s,len,key,&found);
    if(!found) die("at: key '%s' not found in record",sym_name(key));
    Value vb[LOCAL_MAX]; memcpy(vb,&stack[base+ref.base],ref.slots*sizeof(Value));
    sp-=s; memcpy(&stack[sp],vb,ref.slots*sizeof(Value)); sp+=ref.slots;
}

static void prim_rec(Frame *e){(void)e;spush(val_compound(VAL_RECORD,0,1));}

static void prim_into(Frame *e) {
    (void)e; uint32_t key=pop_sym(); POP_VAL(v);
    Value rec_top=stack[sp-1]; if(rec_top.tag!=VAL_RECORD) die("into: expected record");
    int rec_s=val_slots(rec_top),rec_len=(int)rec_top.as.compound.len,rec_base=sp-rec_s;
    int found; ElemRef existing=record_field(&stack[rec_base],rec_s,rec_len,key,&found);
    if(found&&existing.slots==v_s) memcpy(&stack[rec_base+existing.base],v_buf,v_s*sizeof(Value));
    else {
        Value tmp[LOCAL_MAX]; int tmp_sp=0,new_len=0;
        int replaced=0;
        if(rec_len>LOCAL_MAX) die("into: record too large");
        int kpos[LOCAL_MAX],voff[LOCAL_MAX],vsz[LOCAL_MAX];
        record_offsets(&stack[rec_base],rec_s,rec_len,kpos,voff,vsz);
        for(int i=0;i<rec_len;i++){
            if(stack[rec_base+kpos[i]].tag!=VAL_SYM) die("into: record key is not a symbol");
            uint32_t k=stack[rec_base+kpos[i]].as.sym;
            if(k==key){tmp[tmp_sp++]=val_sym(key);memcpy(&tmp[tmp_sp],v_buf,v_s*sizeof(Value));tmp_sp+=v_s;replaced=1;}
            else{tmp[tmp_sp++]=stack[rec_base+kpos[i]];memcpy(&tmp[tmp_sp],&stack[rec_base+voff[i]],vsz[i]*sizeof(Value));tmp_sp+=vsz[i];}
            new_len++;
        }
        if(!replaced){tmp[tmp_sp++]=val_sym(key);memcpy(&tmp[tmp_sp],v_buf,v_s*sizeof(Value));tmp_sp+=v_s;new_len++;}
        sp=rec_base; memcpy(&stack[sp],tmp,tmp_sp*sizeof(Value)); sp+=tmp_sp;
        spush(val_compound(VAL_RECORD,new_len,tmp_sp+1));
    }
}

typedef struct BoxData { Value *data; int slots; } BoxData;
static void prim_box(Frame *e){(void)e;Value top=stack[sp-1];int s=val_slots(top);BoxData *bd=malloc(sizeof(BoxData));bd->data=malloc(s*sizeof(Value));bd->slots=s;memcpy(bd->data,&stack[sp-s],s*sizeof(Value));sp-=s;Value v;v.tag=VAL_BOX;v.as.box=bd;spush(v);}
static void prim_free(Frame *e){(void)e;Value v=spop();if(v.tag!=VAL_BOX)die("free: expected box");BoxData *bd=(BoxData*)v.as.box;free(bd->data);free(bd);}

static void prim_lend(Frame *env) {
    POP_BODY(fn,"lend"); Value box_val=spop(); if(box_val.tag!=VAL_BOX) die("lend: expected box");
    BoxData *bd=(BoxData*)box_val.as.box;
    memcpy(&stack[sp],bd->data,bd->slots*sizeof(Value)); sp+=bd->slots;
    int sp_before=sp-bd->slots; eval_body(fn_buf,fn_s,env);
    int results_slots=sp-sp_before; if(results_slots>LOCAL_MAX) die("lend: result too large");
    Value results[LOCAL_MAX]; memcpy(results,&stack[sp_before],results_slots*sizeof(Value)); sp=sp_before;
    spush(box_val); memcpy(&stack[sp],results,results_slots*sizeof(Value)); sp+=results_slots;
}

static void prim_mutate(Frame *env) {
    POP_BODY(fn,"mutate"); Value box_val=spop(); if(box_val.tag!=VAL_BOX) die("mutate: expected box");
    BoxData *bd=(BoxData*)box_val.as.box;
    memcpy(&stack[sp],bd->data,bd->slots*sizeof(Value)); sp+=bd->slots;
    eval_body(fn_buf,fn_s,env);
    Value new_top=stack[sp-1]; int new_s=val_slots(new_top);
    free(bd->data); bd->data=malloc(new_s*sizeof(Value)); bd->slots=new_s;
    memcpy(bd->data,&stack[sp-new_s],new_s*sizeof(Value)); sp-=new_s; spush(box_val);
}

static void deep_copy_values(Value *dst, const Value *src, int slots) {
    memcpy(dst,src,slots*sizeof(Value));
    for(int i=0;i<slots;i++) if(dst[i].tag==VAL_BOX){
        BoxData *orig=(BoxData*)dst[i].as.box; BoxData *copy=malloc(sizeof(BoxData));
        copy->slots=orig->slots; copy->data=malloc(orig->slots*sizeof(Value));
        deep_copy_values(copy->data,orig->data,orig->slots); dst[i].as.box=copy;
    }
}

static void prim_clone(Frame *e){(void)e;Value v=spop();if(v.tag!=VAL_BOX)die("clone: expected box");BoxData *orig=(BoxData*)v.as.box;BoxData *copy=malloc(sizeof(BoxData));copy->data=malloc(orig->slots*sizeof(Value));copy->slots=orig->slots;deep_copy_values(copy->data,orig->data,orig->slots);spush(v);Value v2;v2.tag=VAL_BOX;v2.as.box=copy;spush(v2);}

static void prim_grade(Frame *e,int ascending) {
    (void)e; Value top=speek(); if(top.tag!=VAL_LIST) die(ascending?"rise: expected list":"fall: expected list");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    typedef struct{int idx;Value val;} IV;
    IV *items=malloc(len*sizeof(IV));
    for(int i=0;i<len;i++){ElemRef r=compound_elem(&stack[base],s,len,i);items[i].idx=i;items[i].val=stack[base+r.base];}
    for(int i=0;i<len-1;i++) for(int j=i+1;j<len;j++){
        int cmp=0;
        if(items[i].val.tag==VAL_INT) cmp=ascending?(items[i].val.as.i>items[j].val.as.i):(items[i].val.as.i<items[j].val.as.i);
        else if(items[i].val.tag==VAL_FLOAT) cmp=ascending?(items[i].val.as.f>items[j].val.as.f):(items[i].val.as.f<items[j].val.as.f);
        if(cmp){IV tmp=items[i];items[i]=items[j];items[j]=tmp;}
    }
    sp-=s; for(int i=0;i<len;i++) spush(val_int(items[i].idx));
    spush(val_compound(VAL_LIST,len,len+1)); free(items);
}
static void prim_rise(Frame *e){prim_grade(e,1);}
static void prim_fall(Frame *e){prim_grade(e,0);}
static void prim_shape(Frame *e) {
    (void)e; Value top=speek(); if(top.tag!=VAL_LIST) die("shape: expected list");
    int len=(int)top.as.compound.len,s=val_slots(top),base=sp-s;
    int dims[16],ndims=0; dims[ndims++]=len;
    if(len>0){ElemRef r0=compound_elem(&stack[base],s,len,0);Value e0=stack[base+r0.base+r0.slots-1];if(e0.tag==VAL_LIST)dims[ndims++]=(int)e0.as.compound.len;}
    sp-=s; for(int i=0;i<ndims;i++) spush(val_int(dims[i])); spush(val_compound(VAL_LIST,ndims,ndims+1));
}

/* ---- EVAL ---- */

static void dispatch_word(uint32_t sym, Frame *env) {
    Lookup lu=frame_lookup(env,sym);
    if(!lu.found){PrimFn fn=prim_lookup(sym);if(fn){fn(env);return;}die("unknown word: %s",sym_name(sym));}
    if(lu.kind==BIND_DEF){Value bt=lu.frame->vals[lu.offset+lu.slots-1];if(bt.tag==VAL_TUPLE){eval_body(&lu.frame->vals[lu.offset],lu.slots,env);return;}}
    memcpy(&stack[sp],&lu.frame->vals[lu.offset],lu.slots*sizeof(Value)); sp+=lu.slots;
}

static void eval_body(Value *body, int slots, Frame *env) {
    Value hdr=body[slots-1]; if(hdr.tag!=VAL_TUPLE) die("eval_body: expected tuple");
    int len=(int)hdr.as.compound.len;
    Frame *exec_env=hdr.as.compound.env?hdr.as.compound.env:env;
    int saved_bc=exec_env->bind_count,saved_vu=exec_env->vals_used;
    int all_scalar=(slots==len+1);
    if(len>LOCAL_MAX) die("tuple body too large");
    int offsets_buf[LOCAL_MAX],sizes_buf[LOCAL_MAX];
    if(!all_scalar) compute_offsets(body,slots,len,offsets_buf,sizes_buf);
    for(int k=0;k<len;k++){
        int eoff,esz; if(all_scalar){eoff=k;esz=1;}else{eoff=offsets_buf[k];esz=sizes_buf[k];}
        Value elem=body[eoff+esz-1];
        if(elem.tag==VAL_INT||elem.tag==VAL_FLOAT||elem.tag==VAL_SYM) stack[sp++]=elem;
        else if(is_compound(elem.tag)){
            memcpy(&stack[sp],&body[eoff],esz*sizeof(Value)); sp+=esz;
            if(elem.tag==VAL_TUPLE){exec_env->refcount++;stack[sp-1].as.compound.env=exec_env;}
        } else if(elem.tag==VAL_XT){
            elem.as.xt.fn(exec_env);
        } else if(elem.tag==VAL_WORD){
            uint32_t sym=elem.as.sym;
            if(sym==S_DEF){
                Value dv_top=stack[sp-1]; int dv_s=val_slots(dv_top); uint32_t name; int rec=0;
                if(recur_pending){name=recur_sym;rec=1;recur_pending=0;frame_bind(exec_env,name,&stack[sp-dv_s],dv_s,BIND_DEF,rec);sp-=dv_s;}
                else{Value dv_buf[LOCAL_MAX];memcpy(dv_buf,&stack[sp-dv_s],dv_s*sizeof(Value));sp-=dv_s;name=pop_sym();frame_bind(exec_env,name,dv_buf,dv_s,BIND_DEF,rec);}
            } else if(sym==S_LET){
                uint32_t name=pop_sym(); Value lv_top=stack[sp-1]; int lv_s=val_slots(lv_top);
                frame_bind(exec_env,name,&stack[sp-lv_s],lv_s,BIND_LET,0); sp-=lv_s;
            } else if(sym==S_RECUR){recur_sym=pop_sym();recur_pending=1;}
            else dispatch_word(sym,exec_env);
        } else if(elem.tag==VAL_BOX) spush(elem);
    }
    if(exec_env->refcount==0){exec_env->bind_count=saved_bc;exec_env->vals_used=saved_vu;}
}

static int find_matching(Token *toks, int start, int count, TokTag open, TokTag close) {
    int depth=1;
    for(int i=start;i<count;i++){if(toks[i].tag==open)depth++;else if(toks[i].tag==close){depth--;if(depth==0)return i;}}
    die("unmatched bracket"); return -1;
}

static void build_tuple(Token *toks, int start, int end, int total_count, Frame *env) {
    int elem_base=sp,elem_count=0;
    for(int j=start;j<end;j++){
        Token *tt=&toks[j];
        switch(tt->tag){
        case TOK_INT: spush(val_int(tt->as.i)); elem_count++; break;
        case TOK_FLOAT: spush(val_float(tt->as.f)); elem_count++; break;
        case TOK_SYM: spush(val_sym(tt->as.sym)); elem_count++; break;
        case TOK_WORD:
            if(tt->as.sym==S_CHECK){if(elem_count>0&&(stack[sp-1].tag==VAL_WORD||stack[sp-1].tag==VAL_XT)){sp--;elem_count--;}else die("check: expected preceding type word");}
            else{PrimFn xt_fn=prim_lookup(tt->as.sym);if(xt_fn)spush(val_xt(tt->as.sym,xt_fn));else spush(val_word(tt->as.sym));elem_count++;}
            break;
        case TOK_STRING:
            for(int c=0;c<tt->as.str.len;c++) spush(val_int(tt->as.str.codes[c]));
            spush(val_compound(VAL_LIST,tt->as.str.len,tt->as.str.len+1)); elem_count++; break;
        case TOK_LPAREN:{int nc=find_matching(toks,j+1,total_count,TOK_LPAREN,TOK_RPAREN);build_tuple(toks,j+1,nc,total_count,env);elem_count++;j=nc;break;}
        case TOK_LBRACKET:{
            int bc=find_matching(toks,j+1,total_count,TOK_LBRACKET,TOK_RBRACKET);
            if(bc+1<total_count&&toks[bc+1].tag==TOK_WORD&&toks[bc+1].as.sym==S_EFFECT){j=bc+1;break;}
            int lb=sp; eval(toks+j+1,bc-j-1,env); int ls=sp-lb;
            int ec2=0,pos=sp; while(pos>lb){pos-=val_slots(stack[pos-1]);ec2++;}
            spush(val_compound(VAL_LIST,ec2,ls+1)); elem_count++; j=bc; break;
        }
        case TOK_LBRACE:{
            int bc=find_matching(toks,j+1,total_count,TOK_LBRACE,TOK_RBRACE);
            int lb=sp; eval(toks+j+1,bc-j-1,env); int total_slots=sp-lb;
            int nfields=0,is_record=1,pos=sp;
            while(pos>lb){Value v=stack[pos-1];int vs=val_slots(v);pos-=vs;if(is_record&&pos>lb&&stack[pos-1].tag==VAL_SYM){pos--;nfields++;}else is_record=0;}
            if(is_record&&nfields>0) spush(val_compound(VAL_RECORD,nfields,total_slots+1));
            else{int ec2=0;pos=sp;while(pos>lb){pos-=val_slots(stack[pos-1]);ec2++;}spush(val_compound(VAL_TUPLE,ec2,total_slots+1));}
            elem_count++; j=bc; break;
        }
        default: break;
        }
    }
    int total_elem_slots=sp-elem_base;
    spush(val_compound(VAL_TUPLE,elem_count,total_elem_slots+1));
    if(env) env->refcount++;
    stack[sp-1].as.compound.env=env;
}

static void eval(Token *toks, int count, Frame *env) {
    int base=sp; build_tuple(toks,0,count,count,env);
    int s=val_slots(stack[sp-1]); Value *body=malloc(s*sizeof(Value));
    memcpy(body,&stack[base],s*sizeof(Value)); sp=base; eval_body(body,s,env); free(body);
}

static void prim_where(Frame *env) {
    POP_BODY(fn,"where"); POP_LIST_BUF(list,"where");
    int rb=sp,rc=0;
    for(int i=0;i<list_len;i++){
        ElemRef r=compound_elem(list_buf,list_s,list_len,i);
        memcpy(&stack[sp],&list_buf[r.base],r.slots*sizeof(Value));sp+=r.slots;
        eval_body(fn_buf,fn_s,env); if(pop_int()){spush(val_int(i));rc++;}
    }
    spush(val_compound(VAL_LIST,rc,sp-rb+1));
}

static void prim_find_elem(Frame *env) {
    POP_BODY(fn,"find"); POP_LIST_BUF(list,"find");
    for(int i=0;i<list_len;i++){
        ElemRef r=compound_elem(list_buf,list_s,list_len,i);
        memcpy(&stack[sp],&list_buf[r.base],r.slots*sizeof(Value));sp+=r.slots;
        memcpy(&stack[sp],&list_buf[r.base],r.slots*sizeof(Value));sp+=r.slots;
        eval_body(fn_buf,fn_s,env); if(pop_int()) return; sp-=r.slots;
    }
    spush(val_int(-1));
}
#ifndef SLAP_SDL
static void prim_millis(Frame *e){(void)e;struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);spush(val_int((int64_t)(ts.tv_sec*1000+ts.tv_nsec/1000000)));}
#endif

static const char *BUILTIN_TYPES =
    "'dup ['a copy in  'a copy out  'a copy out] effect\n"
    "'drop ['a copy in] effect\n"
    "'swap ['a own in  'b own in  'b own out  'a own out] effect\n"
#define A2E " ['a num lent in  'a num lent in  'a num move out] effect\n"
    "'plus" A2E "'sub" A2E "'mul" A2E "'div" A2E
#undef A2E
#define I2E " [int lent in  int lent in  int move out] effect\n"
    "'mod" I2E "'wrap" I2E
#undef I2E
    "'divmod [int lent in  int lent in  int move out  int move out] effect\n"
#define C2E " [lent in  lent in  int move out] effect\n"
    "'eq" C2E "'lt" C2E
#undef C2E
#define B2E " [int lent in  int lent in  int move out] effect\n"
    "'and" B2E "'or" B2E
#undef B2E
    "'print [own in] effect\n"
    "'assert [int own in] effect\n"
    "'millis [int move out] effect\n"
    "'itof [int lent in  float move out] effect\n"
    "'ftoi [float lent in  int move out] effect\n"
#define F1E " [float lent in  float move out] effect\n"
    "'fsqrt" F1E "'fsin" F1E "'fcos" F1E "'ftan" F1E "'ffloor" F1E "'fceil" F1E "'fround" F1E "'fexp" F1E "'flog" F1E
#undef F1E
    "'fpow [float lent in  float lent in  float move out] effect\n"
    "'fatan2 [float lent in  float lent in  float move out] effect\n"
    "'list [list move out] effect\n"
    "'len [list lent in  int move out] effect\n"
    "'give ['a list own in  'a own in  'a list move out] effect\n"
    "'grab ['a list own in  'a list move out  'a move out] effect\n"
    "'get ['a list own in  int lent in  'a move out] effect\n"
    "'nth [sym lent in  int lent in  'a move out] effect\n"
    "'set ['a list own in  int lent in  'a own in  'a list move out] effect\n"
    "'cat ['a list own in  'a list own in  'a list move out] effect\n"
#define LSE " ['a list own in  int lent in  'a list move out] effect\n"
    "'take-n" LSE "'drop-n" LSE
#undef LSE
    "'range [int lent in  int lent in  int list move out] effect\n"
#define L1E " ['a list own in  'a list move out] effect\n"
    "'sort" L1E "'reverse" L1E "'dedup" L1E
#undef L1E
    "'index-of ['a list own in  'a lent in  int move out] effect\n"
#define LIE " ['a list own in  int list own in  'a list move out] effect\n"
    "'select" LIE "'pick" LIE "'keep-mask" LIE
#undef LIE
#define LGE " ['a list own in  int list move out] effect\n"
    "'rise" LGE "'fall" LGE "'shape" LGE "'classify" LGE
#undef LGE
    "'stack [tuple move out] effect\n"
    "'size [tuple own in  int move out] effect\n"
    "'push [tuple own in  own in  tuple move out] effect\n"
    "'pop [tuple own in  tuple move out  move out] effect\n"
    "'pull [tuple lent in  int lent in  tuple move out  move out] effect\n"
    "'put [tuple own in  int lent in  own in  tuple move out] effect\n"
    "'compose [tuple own in  tuple own in  tuple move out] effect\n"
    "'rec [rec move out] effect\n"
    "'random [int lent in  int move out] effect\n"
    "'halt [] effect\n"
    "'box ['a own in  'a box move out] effect\n"
    "'free ['a box own in] effect\n"
    "'at [rec own in  sym lent in  move out] effect\n"
    "'into [rec own in  own in  sym lent in  rec move out] effect\n"
    "'clone ['a box own in  'a box move out  'a box move out] effect\n"
    "'clear [int lent in] effect\n"
    "'pixel [int lent in  int lent in  int lent in] effect\n"
    "'fill-rect [int lent in  int lent in  int lent in  int lent in  int lent in] effect\n"
#define LLE " ['a list own in  int lent in  'a list move out] effect\n"
    "'rotate" LLE "'windows" LLE
#undef LLE
    "'zip ['a list own in  'a list own in  list move out] effect\n"
#define LDE " ['a list own in  int list own in  list move out] effect\n"
    "'group" LDE "'partition" LDE "'reshape" LDE
#undef LDE
    "'transpose [list own in  list move out] effect\n";

static const char *PRELUDE =
    "'over (swap dup (swap) dip) def\n'peek (over) def\n'nip (swap drop) def\n"
    "'rot ((swap) dip swap) def\n"
    "'not (0 eq) [int lent in  int move out] effect def\n"
    "'neq (eq not) [lent in  lent in  int move out] effect def\n"
    "'gt (swap lt) [lent in  lent in  int move out] effect def\n"
    "'ge (lt not) [lent in  lent in  int move out] effect def\n"
    "'le (swap lt not) [lent in  lent in  int move out] effect def\n"
    "'inc (1 plus) [num lent in  num move out] effect def\n"
    "'dec (1 sub) [num lent in  num move out] effect def\n"
    "'neg (0 swap sub) [num lent in  num move out] effect def\n"
    "'max (over over lt (nip) (drop) if) [num lent in  num lent in  num move out] effect def\n"
    "'min (over over lt (drop) (nip) if) [num lent in  num lent in  num move out] effect def\n"
    "'abs (dup neg max) [num lent in  num move out] effect def\n"
    "'bi ('g swap def 'f swap def dup f swap g) def\n"
    "'keep ('f swap def dup f swap) def\n"
    "'repeat ('f swap def (dup 0 gt) (1 sub (f) dip) while drop) def\n"
    "'select (swap 'data swap def (data swap get) map) def\n"
    "'pick (swap 'data swap def (data swap get) map) def\n"
    "'reduce (swap dup 0 get swap 1 drop-n swap rot fold) def\n"
    "'table ((dup) swap compose (couple) compose map) def\n"
    "'sqr (dup mul) def\n'cube (dup dup mul mul) def\n"
    "'ispos (0 swap lt) def\n'isneg (0 lt) def\n'first (0 get) def\n"
    "'last (dup len 1 sub get) def\n'sum (0 (plus) fold) def\n'product (1 (mul) fold) def\n"
    "'max-of (dup first (max) fold) def\n'min-of (dup first (min) fold) def\n"
    "'member (index-of -1 neq) def\n'couple (list rot give swap give) def\n"
    "'isany (0 (or) fold) def\n'isall (1 (and) fold) def\n'count (len) def\n"
    "'flatten (list (cat) fold) def\n'sort-desc (sort reverse) def\n'fneg (0.0 swap sub) def\n"
    "'fabs (dup 0.0 lt (fneg) () if) def\n"
    "'frecip (1.0 swap div) def\n"
    "'fsign (dup 0.0 lt (drop -1.0) (dup 0.0 eq (drop 0.0) (drop 1.0) if) if) def\n"
    "'sign (dup 0 lt (drop -1) (dup 0 eq (drop 0) (drop 1) if) if) def\n"
    "'clamp (rot swap min max) def\n'fclamp (swap min max) def\n"
    "'lerp ((over sub) dip swap mul plus) def\n"
    "'isbetween (rot dup (rot swap le) dip rot rot ge and) def\n"
    "'iszero (0 eq) [int lent in  int move out] effect def\n"
    "'iseven (2 mod 0 eq) [int lent in  int move out] effect def\n"
    "'isodd (2 mod 0 neq) [int lent in  int move out] effect def\n"
    "'divides (mod 0 eq) [int lent in  int lent in  int move out] effect def\n"
    "'times-i ('f swap def 'n let 0 (dup n lt) (dup (f) dip 1 plus) while drop) def\n"
            "3.14159265358979323846 'pi let\n6.28318530717958647692 'tau let\n2.71828182845904523536 'e let\n"
    "'rotate ('n let dup len 'ln let ln 0 eq not (n ln wrap 'nn let nn 0 eq not (dup ln nn sub take-n swap ln nn sub drop-n swap cat) () if) () if) def\n"
    "'zip ('b swap def 'a swap def a len b len min 'n let 0 n range (dup a swap get swap b swap get couple) map) def\n"
    "'windows ('n let 'l swap def l len 'll let n 0 le ll n lt or (list) (0 ll n sub 1 plus range ('i let l i drop-n n take-n) map) if) def\n"
    "'reshape ('dims swap def 'data swap def dims 0 get 'rows let dims 1 get 'cols let 0 rows range ('r let 0 cols range ('c let data r cols mul c plus get) map) map) def\n"
    "'transpose ('m swap def m 0 get len 'cols let m len 'rows let 0 cols range ('c let 0 rows range ('r let m r get c get) map) map) def\n"
    "'keep-mask ('mask swap def 0 mask len range (mask swap get 0 neq) where select) def\n"
    "'group ('idx swap def 'data swap def 0 idx (max) fold 1 plus 'ng let 0 ng range ('g let 0 idx len range (idx swap get g eq) where data swap select) map) def\n"
    "'partition (group) def\n"
    "'classify (dup 'l swap def (l swap index-of) map dup dedup 'u swap def (u swap index-of) map) def\n";

static void prim_reverse(Frame *e) {
    (void)e; Value top=speek(); if(top.tag!=VAL_LIST) die("reverse: expected list");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    if(s>LOCAL_MAX) die("reverse: list too large");
    Value buf[LOCAL_MAX]; memcpy(buf,&stack[base],s*sizeof(Value)); sp=base;
    if(len>LOCAL_MAX) die("reverse: list too large");
    int offsets[LOCAL_MAX],sizes[LOCAL_MAX]; compute_offsets(buf,s,len,offsets,sizes);
    int rb=sp;
    for(int i=len-1;i>=0;i--){memcpy(&stack[sp],&buf[offsets[i]],sizes[i]*sizeof(Value));sp+=sizes[i];}
    spush(val_compound(VAL_LIST,len,sp-rb+1));
}

static void prim_dedup(Frame *e) {
    (void)e; Value top=speek(); if(top.tag!=VAL_LIST) die("dedup: expected list");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    Value buf[LOCAL_MAX]; memcpy(buf,&stack[base],s*sizeof(Value)); sp=base;
    if(len>LOCAL_MAX) die("dedup: list too large");
    int offsets[LOCAL_MAX],sizes2[LOCAL_MAX]; compute_offsets(buf,s,len,offsets,sizes2);
    int rb=sp,rc=0;
    for(int i=0;i<len;i++){
        int dup2=0; for(int j=0;j<i;j++) if(val_equal(&buf[offsets[i]],sizes2[i],&buf[offsets[j]],sizes2[j])){dup2=1;break;}
        if(!dup2){memcpy(&stack[sp],&buf[offsets[i]],sizes2[i]*sizeof(Value));sp+=sizes2[i];rc++;}
    }
    spush(val_compound(VAL_LIST,rc,sp-rb+1));
}

/* ---- SDL ---- */

#ifdef SLAP_SDL
#include <SDL.h>
#define CANVAS_W 640
#define CANVAS_H 480
static uint8_t canvas[CANVAS_W*CANVAS_H];
static SDL_Window *sdl_window=NULL; static SDL_Renderer *sdl_renderer=NULL; static SDL_Texture *sdl_texture=NULL;
static int sdl_test_mode=0;
#define MAX_HANDLERS 16
static struct{uint32_t event_sym;Value handler_body[LOCAL_MAX];int handler_slots;} event_handlers[MAX_HANDLERS];
static int handler_count=0;
static Value render_body[LOCAL_MAX]; static int render_slots=0;
static uint8_t gray_lut[4]={0,85,170,255};
static void sdl_init(void) {
    if(sdl_window) return;
    if(SDL_Init(SDL_INIT_VIDEO)<0) die("SDL_Init: %s",SDL_GetError());
    sdl_window=SDL_CreateWindow("slap",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,CANVAS_W,CANVAS_H,0);
    if(!sdl_window) die("SDL_CreateWindow: %s",SDL_GetError());
    sdl_renderer=SDL_CreateRenderer(sdl_window,-1,SDL_RENDERER_ACCELERATED);
    if(!sdl_renderer) die("SDL_CreateRenderer: %s",SDL_GetError());
    sdl_texture=SDL_CreateTexture(sdl_renderer,SDL_PIXELFORMAT_RGB24,SDL_TEXTUREACCESS_STREAMING,CANVAS_W,CANVAS_H);
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
static void prim_millis(Frame *e){(void)e;spush(val_int((int64_t)SDL_GetTicks()));}
static void prim_on(Frame *e) {
    (void)e; Value fn_top=stack[sp-1]; if(fn_top.tag!=VAL_TUPLE) die("on: expected tuple handler");
    int fn_s=val_slots(fn_top); if(handler_count>=MAX_HANDLERS) die("on: too many event handlers");
    memcpy(event_handlers[handler_count].handler_body,&stack[sp-fn_s],fn_s*sizeof(Value));
    event_handlers[handler_count].handler_slots=fn_s; sp-=fn_s;
    event_handlers[handler_count].event_sym=pop_sym(); handler_count++;
}
static uint32_t sym_tick=0,sym_keydown=0,sym_mousedown=0,sym_mouseup=0,sym_mousemove=0;
static void show_intern_syms(void) {
    if(!sym_tick){sym_tick=sym_intern("tick");sym_keydown=sym_intern("keydown");sym_mousedown=sym_intern("mousedown");sym_mouseup=sym_intern("mouseup");sym_mousemove=sym_intern("mousemove");}
}
static void show_dispatch_event(SDL_Event *ev, Frame *env) {
    if(ev->type==SDL_KEYDOWN) for(int h=0;h<handler_count;h++)
        if(event_handlers[h].event_sym==sym_keydown){spush(val_int((int64_t)ev->key.keysym.sym));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}
    if(ev->type==SDL_MOUSEBUTTONDOWN) for(int h=0;h<handler_count;h++)
        if(event_handlers[h].event_sym==sym_mousedown){spush(val_int((int64_t)ev->button.x));spush(val_int((int64_t)ev->button.y));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}
    if(ev->type==SDL_MOUSEBUTTONUP) for(int h=0;h<handler_count;h++)
        if(event_handlers[h].event_sym==sym_mouseup){spush(val_int((int64_t)ev->button.x));spush(val_int((int64_t)ev->button.y));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}
    if(ev->type==SDL_MOUSEMOTION) for(int h=0;h<handler_count;h++)
        if(event_handlers[h].event_sym==sym_mousemove){spush(val_int((int64_t)ev->motion.x));spush(val_int((int64_t)ev->motion.y));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}
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
    for(int h=0;h<handler_count;h++) if(event_handlers[h].event_sym==sym_tick){spush(val_int(show_frame));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,show_env);}
    if(render_slots>0){Value mt=stack[sp-1];int ms=val_slots(mt);memcpy(&stack[sp],&stack[sp-ms],ms*sizeof(Value));sp+=ms;eval_body(render_body,render_slots,show_env);}
    sdl_present(); show_frame++;
}
#endif
static void prim_show(Frame *env) {
    Value fn_top=stack[sp-1]; if(fn_top.tag!=VAL_TUPLE) die("show: expected tuple render function");
    render_slots=val_slots(fn_top); memcpy(render_body,&stack[sp-render_slots],render_slots*sizeof(Value)); sp-=render_slots;
    sdl_init(); show_intern_syms();
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
        for(int h=0;h<handler_count;h++) if(event_handlers[h].event_sym==sym_tick){spush(val_int(frame));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}
        if(render_slots>0){Value mt=stack[sp-1];int ms=val_slots(mt);memcpy(&stack[sp],&stack[sp-ms],ms*sizeof(Value));sp+=ms;eval_body(render_body,render_slots,env);}
        sdl_present(); frame++;
        if(sdl_test_mode) break; SDL_Delay(16);
    }
    SDL_DestroyTexture(sdl_texture);SDL_DestroyRenderer(sdl_renderer);SDL_DestroyWindow(sdl_window);SDL_Quit();exit(0);
#endif
}
#endif

static void register_prims(void) {
    static struct{const char*n;PrimFn f;} t[]={
        {"dup",prim_dup},{"drop",prim_drop},{"swap",prim_swap},{"dip",prim_dip},{"apply",prim_apply},
        {"plus",prim_plus},{"sub",prim_sub},{"mul",prim_mul},{"div",prim_div},{"mod",prim_mod},{"divmod",prim_divmod},{"wrap",prim_wrap},
        {"eq",prim_eq},{"lt",prim_lt},{"and",prim_and},{"or",prim_or},
        {"print",prim_print},{"assert",prim_assert},{"halt",prim_halt},{"random",prim_random},
        {"if",prim_if},{"cond",prim_cond},{"match",prim_match},{"loop",prim_loop},{"while",prim_while},
        {"itof",prim_itof},{"ftoi",prim_ftoi},{"fsqrt",prim_fsqrt},{"fsin",prim_fsin},{"fcos",prim_fcos},
        {"ftan",prim_ftan},{"ffloor",prim_ffloor},{"fceil",prim_fceil},{"fround",prim_fround},
        {"fexp",prim_fexp},{"flog",prim_flog},{"fpow",prim_fpow},{"fatan2",prim_fatan2},
        {"stack",prim_stack},{"size",prim_size},{"push",prim_push_op},{"pop",prim_pop_op},
        {"pull",prim_pull},{"put",prim_replace_at},{"compose",prim_concat},
        {"list",prim_list},{"len",prim_size},{"give",prim_push_op},{"grab",prim_grab},
        {"get",prim_get},{"nth",prim_nth},{"set",prim_replace_at},{"cat",prim_concat},
        {"take-n",prim_take_n},{"drop-n",prim_drop_n},{"range",prim_range},
        {"map",prim_map},{"filter",prim_filter},{"fold",prim_fold},{"each",prim_each},
        {"sort",prim_sort},{"index-of",prim_index_of},{"scan",prim_scan},
        {"at",prim_at},{"rise",prim_rise},{"fall",prim_fall},
        {"shape",prim_shape},
        {"rec",prim_rec},{"into",prim_into},{"reverse",prim_reverse},{"dedup",prim_dedup},
        {"where",prim_where},{"find",prim_find_elem},
        {"millis",prim_millis},{"box",prim_box},{"free",prim_free},
        {"lend",prim_lend},{"mutate",prim_mutate},{"clone",prim_clone},
#ifdef SLAP_SDL
        {"clear",prim_clear},{"pixel",prim_pixel},{"fill-rect",prim_fill_rect},{"millis",prim_millis},{"on",prim_on},{"show",prim_show},
#endif
        {NULL,NULL}
    };
    for(int i=0;t[i].n;i++) prim_register(t[i].n,t[i].f);
}

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));
    int check_only=0,dump_types=0; const char *filename=NULL;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--check")==0) check_only=1;
        else if(strcmp(argv[i],"--dump-types")==0) dump_types=1;
#ifdef SLAP_SDL
        else if(strcmp(argv[i],"--test")==0) sdl_test_mode=1;
#endif
        else filename=argv[i];
    }
#ifdef SLAP_WASM
    if(!filename) filename="program.slap";
#endif
    if(!filename){fprintf(stderr,"usage: slap [--check] <file.slap>\n");return 1;}
    current_file=filename; syms_init(); register_prims();
    Frame *global=frame_new(NULL);
    current_file="<prelude>"; lex(PRELUDE); eval(tokens,tok_count,global);
    current_file=filename;
    FILE *f=fopen(filename,"r"); if(!f){fprintf(stderr,"error: cannot open '%s'\n",filename);return 1;}
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *src=malloc(sz+1); if((long)fread(src,1,sz,f)!=sz){fprintf(stderr,"error: read failed\n");return 1;} src[sz]=0; fclose(f);
    store_source_lines(src);
    lex(src); int user_tok_count=tok_count;
    static Token user_tokens[TOK_MAX]; memcpy(user_tokens,tokens,user_tok_count*sizeof(Token));
    if(dump_types){
        eval(user_tokens,user_tok_count,global);
        for(int i=0;i<type_sig_count;i++){
            TypeSig *s=&type_sigs[i].sig; printf("'%s type",sym_name(type_sigs[i].sym));
            for(int j=0;j<s->slot_count;j++){
                TypeSlot *sl=&s->slots[j];
                printf("  "); if(sl->type_var) printf("'%s ",sym_name(sl->type_var));
                if(sl->constraint!=TC_NONE) printf("%s ",constraint_name(sl->constraint));
                switch(sl->ownership){case OWN_OWN:printf("own ");break;case OWN_COPY:printf("copy ");break;case OWN_MOVE:printf("move ");break;case OWN_LENT:printf("lent ");break;}
                printf("%s",sl->direction==DIR_IN?"in":"out");
            }
            printf(" def\n");
        }
        free(src); frame_free(global); return 0;
    }
    static Token combined[TOK_MAX]; int cpos=0;
    lex(BUILTIN_TYPES); memcpy(combined,tokens,tok_count*sizeof(Token)); cpos=tok_count;
    lex(PRELUDE); memcpy(&combined[cpos],tokens,tok_count*sizeof(Token)); cpos+=tok_count;
    int user_start=cpos;
    memcpy(&combined[cpos],user_tokens,user_tok_count*sizeof(Token)); cpos+=user_tok_count;
    int errors=typecheck_tokens(combined,cpos,user_start);
    if(errors>0){fprintf(stderr,"%d type error(s)\n",errors);free(src);frame_free(global);return 1;}
    if(check_only){fprintf(stderr,"type check passed\n");free(src);frame_free(global);return 0;}
    eval(user_tokens,user_tok_count,global);
    free(src); frame_free(global); return 0;
}
