// slap — a minimal concatenative language with linear types
// C99 library. Include from sdl.c or wasm.c.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>


// ── Limits ──────────────────────────────────────────────────────────────────

#define STACK_MAX    8192
#define MAX_SYMS     4096
#define MAX_NODES    131072
#define MAX_SLICES   65536
#define MAX_TUPLES   131072
#define MAX_RECS     16384
#define MAX_LISTS    16384
#define MAX_DICTS    4096
#define MAX_STRS     4096
#define MAX_HEAP     65536
#define MAX_SCOPES   262144
#define SYM_NAME_MAX 128
#define NONE         UINT32_MAX

// ── Forward declarations ────────────────────────────────────────────────────

typedef struct Val Val;
static void eval(int start, int len, uint32_t scope);
static void exec_tuple(Val t);
static Val val_clone(Val v);
static void val_free(Val v);
static void val_print(Val v, FILE *f);
static Val linear_snapshot(Val v);
static uint32_t scope_new(uint32_t parent);
static void scope_release(uint32_t sc);
static uint32_t scope_snapshot(uint32_t sc);

// ── Panic ───────────────────────────────────────────────────────────────────

static int panic_line = 0, panic_col = 0;
static const char *current_word = "";
static const char *user_src = NULL;
static bool use_color;
#define C_RESET (use_color ? "\033[0m"  : "")
#define C_RED   (use_color ? "\033[31m" : "")
#define C_BOLD  (use_color ? "\033[1m"  : "")
#define C_DIM   (use_color ? "\033[2m"  : "")
#define C_CYAN  (use_color ? "\033[36m" : "")

static void (*panic_stack_printer)(void);

static void get_src_line(int line, const char **start, int *len) {
    *start = NULL; *len = 0;
    if (!user_src || line < 1) return;
    const char *p = user_src;
    for (int i = 1; i < line; i++) {
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    *start = p;
    const char *eol = p;
    while (*eol && *eol != '\n') eol++;
    *len = (int)(eol - p);
}

__attribute__((noreturn, format(printf, 1, 2)))
static void slap_panic(const char *fmt, ...) {
    fprintf(stderr, "\n%s── SLAP PANIC ──────────────────────────────────────%s\n\n",
        C_RED, C_RESET);
    const char *src_line; int src_len;
    get_src_line(panic_line, &src_line, &src_len);
    if (src_line && src_len > 0 && panic_line > 0) {
        int gw = snprintf(NULL, 0, "%d", panic_line);
        fprintf(stderr, "  %s%d%s %s│%s %.*s\n",
            C_CYAN, panic_line, C_RESET, C_DIM, C_RESET, src_len, src_line);
        if (panic_col > 0 && current_word[0]) {
            int span = (int)strlen(current_word);
            fprintf(stderr, "  %*s %s│%s ", gw, "", C_DIM, C_RESET);
            for (int c = 1; c < panic_col; c++) fputc(' ', stderr);
            fprintf(stderr, "%s", C_RED);
            for (int c = 0; c < span; c++) fputc('^', stderr);
            fprintf(stderr, "%s\n", C_RESET);
        }
        fprintf(stderr, "\n");
    }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "  %s%s%s\n", C_BOLD, msg, C_RESET);
    if (panic_stack_printer) panic_stack_printer();
    fprintf(stderr, "\n");
    exit(1);
}

// ── Symbols ─────────────────────────────────────────────────────────────────

static char sym_names[MAX_SYMS][SYM_NAME_MAX];
static int sym_count;

static uint32_t sym_intern(const char *name) {
    for (int i = 0; i < sym_count; i++)
        if (strcmp(sym_names[i], name) == 0) return (uint32_t)i;
    if (sym_count >= MAX_SYMS) slap_panic("symbol table full");
    strncpy(sym_names[sym_count], name, SYM_NAME_MAX - 1);
    sym_names[sym_count][SYM_NAME_MAX - 1] = '\0';
    return (uint32_t)sym_count++;
}

// ── Value types ─────────────────────────────────────────────────────────────

typedef enum {
    T_INT, T_FLOAT, T_SYM,                    // scalar (copyable)
    T_TUPLE, T_REC, T_SLICE, T_DICE,         // compound copyable
    T_BOX, T_LIST, T_DICT, T_STR             // linear
} ValType;

struct Val {
    ValType type;
    union {
        int64_t ival;
        double fval;
        uint32_t sym;
        uint32_t tuple;
        uint32_t rec;
        uint32_t slice; // also used by T_DICE
        uint32_t box;
        uint32_t list;
        uint32_t dict;
        uint32_t str;
    };
};

#define VAL_INT(v)   ((Val){.type = T_INT,   .ival  = (v)})
#define VAL_FLOAT(v) ((Val){.type = T_FLOAT, .fval  = (v)})
#define VAL_SYM(v)   ((Val){.type = T_SYM,   .sym   = (v)})
#define VAL_TUPLE(v) ((Val){.type = T_TUPLE, .tuple = (v)})
#define VAL_REC(v)   ((Val){.type = T_REC,   .rec   = (v)})
#define VAL_SLICE(v) ((Val){.type = T_SLICE, .slice = (v)})
#define VAL_DICE(v)  ((Val){.type = T_DICE,  .slice = (v)})
#define VAL_BOX(v)   ((Val){.type = T_BOX,   .box   = (v)})
#define VAL_LIST(v)  ((Val){.type = T_LIST,  .list  = (v)})
#define VAL_DICT(v)  ((Val){.type = T_DICT,  .dict  = (v)})
#define VAL_STR(v)   ((Val){.type = T_STR,   .str   = (v)})

static const char *type_name(ValType t) {
    switch (t) {
        case T_INT: return "Int";
        case T_FLOAT: return "Float"; case T_SYM: return "Symbol";
        case T_TUPLE: return "Tuple"; case T_REC: return "Record";
        case T_SLICE: return "Slice"; case T_DICE: return "Dice";
        case T_BOX: return "Box"; case T_LIST: return "List";
        case T_DICT: return "Dict"; case T_STR: return "String";
    }
    return "?";
}

static bool val_is_linear(Val v) {
    return v.type >= T_BOX;
}

// ── Pool allocation ────────────────────────────────────────────────────────

#define POOL_DECL(type, name, max) \
    static type name[max]; \
    static int name##_count = 1, name##_free = -1; \
    static const int name##_max = max

#define POOL_ALLOC(idx, name) \
    do { if (name##_free >= 0) { (idx) = (uint32_t)name##_free; \
    name##_free = *(int*)&name[idx]; } else { \
    if (name##_count >= name##_max) slap_panic(#name " pool full"); \
    (idx) = (uint32_t)name##_count++; } } while(0)

#define POOL_FREE(name, idx) do { \
    *(int*)&name[idx] = name##_free; \
    name##_free = (int)(idx); } while(0)

// ── Pool: Slices (shared by T_SLICE and T_DICE) ────────────────────────────

typedef struct { Val *data; int len; int rc; } SliceObj;
POOL_DECL(SliceObj, slices, MAX_SLICES);

static uint32_t slice_new(Val *data, int len) {
    uint32_t idx;
    POOL_ALLOC(idx, slices);
    slices[idx].len = len;
    slices[idx].rc = 1;
    slices[idx].data = NULL;
    if (len > 0) {
        slices[idx].data = malloc((size_t)len * sizeof(Val));
        if (data) memcpy(slices[idx].data, data, (size_t)len * sizeof(Val));
    }
    return idx;
}

static uint32_t slice_clone_range(SliceObj *sl, int start, int len) {
    Val *data = len > 0 ? malloc((size_t)len * sizeof(Val)) : NULL;
    for (int i = 0; i < len; i++) data[i] = val_clone(sl->data[start + i]);
    uint32_t s = slice_new(data, len);
    free(data);
    return s;
}

// ── Pool: Tuples ────────────────────────────────────────────────────────────

typedef struct {
    int body_start, body_len;
    uint32_t env;
    bool owns_env;
    bool needs_scope;
    uint32_t compose_with;
    int rc;
} TupleObj;
POOL_DECL(TupleObj, tuples, MAX_TUPLES);

static uint32_t sym_def_id, sym_let_id, sym_recur_id;
static bool tuple_body_needs_scope(int start, int len); // defined after Node

static uint32_t tuple_new(int start, int len, uint32_t env, bool owns) {
    uint32_t idx;
    POOL_ALLOC(idx, tuples);
    bool ns = (sym_def_id == 0) ? true : tuple_body_needs_scope(start, len);
    tuples[idx] = (TupleObj){start, len, env, owns, ns, NONE, 1};
    return idx;
}

// ── Pool: Records ───────────────────────────────────────────────────────────

typedef struct { uint32_t *keys; Val *vals; int count, cap, rc; } RecObj;
POOL_DECL(RecObj, recs, MAX_RECS);

static uint32_t rec_new(void) {
    uint32_t idx;
    POOL_ALLOC(idx, recs);
    recs[idx] = (RecObj){NULL, NULL, 0, 0, 1};
    return idx;
}

static void rec_set(uint32_t r, uint32_t key, Val val) {
    RecObj *rc = &recs[r];
    for (int i = 0; i < rc->count; i++) {
        if (rc->keys[i] == key) { val_free(rc->vals[i]); rc->vals[i] = val; return; }
    }
    if (rc->count >= rc->cap) {
        rc->cap = rc->cap ? rc->cap * 2 : 4;
        rc->keys = realloc(rc->keys, (size_t)rc->cap * sizeof(uint32_t));
        rc->vals = realloc(rc->vals, (size_t)rc->cap * sizeof(Val));
    }
    rc->keys[rc->count] = key;
    rc->vals[rc->count] = val;
    rc->count++;
}

static bool rec_get(uint32_t r, uint32_t key, Val *out) {
    RecObj *rc = &recs[r];
    for (int i = 0; i < rc->count; i++) {
        if (rc->keys[i] == key) { *out = rc->vals[i]; return true; }
    }
    return false;
}

// ── Pool: Lists (mutable) ──────────────────────────────────────────────────

typedef struct { Val *data; int len, cap; } ListObj;
POOL_DECL(ListObj, lists, MAX_LISTS);

static uint32_t list_new(Val *data, int len) {
    uint32_t idx;
    POOL_ALLOC(idx, lists);
    int cap = len > 8 ? len : 8;
    lists[idx].data = malloc((size_t)cap * sizeof(Val));
    lists[idx].len = len;
    lists[idx].cap = cap;
    if (data && len > 0) memcpy(lists[idx].data, data, (size_t)len * sizeof(Val));
    return idx;
}

// ── Pool: Dicts (hash table, mutable) ──────────────────────────────────────

#define DICT_INIT_CAP 16

typedef struct { Val *keys; Val *vals; bool *used; int count, cap; } DictObj;
POOL_DECL(DictObj, dicts, MAX_DICTS);

static uint32_t val_hash(Val v) {
    switch (v.type) {
        case T_INT:   { uint64_t x = (uint64_t)v.ival; return (uint32_t)(x ^ (x >> 32)); }
        case T_SYM:   return v.sym * 2654435761u;
        case T_FLOAT: { uint64_t x; memcpy(&x, &v.fval, 8); return (uint32_t)(x ^ (x >> 32)); }
        case T_SLICE: case T_DICE: {
            SliceObj *sl = &slices[v.slice];
            uint32_t h = 2166136261u;
            for (int i = 0; i < sl->len; i++) { h ^= val_hash(sl->data[i]); h *= 16777619u; }
            return h;
        }
        default: return 0;
    }
}

static bool val_eq(Val a, Val b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case T_INT:   return a.ival == b.ival;
        case T_FLOAT: return a.fval == b.fval;
        case T_SYM:   return a.sym == b.sym;
        case T_SLICE: case T_DICE: {
            if (a.slice == b.slice) return true;
            SliceObj *sa = &slices[a.slice], *sb = &slices[b.slice];
            if (sa->len != sb->len) return false;
            for (int i = 0; i < sa->len; i++)
                if (!val_eq(sa->data[i], sb->data[i])) return false;
            return true;
        }
        case T_REC: {
            if (a.rec == b.rec) return true;
            RecObj *ra = &recs[a.rec], *rb = &recs[b.rec];
            if (ra->count != rb->count) return false;
            for (int i = 0; i < ra->count; i++) {
                Val bv;
                if (!rec_get(b.rec, ra->keys[i], &bv)) return false;
                if (!val_eq(ra->vals[i], bv)) return false;
            }
            return true;
        }
        default: return false;
    }
}

static uint32_t dict_new(void) {
    uint32_t idx;
    POOL_ALLOC(idx, dicts);
    dicts[idx].cap = DICT_INIT_CAP;
    dicts[idx].count = 0;
    dicts[idx].keys = malloc(DICT_INIT_CAP * sizeof(Val));
    dicts[idx].vals = malloc(DICT_INIT_CAP * sizeof(Val));
    dicts[idx].used = calloc(DICT_INIT_CAP, sizeof(bool));
    return idx;
}

static void dict_resize(uint32_t di) {
    DictObj *d = &dicts[di];
    int old_cap = d->cap;
    Val *ok = d->keys; Val *ov = d->vals; bool *ou = d->used;
    d->cap *= 2;
    d->keys = malloc((size_t)d->cap * sizeof(Val));
    d->vals = malloc((size_t)d->cap * sizeof(Val));
    d->used = calloc((size_t)d->cap, sizeof(bool));
    d->count = 0;
    for (int i = 0; i < old_cap; i++) {
        if (ou[i]) {
            uint32_t h = val_hash(ok[i]) & (uint32_t)(d->cap - 1);
            while (d->used[h]) h = (h + 1) & (uint32_t)(d->cap - 1);
            d->keys[h] = ok[i]; d->vals[h] = ov[i]; d->used[h] = true; d->count++;
        }
    }
    free(ok); free(ov); free(ou);
}

static void dict_set(uint32_t di, Val key, Val val) {
    DictObj *d = &dicts[di];
    if (d->count * 2 >= d->cap) dict_resize(di);
    d = &dicts[di]; // refresh after resize
    uint32_t h = val_hash(key) & (uint32_t)(d->cap - 1);
    while (d->used[h]) {
        if (val_eq(d->keys[h], key)) {
            val_free(d->keys[h]); val_free(d->vals[h]);
            d->keys[h] = key; d->vals[h] = val; return;
        }
        h = (h + 1) & (uint32_t)(d->cap - 1);
    }
    d->keys[h] = key; d->vals[h] = val; d->used[h] = true; d->count++;
}


static bool dict_remove(uint32_t di, Val key) {
    DictObj *d = &dicts[di];
    uint32_t h = val_hash(key) & (uint32_t)(d->cap - 1);
    for (int i = 0; i < d->cap; i++) {
        if (!d->used[h]) return false;
        if (val_eq(d->keys[h], key)) {
            val_free(d->keys[h]); val_free(d->vals[h]);
            d->used[h] = false; d->count--;
            // rehash following entries
            uint32_t j = (h + 1) & (uint32_t)(d->cap - 1);
            while (d->used[j]) {
                Val rk = d->keys[j], rv = d->vals[j];
                d->used[j] = false; d->count--;
                dict_set(di, rk, rv);
                j = (j + 1) & (uint32_t)(d->cap - 1);
            }
            return true;
        }
        h = (h + 1) & (uint32_t)(d->cap - 1);
    }
    return false;
}

// ── Pool: Strings (mutable byte array) ─────────────────────────────────────

typedef struct { uint8_t *data; int len, cap; } StrObj;
POOL_DECL(StrObj, strs, MAX_STRS);

static uint32_t str_new(const uint8_t *data, int len) {
    uint32_t idx;
    POOL_ALLOC(idx, strs);
    int cap = len > 16 ? len : 16;
    strs[idx].data = malloc((size_t)cap);
    strs[idx].len = len;
    strs[idx].cap = cap;
    if (data && len > 0) memcpy(strs[idx].data, data, (size_t)len);
    return idx;
}

// ── Heap (Boxes) ────────────────────────────────────────────────────────────

typedef struct { Val val; bool alive; } HeapSlot;
POOL_DECL(HeapSlot, heap, MAX_HEAP);

static uint32_t heap_alloc(Val v) {
    uint32_t idx;
    POOL_ALLOC(idx, heap);
    heap[idx] = (HeapSlot){v, true};
    return idx;
}

// ── val_clone ───────────────────────────────────────────────────────────────

static Val val_clone(Val v) {
    switch (v.type) {
        case T_INT: case T_FLOAT: case T_SYM:
            return v;
        case T_TUPLE:
            tuples[v.tuple].rc++;
            return v;
        case T_SLICE: case T_DICE:
            slices[v.slice].rc++;
            return v;
        case T_REC:
            recs[v.rec].rc++;
            return v;
        default:
            slap_panic("val_clone: cannot clone linear type %s\n\n"
                        "  Linear values (Box, List, Dict, String) must be consumed\n"
                        "  exactly once. Use 'lend' to borrow or 'clone' for an\n"
                        "  explicit deep copy.", type_name(v.type));
    }
}

// ── val_free ────────────────────────────────────────────────────────────────

static void val_free(Val v) {
    switch (v.type) {
        case T_INT: case T_FLOAT: case T_SYM:
            return;
        case T_TUPLE: {
            TupleObj *tu = &tuples[v.tuple];
            if (--tu->rc <= 0) {
                if (tu->owns_env) scope_release(tu->env);
                if (tu->compose_with != NONE) val_free(VAL_TUPLE(tu->compose_with));
                POOL_FREE(tuples, v.tuple);
            }
            return;
        }
        case T_SLICE: case T_DICE: {
            SliceObj *sl = &slices[v.slice];
            if (--sl->rc <= 0) {
                for (int i = 0; i < sl->len; i++) val_free(sl->data[i]);
                free(sl->data);
                POOL_FREE(slices, v.slice);
            }
            return;
        }
        case T_REC: {
            RecObj *rc = &recs[v.rec];
            if (--rc->rc <= 0) {
                for (int i = 0; i < rc->count; i++) val_free(rc->vals[i]);
                free(rc->keys); free(rc->vals);
                POOL_FREE(recs, v.rec);
            }
            return;
        }
        case T_BOX:
            if (!heap[v.box].alive) slap_panic("double-free on Box %u", v.box);
            val_free(heap[v.box].val);
            heap[v.box].alive = false;
            POOL_FREE(heap, v.box);
            return;
        case T_LIST: {
            ListObj *li = &lists[v.list];
            for (int i = 0; i < li->len; i++) val_free(li->data[i]);
            free(li->data);
            POOL_FREE(lists, v.list);
            return;
        }
        case T_DICT: {
            DictObj *d = &dicts[v.dict];
            for (int i = 0; i < d->cap; i++) {
                if (d->used[i]) { val_free(d->keys[i]); val_free(d->vals[i]); }
            }
            free(d->keys); free(d->vals); free(d->used);
            POOL_FREE(dicts, v.dict);
            return;
        }
        case T_STR: {
            free(strs[v.str].data);
            POOL_FREE(strs, v.str);
            return;
        }
    }
}

// ── val_print ───────────────────────────────────────────────────────────────

static void val_print(Val v, FILE *f) {
    switch (v.type) {
        case T_INT:   fprintf(f, "%lld", (long long)v.ival); break;
        case T_FLOAT: fprintf(f, "%g", v.fval); break;
        case T_SYM:   fprintf(f, "'%s", sym_names[v.sym]); break;
        case T_TUPLE: fprintf(f, "(...)"); break;
        case T_SLICE: {
            SliceObj *sl = &slices[v.slice];
            bool is_str = sl->len > 0;
            for (int i = 0; i < sl->len && is_str; i++)
                is_str = sl->data[i].type == T_INT && sl->data[i].ival >= 0 && sl->data[i].ival <= 127;
            if (is_str) {
                fputc('"', f);
                for (int i = 0; i < sl->len; i++) fputc((char)sl->data[i].ival, f);
                fputc('"', f);
            } else {
                fputc('[', f);
                for (int i = 0; i < sl->len; i++) { if (i) fputc(' ', f); val_print(sl->data[i], f); }
                fputc(']', f);
            }
            break;
        }
        case T_DICE: {
            SliceObj *sl = &slices[v.slice];
            fprintf(f, "<dice:%d>", sl->len / 2);
            break;
        }
        case T_REC: {
            RecObj *rc = &recs[v.rec];
            fputc('{', f);
            for (int i = 0; i < rc->count; i++) {
                if (i) fputc(' ', f);
                fprintf(f, "'%s ", sym_names[rc->keys[i]]);
                val_print(rc->vals[i], f);
            }
            fputc('}', f);
            break;
        }
        case T_BOX:  fprintf(f, "Box<%u>", v.box); break;
        case T_LIST: fprintf(f, "List<%u>", v.list); break;
        case T_DICT: fprintf(f, "Dict<%u>", v.dict); break;
        case T_STR: {
            StrObj *st = &strs[v.str];
            fprintf(f, "Str<\"");
            for (int i = 0; i < st->len; i++) fputc(st->data[i], f);
            fprintf(f, "\">");
            break;
        }
    }
}

// ── Stack ───────────────────────────────────────────────────────────────────

static Val stack[STACK_MAX];
static int sp;

static void print_stack_top(void) {
    if (sp > 0) {
        fprintf(stderr, "\n  %sstack top:%s", C_DIM, C_RESET);
        int show = sp < 5 ? sp : 5;
        for (int i = sp - show; i < sp; i++) {
            fprintf(stderr, " ");
            val_print(stack[i], stderr);
        }
        fprintf(stderr, "\n");
    }
}

static void push(Val v) {
    if (sp >= STACK_MAX) slap_panic("stack overflow");
    stack[sp++] = v;
}
static Val pop(void) {
    if (sp <= 0) slap_panic("stack underflow");
    return stack[--sp];
}
static Val peek(void) {
    if (sp <= 0) slap_panic("stack underflow (peek)");
    return stack[sp - 1];
}
static inline int64_t  pop_int(void)   { Val v=pop(); if(v.type!=T_INT)   slap_panic("expected Int, got %s",   type_name(v.type)); return v.ival; }
static inline double   pop_float(void) { Val v=pop(); if(v.type!=T_FLOAT) slap_panic("expected Float, got %s", type_name(v.type)); return v.fval; }
static inline uint32_t pop_sym(void)   { Val v=pop(); if(v.type!=T_SYM)   slap_panic("expected Symbol, got %s",type_name(v.type)); return v.sym; }
static inline Val      pop_tuple(void) { Val v=pop(); if(v.type!=T_TUPLE) slap_panic("expected Tuple, got %s", type_name(v.type)); return v; }
static inline Val      pop_slice(void) { Val v=pop(); if(v.type!=T_SLICE) slap_panic("expected Slice, got %s", type_name(v.type)); return v; }

#define EXPECT(v, t) do { if ((v).type != (t)) \
    slap_panic("%s: expected %s, got %s", current_word, type_name(t), type_name((v).type)); } while(0)
#define EXPECT2(v, t1, t2) do { if ((v).type != (t1) && (v).type != (t2)) \
    slap_panic("%s: expected %s or %s, got %s", current_word, type_name(t1), type_name(t2), type_name((v).type)); } while(0)

