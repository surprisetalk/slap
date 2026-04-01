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
#define MARK_MAX  256
#define PRIM_MAX  256
#define LOCAL_MAX 4096

typedef enum { VAL_INT, VAL_FLOAT, VAL_SYM, VAL_WORD, VAL_TUPLE, VAL_LIST, VAL_RECORD, VAL_BOX } ValTag;
typedef struct Frame Frame;
typedef struct Value {
    ValTag tag;
    union {
        int64_t i; double f; uint32_t sym;
        struct { uint32_t len; uint32_t slots; Frame *env; } compound;
        void *box;
    } as;
} Value;

__attribute__((noreturn)) static void die(const char *fmt, ...);

static int val_slots(Value v) {
    switch (v.tag) {
    case VAL_TUPLE: case VAL_LIST: case VAL_RECORD: {
        int s = (int)v.as.compound.slots;
        if (s < 1) die("corrupt value: compound with %d slots", s);
        return s;
    }
    default: return 1;
    }
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

__attribute__((noreturn))
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "\n-- ERROR %s:%d ", current_file, current_line);
    int hdr_len = 10 + (int)strlen(current_file) + 10;
    for (int i = hdr_len; i < 60; i++) fprintf(stderr, "-");
    fprintf(stderr, "\n\n    ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
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

#define FRAME_POOL_SIZE 32
static Frame frame_pool[FRAME_POOL_SIZE];
static int frame_pool_used[FRAME_POOL_SIZE];
static int frame_pool_init = 0;

static Frame *frame_new(Frame *parent) {
    if (!frame_pool_init) { frame_pool_init = 1; memset(frame_pool_used, 0, sizeof(frame_pool_used)); }
    for (int i = 0; i < FRAME_POOL_SIZE; i++) {
        if (!frame_pool_used[i]) {
            frame_pool_used[i] = 1; Frame *f = &frame_pool[i];
            f->parent = parent; f->bind_count = 0; f->vals_used = 0; f->refcount = 0; return f;
        }
    }
    Frame *f = malloc(sizeof(Frame));
    f->parent = parent; f->bind_count = 0; f->vals_used = 0; f->refcount = 0; return f;
}

static void frame_free(Frame *f) {
    if (f >= &frame_pool[0] && f < &frame_pool[FRAME_POOL_SIZE]) frame_pool_used[(int)(f - &frame_pool[0])] = 0;
    else free(f);
}

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

typedef void (*PrimFn)(Frame *env);
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
    int elem_end = total_slots - 1, off = 0, sz = 0;
    for (int i = len - 1; i >= 0; i--) {
        int lp = elem_end - 1; Value last = data[lp];
        int esize = (last.tag == VAL_TUPLE || last.tag == VAL_LIST || last.tag == VAL_RECORD) ? (int)last.as.compound.slots : 1;
        if (i == index) { off = elem_end - esize; sz = esize; }
        elem_end -= esize;
    }
    ElemRef ref = { off, sz }; return ref;
}

static ElemRef record_field(Value *data, int total_slots, int len, uint32_t key, int *found) {
    int elem_end = total_slots - 1; *found = 0; ElemRef ref = {0, 0};
    for (int i = len - 1; i >= 0; i--) {
        int lp = elem_end - 1; Value last = data[lp];
        int vsize = (last.tag == VAL_TUPLE || last.tag == VAL_LIST || last.tag == VAL_RECORD) ? (int)last.as.compound.slots : 1;
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
        int sz = (l.tag == VAL_TUPLE || l.tag == VAL_LIST || l.tag == VAL_RECORD) ? (int)l.as.compound.slots : 1;
        offsets[i] = elem_end - sz; sizes[i] = sz; elem_end = offsets[i];
    }
}