// ── Nodes ───────────────────────────────────────────────────────────────────

typedef void (*PrimFn)(void);

typedef enum { N_PUSH, N_WORD, N_SLICE, N_TUPLE, N_RECORD } NodeType;

typedef struct {
    NodeType type;
    int line, col;
    union {
        Val literal;       // N_PUSH
        uint32_t sym;      // N_WORD
        struct { int start, len; } body; // N_SLICE, N_TUPLE, N_RECORD
    };
    PrimFn cached_prim;    // N_WORD: resolved primitive (NULL if user-bound)
} Node;

static Node nodes[MAX_NODES];
static int node_count;

static bool tuple_body_needs_scope(int start, int len) {
    for (int i = start; i < start + len; ) {
        Node *n = &nodes[i];
        if (n->type == N_WORD) {
            if (n->sym == sym_def_id || n->sym == sym_let_id || n->sym == sym_recur_id)
                return true;
            i++;
        } else if (n->type == N_TUPLE || n->type == N_SLICE || n->type == N_RECORD) {
            i += 1 + n->body.len;
        } else {
            i++;
        }
    }
    return false;
}

// ── Scopes ──────────────────────────────────────────────────────────────────

typedef struct { uint32_t name; Val val; bool is_def; } Binding;
typedef struct { uint32_t parent; Binding *binds; int count, cap; bool owns_parent; } Scope;
POOL_DECL(Scope, scopes, MAX_SCOPES);
static uint32_t global_scope;

// Fast lookup table for global scope: indexed by symbol ID
static Binding *global_lookup[MAX_SYMS];

static uint32_t scope_new(uint32_t parent) {
    uint32_t idx;
    POOL_ALLOC(idx, scopes);
    Scope *s = &scopes[idx];
    s->parent = parent;
    s->count = 0;          // keep binds+cap from previous use
    s->owns_parent = false;
    return idx;
}

static void scope_release(uint32_t sc) {
    if (sc == NONE) return;
    Scope *s = &scopes[sc];
    uint32_t parent = s->parent;
    bool owns = s->owns_parent;
    for (int i = 0; i < s->count; i++)
        if (s->binds[i].val.type >= T_TUPLE) val_free(s->binds[i].val);
    POOL_FREE(scopes, sc);
    if (owns) scope_release(parent);
}

static void scope_bind(uint32_t sc, uint32_t name, Val val, bool is_def) {
    Scope *s = &scopes[sc];
    for (int i = 0; i < s->count; i++) {
        if (s->binds[i].name == name) {
            val_free(s->binds[i].val);
            s->binds[i].val = val;
            s->binds[i].is_def = is_def;
            if (sc == global_scope) global_lookup[name] = &s->binds[i];
            return;
        }
    }
    if (s->count >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4;
        s->binds = realloc(s->binds, (size_t)s->cap * sizeof(Binding));
        // Realloc may move binds — rebuild global_lookup pointers
        if (sc == global_scope) {
            for (int i = 0; i < s->count; i++)
                global_lookup[s->binds[i].name] = &s->binds[i];
        }
    }
    s->binds[s->count] = (Binding){name, val, is_def};
    if (sc == global_scope) global_lookup[name] = &s->binds[s->count];
    s->count++;
}

static Binding *scope_find(uint32_t sc, uint32_t name) {
    while (sc != NONE && sc != 0) {
        if (sc == global_scope) return global_lookup[name];
        Scope *s = &scopes[sc];
        for (int i = s->count - 1; i >= 0; i--)
            if (s->binds[i].name == name) return &s->binds[i];
        sc = s->parent;
    }
    return NULL;
}

static uint32_t scope_snapshot(uint32_t sc) {
    if (sc == NONE || sc == 0 || sc == global_scope) return sc;
    uint32_t parent_copy = scope_snapshot(scopes[sc].parent);
    uint32_t copy = scope_new(parent_copy);
    if (parent_copy != NONE && parent_copy != 0 && parent_copy != global_scope)
        scopes[copy].owns_parent = true;
    Scope *orig = &scopes[sc];
    for (int i = 0; i < orig->count; i++)
        scope_bind(copy, orig->binds[i].name, val_clone(orig->binds[i].val), orig->binds[i].is_def);
    return copy;
}

// ── Tokenizer ───────────────────────────────────────────────────────────────

typedef enum {
    TOK_INT, TOK_FLOAT, TOK_TRUE, TOK_FALSE, TOK_SYMBOL, TOK_STRING,
    TOK_WORD, TOK_LPAREN, TOK_RPAREN, TOK_LBRACKET, TOK_RBRACKET,
    TOK_LBRACE, TOK_RBRACE, TOK_EOF
} TokType;

typedef struct { TokType type; int line, col; union { int64_t ival; double fval; char word[SYM_NAME_MAX]; }; } Token;

static const char *src;
static int src_pos, src_line, src_col;

static char src_peek(void) { return src[src_pos]; }
static char src_next(void) {
    char c = src[src_pos++];
    if (c == '\n') { src_line++; src_col = 1; } else src_col++;
    return c;
}
static bool src_eof(void) { return src[src_pos] == '\0'; }

static bool is_word_char(char c) {
    return c && !isspace((unsigned char)c) && c != '(' && c != ')' &&
           c != '[' && c != ']' && c != '{' && c != '}' && c != '"';
}

// ── Parser ──────────────────────────────────────────────────────────────────

static Token curtok;
static void advance(void) {
    while (!src_eof()) {
        if (isspace((unsigned char)src_peek())) { src_next(); continue; }
        if (src_peek() == '-' && src_pos + 1 < (int)strlen(src) && src[src_pos + 1] == '-') {
            while (!src_eof() && src_peek() != '\n') src_next();
            continue;
        }
        break;
    }
    Token t = {0};
    t.line = src_line; t.col = src_col;
    if (src_eof()) { t.type = TOK_EOF; curtok = t; return; }
    char c = src_peek();

    if (c == '(') { src_next(); t.type = TOK_LPAREN; curtok = t; return; }
    if (c == ')') { src_next(); t.type = TOK_RPAREN; curtok = t; return; }
    if (c == '[') { src_next(); t.type = TOK_LBRACKET; curtok = t; return; }
    if (c == ']') { src_next(); t.type = TOK_RBRACKET; curtok = t; return; }
    if (c == '{') { src_next(); t.type = TOK_LBRACE; curtok = t; return; }
    if (c == '}') { src_next(); t.type = TOK_RBRACE; curtok = t; return; }

    // string literal
    if (c == '"') {
        src_next();
        int i = 0;
        while (!src_eof() && src_peek() != '"') {
            char ch = src_next();
            if (ch == '\\' && !src_eof()) {
                ch = src_next();
                switch (ch) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case '\\': ch = '\\'; break;
                    case '"': ch = '"'; break;
                    default: break;
                }
            }
            if (i < SYM_NAME_MAX - 1) t.word[i++] = ch;
        }
        if (!src_eof()) src_next();
        t.word[i] = '\0';
        t.type = TOK_STRING;
        curtok = t; return;
    }

    // symbol: 'name
    if (c == '\'') {
        src_next();
        int i = 0;
        while (!src_eof() && is_word_char(src_peek()))
            if (i < SYM_NAME_MAX - 1) t.word[i++] = src_next();
            else src_next();
        t.word[i] = '\0';
        t.type = TOK_SYMBOL;
        curtok = t; return;
    }

    // number or negative number
    if (isdigit((unsigned char)c) ||
        (c == '-' && src_pos + 1 < (int)strlen(src) && (isdigit((unsigned char)src[src_pos + 1]) || src[src_pos + 1] == '.'))) {
        int i = 0;
        bool is_float = false;
        if (c == '-') { t.word[i++] = src_next(); }
        while (!src_eof() && (isdigit((unsigned char)src_peek()) || src_peek() == '.')) {
            if (src_peek() == '.') is_float = true;
            if (i < SYM_NAME_MAX - 1) t.word[i++] = src_next();
            else src_next();
        }
        t.word[i] = '\0';
        if (is_float) { t.type = TOK_FLOAT; t.fval = strtod(t.word, NULL); }
        else { t.type = TOK_INT; t.ival = strtoll(t.word, NULL, 10); }
        curtok = t; return;
    }

    // word
    {
        int i = 0;
        while (!src_eof() && is_word_char(src_peek()))
            if (i < SYM_NAME_MAX - 1) t.word[i++] = src_next();
            else src_next();
        t.word[i] = '\0';
        if (strcmp(t.word, "true") == 0) { t.type = TOK_INT; t.ival = 1; curtok = t; return; }
        if (strcmp(t.word, "false") == 0) { t.type = TOK_INT; t.ival = 0; curtok = t; return; }
        t.type = TOK_WORD;
        curtok = t;
    }
}

static void parse_body(TokType close);

static void emit_push(Val v, int line, int col) {
    if (node_count >= MAX_NODES) slap_panic("node overflow");
    nodes[node_count++] = (Node){.type = N_PUSH, .line = line, .col = col, .literal = v};
}

static void parse_body(TokType close) {
    while (curtok.type != close && curtok.type != TOK_EOF) {
        Token t = curtok;
        switch (t.type) {
            case TOK_INT:
                emit_push(VAL_INT(t.ival), t.line, t.col);
                advance();
                break;
            case TOK_FLOAT:
                emit_push(VAL_FLOAT(t.fval), t.line, t.col);
                advance();
                break;
            case TOK_SYMBOL:
                emit_push(VAL_SYM(sym_intern(t.word)), t.line, t.col);
                advance();
                break;
            case TOK_STRING: {
                int slen = (int)strlen(t.word);
                Val *data = NULL;
                if (slen > 0) {
                    data = malloc((size_t)slen * sizeof(Val));
                    for (int i = 0; i < slen; i++) data[i] = VAL_INT((uint8_t)t.word[i]);
                }
                uint32_t s = slice_new(data, slen);
                free(data);
                emit_push(VAL_SLICE(s), t.line, t.col);
                advance();
                break;
            }
            case TOK_WORD:
                if (node_count >= MAX_NODES) slap_panic("node overflow");
                nodes[node_count++] = (Node){.type = N_WORD, .line = t.line, .col = t.col, .sym = sym_intern(t.word), .cached_prim = NULL};
                advance();
                break;
            case TOK_LPAREN: case TOK_LBRACKET: case TOK_LBRACE: {
                static const struct { TokType close; const char *ch; NodeType nt; }
                    bk[] = { [TOK_LPAREN]={TOK_RPAREN,")",N_TUPLE}, [TOK_LBRACKET]={TOK_RBRACKET,"]",N_SLICE}, [TOK_LBRACE]={TOK_RBRACE,"}",N_RECORD} };
                advance();
                int placeholder = node_count++;
                if (placeholder >= MAX_NODES) slap_panic("node overflow");
                int body_start = node_count;
                parse_body(bk[t.type].close);
                if (curtok.type != bk[t.type].close) slap_panic("expected '%s' at line %d, col %d", bk[t.type].ch, curtok.line, curtok.col);
                advance();
                nodes[placeholder] = (Node){.type = bk[t.type].nt, .line = t.line, .col = t.col,
                                             .body = {body_start, node_count - body_start}};
                break;
            }
            default:
                slap_panic("unexpected token at line %d, col %d", t.line, t.col);
        }
    }
}

static int parse_source(const char *source) {
    src = source; src_pos = 0; src_line = 1; src_col = 1;
    node_count = 0;
    advance();
    int start = node_count;
    parse_body(TOK_EOF);
    return node_count - start;
}

// ── exec_tuple ──────────────────────────────────────────────────────────────

static uint32_t g_scope; // current scope for primitives like def/let
static uint32_t g_lazy_parent = NONE; // deferred scope creation: parent env awaiting first let/def
static uint32_t recur_pending = NONE; // set by 'recur' primitive, consumed by N_TUPLE

static void ensure_own_scope(void) {
    if (g_lazy_parent != NONE) {
        g_scope = scope_new(g_lazy_parent);
        g_lazy_parent = NONE;
    }
}

static void exec_tuple(Val t) {
    TupleObj *tu = &tuples[t.tuple];
    if (tu->body_len > 0) {
        uint32_t saved_lazy = g_lazy_parent;
        g_lazy_parent = tu->env;
        eval(tu->body_start, tu->body_len, tu->env);
        g_lazy_parent = saved_lazy;
    }
    if (tu->compose_with != NONE) {
        exec_tuple(VAL_TUPLE(tu->compose_with));
    }
}

// ── Primitives ──────────────────────────────────────────────────────────────

static PrimFn prim_table[MAX_SYMS];

// -- Stack --
static void p_dup(void) { push(val_clone(peek())); }
static void p_drop(void) { Val v = pop(); val_free(v); }
static void p_swap(void) { Val b = pop(), a = pop(); push(b); push(a); }
static void p_dip(void) { Val body = pop_tuple(); Val stash = pop(); exec_tuple(body); val_free(body); push(stash); }

// -- Control --
static void p_apply(void) { Val t = pop_tuple(); exec_tuple(t); val_free(t); }

static void p_if(void) {
    Val fb = pop_tuple(), tb = pop_tuple();
    int64_t c = pop_int();
    exec_tuple(c != 0 ? tb : fb);
    val_free(tb); val_free(fb);
}

static void p_loop(void) {
    Val body = pop_tuple();
    while (true) { exec_tuple(body); if (pop_int() == 0) break; }
    val_free(body);
}

static void p_while(void) {
    Val body = pop_tuple(), pred = pop_tuple();
    while (true) {
        exec_tuple(pred);
        if (pop_int() == 0) break;
        exec_tuple(body);
    }
    val_free(pred);
    val_free(body);
}

static void p_cond(void) {
    Val default_b = pop_tuple();
    Val clauses = pop_slice();
    Val scrutinee = pop();
    SliceObj *cl = &slices[clauses.slice];
    bool matched = false;

    for (int i = 0; i < cl->len && !matched; i++) {
        Val clause = cl->data[i];
        if (clause.type != T_TUPLE) slap_panic("cond: clauses must be tuples");
        // execute clause to get (pred, body)
        int base = sp;
        exec_tuple(clause);
        if (sp - base != 2) slap_panic("cond: each clause must produce exactly 2 values (predicate and body), got %d", sp - base);
        Val pred = stack[base], body = stack[base + 1];
        sp = base;

        // push snapshot of scrutinee for predicate
        push(val_is_linear(scrutinee) ? linear_snapshot(scrutinee) : val_clone(scrutinee));
        exec_tuple(pred); val_free(pred);
        int64_t result = pop_int();

        if (result != 0) {
            push(scrutinee);
            exec_tuple(body); val_free(body);
            matched = true;
        } else {
            val_free(body);
        }
    }

    if (!matched) {
        push(scrutinee);
        exec_tuple(default_b);
    }
    val_free(default_b);
    val_free(clauses);
}

static void p_match(void) {
    Val default_b = pop_tuple();
    Val dispatch = pop();
    EXPECT(dispatch, T_REC);
    uint32_t key = pop_sym();
    Val body;
    if (rec_get(dispatch.rec, key, &body)) {
        Val body_c = val_clone(body);
        val_free(dispatch);
        exec_tuple(body_c); val_free(body_c);
        val_free(default_b);
    } else {
        val_free(dispatch);
        exec_tuple(default_b); val_free(default_b);
    }
}

// -- Bool --
static void p_not(void) { int64_t v = pop_int(); push(VAL_INT(v == 0 ? 1 : 0)); }
static void p_and(void) { int64_t b = pop_int(), a = pop_int(); push(VAL_INT((a != 0 && b != 0) ? 1 : 0)); }
static void p_or(void)  { int64_t b = pop_int(), a = pop_int(); push(VAL_INT((a != 0 || b != 0) ? 1 : 0)); }

// -- Compare --
static void p_eq(void) {
    Val b = pop(), a = pop();
    bool r = val_eq(a, b);
    val_free(a); val_free(b);
    push(VAL_INT(r ? 1 : 0));
}

static void p_lt(void) {
    Val b = pop(), a = pop();
    if (a.type != b.type) slap_panic("%s: type mismatch (%s vs %s)", current_word, type_name(a.type), type_name(b.type));
    bool r;
    switch (a.type) {
        case T_INT:   r = a.ival < b.ival; break;
        case T_FLOAT: r = a.fval < b.fval; break;
        default: slap_panic("lt: cannot compare %s", type_name(a.type));
    }
    push(VAL_INT(r ? 1 : 0));
}

// -- Int Math --
#define BINOP_INT(name, op) static void p_##name(void) { int64_t b=pop_int(),a=pop_int(); push(VAL_INT(a op b)); }
BINOP_INT(iplus, +) BINOP_INT(isub, -) BINOP_INT(imul, *)
static void p_idiv(void) {
    int64_t b = pop_int(), a = pop_int();
    if (b == 0) slap_panic("idiv: division by zero");
    push(VAL_INT(a / b));
}
static void p_imod(void) {
    int64_t b = pop_int(), a = pop_int();
    if (b == 0) slap_panic("imod: division by zero");
    push(VAL_INT(a % b));
}

// -- Float Math --
#define BINOP_FLOAT(name, op) static void p_##name(void) { double b=pop_float(),a=pop_float(); push(VAL_FLOAT(a op b)); }
#define UNOP_FLOAT(name, fn) static void p_##name(void) { push(VAL_FLOAT(fn(pop_float()))); }
#define BINOP_FLOAT2(name, fn) static void p_##name(void) { double b=pop_float(),a=pop_float(); push(VAL_FLOAT(fn(a,b))); }
BINOP_FLOAT(fplus, +) BINOP_FLOAT(fsub, -) BINOP_FLOAT(fmul, *)
static void p_fdiv(void) {
    double b = pop_float(), a = pop_float();
    if (b == 0.0) slap_panic("fdiv: division by zero");
    push(VAL_FLOAT(a / b));
}
UNOP_FLOAT(fsqrt, sqrt) UNOP_FLOAT(fsin, sin) UNOP_FLOAT(fcos, cos) UNOP_FLOAT(ftan, tan)
UNOP_FLOAT(ffloor, floor) UNOP_FLOAT(fceil, ceil) UNOP_FLOAT(fround, round)
UNOP_FLOAT(fexp, exp) UNOP_FLOAT(flog, log)
BINOP_FLOAT2(fpow, pow) BINOP_FLOAT2(fatan2, atan2)