static void val_print(Value *data, int slots, FILE *out) {
    Value top = data[slots - 1];
    switch (top.tag) {
    case VAL_INT: fprintf(out, "%lld", (long long)top.as.i); break;
    case VAL_FLOAT: fprintf(out, "%g", top.as.f); break;
    case VAL_SYM: fprintf(out, "'%s", sym_name(top.as.sym)); break;
    case VAL_WORD: fprintf(out, "%s", sym_name(top.as.sym)); break;
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
            fprintf(out, "\"");
        } else {
            fprintf(out, "[");
            if (len > LOCAL_MAX) die("list too large to print (%d elements)", len);
            int offsets[LOCAL_MAX], sizes[LOCAL_MAX];
            compute_offsets(data, slots, len, offsets, sizes);
            for (int i = 0; i < len; i++) { if (i > 0) fprintf(out, " "); val_print(&data[offsets[i]], sizes[i], out); }
            fprintf(out, "]");
        }
        break;
    }
    case VAL_TUPLE: {
        int len = (int)top.as.compound.len;
        fprintf(out, "(");
        if (len > LOCAL_MAX) die("tuple too large to print (%d elements)", len);
        int offsets[LOCAL_MAX], sizes[LOCAL_MAX];
        compute_offsets(data, slots, len, offsets, sizes);
        for (int i = 0; i < len; i++) { if (i > 0) fprintf(out, " "); val_print(&data[offsets[i]], sizes[i], out); }
        fprintf(out, ")");
        break;
    }
    case VAL_RECORD: {
        int len = (int)top.as.compound.len;
        fprintf(out, "{");
        if (len > LOCAL_MAX) die("record too large to print (%d fields)", len);
        int elem_end = slots - 1, kpos[LOCAL_MAX], voff[LOCAL_MAX], vsz[LOCAL_MAX];
        for (int i = len - 1; i >= 0; i--) {
            int lp = elem_end - 1; Value l = data[lp];
            int sz = (l.tag == VAL_TUPLE || l.tag == VAL_LIST || l.tag == VAL_RECORD) ? (int)l.as.compound.slots : 1;
            voff[i] = elem_end - sz; vsz[i] = sz; kpos[i] = voff[i] - 1; elem_end = kpos[i];
        }
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
    case VAL_SYM: case VAL_WORD: return atop.as.sym == btop.as.sym;
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
static uint32_t sym_effect_kw = 0, sym_check_kw = 0;

/* ---- type system ---- */
typedef enum { DIR_IN, DIR_OUT } SlotDir;
typedef enum { OWN_OWN, OWN_COPY, OWN_MOVE, OWN_LENT } OwnMode;
typedef enum { TC_NONE=0, TC_INT, TC_FLOAT, TC_SYM, TC_NUM, TC_LIST, TC_TUPLE, TC_REC, TC_BOX, TC_STACK } TypeConstraint;

typedef struct { const char *name; uint32_t sym; int need; int out; TypeConstraint out_type; } HOEffect;
#define HO_OP_COUNT 26
static HOEffect ho_ops[HO_OP_COUNT] = {
    {"apply",0,1,0,TC_NONE},{"dip",0,2,1,TC_NONE},{"if",0,3,1,TC_NONE},
    {"map",0,2,1,TC_LIST},{"filter",0,2,1,TC_LIST},{"fold",0,3,1,TC_NONE},
    {"reduce",0,2,1,TC_NONE},{"each",0,2,0,TC_NONE},{"while",0,2,0,TC_NONE},
    {"loop",0,1,0,TC_NONE},{"lend",0,2,2,TC_BOX},{"mutate",0,2,1,TC_BOX},
    {"clone",0,1,2,TC_BOX},{"cond",0,3,1,TC_NONE},{"match",0,3,1,TC_NONE},
    {"where",0,2,1,TC_LIST},{"find",0,2,1,TC_NONE},{"table",0,2,1,TC_LIST},
    {"scan",0,3,1,TC_LIST},{"at",0,3,1,TC_NONE},{"into",0,3,1,TC_REC},
    {"repeat",0,2,0,TC_NONE},{"bi",0,3,2,TC_NONE},{"keep",0,1,1,TC_NONE},
    {"on",0,1,0,TC_NONE},{"show",0,1,0,TC_NONE},
};
static int ho_ops_init = 0;
static void ho_ops_ensure_init(void) {
    if (ho_ops_init) return; ho_ops_init = 1;
    for (int i = 0; i < HO_OP_COUNT; i++) ho_ops[i].sym = sym_intern(ho_ops[i].name);
}
static HOEffect *ho_ops_find(uint32_t sym) {
    ho_ops_ensure_init();
    for (int i = 0; i < HO_OP_COUNT; i++) if (ho_ops[i].sym == sym) return &ho_ops[i];
    return NULL;
}

typedef struct {
    uint32_t type_var, type_var2; TypeConstraint constraint; OwnMode ownership; SlotDir direction;
    int is_env; uint32_t env_key_var, env_val_var;
} TypeSlot;
#define TYPE_SLOTS_MAX 16
typedef struct { TypeSlot slots[TYPE_SLOTS_MAX]; int slot_count; int is_todo; } TypeSig;

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

static TypeSig parse_type_annotation(Token *toks, int start, int end) {
    TypeSig sig; memset(&sig, 0, sizeof(sig));
    for (int i = start; i < end; i++)
        if (toks[i].tag == TOK_WORD && strcmp(sym_name(toks[i].as.sym), "todo") == 0) { sig.is_todo = 1; return sig; }
    int i = start;
    while (i < end) {
        if (sig.slot_count >= TYPE_SLOTS_MAX) die("too many type slots");
        TypeSlot *slot = &sig.slots[sig.slot_count]; memset(slot, 0, sizeof(*slot));
        int slot_start = i;
        while (i < end) {
            if (toks[i].tag != TOK_WORD && toks[i].tag != TOK_SYM) break;
            const char *w = sym_name(toks[i].as.sym);
            if (strcmp(w,"in")==0 || strcmp(w,"out")==0 || strcmp(w,"env")==0) break;
            i++;
        }
        if (i >= end) break;
        const char *dir_word = sym_name(toks[i].as.sym);
        if (strcmp(dir_word, "env") == 0) {
            slot->is_env = 1;
            if (i - slot_start >= 2) { slot->env_key_var = toks[slot_start].as.sym; slot->env_val_var = toks[slot_start+1].as.sym; }
            sig.slot_count++; i++; continue;
        }
        slot->direction = (strcmp(dir_word, "in") == 0) ? DIR_IN : DIR_OUT; i++;
        for (int j = slot_start; j < i - 1; j++) {
            const char *tw = sym_name(toks[j].as.sym);
            if (strcmp(tw,"own")==0) { slot->ownership=OWN_OWN; continue; }
            if (strcmp(tw,"copy")==0) { slot->ownership=OWN_COPY; continue; }
            if (strcmp(tw,"move")==0) { slot->ownership=OWN_MOVE; continue; }
            if (strcmp(tw,"lent")==0) { slot->ownership=OWN_LENT; continue; }
            if (strcmp(tw,"int")==0) { slot->constraint=TC_INT; continue; }
            if (strcmp(tw,"float")==0) { slot->constraint=TC_FLOAT; continue; }
            if (strcmp(tw,"sym")==0) { slot->constraint=TC_SYM; continue; }
            if (strcmp(tw,"num")==0) { slot->constraint=TC_NUM; continue; }
            if (strcmp(tw,"list")==0) { slot->constraint=TC_LIST; continue; }
            if (strcmp(tw,"tuple")==0) { slot->constraint=TC_TUPLE; continue; }
            if (strcmp(tw,"rec")==0) { slot->constraint=TC_REC; continue; }
            if (strcmp(tw,"box")==0) { slot->constraint=TC_BOX; continue; }
            if (strcmp(tw,"stack")==0) { slot->constraint=TC_STACK; continue; }
            if (toks[j].tag == TOK_SYM) { if (!slot->type_var) slot->type_var = toks[j].as.sym; else slot->type_var2 = toks[j].as.sym; continue; }
            if (toks[j].tag == TOK_WORD) { if (!slot->type_var) slot->type_var = toks[j].as.sym; else slot->type_var2 = toks[j].as.sym; }
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

#define TVAR_MAX 4096
typedef struct { int parent; TypeConstraint bound; int elem; int box_c; } TVarEntry;

typedef struct {
    TypeConstraint type; int tvar_id; uint32_t sym_id; OwnMode ownership;
    int is_linear, consumed, borrowed, source_line;
    int has_effect, effect_consumed, effect_produced;
    int elem_tvar, box_tvar;
    int scheme_in[8], scheme_out[8], scheme_in_count, scheme_out_count, scheme_tvar_base, scheme_tvar_count;
    TypeConstraint effect_out_type, elem_type, box_contents;
} AbstractType;

#define ASTACK_MAX 256
#define TC_BINDS_MAX 256
typedef struct { uint32_t sym; AbstractType atype; int is_def; } TCBinding;
typedef struct { uint32_t sym; int line; } TCUnknown;
#define TC_UNKNOWN_MAX 256

typedef struct {
    AbstractType data[ASTACK_MAX]; int sp, errors;
    TCBinding bindings[TC_BINDS_MAX]; int bind_count;
    int recur_pending; uint32_t recur_sym;
    TCUnknown unknowns[TC_UNKNOWN_MAX]; int unknown_count;
    TVarEntry tvars[TVAR_MAX]; int tvar_count;
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
static TypeConstraint at_type(TypeChecker *tc, AbstractType *at) {
    if (at->type != TC_NONE) return at->type;
    if (at->tvar_id > 0) return tvar_resolve(tc, at->tvar_id);
    return TC_NONE;
}
static int tvar_bind(TypeChecker *tc, int id, TypeConstraint c, int line) {
    int root = tvar_find(tc, id); TypeConstraint cur = tc->tvars[root].bound;
    if (cur == TC_NONE) { tc->tvars[root].bound = c; return 0; }
    if (cur == c) return 0;
    if (cur == TC_NUM && (c == TC_INT || c == TC_FLOAT)) { tc->tvars[root].bound = c; return 0; }
    if (c == TC_NUM && (cur == TC_INT || cur == TC_FLOAT)) return 0;
    (void)line; return 1;
}
static int tvar_unify(TypeChecker *tc, int a, int b, int line) {
    int ra = tvar_find(tc, a), rb = tvar_find(tc, b);
    if (ra == rb) return 0;
    TypeConstraint ca = tc->tvars[ra].bound, cb = tc->tvars[rb].bound;
    if (ca != TC_NONE && cb != TC_NONE && ca != cb) {
        if (ca == TC_NUM && (cb == TC_INT || cb == TC_FLOAT)) { }
        else if (cb == TC_NUM && (ca == TC_INT || ca == TC_FLOAT)) { }
        else { (void)line; return 1; }
    }
    tc->tvars[rb].parent = ra;
    if (tc->tvars[ra].elem == 0 && tc->tvars[rb].elem != 0) tc->tvars[ra].elem = tc->tvars[rb].elem;
    if (tc->tvars[ra].box_c == 0 && tc->tvars[rb].box_c != 0) tc->tvars[ra].box_c = tc->tvars[rb].box_c;
    if (ca == TC_NONE) tc->tvars[ra].bound = cb;
    else if (ca == TC_NUM && (cb == TC_INT || cb == TC_FLOAT)) tc->tvars[ra].bound = cb;
    return 0;
}
static int tvar_unify_at(TypeChecker *tc, int tvar, AbstractType *at, int line) {
    if (at->tvar_id > 0) return tvar_unify(tc, tvar, at->tvar_id, line);
    if (at->type != TC_NONE) return tvar_bind(tc, tvar, at->type, line);
    return 0;
}
static int tvar_constrained(TypeChecker *tc, TypeConstraint c) { int id = tvar_fresh(tc); tc->tvars[id].bound = c; return id; }
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

static void tc_push(TypeChecker *tc, TypeConstraint type, int is_linear, int line) {
    if (tc->sp >= ASTACK_MAX) return;
    AbstractType *at = &tc->data[tc->sp++]; memset(at, 0, sizeof(*at));
    at->type = type; at->ownership = OWN_OWN; at->is_linear = is_linear; at->source_line = line;
}

static TCBinding *tc_lookup(TypeChecker *tc, uint32_t sym) {
    for (int i = tc->bind_count - 1; i >= 0; i--) if (tc->bindings[i].sym == sym) return &tc->bindings[i];
    return NULL;
}

static void tc_error(TypeChecker *tc, int line, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "%s:%d: type error: ", current_file, line);
    vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap); tc->errors++;
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
    static uint32_t s_def = 0, s_let = 0, s_recur = 0;
    if (!s_def) { s_def = sym_intern("def"); s_let = sym_intern("let"); s_recur = sym_intern("recur"); }
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
            if (sym == s_def || sym == s_let) {
                int need = 2; if (vsp < need) { consumed += need - vsp; vsp = 0; } else vsp -= need;
            } else if (sym == s_recur) {
                int need = 1; if (vsp < need) { consumed += need - vsp; vsp = 0; } else vsp -= need;
            } else {
                TypeSig *sig = typesig_find(sym);
                if (sig && !sig->is_todo) {
                    int inputs = 0, outputs = 0; TypeConstraint last_out = TC_NONE;
                    for (int j = 0; j < sig->slot_count; j++) {
                        if (sig->slots[j].is_env) continue;
                        if (sig->slots[j].direction == DIR_IN) inputs++;
                        else { outputs++; last_out = sig->slots[j].constraint; }
                    }
                    if (vsp < inputs) { consumed += inputs - vsp; vsp = 0; } else vsp -= inputs;
                    vsp += outputs; if (outputs > 0) top_type = last_out;
                } else if (sig && sig->is_todo) {
                    HOEffect *ho = ho_ops_find(sym);
                    if (ho) {
                        if (vsp < ho->need) { consumed += ho->need - vsp; vsp = 0; } else vsp -= ho->need;
                        vsp += ho->out; if (ho->out > 0) top_type = ho->out_type;
                    }
                }
                if (ctx) {
                    TCBinding *ub = tc_lookup(ctx, sym);
                    if (ub && ub->is_def && ub->atype.type == TC_TUPLE && ub->atype.has_effect) {
                        int need = ub->atype.effect_consumed, out = ub->atype.effect_produced;
                        if (vsp < need) { consumed += need - vsp; vsp = 0; } else vsp -= need;
                        vsp += out;
                        if (out > 0 && ub->atype.effect_out_type != TC_NONE) top_type = ub->atype.effect_out_type;
                        else if (out > 0) top_type = TC_NONE;
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

static TypeConstraint tc_infer_effect(Token *toks, int start, int end, int total_count, int *out_consumed, int *out_produced) {
    return tc_infer_effect_ctx(toks, start, end, total_count, out_consumed, out_produced, NULL);
}

static void tc_expect(TypeChecker *tc, TypeConstraint expected, const char *op, int line) {
    if (tc->sp > 0 && tc->data[tc->sp-1].type != expected && tc->data[tc->sp-1].type != TC_NONE)
        tc_error(tc, line, "'%s' expected %s, got %s", op, constraint_name(expected), constraint_name(tc->data[tc->sp-1].type));
}

static void tc_apply_effect(TypeChecker *tc, int consumed, int produced, TypeConstraint out_type, int line) {
    if (tc->sp < consumed) tc->sp = 0; else tc->sp -= consumed;
    for (int i = 0; i < produced; i++) {
        TypeConstraint t = (i == produced - 1) ? out_type : TC_NONE;
        tc_push(tc, t, (t == TC_BOX) ? 1 : 0, line);
    }
}

static TypeConstraint tc_last_popped_out_type;
static int tc_pop_tuple(TypeChecker *tc, int *eff_c, int *eff_p) {
    tc_last_popped_out_type = TC_NONE;
    if (tc->sp <= 0) return 0;
    AbstractType *top = &tc->data[tc->sp - 1];
    if (top->type != TC_TUPLE) return 0;
    *eff_c = top->has_effect ? top->effect_consumed : 0;
    *eff_p = top->has_effect ? top->effect_produced : 0;
    tc_last_popped_out_type = top->effect_out_type; tc->sp--;
    return top->has_effect;
}

static int tc_check_body_against_sig(Token *toks, int start, int end, int total_count, TypeSig *sig) {
    int errors = 0, n_in = 0, n_out = 0;
    for (int i = 0; i < sig->slot_count; i++) {
        if (sig->slots[i].is_env) continue;
        if (sig->slots[i].direction == DIR_IN) n_in++; else n_out++;
    }
    int eff_consumed, eff_produced;
    tc_infer_effect(toks, start, end, total_count, &eff_consumed, &eff_produced);
    if (eff_consumed != n_in) { fprintf(stderr, "%s:%d: type error: function body consumes %d value(s) but type declares %d input(s)\n", current_file, toks[start].line, eff_consumed, n_in); errors++; }
    if (eff_produced != n_out) { fprintf(stderr, "%s:%d: type error: function body produces %d value(s) but type declares %d output(s)\n", current_file, toks[start].line, eff_produced, n_out); errors++; }
    return errors;
}

static int tc_is_copyable(AbstractType *t) { return !t->is_linear && t->type != TC_BOX; }
static int tc_constraint_matches(TypeConstraint constraint, TypeConstraint actual) {
    if (constraint == TC_NONE || constraint == actual) return 1;
    if (constraint == TC_NUM && (actual == TC_INT || actual == TC_FLOAT)) return 1;
    return 0;
}
static void tc_bind(TypeChecker *tc, uint32_t sym, AbstractType *atype, int is_def) {
    for (int i = 0; i < tc->bind_count; i++) {
        if (tc->bindings[i].sym == sym) { tc->bindings[i].atype = *atype; tc->bindings[i].is_def = is_def; return; }
    }
    if (tc->bind_count < TC_BINDS_MAX) {
        tc->bindings[tc->bind_count].sym = sym; tc->bindings[tc->bind_count].atype = *atype;
        tc->bindings[tc->bind_count].is_def = is_def; tc->bind_count++;
    }
}

static void tc_check_word(TypeChecker *tc, uint32_t sym, int line) {
    TypeSig *sig = typesig_find(sym);
    if (!sig) {
        TCBinding *b = tc_lookup(tc, sym);
        if (b) {
            if (b->is_def && b->atype.type == TC_TUPLE) {
                TypeSig *user_sig = typesig_find(sym);
                if (user_sig && !user_sig->is_todo) { sig = user_sig; }
                else if (b->atype.has_effect) {
                    if (b->atype.scheme_tvar_count > 0) {
                        int map[64] = {0}, sc = b->atype.scheme_tvar_count;
                        if (sc > 64) sc = 64;
                        tvar_instantiate(tc, b->atype.scheme_tvar_base, sc, map);
                        for (int j = 0; j < b->atype.scheme_in_count && j < tc->sp; j++) {
                            int scheme_tv = b->atype.scheme_in[j] - b->atype.scheme_tvar_base;
                            if (scheme_tv >= 0 && scheme_tv < sc) {
                                int fresh_tv = map[scheme_tv];
                                AbstractType *input = &tc->data[tc->sp - b->atype.scheme_in_count + j];
                                tvar_unify_at(tc, fresh_tv, input, line);
                                int fresh_elem = tc->tvars[tvar_find(tc, fresh_tv)].elem;
                                if (fresh_elem > 0 && input->elem_tvar > 0) tvar_unify(tc, fresh_elem, input->elem_tvar, line);
                                else if (fresh_elem > 0 && input->elem_type != TC_NONE) tvar_bind(tc, fresh_elem, input->elem_type, line);
                                int fresh_box = tc->tvars[tvar_find(tc, fresh_tv)].box_c;
                                if (fresh_box > 0 && input->box_tvar > 0) tvar_unify(tc, fresh_box, input->box_tvar, line);
                                else if (fresh_box > 0 && input->box_contents != TC_NONE) tvar_bind(tc, fresh_box, input->box_contents, line);
                            }
                        }
                        if (tc->sp < b->atype.effect_consumed) tc->sp = 0; else tc->sp -= b->atype.effect_consumed;
                        for (int j = 0; j < b->atype.scheme_out_count; j++) {
                            int scheme_tv = b->atype.scheme_out[j] - b->atype.scheme_tvar_base;
                            int fresh_tv = (scheme_tv >= 0 && scheme_tv < sc) ? map[scheme_tv] : 0;
                            tc_push(tc, TC_NONE, 0, line);
                            AbstractType *out_at = &tc->data[tc->sp - 1];
                            if (fresh_tv > 0) {
                                TypeConstraint resolved = tvar_resolve(tc, fresh_tv);
                                if (resolved != TC_NONE) out_at->type = resolved;
                                out_at->tvar_id = fresh_tv;
                                out_at->is_linear = (resolved == TC_BOX) ? 1 : 0;
                            }
                        }
                        for (int j = b->atype.scheme_out_count; j < b->atype.effect_produced; j++) {
                            TypeConstraint out = (j == b->atype.effect_produced - 1) ? b->atype.effect_out_type : TC_NONE;
                            tc_push(tc, out, 0, line);
                        }
                    } else tc_apply_effect(tc, b->atype.effect_consumed, b->atype.effect_produced, b->atype.effect_out_type, line);
                    return;
                }
                if (!sig) return;
            } else { tc_push(tc, b->atype.type, b->atype.is_linear, line); return; }
        } else {
            if (tc->unknown_count < TC_UNKNOWN_MAX) { tc->unknowns[tc->unknown_count].sym = sym; tc->unknowns[tc->unknown_count].line = line; tc->unknown_count++; }
            return;
        }
    }
    if (sig->is_todo) {
        ho_ops_ensure_init();
        uint32_t s_apply=ho_ops[0].sym, s_dip=ho_ops[1].sym, s_if=ho_ops[2].sym,
            s_map=ho_ops[3].sym, s_filter=ho_ops[4].sym, s_fold=ho_ops[5].sym,
            s_lend=ho_ops[10].sym, s_mutate=ho_ops[11].sym,
            s_clone=ho_ops[12].sym, s_cond=ho_ops[13].sym, s_match=ho_ops[14].sym;
        if (sym == s_apply) {
            if (tc->sp > 0 && tc->data[tc->sp-1].type != TC_TUPLE && tc->data[tc->sp-1].type != TC_NONE) { tc_expect(tc, TC_TUPLE, "apply", line); tc->sp--; }
            else { int ec, ep; if (tc_pop_tuple(tc, &ec, &ep)) tc_apply_effect(tc, ec, ep, tc_last_popped_out_type, line); }
        } else if (sym == s_dip) {
            tc_expect(tc, TC_TUPLE, "dip", line);
            int ec, ep;
            if (tc_pop_tuple(tc, &ec, &ep)) {
                int had_saved = (tc->sp > 0); AbstractType saved = {0};
                if (had_saved) { saved = tc->data[tc->sp - 1]; tc->sp--; }
                tc_apply_effect(tc, ec, ep, tc_last_popped_out_type, line);
                if (had_saved && tc->sp < ASTACK_MAX) tc->data[tc->sp++] = saved;
            }
        } else if (sym == s_if) {
            int eec, eep, else_has; TypeConstraint else_out = TC_NONE;
            if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE) { else_has = tc_pop_tuple(tc, &eec, &eep); else_out = tc_last_popped_out_type; }
            else if (tc->sp > 0) { else_out = tc->data[tc->sp-1].type; else_has = 0; eec = 0; eep = 1; tc->sp--; }
            else { else_has = 0; eec = 0; eep = 0; }
            if (tc->sp > 0 && tc->data[tc->sp-1].type != TC_TUPLE && tc->data[tc->sp-1].type != TC_NONE)
                tc_error(tc, line, "'if' then branch must be tuple, got %s", constraint_name(tc->data[tc->sp-1].type));
            int tec, tep; int then_has = tc_pop_tuple(tc, &tec, &tep); TypeConstraint then_out = tc_last_popped_out_type;
            if (then_out != TC_NONE && else_out != TC_NONE && then_out != else_out)
                if (!tc_constraint_matches(then_out, else_out) && !tc_constraint_matches(else_out, then_out))
                    tc_error(tc, line, "'if' branches produce different types: then produces %s, else produces %s", constraint_name(then_out), constraint_name(else_out));
            if (tc->sp > 0) tc->sp--;
            TypeConstraint out = then_out; if (out == TC_NONE) out = else_out;
            if (then_has) tc_apply_effect(tc, tec, tep, out, line);
            else if (else_has) tc_apply_effect(tc, eec, eep, out, line);
        } else if (sym == s_cond || sym == s_match) {
            TypeConstraint result_type = TC_NONE;
            if (tc->sp >= 3) {
                AbstractType *def = &tc->data[tc->sp - 1];
                if (def->type == TC_TUPLE && def->effect_out_type != TC_NONE) result_type = def->effect_out_type;
                else if (def->type != TC_TUPLE && def->type != TC_NONE) result_type = def->type;
                tc->sp--;
                AbstractType *clauses = &tc->data[tc->sp - 1];
                if (clauses->type != TC_TUPLE && clauses->type != TC_REC && clauses->type != TC_NONE)
                    tc_error(tc, line, "'%s' expected tuple or record of clauses, got %s", sym_name(sym), constraint_name(clauses->type));
                if (result_type != TC_NONE && clauses->effect_out_type != TC_NONE && result_type != clauses->effect_out_type)
                    if (!tc_constraint_matches(result_type, clauses->effect_out_type) && !tc_constraint_matches(clauses->effect_out_type, result_type))
                        tc_error(tc, line, "'%s' clause bodies produce %s but default is %s", sym_name(sym), constraint_name(clauses->effect_out_type), constraint_name(result_type));
                tc->sp--;
                if (sym == s_match && tc->data[tc->sp-1].type != TC_SYM && tc->data[tc->sp-1].type != TC_NONE)
                    tc_error(tc, line, "'match' scrutinee must be a symbol, got %s", constraint_name(tc->data[tc->sp-1].type));
                tc->sp--;
            } else tc->sp = 0;
            tc_push(tc, result_type, 0, line);
        } else if (sym == s_map) {
            int ec, ep; int map_known = tc_pop_tuple(tc, &ec, &ep); TypeConstraint map_out = tc_last_popped_out_type;
            if (map_known && (ec != 1 || ep != 1) && (ec + ep > 0)) tc_error(tc, line, "'map' body must be 1->1, got %d->%d", ec, ep);
            tc_expect(tc, TC_LIST, "map", line);
            if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_LIST) { if (map_out != TC_NONE) tc->data[tc->sp-1].elem_type = map_out; }
            else if (tc->sp > 0) { tc->sp--; tc_push(tc, TC_LIST, 0, line); if (map_out != TC_NONE) tc->data[tc->sp-1].elem_type = map_out; }
        } else if (sym == s_filter) {
            int ec, ep; int filt_known = tc_pop_tuple(tc, &ec, &ep);
            if (filt_known && (ec != 1 || ep != 1) && (ec + ep > 0)) tc_error(tc, line, "'filter' body must be 1->1, got %d->%d", ec, ep);
            tc_expect(tc, TC_LIST, "filter", line);
        } else if (sym == s_fold) {
            int ec, ep; tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
            AbstractType init = {0};
            if (tc->sp > 0) { init = tc->data[tc->sp-1]; tc->sp--; }
            if (tc->sp > 0) {
                if (tc->data[tc->sp-1].type == TC_LIST && tc->data[tc->sp-1].elem_type != TC_NONE
                    && init.type != TC_NONE && !tc_constraint_matches(init.type, tc->data[tc->sp-1].elem_type)
                    && !tc_constraint_matches(tc->data[tc->sp-1].elem_type, init.type))
                    tc_error(tc, line, "'fold' init is %s but list contains %s", constraint_name(init.type), constraint_name(tc->data[tc->sp-1].elem_type));
                tc_expect(tc, TC_LIST, "fold", line);
                if (tc->sp > 0) tc->sp--;
            }
            if (tc->sp < ASTACK_MAX) tc->data[tc->sp++] = init;
        } else if (sym == s_lend) {
            int ec, ep; int has = tc_pop_tuple(tc, &ec, &ep); TypeConstraint lend_fn_out = tc_last_popped_out_type;
            tc_expect(tc, TC_BOX, "lend", line);
            if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
                TypeConstraint contents = tc->data[tc->sp-1].box_contents;
                tc->data[tc->sp-1].borrowed++;
                int results = has ? (1 - ec + ep) : 1; if (results < 0) results = 0;
                for (int j = 0; j < results; j++) {
                    TypeConstraint rt = (j == results - 1 && lend_fn_out != TC_NONE) ? lend_fn_out : (j == 0 && contents != TC_NONE) ? contents : TC_NONE;
                    tc_push(tc, rt, 0, line);
                }
                int box_idx = tc->sp - results - 1;
                if (box_idx >= 0) tc->data[box_idx].borrowed--;
            }
        } else if (sym == s_mutate) {
            int ec, ep; tc_pop_tuple(tc, &ec, &ep); TypeConstraint mut_fn_out = tc_last_popped_out_type;
            tc_expect(tc, TC_BOX, "mutate", line);
            if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
                TypeConstraint contents = tc->data[tc->sp-1].box_contents;
                if (contents != TC_NONE && mut_fn_out != TC_NONE && !tc_constraint_matches(contents, mut_fn_out) && !tc_constraint_matches(mut_fn_out, contents))
                    tc_error(tc, line, "'mutate' body produces %s but box contains %s", constraint_name(mut_fn_out), constraint_name(contents));
            }
        } else if (sym == s_clone) {
            if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
                TypeConstraint contents = tc->data[tc->sp-1].box_contents;
                tc_push(tc, TC_BOX, 1, line); tc->data[tc->sp-1].box_contents = contents;
            } else if (tc->sp > 0 && tc->data[tc->sp-1].type != TC_NONE)
                tc_error(tc, line, "'clone' expected box, got %s", constraint_name(tc->data[tc->sp-1].type));
        } else {
            HOEffect *ho = ho_ops_find(sym);
            if (ho) {
                int to_pop = ho->need;
                while (to_pop > 0 && tc->sp > 0) {
                    int ec, ep;
                    if (tc->data[tc->sp-1].type == TC_TUPLE) tc_pop_tuple(tc, &ec, &ep); else tc->sp--;
                    to_pop--;
                }
                for (int j = 0; j < ho->out; j++) tc_push(tc, (j == ho->out - 1) ? ho->out_type : TC_NONE, 0, line);
            }
        }
        return;
    }
    int inputs = 0;
    for (int i = 0; i < sig->slot_count; i++) { if (sig->slots[i].is_env) continue; if (sig->slots[i].direction == DIR_IN) inputs++; }
    if (tc->sp < inputs) {
        tc_error(tc, line, "'%s' needs %d input(s), stack has %d", sym_name(sym), inputs, tc->sp);
        if (tc->sp > 0) tc_dump_stack(tc);
        return;
    }
    #define MAX_TVARS 16
    struct { uint32_t var; int tvar; } tvar_map[MAX_TVARS]; int tvar_map_count = 0;
    for (int i = 0; i < sig->slot_count; i++) {
        uint32_t tv = sig->slots[i].type_var;
        if (!tv || sig->slots[i].is_env) continue;
        int found = 0;
        for (int j = 0; j < tvar_map_count; j++) if (tvar_map[j].var == tv) { found = 1; break; }
        if (!found && tvar_map_count < MAX_TVARS) {
            int id = tvar_fresh(tc);
            if (sig->slots[i].constraint != TC_NONE) tc->tvars[id].bound = sig->slots[i].constraint;
            tvar_map[tvar_map_count].var = tv; tvar_map[tvar_map_count].tvar = id; tvar_map_count++;
        }
    }
    #define FIND_TVAR(tv_name) ({ int _tv = 0; for (int _j = 0; _j < tvar_map_count; _j++) if (tvar_map[_j].var == (tv_name)) { _tv = tvar_map[_j].tvar; break; } _tv; })
    int stack_pos = tc->sp - 1;
    for (int i = sig->slot_count - 1; i >= 0; i--) {
        TypeSlot *slot = &sig->slots[i];
        if (slot->is_env || slot->direction != DIR_IN) continue;
        if (stack_pos < 0) break;
        AbstractType *at = &tc->data[stack_pos];
        if (slot->ownership == OWN_COPY && !tc_is_copyable(at))
            tc_error(tc, line, "'%s' requires copyable value, got linear type (box or box-containing)", sym_name(sym));
        if (slot->ownership == OWN_OWN && at->borrowed > 0)
            tc_error(tc, line, "'%s' cannot consume value that is currently borrowed (lent)", sym_name(sym));
        if (slot->constraint != TC_NONE && at->type != TC_NONE) {
            if (!tc_constraint_matches(slot->constraint, at->type)) {
                fprintf(stderr, "%s:%d: type error: '%s' expected %s, got %s", current_file, line, sym_name(sym), constraint_name(slot->constraint), constraint_name(at->type));
                if (at->source_line) fprintf(stderr, " (value from line %d)", at->source_line);
                fprintf(stderr, "\n"); tc_dump_stack(tc); tc->errors++;
            }
        }
        if (slot->type_var) {
            int tv = FIND_TVAR(slot->type_var);
            if (tv > 0) {
                if (at->tvar_id > 0) {
                    if (tvar_unify(tc, tv, at->tvar_id, line)) {
                        fprintf(stderr, "%s:%d: type error: '%s' type variable '%s' mismatch: expected %s, got %s", current_file, line, sym_name(sym), sym_name(slot->type_var), constraint_name(tvar_resolve(tc, tv)), constraint_name(tvar_resolve(tc, at->tvar_id)));
                        if (at->source_line) fprintf(stderr, " (value from line %d)", at->source_line);
                        fprintf(stderr, "\n"); tc->errors++;
                    }
                } else if (at->type != TC_NONE) {
                    if (tvar_bind(tc, tv, at->type, line)) {
                        fprintf(stderr, "%s:%d: type error: '%s' type variable '%s' mismatch: expected %s (from earlier arg), got %s", current_file, line, sym_name(sym), sym_name(slot->type_var), constraint_name(tvar_resolve(tc, tv)), constraint_name(at->type));
                        if (at->source_line) fprintf(stderr, " (value from line %d)", at->source_line);
                        fprintf(stderr, "\n"); tc_dump_stack(tc); tc->errors++;
                    }
                }
            }
        }
        if (at->is_linear && slot->ownership == OWN_OWN) at->consumed = 1;
        stack_pos--;
    }
    TypeConstraint input_list_elem = TC_NONE; int input_list_elem_tvar = 0, input_box_tvar = 0;
    TypeConstraint input_val_type = TC_NONE;
    {
        int val_slot_idx = -1, slot_pos = 0;
        for (int j = 0; j < sig->slot_count; j++) {
            if (sig->slots[j].is_env || sig->slots[j].direction != DIR_IN) continue;
            if (sig->slots[j].constraint == TC_NONE) val_slot_idx = slot_pos; slot_pos++;
        }
        for (int i = tc->sp - inputs; i < tc->sp; i++) {
            if (i < 0) continue; TypeConstraint it = at_type(tc, &tc->data[i]);
            if (it == TC_LIST || tc->data[i].elem_tvar > 0) {
                if (tc->data[i].elem_type != TC_NONE) input_list_elem = tc->data[i].elem_type;
                if (tc->data[i].elem_tvar > 0) input_list_elem_tvar = tc->data[i].elem_tvar;
            }
            if (it == TC_BOX || tc->data[i].box_tvar > 0) { if (tc->data[i].box_tvar > 0) input_box_tvar = tc->data[i].box_tvar; }
        }
        (void)input_box_tvar;
        if (val_slot_idx >= 0) { int vi = tc->sp - inputs + val_slot_idx; if (vi >= 0 && vi < tc->sp) input_val_type = tc->data[vi].type; }
    }
    if (input_list_elem != TC_NONE && input_val_type != TC_NONE) {
        static uint32_t s_give3=0, s_set3=0;
        if (!s_give3) { s_give3=sym_intern("give"); s_set3=sym_intern("set"); }
        if (sym == s_give3 || sym == s_set3)
            if (!tc_constraint_matches(input_list_elem, input_val_type))
                tc_error(tc, line, "'%s' adds %s to list of %s", sym_name(sym), constraint_name(input_val_type), constraint_name(input_list_elem));
    }
    { static uint32_t s_cat2=0; if (!s_cat2) s_cat2=sym_intern("cat");
      if (sym == s_cat2 && inputs == 2) {
          int a = tc->sp - inputs, b = tc->sp - inputs + 1;
          if (a >= 0 && b < tc->sp + inputs && tc->data[a].type == TC_LIST && tc->data[b].type == TC_LIST
              && tc->data[a].elem_type != TC_NONE && tc->data[b].elem_type != TC_NONE && tc->data[a].elem_type != tc->data[b].elem_type)
              tc_error(tc, line, "'cat' concatenates list of %s with list of %s", constraint_name(tc->data[a].elem_type), constraint_name(tc->data[b].elem_type));
      }
    }
    tc->sp -= inputs;
    for (int i = 0; i < sig->slot_count; i++) {
        TypeSlot *slot = &sig->slots[i];
        if (slot->is_env || slot->direction != DIR_OUT) continue;
        if (tc->sp >= ASTACK_MAX) { tc->errors++; return; }
        AbstractType *at = &tc->data[tc->sp++]; memset(at, 0, sizeof(*at));
        at->type = slot->constraint;
        if (slot->type_var) {
            int tv = FIND_TVAR(slot->type_var);
            if (tv > 0) { TypeConstraint resolved = tvar_resolve(tc, tv); if (resolved != TC_NONE) at->type = resolved; at->tvar_id = tv; }
        }
        at->ownership = slot->ownership; at->is_linear = (at->type == TC_BOX) ? 1 : 0;
        at->consumed = 0; at->source_line = line;
        if (input_list_elem != TC_NONE || input_list_elem_tvar > 0) {
            static uint32_t s_get2=0,s_first2=0,s_last2=0,s_grab2=0,s_pop2=0,s_pull2=0;
            if (!s_get2) { s_get2=sym_intern("get"); s_first2=sym_intern("first"); s_last2=sym_intern("last"); s_grab2=sym_intern("grab"); s_pop2=sym_intern("pop"); s_pull2=sym_intern("pull"); }
            if (at->type == TC_NONE && (sym==s_get2||sym==s_first2||sym==s_last2||sym==s_grab2||sym==s_pop2||sym==s_pull2)) {
                if (input_list_elem != TC_NONE) at->type = input_list_elem;
                if (input_list_elem_tvar > 0) { at->tvar_id = input_list_elem_tvar; TypeConstraint resolved = tvar_resolve(tc, input_list_elem_tvar); if (resolved != TC_NONE) at->type = resolved; }
            }
            if (at->type == TC_LIST) { at->elem_type = input_list_elem; if (input_list_elem_tvar > 0) at->elem_tvar = input_list_elem_tvar; }
        }
        if (at->type == TC_LIST && at->elem_type == TC_NONE && input_val_type != TC_NONE) {
            static uint32_t s_give2=0,s_set2=0; if (!s_give2) { s_give2=sym_intern("give"); s_set2=sym_intern("set"); }
            if (sym==s_give2||sym==s_set2) at->elem_type = input_val_type;
        }
        if (at->type == TC_LIST && at->elem_type == TC_NONE) {
            static uint32_t s_range2=0,s_classify2=0,s_rise2=0,s_fall2=0;
            if (!s_range2) { s_range2=sym_intern("range"); s_classify2=sym_intern("classify"); s_rise2=sym_intern("rise"); s_fall2=sym_intern("fall"); }
            if (sym==s_range2||sym==s_classify2||sym==s_rise2||sym==s_fall2) at->elem_type = TC_INT;
        }
        if (at->type == TC_BOX && input_val_type != TC_NONE) {
            static uint32_t s_box2=0; if (!s_box2) s_box2=sym_intern("box");
            if (sym == s_box2) at->box_contents = input_val_type;
        }
    }
    #undef MAX_TVARS
}

static TypeConstraint tc_check_list_elements(Token *toks, int start, int end, int total_count, int *errors, int line) {
    TypeConstraint elem_type = TC_NONE; int elem_count = 0;
    int first_effect_c = 0, first_effect_p = 0; TypeConstraint first_effect_out = TC_NONE; int has_first_effect = 0;
    for (int i = start; i < end; i++) {
        Token *t = &toks[i]; TypeConstraint this_type = TC_NONE;
        int this_eff_c = 0, this_eff_p = 0; TypeConstraint this_eff_out = TC_NONE; int this_has_effect = 0;
        switch (t->tag) {
        case TOK_INT: this_type = TC_INT; break; case TOK_FLOAT: this_type = TC_FLOAT; break;
        case TOK_SYM: this_type = TC_SYM; break; case TOK_STRING: this_type = TC_LIST; break;
        case TOK_LPAREN: { int close = find_matching(toks, i+1, total_count, TOK_LPAREN, TOK_RPAREN);
            this_type = TC_TUPLE; this_eff_out = tc_infer_effect(toks, i+1, close, total_count, &this_eff_c, &this_eff_p);
            this_has_effect = 1; i = close; break; }
        case TOK_LBRACKET: { int close = find_matching(toks, i+1, total_count, TOK_LBRACKET, TOK_RBRACKET); this_type = TC_LIST; i = close; break; }
        case TOK_LBRACE: { int close = find_matching(toks, i+1, total_count, TOK_LBRACE, TOK_RBRACE); this_type = TC_REC; i = close; break; }
        case TOK_WORD: { TypeSig *sig = typesig_find(t->as.sym);
            if (sig && !sig->is_todo) { for (int j = sig->slot_count-1; j >= 0; j--) { if (sig->slots[j].is_env) continue; if (sig->slots[j].direction == DIR_OUT) { this_type = sig->slots[j].constraint; break; } } }
            if (this_type == TC_NONE) return TC_NONE; break; }
        default: continue;
        }
        if (this_type == TC_NONE) continue;
        elem_count++;
        if (elem_count == 1) { elem_type = this_type; first_effect_c = this_eff_c; first_effect_p = this_eff_p; first_effect_out = this_eff_out; has_first_effect = this_has_effect; }
        else {
            if (this_type != elem_type) {
                fprintf(stderr, "%s:%d: type error: list elements have inconsistent types: element 1 is %s, element %d is %s\n", current_file, line, constraint_name(elem_type), elem_count, constraint_name(this_type));
                (*errors)++; return TC_NONE;
            }
            if (this_type == TC_TUPLE && has_first_effect && this_has_effect) {
                int first_net = first_effect_p - first_effect_c, this_net = this_eff_p - this_eff_c;
                if (first_net != this_net) {
                    fprintf(stderr, "%s:%d: type error: list of tuples have different stack effects: element 1 is -%d+%d (net %+d), element %d is -%d+%d (net %+d)\n",
                            current_file, line, first_effect_c, first_effect_p, first_net, elem_count, this_eff_c, this_eff_p, this_net);
                    (*errors)++; return TC_NONE;
                }
                if (first_effect_out != this_eff_out && first_effect_out != TC_NONE && this_eff_out != TC_NONE) {
                    fprintf(stderr, "%s:%d: type error: list of tuples produce different types: element 1 produces %s, element %d produces %s\n",
                            current_file, line, constraint_name(first_effect_out), elem_count, constraint_name(this_eff_out));
                    (*errors)++; return TC_NONE;
                }
            }
        }
    }
    return elem_type;
}

static uint32_t sym_def_k = 0, sym_let_k = 0, sym_recur_k = 0;

static void tc_process_range(TypeChecker *tc, Token *toks, int start, int end, int total_count) {
    if (!sym_def_k) {
        sym_def_k = sym_intern("def"); sym_let_k = sym_intern("let"); sym_recur_k = sym_intern("recur");
        if (!sym_effect_kw) sym_effect_kw = sym_intern("effect");
        if (!sym_check_kw) sym_check_kw = sym_intern("check");
    }
    for (int i = start; i < end; i++) {
        Token *t = &toks[i]; current_line = t->line;
        switch (t->tag) {
        case TOK_INT: tc_push(tc, TC_INT, 0, t->line); break;
        case TOK_FLOAT: tc_push(tc, TC_FLOAT, 0, t->line); break;
        case TOK_SYM: tc_push(tc, TC_SYM, 0, t->line); tc->data[tc->sp-1].sym_id = t->as.sym; break;
        case TOK_STRING: tc_push(tc, TC_LIST, 0, t->line); break;
        case TOK_LPAREN: {
            int close = find_matching(toks, i+1, total_count, TOK_LPAREN, TOK_RPAREN);
            int eff_c = 0, eff_p = 0;
            TypeConstraint eff_out = tc_infer_effect_ctx(toks, i+1, close, total_count, &eff_c, &eff_p, tc);
            int is_simple = 1;
            for (int j = i+1; j < close; j++) if (toks[j].tag == TOK_LPAREN || toks[j].tag == TOK_LBRACKET || toks[j].tag == TOK_LBRACE) { is_simple = 0; break; }
            int scheme_base = tc->tvar_count, in_count = 0, out_count = 0;
            int in_tvars[8] = {0}, out_tvars[8] = {0}, scheme_count = 0;
            if (is_simple && eff_c <= 8 && eff_p <= 8) {
                int saved_sp = tc->sp, saved_binds = tc->bind_count, saved_unknowns = tc->unknown_count;
                int saved_errors = tc->errors, saved_recur = tc->recur_pending;
                in_count = eff_c;
                for (int j = 0; j < in_count; j++) {
                    in_tvars[j] = tvar_fresh(tc);
                    int e_tv = tvar_fresh(tc), b_tv = tvar_fresh(tc);
                    tc->tvars[in_tvars[j]].elem = e_tv; tc->tvars[in_tvars[j]].box_c = b_tv;
                    tc_push(tc, TC_NONE, 0, t->line);
                    tc->data[tc->sp-1].tvar_id = in_tvars[j]; tc->data[tc->sp-1].elem_tvar = e_tv; tc->data[tc->sp-1].box_tvar = b_tv;
                }
                tc_process_range(tc, toks, i+1, close, total_count);
                int actual_out = tc->sp - saved_sp;
                out_count = actual_out > 8 ? 8 : (actual_out > 0 ? actual_out : 0);
                for (int j = 0; j < out_count; j++) {
                    int idx = tc->sp - out_count + j;
                    if (idx >= 0) {
                        out_tvars[j] = tc->data[idx].tvar_id;
                        if (out_tvars[j] == 0 && tc->data[idx].type != TC_NONE) out_tvars[j] = tvar_constrained(tc, tc->data[idx].type);
                        else if (out_tvars[j] == 0) out_tvars[j] = tvar_fresh(tc);
                    }
                }
                scheme_count = tc->tvar_count - scheme_base;
                tc->sp = saved_sp; tc->bind_count = saved_binds; tc->unknown_count = saved_unknowns;
                tc->errors = saved_errors; tc->recur_pending = saved_recur;
            }
            tc_push(tc, TC_TUPLE, 0, t->line);
            AbstractType *tup = &tc->data[tc->sp-1];
            tup->has_effect = 1; tup->effect_consumed = eff_c; tup->effect_produced = eff_p; tup->effect_out_type = eff_out;
            tup->scheme_tvar_base = scheme_base; tup->scheme_tvar_count = scheme_count;
            tup->scheme_in_count = in_count; tup->scheme_out_count = out_count;
            for (int j = 0; j < in_count; j++) tup->scheme_in[j] = in_tvars[j];
            for (int j = 0; j < out_count; j++) tup->scheme_out[j] = out_tvars[j];
            i = close; break;
        }
        case TOK_LBRACKET: {
            int close = find_matching(toks, i+1, total_count, TOK_LBRACKET, TOK_RBRACKET);
            TypeConstraint elem = tc_check_list_elements(toks, i+1, close, total_count, &tc->errors, t->line);
            tc_push(tc, TC_LIST, 0, t->line); tc->data[tc->sp-1].elem_type = elem; i = close; break;
        }
        case TOK_LBRACE: {
            int close = find_matching(toks, i+1, total_count, TOK_LBRACE, TOK_RBRACE);
            TypeConstraint val_type = TC_NONE, val_effect_out = TC_NONE;
            int val_eff_c = 0, val_eff_p = 0, pair_count = 0, has_val_effect = 0;
            for (int j = i+1; j < close; ) {
                if (j >= close) break;
                if (toks[j].tag == TOK_LPAREN) j = find_matching(toks, j+1, total_count, TOK_LPAREN, TOK_RPAREN) + 1;
                else if (toks[j].tag == TOK_SYM) j++; else j++;
                if (j >= close) break;
                TypeConstraint this_val = TC_NONE;
                if (toks[j].tag == TOK_LPAREN) {
                    int vclose = find_matching(toks, j+1, total_count, TOK_LPAREN, TOK_RPAREN);
                    int vc = 0, vp = 0;
                    TypeConstraint vout = tc_infer_effect_ctx(toks, j+1, vclose, total_count, &vc, &vp, tc);
                    this_val = TC_TUPLE;
                    if (pair_count == 0) { val_eff_c = vc; val_eff_p = vp; val_effect_out = vout; has_val_effect = 1; }
                    if (has_val_effect && vout != TC_NONE && val_effect_out != TC_NONE && vout != val_effect_out)
                        if (!tc_constraint_matches(val_effect_out, vout) && !tc_constraint_matches(vout, val_effect_out))
                            tc_error(tc, t->line, "clause bodies produce different types: clause 1 produces %s, clause %d produces %s", constraint_name(val_effect_out), pair_count+1, constraint_name(vout));
                    j = vclose + 1;
                } else {
                    if (toks[j].tag == TOK_INT) this_val = TC_INT;
                    else if (toks[j].tag == TOK_FLOAT) this_val = TC_FLOAT;
                    else if (toks[j].tag == TOK_SYM) this_val = TC_SYM;
                    else if (toks[j].tag == TOK_STRING) this_val = TC_LIST;
                    j++;
                }
                if (pair_count == 0 && this_val != TC_NONE) val_type = this_val;
                else if (this_val != TC_NONE && val_type != TC_NONE && this_val != val_type)
                    if (!tc_constraint_matches(val_type, this_val) && !tc_constraint_matches(this_val, val_type))
                        tc_error(tc, t->line, "clause values have inconsistent types: %s vs %s", constraint_name(val_type), constraint_name(this_val));
                pair_count++;
            }
            int is_record = (val_type != TC_TUPLE && !has_val_effect);
            if (is_record) tc_push(tc, TC_REC, 0, t->line);
            else {
                tc_push(tc, TC_TUPLE, 0, t->line);
                if (has_val_effect) tc->data[tc->sp-1].effect_out_type = val_effect_out;
            }
            i = close; break;
        }
        case TOK_WORD: {
            uint32_t sym = t->as.sym;
            if (sym == sym_def_k) {
                if (tc->recur_pending) {
                    if (tc->sp >= 1) { AbstractType vt = tc->data[tc->sp-1]; tc->sp--; tc_bind(tc, tc->recur_sym, &vt, 1); }
                    tc->recur_pending = 0;
                } else {
                    if (tc->sp >= 2) {
                        AbstractType vt = tc->data[tc->sp-1]; uint32_t name_sym = tc->data[tc->sp-2].sym_id;
                        tc->sp -= 2; if (name_sym) tc_bind(tc, name_sym, &vt, 1);
                    } else tc->sp = 0;
                }
            } else if (sym == sym_let_k) {
                if (tc->sp >= 2) {
                    uint32_t name_sym = tc->data[tc->sp-1].sym_id; tc->sp--;
                    AbstractType val_t = tc->data[tc->sp-1]; tc->sp--;
                    if (name_sym) {
                        TCBinding *existing = tc_lookup(tc, name_sym);
                        if (existing && !existing->is_def) {
                            TypeConstraint old_t = at_type(tc, &existing->atype), new_t = at_type(tc, &val_t);
                            if (old_t != TC_NONE && new_t != TC_NONE && old_t != new_t)
                                if (!tc_constraint_matches(old_t, new_t) && !tc_constraint_matches(new_t, old_t))
                                    tc_error(tc, t->line, "rebinding '%s' changes type from %s to %s", sym_name(name_sym), constraint_name(old_t), constraint_name(new_t));
                        }
                        tc_bind(tc, name_sym, &val_t, 0);
                    }
                } else tc->sp = 0;
            } else if (sym == sym_recur_k) {
                if (tc->sp >= 1) { uint32_t name_sym = tc->data[tc->sp-1].sym_id; tc->recur_pending = 1; tc->sp--; if (name_sym) tc->recur_sym = name_sym; }
            } else if (sym == sym_effect_kw) {
                if (tc->sp > 0) tc->sp--;
                int bracket_end = i - 1;
                if (bracket_end >= start && toks[bracket_end].tag == TOK_RBRACKET) {
                    int depth = 1, bracket_start = bracket_end;
                    for (int bi = bracket_end - 1; bi >= start; bi--) {
                        if (toks[bi].tag == TOK_RBRACKET) depth++;
                        else if (toks[bi].tag == TOK_LBRACKET) { depth--; if (depth == 0) { bracket_start = bi; break; } }
                    }
                    TypeSig sig = parse_type_annotation(toks, bracket_start+1, bracket_end);
                    if (tc->sp >= 1 && tc->data[tc->sp-1].type == TC_TUPLE) {
                        if (!sig.is_todo) {
                            if (tc->sp >= 2 && tc->data[tc->sp-2].type == TC_SYM) typesig_register(tc->data[tc->sp-2].sym_id, &sig);
                            for (int bi2 = i-2; bi2 >= 0; bi2--) {
                                if (toks[bi2].tag == TOK_RPAREN) {
                                    int d2 = 1;
                                    for (int k = bi2-1; k >= 0; k--) {
                                        if (toks[k].tag == TOK_RPAREN) d2++;
                                        else if (toks[k].tag == TOK_LPAREN) { d2--; if (d2 == 0) { tc->errors += tc_check_body_against_sig(toks, k+1, bi2, total_count, &sig); break; } }
                                    }
                                    break;
                                }
                            }
                        }
                    } else if (tc->sp >= 1 && tc->data[tc->sp-1].type == TC_SYM) {
                        uint32_t name_sym = tc->data[tc->sp-1].sym_id;
                        if (!sig.is_todo) typesig_register(name_sym, &sig);
                        int has_def = (i+1 < end && toks[i+1].tag == TOK_WORD && toks[i+1].as.sym == sym_def_k);
                        if (!has_def) tc->sp--;
                    }
                }
            } else if (sym == sym_check_kw) {
                if (i >= 1 && toks[i-1].tag == TOK_WORD) {
                    const char *tw = sym_name(toks[i-1].as.sym);
                    TypeConstraint expected = TC_NONE;
                    if (strcmp(tw,"int")==0) expected=TC_INT; else if (strcmp(tw,"float")==0) expected=TC_FLOAT;
                    else if (strcmp(tw,"sym")==0) expected=TC_SYM; else if (strcmp(tw,"num")==0) expected=TC_NUM;
                    else if (strcmp(tw,"list")==0) expected=TC_LIST; else if (strcmp(tw,"tuple")==0) expected=TC_TUPLE;
                    else if (strcmp(tw,"rec")==0) expected=TC_REC; else if (strcmp(tw,"box")==0) expected=TC_BOX;
                    if (expected != TC_NONE && tc->sp > 0) tc_expect(tc, expected, "check", t->line);
                    if (tc->unknown_count > 0 && tc->unknowns[tc->unknown_count-1].sym == toks[i-1].as.sym) tc->unknown_count--;
                }
            } else tc_check_word(tc, sym, t->line);
            break;
        }
        default: break;
        }
    }
}

static int typecheck_tokens(Token *toks, int count) {
    TypeChecker tc; memset(&tc, 0, sizeof(tc)); tc.tvar_count = 1;
    tc_process_range(&tc, toks, 0, count, count);
    for (int i = 0; i < tc.sp; i++)
        if (tc.data[i].is_linear && !tc.data[i].consumed)
            tc_error(&tc, tc.data[i].source_line, "linear value (box) created here was never consumed (must free, lend, mutate, or clone)");
    for (int i = 0; i < tc.unknown_count; i++) {
        uint32_t sym = tc.unknowns[i].sym; int defined = 0;
        for (int j = 0; j < tc.bind_count; j++) if (tc.bindings[j].sym == sym) { defined = 1; break; }
        if (!defined) tc_error(&tc, tc.unknowns[i].line, "unknown word '%s'", sym_name(sym));
    }
    return tc.errors;
}

static void register_builtin_types(void) {
    static const char *todo_names[] = {
        "if","cond","match","apply","dip","map","filter","fold","reduce","each","while","loop",
        "repeat","bi","keep","lend","mutate","clone","scan","where","find","table","at","into","on","show",NULL
    };
    for (int i = 0; todo_names[i]; i++) { TypeSig s; memset(&s,0,sizeof(s)); s.is_todo=1; typesig_register(sym_intern(todo_names[i]),&s); }
}

/* ---- primitives ---- */
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

#define CMP2(nm,expr) static void prim_##nm(Frame *e){(void)e;Value bt=stack[sp-1];int bs=val_slots(bt);int as=val_slots(stack[sp-1-bs]);int r=(expr);sp-=as+bs;spush(val_int(r?1:0));}
CMP2(eq, val_equal(&stack[sp-bs-as],as,&stack[sp-bs],bs))
CMP2(neq, !val_equal(&stack[sp-bs-as],as,&stack[sp-bs],bs))
CMP2(lt, val_less(&stack[sp-bs-as],as,&stack[sp-bs],bs))
CMP2(gt, val_less(&stack[sp-bs],bs,&stack[sp-bs-as],as))
CMP2(ge, !val_less(&stack[sp-bs-as],as,&stack[sp-bs],bs))
CMP2(le, !val_less(&stack[sp-bs],bs,&stack[sp-bs-as],as))

static void prim_not(Frame *e){(void)e;spush(val_int(pop_int()==0?1:0));}
static void prim_and(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();spush(val_int((a&&b)?1:0));}
static void prim_or(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();spush(val_int((a||b)?1:0));}

#define NUM1(nm,iexpr,fexpr) static void prim_##nm(Frame *e){(void)e;Value v=spop(); \
    if(v.tag==VAL_INT)spush(val_int(iexpr));else if(v.tag==VAL_FLOAT)spush(val_float(fexpr));else die(#nm ": expected number");}
NUM1(inc,v.as.i+1,v.as.f+1) NUM1(dec,v.as.i-1,v.as.f-1) NUM1(neg,-v.as.i,-v.as.f)
NUM1(abs_op,v.as.i<0?-v.as.i:v.as.i,v.as.f<0?-v.as.f:v.as.f)

static void prim_iseven(Frame *e){(void)e;spush(val_int(pop_int()%2==0?1:0));}
static void prim_isodd(Frame *e){(void)e;spush(val_int(pop_int()%2!=0?1:0));}
static void prim_iszero(Frame *e){(void)e;spush(val_int(pop_int()==0?1:0));}

#define NUMBIN(nm,iexpr,fexpr) static void prim_##nm(Frame *e){(void)e;Value b=spop(),a=spop(); \
    if(a.tag==VAL_INT&&b.tag==VAL_INT) spush(val_int(iexpr)); \
    else if(a.tag==VAL_FLOAT&&b.tag==VAL_FLOAT) spush(val_float(fexpr)); \
    else die(#nm ": type mismatch");}
NUMBIN(max,a.as.i>b.as.i?a.as.i:b.as.i,a.as.f>b.as.f?a.as.f:b.as.f)
NUMBIN(min,a.as.i<b.as.i?a.as.i:b.as.i,a.as.f<b.as.f?a.as.f:b.as.f)

static void prim_divides(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();if(b==0)die("divides: division by zero");spush(val_int(a%b==0?1:0));}

static void prim_print(Frame *e){(void)e;if(sp<=0)die("print: stack underflow");Value top=stack[sp-1];int s=val_slots(top);val_print(&stack[sp-s],s,stdout);printf("\n");sp-=s;}
static void prim_assert(Frame *e){(void)e;if(!pop_int())die("assertion failed");}
static void prim_halt(Frame *e){(void)e;exit(0);}
static void prim_random(Frame *e){(void)e;int64_t max=pop_int();if(max<=0)die("random: max must be positive");spush(val_int(rand()%max));}

static void eval_default(Value *buf, int s, Value top, Value *scrut, int scrut_s, Frame *env) {
    if (top.tag == VAL_INT && top.as.i == -1) { spush(val_int(-1)); return; }
    if (scrut) { memcpy(&stack[sp], scrut, scrut_s*sizeof(Value)); sp += scrut_s; }
    if (top.tag == VAL_TUPLE) eval_body(buf, s, env);
    else { memcpy(&stack[sp], buf, s*sizeof(Value)); sp += s; }
}

static void prim_if(Frame *env) {
    Value el_top=stack[sp-1]; int el_s=val_slots(el_top);
    int then_end=sp-el_s; Value then_top=stack[then_end-1];
    if(then_top.tag!=VAL_TUPLE) die("if: then branch must be tuple");
    int then_s=val_slots(then_top), cond_end=then_end-then_s;
    Value cond_top=stack[cond_end-1];
    if (cond_top.tag == VAL_INT) {
        if (cond_top.as.i) { Value then_buf[then_s]; memcpy(then_buf,&stack[then_end-then_s],then_s*sizeof(Value)); sp=cond_end-1; eval_body(then_buf,then_s,env); }
        else { Value el_buf[el_s]; memcpy(el_buf,&stack[sp-el_s],el_s*sizeof(Value)); sp=cond_end-1; eval_default(el_buf,el_s,el_top,NULL,0,env); }
    } else if (cond_top.tag == VAL_TUPLE) {
        Value el_buf[el_s]; memcpy(el_buf,&stack[sp-el_s],el_s*sizeof(Value)); sp-=el_s;
        Value then_buf[then_s]; memcpy(then_buf,&stack[sp-then_s],then_s*sizeof(Value)); sp-=then_s;
        int cond_s2=val_slots(cond_top); Value cond_buf[cond_s2]; memcpy(cond_buf,&stack[sp-cond_s2],cond_s2*sizeof(Value)); sp-=cond_s2;
        Value val_t=stack[sp-1]; int val_s2=val_slots(val_t); Value val_save[val_s2]; memcpy(val_save,&stack[sp-val_s2],val_s2*sizeof(Value));
        eval_body(cond_buf,cond_s2,env); int64_t cond=pop_int();
        memcpy(&stack[sp],val_save,val_s2*sizeof(Value)); sp+=val_s2;
        if(cond) eval_body(then_buf,then_s,env); else eval_default(el_buf,el_s,el_top,NULL,0,env);
    } else die("if: condition must be int or tuple, got tag %d", cond_top.tag);
}

static void prim_cond(Frame *env) {
    POP_VAL(def);
    Value clauses_top=stack[sp-1];
    if(clauses_top.tag!=VAL_TUPLE&&clauses_top.tag!=VAL_RECORD) die("cond: expected tuple of clauses");
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
    if(top.tag!=VAL_TUPLE&&top.tag!=VAL_LIST&&top.tag!=VAL_RECORD) die("size: expected compound");
    sp-=val_slots(top); spush(val_int((int)top.as.compound.len));
}

static void prim_push_op(Frame *e) {
    (void)e; POP_VAL(v); Value ct=stack[sp-1];
    if(ct.tag!=VAL_TUPLE&&ct.tag!=VAL_LIST&&ct.tag!=VAL_RECORD) die("push: expected compound");
    ValTag tag=ct.tag; int cs=val_slots(ct),cl=(int)ct.as.compound.len; sp--;
    memcpy(&stack[sp],v_buf,v_s*sizeof(Value)); sp+=v_s;
    spush(val_compound(tag,cl+1,cs+v_s));
}

static void prim_pop_impl(const char *label) {
    Value top=speek();
    if(top.tag!=VAL_TUPLE&&top.tag!=VAL_LIST&&top.tag!=VAL_RECORD) die("%s: expected compound", label);
    ValTag tag=top.tag; int s=val_slots(top),len=(int)top.as.compound.len;
    if(len==0) die("%s: empty %s", label, tag==VAL_LIST?"list":tag==VAL_TUPLE?"tuple":"record");
    int base=sp-s; ElemRef last=compound_elem(&stack[base],s,len,len-1);
    Value elem_buf[LOCAL_MAX]; memcpy(elem_buf,&stack[base+last.base],last.slots*sizeof(Value));
    sp--; sp-=last.slots; spush(val_compound(tag,len-1,s-last.slots));
    memcpy(&stack[sp],elem_buf,last.slots*sizeof(Value)); sp+=last.slots;
}
static void prim_pop_op(Frame *e){(void)e;prim_pop_impl("pop");}

static void prim_pull(Frame *e) {
    (void)e; int64_t idx=pop_int(); Value top=speek();
    if(top.tag!=VAL_TUPLE&&top.tag!=VAL_LIST&&top.tag!=VAL_RECORD) die("pull: expected compound");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    ElemRef ref=compound_elem(&stack[base],s,len,(int)idx);
    Value eb[LOCAL_MAX]; memcpy(eb,&stack[base+ref.base],ref.slots*sizeof(Value));
    memcpy(&stack[sp],eb,ref.slots*sizeof(Value)); sp+=ref.slots;
}

static void prim_replace_at(Frame *e) {
    (void)e; POP_VAL(v); int64_t idx=pop_int(); Value top=speek();
    if(top.tag!=VAL_TUPLE&&top.tag!=VAL_LIST&&top.tag!=VAL_RECORD) die("put: expected compound");
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
    if(top2.tag!=VAL_TUPLE&&top2.tag!=VAL_LIST&&top2.tag!=VAL_RECORD) die("concat: expected compound");
    ValTag tag=top2.tag; int s2=val_slots(top2),len2=(int)top2.as.compound.len,base2=sp-s2;
    Value below=stack[base2-1];
    if(below.tag!=VAL_TUPLE&&below.tag!=VAL_LIST&&below.tag!=VAL_RECORD) die("concat: expected compound");
    int s1=val_slots(below),len1=(int)below.as.compound.len,base1=base2-s1;
    int new_elem_slots=(s1-1)+(s2-1); Value tmp[LOCAL_MAX];
    memcpy(tmp,&stack[base1],(s1-1)*sizeof(Value));
    memcpy(&tmp[s1-1],&stack[base2],(s2-1)*sizeof(Value));
    sp=base1; memcpy(&stack[sp],tmp,new_elem_slots*sizeof(Value)); sp+=new_elem_slots;
    spush(val_compound(tag,len1+len2,new_elem_slots+1));
}

static void prim_list(Frame *e){(void)e;spush(val_compound(VAL_LIST,0,1));}
static void prim_grab(Frame *e){(void)e;prim_pop_impl("grab");}

static void prim_get(Frame *e) {
    (void)e; int64_t idx=pop_int(); Value top=speek();
    if(top.tag!=VAL_TUPLE&&top.tag!=VAL_LIST&&top.tag!=VAL_RECORD) die("get: expected compound");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    ElemRef ref=compound_elem(&stack[base],s,len,(int)idx);
    Value eb[LOCAL_MAX]; memcpy(eb,&stack[base+ref.base],ref.slots*sizeof(Value));
    sp-=s; memcpy(&stack[sp],eb,ref.slots*sizeof(Value)); sp+=ref.slots;
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

static void prim_reduce(Frame *env) {
    POP_BODY(fn,"reduce"); if(stack[sp-1].tag!=VAL_LIST) die("reduce: expected list");
    if((int)stack[sp-1].as.compound.len==0) die("reduce: empty list");
    POP_LIST_BUF(list,"reduce");
    int offs[LOCAL_MAX],szs[LOCAL_MAX]; compute_offsets(list_buf,list_s,list_len,offs,szs);
    memcpy(&stack[sp],&list_buf[offs[0]],szs[0]*sizeof(Value));sp+=szs[0];
    for(int i=1;i<list_len;i++){memcpy(&stack[sp],&list_buf[offs[i]],szs[i]*sizeof(Value));sp+=szs[i];eval_body(fn_buf,fn_s,env);}
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
static void prim_sort(Frame *e){(void)e;Value top=speek();if(top.tag!=VAL_LIST)die("sort: expected list");int s=val_slots(top),len=(int)top.as.compound.len;qsort(&stack[sp-s],len,sizeof(Value),sort_cmp);}

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

static void prim_keep_mask(Frame *e) {
    (void)e; POP_LIST_BUF(mask,"keep-mask"); POP_LIST_BUF(list,"keep-mask");
    if(list_len!=mask_len) die("keep-mask: list and mask must have same length");
    int mo[LOCAL_MAX],ms[LOCAL_MAX],lo[LOCAL_MAX],ls2[LOCAL_MAX];
    compute_offsets(mask_buf,mask_s,mask_len,mo,ms);
    compute_offsets(list_buf,list_s,list_len,lo,ls2);
    int rb=sp,rc=0;
    for(int i=0;i<list_len;i++){
        Value mv=mask_buf[mo[i]]; if(mv.tag!=VAL_INT) die("keep-mask: mask elements must be int");
        if(mv.as.i){memcpy(&stack[sp],&list_buf[lo[i]],ls2[i]*sizeof(Value));sp+=ls2[i];rc++;}
    }
    spush(val_compound(VAL_LIST,rc,sp-rb+1));
}

static void prim_at(Frame *env) {
    (void)env; POP_VAL(top1);
    if(sp<=0) die("at: stack underflow"); Value next=stack[sp-1];
    if(top1_top.tag==VAL_SYM&&next.tag==VAL_RECORD) {
        uint32_t key=top1_top.as.sym; int s=val_slots(next),len=(int)next.as.compound.len,base=sp-s;
        int found; ElemRef ref=record_field(&stack[base],s,len,key,&found);
        if(!found) die("at: key '%s' not found in record",sym_name(key));
        Value vb[LOCAL_MAX]; memcpy(vb,&stack[base+ref.base],ref.slots*sizeof(Value));
        sp-=s; memcpy(&stack[sp],vb,ref.slots*sizeof(Value)); sp+=ref.slots;
    } else {
        if(next.tag!=VAL_INT) die("at: expected int index");
        int64_t idx=next.as.i; sp--;
        Value lt=stack[sp-1]; if(lt.tag!=VAL_LIST) die("at: expected list");
        int s=val_slots(lt),len=(int)lt.as.compound.len,base=sp-s;
        if(idx<0||idx>=len){sp-=s;memcpy(&stack[sp],top1_buf,top1_s*sizeof(Value));sp+=top1_s;}
        else{ElemRef ref=compound_elem(&stack[base],s,len,(int)idx);Value vb[LOCAL_MAX];memcpy(vb,&stack[base+ref.base],ref.slots*sizeof(Value));sp-=s;memcpy(&stack[sp],vb,ref.slots*sizeof(Value));sp+=ref.slots;}
    }
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
        int elem_end=rec_s-1,replaced=0;
        if(rec_len>LOCAL_MAX) die("into: record too large");
        int kpos[LOCAL_MAX],voff[LOCAL_MAX],vsz[LOCAL_MAX];
        for(int i=rec_len-1;i>=0;i--){
            int lp=elem_end-1; Value l=stack[rec_base+lp];
            int sz=(l.tag==VAL_TUPLE||l.tag==VAL_LIST||l.tag==VAL_RECORD)?(int)l.as.compound.slots:1;
            voff[i]=elem_end-sz; vsz[i]=sz; kpos[i]=voff[i]-1; elem_end=kpos[i];
        }
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

static void prim_rotate(Frame *e) {
    (void)e; int64_t n=pop_int(); Value top=speek(); if(top.tag!=VAL_LIST) die("rotate: expected list");
    int s=val_slots(top),len=(int)top.as.compound.len; if(len==0) return;
    int base=sp-s; n=((n%len)+len)%len; if(n==0) return;
    if(len>LOCAL_MAX) die("rotate: list too large");
    Value tmp[LOCAL_MAX]; int split=len-(int)n;
    memcpy(tmp,&stack[base+split],(int)n*sizeof(Value));
    memcpy(&tmp[n],&stack[base],split*sizeof(Value));
    memcpy(&stack[base],tmp,len*sizeof(Value));
}

static void prim_select(Frame *e) {
    (void)e; POP_LIST_BUF(idx,"select"); POP_LIST_BUF(list,"select");
    int rb=sp,rc=0;
    for(int i=0;i<idx_len;i++){
        ElemRef ir=compound_elem(idx_buf,idx_s,idx_len,i); Value iv=idx_buf[ir.base];
        if(iv.tag!=VAL_INT) die("select: index must be int");
        ElemRef lr=compound_elem(list_buf,list_s,list_len,(int)iv.as.i);
        memcpy(&stack[sp],&list_buf[lr.base],lr.slots*sizeof(Value)); sp+=lr.slots; rc++;
    }
    spush(val_compound(VAL_LIST,rc,sp-rb+1));
}

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

static void prim_windows(Frame *e) {
    (void)e; int64_t n=pop_int(); Value top=speek(); if(top.tag!=VAL_LIST) die("windows: expected list");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    Value buf[LOCAL_MAX]; memcpy(buf,&stack[base],s*sizeof(Value)); sp=base;
    if((int)n>len||n<=0){spush(val_compound(VAL_LIST,0,1));return;}
    int rb=sp,wc=len-(int)n+1;
    for(int i=0;i<wc;i++){int ws=0;for(int j=0;j<(int)n;j++){ElemRef r=compound_elem(buf,s,len,i+j);memcpy(&stack[sp],&buf[r.base],r.slots*sizeof(Value));sp+=r.slots;ws+=r.slots;}spush(val_compound(VAL_LIST,(int)n,ws+1));}
    spush(val_compound(VAL_LIST,wc,sp-rb+1));
}

static void prim_reshape(Frame *e) {
    (void)e; Value dt=stack[sp-1]; if(dt.tag!=VAL_LIST) die("reshape: expected dims list");
    int ds=val_slots(dt),dl=(int)dt.as.compound.len,db=sp-ds;
    if(dl!=2) die("reshape: only 2D reshape supported");
    ElemRef d0=compound_elem(&stack[db],ds,dl,0),d1=compound_elem(&stack[db],ds,dl,1);
    if(stack[db+d0.base].tag!=VAL_INT||stack[db+d1.base].tag!=VAL_INT) die("reshape: dimensions must be int");
    int rows=(int)stack[db+d0.base].as.i,cols=(int)stack[db+d1.base].as.i;
    if(rows<0||cols<0) die("reshape: dimensions must be non-negative");
    if(rows>0&&cols>INT32_MAX/rows) die("reshape: dimension overflow");
    sp-=ds; Value lt=stack[sp-1]; if(lt.tag!=VAL_LIST) die("reshape: expected list");
    int ls=val_slots(lt),ll=(int)lt.as.compound.len,lb=sp-ls;
    Value lbuf[LOCAL_MAX]; memcpy(lbuf,&stack[lb],ls*sizeof(Value)); sp=lb;
    if(rows*cols!=ll) die("reshape: dimension mismatch");
    int rb=sp;
    for(int r=0;r<rows;r++){for(int c=0;c<cols;c++){ElemRef er=compound_elem(lbuf,ls,ll,r*cols+c);memcpy(&stack[sp],&lbuf[er.base],er.slots*sizeof(Value));sp+=er.slots;}spush(val_compound(VAL_LIST,cols,cols+1));}
    spush(val_compound(VAL_LIST,rows,sp-rb+1));
}

static void prim_transpose(Frame *e) {
    (void)e; Value top=speek(); if(top.tag!=VAL_LIST) die("transpose: expected list");
    int s=val_slots(top),rows=(int)top.as.compound.len,base=sp-s; if(rows==0) return;
    ElemRef r0=compound_elem(&stack[base],s,rows,0); Value row0_top=stack[base+r0.base+r0.slots-1];
    if(row0_top.tag!=VAL_LIST) die("transpose: expected list of lists");
    int cols=(int)row0_top.as.compound.len;
    if(s>LOCAL_MAX) die("transpose: matrix too large");
    Value buf[LOCAL_MAX]; memcpy(buf,&stack[base],s*sizeof(Value)); sp=base;
    if(rows>0&&cols>INT32_MAX/rows) die("transpose: dimension overflow");
    double *flat=malloc(rows*cols*sizeof(double)); int all_int=1;
    for(int r=0;r<rows;r++){
        ElemRef rr=compound_elem(buf,s,rows,r); Value *rd=&buf[rr.base]; Value rh=rd[rr.slots-1];
        if(rh.tag!=VAL_LIST) die("transpose: row %d is not a list",r);
        if((int)rh.as.compound.len!=cols) die("transpose: ragged matrix (row %d has %d cols, expected %d)",r,(int)rh.as.compound.len,cols);
        for(int c=0;c<cols;c++){ElemRef cell=compound_elem(rd,rr.slots,cols,c);Value cv=rd[cell.base];
            if(cv.tag==VAL_INT) flat[r*cols+c]=(double)cv.as.i;
            else if(cv.tag==VAL_FLOAT){flat[r*cols+c]=cv.as.f;all_int=0;}
            else die("transpose: only scalar matrices supported");}
    }
    int rb=sp;
    for(int c=0;c<cols;c++){for(int r=0;r<rows;r++){if(all_int)spush(val_int((int64_t)flat[r*cols+c]));else spush(val_float(flat[r*cols+c]));}spush(val_compound(VAL_LIST,rows,rows+1));}
    spush(val_compound(VAL_LIST,cols,sp-rb+1)); free(flat);
}

static void prim_shape(Frame *e) {
    (void)e; Value top=speek(); if(top.tag!=VAL_LIST) die("shape: expected list");
    int len=(int)top.as.compound.len,s=val_slots(top),base=sp-s;
    int dims[16],ndims=0; dims[ndims++]=len;
    if(len>0){ElemRef r0=compound_elem(&stack[base],s,len,0);Value e0=stack[base+r0.base+r0.slots-1];if(e0.tag==VAL_LIST)dims[ndims++]=(int)e0.as.compound.len;}
    sp-=s; for(int i=0;i<ndims;i++) spush(val_int(dims[i])); spush(val_compound(VAL_LIST,ndims,ndims+1));
}

static void prim_classify(Frame *e) {
    (void)e; Value top=speek(); if(top.tag!=VAL_LIST) die("classify: expected list");
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    Value uniques[LOCAL_MAX]; int uc=0; int *classes=malloc(len*sizeof(int));
    for(int i=0;i<len;i++){
        ElemRef r=compound_elem(&stack[base],s,len,i); int found=-1;
        for(int j=0;j<uc;j++) if(val_equal(&stack[base+r.base],r.slots,&uniques[j],1)){found=j;break;}
        if(found>=0) classes[i]=found;
        else{classes[i]=uc;if(uc>=LOCAL_MAX)die("classify: too many unique values");uniques[uc++]=stack[base+r.base];}
    }
    sp-=s; for(int i=0;i<len;i++) spush(val_int(classes[i])); spush(val_compound(VAL_LIST,len,len+1)); free(classes);
}

static void prim_group(Frame *e) {
    (void)e; Value it=stack[sp-1]; if(it.tag!=VAL_LIST) die("group: expected index list");
    int is=val_slots(it),il=(int)it.as.compound.len,ib=sp-is;
    Value lt=stack[ib-1]; if(lt.tag!=VAL_LIST) die("group: expected list");
    int ls=val_slots(lt),ll=(int)lt.as.compound.len,lb=ib-ls;
    if(il!=ll) die("group: list and indices must have same length");
    Value ibuf[LOCAL_MAX],lbuf[LOCAL_MAX]; memcpy(ibuf,&stack[ib],is*sizeof(Value)); memcpy(lbuf,&stack[lb],ls*sizeof(Value)); sp=lb;
    int mg=-1;
    for(int i=0;i<il;i++){ElemRef ir=compound_elem(ibuf,is,il,i);int g=(int)ibuf[ir.base].as.i;if(g>mg)mg=g;}
    int rb=sp,gc=mg+1;
    for(int g=0;g<gc;g++){int gb=sp,grc=0;
        for(int i=0;i<il;i++){ElemRef ir=compound_elem(ibuf,is,il,i);if((int)ibuf[ir.base].as.i==g){ElemRef lr=compound_elem(lbuf,ls,ll,i);memcpy(&stack[sp],&lbuf[lr.base],lr.slots*sizeof(Value));sp+=lr.slots;grc++;}}
        spush(val_compound(VAL_LIST,grc,sp-gb+1));
    }
    spush(val_compound(VAL_LIST,gc,sp-rb+1));
}

/* ---- evaluator ---- */
static void dispatch_word(uint32_t sym, Frame *env) {
    PrimFn fn=prim_lookup(sym); if(fn){fn(env);return;}
    Lookup lu=frame_lookup(env,sym); if(!lu.found) die("unknown word: %s",sym_name(sym));
    if(lu.kind==BIND_DEF){Value bt=lu.frame->vals[lu.offset+lu.slots-1];if(bt.tag==VAL_TUPLE){eval_body(&lu.frame->vals[lu.offset],lu.slots,env);return;}}
    memcpy(&stack[sp],&lu.frame->vals[lu.offset],lu.slots*sizeof(Value)); sp+=lu.slots;
}

static void eval_body(Value *body, int slots, Frame *env) {
    Value hdr=body[slots-1]; if(hdr.tag!=VAL_TUPLE) die("eval_body: expected tuple");
    int len=(int)hdr.as.compound.len;
    Frame *exec_env=hdr.as.compound.env?hdr.as.compound.env:env;
    int saved_bc=exec_env->bind_count,saved_vu=exec_env->vals_used;
    static uint32_t sym_def=0,sym_let=0,sym_recur_kw=0;
    if(!sym_def){sym_def=sym_intern("def");sym_let=sym_intern("let");sym_recur_kw=sym_intern("recur");}
    int all_scalar=(slots==len+1);
    if(len>LOCAL_MAX) die("tuple body too large");
    int offsets_buf[LOCAL_MAX],sizes_buf[LOCAL_MAX];
    if(!all_scalar) compute_offsets(body,slots,len,offsets_buf,sizes_buf);
    for(int k=0;k<len;k++){
        int eoff,esz; if(all_scalar){eoff=k;esz=1;}else{eoff=offsets_buf[k];esz=sizes_buf[k];}
        Value elem=body[eoff+esz-1];
        if(elem.tag==VAL_INT||elem.tag==VAL_FLOAT||elem.tag==VAL_SYM) stack[sp++]=elem;
        else if(elem.tag==VAL_TUPLE||elem.tag==VAL_LIST||elem.tag==VAL_RECORD){
            memcpy(&stack[sp],&body[eoff],esz*sizeof(Value)); sp+=esz;
            if(elem.tag==VAL_TUPLE){exec_env->refcount++;stack[sp-1].as.compound.env=exec_env;}
        } else if(elem.tag==VAL_WORD){
            uint32_t sym=elem.as.sym;
            if(sym==sym_def){
                Value dv_top=stack[sp-1]; int dv_s=val_slots(dv_top); uint32_t name; int rec=0;
                if(recur_pending){name=recur_sym;rec=1;recur_pending=0;frame_bind(exec_env,name,&stack[sp-dv_s],dv_s,BIND_DEF,rec);sp-=dv_s;}
                else{Value dv_buf[LOCAL_MAX];memcpy(dv_buf,&stack[sp-dv_s],dv_s*sizeof(Value));sp-=dv_s;name=pop_sym();frame_bind(exec_env,name,dv_buf,dv_s,BIND_DEF,rec);}
            } else if(sym==sym_let){
                uint32_t name=pop_sym(); Value lv_top=stack[sp-1]; int lv_s=val_slots(lv_top);
                frame_bind(exec_env,name,&stack[sp-lv_s],lv_s,BIND_LET,0); sp-=lv_s;
            } else if(sym==sym_recur_kw){recur_sym=pop_sym();recur_pending=1;}
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
    if(!sym_check_kw) sym_check_kw=sym_intern("check");
    if(!sym_effect_kw) sym_effect_kw=sym_intern("effect");
    int elem_base=sp,elem_count=0;
    for(int j=start;j<end;j++){
        Token *tt=&toks[j];
        switch(tt->tag){
        case TOK_INT: spush(val_int(tt->as.i)); elem_count++; break;
        case TOK_FLOAT: spush(val_float(tt->as.f)); elem_count++; break;
        case TOK_SYM: spush(val_sym(tt->as.sym)); elem_count++; break;
        case TOK_WORD:
            if(tt->as.sym==sym_check_kw){if(elem_count>0&&stack[sp-1].tag==VAL_WORD){sp--;elem_count--;}}
            else{spush(val_word(tt->as.sym));elem_count++;}
            break;
        case TOK_STRING:
            for(int c=0;c<tt->as.str.len;c++) spush(val_int(tt->as.str.codes[c]));
            spush(val_compound(VAL_LIST,tt->as.str.len,tt->as.str.len+1)); elem_count++; break;
        case TOK_LPAREN:{int nc=find_matching(toks,j+1,total_count,TOK_LPAREN,TOK_RPAREN);build_tuple(toks,j+1,nc,total_count,env);elem_count++;j=nc;break;}
        case TOK_LBRACKET:{
            int bc=find_matching(toks,j+1,total_count,TOK_LBRACKET,TOK_RBRACKET);
            if(bc+1<total_count&&toks[bc+1].tag==TOK_WORD&&toks[bc+1].as.sym==sym_effect_kw){j=bc+1;break;}
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
    if(!sym_effect_kw) sym_effect_kw=sym_intern("effect");
    int base=sp; build_tuple(toks,0,count,count,env);
    int s=val_slots(stack[sp-1]); Value *body=malloc(s*sizeof(Value));
    memcpy(body,&stack[base],s*sizeof(Value)); sp=base; eval_body(body,s,env); free(body);
}

static void prim_bi(Frame *env){POP_BODY(g,"bi");POP_BODY(f,"bi");POP_VAL(x);memcpy(&stack[sp],x_buf,x_s*sizeof(Value));sp+=x_s;eval_body(f_buf,f_s,env);memcpy(&stack[sp],x_buf,x_s*sizeof(Value));sp+=x_s;eval_body(g_buf,g_s,env);}
static void prim_keep(Frame *env){POP_BODY(f,"keep");POP_VAL(x);memcpy(&stack[sp],x_buf,x_s*sizeof(Value));sp+=x_s;eval_body(f_buf,f_s,env);memcpy(&stack[sp],x_buf,x_s*sizeof(Value));sp+=x_s;}

static void prim_repeat_op(Frame *env) {
    if(stack[sp-1].tag!=VAL_TUPLE) die("repeat: expected tuple");
    int f_s=val_slots(stack[sp-1]); if(f_s>LOCAL_MAX||f_s>sp) die("repeat: stack underflow");
    Value f_buf[f_s]; memcpy(f_buf,&stack[sp-f_s],f_s*sizeof(Value)); sp-=f_s;
    int64_t n=pop_int(); for(int64_t i=0;i<n;i++) eval_body(f_buf,f_s,env);
}

static void prim_zip(Frame *e) {
    (void)e; POP_LIST_BUF(b,"zip"); POP_LIST_BUF(a,"zip");
    int n=a_len<b_len?a_len:b_len,rb=sp;
    for(int i=0;i<n;i++){
        ElemRef r1=compound_elem(a_buf,a_s,a_len,i),r2=compound_elem(b_buf,b_s,b_len,i);
        memcpy(&stack[sp],&a_buf[r1.base],r1.slots*sizeof(Value));sp+=r1.slots;
        memcpy(&stack[sp],&b_buf[r2.base],r2.slots*sizeof(Value));sp+=r2.slots;
        spush(val_compound(VAL_LIST,2,r1.slots+r2.slots+1));
    }
    spush(val_compound(VAL_LIST,n,sp-rb+1));
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

static void prim_table(Frame *env) {
    POP_BODY(fn,"table"); POP_LIST_BUF(list,"table");
    int rb=sp;
    for(int i=0;i<list_len;i++){
        ElemRef r=compound_elem(list_buf,list_s,list_len,i);
        memcpy(&stack[sp],&list_buf[r.base],r.slots*sizeof(Value));sp+=r.slots;
        memcpy(&stack[sp],&list_buf[r.base],r.slots*sizeof(Value));sp+=r.slots;
        eval_body(fn_buf,fn_s,env);
        Value res_top=stack[sp-1]; int res_s=val_slots(res_top);
        spush(val_compound(VAL_LIST,2,r.slots+res_s+1));
    }
    spush(val_compound(VAL_LIST,list_len,sp-rb+1));
}

#ifndef SLAP_SDL
static void prim_millis(Frame *e){(void)e;struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);spush(val_int((int64_t)(ts.tv_sec*1000+ts.tv_nsec/1000000)));}
#endif

static const char *BUILTIN_TYPES =
    "'dup ['a copy in  'a copy out  'a copy out] effect\n"
    "'drop ['a copy in] effect\n"
    "'swap ['a own in  'b own in  'b own out  'a own out] effect\n"
    "'plus ['a num lent in  'a num lent in  'a num move out] effect\n"
    "'sub ['a num lent in  'a num lent in  'a num move out] effect\n"
    "'mul ['a num lent in  'a num lent in  'a num move out] effect\n"
    "'div ['a num lent in  'a num lent in  'a num move out] effect\n"
    "'mod [int lent in  int lent in  int move out] effect\n"
    "'eq [lent in  lent in  int move out] effect\n"
    "'lt [lent in  lent in  int move out] effect\n"
    "'not [int lent in  int move out] effect\n"
    "'and [int lent in  int lent in  int move out] effect\n"
    "'or [int lent in  int lent in  int move out] effect\n"
    "'neq [lent in  lent in  int move out] effect\n"
    "'gt [lent in  lent in  int move out] effect\n"
    "'ge [lent in  lent in  int move out] effect\n"
    "'le [lent in  lent in  int move out] effect\n"
    "'inc [num lent in  num move out] effect\n"
    "'dec [num lent in  num move out] effect\n"
    "'neg [num lent in  num move out] effect\n"
    "'abs [num lent in  num move out] effect\n"
    "'iseven [int lent in  int move out] effect\n"
    "'isodd [int lent in  int move out] effect\n"
    "'iszero [int lent in  int move out] effect\n"
    "'max ['a num lent in  'a num lent in  'a num move out] effect\n"
    "'min ['a num lent in  'a num lent in  'a num move out] effect\n"
    "'divides [int lent in  int lent in  int move out] effect\n"
    "'print [own in] effect\n"
    "'assert [int own in] effect\n"
    "'millis [int move out] effect\n"
    "'itof [int lent in  float move out] effect\n"
    "'ftoi [float lent in  int move out] effect\n"
    "'fsqrt [float lent in  float move out] effect\n"
    "'fsin [float lent in  float move out] effect\n"
    "'fcos [float lent in  float move out] effect\n"
    "'ftan [float lent in  float move out] effect\n"
    "'ffloor [float lent in  float move out] effect\n"
    "'fceil [float lent in  float move out] effect\n"
    "'fround [float lent in  float move out] effect\n"
    "'fexp [float lent in  float move out] effect\n"
    "'flog [float lent in  float move out] effect\n"
    "'fpow [float lent in  float lent in  float move out] effect\n"
    "'fatan2 [float lent in  float lent in  float move out] effect\n"
    "'list [list move out] effect\n"
    "'len [list lent in  int move out] effect\n"
    "'give [list own in  own in  list move out] effect\n"
    "'grab [list own in  list move out  move out] effect\n"
    "'get [list own in  int lent in  move out] effect\n"
    "'set [list own in  int lent in  own in  list move out] effect\n"
    "'cat [list own in  list own in  list move out] effect\n"
    "'take-n [list own in  int lent in  list move out] effect\n"
    "'drop-n [list own in  int lent in  list move out] effect\n"
    "'range [int lent in  int lent in  list move out] effect\n"
    "'sort [list own in  list move out] effect\n"
    "'reverse [list own in  list move out] effect\n"
    "'dedup [list own in  list move out] effect\n"
    "'index-of [list own in  lent in  int move out] effect\n"
    "'keep-mask [list own in  list own in  list move out] effect\n"
    "'select [list own in  list own in  list move out] effect\n"
    "'pick [list own in  list own in  list move out] effect\n"
    "'rise [list own in  list move out] effect\n"
    "'fall [list own in  list move out] effect\n"
    "'classify [list own in  list move out] effect\n"
    "'shape [list own in  list move out] effect\n"
    "'rotate [list own in  int lent in  list move out] effect\n"
    "'windows [list own in  int lent in  list move out] effect\n"
    "'zip [list own in  list own in  list move out] effect\n"
    "'group [list own in  list own in  list move out] effect\n"
    "'partition [list own in  list own in  list move out] effect\n"
    "'reshape [list own in  list own in  list move out] effect\n"
    "'transpose [list own in  list move out] effect\n"
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
    "'box [own in  box move out] effect\n"
    "'free [box own in] effect\n"
    "'clear [int lent in] effect\n"
    "'pixel [int lent in  int lent in  int lent in] effect\n";

static const char *PRELUDE =
    "'over (swap dup (swap) dip) def\n'peek (over) def\n'nip (swap drop) def\n"
    "'rot ((swap) dip swap) def\n'sqr (dup mul) def\n'cube (dup dup mul mul) def\n"
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
    "3.14159265358979323846 'pi let\n6.28318530717958647692 'tau let\n2.71828182845904523536 'e let\n";

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
    uint8_t pixels[CANVAS_W*CANVAS_H*3];
    for(int i=0;i<CANVAS_W*CANVAS_H;i++){uint8_t g=gray_lut[canvas[i]&3];pixels[i*3]=pixels[i*3+1]=pixels[i*3+2]=g;}
    SDL_UpdateTexture(sdl_texture,NULL,pixels,CANVAS_W*3);
    SDL_RenderClear(sdl_renderer);SDL_RenderCopy(sdl_renderer,sdl_texture,NULL,NULL);SDL_RenderPresent(sdl_renderer);
}
static void prim_clear(Frame *e){(void)e;memset(canvas,(int)(pop_int()&3),sizeof(canvas));}
static void prim_pixel(Frame *e){(void)e;int64_t color=pop_int(),y=pop_int(),x=pop_int();if(x>=0&&x<CANVAS_W&&y>=0&&y<CANVAS_H)canvas[y*CANVAS_W+x]=(uint8_t)(color&3);}
static void prim_millis(Frame *e){(void)e;spush(val_int((int64_t)SDL_GetTicks()));}
static void prim_on(Frame *e) {
    (void)e; Value fn_top=stack[sp-1]; if(fn_top.tag!=VAL_TUPLE) die("on: expected tuple handler");
    int fn_s=val_slots(fn_top); if(handler_count>=MAX_HANDLERS) die("on: too many event handlers");
    memcpy(event_handlers[handler_count].handler_body,&stack[sp-fn_s],fn_s*sizeof(Value));
    event_handlers[handler_count].handler_slots=fn_s; sp-=fn_s;
    event_handlers[handler_count].event_sym=pop_sym(); handler_count++;
}
static void prim_show(Frame *env) {
    Value fn_top=stack[sp-1]; if(fn_top.tag!=VAL_TUPLE) die("show: expected tuple render function");
    render_slots=val_slots(fn_top); memcpy(render_body,&stack[sp-render_slots],render_slots*sizeof(Value)); sp-=render_slots;
    sdl_init();
    static uint32_t sym_tick=0,sym_keydown=0;
    if(!sym_tick){sym_tick=sym_intern("tick");sym_keydown=sym_intern("keydown");}
    int64_t frame=0; int running=1;
    while(running){
        SDL_Event ev;
        while(SDL_PollEvent(&ev)){
            if(ev.type==SDL_QUIT){running=0;break;}
            if(ev.type==SDL_KEYDOWN) for(int h=0;h<handler_count;h++)
                if(event_handlers[h].event_sym==sym_keydown){spush(val_int((int64_t)ev.key.keysym.sym));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}
        }
        for(int h=0;h<handler_count;h++) if(event_handlers[h].event_sym==sym_tick){spush(val_int(frame));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}
        if(render_slots>0){Value mt=stack[sp-1];int ms=val_slots(mt);memcpy(&stack[sp],&stack[sp-ms],ms*sizeof(Value));sp+=ms;eval_body(render_body,render_slots,env);}
        sdl_present(); frame++;
        if(sdl_test_mode) break; SDL_Delay(16);
    }
    SDL_DestroyTexture(sdl_texture);SDL_DestroyRenderer(sdl_renderer);SDL_DestroyWindow(sdl_window);SDL_Quit();exit(0);
}
#endif

static void register_prims(void) {
    static struct{const char*n;PrimFn f;} t[]={
        {"dup",prim_dup},{"drop",prim_drop},{"swap",prim_swap},{"dip",prim_dip},{"apply",prim_apply},
        {"plus",prim_plus},{"sub",prim_sub},{"mul",prim_mul},{"div",prim_div},{"mod",prim_mod},
        {"eq",prim_eq},{"lt",prim_lt},{"not",prim_not},{"and",prim_and},{"or",prim_or},
        {"neq",prim_neq},{"gt",prim_gt},{"ge",prim_ge},{"le",prim_le},
        {"inc",prim_inc},{"dec",prim_dec},{"neg",prim_neg},{"abs",prim_abs_op},
        {"iseven",prim_iseven},{"isodd",prim_isodd},{"iszero",prim_iszero},
        {"max",prim_max},{"min",prim_min},{"divides",prim_divides},
        {"print",prim_print},{"assert",prim_assert},{"halt",prim_halt},{"random",prim_random},
        {"if",prim_if},{"cond",prim_cond},{"match",prim_match},{"loop",prim_loop},{"while",prim_while},
        {"itof",prim_itof},{"ftoi",prim_ftoi},{"fsqrt",prim_fsqrt},{"fsin",prim_fsin},{"fcos",prim_fcos},
        {"ftan",prim_ftan},{"ffloor",prim_ffloor},{"fceil",prim_fceil},{"fround",prim_fround},
        {"fexp",prim_fexp},{"flog",prim_flog},{"fpow",prim_fpow},{"fatan2",prim_fatan2},
        {"stack",prim_stack},{"size",prim_size},{"push",prim_push_op},{"pop",prim_pop_op},
        {"pull",prim_pull},{"put",prim_replace_at},{"compose",prim_concat},
        {"list",prim_list},{"len",prim_size},{"give",prim_push_op},{"grab",prim_grab},
        {"get",prim_get},{"set",prim_replace_at},{"cat",prim_concat},
        {"take-n",prim_take_n},{"drop-n",prim_drop_n},{"range",prim_range},
        {"map",prim_map},{"filter",prim_filter},{"fold",prim_fold},{"reduce",prim_reduce},{"each",prim_each},
        {"sort",prim_sort},{"index-of",prim_index_of},{"scan",prim_scan},{"keep-mask",prim_keep_mask},
        {"at",prim_at},{"rotate",prim_rotate},{"select",prim_select},{"rise",prim_rise},{"fall",prim_fall},
        {"windows",prim_windows},{"pick",prim_select},{"reshape",prim_reshape},{"transpose",prim_transpose},
        {"shape",prim_shape},{"classify",prim_classify},{"group",prim_group},{"partition",prim_group},
        {"rec",prim_rec},{"into",prim_into},{"reverse",prim_reverse},{"dedup",prim_dedup},
        {"bi",prim_bi},{"keep",prim_keep},{"repeat",prim_repeat_op},{"zip",prim_zip},
        {"where",prim_where},{"find",prim_find_elem},{"table",prim_table},
        {"millis",prim_millis},{"box",prim_box},{"free",prim_free},
        {"lend",prim_lend},{"mutate",prim_mutate},{"clone",prim_clone},
#ifdef SLAP_SDL
        {"clear",prim_clear},{"pixel",prim_pixel},{"millis",prim_millis},{"on",prim_on},{"show",prim_show},
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
    if(!filename){fprintf(stderr,"usage: slap [--check] <file.slap>\n");return 1;}
    current_file=filename; register_prims(); register_builtin_types();
    Frame *global=frame_new(NULL);
    current_file="<prelude>"; lex(PRELUDE);
    int prelude_tok_count=tok_count;
    Token prelude_tokens[TOK_MAX]; memcpy(prelude_tokens,tokens,prelude_tok_count*sizeof(Token));
    eval(tokens,tok_count,global);
    current_file=filename;
    FILE *f=fopen(filename,"r"); if(!f){fprintf(stderr,"error: cannot open '%s'\n",filename);return 1;}
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *src=malloc(sz+1); if((long)fread(src,1,sz,f)!=sz){fprintf(stderr,"error: read failed\n");return 1;} src[sz]=0; fclose(f);
    lex(src); int user_tok_count=tok_count;
    static Token user_tokens[TOK_MAX]; memcpy(user_tokens,tokens,user_tok_count*sizeof(Token));
    if(dump_types){
        eval(user_tokens,user_tok_count,global);
        for(int i=0;i<type_sig_count;i++){
            TypeSig *s=&type_sigs[i].sig; printf("'%s type",sym_name(type_sigs[i].sym));
            if(s->is_todo) printf(" todo");
            else for(int j=0;j<s->slot_count;j++){
                TypeSlot *sl=&s->slots[j];
                if(sl->is_env){printf("  '%s '%s env",sym_name(sl->env_key_var),sym_name(sl->env_val_var));continue;}
                printf("  "); if(sl->type_var) printf("'%s ",sym_name(sl->type_var));
                if(sl->constraint!=TC_NONE) printf("%s ",constraint_name(sl->constraint));
                switch(sl->ownership){case OWN_OWN:printf("own ");break;case OWN_COPY:printf("copy ");break;case OWN_MOVE:printf("move ");break;case OWN_LENT:printf("lent ");break;}
                printf("%s",sl->direction==DIR_IN?"in":"out");
            }
            printf(" def\n");
        }
        free(src); frame_free(global); return 0;
    }
    lex(BUILTIN_TYPES); int types_tok_count=tok_count;
    static Token types_tokens[TOK_MAX]; memcpy(types_tokens,tokens,types_tok_count*sizeof(Token));
    int combined_count=types_tok_count+prelude_tok_count+user_tok_count;
    if(combined_count>TOK_MAX) die("too many tokens (types + prelude + user)");
    static Token combined[TOK_MAX];
    memcpy(combined,types_tokens,types_tok_count*sizeof(Token));
    memcpy(&combined[types_tok_count],prelude_tokens,prelude_tok_count*sizeof(Token));
    memcpy(&combined[types_tok_count+prelude_tok_count],user_tokens,user_tok_count*sizeof(Token));
    int errors=typecheck_tokens(combined,combined_count);
    if(errors>0){fprintf(stderr,"%d type error(s)\n",errors);free(src);frame_free(global);return 1;}
    if(check_only){fprintf(stderr,"type check passed\n");free(src);frame_free(global);return 0;}
    eval(user_tokens,user_tok_count,global);
    free(src); frame_free(global); return 0;
}