// -- Polymorphic arithmetic --
#define POLY_BINOP(name, op) static void p_##name(void) { \
    Val b = pop(), a = pop(); \
    if (a.type != b.type) slap_panic(#name ": type mismatch: %s vs %s", type_name(a.type), type_name(b.type)); \
    switch (a.type) { \
        case T_INT:   push(VAL_INT(a.ival op b.ival)); break; \
        case T_FLOAT: push(VAL_FLOAT(a.fval op b.fval)); break; \
        default: slap_panic(#name ": unsupported type %s", type_name(a.type)); \
    } \
}
POLY_BINOP(plus, +) POLY_BINOP(sub, -) POLY_BINOP(mul, *)
static void p_div(void) {
    Val b = pop(), a = pop();
    if (a.type != b.type) slap_panic("div: type mismatch: %s vs %s", type_name(a.type), type_name(b.type));
    switch (a.type) {
        case T_INT:
            if (b.ival == 0) slap_panic("div: division by zero");
            push(VAL_INT(a.ival / b.ival)); break;
        case T_FLOAT:
            if (b.fval == 0.0) slap_panic("div: division by zero");
            push(VAL_FLOAT(a.fval / b.fval)); break;
        default: slap_panic("div: unsupported type %s", type_name(a.type));
    }
}
static void p_mod(void) {
    int64_t b = pop_int(), a = pop_int();
    if (b == 0) slap_panic("mod: division by zero");
    push(VAL_INT(a % b));
}

// -- Conversion --
static void p_itof(void) { push(VAL_FLOAT((double)pop_int())); }
static void p_ftoi(void) { push(VAL_INT((int64_t)pop_float())); }

// -- Tuples --
static void p_compose(void) {
    Val b = pop_tuple(), a = pop_tuple();
    TupleObj *ta = &tuples[a.tuple];
    uint32_t env = ta->env;
    bool owns = false;
    if (ta->owns_env && env != NONE && env != 0) { env = scope_snapshot(env); owns = true; }
    uint32_t result = tuple_new(ta->body_start, ta->body_len, env, owns);
    // Clone a's compose chain
    uint32_t *tail = &tuples[result].compose_with;
    uint32_t cur = ta->compose_with;
    while (cur != NONE) {
        TupleObj *tc = &tuples[cur];
        uint32_t cenv = tc->env; bool cowns = false;
        if (tc->owns_env && cenv != NONE && cenv != 0) { cenv = scope_snapshot(cenv); cowns = true; }
        uint32_t link = tuple_new(tc->body_start, tc->body_len, cenv, cowns);
        *tail = link;
        tail = &tuples[link].compose_with;
        cur = tc->compose_with;
    }
    // Attach b at the end of the chain
    *tail = b.tuple;
    tuples[b.tuple].rc++;
    val_free(a);
    val_free(b);
    push(VAL_TUPLE(result));
}

static void p_cons(void) {
    Val elem = pop();
    Val t = pop_tuple();
    TupleObj *tu = &tuples[t.tuple];
    int new_start = node_count;
    int new_len = tu->body_len + 1;
    if (node_count + new_len > MAX_NODES) slap_panic("cons: node overflow");
    memcpy(&nodes[new_start], &nodes[tu->body_start], (size_t)tu->body_len * sizeof(Node));
    nodes[new_start + tu->body_len] = (Node){.type = N_PUSH, .line = panic_line, .col = panic_col, .literal = elem};
    node_count += new_len;
    uint32_t env = tu->env;
    bool owns = false;
    if (tu->owns_env && env != NONE && env != 0) { env = scope_snapshot(env); owns = true; }
    uint32_t result = tuple_new(new_start, new_len, env, owns);
    if (tu->compose_with != NONE) { tuples[result].compose_with = tu->compose_with; tuples[tu->compose_with].rc++; }
    val_free(t);
    push(VAL_TUPLE(result));
}

static void p_car(void) {
    Val t = pop_tuple();
    TupleObj *tu = &tuples[t.tuple];
    if (tu->body_len == 0) slap_panic("car: empty tuple");
    int last_idx = tu->body_start;
    for (int ii = tu->body_start; ii < tu->body_start + tu->body_len; ) {
        last_idx = ii;
        Node *nn = &nodes[ii];
        if (nn->type == N_TUPLE || nn->type == N_SLICE || nn->type == N_RECORD) ii += 1 + nn->body.len;
        else ii++;
    }
    Node *last = &nodes[last_idx];
    Val extracted;
    int last_size;
    switch (last->type) {
        case N_PUSH:
            extracted = val_clone(last->literal);
            last_size = 1;
            break;
        case N_WORD:
            extracted = VAL_SYM(last->sym);
            last_size = 1;
            break;
        default: {
            // N_TUPLE, N_SLICE, N_RECORD: evaluate to get the value
            int save_sp = sp;
            last_size = 1 + last->body.len;
            uint32_t saved_lazy = g_lazy_parent;
            uint32_t child = scope_new(tu->env);
            g_lazy_parent = NONE;
            eval(last_idx, last_size, child);
            scope_release(child);
            g_lazy_parent = saved_lazy;
            if (sp != save_sp + 1) slap_panic("car: bracket node produced %d values, expected 1", sp - save_sp);
            extracted = pop();
            break;
        }
    }
    int shorter_len = tu->body_len - last_size;
    uint32_t env = tu->env;
    bool owns = false;
    if (tu->owns_env && env != NONE && env != 0) { env = scope_snapshot(env); owns = true; }
    uint32_t result = tuple_new(tu->body_start, shorter_len, env, owns);
    if (tu->compose_with != NONE) { tuples[result].compose_with = tu->compose_with; tuples[tu->compose_with].rc++; }
    val_free(t);
    push(VAL_TUPLE(result));
    push(extracted);
}

// -- Records --
static void p_rec(void) {
    Val t = pop_tuple();
    int base = sp;
    exec_tuple(t); val_free(t);
    int n = sp - base;
    Val *elems = malloc((size_t)n * sizeof(Val));
    for (int i = 0; i < n; i++) elems[i] = stack[base + i];
    sp = base;
    uint32_t r = rec_new();
    for (int i = 0; i < n; i++) {
        if (elems[i].type != T_TUPLE) slap_panic("rec: elements must be (key value) tuples, got %s", type_name(elems[i].type));
        int pair_base = sp;
        exec_tuple(elems[i]); val_free(elems[i]);
        if (sp - pair_base != 2) slap_panic("rec: each tuple must produce 2 values (key, value), got %d", sp - pair_base);
        Val kv = stack[pair_base], vv = stack[pair_base + 1];
        sp = pair_base;
        if (kv.type != T_SYM) slap_panic("rec: keys must be symbols, got %s", type_name(kv.type));
        rec_set(r, kv.sym, vv);
    }
    free(elems);
    push(VAL_REC(r));
}

static void p_get(void) {
    uint32_t key = pop_sym();
    Val r = pop();
    EXPECT(r, T_REC);
    Val out;
    if (!rec_get(r.rec, key, &out)) slap_panic("get: key '%s not found in record", sym_names[key]);
    push(val_clone(out));
    val_free(r);
}

static void p_set(void) {
    uint32_t key = pop_sym();
    Val val = pop();
    Val r = pop();
    EXPECT(r, T_REC);
    // create new record with updated field
    RecObj *old = &recs[r.rec];
    uint32_t nr = rec_new();
    bool found = false;
    for (int i = 0; i < old->count; i++) {
        if (old->keys[i] == key) { rec_set(nr, key, val); found = true; }
        else rec_set(nr, old->keys[i], val_clone(old->vals[i]));
    }
    if (!found) rec_set(nr, key, val);
    val_free(r);
    push(VAL_REC(nr));
}

// -- Slices --
static void p_len(void) {
    Val s = pop();
    EXPECT2(s, T_SLICE, T_DICE);
    int n = slices[s.slice].len;
    if (s.type == T_DICE) n /= 2;
    val_free(s);
    push(VAL_INT(n));
}

static void p_fold(void) {
    Val body = pop_tuple();
    Val acc = pop();
    Val s = pop();
    EXPECT2(s, T_SLICE, T_DICE);
    SliceObj *sl = &slices[s.slice];
    push(acc);
    for (int i = 0; i < sl->len; i++) {
        push(val_clone(sl->data[i]));
        exec_tuple(body);
    }
    val_free(body);
    val_free(s);
}

static void p_reduce(void) {
    Val body = pop_tuple();
    Val s = pop();
    EXPECT2(s, T_SLICE, T_DICE);
    SliceObj *sl = &slices[s.slice];
    if (sl->len == 0) slap_panic("reduce: empty slice");
    push(val_clone(sl->data[0]));
    for (int i = 1; i < sl->len; i++) {
        push(val_clone(sl->data[i]));
        exec_tuple(body);
    }
    val_free(body);
    val_free(s);
}

static void p_at(void) {
    Val def = pop();
    int64_t idx = pop_int();
    Val s = pop();
    EXPECT2(s, T_SLICE, T_DICE);
    SliceObj *sl = &slices[s.slice];
    if (idx < 0 || idx >= sl->len) {
        val_free(s);
        push(def);
    } else {
        push(val_clone(sl->data[idx]));
        val_free(s);
        val_free(def);
    }
}

static void p_put(void) {
    Val new_val = pop();
    int64_t idx = pop_int();
    Val s = pop();
    EXPECT(s, T_SLICE);
    SliceObj *sl = &slices[s.slice];
    if (idx < 0 || idx >= sl->len) slap_panic("put: index %lld out of bounds [0, %d)", (long long)idx, sl->len);
    Val *data = malloc((size_t)sl->len * sizeof(Val));
    for (int i = 0; i < sl->len; i++) {
        data[i] = (i == (int)idx) ? new_val : val_clone(sl->data[i]);
    }
    uint32_t ns = slice_new(data, sl->len);
    free(data);
    val_free(s);
    push(VAL_SLICE(ns));
}

// -- Slice iteration: each, map, filter, range --
static void p_each(void) {
    Val body = pop_tuple();
    Val s = pop();
    EXPECT2(s, T_SLICE, T_DICE);
    SliceObj *sl = &slices[s.slice];
    for (int i = 0; i < sl->len; i++) {
        push(val_clone(sl->data[i]));
        exec_tuple(body);
    }
    val_free(body);
    val_free(s);
}

static void p_map(void) {
    Val body = pop_tuple();
    Val s = pop();
    EXPECT2(s, T_SLICE, T_DICE);
    SliceObj *sl = &slices[s.slice];
    int n = sl->len;
    Val *out = n > 0 ? malloc((size_t)n * sizeof(Val)) : NULL;
    for (int i = 0; i < n; i++) {
        push(val_clone(sl->data[i]));
        exec_tuple(body);
        out[i] = pop();
    }
    val_free(body);
    val_free(s);
    uint32_t ns = slice_new(out, n);
    free(out);
    push(VAL_SLICE(ns));
}

static void p_filter(void) {
    Val body = pop_tuple();
    Val s = pop();
    EXPECT2(s, T_SLICE, T_DICE);
    SliceObj *sl = &slices[s.slice];
    int n = sl->len;
    Val *out = n > 0 ? malloc((size_t)n * sizeof(Val)) : NULL;
    int count = 0;
    for (int i = 0; i < n; i++) {
        push(val_clone(sl->data[i]));
        exec_tuple(body);
        int64_t keep = pop_int();
        if (keep != 0) out[count++] = val_clone(sl->data[i]);
    }
    val_free(body);
    val_free(s);
    uint32_t ns = slice_new(out, count);
    free(out);
    push(VAL_SLICE(ns));
}


static void p_sort(void) {
    Val s = pop();
    EXPECT(s, T_SLICE);
    SliceObj *sl = &slices[s.slice];
    int n = sl->len;
    uint32_t ns = slice_clone_range(sl, 0, n);
    val_free(s);
    Val *data = slices[ns].data;
    for (int i = 1; i < n; i++) {
        Val key = data[i];
        int j = i - 1;
        while (j >= 0) {
            bool gt = false;
            if (key.type == T_INT && data[j].type == T_INT) gt = data[j].ival > key.ival;
            else if (key.type == T_FLOAT && data[j].type == T_FLOAT) gt = data[j].fval > key.fval;
            else slap_panic("sort: elements must be Int or Float");
            if (!gt) break;
            data[j + 1] = data[j];
            j--;
        }
        data[j + 1] = key;
    }
    push(VAL_SLICE(ns));
}

static void p_cat(void) {
    Val b = pop();
    Val a = pop();
    EXPECT(a, T_SLICE); EXPECT(b, T_SLICE);
    SliceObj *sa = &slices[a.slice], *sb = &slices[b.slice];
    int na = sa->len, nb = sb->len;
    uint32_t ns = slice_clone_range(sa, 0, na);
    // append b's elements
    SliceObj *out = &slices[ns];
    out->data = realloc(out->data, (size_t)(na + nb) * sizeof(Val));
    for (int i = 0; i < nb; i++) out->data[na + i] = val_clone(sb->data[i]);
    out->len = na + nb;
    val_free(a); val_free(b);
    push(VAL_SLICE(ns));
}

static void p_take(void) {
    int64_t n = pop_int();
    Val s = pop_slice();
    SliceObj *sl = &slices[s.slice];
    if (n < 0) n = 0; if (n > sl->len) n = sl->len;
    uint32_t ns = slice_clone_range(sl, 0, (int)n);
    val_free(s);
    push(VAL_SLICE(ns));
}

static void p_drop_n(void) {
    int64_t n = pop_int();
    Val s = pop_slice();
    SliceObj *sl = &slices[s.slice];
    if (n < 0) n = 0; if (n > sl->len) n = sl->len;
    uint32_t ns = slice_clone_range(sl, (int)n, sl->len - (int)n);
    val_free(s);
    push(VAL_SLICE(ns));
}



static void p_range(void) {
    int64_t hi = pop_int();
    int64_t lo = pop_int();
    int n = (hi > lo) ? (int)(hi - lo) : 0;
    Val *data = n > 0 ? malloc((size_t)n * sizeof(Val)) : NULL;
    for (int i = 0; i < n; i++) data[i] = VAL_INT(lo + i);
    uint32_t s = slice_new(data, n);
    free(data);
    push(VAL_SLICE(s));
}

// -- More Slice ops --

static void p_scan(void) {
    Val body = pop_tuple();
    Val acc = pop();
    Val s = pop();
    EXPECT(s, T_SLICE);
    SliceObj *sl = &slices[s.slice];
    int n = sl->len;
    Val *out = (n + 1) > 0 ? malloc((size_t)(n + 1) * sizeof(Val)) : NULL;
    out[0] = val_clone(acc);
    push(acc);
    for (int i = 0; i < n; i++) {
        push(val_clone(sl->data[i]));
        exec_tuple(body);
        out[i + 1] = val_clone(stack[sp - 1]);
    }
    Val final_acc = pop(); val_free(final_acc);
    val_free(body);
    val_free(s);
    uint32_t ns = slice_new(out, n + 1);
    free(out);
    push(VAL_SLICE(ns));
}


static void p_rotate(void) {
    int64_t n = pop_int();
    Val s = pop_slice();
    SliceObj *sl = &slices[s.slice];
    int len = sl->len;
    Val *data = len > 0 ? malloc((size_t)len * sizeof(Val)) : NULL;
    if (len > 0) {
        int shift = (int)(n % len);
        if (shift < 0) shift += len;
        for (int i = 0; i < len; i++) data[i] = val_clone(sl->data[(i + shift) % len]);
    }
    val_free(s);
    uint32_t ns = slice_new(data, len);
    free(data);
    push(VAL_SLICE(ns));
}

static void p_select(void) {
    Val indices = pop_slice();
    Val data_s = pop_slice();
    SliceObj *idx = &slices[indices.slice];
    SliceObj *src = &slices[data_s.slice];
    int n = idx->len;
    Val *out = n > 0 ? malloc((size_t)n * sizeof(Val)) : NULL;
    for (int i = 0; i < n; i++) {
        if (idx->data[i].type != T_INT) slap_panic("select: indices must be Int, got %s", type_name(idx->data[i].type));
        int64_t ix = idx->data[i].ival;
        if (ix < 0 || ix >= src->len) slap_panic("select: index %lld out of bounds [0, %d)", (long long)ix, src->len);
        out[i] = val_clone(src->data[ix]);
    }
    val_free(indices); val_free(data_s);
    uint32_t ns = slice_new(out, n);
    free(out);
    push(VAL_SLICE(ns));
}

static void p_keep_mask(void) {
    Val mask = pop_slice();
    Val data_s = pop_slice();
    SliceObj *ms = &slices[mask.slice];
    SliceObj *ds = &slices[data_s.slice];
    if (ms->len != ds->len) slap_panic("keep-mask: mask length %d != data length %d", ms->len, ds->len);
    int count = 0;
    for (int i = 0; i < ms->len; i++) {
        if (ms->data[i].type != T_INT) slap_panic("keep-mask: mask elements must be Int, got %s", type_name(ms->data[i].type));
        if (ms->data[i].ival != 0) count++;
    }
    Val *out = count > 0 ? malloc((size_t)count * sizeof(Val)) : NULL;
    int k = 0;
    for (int i = 0; i < ms->len; i++) {
        if (ms->data[i].ival != 0) out[k++] = val_clone(ds->data[i]);
    }
    val_free(mask); val_free(data_s);
    uint32_t ns = slice_new(out, count);
    free(out);
    push(VAL_SLICE(ns));
}

static void p_windows(void) {
    int64_t wn = pop_int();
    Val s = pop_slice();
    SliceObj *sl = &slices[s.slice];
    if (wn <= 0) slap_panic("windows: size must be positive, got %lld", (long long)wn);
    int n = sl->len - (int)wn + 1;
    if (n < 0) n = 0;
    Val *out = n > 0 ? malloc((size_t)n * sizeof(Val)) : NULL;
    for (int i = 0; i < n; i++) {
        Val *win = malloc((size_t)wn * sizeof(Val));
        for (int j = 0; j < (int)wn; j++) win[j] = val_clone(sl->data[i + j]);
        out[i] = VAL_SLICE(slice_new(win, (int)wn));
        free(win);
    }
    val_free(s);
    uint32_t ns = slice_new(out, n);
    free(out);
    push(VAL_SLICE(ns));
}


static void p_index_of(void) {
    Val s = pop_slice();
    Val needle = pop();
    SliceObj *sl = &slices[s.slice];
    int64_t result = -1;
    for (int i = 0; i < sl->len; i++) {
        if (val_eq(sl->data[i], needle)) { result = i; break; }
    }
    val_free(needle); val_free(s);
    push(VAL_INT(result));
}

static bool val_lt(Val a, Val b) {
    if (a.type == T_INT && b.type == T_INT) return a.ival < b.ival;
    if (a.type == T_FLOAT && b.type == T_FLOAT) return a.fval < b.fval;
    slap_panic("rise/fall: elements must be Int or Float");
    return false;
}

static void argsort(bool asc) {
    Val s = pop_slice();
    SliceObj *sl = &slices[s.slice];
    int n = sl->len;
    Val *out = n > 0 ? malloc((size_t)n * sizeof(Val)) : NULL;
    for (int i = 0; i < n; i++) out[i] = VAL_INT(i);
    for (int i = 1; i < n; i++) {
        int64_t key = out[i].ival;
        int j = i - 1;
        while (j >= 0 && (asc ? val_lt(sl->data[key], sl->data[out[j].ival])
                               : val_lt(sl->data[out[j].ival], sl->data[key]))) {
            out[j + 1] = out[j]; j--;
        }
        out[j + 1] = VAL_INT(key);
    }
    val_free(s);
    uint32_t ns = slice_new(out, n);
    free(out);
    push(VAL_SLICE(ns));
}
static void p_rise(void) { argsort(true); }
static void p_fall(void) { argsort(false); }

static void p_reshape(void) {
    int64_t cols = pop_int();
    Val s = pop_slice();
    SliceObj *sl = &slices[s.slice];
    if (cols <= 0) slap_panic("reshape: column count must be positive, got %lld", (long long)cols);
    if (sl->len % (int)cols != 0) slap_panic("reshape: slice length %d is not divisible by %lld", sl->len, (long long)cols);
    int rows = sl->len / (int)cols;
    Val *out = rows > 0 ? malloc((size_t)rows * sizeof(Val)) : NULL;
    for (int r = 0; r < rows; r++) {
        Val *row = malloc((size_t)cols * sizeof(Val));
        for (int c = 0; c < (int)cols; c++) row[c] = val_clone(sl->data[r * (int)cols + c]);
        out[r] = VAL_SLICE(slice_new(row, (int)cols));
        free(row);
    }
    val_free(s);
    uint32_t ns = slice_new(out, rows);
    free(out);
    push(VAL_SLICE(ns));
}

static void p_transpose(void) {
    Val s = pop_slice();
    SliceObj *sl = &slices[s.slice];
    int rows = sl->len;
    if (rows == 0) { val_free(s); push(VAL_SLICE(slice_new(NULL, 0))); return; }
    if (sl->data[0].type != T_SLICE) slap_panic("transpose: expected Slice(Slice), got Slice(%s)", type_name(sl->data[0].type));
    int cols = slices[sl->data[0].slice].len;
    for (int r = 1; r < rows; r++) {
        if (sl->data[r].type != T_SLICE) slap_panic("transpose: expected Slice(Slice), got Slice(%s) at row %d", type_name(sl->data[r].type), r);
        if (slices[sl->data[r].slice].len != cols)
            slap_panic("transpose: rows have different lengths (%d vs %d) — cannot transpose a ragged array", cols, slices[sl->data[r].slice].len);
    }
    Val *out = cols > 0 ? malloc((size_t)cols * sizeof(Val)) : NULL;
    for (int c = 0; c < cols; c++) {
        Val *col = malloc((size_t)rows * sizeof(Val));
        for (int r = 0; r < rows; r++) col[r] = val_clone(slices[sl->data[r].slice].data[c]);
        out[c] = VAL_SLICE(slice_new(col, rows));
        free(col);
    }
    val_free(s);
    uint32_t ns = slice_new(out, cols);
    free(out);
    push(VAL_SLICE(ns));
}

static void p_shape(void) {
    Val s = pop_slice();
    SliceObj *sl = &slices[s.slice];
    int outer = sl->len;
    bool is_2d = outer > 0 && sl->data[0].type == T_SLICE;
    if (is_2d) {
        int inner = slices[sl->data[0].slice].len;
        for (int i = 1; i < outer; i++) {
            if (sl->data[i].type != T_SLICE || slices[sl->data[i].slice].len != inner) { is_2d = false; break; }
        }
        if (is_2d) {
            val_free(s);
            Val dims[2] = { VAL_INT(outer), VAL_INT(inner) };
            push(VAL_SLICE(slice_new(dims, 2)));
            return;
        }
    }
    val_free(s);
    Val dim = VAL_INT(outer);
    push(VAL_SLICE(slice_new(&dim, 1)));
}

static void p_classify(void) {
    Val s = pop_slice();
    SliceObj *sl = &slices[s.slice];
    int n = sl->len;
    Val *out = n > 0 ? malloc((size_t)n * sizeof(Val)) : NULL;
    Val *uniq = n > 0 ? malloc((size_t)n * sizeof(Val)) : NULL;
    int nuniq = 0;
    for (int i = 0; i < n; i++) {
        int found = -1;
        for (int j = 0; j < nuniq; j++) {
            if (val_eq(sl->data[i], uniq[j])) { found = j; break; }
        }
        if (found < 0) { uniq[nuniq] = sl->data[i]; found = nuniq++; }
        out[i] = VAL_INT(found);
    }
    free(uniq);
    val_free(s);
    uint32_t ns = slice_new(out, n);
    free(out);
    push(VAL_SLICE(ns));
}

static void p_pick(void) {
    Val indices = pop_slice();
    Val data = pop();
    SliceObj *idx = &slices[indices.slice];
    Val cur = val_clone(data);
    for (int i = 0; i < idx->len; i++) {
        if (idx->data[i].type != T_INT) slap_panic("pick: indices must be Int, got %s", type_name(idx->data[i].type));
        if (cur.type != T_SLICE) slap_panic("pick: cannot index into %s at depth %d", type_name(cur.type), i);
        SliceObj *sl = &slices[cur.slice];
        int64_t ix = idx->data[i].ival;
        if (ix < 0 || ix >= sl->len) slap_panic("pick: index %lld out of bounds [0, %d) at depth %d", (long long)ix, sl->len, i);
        Val next = val_clone(sl->data[ix]);
        val_free(cur);
        cur = next;
    }
    val_free(indices); val_free(data);
    push(cur);
}

static void p_group(void) {
    Val idx_s = pop_slice();
    Val data_s = pop_slice();
    SliceObj *idx = &slices[idx_s.slice];
    SliceObj *dat = &slices[data_s.slice];
    if (idx->len != dat->len) slap_panic("group: slices must have equal length (%d vs %d)", idx->len, dat->len);
    int n = idx->len;
    int ngroups = 0;
    for (int i = 0; i < n; i++) {
        if (idx->data[i].type != T_INT) slap_panic("group: indices must be Int, got %s", type_name(idx->data[i].type));
        int64_t g = idx->data[i].ival;
        if (g < 0) continue;
        if (g + 1 > ngroups) ngroups = (int)(g + 1);
    }
    int *counts = calloc((size_t)ngroups, sizeof(int));
    for (int i = 0; i < n; i++) { int64_t g = idx->data[i].ival; if (g >= 0) counts[g]++; }
    Val **bufs = malloc((size_t)ngroups * sizeof(Val *));
    int *pos = calloc((size_t)ngroups, sizeof(int));
    for (int g = 0; g < ngroups; g++) bufs[g] = counts[g] > 0 ? malloc((size_t)counts[g] * sizeof(Val)) : NULL;
    for (int i = 0; i < n; i++) {
        int64_t g = idx->data[i].ival;
        if (g < 0) continue;
        bufs[g][pos[g]++] = val_clone(dat->data[i]);
    }
    Val *out = ngroups > 0 ? malloc((size_t)ngroups * sizeof(Val)) : NULL;
    for (int g = 0; g < ngroups; g++) {
        out[g] = VAL_SLICE(slice_new(bufs[g], counts[g]));
        free(bufs[g]);
    }
    free(bufs); free(pos); free(counts);
    val_free(idx_s); val_free(data_s);
    uint32_t ns = slice_new(out, ngroups);
    free(out);
    push(VAL_SLICE(ns));
}

static void p_partition(void) {
    Val marks_s = pop_slice();
    Val data_s = pop_slice();
    SliceObj *marks = &slices[marks_s.slice];
    SliceObj *dat = &slices[data_s.slice];
    if (marks->len != dat->len) slap_panic("partition: slices must have equal length (%d vs %d)", marks->len, dat->len);
    int n = marks->len;
    // count groups: a new group starts when marker is nonzero and differs from previous
    int ngroups = 0;
    for (int i = 0; i < n; i++) {
        if (marks->data[i].type != T_INT) slap_panic("partition: markers must be Int, got %s", type_name(marks->data[i].type));
        int64_t m = marks->data[i].ival;
        if (m != 0 && (i == 0 || marks->data[i - 1].ival != m)) ngroups++;
    }
    Val *out = ngroups > 0 ? malloc((size_t)ngroups * sizeof(Val)) : NULL;
    int gi = -1;
    int cap = 0, len = 0;
    Val *buf = NULL;
    for (int i = 0; i < n; i++) {
        int64_t m = marks->data[i].ival;
        if (m == 0) {
            if (len > 0) { out[gi] = VAL_SLICE(slice_new(buf, len)); free(buf); buf = NULL; len = 0; cap = 0; }
            continue;
        }
        if (i == 0 || marks->data[i - 1].ival != m) {
            if (len > 0) { out[gi] = VAL_SLICE(slice_new(buf, len)); free(buf); buf = NULL; len = 0; cap = 0; }
            gi++;
        }
        if (len >= cap) { cap = cap ? cap * 2 : 8; buf = realloc(buf, (size_t)cap * sizeof(Val)); }
        buf[len++] = val_clone(dat->data[i]);
    }
    if (len > 0) { out[gi] = VAL_SLICE(slice_new(buf, len)); free(buf); }
    val_free(marks_s); val_free(data_s);
    uint32_t ns = slice_new(out, ngroups);
    free(out);
    push(VAL_SLICE(ns));
}

// -- Dices --
static void p_dice(void) {
    Val s = pop_slice();
    SliceObj *sl = &slices[s.slice];
    int n = sl->len;
    Val *data = malloc((size_t)(2 * n) * sizeof(Val));
    int count = 0;
    for (int i = 0; i < n; i++) {
        Val elem = sl->data[i];
        if (elem.type != T_TUPLE) slap_panic("dice: elements must be tuples, got %s", type_name(elem.type));
        int base = sp;
        exec_tuple(val_clone(elem));
        if (sp - base != 2) slap_panic("dice: each tuple must produce 2 values (key, value), got %d", sp - base);
        data[count * 2] = stack[base];
        data[count * 2 + 1] = stack[base + 1];
        sp = base;
        count++;
    }
    val_free(s);
    uint32_t di = slice_new(data, count * 2);
    free(data);
    push(VAL_DICE(di));
}

static void p_grab(void) {
    Val def = pop();
    Val key = pop();
    Val d = pop();
    EXPECT(d, T_DICE);
    SliceObj *sl = &slices[d.slice];
    for (int i = 0; i < sl->len; i += 2) {
        if (val_eq(sl->data[i], key)) {
            push(val_clone(sl->data[i + 1]));
            val_free(d); val_free(key); val_free(def);
            return;
        }
    }
    val_free(d); val_free(key);
    push(def);
}

static void p_ifsert(void) {
    Val val = pop();
    Val key = pop();
    Val d = pop();
    EXPECT(d, T_DICE);
    SliceObj *sl = &slices[d.slice];
    // check if key exists
    for (int i = 0; i < sl->len; i += 2) {
        if (val_eq(sl->data[i], key)) {
            // update: create new dice with replaced value
            Val *data = malloc((size_t)sl->len * sizeof(Val));
            for (int j = 0; j < sl->len; j++) {
                if (j == i) data[j] = val_clone(key);
                else if (j == i + 1) data[j] = val;
                else data[j] = val_clone(sl->data[j]);
            }
            uint32_t nd = slice_new(data, sl->len);
            free(data);
            val_free(d); val_free(key);
            push(VAL_DICE(nd));
            return;
        }
    }
    // insert: append new key-value pair
    Val *data = malloc((size_t)(sl->len + 2) * sizeof(Val));
    for (int j = 0; j < sl->len; j++) data[j] = val_clone(sl->data[j]);
    data[sl->len] = key;
    data[sl->len + 1] = val;
    uint32_t nd = slice_new(data, sl->len + 2);
    free(data);
    val_free(d);
    push(VAL_DICE(nd));
}

// -- Memory --
static Val linear_snapshot(Val v) {
    switch (v.type) {
        case T_BOX: {
            if (!heap[v.box].alive) slap_panic("use-after-free on box %u", v.box);
            return val_clone(heap[v.box].val);
        }
        case T_LIST: {
            ListObj *li = &lists[v.list];
            SliceObj tmp = {li->data, li->len, 0};
            return VAL_SLICE(slice_clone_range(&tmp, 0, li->len));
        }
        case T_DICT: {
            DictObj *di = &dicts[v.dict];
            int n = 0;
            for (int i = 0; i < di->cap; i++) if (di->used[i]) n++;
            Val *d = n > 0 ? malloc((size_t)(2 * n) * sizeof(Val)) : NULL;
            int k = 0;
            for (int i = 0; i < di->cap; i++) {
                if (di->used[i]) {
                    d[k * 2] = val_clone(di->keys[i]);
                    d[k * 2 + 1] = val_clone(di->vals[i]);
                    k++;
                }
            }
            uint32_t s = slice_new(d, 2 * n); free(d);
            return VAL_DICE(s);
        }
        case T_STR: {
            StrObj *st = &strs[v.str];
            Val *d = st->len > 0 ? malloc((size_t)st->len * sizeof(Val)) : NULL;
            for (int i = 0; i < st->len; i++) d[i] = VAL_INT(st->data[i]);
            uint32_t s = slice_new(d, st->len); free(d);
            return VAL_SLICE(s);
        }
        default: slap_panic("linear_snapshot: unreachable"); __builtin_unreachable();
    }
}

static void p_lend(void) {
    Val body = pop_tuple();
    Val linear = pop();
    if (!val_is_linear(linear)) slap_panic("%s: expected linear type, got %s", current_word, type_name(linear.type));

    Val snapshot = linear_snapshot(linear);

    int base = sp;
    push(snapshot);
    exec_tuple(body); val_free(body);
    // insert linear below body results
    if (sp >= STACK_MAX) slap_panic("stack overflow in lend");
    for (int i = sp; i > base; i--) stack[i] = stack[i - 1];
    stack[base] = linear;
    sp++;
}

static void p_clone(void) {
    Val v = pop();
    if (!val_is_linear(v)) slap_panic("%s: expected linear type, got %s", current_word, type_name(v.type));
    Val copy;
    switch (v.type) {
        case T_BOX: {
            if (!heap[v.box].alive) slap_panic("clone: use-after-free on box %u", v.box);
            copy = VAL_BOX(heap_alloc(val_clone(heap[v.box].val)));
            break;
        }
        case T_LIST: {
            ListObj *li = &lists[v.list];
            uint32_t nl = list_new(NULL, li->len);
            for (int i = 0; i < li->len; i++) lists[nl].data[i] = val_clone(li->data[i]);
            copy = VAL_LIST(nl);
            break;
        }
        case T_DICT: {
            DictObj *di = &dicts[v.dict];
            uint32_t nd = dict_new();
            for (int i = 0; i < di->cap; i++)
                if (di->used[i]) dict_set(nd, val_clone(di->keys[i]), val_clone(di->vals[i]));
            copy = VAL_DICT(nd);
            break;
        }
        case T_STR: {
            StrObj *st = &strs[v.str];
            copy = VAL_STR(str_new(st->data, st->len));
            break;
        }
        default: slap_panic("clone: unreachable");
    }
    push(v);
    push(copy);
}

static void p_free(void) {
    Val v = pop();
    if (!val_is_linear(v)) slap_panic("%s: expected linear type, got %s", current_word, type_name(v.type));
    val_free(v);
}

// -- Box --
static void p_box(void) { push(VAL_BOX(heap_alloc(pop()))); }

// -- Lists --
static void p_list(void) {
    Val s = pop_slice();
    SliceObj *sl = &slices[s.slice];
    uint32_t li = list_new(NULL, sl->len);
    for (int i = 0; i < sl->len; i++) lists[li].data[i] = val_clone(sl->data[i]);
    val_free(s);
    push(VAL_LIST(li));
}

static void p_list_zero(void) {
    int64_t n = pop_int();
    if (n < 0) slap_panic("list-zero: negative size %lld", (long long)n);
    uint32_t li = list_new(NULL, (int)n);
    for (int i = 0; i < (int)n; i++) lists[li].data[i] = VAL_INT(0);
    push(VAL_LIST(li));
}

static void p_list_concat(void) {
    Val s = pop_slice();
    Val l = pop();
    EXPECT(l, T_LIST);
    SliceObj *sl = &slices[s.slice];
    ListObj *li = &lists[l.list];
    int new_len = li->len + sl->len;
    if (new_len > li->cap) {
        li->cap = new_len > li->cap * 2 ? new_len : li->cap * 2;
        li->data = realloc(li->data, (size_t)li->cap * sizeof(Val));
    }
    for (int i = 0; i < sl->len; i++) li->data[li->len + i] = val_clone(sl->data[i]);
    li->len = new_len;
    val_free(s);
    push(l);
}

static void p_list_assign(void) {
    Val val = pop();
    int64_t idx = pop_int();
    Val l = pop();
    EXPECT(l, T_LIST);
    ListObj *li = &lists[l.list];
    if (idx < 0 || idx >= li->len) slap_panic("list-assign: index %lld out of bounds [0, %d)", (long long)idx, li->len);
    val_free(li->data[idx]);
    li->data[idx] = val;
    push(l);
}

static void p_list_at(void) {
    Val def = pop();
    int64_t idx = pop_int();
    Val l = pop();
    EXPECT(l, T_LIST);
    ListObj *li = &lists[l.list];
    if (idx < 0 || idx >= li->len) {
        push(l);
        push(def);
    } else {
        val_free(def);
        push(l);
        push(val_clone(li->data[idx]));
    }
}

// -- Dicts (linear) --
static void p_dict(void) {
    Val d = pop();
    EXPECT(d, T_DICE);
    SliceObj *sl = &slices[d.slice];
    uint32_t di = dict_new();
    for (int i = 0; i < sl->len; i += 2)
        dict_set(di, val_clone(sl->data[i]), val_clone(sl->data[i + 1]));
    val_free(d);
    push(VAL_DICT(di));
}

static void p_dict_insert(void) {
    Val val = pop();
    Val key = pop();
    Val d = pop();
    EXPECT(d, T_DICT);
    dict_set(d.dict, key, val);
    push(d);
}

static void p_dict_remove(void) {
    Val key = pop();
    Val d = pop();
    EXPECT(d, T_DICT);
    if (!dict_remove(d.dict, key)) val_free(key);
    push(d);
}

// -- Strings --
static void p_str(void) {
    Val s = pop_slice();
    SliceObj *sl = &slices[s.slice];
    uint8_t *data = malloc((size_t)sl->len);
    for (int i = 0; i < sl->len; i++) {
        if (sl->data[i].type != T_INT) slap_panic("str: slice elements must be Int (char codes), got %s", type_name(sl->data[i].type));
        data[i] = (uint8_t)sl->data[i].ival;
    }
    uint32_t st = str_new(data, sl->len);
    free(data);
    val_free(s);
    push(VAL_STR(st));
}

static void p_str_concat(void) {
    Val s = pop_slice();
    Val st = pop();
    EXPECT(st, T_STR);
    SliceObj *sl = &slices[s.slice];
    StrObj *so = &strs[st.str];
    int new_len = so->len + sl->len;
    if (new_len > so->cap) {
        so->cap = new_len > so->cap * 2 ? new_len : so->cap * 2;
        so->data = realloc(so->data, (size_t)so->cap);
    }
    for (int i = 0; i < sl->len; i++) {
        if (sl->data[i].type != T_INT) slap_panic("str-concat: elements must be Int, got %s", type_name(sl->data[i].type));
        so->data[so->len + i] = (uint8_t)sl->data[i].ival;
    }
    so->len = new_len;
    val_free(s);
    push(st);
}

static void p_str_assign(void) {
    int64_t ch = pop_int();
    int64_t idx = pop_int();
    Val st = pop();
    EXPECT(st, T_STR);
    StrObj *so = &strs[st.str];
    if (idx < 0 || idx >= so->len) slap_panic("str-assign: index %lld out of bounds [0, %d)", (long long)idx, so->len);
    so->data[idx] = (uint8_t)ch;
    push(st);
}

// -- Meta --
static void p_def(void) {
    Val val = pop();
    uint32_t name = pop_sym();
    ensure_own_scope();
    scope_bind(g_scope, name, val, true);
}

static void p_let(void) {
    uint32_t name = pop_sym();
    Val val = pop();
    ensure_own_scope();
    scope_bind(g_scope, name, val, false);
}

static void p_recur(void) {
    Val sym = peek();
    EXPECT(sym, T_SYM);
    recur_pending = sym.sym;
}

// -- IO --
static void p_print(void) {
    Val v = pop();
    val_print(v, stdout);
    printf("\n");
    val_free(v);
}

static void p_print_stack(void) {
    printf("<%d> ", sp);
    for (int i = 0; i < sp; i++) { val_print(stack[i], stdout); printf(" "); }
    printf("\n");
}

static void p_assert(void) {
    int64_t v = pop_int();
    if (v == 0) slap_panic("assertion failed");
}

static void p_halt(void) { exit(0); }

// -- Random --
static void p_random(void) {
    int64_t max = pop_int();
    if (max <= 0) slap_panic("random: max must be positive, got %lld", (long long)max);
    push(VAL_INT((int64_t)(rand() % (unsigned)max)));
}

// ── Primitive registration ──────────────────────────────────────────────────

#define PRIMITIVES \
    X("dup", p_dup) X("drop", p_drop) X("swap", p_swap) X("dip", p_dip) \
    X("apply", p_apply) X("if", p_if) X("loop", p_loop) X("while", p_while) X("cond", p_cond) X("match", p_match) \
    X("not", p_not) X("and", p_and) X("or", p_or) \
    X("eq", p_eq) X("lt", p_lt) \
    X("plus", p_plus) X("sub", p_sub) X("mul", p_mul) X("div", p_div) X("mod", p_mod) \
    X("iplus", p_iplus) X("isub", p_isub) X("imul", p_imul) X("idiv", p_idiv) X("imod", p_imod) \
    X("fplus", p_fplus) X("fsub", p_fsub) X("fmul", p_fmul) X("fdiv", p_fdiv) \
    X("fsqrt", p_fsqrt) X("fsin", p_fsin) X("fcos", p_fcos) X("ftan", p_ftan) \
    X("ffloor", p_ffloor) X("fceil", p_fceil) X("fround", p_fround) \
    X("fexp", p_fexp) X("flog", p_flog) X("fpow", p_fpow) X("fatan2", p_fatan2) \
    X("itof", p_itof) X("ftoi", p_ftoi) \
    X("compose", p_compose) X("cons", p_cons) X("car", p_car) \
    X("rec", p_rec) X("get", p_get) X("set", p_set) \
    X("len", p_len) X("fold", p_fold) X("reduce", p_reduce) X("at", p_at) X("put", p_put) \
    X("each", p_each) X("map", p_map) X("filter", p_filter) X("range", p_range) \
    X("sort", p_sort) X("cat", p_cat) \
    X("take", p_take) X("drop-n", p_drop_n) \
    X("scan", p_scan) X("rotate", p_rotate) X("select", p_select) \
    X("keep-mask", p_keep_mask) X("windows", p_windows) \
    X("rise", p_rise) X("fall", p_fall) X("index-of", p_index_of) \
    X("reshape", p_reshape) X("transpose", p_transpose) X("shape", p_shape) X("classify", p_classify) X("pick", p_pick) \
    X("group", p_group) X("partition", p_partition) \
    X("dice", p_dice) X("grab", p_grab) X("ifsert", p_ifsert) \
    X("lend", p_lend) X("clone", p_clone) X("free", p_free) \
    X("box", p_box) \
    X("list", p_list) X("list-zero", p_list_zero) X("list-concat", p_list_concat) X("list-assign", p_list_assign) X("list-at", p_list_at) \
    X("dict", p_dict) X("dict-insert", p_dict_insert) X("dict-remove", p_dict_remove) \
    X("str", p_str) X("str-concat", p_str_concat) X("str-assign", p_str_assign) \
    X("def", p_def) X("let", p_let) X("recur", p_recur) \
    X("print", p_print) X("print-stack", p_print_stack) X("assert", p_assert) X("halt", p_halt) \
    X("random", p_random)

static void init_primitives(void) {
    #define X(name, fn) prim_table[sym_intern(name)] = fn;
    PRIMITIVES
    #undef X
    sym_def_id = sym_intern("def");
    sym_let_id = sym_intern("let");
    sym_recur_id = sym_intern("recur");
}


static void resolve_cached_prims(int start, int len) {
    for (int i = start; i < start + len; ) {
        Node *n = &nodes[i];
        if (n->type == N_WORD) {
            uint32_t s = n->sym;
            n->cached_prim = (s < MAX_SYMS && prim_table[s] && !scope_find(global_scope, s))
                             ? prim_table[s] : NULL;
            i++;
        } else if (n->type == N_TUPLE || n->type == N_SLICE || n->type == N_RECORD) {
            i += 1 + n->body.len;
        } else {
            i++;
        }
    }
}

// ── Eval ────────────────────────────────────────────────────────────────────

static void eval(int start, int len, uint32_t scope) {
    uint32_t prev_scope = g_scope;
    g_scope = scope;
    for (int i = start; i < start + len; ) {
        Node *n = &nodes[i];
        switch (n->type) {
            case N_PUSH: {
                Val v = n->literal;
                if (v.type <= T_SYM) push(v);
                else push(val_clone(v));
                i++;
                break;
            }
            case N_WORD: {
                PrimFn pf = n->cached_prim;
                if (pf) {
                    Scope *sc = &scopes[g_scope];
                    int k = sc->count;
                    while (k-- > 0) {
                        if (sc->binds[k].name == n->sym) goto word_slow;
                    }
                    pf();
                    i++;
                    break;
                }
                word_slow:
                panic_line = n->line;
                panic_col = n->col;
                current_word = sym_names[n->sym];
                Binding *b = scope_find(g_scope, n->sym);
                if (b) {
                    if (b->is_def && b->val.type == T_TUPLE) {
                        exec_tuple(b->val);
                    } else {
                        push(val_clone(b->val));
                    }
                } else if (n->sym < MAX_SYMS && prim_table[n->sym]) {
                    prim_table[n->sym]();
                } else {
                    slap_panic("unbound word: %s", sym_names[n->sym]);
                }
                current_word = "";
                i++;
                break;
            }
            case N_SLICE: {
                uint32_t saved_lazy = g_lazy_parent;
                uint32_t child = scope_new(g_scope);
                g_lazy_parent = NONE;
                int base = sp;
                eval(n->body.start, n->body.len, child);
                scope_release(child);
                g_lazy_parent = saved_lazy;
                int count = sp - base;
                Val *data = count > 0 ? malloc((size_t)count * sizeof(Val)) : NULL;
                for (int j = 0; j < count; j++) data[j] = stack[base + j];
                sp = base;
                uint32_t s = slice_new(data, count);
                free(data);
                push(VAL_SLICE(s));
                i += 1 + n->body.len;
                break;
            }
            case N_TUPLE: {
                uint32_t env = g_scope;
                bool owns = false;
                if (g_scope != global_scope) { env = scope_snapshot(g_scope); owns = true; }
                uint32_t t = tuple_new(n->body.start, n->body.len, env, owns);
                if (recur_pending != NONE) {
                    if (g_scope != global_scope)
                        scope_bind(env, recur_pending, val_clone(VAL_TUPLE(t)), true);
                    recur_pending = NONE;
                }
                push(VAL_TUPLE(t));
                i += 1 + n->body.len;
                break;
            }
            case N_RECORD: {
                uint32_t saved_lazy = g_lazy_parent;
                uint32_t child = scope_new(g_scope);
                g_lazy_parent = NONE;
                int base = sp;
                eval(n->body.start, n->body.len, child);
                scope_release(child);
                g_lazy_parent = saved_lazy;
                int count = sp - base;
                if (count % 2 != 0) slap_panic("record literal must have even number of elements (key-value pairs), got %d", count);
                uint32_t r = rec_new();
                for (int j = base; j < sp; j += 2) {
                    if (stack[j].type != T_SYM) slap_panic("record keys must be symbols, got %s", type_name(stack[j].type));
                    rec_set(r, stack[j].sym, stack[j + 1]);
                }
                sp = base;
                push(VAL_REC(r));
                i += 1 + n->body.len;
                break;
            }
        }
    }
    // Clean up lazily-created child scope (if any)
    if (g_scope != scope) scope_release(g_scope);
    g_scope = prev_scope;
}

// ── Prelude ─────────────────────────────────────────────────────────────────

static const char *prelude =
    "'peek   (() lend) def\n"
    "'inc    (1 iplus) def\n"
    "'dec    (1 isub) def\n"
    "'neg    (0 swap isub) def\n"
    "'abs    (dup 0 lt (neg) () if) def\n"
    "'over   (swap dup (swap) dip) def\n"
    "'nip    (swap drop) def\n"
    "'rot    ((swap) dip swap) def\n"
    "'keep   (over (apply) dip) def\n"
    "'bi     ((keep) dip apply) def\n"
    "'iszero (0 eq) def\n"
    "'ispos  (0 lt not) def\n"
    "'isneg  (0 lt) def\n"
    "'iseven (2 imod 0 eq) def\n"
    "'isodd  (2 imod 1 eq) def\n"
    "'max    (over over lt (nip) (drop) if) def\n"
    "'min    (over over lt (drop) (nip) if) def\n"
    "'neq    (eq not) def\n"
    "'gt     (swap lt) def\n"
    "'ge     (lt not) def\n"
    "'le     (swap lt not) def\n"
    // iteration
    "'sum       (0 (iplus) fold) def\n"
    "'product   (1 (imul) fold) def\n"
    "'isany     ('p swap def (p) filter len 0 gt) def\n"
    "'isall     ('p swap def (p not) filter len iszero) def\n"
    "'count     ('p swap def (p) filter len) def\n"
    "'first     (0 swap at) def\n"
    "'last      (swap dup len 1 isub rot at) def\n"
    // arithmetic
    "'sqr       (dup imul) def\n"
    "'cube      (dup dup imul imul) def\n"
    // float
    "'fneg      (0.0 swap fsub) def\n"
    "'fabs      (dup 0.0 lt (fneg) () if) def\n"
    "'fclamp    ('hi let 'lo let lo max hi min) def\n"
    "'lerp      ('t let swap 1.0 t fsub fmul swap t fmul fplus) def\n"
    // constants
    "3.14159265358979323846 'pi let\n"
    "6.28318530717958647692 'tau let\n"
    // misc
    "'clamp     ('hi let 'lo let lo max hi min) def\n"
    "'isbetween ('hi let 'lo let dup lo ge swap hi le and) def\n"
    "'sign      (dup 0 lt (-1) (dup 0 gt (1) (0) if) if nip) def\n"
    "'repeat    ('n let 'f swap def n (dup 0 gt (1 isub (f) dip 1) (0) if) loop drop) def\n"
    "'reverse   ('s let 0 s len range (s len 1 isub swap isub s swap -1 at) map) def\n"
    "'flatten   ([] (cat) fold) def\n"
    "'zip       ('b let 'a let 0 a len b len min range ('i let [a i -1 at b i -1 at]) map) def\n"
    "'where     ('s let 0 s len range (s swap 0 at) filter) def\n"
    "'member    ('hs let ('n let hs (n eq) isany) map) def\n"
    "'dedup     ('s let s [] ('e let dup (e eq) isany not ([e] cat) () if) fold) def\n"
    "'table     ('f let 'bs let ('a let bs (a swap f apply) map) map) def\n"
    "'sort-desc (sort reverse) def\n"
    "'max-of    ((max) reduce) def\n"
    "'min-of    ((min) reduce) def\n"
    // more float
    "'frecip  (1.0 swap fdiv) def\n"
    "'fsign   (dup 0.0 lt (-1.0) (dup 0.0 eq (0.0) (1.0) if) if nip) def\n"
    // constants
    "2.71828182845904523536 'e let\n"
    // 2D array utilities
    "'couple  ('b let 'a let [a b]) def\n"
    "'find    ('needle let needle len windows (needle eq) map) def\n"
;

// ── Type Checker ────────────────────────────────────────────────────────────

#define MAX_TNODES  131072
#define MAX_TVARS   16384
#define MAX_TENVS   4096
#define MAX_TCOPY   4096
#define MAX_TERRS   64
#define TN_NONE     UINT32_MAX

typedef enum {
    TN_INT, TN_FLOAT, TN_SYM,
    TN_TUPLE, TN_REC, TN_SLICE, TN_DICE,
    TN_BOX, TN_LIST, TN_DICT, TN_STR,
    TN_VAR, TN_SCONS, TN_SVAR,
    TN_REMPTY, TN_REXT, TN_RVAR
} TNodeKind;

typedef struct {
    TNodeKind kind;
    union {
        uint32_t arg;                          // SLICE, BOX, LIST, REC
        struct { uint32_t key, val; } kv;      // DICE, DICT
        struct { uint32_t in, out; } tuple;    // TUPLE (stack effect)
        uint32_t var_id;                       // VAR, SVAR, RVAR
        struct { uint32_t head, tail; } scons; // SCONS
        struct { uint32_t label, type, tail; } rext; // REXT
    };
} TNode;

static TNode    tnodes[MAX_TNODES];
static int      tn_count;
static uint32_t tc_subst[MAX_TVARS];
static int      tc_next_var;

typedef struct { uint32_t var_node; const char *word; int line, col; bool handled; } CopyConst;
static CopyConst tc_cc[MAX_TCOPY];
static int tc_cc_count;
static CopyConst tc_lc[MAX_TCOPY];
static int tc_lc_count;

typedef struct {
    uint32_t name, type; bool poly, is_def, freed, used; int freed_line, freed_col;
    CopyConst poly_cc[8]; int poly_cc_n;
    CopyConst poly_lc[8]; int poly_lc_n;
} TCBinding;
typedef struct { uint32_t parent; TCBinding *binds; int count, cap; } TCEnv;
static TCEnv tenvs[MAX_TENVS];
static int tenv_count;

typedef struct { uint32_t tuple_node; int cc_start, cc_end, lc_start, lc_end; } PendingTupleCC;
static PendingTupleCC tc_pending[64];
static int tc_pending_n;

typedef struct { char msg[512]; int line, col, span; } TCError;
static TCError tc_errs[MAX_TERRS];
static int tc_err_count;
static bool tc_had_err;
static const char *tc_cur_word;
static int tc_cur_line, tc_cur_col;

typedef struct { uint32_t in, out; } StackEff;

static void tc_ut(uint32_t a, uint32_t b);
static void tc_ust(uint32_t a, uint32_t b);
static void tc_ur(uint32_t a, uint32_t b);

static void tc_err(const char *fmt, ...) {
    tc_had_err = true;
    if (tc_err_count >= MAX_TERRS) return;
    TCError *e = &tc_errs[tc_err_count++];
    e->line = tc_cur_line; e->col = tc_cur_col;
    e->span = (tc_cur_word && tc_cur_word[0]) ? (int)strlen(tc_cur_word) : 1;
    va_list ap; va_start(ap, fmt); vsnprintf(e->msg, sizeof(e->msg), fmt, ap); va_end(ap);
}

// TNode constructors
static uint32_t tn_new(TNodeKind k) {
    if (tn_count >= MAX_TNODES) slap_panic("type node pool full");
    tnodes[tn_count].kind = k;
    return (uint32_t)tn_count++;
}
static uint32_t tn_new_var(TNodeKind k) {
    if (tc_next_var >= MAX_TVARS) slap_panic("type variable pool full");
    uint32_t n = tn_new(k);
    tnodes[n].var_id = (uint32_t)tc_next_var;
    tc_subst[tc_next_var] = TN_NONE;
    tc_next_var++;
    return n;
}

#define tn_int()   tn_new(TN_INT)
#define tn_float() tn_new(TN_FLOAT)
#define tn_sym()   tn_new(TN_SYM)
#define tn_str()   tn_new(TN_STR)
#define tn_var()   tn_new_var(TN_VAR)
#define tn_svar()  tn_new_var(TN_SVAR)
#define tn_rvar()  tn_new_var(TN_RVAR)

static uint32_t tn_slice(uint32_t e) { uint32_t n=tn_new(TN_SLICE); tnodes[n].arg=e; return n; }
static uint32_t tn_list(uint32_t e) { uint32_t n=tn_new(TN_LIST); tnodes[n].arg=e; return n; }
static uint32_t tn_box(uint32_t e) { uint32_t n=tn_new(TN_BOX); tnodes[n].arg=e; return n; }
static uint32_t tn_rec(uint32_t r) { uint32_t n=tn_new(TN_REC); tnodes[n].arg=r; return n; }
static uint32_t tn_dice(uint32_t k, uint32_t v) { uint32_t n=tn_new(TN_DICE); tnodes[n].kv.key=k; tnodes[n].kv.val=v; return n; }
static uint32_t tn_dict_t(uint32_t k, uint32_t v) { uint32_t n=tn_new(TN_DICT); tnodes[n].kv.key=k; tnodes[n].kv.val=v; return n; }
static uint32_t tn_tuple(uint32_t in, uint32_t out) { uint32_t n=tn_new(TN_TUPLE); tnodes[n].tuple.in=in; tnodes[n].tuple.out=out; return n; }
static uint32_t tn_scons(uint32_t h, uint32_t t) { uint32_t n=tn_new(TN_SCONS); tnodes[n].scons.head=h; tnodes[n].scons.tail=t; return n; }
static uint32_t tn_rext(uint32_t l, uint32_t t, uint32_t tl) { uint32_t n=tn_new(TN_REXT); tnodes[n].rext.label=l; tnodes[n].rext.type=t; tnodes[n].rext.tail=tl; return n; }
static uint32_t tn_rempty(void) { return tn_new(TN_REMPTY); }

static uint32_t tn_resolve(uint32_t t) {
    TNode *n = &tnodes[t];
    if ((n->kind == TN_VAR || n->kind == TN_SVAR || n->kind == TN_RVAR) && tc_subst[n->var_id] != TN_NONE)
        return tn_resolve(tc_subst[n->var_id]);
    return t;
}

// Show types
static int tn_show(uint32_t t, char *buf, int cap) {
    t = tn_resolve(t);
    TNode *n = &tnodes[t];
    switch (n->kind) {
    case TN_INT: return snprintf(buf, cap, "Int");
    case TN_FLOAT: return snprintf(buf, cap, "Float");
    case TN_SYM: return snprintf(buf, cap, "Symbol");
    case TN_STR: return snprintf(buf, cap, "String");
    case TN_VAR: return snprintf(buf, cap, "?%u", n->var_id);
    case TN_SLICE: case TN_LIST: case TN_BOX: {
        const char *nm = n->kind==TN_SLICE ? "Slice" : n->kind==TN_LIST ? "List" : "Box";
        int o=snprintf(buf,cap,"%s<",nm); o+=tn_show(n->arg,buf+o,cap-o); o+=snprintf(buf+o,cap-o,">"); return o;
    }
    case TN_DICE: case TN_DICT: {
        const char *nm = n->kind==TN_DICE ? "Dice" : "Dict";
        int o=snprintf(buf,cap,"%s<",nm); o+=tn_show(n->kv.key,buf+o,cap-o); o+=snprintf(buf+o,cap-o,","); o+=tn_show(n->kv.val,buf+o,cap-o); o+=snprintf(buf+o,cap-o,">"); return o;
    }
    case TN_REC:   return snprintf(buf, cap, "Record");
    case TN_TUPLE: return snprintf(buf, cap, "Tuple");
    default: return snprintf(buf, cap, "?");
    }
}

// Occurs check
static bool tc_occurs(uint32_t var_id, TNodeKind vk, uint32_t t) {
    t = tn_resolve(t);
    TNode *n = &tnodes[t];
    if (n->kind == vk && (vk == TN_VAR || vk == TN_SVAR || vk == TN_RVAR) && n->var_id == var_id) return true;
    switch (n->kind) {
    case TN_SLICE: case TN_BOX: case TN_LIST: case TN_REC: return tc_occurs(var_id, vk, n->arg);
    case TN_DICE: case TN_DICT: return tc_occurs(var_id, vk, n->kv.key) || tc_occurs(var_id, vk, n->kv.val);
    case TN_TUPLE: return tc_occurs(var_id, vk, n->tuple.in) || tc_occurs(var_id, vk, n->tuple.out);
    case TN_SCONS: return tc_occurs(var_id, vk, n->scons.head) || tc_occurs(var_id, vk, n->scons.tail);
    case TN_REXT: return tc_occurs(var_id, vk, n->rext.type) || tc_occurs(var_id, vk, n->rext.tail);
    default: return false;
    }
}

// Unification
static void tc_ut(uint32_t a, uint32_t b) {
    if (tc_had_err) return;
    a = tn_resolve(a); b = tn_resolve(b);
    if (a == b) return;
    TNode *na = &tnodes[a], *nb = &tnodes[b];
    if (na->kind == TN_VAR) { if (tc_occurs(na->var_id, TN_VAR, b)) { tc_err("infinite type"); return; } tc_subst[na->var_id] = b; return; }
    if (nb->kind == TN_VAR) { if (tc_occurs(nb->var_id, TN_VAR, a)) { tc_err("infinite type"); return; } tc_subst[nb->var_id] = a; return; }
    if (na->kind != nb->kind) {
        char ab[128], bb[128]; tn_show(a, ab, sizeof(ab)); tn_show(b, bb, sizeof(bb));
        tc_err("expected %s, got %s", ab, bb); return;
    }
    switch (na->kind) {
    case TN_INT: case TN_FLOAT: case TN_SYM: case TN_STR: return;
    case TN_SLICE: case TN_BOX: case TN_LIST: tc_ut(na->arg, nb->arg); return;
    case TN_REC: tc_ur(na->arg, nb->arg); return;
    case TN_DICE: case TN_DICT: tc_ut(na->kv.key, nb->kv.key); tc_ut(na->kv.val, nb->kv.val); return;
    case TN_TUPLE: tc_ust(na->tuple.in, nb->tuple.in); tc_ust(na->tuple.out, nb->tuple.out); return;
    default: tc_err("cannot unify"); return;
    }
}

static void tc_ust(uint32_t a, uint32_t b) {
    if (tc_had_err) return;
    a = tn_resolve(a); b = tn_resolve(b);
    if (a == b) return;
    TNode *na = &tnodes[a], *nb = &tnodes[b];
    if (na->kind == TN_SVAR) { if (tc_occurs(na->var_id, TN_SVAR, b)) { tc_err("infinite stack"); return; } tc_subst[na->var_id] = b; return; }
    if (nb->kind == TN_SVAR) { if (tc_occurs(nb->var_id, TN_SVAR, a)) { tc_err("infinite stack"); return; } tc_subst[nb->var_id] = a; return; }
    if (na->kind == TN_SCONS && nb->kind == TN_SCONS) { tc_ut(na->scons.head, nb->scons.head); tc_ust(na->scons.tail, nb->scons.tail); return; }
    tc_err("stack shape mismatch");
}

static void tc_ur(uint32_t a, uint32_t b) {
    if (tc_had_err) return;
    a = tn_resolve(a); b = tn_resolve(b);
    if (a == b) return;
    TNode *na = &tnodes[a], *nb = &tnodes[b];
    if (na->kind == TN_RVAR) { if (tc_occurs(na->var_id, TN_RVAR, b)) { tc_err("infinite row"); return; } tc_subst[na->var_id] = b; return; }
    if (nb->kind == TN_RVAR) { if (tc_occurs(nb->var_id, TN_RVAR, a)) { tc_err("infinite row"); return; } tc_subst[nb->var_id] = a; return; }
    if (na->kind == TN_REMPTY && nb->kind == TN_REMPTY) return;
    if (na->kind == TN_REMPTY && nb->kind == TN_REXT) { tc_err("extra field '%s'", sym_names[nb->rext.label]); return; }
    if (na->kind == TN_REXT && nb->kind == TN_REMPTY) { tc_err("missing field '%s'", sym_names[na->rext.label]); return; }
    if (na->kind == TN_REXT && nb->kind == TN_REXT) {
        if (na->rext.label == nb->rext.label) { tc_ut(na->rext.type, nb->rext.type); tc_ur(na->rext.tail, nb->rext.tail); return; }
        uint32_t r = tn_rvar();
        tc_ur(na->rext.tail, tn_rext(nb->rext.label, nb->rext.type, r));
        tc_ur(nb->rext.tail, tn_rext(na->rext.label, na->rext.type, r));
    }
}

// Instantiation
typedef struct { uint32_t old_id, new_node; } InstEntry;
typedef struct { InstEntry e[256]; int n; } InstMap;

static uint32_t tc_inst_r(InstMap *m, uint32_t t) {
    t = tn_resolve(t);
    TNode *n = &tnodes[t];
    switch (n->kind) {
    case TN_INT: case TN_FLOAT: case TN_SYM: case TN_STR: case TN_REMPTY: return t;
    case TN_VAR: case TN_SVAR: case TN_RVAR: {
        for (int i = 0; i < m->n; i++) if (m->e[i].old_id == n->var_id) return m->e[i].new_node;
        uint32_t nv = tn_new_var(n->kind);
        if (m->n < 256) m->e[m->n++] = (InstEntry){n->var_id, nv};
        return nv;
    }
    case TN_SLICE: return tn_slice(tc_inst_r(m, n->arg));
    case TN_LIST:  return tn_list(tc_inst_r(m, n->arg));
    case TN_BOX:   return tn_box(tc_inst_r(m, n->arg));
    case TN_REC:   return tn_rec(tc_inst_r(m, n->arg));
    case TN_DICE:  return tn_dice(tc_inst_r(m, n->kv.key), tc_inst_r(m, n->kv.val));
    case TN_DICT:  return tn_dict_t(tc_inst_r(m, n->kv.key), tc_inst_r(m, n->kv.val));
    case TN_TUPLE: return tn_tuple(tc_inst_r(m, n->tuple.in), tc_inst_r(m, n->tuple.out));
    case TN_SCONS: return tn_scons(tc_inst_r(m, n->scons.head), tc_inst_r(m, n->scons.tail));
    case TN_REXT:  return tn_rext(n->rext.label, tc_inst_r(m, n->rext.type), tc_inst_r(m, n->rext.tail));
    }
    return t;
}


// Type environment
static uint32_t tenv_new(uint32_t parent) {
    if (tenv_count >= MAX_TENVS) slap_panic("type env pool full");
    int idx = tenv_count++;
    tenvs[idx] = (TCEnv){parent, NULL, 0, 0};
    return (uint32_t)idx;
}
static void tenv_bind(uint32_t env, uint32_t name, uint32_t type, bool poly, bool is_def) {
    TCEnv *e = &tenvs[env];
    for (int i = 0; i < e->count; i++) {
        if (e->binds[i].name == name) { e->binds[i] = (TCBinding){name, type, poly, is_def, false, 0, 0}; return; }
    }
    if (e->count >= e->cap) { e->cap = e->cap ? e->cap * 2 : 8; e->binds = realloc(e->binds, (size_t)e->cap * sizeof(TCBinding)); }
    e->binds[e->count++] = (TCBinding){name, type, poly, is_def, false, 0, 0};
}
typedef struct { uint32_t type; bool poly, is_def, freed, was_used, found; TCBinding *binding; } TLookup;
typedef struct { StackEff eff; uint32_t copy_var, linear_var; } PrimScheme;


// Known symbol tracking for def/let
typedef struct { uint32_t tnode; uint32_t sym_id; } KnownSym;
static uint32_t ks_lookup(KnownSym *ks, int n, uint32_t tn) {
    tn = tn_resolve(tn);
    for (int i = n - 1; i >= 0; i--) if (tn_resolve(ks[i].tnode) == tn) return ks[i].sym_id;
    return TN_NONE;
}

// Walk a row type (REXT chain) looking for a field by symbol ID.
// Returns the field's type node, or TN_NONE if not found.
// Sets *closed = true if the row ends in REMPTY (field provably absent).
static uint32_t tc_row_find(uint32_t row, uint32_t sym_id, bool *closed) {
    *closed = false;
    while (true) {
        row = tn_resolve(row);
        if (tnodes[row].kind == TN_REXT) {
            if (tnodes[row].rext.label == sym_id) return tnodes[row].rext.type;
            row = tnodes[row].rext.tail;
        } else if (tnodes[row].kind == TN_REMPTY) {
            *closed = true;
            return TN_NONE;
        } else {
            return TN_NONE;  // RVAR or other — can't determine
        }
    }
}

// Map linear type to its snapshot type (Box→inner, List→Slice, Dict→Dice, Str→Slice(Int))
// Returns TN_NONE if the type is not a known linear kind.
static uint32_t tc_snapshot_of(uint32_t resolved) {
    switch (tnodes[resolved].kind) {
    case TN_BOX:  return tnodes[resolved].arg;
    case TN_LIST: return tn_slice(tnodes[resolved].arg);
    case TN_DICT: return tn_dice(tnodes[resolved].kv.key, tnodes[resolved].kv.val);
    case TN_STR:  return tn_slice(tn_int());
    default:      return TN_NONE;
    }
}

static uint32_t tc_pop(uint32_t *cur) {
    uint32_t s = tn_resolve(*cur);
    if (tnodes[s].kind == TN_SCONS) { *cur = tnodes[s].scons.tail; return tnodes[s].scons.head; }
    uint32_t t = tn_var(), r = tn_svar();
    tc_ust(*cur, tn_scons(t, r));
    *cur = r;
    return t;
}

static uint32_t tc_lit_type(Val *v) {
    switch (v->type) {
    case T_INT: return tn_int();
    case T_FLOAT: return tn_float();
    case T_SYM: return tn_sym();
    case T_SLICE: return tn_slice(tn_var()); // string/slice literal
    default: return tn_var();
    }
}

// Inference
#define EMIT_CC(v,w) do { if (tc_cc_count < MAX_TCOPY) tc_cc[tc_cc_count++] = (CopyConst){v, w, n->line, n->col}; } while(0)
#define EMIT_LC(v,w) do { if (tc_lc_count < MAX_TCOPY) tc_lc[tc_lc_count++] = (CopyConst){v, w, n->line, n->col}; } while(0)
static StackEff tc_infer(int start, int len, uint32_t env, int depth) {
    uint32_t cur = tn_svar();
    uint32_t base = cur;
    KnownSym known[256];
    int known_n = 0;

    uint32_t sym_def = sym_intern("def"), sym_let = sym_intern("let"), sym_recur = sym_intern("recur"), sym_lend = sym_intern("lend");
    uint32_t sym_get = sym_intern("get"), sym_set = sym_intern("set"), sym_match = sym_intern("match"), sym_cond = sym_intern("cond");
    uint32_t sym_on = sym_intern("on"), sym_show = sym_intern("show");
    uint32_t tc_recur_sym = TN_NONE;

    for (int i = start; i < start + len && !tc_had_err; ) {
        Node *n = &nodes[i];
        tc_cur_line = n->line; tc_cur_col = n->col;

        if (n->type == N_PUSH) {
            uint32_t t = tc_lit_type(&n->literal);
            cur = tn_scons(t, cur);
            if (n->literal.type == T_SYM && known_n < 256) {
                known[known_n++] = (KnownSym){tn_resolve(tnodes[tn_resolve(cur)].scons.head), n->literal.sym};
            }
            i++;
        }
        else if (n->type == N_TUPLE) {
            int cc_mark = tc_cc_count, lc_mark = tc_lc_count;
            uint32_t child = tenv_new(env);
            uint32_t pre_bound = TN_NONE;
            if (tc_recur_sym != TN_NONE) {
                uint32_t gi = tn_svar(), go = tn_svar();
                pre_bound = tn_tuple(gi, go);
                tenv_bind(child, tc_recur_sym, pre_bound, true, true);
                tc_recur_sym = TN_NONE;
            }
            StackEff eff = tc_infer(n->body.start, n->body.len, child, depth + 1);
            if (pre_bound != TN_NONE) tc_ut(pre_bound, tn_tuple(eff.in, eff.out));
            uint32_t tup = tn_tuple(eff.in, eff.out);
            if (tc_pending_n < 64 && (tc_cc_count > cc_mark || tc_lc_count > lc_mark))
                tc_pending[tc_pending_n++] = (PendingTupleCC){tup, cc_mark, tc_cc_count, lc_mark, tc_lc_count};
            cur = tn_scons(tup, cur);
            i += 1 + n->body.len;
        }
        else if (n->type == N_SLICE) {
            StackEff eff = tc_infer(n->body.start, n->body.len, env, depth);
            uint32_t elem = tn_var();
            uint32_t s = eff.out;
            while (true) {
                s = tn_resolve(s);
                if (tnodes[s].kind == TN_SCONS) { tc_ut(tnodes[s].scons.head, elem); s = tnodes[s].scons.tail; }
                else break;
            }
            cur = tn_scons(tn_slice(elem), cur);
            i += 1 + n->body.len;
        }
        else if (n->type == N_RECORD) {
            int bs = n->body.start, bl = n->body.len;
            // Check if body is all literals (fast path)
            bool all_lit = true;
            for (int j = bs; j < bs + bl; j++) if (nodes[j].type != N_PUSH) { all_lit = false; break; }
            uint32_t row = all_lit ? tn_rempty() : tn_rvar();
            if (all_lit && bl % 2 == 0) {
                for (int j = bs; j < bs + bl; j += 2) {
                    if (nodes[j].literal.type == T_SYM)
                        row = tn_rext(nodes[j].literal.sym, tc_lit_type(&nodes[j+1].literal), row);
                }
            } else {
                // Infer body to type-check it, then build row from output
                StackEff eff = tc_infer(bs, bl, env, depth);
                uint32_t s = eff.out;
                // Collect stack types (top-first: stk[0]=top)
                uint32_t stk[256]; int stk_n = 0;
                while (true) {
                    s = tn_resolve(s);
                    if (tnodes[s].kind == TN_SCONS && stk_n < 256) {
                        stk[stk_n++] = tnodes[s].scons.head;
                        s = tnodes[s].scons.tail;
                    } else break;
                }
                // Record body pushes key,val pairs. Output (bottom-to-top):
                //   key0 val0 key1 val1 ...
                // stk (top-first): val_last key_last ... val0 key0
                // Pairs from bottom: stk[stk_n-1]=key0, stk[stk_n-2]=val0, ...
                if (stk_n >= 2 && stk_n % 2 == 0) {
                    for (int j = stk_n - 1; j >= 1; j -= 2) {
                        // stk[j] should be key type (Sym), stk[j-1] is value type
                        row = tn_rext(sym_intern("?"), stk[j - 1], row);
                    }
                }
            }
            cur = tn_scons(tn_rec(row), cur);
            i += 1 + bl;
        }
        else if (n->type == N_WORD) {
            tc_cur_word = sym_names[n->sym];
            // def: 'name val def → pops val then name
            if (n->sym == sym_def) {
                uint32_t val_t = tc_pop(&cur);
                uint32_t sym_t = tc_pop(&cur);
                tc_ut(sym_t, tn_sym());
                uint32_t ns = ks_lookup(known, known_n, sym_t);
                if (ns != TN_NONE) {
                    uint32_t rt = tn_resolve(val_t);
                    bool is_tup = tnodes[rt].kind == TN_TUPLE;
                    if (!is_tup) EMIT_CC(val_t, "def");
                    tenv_bind(env, ns, is_tup ? rt : val_t, depth == 0, true);
                    // Fix 3: propagate body constraints through polymorphic tuple bindings
                    if (is_tup && depth == 0) {
                        TCEnv *e = &tenvs[env];
                        TCBinding *b = &e->binds[e->count - 1];
                        for (int p = tc_pending_n - 1; p >= 0; p--) {
                            if (tc_pending[p].tuple_node == rt) {
                                for (int j = tc_pending[p].cc_start; j < tc_pending[p].cc_end && b->poly_cc_n < 8; j++) {
                                    b->poly_cc[b->poly_cc_n++] = tc_cc[j];
                                    tc_cc[j].handled = true;
                                }
                                for (int j = tc_pending[p].lc_start; j < tc_pending[p].lc_end && b->poly_lc_n < 8; j++) {
                                    b->poly_lc[b->poly_lc_n++] = tc_lc[j];
                                    tc_lc[j].handled = true;
                                }
                                break;
                            }
                        }
                    }
                }
                i++; continue;
            }
            // let: val 'name let → pops name then val
            if (n->sym == sym_let) {
                uint32_t sym_t = tc_pop(&cur);
                uint32_t val_t = tc_pop(&cur);
                tc_ut(sym_t, tn_sym());
                EMIT_CC(val_t, "let");
                uint32_t ns = ks_lookup(known, known_n, sym_t);
                if (ns != TN_NONE) tenv_bind(env, ns, val_t, false, false);
                i++; continue;
            }
            if (n->sym == sym_recur) {
                uint32_t s = tn_resolve(cur);
                if (tnodes[s].kind != TN_SCONS || tnodes[tn_resolve(tnodes[s].scons.head)].kind != TN_SYM)
                    tc_err("recur expects a Symbol on the stack");
                else
                    tc_recur_sym = ks_lookup(known, known_n, tn_resolve(tnodes[s].scons.head));
                i++; continue;
            }
            if (n->sym == sym_lend) {
                uint32_t s = tn_svar();
                uint32_t a = tn_var(), b = tn_var(), l = tn_var();
                tc_ust(cur, tn_scons(tn_tuple(tn_scons(a,s), tn_scons(b,s)), tn_scons(l, s)));
                EMIT_LC(l, "lend");
                uint32_t snap = tc_snapshot_of(tn_resolve(l));
                if (snap != TN_NONE) tc_ut(a, snap);
                cur = tn_scons(b, tn_scons(l, s));
                i++; continue;
            }
            // get: 'key record get -> value
            if (n->sym == sym_get) {
                uint32_t sym_t = tc_pop(&cur);
                uint32_t rec_t = tc_pop(&cur);
                tc_ut(sym_t, tn_sym());
                // unify with Record to catch type errors like "42 'a get"
                uint32_t row = tn_rvar();
                tc_ut(rec_t, tn_rec(row));
                uint32_t rec_r = tn_resolve(rec_t);
                uint32_t known_label = ks_lookup(known, known_n, sym_t);
                if (known_label != TN_NONE && tnodes[rec_r].kind == TN_REC) {
                    bool closed;
                    uint32_t field_t = tc_row_find(tnodes[rec_r].arg, known_label, &closed);
                    if (field_t != TN_NONE) {
                        EMIT_CC(field_t, "get");
                        cur = tn_scons(field_t, cur);
                    } else if (closed) {
                        tc_err("'get': field '%s not found in record", sym_names[known_label]);
                        cur = tn_scons(tn_var(), cur);
                    } else {
                        // open row — field might exist, return fresh var
                        uint32_t ft = tn_var();
                        // extend the row with this field
                        uint32_t new_row = tn_rext(known_label, ft, tn_rvar());
                        tc_ur(tnodes[rec_r].arg, new_row);
                        EMIT_CC(ft, "get");
                        cur = tn_scons(ft, cur);
                    }
                } else {
                    // fallback: opaque
                    cur = tn_scons(tn_var(), cur);
                }
                i++; continue;
            }
            // set: 'key value record set -> record
            if (n->sym == sym_set) {
                uint32_t sym_t = tc_pop(&cur);
                uint32_t val_t = tc_pop(&cur);
                uint32_t rec_t = tc_pop(&cur);
                tc_ut(sym_t, tn_sym());
                uint32_t set_row = tn_rvar();
                tc_ut(rec_t, tn_rec(set_row));
                uint32_t rec_r = tn_resolve(rec_t);
                uint32_t known_label = ks_lookup(known, known_n, sym_t);
                if (known_label != TN_NONE && tnodes[rec_r].kind == TN_REC) {
                    bool closed;
                    uint32_t field_t = tc_row_find(tnodes[rec_r].arg, known_label, &closed);
                    if (field_t != TN_NONE) {
                        tc_ut(val_t, field_t);
                        cur = tn_scons(rec_t, cur);
                    } else {
                        // field not in row — extend with new field
                        uint32_t tail = closed ? tn_rempty() : tnodes[rec_r].arg;
                        uint32_t new_row = tn_rext(known_label, val_t, tail);
                        cur = tn_scons(tn_rec(new_row), cur);
                    }
                } else {
                    cur = tn_scons(tn_rec(tn_rvar()), cur);
                }
                i++; continue;
            }
            // match: 'key {dispatch} (default) match -> result
            if (n->sym == sym_match) {
                uint32_t r = tn_svar();
                uint32_t default_t = tc_pop(&cur);
                uint32_t rec_t = tc_pop(&cur);
                uint32_t sym_t = tc_pop(&cur);
                tc_ut(sym_t, tn_sym());
                uint32_t match_row = tn_rvar();
                tc_ut(rec_t, tn_rec(match_row));
                tc_ut(default_t, tn_tuple(cur, r));
                uint32_t rec_r = tn_resolve(rec_t);
                if (tnodes[rec_r].kind == TN_REC) {
                    uint32_t row = tnodes[rec_r].arg;
                    while (true) {
                        row = tn_resolve(row);
                        if (tnodes[row].kind == TN_REXT) {
                            tc_ut(tnodes[row].rext.type, tn_tuple(cur, r));
                            row = tnodes[row].rext.tail;
                        } else break;
                    }
                }
                cur = r;
                i++; continue;
            }
            // cond: scrutinee [(clause ...)] (default) cond -> result
            if (n->sym == sym_cond) {
                uint32_t s = tn_svar(), r = tn_svar();
                uint32_t scrutinee_t = tn_var();
                uint32_t default_t = tc_pop(&cur);
                uint32_t clauses_t = tc_pop(&cur);
                tc_ust(cur, tn_scons(scrutinee_t, s));
                cur = s;
                // default: takes scrutinee, produces r
                tc_ut(default_t, tn_tuple(tn_scons(scrutinee_t, s), r));
                // clauses: Slice of clause tuples
                uint32_t pred_t = tn_var(), body_t = tn_var();
                uint32_t clause_s = tn_svar();
                uint32_t clause_elem = tn_tuple(clause_s, tn_scons(body_t, tn_scons(pred_t, clause_s)));
                tc_ut(clauses_t, tn_slice(clause_elem));
                // body must have same effect as default
                tc_ut(body_t, tn_tuple(tn_scons(scrutinee_t, s), r));
                // predicate: takes snapshot, returns Bool
                // derive snapshot type from scrutinee (like lend)
                uint32_t snap = tc_snapshot_of(tn_resolve(scrutinee_t));
                uint32_t snapshot_t = (snap != TN_NONE) ? snap : scrutinee_t;
                tc_ut(pred_t, tn_tuple(tn_scons(snapshot_t, s), tn_scons(tn_int(), s)));
                cur = r;
                i++; continue;
            }
            // on: model 'event (handler) on -> model
            // handler: (Int, model) -> model
            if (n->sym == sym_on) {
                uint32_t handler_t = tc_pop(&cur);
                uint32_t sym_t = tc_pop(&cur);
                uint32_t model_t = tc_pop(&cur);
                tc_ut(sym_t, tn_sym());
                uint32_t hs = tn_svar();
                tc_ut(handler_t, tn_tuple(
                    tn_scons(model_t, tn_scons(tn_int(), hs)),
                    tn_scons(model_t, hs)
                ));
                cur = tn_scons(model_t, cur);
                i++; continue;
            }
            // show: model (render) show -> bottom
            // render: (model-snapshot) -> ()
            if (n->sym == sym_show) {
                uint32_t render_t = tc_pop(&cur);
                uint32_t model_t = tc_pop(&cur);
                uint32_t snap_src = tn_resolve(model_t);
                uint32_t snap = tc_snapshot_of(snap_src);
                uint32_t snapshot_t = (snap != TN_NONE) ? snap : model_t;
                uint32_t rs = tn_svar();
                tc_ut(render_t, tn_tuple(tn_scons(snapshot_t, rs), rs));
                cur = tn_svar();
                i++; continue;
            }
            // Lookup in env
            TLookup lu = {0, false, false, false, false, false, NULL};
            { uint32_t lu_env = env;
              while (lu_env != TN_NONE) {
                  TCEnv *e = &tenvs[lu_env];
                  for (int j = 0; j < e->count; j++)
                      if (e->binds[j].name == n->sym) {
                          TCBinding *b = &e->binds[j];
                          bool was = b->used;
                          b->used = true;
                          lu = (TLookup){b->type, b->poly, b->is_def, b->freed, was, true, b};
                          goto lu_done;
                      }
                  lu_env = e->parent;
              }
              lu_done:; }
            if (lu.found) {
                uint32_t t = lu.type;
                if (lu.was_used && !lu.is_def && !lu.poly) EMIT_CC(t, sym_names[n->sym]);
                if (lu.poly) {
                    InstMap im = {.n = 0};
                    if (tnodes[tn_resolve(t)].kind == TN_TUPLE) {
                        uint32_t rt = tn_resolve(t);
                        StackEff e = {tc_inst_r(&im, tnodes[rt].tuple.in), tc_inst_r(&im, tnodes[rt].tuple.out)};
                        if (lu.is_def) { tc_ust(cur, e.in); cur = e.out; }
                        else { cur = tn_scons(tn_tuple(e.in, e.out), cur); }
                    } else {
                        cur = tn_scons(tc_inst_r(&im, t), cur);
                    }
                    // Fix 3: re-emit constraints with fresh type variables
                    if (lu.binding) {
                        for (int j = 0; j < lu.binding->poly_cc_n && tc_cc_count < MAX_TCOPY; j++)
                            tc_cc[tc_cc_count++] = (CopyConst){tc_inst_r(&im, lu.binding->poly_cc[j].var_node),
                                lu.binding->poly_cc[j].word, n->line, n->col};
                        for (int j = 0; j < lu.binding->poly_lc_n && tc_lc_count < MAX_TCOPY; j++)
                            tc_lc[tc_lc_count++] = (CopyConst){tc_inst_r(&im, lu.binding->poly_lc[j].var_node),
                                lu.binding->poly_lc[j].word, n->line, n->col};
                    }
                } else {
                    uint32_t rt = tn_resolve(t);
                    if (lu.is_def && tnodes[rt].kind == TN_TUPLE) {
                        tc_ust(cur, tnodes[rt].tuple.in);
                        cur = tnodes[rt].tuple.out;
                    } else if (lu.is_def && tnodes[rt].kind == TN_VAR) {
                        // def binding with unknown type — assume tuple, give generic effect
                        uint32_t gi = tn_svar(), go = tn_svar();
                        tc_ut(t, tn_tuple(gi, go));
                        tc_ust(cur, gi);
                        cur = go;
                    } else {
                        cur = tn_scons(t, cur);
                    }
                }
                i++; continue;
            }
            // Primitive
            { PrimScheme ps = {.copy_var = TN_NONE, .linear_var = TN_NONE};
              const char *nm = sym_names[n->sym];
              #define S tn_svar()
              #define T tn_var()
              #define INT tn_int()
              #define FLT tn_float()
              #define SYM_ tn_sym()
              #define SL(e) tn_slice(e)
              #define BX(e) tn_box(e)
              #define LS(e) tn_list(e)
              #define DI(k,v) tn_dice(k,v)
              #define DT(k,v) tn_dict_t(k,v)
              #define TU(i,o) tn_tuple(i,o)
              #define C(h,t) tn_scons(h,t)
              #define RC(r) tn_rec(r)
              #define STR_ tn_str()
              #define NM(s) (strcmp(nm, s) == 0)
              #define EFF(i,o) do { ps.eff = (StackEff){i, o}; } while(0)
              #define COPY(v) do { ps.copy_var = v; } while(0)
              #define LINEAR(v) do { ps.linear_var = v; } while(0)

              if (NM("dup"))  { uint32_t s=S,a=T; EFF(C(a,s), C(a,C(a,s))); COPY(a); }
              else if (NM("drop")) { uint32_t s=S,a=T; EFF(C(a,s), s); COPY(a); }
              else if (NM("swap")) { uint32_t s=S,a=T,b=T; EFF(C(b,C(a,s)), C(a,C(b,s))); }
              else if (NM("dip"))  { uint32_t s=S,r=S,a=T; EFF(C(TU(s,r),C(a,s)), C(a,r)); }
              else if (NM("apply")) { uint32_t s=S,r=S; EFF(C(TU(s,r),s), r); }
              else if (NM("if"))    { uint32_t s=S,r=S; EFF(C(TU(s,r),C(TU(s,r),C(INT,s))), r); }
              else if (NM("loop"))  { uint32_t s=S; EFF(C(TU(s,C(INT,s)),s), s); }
              else if (NM("while")) { uint32_t s=S; EFF(C(TU(s,s),C(TU(s,C(INT,s)),s)), s); }
              else if (NM("not"))   { uint32_t s=S; EFF(C(INT,s), C(INT,s)); }
              else if (NM("and") || NM("or")) { uint32_t s=S; EFF(C(INT,C(INT,s)), C(INT,s)); }
              else if (NM("eq")||NM("lt")) { uint32_t s=S,a=T; EFF(C(a,C(a,s)), C(INT,s)); COPY(a); }
              else if (NM("plus")||NM("sub")||NM("mul")||NM("div")) { uint32_t s=S,a=T; EFF(C(a,C(a,s)), C(a,s)); COPY(a); }
              else if (NM("mod")||NM("iplus")||NM("isub")||NM("imul")||NM("idiv")||NM("imod")) { uint32_t s=S; EFF(C(INT,C(INT,s)), C(INT,s)); }
              else if (NM("fplus")||NM("fsub")||NM("fmul")||NM("fdiv")) { uint32_t s=S; EFF(C(FLT,C(FLT,s)), C(FLT,s)); }
              else if (NM("fsqrt")||NM("fsin")||NM("fcos")||NM("ftan")||NM("ffloor")||NM("fceil")||NM("fround")||NM("fexp")||NM("flog")) { uint32_t s=S; EFF(C(FLT,s), C(FLT,s)); }
              else if (NM("fpow")||NM("fatan2")) { uint32_t s=S; EFF(C(FLT,C(FLT,s)), C(FLT,s)); }
              else if (NM("itof")) { uint32_t s=S; EFF(C(INT,s), C(FLT,s)); }
              else if (NM("ftoi")) { uint32_t s=S; EFF(C(FLT,s), C(INT,s)); }
              else if (NM("compose")) { uint32_t a=S,b=S,c=S,s=S; EFF(C(TU(b,c),C(TU(a,b),s)), C(TU(a,c),s)); }
              else if (NM("cons")) { uint32_t s=S,s2=S,r=S,a=T; EFF(C(a,C(TU(s2,r),s)), C(TU(s2,C(a,r)),s)); }
              else if (NM("car"))  { uint32_t s=S,s2=S,r=S,a=T; EFF(C(TU(s2,C(a,r)),s), C(a,C(TU(s2,r),s))); }
              else if (NM("rec"))  { uint32_t s=S; EFF(C(TU(S,S),s), C(RC(tn_rvar()),s)); }
              else if (NM("len"))  { uint32_t s=S; EFF(C(T,s), C(INT,s)); }
              else if (NM("fold")) { uint32_t s=S,a=T,b=T; EFF(C(TU(C(a,C(b,s)),C(b,s)),C(b,C(SL(a),s))), C(b,s)); }
              else if (NM("reduce")) { uint32_t s=S,a=T; EFF(C(TU(C(a,C(a,s)),C(a,s)),C(SL(a),s)), C(a,s)); }
              else if (NM("at"))   { uint32_t s=S,a=T; EFF(C(a,C(INT,C(SL(a),s))), C(a,s)); }
              else if (NM("put"))  { uint32_t s=S,a=T; EFF(C(a,C(INT,C(SL(a),s))), C(SL(a),s)); }
              else if (NM("each")) { uint32_t s=S,a=T; EFF(C(TU(C(a,s),s),C(SL(a),s)), s); }
              else if (NM("map"))  { uint32_t s=S,a=T,b=T; EFF(C(TU(C(a,s),C(b,s)),C(SL(a),s)), C(SL(b),s)); }
              else if (NM("filter")) { uint32_t s=S,a=T; EFF(C(TU(C(a,s),C(INT,s)),C(SL(a),s)), C(SL(a),s)); }
              else if (NM("range")) { uint32_t s=S; EFF(C(INT,C(INT,s)), C(SL(INT),s)); }
              else if (NM("sort"))    { uint32_t s=S,a=T; EFF(C(SL(a),s), C(SL(a),s)); }
              else if (NM("cat"))     { uint32_t s=S,a=T; EFF(C(SL(a),C(SL(a),s)), C(SL(a),s)); }
              else if (NM("take")||NM("drop-n")||NM("rotate")) { uint32_t s=S,a=T; EFF(C(INT,C(SL(a),s)), C(SL(a),s)); }
              else if (NM("scan")) { uint32_t s=S,a=T,b=T; EFF(C(TU(C(a,C(b,s)),C(b,s)),C(b,C(SL(a),s))), C(SL(b),s)); }
              else if (NM("select")||NM("keep-mask")) { uint32_t s=S,a=T; EFF(C(SL(INT),C(SL(a),s)), C(SL(a),s)); }
              else if (NM("windows")||NM("reshape")) { uint32_t s=S,a=T; EFF(C(INT,C(SL(a),s)), C(SL(SL(a)),s)); }
              else if (NM("rise")||NM("fall")||NM("shape")) { uint32_t s=S,a=T; EFF(C(SL(a),s), C(SL(INT),s)); }
              else if (NM("index-of")) { uint32_t s=S,a=T; EFF(C(SL(a),C(a,s)), C(INT,s)); COPY(a); }
              else if (NM("transpose")) { uint32_t s=S,a=T; EFF(C(SL(SL(a)),s), C(SL(SL(a)),s)); }
              else if (NM("classify")) { uint32_t s=S,a=T; EFF(C(SL(a),s), C(SL(INT),s)); COPY(a); }
              else if (NM("pick")) { uint32_t s=S,a=T,b=T; EFF(C(SL(INT),C(a,s)), C(b,s)); }
              else if (NM("group")||NM("partition")) { uint32_t s=S,a=T; EFF(C(SL(INT),C(SL(a),s)), C(SL(SL(a)),s)); }
              else if (NM("dice"))   { uint32_t s=S,a=T,b=T; EFF(C(SL(T),s), C(DI(a,b),s)); }
              else if (NM("grab"))   { uint32_t s=S,a=T,b=T; EFF(C(b,C(a,C(DI(a,b),s))), C(b,s)); }
              else if (NM("ifsert")) { uint32_t s=S,a=T,b=T; EFF(C(b,C(a,C(DI(a,b),s))), C(DI(a,b),s)); }
              else if (NM("clone")) { uint32_t s=S,l=T; EFF(C(l,s), C(l,C(l,s))); LINEAR(l); }
              else if (NM("free"))  { uint32_t s=S,a=T; EFF(C(a,s), s); LINEAR(a); }
              else if (NM("box"))   { uint32_t s=S,a=T; EFF(C(a,s), C(BX(a),s)); }
              else if (NM("list"))  { uint32_t s=S,a=T; EFF(C(SL(a),s), C(LS(a),s)); }
              else if (NM("list-zero"))  { uint32_t s=S; EFF(C(INT,s), C(LS(INT),s)); }
              else if (NM("list-concat"))  { uint32_t s=S,a=T; EFF(C(SL(a),C(LS(a),s)), C(LS(a),s)); }
              else if (NM("list-assign"))  { uint32_t s=S,a=T; EFF(C(a,C(INT,C(LS(a),s))), C(LS(a),s)); }
              else if (NM("list-at"))      { uint32_t s=S,a=T; EFF(C(a,C(INT,C(LS(a),s))), C(a,C(LS(a),s))); }
              else if (NM("dict"))  { uint32_t s=S,a=T,b=T; EFF(C(DI(a,b),s), C(DT(a,b),s)); }
              else if (NM("dict-insert")) { uint32_t s=S,a=T,b=T; EFF(C(b,C(a,C(DT(a,b),s))), C(DT(a,b),s)); }
              else if (NM("dict-remove")) { uint32_t s=S,a=T; EFF(C(a,C(DT(a,T),s)), C(DT(a,T),s)); }
              else if (NM("str"))   { uint32_t s=S; EFF(C(SL(INT),s), C(STR_,s)); }
              else if (NM("str-concat"))  { uint32_t s=S; EFF(C(SL(INT),C(STR_,s)), C(STR_,s)); }
              else if (NM("str-assign"))  { uint32_t s=S; EFF(C(INT,C(INT,C(STR_,s))), C(STR_,s)); }
              else if (NM("print")) { uint32_t s=S,a=T; EFF(C(a,s), s); }
              else if (NM("assert")) { uint32_t s=S; EFF(C(INT,s), s); }
              else if (NM("halt") || NM("print-stack")) { uint32_t s=S; EFF(s, s); }
              else if (NM("random")) { uint32_t s=S; EFF(C(INT,s), C(INT,s)); }
              else if (NM("clear")) { uint32_t s=S; EFF(C(INT,s), s); }
              else if (NM("pixel")) { uint32_t s=S; EFF(C(INT,C(INT,C(INT,s))), s); }
              else if (NM("millis")) { uint32_t s=S; EFF(s, C(INT,s)); }
              else { tc_err("unknown word: %s", nm); i++; continue; }

              #undef S
              #undef T
              #undef INT
              #undef FLT
              #undef SYM_
              #undef SL
              #undef BX
              #undef LS
              #undef DI
              #undef DT
              #undef TU
              #undef C
              #undef RC
              #undef STR_
              #undef NM
              #undef EFF
              #undef COPY
              #undef LINEAR

              InstMap im = {.n = 0};
              StackEff e = {tc_inst_r(&im, ps.eff.in), tc_inst_r(&im, ps.eff.out)};
              tc_ust(cur, e.in);
              cur = e.out;
              if (ps.copy_var != TN_NONE) EMIT_CC(tc_inst_r(&im, ps.copy_var), sym_names[n->sym]);
              if (ps.linear_var != TN_NONE) EMIT_LC(tc_inst_r(&im, ps.linear_var), sym_names[n->sym]);
            }
            i++;
        }
    }
    return (StackEff){base, cur};
}

typedef enum { LIN_YES, LIN_NO, LIN_UNKNOWN } Linearity;
static Linearity tc_linearity(uint32_t t) {
    t = tn_resolve(t);
    TNode *n = &tnodes[t];
    switch (n->kind) {
    case TN_BOX: case TN_LIST: case TN_DICT: case TN_STR: return LIN_YES;
    case TN_INT: case TN_FLOAT: case TN_SYM: case TN_TUPLE: case TN_REMPTY: return LIN_NO;
    case TN_SLICE: case TN_REC: return tc_linearity(n->arg);
    case TN_DICE: {
        Linearity k = tc_linearity(n->kv.key), v = tc_linearity(n->kv.val);
        return (k == LIN_YES || v == LIN_YES) ? LIN_YES : (k == LIN_NO && v == LIN_NO) ? LIN_NO : LIN_UNKNOWN;
    }
    case TN_REXT: {
        Linearity h = tc_linearity(n->rext.type), tl = tc_linearity(n->rext.tail);
        return (h == LIN_YES || tl == LIN_YES) ? LIN_YES : (h == LIN_NO && tl == LIN_NO) ? LIN_NO : LIN_UNKNOWN;
    }
    default: return LIN_UNKNOWN;
    }
}

static void tc_check_constraints(CopyConst *arr, int count, bool check_linear) {
    for (int i = 0; i < count && !tc_had_err; i++) {
        if (arr[i].handled) continue;
        Linearity lin = tc_linearity(tn_resolve(arr[i].var_node));
        bool bad = check_linear ? (lin == LIN_NO) : (lin == LIN_YES);
        if (bad) {
            char tb[128]; tn_show(tn_resolve(arr[i].var_node), tb, sizeof(tb));
            tc_cur_line = arr[i].line; tc_cur_col = arr[i].col;
            if (check_linear)
                tc_err("'%s' requires a linear type (Box, List, Dict, String), but got %s", arr[i].word, tb);
            else
                tc_err("'%s' requires a copyable type, but got %s (linear)\n\n"
                       "    Linear values (Box, List, Dict, String) cannot be duplicated.\n"
                       "    Use 'lend' to borrow or 'clone' for an explicit deep copy.", arr[i].word, tb);
        }
    }
}

static bool tc_run(int start, int len) {
    tn_count = 0; tc_next_var = 0; tenv_count = 0;
    tc_cc_count = 0; tc_lc_count = 0; tc_err_count = 0; tc_had_err = false;
    tc_pending_n = 0;

    uint32_t env = tenv_new(TN_NONE);

    // Type-check prelude by inferring it (bindings get added to env)
    // (prelude nodes are before start)
    if (start > 0) tc_infer(0, start, env, 0);

    // Type-check user code
    if (!tc_had_err) tc_infer(start, len, env, 0);
    if (!tc_had_err) tc_check_constraints(tc_cc, tc_cc_count, false);
    if (!tc_had_err) tc_check_constraints(tc_lc, tc_lc_count, true);

    if (tc_err_count > 0) {
        fprintf(stderr, "\n");
        for (int i = 0; i < tc_err_count; i++) {
            TCError *e = &tc_errs[i];
            fprintf(stderr, "%s── TYPE ERROR ─────────────────────────────────────%s\n\n",
                C_RED, C_RESET);
            const char *sl; int sl_len;
            get_src_line(e->line, &sl, &sl_len);
            if (sl && sl_len > 0 && e->line > 0) {
                int gw = snprintf(NULL, 0, "%d", e->line);
                fprintf(stderr, "  %s%d%s %s│%s %.*s\n",
                    C_CYAN, e->line, C_RESET, C_DIM, C_RESET, sl_len, sl);
                if (e->col > 0 && e->span > 0) {
                    fprintf(stderr, "  %*s %s│%s ", gw, "", C_DIM, C_RESET);
                    for (int c = 1; c < e->col; c++) fputc(' ', stderr);
                    fprintf(stderr, "%s", C_RED);
                    for (int c = 0; c < e->span; c++) fputc('^', stderr);
                    fprintf(stderr, "%s\n", C_RESET);
                }
                fprintf(stderr, "\n  %s%s%s\n\n", C_BOLD, e->msg, C_RESET);
            } else if (e->line > 0) {
                fprintf(stderr, "  %sline %d%s: %s%s%s\n\n",
                    C_CYAN, e->line, C_RESET, C_BOLD, e->msg, C_RESET);
            } else {
                fprintf(stderr, "  %s%s%s\n\n", C_BOLD, e->msg, C_RESET);
            }
        }
        return false;
    }
    return true;
}

