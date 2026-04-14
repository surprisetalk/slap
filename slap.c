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
#ifndef SLAP_WASM
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif
#define STACK_MAX  2097152
#define SYM_MAX   4096
#define FRAME_MAX 1024
#define FRAME_HASH_SIZE 2048
#define FRAME_VALS_MAX 4194304
#define TOK_MAX   65536
#define LOCAL_MAX 16384
typedef enum { VAL_INT, VAL_FLOAT, VAL_SYM, VAL_WORD, VAL_XT, VAL_TUPLE, VAL_LIST, VAL_RECORD, VAL_BOX, VAL_TAGGED, VAL_DICT } ValTag;
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
    const char *names[] = {"int","float","symbol","word","xt","tuple","list","record","box","tagged","dict"};
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
    if (sym_count >= SYM_MAX) { fprintf(stderr, "error: symbol table full\n"); exit(1); }
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
static int current_fid  = FID_STDIN;
static int current_line = 0;
static int current_col  = 0;
static void print_stack_summary(FILE *out);
static void store_source_lines(const char *src, int fid) {
    if (src_text[fid]) { free(src_text[fid]); free(src_lines[fid]); }
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
    static int dying = 0; const char *f = src_files[current_fid];
    va_list ap; va_start(ap, fmt);
    if (current_col > 0) fprintf(stderr, "\n-- ERROR %s:%d:%d ", f, current_line, current_col);
    else fprintf(stderr, "\n-- ERROR %s:%d ", f, current_line);
    int hl = 10+(int)strlen(f)+10; for(int i=hl;i<60;i++) fputc('-',stderr);
    fprintf(stderr, "\n\n    "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n\n");
    print_source_line(stderr, current_fid, current_line, current_col); va_end(ap);
    if (!dying) { dying = 1; print_stack_summary(stderr); fprintf(stderr, "\n"); }
    exit(1);
}
static Value stack[STACK_MAX];
static int sp = 0;
static char **cli_args=NULL; static int cli_argc=0;
static int headless_mode=0;
static void spush(Value v) { if (sp >= STACK_MAX) die("stack overflow"); stack[sp++] = v; }
static Value spop(void) { if (sp <= 0) die("stack underflow"); return stack[--sp]; }
static Value speek(void) { if (sp <= 0) die("stack underflow on peek"); return stack[sp - 1]; }
#define VCPY(d,s,n) memcpy(d,s,(n)*sizeof(Value))
#define SPUSH(src,n) do{VCPY(&stack[sp],src,n);sp+=(n);}while(0)
#define MKVAL(t) Value v={0};v.tag=t
static Value val_int(int64_t i){MKVAL(VAL_INT);v.as.i=i;return v;}
static Value val_float(double f){MKVAL(VAL_FLOAT);v.as.f=f;return v;}
static Value val_sym(uint32_t s){MKVAL(VAL_SYM);v.as.sym=s;return v;}
static Value val_word(uint32_t s){MKVAL(VAL_WORD);v.as.sym=s;return v;}
static Value val_xt(uint32_t s,PrimFn fn){MKVAL(VAL_XT);v.as.xt.sym=s;v.as.xt.fn=fn;return v;}
static Value val_compound(ValTag tag,uint32_t len,uint32_t slots){MKVAL(tag);v.as.compound.len=len;v.as.compound.slots=slots;v.as.compound.env=NULL;return v;}
typedef enum {
    TOK_INT, TOK_FLOAT, TOK_SYM, TOK_WORD, TOK_STRING,
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACKET, TOK_RBRACKET, TOK_LBRACE, TOK_RBRACE, TOK_EOF
} TokTag;
typedef struct {
    TokTag tag;
    union { int64_t i; double f; uint32_t sym; struct { int *codes; int len; } str; } as;
    int line;
    int col;
    int fid;
} Token;
static Token tokens[TOK_MAX];
static int tok_count = 0;
#define LEX_ADVANCE() do { if (*p == '\n') { line++; col = 1; } else { col++; } p++; } while (0)
static void lex(const char *src, int fid) {
    tok_count = 0; int line = 1; int col = 1; const char *p = src;
    while (*p) {
        if (*p == '\n') { line++; col = 1; p++; continue; }
        if (isspace((unsigned char)*p)) { col++; p++; continue; }
        if (p[0] == '-' && p[1] == '-') { while (*p && *p != '\n') { col++; p++; } continue; }
        if (tok_count >= TOK_MAX) die("too many tokens");
        Token *t = &tokens[tok_count];
        t->line = line; t->col = col; t->fid = fid;
        { static const char brackets[]="()[]{}"; static const TokTag btags[]={TOK_LPAREN,TOK_RPAREN,TOK_LBRACKET,TOK_RBRACKET,TOK_LBRACE,TOK_RBRACE};
          const char *bp=strchr(brackets,*p); if(bp){t->tag=btags[bp-brackets];LEX_ADVANCE();tok_count++;continue;} }
        if (*p == '"') {
            LEX_ADVANCE();
            int *codes = NULL; int len = 0, cap = 0;
            while (*p && *p != '"') {
                int ch;
                if (*p == '\\') { LEX_ADVANCE(); switch (*p) { case 'n': ch='\n'; break; case 't': ch='\t'; break; case '\\': ch='\\'; break; case '"': ch='"'; break; case '0': ch=0; break; default: ch=*p; break; } LEX_ADVANCE(); }
                else {
                    unsigned char b0 = (unsigned char)*p;
                    if (b0 < 0x80) { ch = b0; LEX_ADVANCE(); }
                    else {
                        int need; unsigned mask;
                        if      ((b0 & 0xE0) == 0xC0) { need = 1; mask = 0x1F; }
                        else if ((b0 & 0xF0) == 0xE0) { need = 2; mask = 0x0F; }
                        else if ((b0 & 0xF8) == 0xF0) { need = 3; mask = 0x07; }
                        else die("string literal: invalid UTF-8 lead byte 0x%02X at line %d", b0, t->line);
                        ch = b0 & mask; LEX_ADVANCE();
                        for (int k = 0; k < need; k++) {
                            unsigned char bn = (unsigned char)*p;
                            if ((bn & 0xC0) != 0x80) die("string literal: invalid UTF-8 continuation byte 0x%02X at line %d", bn, t->line);
                            ch = (ch << 6) | (bn & 0x3F); LEX_ADVANCE();
                        }
                    }
                }
                if (len >= cap) { cap = cap ? cap*2 : 16; codes = realloc(codes, cap * sizeof(int)); }
                codes[len++] = ch;
            }
            if (*p == '"') LEX_ADVANCE();
            t->tag = TOK_STRING; t->as.str.codes = codes; t->as.str.len = len; tok_count++; continue;
        }
        if (*p == '\'') {
            LEX_ADVANCE();
            const char *start = p;
            while (*p && !isspace((unsigned char)*p) && *p!='(' && *p!=')' && *p!='[' && *p!=']' && *p!='{' && *p!='}') LEX_ADVANCE();
            int len = (int)(p - start);
            if (len == 0) die("empty symbol literal");
            char buf[256]; if (len >= (int)sizeof(buf)) die("symbol too long");
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
            } else { t->tag = TOK_INT; t->as.i = strtoll(start, NULL, 10); }
            tok_count++; continue;
        }
        { const char *start = p;
          while (*p && !isspace((unsigned char)*p) && *p!='(' && *p!=')' && *p!='[' && *p!=']' && *p!='{' && *p!='}') LEX_ADVANCE();
          int len = (int)(p - start); char buf[256];
          if (len >= (int)sizeof(buf)) die("word too long");
          memcpy(buf, start, len); buf[len] = 0;
          if (strcmp(buf, "true") == 0) { t->tag = TOK_INT; t->as.i = 1; tok_count++; continue; }
          if (strcmp(buf, "false") == 0) { t->tag = TOK_INT; t->as.i = 0; tok_count++; continue; }
          t->tag = TOK_WORD; t->as.sym = sym_intern(buf); tok_count++;
        }
    }
    if (tok_count < TOK_MAX) { Token *eof=&tokens[tok_count]; eof->tag=TOK_EOF; eof->line=line; eof->col=col; eof->fid=fid; }
}
static inline Value with_tok(Value v, const Token *t) {
    v.loc = LOC_PACK(t->fid, t->line, t->col);
    return v;
}
typedef enum { BIND_DEF, BIND_LET } BindKind;
typedef struct Binding { uint32_t sym; int offset; int slots; int allocated; BindKind kind; int recur; } Binding;
struct Frame {
    struct Frame *parent; int bind_count; int vals_used; int refcount;
    Binding bindings[FRAME_MAX]; Value vals[FRAME_VALS_MAX];
    int16_t hash[FRAME_HASH_SIZE]; // maps sym hash -> binding index+1 (0=empty)
};
#define SAVE_BUF_MAX 4194304
static Value save_buf[SAVE_BUF_MAX];
static int save_buf_sp=0;
static int frame_save_active=0;
static Frame *frame_save_target=NULL;
static int frame_save_sbc=0;
static Frame *frame_new(Frame *parent) {
    Frame *f = malloc(sizeof(Frame));
    f->parent = parent; f->bind_count = 0; f->vals_used = 0; f->refcount = 0;
    memset(f->hash, 0, sizeof(f->hash));
    return f;
}
static void frame_free(Frame *f) { free(f); }
static void frame_bind(Frame *f, uint32_t sym, Value *vals, int slots, BindKind kind, int recur) {
    uint32_t h = sym % FRAME_HASH_SIZE; Binding *b = NULL;
    for (int i = 0; i < FRAME_HASH_SIZE; i++) { uint32_t s = (h + i) % FRAME_HASH_SIZE;
        if (f->hash[s] == 0) break; int idx = f->hash[s] - 1;
        if (idx < f->bind_count && f->bindings[idx].sym == sym) { b = &f->bindings[idx]; break; } }
    if (!b) {
        if (f->bind_count >= FRAME_MAX) die("too many bindings in frame");
        int idx = f->bind_count++; b = &f->bindings[idx]; b->sym = sym; b->allocated = 0;
        for (int i = 0; i < FRAME_HASH_SIZE; i++) { uint32_t s = (h + i) % FRAME_HASH_SIZE;
            if (f->hash[s] == 0 || f->bindings[f->hash[s]-1].sym == sym) { f->hash[s] = (int16_t)(idx + 1); break; } }
    }
    if(frame_save_active&&f==frame_save_target&&(b-f->bindings)<frame_save_sbc){
        if(save_buf_sp+5+b->slots>SAVE_BUF_MAX) die("frame save buffer overflow (%d slots)",save_buf_sp+5+b->slots);
        save_buf[save_buf_sp++]=val_int((int)(b-f->bindings));
        save_buf[save_buf_sp++]=val_int(b->offset);
        save_buf[save_buf_sp++]=val_int(b->allocated);
        save_buf[save_buf_sp++]=val_int(b->slots);
        save_buf[save_buf_sp++]=val_int(((int)b->kind<<1)|b->recur);
        VCPY(&save_buf[save_buf_sp],&f->vals[b->offset],b->slots);save_buf_sp+=b->slots;}
    if (slots <= b->allocated) VCPY(&f->vals[b->offset],vals,slots);
    else { int off = f->vals_used; if (off + slots > FRAME_VALS_MAX) die("frame value storage full");
        VCPY(&f->vals[off],vals,slots ); f->vals_used += slots; b->offset = off; b->allocated = slots; }
    b->slots = slots; b->kind = kind; b->recur = recur;
}
typedef struct { Binding *bind; Frame *frame; } Lookup;
static Lookup frame_lookup(Frame *f, uint32_t sym) {
    for (Frame *cur = f; cur; cur = cur->parent) {
        uint32_t h = sym % FRAME_HASH_SIZE;
        for (int i = 0; i < FRAME_HASH_SIZE; i++) {
            uint32_t slot = (h + i) % FRAME_HASH_SIZE;
            if (cur->hash[slot] == 0) break;
            int idx = cur->hash[slot] - 1;
            if (idx < cur->bind_count && cur->bindings[idx].sym == sym) { Lookup r = {&cur->bindings[idx], cur}; return r; }
        }
    }
    Lookup r = {NULL, NULL}; return r;
}
static void eval(Token *toks, int count, Frame *env);
static void eval_body(Value *body, int slots, Frame *env);
static void eval_tuple_scoped(Value *body, int slots, Frame *env);
static void dispatch_word(uint32_t sym, Frame *env);
static void prim_strfind_must(Frame *e);
static inline void eval_body_fast(Value *body, int slots, Frame *env);
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
static int64_t pop_int(void) { Value v=spop(); if(v.tag==VAL_INT) return v.as.i; die("expected int, got %s",valtag_name(v.tag)); }
static double pop_float(void) { Value v=spop(); if(v.tag==VAL_FLOAT) return v.as.f; die("expected float, got %s",valtag_name(v.tag)); }
static uint32_t pop_sym(void) { Value v=spop(); if(v.tag==VAL_SYM) return v.as.sym; die("expected symbol, got %s",valtag_name(v.tag)); }
typedef struct { int base; int slots; } ElemRef;
static ElemRef compound_elem(Value *data, int total_slots, int len, int index) {
    if (index < 0 || index >= len) { ElemRef ref = { -1, 0 }; return ref; }
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
        if (key_pos < 0) die("malformed record (len=%d, total_slots=%d)", len, total_slots);
        if (data[key_pos].tag != VAL_SYM) die("record key must be symbol, got %s", valtag_name(data[key_pos].tag));
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
static void record_offsets(Value *data, int ts, int len, int *kp, int *vo, int *vs) {
    int e=ts-1; for(int i=len-1;i>=0;i--){int s=val_slots(data[e-1]);vo[i]=e-s;vs[i]=s;kp[i]=vo[i]-1;e=kp[i];}
}
static void val_print(Value *data, int slots, FILE *out) {
    Value top = data[slots - 1];
    switch (top.tag) {
    case VAL_INT: fprintf(out, "%lld", (long long)top.as.i); break;
    case VAL_FLOAT: fprintf(out, "%g", top.as.f); break;
    case VAL_SYM: fprintf(out, "'%s", sym_name(top.as.sym)); break;
    case VAL_WORD: case VAL_XT: fprintf(out, "%s", sym_name(top.tag==VAL_XT?top.as.xt.sym:top.as.sym)); break;
    case VAL_LIST: {
        int len=(int)top.as.compound.len, is_str=len>0;
        for(int i=0;is_str&&i<len;i++){Value v=data[i];if(v.tag!=VAL_INT||v.as.i<32||v.as.i>126)is_str=0;}
        if(is_str&&slots==len+1){fprintf(out,"\"");for(int i=0;i<len;i++)fputc((char)data[i].as.i,out);fprintf(out,"\"");break;}
    } /* fall through */
    case VAL_TUPLE: {
        int len=(int)top.as.compound.len; char o=top.tag==VAL_LIST?'[':'(',c=top.tag==VAL_LIST?']':')';
        fprintf(out,"%c",o); if(len>LOCAL_MAX){fprintf(out,"...%d elements%c",len,c);break;}
        int of[LOCAL_MAX],sz[LOCAL_MAX]; compute_offsets(data,slots,len,of,sz);
        for(int i=0;i<len;i++){if(i>0)fprintf(out," ");val_print(&data[of[i]],sz[i],out);}
        fprintf(out,"%c",c); break;
    }
    case VAL_RECORD: {
        int len=(int)top.as.compound.len; fprintf(out,"{"); if(len>LOCAL_MAX){fprintf(out,"...%d fields}",len);break;}
        int kp[LOCAL_MAX],vo[LOCAL_MAX],vs[LOCAL_MAX]; record_offsets(data,slots,len,kp,vo,vs);
        for(int i=0;i<len;i++){if(i>0)fprintf(out," ");fprintf(out,"'%s ",sym_name(data[kp[i]].as.sym));val_print(&data[vo[i]],vs[i],out);}
        fprintf(out,"}"); break;
    }
    case VAL_BOX: fprintf(out, "<box>"); break;
    case VAL_DICT: { DictData *dd=(DictData*)top.as.box; fprintf(out,"<dict:%d>",dd?dd->len:0); break; }
    case VAL_TAGGED: val_print(data,(int)top.as.compound.slots-1,out); fprintf(out," '%s tagged",sym_name(top.as.compound.len)); break;
    }
}
static void print_stack_summary(FILE *out) {
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
    case VAL_SYM: case VAL_WORD: case VAL_XT: return atop.as.sym == btop.as.sym;
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
    case VAL_SYM: return atop.as.sym < btop.as.sym;
    default: die("lt: unsupported type %s (only int/float/symbol are ordered)", valtag_name(atop.tag)); return 0;
    }
}
static uint32_t recur_sym = 0;
static int recur_pending = 0;
static int eval_depth = 0;
#define EVAL_DEPTH_MAX 10000
static uint32_t S_DEF, S_LET, S_RECUR, S_IF, S_EFFECT, S_CHECK, S_UNION, S_OK, S_NO, S_UNTAG, S_DEFAULT, S_MUST;
static uint32_t S_GET, S_POP, S_AT, S_NTH, S_SET, S_EDIT, S_INDEXOF, S_STRFIND;
static uint32_t S_PLUS, S_SUB, S_EQ, S_SWAP, S_DROP, S_MUL, S_DIV, S_MOD;
static void syms_init(void);
/* ---- TYPE SYSTEM ---- */
typedef enum { DIR_IN, DIR_OUT } SlotDir;
typedef enum { OWN_OWN, OWN_COPY, OWN_MOVE, OWN_LENT, OWN_AUTO } OwnMode;
typedef enum { TC_NONE=0, TC_INT, TC_FLOAT, TC_SYM, TC_NUM, TC_LIST, TC_TUPLE, TC_REC, TC_BOX, TC_STACK, TC_TAGGED, TC_SEQ, TC_EQ, TC_ORD, TC_INTEGRAL, TC_SEMIGROUP, TC_MONOID, TC_FUNCTOR, TC_APPLICATIVE, TC_FOLDABLE, TC_MONAD, TC_DICT, TC_LINEAR, TC_SIZED } TypeConstraint;
enum { HO_BODY_1TO1=1, HO_BRANCHES_AGREE=2, HO_SAVES_UNDER=4,
       HO_APPLY_EFFECT=16, HO_BOX_BORROW=32, HO_BOX_MUTATE=64, HO_SCRUTINEE_TAGGED=128 };
typedef struct { const char *name; uint32_t sym; int need; int out; TypeConstraint out_type; uint8_t flags; } HOEffect;
#define HO_OP_COUNT 19
static HOEffect ho_ops[HO_OP_COUNT] = {
    {"apply",0,1,0,TC_NONE,HO_APPLY_EFFECT},{"dip",0,2,1,TC_NONE,HO_APPLY_EFFECT|HO_SAVES_UNDER},
    {"if",0,3,1,TC_NONE,HO_BRANCHES_AGREE},
    {"fold",0,3,1,TC_NONE,0},{"reduce",0,2,1,TC_NONE,0},{"each",0,2,1,TC_FUNCTOR,HO_BODY_1TO1},
    {"while",0,2,0,TC_NONE,0},{"loop",0,1,0,TC_NONE,HO_APPLY_EFFECT},
    {"lend",0,2,2,TC_BOX,HO_BOX_BORROW},{"mutate",0,2,1,TC_BOX,HO_BOX_MUTATE},
    {"cond",0,3,1,TC_NONE,HO_BRANCHES_AGREE},{"case",0,3,1,TC_NONE,HO_BRANCHES_AGREE},
    {"find",0,3,1,TC_NONE,0},
    {"scan",0,3,1,TC_LIST,0},
    {"on",0,1,0,TC_NONE,0},{"show",0,1,0,TC_NONE,0},
    {"untag",0,3,1,TC_NONE,HO_BRANCHES_AGREE|HO_SCRUTINEE_TAGGED},
    {"then",0,2,1,TC_MONAD,HO_BODY_1TO1},
    {"edit",0,3,1,TC_TAGGED,HO_BODY_1TO1},
};
static HOEffect *ho_ops_find(uint32_t sym) { for (int i = 0; i < HO_OP_COUNT; i++) if (ho_ops[i].sym == sym) return &ho_ops[i]; return NULL; }
static void syms_init(void) {
    S_DEF=sym_intern("def"); S_LET=sym_intern("let"); S_RECUR=sym_intern("recur");
    S_IF=sym_intern("if"); S_EFFECT=sym_intern("effect"); S_CHECK=sym_intern("check");
    S_UNION=sym_intern("union"); S_OK=sym_intern("ok"); S_NO=sym_intern("no");
    S_UNTAG=sym_intern("untag"); S_DEFAULT=sym_intern("default"); S_MUST=sym_intern("must");
    S_GET=sym_intern("get"); S_POP=sym_intern("pop"); S_AT=sym_intern("at"); S_NTH=sym_intern("nth");
    S_SET=sym_intern("set"); S_EDIT=sym_intern("edit"); S_INDEXOF=sym_intern("index-of"); S_STRFIND=sym_intern("str-find");
    S_PLUS=sym_intern("plus"); S_SUB=sym_intern("sub"); S_EQ=sym_intern("eq"); S_SWAP=sym_intern("swap"); S_DROP=sym_intern("drop");
    S_MUL=sym_intern("mul"); S_DIV=sym_intern("div"); S_MOD=sym_intern("mod");
    for (int i = 0; i < HO_OP_COUNT; i++) ho_ops[i].sym = sym_intern(ho_ops[i].name);
}
typedef struct {
    uint32_t type_var; TypeConstraint constraint, elem_constraint; OwnMode ownership; SlotDir direction;
    uint32_t either_syms[4]; TypeConstraint either_types[4]; uint32_t either_tvars[4]; int either_count;
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
static const struct { const char *name; TypeConstraint tc; } tc_names[] = {
    {"int",TC_INT},{"float",TC_FLOAT},{"sym",TC_SYM},{"num",TC_NUM},
    {"list",TC_LIST},{"tuple",TC_TUPLE},{"rec",TC_REC},{"box",TC_BOX},{"stack",TC_STACK},{"tagged",TC_TAGGED},
    {"seq",TC_SEQ},{"eq",TC_EQ},{"eql",TC_EQ},{"ord",TC_ORD},
    {"integral",TC_INTEGRAL},{"semigroup",TC_SEMIGROUP},{"monoid",TC_MONOID},
    {"functor",TC_FUNCTOR},{"applicative",TC_APPLICATIVE},{"foldable",TC_FOLDABLE},{"monad",TC_MONAD},{"dict",TC_DICT},{"linear",TC_LINEAR},{"sized",TC_SIZED},{NULL,TC_NONE}
};
static TypeConstraint parse_constraint(const char *tw) {
    for (int i=0; tc_names[i].name; i++) if (strcmp(tw,tc_names[i].name)==0) return tc_names[i].tc;
    return TC_NONE;
}
static int tc_is_container(TypeConstraint c) { return c == TC_LIST || c == TC_BOX || c == TC_TAGGED || c == TC_SEQ || c == TC_FUNCTOR || c == TC_MONAD || c == TC_APPLICATIVE || c == TC_DICT; }
static int tc_is_concrete(TypeConstraint c) {
    return c == TC_INT || c == TC_FLOAT || c == TC_SYM || c == TC_LIST || c == TC_TUPLE || c == TC_REC || c == TC_BOX || c == TC_TAGGED || c == TC_DICT;
}
static TypeSig parse_type_annotation(Token *toks, int start, int end) {
    TypeSig sig; memset(&sig, 0, sizeof(sig));
    int i = start;
    while (i < end) {
        if (sig.slot_count >= TYPE_SLOTS_MAX) die("too many type slots");
        TypeSlot *slot = &sig.slots[sig.slot_count]; memset(slot, 0, sizeof(*slot));
        int slot_start = i;
        /* handle {... } either pattern: {'ok type 'no type} either move out */
        if (toks[i].tag == TOK_LBRACE) {
            int brace_start = i;
            int d = 1; i++;
            while (i < end && d > 0) { if (toks[i].tag == TOK_LBRACE) d++; else if (toks[i].tag == TOK_RBRACE) d--; i++; }
            /* parse variant pairs from brace: 'sym type 'sym type ... */
            int ec = 0;
            for (int b = brace_start + 1; b < i - 1 && ec < 4; ) {
                if (toks[b].tag == TOK_SYM) {
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
                        slot->either_types[ec] = parse_constraint(tn); b++;
                    } else { slot->either_types[ec] = TC_NONE; }
                    ec++;
                } else b++;
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
        if (i >= end) break;
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
#define TVAR_MAX 32768
typedef struct { int parent; TypeConstraint bound; int elem; int box_c; int tag_p; int union_id; } TVarEntry;
#define UNION_MAX 2048
#define UNION_VARIANTS_MAX 16
typedef struct { uint32_t syms[UNION_VARIANTS_MAX]; TypeConstraint types[UNION_VARIANTS_MAX]; int count; } UnionDef;
#define EFFECT_MAX 256
typedef struct {
    int consumed, produced;
    TypeConstraint out_type;
    int scheme_base, scheme_count;
    int in_tvars[8], out_tvars[8], in_count, out_count;
    int out_effect;
    int has_let;
    int captures_linear;  /* body looked up a linear binding from outer scope; tuple is single-use */
} TupleEffect;
#define AT_LINEAR 1
#define AT_CONSUMED 2
typedef struct {
    TypeConstraint type; int tvar_id; uint32_t sym_id;
    uint8_t flags; int8_t borrowed; int source_line; int effect_idx;
} AbstractType;
#define ASTACK_MAX 256
#define TC_BINDS_MAX 2048
typedef struct { uint32_t sym; AbstractType atype; int is_def; int def_line; int body_depth; int consumed_line; } TCBinding;
typedef struct { uint32_t sym; int line; } TCUnknown;
#define TC_UNKNOWN_MAX 256
typedef struct {
    AbstractType data[ASTACK_MAX]; int sp, errors;
    TCBinding bindings[TC_BINDS_MAX]; int bind_count;
    int recur_pending; uint32_t recur_sym;
    int in_recur_body;  /* non-zero inside the immediate body of a recur'd function; gates branch-effect checks */
    TCUnknown unknowns[TC_UNKNOWN_MAX]; int unknown_count;
    TVarEntry tvars[TVAR_MAX]; int tvar_count;
    TupleEffect effects[EFFECT_MAX]; int effect_count;
    int user_start, prelude_sig_count, suppress_errors, sp_floor, body_depth, lend_depth;
    int saw_linear_capture;  /* set by binding-lookup of a linear value; consumed by enclosing tuple-body inference */
    UnionDef unions[UNION_MAX]; int union_count;
} TypeChecker;
static int tvar_fresh(TypeChecker *tc) {
    if (tc->tvar_count >= TVAR_MAX) die("type variable overflow");
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
    [TC_INT]=B(TC_INT)|B(TC_NUM)|B(TC_INTEGRAL)|B(TC_ORD)|B(TC_EQ),
    [TC_FLOAT]=B(TC_FLOAT)|B(TC_NUM)|B(TC_ORD)|B(TC_EQ),
    [TC_SYM]=B(TC_SYM)|B(TC_EQ),
    [TC_NUM]=B(TC_NUM)|B(TC_INT)|B(TC_FLOAT)|B(TC_EQ)|B(TC_ORD),
    [TC_LIST]=B(TC_LIST)|B(TC_SEQ)|B(TC_SEMIGROUP)|B(TC_MONOID)|B(TC_FUNCTOR)|B(TC_APPLICATIVE)|B(TC_FOLDABLE)|B(TC_MONAD)|B(TC_EQ)|B(TC_SIZED),
    [TC_TUPLE]=B(TC_TUPLE)|B(TC_SEMIGROUP)|B(TC_MONOID)|B(TC_EQ)|B(TC_SIZED),
    [TC_REC]=B(TC_REC)|B(TC_SEMIGROUP)|B(TC_MONOID)|B(TC_EQ)|B(TC_SIZED),
    [TC_BOX]=B(TC_BOX)|B(TC_LINEAR), [TC_STACK]=B(TC_STACK),
    [TC_TAGGED]=B(TC_TAGGED)|B(TC_FUNCTOR)|B(TC_APPLICATIVE)|B(TC_MONAD)|B(TC_EQ),
    [TC_SEQ]=B(TC_SEQ)|B(TC_LIST)|B(TC_SEMIGROUP)|B(TC_MONOID)|B(TC_FUNCTOR)|B(TC_APPLICATIVE)|B(TC_FOLDABLE)|B(TC_MONAD)|B(TC_EQ)|B(TC_SIZED),
    /* EQ is the most general stackable constraint. A value typed only as EQ
       cannot be narrowed to something stricter (ORD, NUM, SEQ, ...) without
       concrete evidence — that would silently tighten a function's declared
       signature beyond what was written. Keep this row minimal: EQ and the
       concrete stackable types that inherently satisfy Eq. */
    [TC_EQ]=B(TC_EQ)|B(TC_INT)|B(TC_FLOAT)|B(TC_SYM)|B(TC_LIST)|B(TC_TUPLE)|B(TC_REC)|B(TC_TAGGED),
    [TC_ORD]=B(TC_ORD)|B(TC_INT)|B(TC_FLOAT)|B(TC_EQ)|B(TC_NUM),
    [TC_INTEGRAL]=B(TC_INTEGRAL)|B(TC_INT)|B(TC_NUM)|B(TC_EQ),
    [TC_SEMIGROUP]=B(TC_SEMIGROUP)|B(TC_MONOID)|B(TC_LIST)|B(TC_TUPLE)|B(TC_REC)|B(TC_SEQ),
    [TC_MONOID]=B(TC_MONOID)|B(TC_LIST)|B(TC_TUPLE)|B(TC_REC)|B(TC_SEQ)|B(TC_SEMIGROUP),
    [TC_FUNCTOR]=B(TC_FUNCTOR)|B(TC_APPLICATIVE)|B(TC_MONAD)|B(TC_FOLDABLE)|B(TC_LIST)|B(TC_TAGGED)|B(TC_SEQ)|B(TC_DICT),
    [TC_APPLICATIVE]=B(TC_APPLICATIVE)|B(TC_LIST)|B(TC_TAGGED)|B(TC_SEQ)|B(TC_FUNCTOR)|B(TC_MONAD),
    [TC_FOLDABLE]=B(TC_FOLDABLE)|B(TC_LIST)|B(TC_SEQ)|B(TC_DICT),
    [TC_MONAD]=B(TC_MONAD)|B(TC_LIST)|B(TC_TAGGED)|B(TC_SEQ)|B(TC_FUNCTOR)|B(TC_APPLICATIVE),
    /* DICT still cross-matches LINEAR at the constraint level so ops declared
       with `linear` (free, clone) accept a dict. Runtime linearity is a
       separate flag on the AbstractType (AT_LINEAR), which is off for dicts
       post-A2. */
    [TC_DICT]=B(TC_DICT)|B(TC_FUNCTOR)|B(TC_FOLDABLE)|B(TC_LINEAR)|B(TC_SIZED),
    [TC_LINEAR]=B(TC_LINEAR)|B(TC_BOX)|B(TC_DICT),
    [TC_SIZED]=B(TC_SIZED)|B(TC_LIST)|B(TC_TUPLE)|B(TC_REC)|B(TC_DICT)|B(TC_SEQ)|B(TC_SEMIGROUP)|B(TC_MONOID),
};
#undef B
static int tc_constraint_matches(TypeConstraint a, TypeConstraint b) {
    if (a == TC_NONE || a == b) return 1;
    return (a <= TC_SIZED && b <= TC_SIZED) ? (tc_compat[a] >> b) & 1 : 0;
}
static int tc_should_narrow(TypeConstraint cur, TypeConstraint c) {
    if (tc_is_concrete(c) && !tc_is_concrete(cur)) return 1;
    if (cur == TC_NUM && (c == TC_INT || c == TC_FLOAT || c == TC_INTEGRAL)) return 1;
    if (cur == TC_EQ && (c == TC_ORD || c == TC_NUM || c == TC_SEQ || c == TC_INTEGRAL || c == TC_SEMIGROUP || c == TC_MONOID || c == TC_FUNCTOR || c == TC_APPLICATIVE || c == TC_FOLDABLE || c == TC_MONAD)) return 1;
    if (cur == TC_SEMIGROUP && (c == TC_SEQ || c == TC_MONOID)) return 1;
    if (cur == TC_MONOID && c == TC_SEQ) return 1;
    if (cur == TC_FUNCTOR && (c == TC_APPLICATIVE || c == TC_MONAD || c == TC_FOLDABLE)) return 1;
    if (cur == TC_APPLICATIVE && c == TC_MONAD) return 1;
    return 0;
}
static int tvar_bind(TypeChecker *tc, int id, TypeConstraint c, int line) {
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
    (void)line; return 1;
}
static int tvar_unify(TypeChecker *tc, int a, int b, int line) {
    int ra = tvar_find(tc, a), rb = tvar_find(tc, b);
    if (ra == rb) return 0;
    TypeConstraint ca = tc->tvars[ra].bound, cb = tc->tvars[rb].bound;
    if (ca != TC_NONE && cb != TC_NONE && !tc_constraint_matches(ca, cb) && !tc_constraint_matches(cb, ca))
        { (void)line; return 1; }
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
static int tvar_unify_at(TypeChecker *tc, int tvar, AbstractType *at, int line) {
    if (at->tvar_id > 0) return tvar_unify(tc, tvar, at->tvar_id, line);
    if (at->type != TC_NONE) return tvar_bind(tc, tvar, at->type, line);
    return 0;
}
/* tvar_instantiate: replicate the scheme's tvars at [base, base+count) into
   fresh tvars, remapping cross-references within the scheme.

   KNOWN LIMITATION (A7): when an internal tvar's elem/box_c/tag_p/parent
   points *outside* [base, base+count), we pass the reference through to the
   original tvar unchanged. This aliases the freshly-instantiated copy to
   the outer scope, which can leak unification constraints back onto the
   scheme across separate call sites. Properly fixing this requires
   distinguishing genuinely-quantified scheme tvars from free variables
   captured by a closure's enclosing scope, which is a bigger change.

   For now, build a fresh tvar for any out-of-range ref too, but DON'T unify
   with the original — this breaks shared state. The downside: each
   instantiation loses the "this tvar is the outer X" information. In
   practice nothing in the existing prelude/tests depends on that link. */
static void tvar_instantiate(TypeChecker *tc, int base, int count, int *map) {
    for (int i = 0; i < count; i++) map[i] = tvar_fresh(tc);
    for (int i = 0; i < count; i++) {
        int root = tvar_find(tc, base + i);
        tc->tvars[map[i]].bound = tc->tvars[root].bound;
#define REMAP(f) if (tc->tvars[root].f > 0) { int off = tc->tvars[root].f - base; \
    if (off >= 0 && off < count) tc->tvars[map[i]].f = map[off]; \
    else { int fresh = tvar_fresh(tc); tc->tvars[fresh].bound = tc->tvars[tvar_find(tc, tc->tvars[root].f)].bound; tc->tvars[map[i]].f = fresh; } }
        REMAP(elem) REMAP(box_c) REMAP(tag_p)
#undef REMAP
        tc->tvars[map[i]].union_id = tc->tvars[root].union_id;
    }
    for (int i = 0; i < count; i++) { int off = tvar_find(tc, base + i) - base; if (off >= 0 && off < count && off != i) tvar_unify(tc, map[i], map[off], 0); }
}
static int tc_find_brace_before(Token *toks, int pos) {
    for (int j = pos - 1; j >= 0; j--) {
        TokTag t = toks[j].tag;
        if (t == TOK_RBRACE || t == TOK_RPAREN || t == TOK_RBRACKET) {
            TokTag open = t==TOK_RBRACE?TOK_LBRACE:t==TOK_RPAREN?TOK_LPAREN:TOK_LBRACKET;
            int d = 1;
            for (int k = j-1; k >= 0; k--) { if (toks[k].tag == t) d++; else if (toks[k].tag == open && --d == 0) { if(t==TOK_RBRACE) return k; j = k; break; } }
            if (t == TOK_RBRACE) return -1;
            continue;
        }
        if (t==TOK_SYM||t==TOK_INT||t==TOK_FLOAT||t==TOK_STRING) continue;
        break;
    }
    return -1;
}
static int tc_extract_brace_keys(Token *toks, int bs, int tc2, uint32_t *out_syms, TypeConstraint *out_types, int max) {
    int cl = find_matching(toks, bs+1, tc2, TOK_LBRACE, TOK_RBRACE), count = 0;
    for (int j = bs+1; j < cl; ) {
        if (toks[j].tag == TOK_SYM && count < max) {
            out_syms[count] = toks[j].as.sym; TypeConstraint vt = TC_NONE; j++;
            if (j < cl) {
                if (toks[j].tag==TOK_LPAREN){vt=TC_TUPLE;j=find_matching(toks,j+1,tc2,TOK_LPAREN,TOK_RPAREN)+1;}
                else if(toks[j].tag==TOK_WORD){vt=parse_constraint(sym_name(toks[j].as.sym));j++;}
                else if(toks[j].tag==TOK_SYM){vt=parse_constraint(sym_name(toks[j].as.sym));if(vt==TC_NONE)vt=TC_SYM;j++;}
                else if(toks[j].tag==TOK_INT){vt=TC_INT;j++;}else if(toks[j].tag==TOK_FLOAT){vt=TC_FLOAT;j++;}else j++;
            }
            if(out_types) out_types[count]=vt; count++;
        } else { if(toks[j].tag==TOK_LPAREN) j=find_matching(toks,j+1,tc2,TOK_LPAREN,TOK_RPAREN)+1; else j++; }
    }
    return count;
}
static void tc_push(TypeChecker *tc, TypeConstraint type, int line) {
    if (tc->sp >= ASTACK_MAX) return;
    AbstractType *at = &tc->data[tc->sp++]; memset(at, 0, sizeof(*at));
    at->type = type; at->flags = (type == TC_BOX) ? AT_LINEAR : 0; at->source_line = line; at->effect_idx = -1;
    if (tc_is_container(type) || type == TC_SEQ) {
        at->tvar_id = tvar_fresh(tc); tc->tvars[at->tvar_id].bound = type; int sub = tvar_fresh(tc);
        if (type == TC_LIST || type == TC_SEQ || type == TC_DICT) tc->tvars[at->tvar_id].elem = sub;
        else if (type == TC_BOX) tc->tvars[at->tvar_id].box_c = sub;
        else tc->tvars[at->tvar_id].tag_p = sub;
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
    tc->errors++; const char *f = src_files[current_fid];
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "\n-- TYPE ERROR %s:%d ", f, line);
    int hl=15+(int)strlen(f)+10; for(int i=hl;i<60;i++) fputc('-',stderr);
    fprintf(stderr, "\n\n    "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n\n"); va_end(ap);
    print_source_line(stderr, current_fid, line, 0);
    if (origin_line > 0 && origin_line != line) print_source_line(stderr, current_fid, origin_line, 0);
    fprintf(stderr, "\n");
}
/* tc_error_hard: bypasses suppress_errors. Use for unsafe patterns (lend-body
   aliasing, linear captures) where silent acceptance would let bad code slip
   through speculative re-checks of the builtin prelude. */
static void tc_error_hard(TypeChecker *tc, int line, int origin_line, const char *fmt, ...) {
    int saved = tc->suppress_errors; tc->suppress_errors = 0;
    va_list ap; va_start(ap, fmt);
    tc->errors++; const char *f = src_files[current_fid];
    fprintf(stderr, "\n-- TYPE ERROR %s:%d ", f, line);
    int hl=15+(int)strlen(f)+10; for(int i=hl;i<60;i++) fputc('-',stderr);
    fprintf(stderr, "\n\n    "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n\n"); va_end(ap);
    print_source_line(stderr, current_fid, line, 0);
    if (origin_line > 0 && origin_line != line) print_source_line(stderr, current_fid, origin_line, 0);
    fprintf(stderr, "\n"); tc->suppress_errors = saved;
}
#define EFF_CONSUME(vsp,consumed,need) do{if(vsp<(need)){consumed+=(need)-vsp;vsp=0;}else vsp-=(need);}while(0)
static TypeConstraint tc_infer_effect_ctx2(Token *toks, int start, int end, int tc2,
                            int *out_consumed, int *out_produced, TypeChecker *ctx,
                            const uint32_t *outer_binds, int outer_count);
static TypeConstraint tc_infer_effect_ctx(Token *toks, int start, int end, int tc2,
                            int *out_consumed, int *out_produced, TypeChecker *ctx) {
    return tc_infer_effect_ctx2(toks, start, end, tc2, out_consumed, out_produced, ctx, NULL, 0);
}
static TypeConstraint tc_infer_effect_ctx2(Token *toks, int start, int end, int tc2,
                            int *out_consumed, int *out_produced, TypeChecker *ctx,
                            const uint32_t *outer_binds, int outer_count) {
    int vsp = 0, consumed = 0; TypeConstraint tt = TC_NONE;
    uint32_t local_binds[32]; int local_count = 0;
    for (int i = 0; i < outer_count && local_count < 32; i++) local_binds[local_count++] = outer_binds[i];
    for (int i = start; i < end; i++) {
        switch (toks[i].tag) {
        case TOK_INT: vsp++; tt = TC_INT; break;
        case TOK_FLOAT: vsp++; tt = TC_FLOAT; break;
        case TOK_SYM: vsp++; tt = TC_SYM; break;
        case TOK_STRING: vsp++; tt = TC_LIST; break;
        case TOK_LPAREN: i = find_matching(toks, i+1, tc2, TOK_LPAREN, TOK_RPAREN); vsp++; tt = TC_TUPLE; break;
        case TOK_LBRACKET: i = find_matching(toks, i+1, tc2, TOK_LBRACKET, TOK_RBRACKET); vsp++; tt = TC_LIST; break;
        case TOK_LBRACE: i = find_matching(toks, i+1, tc2, TOK_LBRACE, TOK_RBRACE); vsp++; tt = TC_REC; break;
        case TOK_WORD: {
            uint32_t sym = toks[i].as.sym;
            if (sym == S_DEF || sym == S_LET) {
                if (i >= start + 1 && toks[i-1].tag == TOK_SYM && local_count < 32) local_binds[local_count++] = toks[i-1].as.sym;
                else if (sym == S_DEF && i >= start + 3 && toks[i-3].tag == TOK_SYM && local_count < 32) local_binds[local_count++] = toks[i-3].as.sym;
                EFF_CONSUME(vsp,consumed,2);
            }
            else if (sym == S_RECUR) { EFF_CONSUME(vsp,consumed,1); }
            else {
                TypeSig *sig = typesig_find(sym);
                if (sig) {
                    int ni = 0, no = 0; TypeConstraint lo = TC_NONE;
                    for (int j = 0; j < sig->slot_count; j++) { if (sig->slots[j].direction == DIR_IN) ni++; else { no++; lo = sig->slots[j].constraint; } }
                    EFF_CONSUME(vsp,consumed,ni); vsp += no; if (no > 0) tt = lo;
                } else {
                    HOEffect *ho = ho_ops_find(sym);
                    if (ho) { int need = ho->need, out = ho->out;
                        if (ctx && sym == S_IF && i >= start + 2 && i-1 >= start && toks[i-1].tag == TOK_RPAREN) {
                            int bp = i-1, ep = bp, d = 1;
                            for (int k = bp-1; k >= start; k--) { if (toks[k].tag == TOK_RPAREN) d++; else if (toks[k].tag == TOK_LPAREN && --d == 0) { ep = k; break; } }
                            int bc = 0, bp2 = 0; tc_infer_effect_ctx2(toks, ep+1, bp, tc2, &bc, &bp2, ctx, local_binds, local_count); need = ho->need + bc; out = bp2;
                        }
                        EFF_CONSUME(vsp,consumed,need); vsp += out; if (out > 0) tt = ho->out_type;
                    }
                }
                if (!sig && !ho_ops_find(sym)) {
                    int is_local = 0; for (int j = 0; j < local_count; j++) if (local_binds[j] == sym) { is_local = 1; break; }
                    if (is_local) { vsp++; tt = TC_NONE; }
                    else if (ctx) { TCBinding *ub = tc_lookup(ctx, sym);
                        if (ub && ub->is_def && ub->atype.type == TC_TUPLE && ub->atype.effect_idx >= 0) { TupleEffect *e = &ctx->effects[ub->atype.effect_idx]; EFF_CONSUME(vsp,consumed,e->consumed); vsp += e->produced; if (e->produced > 0) tt = e->out_type; }
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
    int avail = tc->sp - tc->sp_floor; tc->sp -= (consumed <= avail) ? consumed : avail;
    for (int i = 0; i < produced; i++) tc_push(tc, (i == produced - 1) ? out_type : TC_NONE, line);
}
static int tc_check_body_against_sig(Token *toks, int start, int end, int total_count, TypeSig *sig) {
    int errors = 0, n_in = 0, n_out = 0;
    for (int i = 0; i < sig->slot_count; i++)
        if (sig->slots[i].direction == DIR_IN) n_in++; else n_out++;
    int eff_consumed, eff_produced;
    tc_infer_effect_ctx(toks, start, end, total_count, &eff_consumed, &eff_produced, NULL);
    if (eff_consumed != n_in) { fprintf(stderr, "%s:%d: type error: function body consumes %d value(s) but type declares %d input(s)\n", src_files[current_fid], toks[start].line, eff_consumed, n_in); errors++; }
    if (eff_produced != n_out) { fprintf(stderr, "%s:%d: type error: function body produces %d value(s) but type declares %d output(s)\n", src_files[current_fid], toks[start].line, eff_produced, n_out); errors++; }
    return errors;
}
static int tc_is_copyable(AbstractType *t) { return !(t->flags & AT_LINEAR) && t->type != TC_BOX; }
static int tc_is_builtin(uint32_t sym, int prelude_sig_count) {
    for (int i = 0; i < prelude_sig_count; i++) if (type_sigs[i].sym == sym) return 1;
    return ho_ops_find(sym) != NULL;
}
static void tc_bind(TypeChecker *tc, uint32_t sym, AbstractType *atype, int is_def, int line) {
    for (int i = 0; i < tc->bind_count; i++) {
        if (tc->bindings[i].sym == sym) { tc->bindings[i].atype = *atype; tc->bindings[i].is_def = is_def; tc->bindings[i].def_line = line; tc->bindings[i].body_depth = tc->body_depth; tc->bindings[i].consumed_line = 0; return; }
    }
    if (tc->bind_count < TC_BINDS_MAX) {
        tc->bindings[tc->bind_count].sym = sym; tc->bindings[tc->bind_count].atype = *atype;
        tc->bindings[tc->bind_count].is_def = is_def; tc->bindings[tc->bind_count].def_line = line;
        tc->bindings[tc->bind_count].body_depth = tc->body_depth; tc->bindings[tc->bind_count].consumed_line = 0; tc->bind_count++;
    }
}
static void tc_apply_scheme(TypeChecker *tc, TupleEffect *eff, int consumed, int produced,
                            TypeConstraint body_out, const char *name, int line, int fu) {
    int map[64] = {0}, sc = eff->scheme_count; if (sc > 64) sc = 64;
    tvar_instantiate(tc, eff->scheme_base, sc, map);
    for (int j = 0; j < eff->in_count && j < tc->sp; j++) {
        int stv = eff->in_tvars[j] - eff->scheme_base;
        if (stv >= 0 && stv < sc) { int ftv = map[stv], idx = tc->sp - eff->in_count + j;
            if (idx < 0 || idx >= 256) continue;
            AbstractType *inp = &tc->data[idx];
            if (tvar_unify_at(tc, ftv, inp, line))
                tc_error(tc, line, inp->source_line, "'%s' input type mismatch: expected %s, got %s (value from line %d)", name, constraint_name(tvar_resolve(tc, ftv)), constraint_name(inp->type != TC_NONE ? inp->type : tvar_resolve(tc, inp->tvar_id)), inp->source_line);
            if (fu && inp->tvar_id > 0) {
#define UNIFY_SUB(f) { int fa=tc->tvars[tvar_find(tc,ftv)].f, fb=tc->tvars[tvar_find(tc,inp->tvar_id)].f; if(fa>0&&fb>0)tvar_unify(tc,fa,fb,line); }
                UNIFY_SUB(elem) UNIFY_SUB(box_c) UNIFY_SUB(tag_p)
#undef UNIFY_SUB
            }
        }
    }
    int avail = tc->sp - tc->sp_floor; tc->sp -= (consumed <= avail) ? consumed : avail;
    for (int j = 0; j < eff->out_count; j++) {
        int stv = eff->out_tvars[j] - eff->scheme_base, ftv = (stv >= 0 && stv < sc) ? map[stv] : 0;
        if (ftv > 0) { TypeConstraint r = tvar_resolve(tc, ftv);
            if (tc_is_container(r)) {
                tc_push(tc, r, line);
                int fc = tvar_content(tc, ftv, r), oc = tvar_content(tc, tc->data[tc->sp-1].tvar_id, r);
                if (fc > 0 && oc > 0) tvar_unify(tc, oc, fc, line);
                int uid = tc->tvars[tvar_find(tc, ftv)].union_id;
                if (uid > 0) tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].union_id = uid;
            } else {
                tc_push(tc, TC_NONE, line); if (r != TC_NONE) tc->data[tc->sp-1].type = r;
                tc->data[tc->sp-1].tvar_id = ftv; if (fu && r == TC_BOX) tc->data[tc->sp-1].flags |= AT_LINEAR;
            }
        } else tc_push(tc, TC_NONE, line);
    }
    for (int j = eff->out_count; j < produced; j++) tc_push(tc, (j == produced - 1) ? body_out : TC_NONE, line);
}
static void tc_apply_ho(TypeChecker *tc, HOEffect *ho, int line) {
    int eff_c = 0, eff_p = 0, bk = 0, boe = -1; TypeConstraint bo = TC_NONE; TupleEffect *bteff = NULL;
    TypeConstraint bouts[8] = {0}; int bc = 0;
    int barr_c[8] = {0}, barr_p[8] = {0}, barr_has[8] = {0};
    AbstractType saved = {0}; int had_saved = 0;
    if ((tc->sp - tc->sp_floor) < ho->need) {
        tc_error(tc, line, 0, "'%s' needs %d input(s), stack has %d", ho->name, ho->need, tc->sp - tc->sp_floor);
        tc->sp = tc->sp_floor;
        for (int j = 0; j < ho->out; j++) tc_push(tc, ho->out_type, line);
        return;
    }
    if (ho->flags & (HO_BOX_BORROW|HO_BOX_MUTATE)) {
        if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE) {
            if (tc->data[tc->sp-1].effect_idx >= 0) { TupleEffect *te = &tc->effects[tc->data[tc->sp-1].effect_idx]; eff_c = te->consumed; eff_p = te->produced; bo = te->out_type; bk = 1; bteff = te; }
            tc->sp--;
        }
        if (tc->sp > 0 && tc->data[tc->sp-1].type != TC_BOX && tc->data[tc->sp-1].type != TC_NONE)
            tc_error(tc, line, 0, "'%s' expected box, got %s", ho->name, constraint_name(tc->data[tc->sp-1].type));
#define TC_BOX_CONTENTS(tc) ({ TypeConstraint _c=TC_NONE; if((tc)->data[(tc)->sp-1].tvar_id>0){ int _bc=(tc)->tvars[tvar_find((tc),(tc)->data[(tc)->sp-1].tvar_id)].box_c; if(_bc>0)_c=tvar_resolve((tc),_bc); } _c; })
        if ((ho->flags & HO_BOX_BORROW) && tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
            TypeConstraint ct = TC_BOX_CONTENTS(tc); tc->data[tc->sp-1].borrowed++;
            if (bk && bteff && bteff->has_let && (ct == TC_LIST || ct == TC_REC || ct == TC_TUPLE || ct == TC_TAGGED))
                tc_error_hard(tc, line, 0, "'lend' body may not 'let'/'def'-bind values when the box contains a compound value (%s) — the borrowed snapshot aliases the box's backing storage and later 'mutate' calls would silently corrupt the binding", constraint_name(ct));
            int r = bk ? (1 - eff_c + eff_p) : 1; if (r < 0) r = 0;
            for (int j = 0; j < r; j++) tc_push(tc, (j==r-1&&bo!=TC_NONE)?bo:(j==0&&ct!=TC_NONE)?ct:TC_NONE, line);
            int bi = tc->sp - r - 1; if (bi >= 0) tc->data[bi].borrowed--;
        } else if ((ho->flags & HO_BOX_MUTATE) && tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
            TypeConstraint ct = TC_BOX_CONTENTS(tc);
            if (ct != TC_NONE && bo != TC_NONE && !tc_constraint_matches(ct, bo) && !tc_constraint_matches(bo, ct))
                tc_error(tc, line, 0, "'mutate' body produces %s but box contains %s", constraint_name(bo), constraint_name(ct));
        }
        return;
    }
    TypeConstraint lpt = TC_NONE; int tptv = 0, lptv = 0;
    for (int n = ho->need; n > 0 && tc->sp > tc->sp_floor; n--) {
        AbstractType *top = &tc->data[tc->sp - 1];
        if ((ho->flags & HO_SAVES_UNDER) && n == 1) { saved = *top; had_saved = 1; tc->sp--; continue; }
        lpt = top->type; lptv = top->tvar_id;
        if (top->type == TC_TAGGED && top->tvar_id > 0 && !tptv) tptv = tvar_content(tc, top->tvar_id, TC_TAGGED);
        int ib = (n > 1) && !((ho->flags & HO_SCRUTINEE_TAGGED) && n == ho->need);
        if (top->type == TC_TUPLE && top->effect_idx >= 0) {
            TupleEffect *te = &tc->effects[top->effect_idx];
            if (!bk) { eff_c = te->consumed; eff_p = te->produced; bo = te->out_type; bk = 1; boe = te->out_effect; bteff = te; }
            if (ib && bc < 8) { barr_c[bc]=te->consumed; barr_p[bc]=te->produced; barr_has[bc]=1; bouts[bc++] = te->out_type; }
        } else if (ib && top->type != TC_NONE && bc < 8) bouts[bc++] = top->type;
        tc->sp--;
    }
    if ((ho->flags & HO_BODY_1TO1) && bk && (eff_c != 1 || eff_p != 1) && (eff_c + eff_p > 0))
        tc_error(tc, line, 0, "'%s' body must be 1->1, got %d->%d", ho->name, eff_c, eff_p);
    if ((ho->flags & HO_BRANCHES_AGREE) && bc >= 2 && !tc->sp_floor) {
        TypeConstraint ref = TC_NONE;
        for (int i = 0; i < bc; i++) if (bouts[i] != TC_NONE) { ref = bouts[i]; break; }
        for (int i = 0; i < bc; i++)
            if (bouts[i] != TC_NONE && ref != TC_NONE && bouts[i] != ref && !tc_constraint_matches(ref, bouts[i]) && !tc_constraint_matches(bouts[i], ref))
                tc_error(tc, line, 0, "'%s' branches produce different types: %s vs %s", ho->name, constraint_name(ref), constraint_name(bouts[i]));
        if (!tc->in_recur_body) {
            int rnet = 0, rset = 0, rci = 0;
            for (int i = 0; i < bc; i++) if (barr_has[i]) { rnet = barr_p[i] - barr_c[i]; rci = i; rset = 1; break; }
            if (rset) for (int i = 0; i < bc; i++)
                if (barr_has[i] && (barr_p[i] - barr_c[i]) != rnet)
                    tc_error(tc, line, 0, "'%s' branches have different stack effects: net %+d vs net %+d (%d->%d vs %d->%d)", ho->name, rnet, barr_p[i]-barr_c[i], barr_c[rci], barr_p[rci], barr_c[i], barr_p[i]);
        }
    }
    if ((ho->flags & HO_SCRUTINEE_TAGGED) && lpt != TC_TAGGED && lpt != TC_NONE)
        tc_error(tc, line, 0, "'%s' expected tagged value, got %s", ho->name, constraint_name(lpt));
    if ((ho->flags & HO_BODY_1TO1) && (ho->flags & HO_SCRUTINEE_TAGGED) && bteff && bteff->in_count > 0 && lptv > 0) {
        int uid = tc->tvars[tvar_find(tc, lptv)].union_id;
        if (uid > 0) { UnionDef *ud = &tc->unions[uid-1]; TypeConstraint ok_t = TC_NONE;
            for (int v=0;v<ud->count;v++) if(ud->syms[v]==S_OK){ok_t=ud->types[v];break;}
            if (ok_t != TC_NONE) { int iv=bteff->in_tvars[0]; TypeConstraint bi=iv>0?tvar_resolve(tc,iv):TC_NONE;
                if(bi!=TC_NONE&&!tc_constraint_matches(bi,ok_t)&&!tc_constraint_matches(ok_t,bi)) tc_error(tc,line,0,"'then' body expects %s but 'ok variant has %s",constraint_name(bi),constraint_name(ok_t)); } }
    }
    if (ho->flags & HO_APPLY_EFFECT) {
        if (bk) { tc_apply_effect(tc, eff_c, eff_p, bo, line); if (boe >= 0 && tc->sp > 0 && tc->data[tc->sp-1].type == TC_TUPLE) tc->data[tc->sp-1].effect_idx = boe; }
        if (had_saved && tc->sp < ASTACK_MAX) tc->data[tc->sp++] = saved;
        return;
    }
    if (ho->sym == S_IF && bk) {
        if (bteff && bteff->scheme_count > 0 && bteff->in_count > 0) tc_apply_scheme(tc, bteff, eff_c, eff_p, bo, "if", line, 0);
        else tc_apply_effect(tc, eff_c, eff_p, bo, line);
        return;
    }
    TypeConstraint out = ho->out_type;
    if (out == TC_NONE && (ho->flags & HO_BRANCHES_AGREE))
        for (int i = 0; i < bc; i++) if (bouts[i] != TC_NONE) { out = bouts[i]; break; }
    /* Functor/Monad ops: output type matches input container type */
    if ((out == TC_FUNCTOR || out == TC_MONAD) && lpt != TC_NONE && tc_is_concrete(lpt)) out = lpt;
    else if ((out == TC_FUNCTOR || out == TC_MONAD) && (lpt == TC_NONE || !tc_is_concrete(lpt)) && lptv > 0) {
        TypeConstraint r = tvar_resolve(tc, lptv); if (tc_is_concrete(r)) out = r;
    }
    for (int j = 0; j < ho->out; j++) tc_push(tc, (j == ho->out - 1) ? out : TC_NONE, line);
    if ((ho->flags & HO_BODY_1TO1) && out == TC_TAGGED && ho->out_type != TC_MONAD && tc->sp > 0 && tc->data[tc->sp-1].tvar_id > 0) {
        int otp = tvar_content(tc, tc->data[tc->sp-1].tvar_id, TC_TAGGED);
        if (otp > 0) { if (bo != TC_NONE) tvar_bind(tc, otp, bo, line); else if (tptv > 0) tvar_unify(tc, otp, tptv, line); }
        if (lptv > 0) { int uid = tc->tvars[tvar_find(tc, lptv)].union_id; if (uid > 0) tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].union_id = uid; }
    }
    if ((ho->flags & HO_BRANCHES_AGREE) && (ho->flags & HO_SCRUTINEE_TAGGED) && tc->sp > 0) {
        if (out == TC_NONE && bo != TC_NONE) {
            AbstractType *o = &tc->data[tc->sp-1]; o->type = bo;
            if (bo == TC_TAGGED) { o->tvar_id=tvar_fresh(tc);tc->tvars[o->tvar_id].bound=TC_TAGGED; }
        } else if (out == TC_NONE && tptv > 0) {
            TypeConstraint pt = tvar_resolve(tc, tptv);
            if (pt != TC_NONE) {
                AbstractType *o = &tc->data[tc->sp-1]; o->type = pt;
                if (pt == TC_TAGGED) { o->tvar_id=tvar_fresh(tc);tc->tvars[o->tvar_id].bound=TC_TAGGED;
                    int sub=tvar_fresh(tc);tc->tvars[o->tvar_id].tag_p=sub;int inner=tc->tvars[tvar_find(tc,tptv)].tag_p;if(inner>0)tvar_unify(tc,sub,inner,line); }
            }
        }
    }
}
static void tc_check_word(TypeChecker *tc, uint32_t sym, int line) {
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
            tc_error_hard(tc, line, b->consumed_line, "linear-capturing closure '%s' has already been consumed (previous use on line %d) — a closure that captures a linear value can only be applied once", sym_name(sym), b->consumed_line);
            tc_push(tc, b->atype.type, line); tc->data[tc->sp-1].flags |= AT_CONSUMED; return;
        }
        if (is_linear_closure) b->consumed_line = line;
        if (b->is_def && b->atype.type == TC_TUPLE && b->atype.effect_idx >= 0) {
            TupleEffect *eff = &tc->effects[b->atype.effect_idx];
            if (eff->scheme_count > 0) tc_apply_scheme(tc, eff, eff->consumed, eff->produced, eff->out_type, sym_name(sym), line, 1);
            else tc_apply_effect(tc, eff->consumed, eff->produced, eff->out_type, line);
            return;
        } else { tc_push(tc, b->atype.type, line); if (b->atype.flags & AT_LINEAR) { tc->data[tc->sp-1].flags |= AT_LINEAR; tc->saw_linear_capture = 1; } return; }
      }
    }
    if (tc->unknown_count < TC_UNKNOWN_MAX) { tc->unknowns[tc->unknown_count].sym = sym; tc->unknowns[tc->unknown_count].line = line; tc->unknown_count++; }
    return;
apply_sig:;
    int ni = 0;
    for (int i = 0; i < sig->slot_count; i++) if (sig->slots[i].direction == DIR_IN) ni++;
    if (tc->sp < ni) {
        tc_error(tc, line, 0, "'%s' needs %d input(s), stack has %d", sym_name(sym), ni, tc->sp);
        if (tc->sp > 0 && !tc->suppress_errors) { fprintf(stderr, "    stack (top first):"); for(int d=tc->sp-1;d>=0&&d>=tc->sp-5;d--) fprintf(stderr," %s",constraint_name(tc->data[d].type)); fprintf(stderr,"\n"); }
        return;
    }
    #define MAX_TVARS 16
    struct { uint32_t var; int tvar; uint32_t src_sym; int src_effect_idx; } tm[MAX_TVARS]; int tmc = 0;
    for (int i = 0; i < sig->slot_count; i++) {
        uint32_t tv = sig->slots[i].type_var; if (!tv) continue;
        int found = 0; for (int j = 0; j < tmc; j++) if (tm[j].var == tv) { found = 1; break; }
        if (!found && tmc < MAX_TVARS) {
            int id = tvar_fresh(tc); TypeConstraint c = sig->slots[i].constraint;
            if (!tc_is_container(c) && c != TC_NONE) tc->tvars[id].bound = c;
            if (tc_is_container(c) && sig->slots[i].elem_constraint != TC_NONE) tc->tvars[id].bound = sig->slots[i].elem_constraint;
            tm[tmc].var = tv; tm[tmc].tvar = id; tm[tmc].src_sym = 0; tm[tmc].src_effect_idx = -1; tmc++;
        }
    }
    #define FIND_TVAR(tv_name) ({ int _tv = 0; for (int _j = 0; _j < tmc; _j++) if (tm[_j].var == (tv_name)) { _tv = tm[_j].tvar; break; } _tv; })
    AbstractType passthrough[8] = {0}; int pt_count = 0;
    int sp2 = tc->sp - 1;
    for (int i = sig->slot_count - 1; i >= 0; i--) {
        TypeSlot *s = &sig->slots[i]; if (s->direction != DIR_IN || sp2 < 0) { if (s->direction == DIR_IN) sp2--; continue; }
        AbstractType *at = &tc->data[sp2];
        if (s->ownership == OWN_AUTO && (at->flags & AT_LINEAR) && pt_count < 8) { passthrough[pt_count++] = *at; at->flags |= AT_CONSUMED; }
        if (s->ownership == OWN_COPY && !tc_is_copyable(at))
            tc_error(tc, line, at->source_line, "'%s' requires copyable value, got linear type (value from line %d)", sym_name(sym), at->source_line);
        if (s->ownership == OWN_OWN && at->borrowed > 0)
            tc_error(tc, line, at->source_line, "'%s' cannot consume value that is currently borrowed (lent, value from line %d)", sym_name(sym), at->source_line);
        if (s->constraint != TC_NONE && at->type != TC_NONE && !tc_constraint_matches(s->constraint, at->type))
            tc_error(tc, line, at->source_line, "'%s' expected %s, got %s (value from line %d)", sym_name(sym), constraint_name(s->constraint), constraint_name(at->type), at->source_line);
        if (s->constraint != TC_NONE && at->type == TC_NONE && at->tvar_id > 0) tvar_bind(tc, at->tvar_id, s->constraint, line);
        if (s->type_var) { int tv = FIND_TVAR(s->type_var); if (tv > 0) {
            if (tc_is_container(s->constraint) && at->tvar_id > 0) {
                int ef = tvar_content(tc, at->tvar_id, s->constraint);
                if (ef > 0 && tvar_unify(tc, tv, ef, line))
                    tc_error(tc, line, at->source_line, "'%s' type variable '%s' mismatch: expected %s, got %s", sym_name(sym), sym_name(s->type_var), constraint_name(tvar_resolve(tc, tv)), constraint_name(tvar_resolve(tc, ef)));
            } else if (!tc_is_container(s->constraint)) {
                int fail = at->tvar_id > 0 ? tvar_unify(tc, tv, at->tvar_id, line) : (at->type != TC_NONE ? tvar_bind(tc, tv, at->type, line) : 0);
                if (fail) tc_error(tc, line, at->source_line, "'%s' type variable '%s' mismatch: expected %s, got %s", sym_name(sym), sym_name(s->type_var), constraint_name(tvar_resolve(tc, tv)), constraint_name(at->type != TC_NONE ? at->type : tvar_resolve(tc, at->tvar_id)));
            }
            for (int j = 0; j < tmc; j++) if (tm[j].var == s->type_var) { if (at->sym_id) tm[j].src_sym = at->sym_id; if (at->effect_idx >= 0) tm[j].src_effect_idx = at->effect_idx; break; }
        }}
        if ((at->flags & AT_LINEAR) && s->ownership == OWN_OWN) at->flags |= AT_CONSUMED;
        sp2--;
    }
    tc->sp -= ni; if (tc->sp < tc->sp_floor) tc->sp = tc->sp_floor;
    for (int i = pt_count - 1; i >= 0; i--) {
        if (tc->sp >= ASTACK_MAX) break;
        tc_push(tc, passthrough[i].type, line);
        AbstractType *o = &tc->data[tc->sp-1];
        o->tvar_id = passthrough[i].tvar_id; o->flags |= AT_LINEAR;
    }
    for (int i = 0; i < sig->slot_count; i++) {
        TypeSlot *s = &sig->slots[i]; if (s->direction != DIR_OUT) continue;
        if (tc->sp >= ASTACK_MAX) { tc->errors++; return; }
        tc_push(tc, s->constraint, line); AbstractType *at = &tc->data[tc->sp - 1];
        if (s->type_var) { int tv = FIND_TVAR(s->type_var); if (tv > 0) {
            if (tc_is_container(s->constraint) && at->tvar_id > 0) { int ef = tvar_content(tc, at->tvar_id, s->constraint); if (ef > 0) tvar_unify(tc, ef, tv, line); }
            else if (!tc_is_container(s->constraint)) { TypeConstraint r = tvar_resolve(tc, tv); if (r != TC_NONE) at->type = r; at->tvar_id = tv; if (r == TC_BOX) at->flags |= AT_LINEAR; }
        }}
        if (tc_is_container(s->constraint) && s->elem_constraint != TC_NONE && at->tvar_id > 0) { int ef = tvar_content(tc, at->tvar_id, s->constraint); if (ef > 0) tvar_bind(tc, ef, s->elem_constraint, line); }
        if (s->type_var) for (int j = 0; j < tmc; j++) if (tm[j].var == s->type_var) { if (tm[j].src_sym && !tc_is_container(s->constraint)) at->sym_id = tm[j].src_sym; if (tm[j].src_effect_idx >= 0 && !tc_is_container(s->constraint)) at->effect_idx = tm[j].src_effect_idx; break; }
        if (s->either_count > 0 && at->tvar_id > 0 && tc->union_count < UNION_MAX) {
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
    return strcmp(n,"box")==0 || strcmp(n,"clone")==0;
}
static TypeConstraint tc_check_list_elements(TypeChecker *tc, Token *toks, int start, int end, int tc2, int line) {
    TypeConstraint et = TC_NONE; int ec = 0;
    for (int i = start; i < end; i++) { TypeConstraint tt = TC_NONE;
        switch (toks[i].tag) {
        case TOK_INT: tt=TC_INT; break; case TOK_FLOAT: tt=TC_FLOAT; break; case TOK_SYM: tt=TC_SYM; break; case TOK_STRING: tt=TC_LIST; break;
        case TOK_LPAREN: tt=TC_TUPLE;i=find_matching(toks,i+1,tc2,TOK_LPAREN,TOK_RPAREN);break;
        case TOK_LBRACKET: tt=TC_LIST;i=find_matching(toks,i+1,tc2,TOK_LBRACKET,TOK_RBRACKET);break;
        case TOK_LBRACE: tt=TC_REC;i=find_matching(toks,i+1,tc2,TOK_LBRACE,TOK_RBRACE);break;
        case TOK_WORD: return TC_NONE;
        default: continue; }
        if (tt == TC_NONE) continue;
        if (++ec == 1) et = tt;
        else if (tt != et) { tc_error(tc, line, 0, "list elements have inconsistent types: element 1 is %s, element %d is %s", constraint_name(et), ec, constraint_name(tt)); return TC_NONE; }
    }
    return et;
}
static void tc_check_redef(TypeChecker *tc, uint32_t sym, int line) {
    int saved = tc->suppress_errors; tc->suppress_errors = 0;
    if (tc_is_builtin(sym, tc->prelude_sig_count)) tc_error(tc, line, 0, "'%s' is already defined", sym_name(sym));
    else { TCBinding *existing = tc_lookup(tc, sym); if (existing) tc_error(tc, line, 0, "'%s' is already defined (first defined on line %d)", sym_name(sym), existing->def_line); }
    tc->suppress_errors = saved;
}
static void tc_process_range(TypeChecker *tc, Token *toks, int start, int end, int total_count) {
    for (int i = start; i < end; i++) {
        if (i == tc->user_start && !tc->prelude_sig_count) tc->prelude_sig_count = type_sig_count;
        Token *t = &toks[i]; current_line = t->line; current_col = t->col; current_fid = t->fid;
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
            int scheme_base = tc->tvar_count, ic = 0, oc = 0, out_eff = -1;
            int itv[8] = {0}, otv[8] = {0}, sc = 0;
            { int _s[]={tc->sp,tc->bind_count,tc->unknown_count,tc->recur_pending,tc->suppress_errors,type_sig_count,tc->effect_count,tc->sp_floor,tc->saw_linear_capture,tc->in_recur_body};
                int wide = (eff_c > 8 || eff_p > 8), skip = 0;
                tc->saw_linear_capture = 0;
                if (!is_simple) { tc->sp_floor = tc->sp; }
                if (wide && !skip) tc->suppress_errors = 1;
                ic = wide ? 0 : eff_c;
                for (int j = 0; j < ic; j++) {
                    itv[j] = tvar_fresh(tc); tc->tvars[itv[j]].elem = tvar_fresh(tc); tc->tvars[itv[j]].box_c = tvar_fresh(tc); tc->tvars[itv[j]].tag_p = tvar_fresh(tc);
                    tc_push(tc, TC_NONE, t->line); tc->data[tc->sp-1].tvar_id = itv[j];
                }
                if (wide && !skip) for (int j = 0; j < eff_c && tc->sp < ASTACK_MAX; j++) tc_push(tc, TC_NONE, t->line);
                if (tc->recur_pending && tc->recur_sym) {
                    int pe = tc_alloc_effect(tc); tc->effects[pe].consumed = eff_c; tc->effects[pe].produced = eff_p; tc->effects[pe].out_type = eff_out;
                    AbstractType pa = {0}; pa.type = TC_TUPLE; pa.effect_idx = pe; tc_bind(tc, tc->recur_sym, &pa, 1, t->line);
                    tc->in_recur_body = 1;  /* permit mismatched branch effects inside this body */
                }
                /* Inside the body, recur_pending must be 0 so nested defs don't
                   mis-attribute to recur_sym. _s[3] restores it at exit so the
                   outer def post-body still consumes it. */
                tc->recur_pending = 0;
                int is_lend_body = (close+1 < total_count && toks[close+1].tag == TOK_WORD && strcmp(sym_name(toks[close+1].as.sym), "lend") == 0);
                if (!skip) { tc->body_depth++; if (is_lend_body) tc->lend_depth++; tc_process_range(tc, toks, i+1, close, total_count); if (is_lend_body) tc->lend_depth--; tc->body_depth--; }
                if (wide) tc->suppress_errors = _s[4];
                else if (!skip) {
                    int ao = tc->sp - _s[0]; oc = ao > 8 ? 8 : (ao > 0 ? ao : 0);
                    for (int j = 0; j < oc; j++) { int idx = tc->sp - oc + j; if (idx < 0) continue;
                        otv[j] = tc->data[idx].tvar_id;
                        if (!otv[j]) { otv[j] = tvar_fresh(tc); if (tc->data[idx].type != TC_NONE) tc->tvars[otv[j]].bound = tc->data[idx].type; } }
                    sc = tc->tvar_count - scheme_base;
                    if (ao == 1 && tc->data[tc->sp-1].type == TC_TUPLE && tc->data[tc->sp-1].effect_idx >= 0) out_eff = tc->data[tc->sp-1].effect_idx;
                }
                int body_captured = tc->saw_linear_capture;
                tc->sp=_s[0];tc->bind_count=_s[1];tc->unknown_count=_s[2];tc->recur_pending=_s[3];tc->suppress_errors=_s[4];type_sig_count=_s[5];tc->effect_count=_s[6];tc->sp_floor=_s[7];tc->saw_linear_capture=_s[8];tc->in_recur_body=_s[9];
                tc_push(tc, TC_TUPLE, t->line);
                int eidx = tc_alloc_effect(tc); TupleEffect *eff = &tc->effects[eidx];
                eff->consumed = eff_c; eff->produced = eff_p; eff->out_type = eff_out; eff->out_effect = out_eff;
                eff->scheme_base = scheme_base; eff->scheme_count = sc; eff->in_count = ic; eff->out_count = oc;
                eff->captures_linear = body_captured;
                for (int j = 0; j < ic; j++) eff->in_tvars[j] = itv[j];
                for (int j = 0; j < oc; j++) eff->out_tvars[j] = otv[j];
                for (int k = i+1; k < close; k++) if (toks[k].tag == TOK_WORD && (toks[k].as.sym == S_LET || toks[k].as.sym == S_DEF)) { eff->has_let = 1; break; }
                tc->data[tc->sp-1].effect_idx = eidx;
                if (body_captured) tc->data[tc->sp-1].flags |= AT_LINEAR;
            }
            i = close; break;
        }
        case TOK_LBRACKET: {
            int close = find_matching(toks, i+1, total_count, TOK_LBRACKET, TOK_RBRACKET);
            int is_type_annot = (close+1 < total_count && toks[close+1].tag == TOK_WORD && toks[close+1].as.sym == S_EFFECT);
            if (!is_type_annot) {
                for (int j = i+1; j < close; j++) {
                    if (toks[j].tag == TOK_LPAREN) { j = find_matching(toks, j+1, total_count, TOK_LPAREN, TOK_RPAREN); continue; }
                    if (toks[j].tag == TOK_LBRACKET) { j = find_matching(toks, j+1, total_count, TOK_LBRACKET, TOK_RBRACKET); continue; }
                    if (toks[j].tag == TOK_LBRACE) { j = find_matching(toks, j+1, total_count, TOK_LBRACE, TOK_RBRACE); continue; }
                    if (toks[j].tag == TOK_WORD && tc_word_produces_linear(toks[j].as.sym))
                        tc_error(tc, t->line, 0, "list literal cannot contain linear values produced by '%s'", sym_name(toks[j].as.sym));
                }
            }
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
            if (sym == S_DEF) {
                if (tc->recur_pending) {
                    if (tc->sp >= 1) { AbstractType vt = tc->data[--tc->sp]; if (i >= tc->user_start) tc_check_redef(tc, tc->recur_sym, t->line); tc_bind(tc, tc->recur_sym, &vt, 1, t->line); }
                    tc->recur_pending = 0;
                } else if (tc->sp >= 2) {
                    AbstractType vt = tc->data[tc->sp-1]; uint32_t ns = tc->data[tc->sp-2].sym_id; tc->sp -= 2;
                    if (ns) {
                        if (tc->body_depth > 0 && ((vt.flags & AT_LINEAR) || vt.type == TC_BOX)) {
                            int seen = 0;
                            for (int k = i+1; k < end; k++) if (toks[k].tag == TOK_WORD && toks[k].as.sym == ns) { seen = 1; break; }
                            if (!seen) tc_error(tc, t->line, vt.source_line, "linear value bound as '%s' is never referenced in the enclosing quotation — it will be captured and leaked", sym_name(ns));
                        }
                        if (i >= tc->user_start) tc_check_redef(tc, ns, t->line); tc_bind(tc, ns, &vt, 1, t->line);
                    }
                } else if (tc->sp_floor == 0) tc->sp = 0;
            } else if (sym == S_LET) {
                if (tc->sp >= 2) {
                    uint32_t ns = tc->data[tc->sp-1].sym_id; AbstractType val_t = tc->data[tc->sp-2]; tc->sp -= 2;
                    if (ns) {
                        if (tc->body_depth > 0 && ((val_t.flags & AT_LINEAR) || val_t.type == TC_BOX)) {
                            int seen = 0;
                            for (int k = i+1; k < end; k++) if (toks[k].tag == TOK_WORD && toks[k].as.sym == ns) { seen = 1; break; }
                            if (!seen) tc_error(tc, t->line, val_t.source_line, "linear value bound as '%s' is never referenced in the enclosing quotation — it will be captured and leaked", sym_name(ns));
                        }
                        if (tc->lend_depth > 0 && (val_t.type == TC_LIST || val_t.type == TC_REC || val_t.type == TC_TUPLE || val_t.type == TC_TAGGED)) {
                            tc_error_hard(tc, t->line, val_t.source_line, "cannot 'let'-bind compound value '%s' (%s) inside a 'lend' body — the snapshot aliases the box's backing storage and would observe later mutations", sym_name(ns), constraint_name(val_t.type));
                            break;  /* skip the bind; avoid corrupting downstream inference */
                        }
                        if (i >= tc->user_start) { int ss=tc->suppress_errors; tc->suppress_errors=0;
                            if(tc_is_builtin(ns,tc->prelude_sig_count)) tc_error(tc,t->line,0,"'%s' shadows existing definition",sym_name(ns));
                            else{TCBinding *ex=tc_lookup(tc,ns);if(ex)tc_error(tc,t->line,0,"'%s' %s (line %d)",sym_name(ns),ex->is_def?"shadows existing definition":"is already bound",ex->def_line);}
                            tc->suppress_errors=ss; }
                        tc_bind(tc, ns, &val_t, 0, t->line);
                    }
                } else if (tc->sp_floor == 0) tc->sp = 0;
            } else if (sym == S_RECUR) {
                if (tc->sp >= 1) { uint32_t ns = tc->data[tc->sp-1].sym_id; tc->recur_pending = 1; tc->sp--; if (ns) tc->recur_sym = ns; }
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
                            int d2=1; for(int k=b2-1;k>=0;k--){if(toks[k].tag==TOK_RPAREN)d2++;else if(toks[k].tag==TOK_LPAREN&&--d2==0){tc->errors+=tc_check_body_against_sig(toks,k+1,b2,total_count,&sig);break;}} break; }
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
            } else if (sym == S_UNION) {
                if (tc->sp > 0 && (tc->data[tc->sp-1].type == TC_REC || tc->data[tc->sp-1].type == TC_TUPLE)) tc->sp--;
                if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_TAGGED && tc->data[tc->sp-1].tvar_id > 0) {
                    int brace = tc_find_brace_before(toks, i);
                    if (brace >= 0 && tc->union_count < UNION_MAX) {
                        int uid = ++tc->union_count; UnionDef *ud = &tc->unions[uid-1];
                        ud->count = tc_extract_brace_keys(toks, brace, total_count, ud->syms, ud->types, UNION_VARIANTS_MAX);
                        tc->tvars[tvar_find(tc, tc->data[tc->sp-1].tvar_id)].union_id = uid;
                    }
                }
            } else if (sym == S_UNTAG) {
                /* Stack shape: [tagged, default, clauses]. Tagged is at sp-3.
                   A default is required; missing tags fall through to it, so
                   static exhaustiveness is no longer a hard error (runtime is
                   already sound). */
                if (tc->sp >= 3 && tc->data[tc->sp-3].type == TC_TAGGED && tc->data[tc->sp-3].tvar_id > 0) {
                    int tid = tc->data[tc->sp-3].tvar_id;
                    int tp = tvar_content(tc, tid, TC_TAGGED);
                    if (tp > 0) { TypeConstraint pt = tvar_resolve(tc, tp);
                        if (pt == TC_BOX)
                            tc_error(tc, t->line, 0, "'untag' branches cannot safely discard linear payload; use a typed handler that consumes the box"); }
                }
                if (tc->sp >= 2 && ((tc->data[tc->sp-2].flags & AT_LINEAR) || tc->data[tc->sp-2].type == TC_BOX))
                    tc_error(tc, t->line, 0, "'untag' default must not be a linear value (would leak on any matched branch)");
                tc_check_word(tc, sym, t->line);
            } else if (sym == S_DEFAULT) {
                if (tc->sp >= 1 && ((tc->data[tc->sp-1].flags & AT_LINEAR) || tc->data[tc->sp-1].type == TC_BOX))
                    tc_error(tc, t->line, 0, "'default' fallback must not be a linear value (would leak on the 'ok path)");
                TypeConstraint pt = TC_NONE;
                if (tc->sp >= 2 && tc->data[tc->sp-2].type == TC_TAGGED && tc->data[tc->sp-2].tvar_id > 0) {
                    int tid = tc->data[tc->sp-2].tvar_id;
                    int tp = tvar_content(tc, tid, TC_TAGGED); if (tp > 0) pt = tvar_resolve(tc, tp);
                    if (pt == TC_NONE) {
                        int uid = tc->tvars[tvar_find(tc, tid)].union_id;
                        if (uid > 0) { UnionDef *ud = &tc->unions[uid-1];
                            for (int v = 0; v < ud->count; v++) if (ud->syms[v] == S_OK && ud->types[v] != TC_NONE) { pt = ud->types[v]; break; } }
                    }
                }
                TypeConstraint ft = (tc->sp >= 1) ? tc->data[tc->sp-1].type : TC_NONE;
                if (pt != TC_NONE && ft != TC_NONE && pt != ft && !tc_constraint_matches(pt, ft) && !tc_constraint_matches(ft, pt))
                    tc_error(tc, t->line, 0, "'default' fallback type %s doesn't match 'ok payload type %s", constraint_name(ft), constraint_name(pt));
                tc_check_word(tc, sym, t->line);
                if (pt != TC_NONE && tc->sp > 0 && tc->data[tc->sp-1].type == TC_NONE) tc->data[tc->sp-1].type = pt;
            } else tc_check_word(tc, sym, t->line);
            break;
        }
        default: break;
        }
        if (tc->body_depth == 0 && tc->effect_count > EFFECT_MAX/2) {
            int ml = 0;
            for (int s = 0; s < tc->sp; s++) if (tc->data[s].effect_idx >= ml) ml = tc->data[s].effect_idx + 1;
            for (int b = 0; b < tc->bind_count; b++) if (tc->bindings[b].atype.effect_idx >= ml) ml = tc->bindings[b].atype.effect_idx + 1;
            if (ml < tc->effect_count) tc->effect_count = ml;
        }
    }
}
static int typecheck_tokens(Token *toks, int count, int user_start) {
    TypeChecker tc; memset(&tc, 0, sizeof(tc)); tc.tvar_count = 1; tc.user_start = user_start;
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
#define POP_VAL(name) \
    Value name##_top = stack[sp-1]; int name##_s = val_slots(name##_top); \
    if (name##_s > LOCAL_MAX) die("value too large (%d slots, max %d)", name##_s, LOCAL_MAX); \
    if (name##_s > sp) die("stack underflow: need %d slots, have %d", name##_s, sp); \
    Value name##_buf[name##_s]; VCPY(name##_buf,&stack[sp-name##_s],name##_s); sp -= name##_s
#define POP_BODY(name, label) if (sp<=0) die(label ": stack underflow"); if (stack[sp-1].tag != VAL_TUPLE) die(label ": expected tuple, got %s", valtag_name(stack[sp-1].tag)); POP_VAL(name)
#define POP_LIST_BUF(name, label) \
    if (sp<=0) die(label ": stack underflow"); \
    if (stack[sp-1].tag != VAL_LIST) die(label ": expected list, got %s", valtag_name(stack[sp-1].tag)); \
    Value name##_top = stack[sp-1]; int name##_s = val_slots(name##_top); \
    if (name##_s > LOCAL_MAX) die(label ": list too large (%d slots, max %d)", name##_s, LOCAL_MAX); \
    if (name##_s > sp) die(label ": stack underflow: need %d slots, have %d", name##_s, sp); \
    int name##_len = (int)name##_top.as.compound.len; \
    int name##_base __attribute__((unused)) = sp - name##_s; \
    Value name##_buf[name##_s]; VCPY(name##_buf,&stack[name##_base],name##_s); sp = name##_base
static void deep_copy_values(Value *dst, const Value *src, int slots);
static void deep_free_values(Value *vals, int slots);
static void prim_dup(Frame *e) { (void)e; if (sp<=0) die("dup: stack underflow"); Value top=stack[sp-1]; if(top.tag<=VAL_XT){stack[sp++]=top;return;} int s=val_slots(top); if(sp+s>STACK_MAX) die("dup: stack overflow"); deep_copy_values(&stack[sp],&stack[sp-s],s); sp+=s; }
static void prim_drop(Frame *e) { (void)e; if (sp<=0) die("drop: stack underflow"); Value top=stack[sp-1]; if(top.tag<=VAL_XT){sp--;return;} int s=val_slots(top); deep_free_values(&stack[sp-s],s); sp-=s; }
static void prim_swap(Frame *e) {
    (void)e; if(sp<2)die("swap: stack underflow");
    Value top=stack[sp-1],below=stack[sp-2];
    if(top.tag<=VAL_XT&&below.tag<=VAL_XT){stack[sp-1]=below;stack[sp-2]=top;return;}
    int ts=val_slots(top),bp=sp-ts-1; if(bp<0)die("swap: stack underflow");
    int bs=val_slots(stack[bp]),total=ts+bs,base=sp-total;
    if(total>LOCAL_MAX)die("swap: value too large"); Value tmp[total];
    VCPY(tmp,&stack[base],bs); memmove(&stack[base],&stack[base+bs],ts*sizeof(Value)); VCPY(&stack[base+ts],tmp,bs);
}
static void prim_dip(Frame *env) {
    if(sp<2)die("dip: need body and value"); POP_BODY(body,"dip");
    POP_VAL(saved); eval_tuple_scoped(body_buf,body_s,env);
    SPUSH(saved_buf,saved_s);
}
static void prim_apply(Frame *env) { if(sp<=0) die("apply: stack underflow"); POP_BODY(body,"apply"); eval_tuple_scoped(body_buf,body_s,env); }
#define ARITH2(nm,iop,fop) static void prim_##nm(Frame *e){(void)e;Value b=spop(),a=spop(); \
    if(a.tag==VAL_INT&&b.tag==VAL_INT) spush(val_int(a.as.i iop b.as.i)); \
    else if(a.tag==VAL_FLOAT&&b.tag==VAL_FLOAT) spush(val_float(a.as.f fop b.as.f)); \
    else die(#nm ": type mismatch, got %s and %s", valtag_name(a.tag), valtag_name(b.tag));}
ARITH2(plus,+,+) ARITH2(sub,-,-) ARITH2(mul,*,*)
static void prim_div(Frame *e) { (void)e; Value b=spop(),a=spop();
    if(a.tag==VAL_INT&&b.tag==VAL_INT){if(b.as.i==0)die("div: division by zero");spush(val_int(a.as.i/b.as.i));}
    else if(a.tag==VAL_FLOAT&&b.tag==VAL_FLOAT)spush(val_float(a.as.f/b.as.f));
    else die("div: type mismatch, got %s and %s", valtag_name(a.tag), valtag_name(b.tag)); }
static void prim_mod(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();if(b==0)die("mod: division by zero");spush(val_int(a%b));}
static void prim_divmod(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();if(b==0)die("divmod: division by zero");spush(val_int(a%b));spush(val_int(a/b));}
static void prim_wrap(Frame *e){(void)e;int64_t m=pop_int(),v=pop_int();if(m==0)die("wrap: modulus must be non-zero");spush(val_int(((v%m)+m)%m));}
#define INTOP2(nm,expr) static void prim_##nm(Frame *e){(void)e;int64_t b=pop_int(),a=pop_int();spush(val_int(expr));}
INTOP2(band,a&b) INTOP2(bor,a|b) INTOP2(bxor,a^b) INTOP2(shl,(int64_t)((uint64_t)a<<b)) INTOP2(shr,(int64_t)((uint64_t)a>>b))
INTOP2(and,(a&&b)?1:0) INTOP2(or,(a||b)?1:0)
static void prim_bnot(Frame *e){(void)e;int64_t a=pop_int();spush(val_int(~a));}
#define CMP2(nm,expr) static void prim_##nm(Frame *e){(void)e;Value bt=stack[sp-1];int bs=val_slots(bt);int as=val_slots(stack[sp-1-bs]);int r=(expr);sp-=as+bs;spush(val_int(r?1:0));}
CMP2(eq, val_equal(&stack[sp-bs-as],as,&stack[sp-bs],bs))
CMP2(lt, val_less(&stack[sp-bs-as],as,&stack[sp-bs],bs))
static void prim_print(Frame *e){(void)e;if(sp<=0)die("print: stack underflow");Value top=stack[sp-1];int s=val_slots(top);val_print(&stack[sp-s],s,stdout);printf("\n");sp-=s;}
static void prim_assert(Frame *e){(void)e;if(!pop_int())die("assertion failed");}
static void prim_halt(Frame *e){(void)e;exit(0);}
static void prim_random(Frame *e){(void)e;int64_t max=pop_int();if(max<=0)max=1;spush(val_int(rand()%max));}
static void push_ok(void) { spush(val_compound(VAL_TAGGED,S_OK,val_slots(stack[sp-1])+1)); }
static void push_no(void) { spush(val_compound(VAL_TAGGED,S_NO,val_slots(stack[sp-1])+1)); }
static void push_none(void) { spush(val_compound(VAL_TUPLE,0,1)); spush(val_compound(VAL_TAGGED,S_NO,2)); }
static void prim_if(Frame *env) {
    POP_VAL(el); POP_BODY(then,"if");
    Value cond=spop(); if(cond.tag!=VAL_INT) die("if: condition must be int, got %s",valtag_name(cond.tag));
    if(cond.as.i) { if(then_s==(int)then_buf[then_s-1].as.compound.len+1) eval_body_fast(then_buf,then_s,env); else eval_body(then_buf,then_s,env); }
    else if(el_top.tag==VAL_TUPLE) { if(el_s==(int)el_buf[el_s-1].as.compound.len+1) eval_body_fast(el_buf,el_s,env); else eval_body(el_buf,el_s,env); }
    else { SPUSH(el_buf,el_s); }
}
#define POP_CLAUSES(label) \
    Value clauses_top=stack[sp-1]; \
    if(clauses_top.tag!=VAL_TUPLE&&clauses_top.tag!=VAL_RECORD) die(label ": expected tuple or record of clauses, got %s", valtag_name(clauses_top.tag)); \
    int clauses_s=val_slots(clauses_top),clauses_len=(int)clauses_top.as.compound.len; \
    if(clauses_s>LOCAL_MAX) die(label ": clauses too large (%d slots, max %d)",clauses_s,LOCAL_MAX); \
    Value clauses_buf[clauses_s]; VCPY(clauses_buf,&stack[sp-clauses_s],clauses_s); sp-=clauses_s
static int dispatch_clauses(const char *who, Value *cb, int cs, int cl, ValTag ct,
                            uint32_t ms, Value *pp, int pps, Frame *env);
/* Unified scrutinee/default/clauses dispatch.
   - Scrutinee TAGGED → match by tag symbol (payload pushed on match).
   - Scrutinee anything else → evaluate each clause predicate against a
     fresh copy of the scrutinee; on truthy, push scrutinee and run body.
   If nothing matches, the default is pushed.

   Exposed under three names: `cond`, `untag`, and `case`. The word kept
   in error messages is the symbolic one the caller used. */
static void prim_case(Frame *env) {
    POP_CLAUSES("case");
    POP_VAL(def);
    if (sp <= 0) die("case: stack underflow");
    Value top = stack[sp-1];
    if (top.tag == VAL_TAGGED) {
        int tagged_s=val_slots(top), payload_s=tagged_s-1;
        if(payload_s>LOCAL_MAX) die("case: payload too large (%d slots, max %d)",payload_s,LOCAL_MAX);
        Value payload_buf[payload_s]; VCPY(payload_buf,&stack[sp-tagged_s],payload_s);
        uint32_t tag_sym=top.as.compound.len;
        sp-=tagged_s;
        if(!dispatch_clauses("case",clauses_buf,clauses_s,clauses_len,clauses_top.tag,tag_sym,payload_buf,payload_s,env))
            SPUSH(def_buf,def_s);
        return;
    }
    int scrut_s=val_slots(top); Value scrut_buf[scrut_s]; VCPY(scrut_buf,&stack[sp-scrut_s],scrut_s); sp-=scrut_s;
    if(clauses_len%2!=0) die("case: need even number of clauses (pred/body pairs)");
    for(int i=0;i<clauses_len;i+=2){
        ElemRef pred_ref=compound_elem(clauses_buf,clauses_s,clauses_len,i);
        ElemRef body_ref=compound_elem(clauses_buf,clauses_s,clauses_len,i+1);
        SPUSH(scrut_buf,scrut_s);
        eval_body(&clauses_buf[pred_ref.base],pred_ref.slots,env);
        if(pop_int()){SPUSH(scrut_buf,scrut_s);eval_body(&clauses_buf[body_ref.base],body_ref.slots,env);return;}
    }
    SPUSH(def_buf,def_s);
}
static int dispatch_clauses(const char *who, Value *cb, int cs, int cl, ValTag ct,
                            uint32_t ms, Value *pp, int pps, Frame *env) {
#define DC_PUSH() if(pp){SPUSH(pp,pps);}
    if(ct==VAL_RECORD){ int found; ElemRef br=record_field(cb,cs,cl,ms,&found);
        if(found){DC_PUSH();eval_body(&cb[br.base],br.slots,env);return 1;}
    } else { if(cl%2!=0) die("%s: need even number of clauses", who);
        for(int i=0;i<cl;i+=2){ElemRef pr=compound_elem(cb,cs,cl,i);Value pat=cb[pr.base];
            if(pat.tag==VAL_SYM&&pat.as.sym==ms){ElemRef br=compound_elem(cb,cs,cl,i+1);DC_PUSH();eval_body(&cb[br.base],br.slots,env);return 1;}} }
#undef DC_PUSH
    return 0;
}
static void prim_tag(Frame *e) {
    (void)e;
    uint32_t tag_sym=pop_sym();
    if(sp<=0) die("tag: need a payload value");
    Value payload_top=stack[sp-1];
    int payload_s=val_slots(payload_top);
    spush(val_compound(VAL_TAGGED,tag_sym,payload_s+1));
}
#define LIST_ITER(label) POP_BODY(fn,label); POP_LIST_BUF(list,label); \
    int offs[LOCAL_MAX],szs[LOCAL_MAX]; compute_offsets(list_buf,list_s,list_len,offs,szs)
#define PUSH_ELEM(i) SPUSH(&list_buf[offs[i]],szs[i])
static void prim_then(Frame *env) {
    POP_BODY(body,"then");
    Value top=stack[sp-1];
    if(top.tag==VAL_TAGGED){
        if(top.as.compound.len==S_OK){
            sp--; /* remove header, payload stays */
            eval_body(body_buf,body_s,env);
        }
        /* non-ok: leave tagged value untouched */
    } else if(top.tag==VAL_LIST){
        POP_LIST_BUF(list,"then");
        int offs[LOCAL_MAX],szs[LOCAL_MAX]; compute_offsets(list_buf,list_s,list_len,offs,szs);
        int rb=sp,rc=0;
        for(int i=0;i<list_len;i++){
            PUSH_ELEM(i); eval_body(body_buf,body_s,env);
            Value r=stack[sp-1];
            if(r.tag!=VAL_LIST) die("then: body must return list for list bind, got %s",valtag_name(r.tag));
            rc+=(int)r.as.compound.len; sp--; /* remove list header, elements stay */
        }
        spush(val_compound(VAL_LIST,rc,sp-rb+1));
    } else {
        die("then: expected list or tagged, got %s", valtag_name(top.tag));
    }
}
static void prim_default(Frame *e) {
    (void)e; POP_VAL(def);
    Value top=stack[sp-1];
    if(top.tag!=VAL_TAGGED) die("default: expected tagged value, got %s", valtag_name(top.tag));
    int tagged_s=val_slots(top);
    if(top.as.compound.len==S_OK){
        sp--;
    } else {
        sp-=tagged_s;
        SPUSH(def_buf,def_s);
    }
}
static void prim_must(Frame *e) {
    (void)e; Value top=speek();
    if(top.tag!=VAL_TAGGED) die("must: expected tagged value, got %s", valtag_name(top.tag));
    if(top.as.compound.len==S_OK) { sp--; return; }
    int ps=(int)top.as.compound.slots-1;
    fprintf(stderr,"\n-- MUST FAILED ----------------------------\n\n    expected 'ok tagged, got '%s tagged\n",sym_name(top.as.compound.len));
    if(ps>0){fprintf(stderr,"    payload: ");val_print(&stack[sp-ps-1],ps,stderr);fprintf(stderr,"\n");}
    fprintf(stderr,"\n");exit(1);
}
static void prim_pthen(Frame *env) {
    /* pthen: tagged default (body) pthen
       If tagged is ok: pop default, unwrap ok, run body
       If tagged is not ok: push default below the tagged value */
    POP_BODY(body,"pthen");
    Value def=spop();
    Value top=stack[sp-1];
    if(top.tag!=VAL_TAGGED) die("pthen: expected tagged, got %s",valtag_name(top.tag));
    if(top.as.compound.len==S_OK){
        sp--; /* remove ok header, payload stays */
        eval_body(body_buf,body_s,env);
    } else {
        /* push default below the tagged value */
        int ts=val_slots(top);
        Value tbuf[ts]; VCPY(tbuf,&stack[sp-ts],ts); sp-=ts;
        spush(def);
        SPUSH(tbuf,ts);
    }
}
static void prim_fused_inc(Frame *e) {
    (void)e; Value *v=&stack[sp-1];
    if(v->tag==VAL_INT) v->as.i++;
    else if(v->tag==VAL_FLOAT) v->as.f+=1.0;
    else die("plus: type mismatch, got %s and int",valtag_name(v->tag));
}
static void prim_fused_dec(Frame *e) {
    (void)e; Value *v=&stack[sp-1];
    if(v->tag==VAL_INT) v->as.i--;
    else if(v->tag==VAL_FLOAT) v->as.f-=1.0;
    else die("sub: type mismatch, got %s and int",valtag_name(v->tag));
}
static void prim_fused_iszero(Frame *e) {
    (void)e; Value v=stack[sp-1];
    if(v.tag==VAL_INT) stack[sp-1]=val_int(v.as.i==0?1:0);
    else if(v.tag==VAL_FLOAT) stack[sp-1]=val_int(v.as.f==0.0?1:0);
    else die("eq: type mismatch");
}
static void prim_fused_add2(Frame *e) { (void)e; Value *v=&stack[sp-1]; if(v->tag==VAL_INT) v->as.i+=2; else if(v->tag==VAL_FLOAT) v->as.f+=2.0; else die("plus: type mismatch"); }
static void prim_fused_sub2(Frame *e) { (void)e; Value *v=&stack[sp-1]; if(v->tag==VAL_INT) v->as.i-=2; else if(v->tag==VAL_FLOAT) v->as.f-=2.0; else die("sub: type mismatch"); }
static void prim_fused_mul2(Frame *e) { (void)e; Value *v=&stack[sp-1]; if(v->tag==VAL_INT) v->as.i*=2; else if(v->tag==VAL_FLOAT) v->as.f*=2.0; else die("mul: type mismatch"); }
static void prim_fused_div2(Frame *e) { (void)e; Value *v=&stack[sp-1]; if(v->tag==VAL_INT) v->as.i/=2; else if(v->tag==VAL_FLOAT) v->as.f/=2.0; else die("div: type mismatch"); }
static void prim_fused_mod2(Frame *e) { (void)e; Value *v=&stack[sp-1]; if(v->tag!=VAL_INT) die("mod: expected int"); v->as.i=v->as.i%2; }
static void prim_fused_add6(Frame *e) { (void)e; Value *v=&stack[sp-1]; if(v->tag==VAL_INT) v->as.i+=6; else if(v->tag==VAL_FLOAT) v->as.f+=6.0; else die("plus: type mismatch"); }
static void prim_fused_mul10(Frame *e) { (void)e; Value *v=&stack[sp-1]; if(v->tag==VAL_INT) v->as.i*=10; else if(v->tag==VAL_FLOAT) v->as.f*=10.0; else die("mul: type mismatch"); }
static void prim_fused_div10(Frame *e) { (void)e; Value *v=&stack[sp-1]; if(v->tag==VAL_INT) v->as.i/=10; else if(v->tag==VAL_FLOAT) v->as.f/=10.0; else die("div: type mismatch"); }
static void prim_fused_nip(Frame *e) {
    (void)e; if(sp<2) die("nip: stack underflow");
    Value top=stack[sp-1]; int ts=val_slots(top);
    int below_top=sp-ts-1; if(below_top<0) die("nip: stack underflow");
    int bs=val_slots(stack[below_top]);
    Value tmp[ts]; VCPY(tmp,&stack[sp-ts],ts);
    sp-=ts+bs; SPUSH(tmp,ts);
}
static void prim_union(Frame *e) {
    (void)e;
    Value top=stack[sp-1];
    if(!is_compound(top.tag)) die("union: expected record describing tag schema, got %s", valtag_name(top.tag));
    sp-=val_slots(top);
}
static void prim_loop(Frame *env) {
    if(sp<=0) die("loop: stack underflow (need a body tuple)");
    if(stack[sp-1].tag!=VAL_TUPLE) die("loop: expected tuple, got %s", valtag_name(stack[sp-1].tag));
    int body_s=val_slots(stack[sp-1]);
    if(body_s>LOCAL_MAX) die("loop: body too large (%d slots, max %d)", body_s, LOCAL_MAX);
    if(body_s>sp) die("loop: stack underflow (need %d slots, have %d)", body_s, sp);
    Value body_buf[body_s]; VCPY(body_buf,&stack[sp-body_s],body_s); sp-=body_s;
    for(;;){eval_body(body_buf,body_s,env);if(!pop_int())break;}
}
static inline void eval_body_fast(Value *body, int slots, Frame *env) {
    Value hdr=body[slots-1]; Frame *ee=hdr.as.compound.env?hdr.as.compound.env:env;
    int len=(int)hdr.as.compound.len, sbc=ee->bind_count, svu=ee->vals_used;
    eval_depth++;
    for(int k=0;k<len;k++){
        Value elem=body[k];
        if(elem.tag<=VAL_SYM) stack[sp++]=elem;
        else if(elem.tag==VAL_XT) elem.as.xt.fn(ee);
        else if(elem.tag==VAL_WORD){
            uint32_t sym=elem.as.sym;
            if(sym==S_DEF){ int ds=val_slots(stack[sp-1]); Value db[ds]; VCPY(db,&stack[sp-ds],ds); sp-=ds; frame_bind(ee,pop_sym(),db,ds,BIND_DEF,0); }
            else if(sym==S_LET){ uint32_t n=pop_sym(); int ls=val_slots(stack[sp-1]); frame_bind(ee,n,&stack[sp-ls],ls,BIND_LET,0); sp-=ls; }
            else dispatch_word(sym,ee);
        } else if(is_compound(elem.tag)){
            SPUSH(&body[k-((int)elem.as.compound.slots-1)],val_slots(elem));
            if(elem.tag==VAL_TUPLE){ee->refcount++;stack[sp-1].as.compound.env=ee;}
        } else if(elem.tag==VAL_BOX||elem.tag==VAL_DICT) spush(elem);
    }
    if(ee->refcount==0){ee->bind_count=sbc;ee->vals_used=svu;}
    eval_depth--;
}
static void prim_while(Frame *env) {
    POP_BODY(body,"while"); POP_BODY(pred,"while");
    int pred_asc=(pred_s==((int)pred_buf[pred_s-1].as.compound.len)+1);
    int body_asc=(body_s==((int)body_buf[body_s-1].as.compound.len)+1);
    if(pred_asc && body_asc) {
        for(;;){eval_body_fast(pred_buf,pred_s,env);if(!pop_int())break;eval_body_fast(body_buf,body_s,env);}
    } else {
        for(;;){eval_body(pred_buf,pred_s,env);if(!pop_int())break;eval_body(body_buf,body_s,env);}
    }
}
static void prim_itof(Frame *e){(void)e;spush(val_float((double)pop_int()));}
static void prim_ftoi(Frame *e){(void)e;spush(val_int((int64_t)pop_float()));}
#define FLOAT1(nm,fn) static void prim_##nm(Frame *e){(void)e;spush(val_float(fn(pop_float())));}
FLOAT1(fsqrt,sqrt) FLOAT1(fsin,sin) FLOAT1(fcos,cos) FLOAT1(ftan,tan)
FLOAT1(ffloor,floor) FLOAT1(fceil,ceil) FLOAT1(fround,round) FLOAT1(fexp,exp) FLOAT1(flog,log)
#define FLOAT2(nm,fn) static void prim_##nm(Frame *e){(void)e;double b=pop_float(),a=pop_float();spush(val_float(fn(a,b)));}
FLOAT2(fpow,pow) FLOAT2(fatan2,atan2)
static void prim_stack(Frame *e){(void)e;spush(val_compound(VAL_TUPLE,0,1));}
static void dict_data_free(DictData *dd);
static void prim_size(Frame *e) {
    (void)e; Value top=speek();
    if(top.tag==VAL_DICT){ int n=((DictData*)top.as.box)->len; dict_data_free((DictData*)top.as.box); sp--; spush(val_int(n)); return; }
    if(top.tag==VAL_TAGGED) die("len: tagged values have no length (untag first)");
    if(!is_compound(top.tag)) die("len: expected compound, got %s",valtag_name(top.tag));
    sp-=val_slots(top); spush(val_int((int)top.as.compound.len));
}
static void prim_push_op(Frame *e) {
    (void)e; POP_VAL(v); Value ct=stack[sp-1]; if(ct.tag==VAL_TAGGED) die("push: cannot push onto tagged value");
    if(!is_compound(ct.tag)) die("push: expected compound, got %s",valtag_name(ct.tag));
    ValTag tag=ct.tag; int cs=val_slots(ct),cl=(int)ct.as.compound.len; sp--;
    SPUSH(v_buf,v_s); spush(val_compound(tag,cl+1,cs+v_s));
}
static inline void prim_pop_impl(Frame *e, int tagged) {
    (void)e; Value top=speek(); if(top.tag==VAL_TAGGED) die("pop: cannot pop from tagged value");
    if(!is_compound(top.tag)) die("pop: expected compound, got %s",valtag_name(top.tag));
    ValTag tag=top.tag; int s=val_slots(top),len=(int)top.as.compound.len;
    if(len==0) { if(tagged) push_none(); else die("pop: empty %s",tag==VAL_LIST?"list":tag==VAL_TUPLE?"tuple":"record"); return; }
    int base=sp-s; ElemRef last=compound_elem(&stack[base],s,len,len-1);
    Value eb[LOCAL_MAX]; VCPY(eb,&stack[base+last.base],last.slots);
    sp--; sp-=last.slots; spush(val_compound(tag,len-1,s-last.slots));
    SPUSH(eb,last.slots); if(tagged) push_ok();
}
static void prim_pop_op(Frame *e) { prim_pop_impl(e,1); }
static void prim_pop_must(Frame *e) { prim_pop_impl(e,0); }
static inline void prim_get_impl(Frame *e, int tagged) {
    (void)e; int64_t idx=pop_int(); Value top=speek();
    if(top.tag==VAL_TAGGED) die("get: cannot index tagged value");
    if(!is_compound(top.tag)) die("get: expected compound, got %s", valtag_name(top.tag));
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    ElemRef ref=compound_elem(&stack[base],s,len,(int)idx);
    if(ref.base<0) { sp-=s; if(tagged) push_none(); else die("get: index %lld out of bounds (len %d)",(long long)idx,len); return; }
    Value eb[LOCAL_MAX]; VCPY(eb,&stack[base+ref.base],ref.slots);
    sp-=s; SPUSH(eb,ref.slots); if(tagged) push_ok();
}
static void prim_get(Frame *e) { prim_get_impl(e,1); }
static void prim_get_must(Frame *e) { prim_get_impl(e,0); }
static inline void prim_set_impl(Frame *e, int tagged) {
    (void)e; POP_VAL(v); int64_t idx=pop_int(); Value top=speek();
    if(top.tag==VAL_TAGGED) die("set: cannot index tagged value");
    if(!is_compound(top.tag)) die("set: expected compound, got %s", valtag_name(top.tag));
    ValTag tag=top.tag; int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    ElemRef old_ref=compound_elem(&stack[base],s,len,(int)idx);
    if(old_ref.base<0) { if(tagged) push_none(); else die("set: index %lld out of bounds (len %d)",(long long)idx,len); return; }
    if(old_ref.slots==v_s){VCPY(&stack[base+old_ref.base],v_buf,v_s);if(tagged)push_ok();return;}
    Value tmp[LOCAL_MAX]; int ts=0;
    for(int i=0;i<len;i++){ElemRef r=compound_elem(&stack[base],s,len,i);Value *src=i==(int)idx?v_buf:&stack[base+r.base];int sz=i==(int)idx?v_s:r.slots;VCPY(&tmp[ts],src,sz);ts+=sz;}
    sp=base; SPUSH(tmp,ts); spush(val_compound(tag,len,ts+1)); if(tagged) push_ok();
}
static void prim_replace_at(Frame *e) { prim_set_impl(e,1); }
static void prim_set_must(Frame *e) { prim_set_impl(e,0); }
static void prim_concat(Frame *e) {
    (void)e; Value t2=stack[sp-1]; if(t2.tag==VAL_TAGGED)die("concat: cannot concat tagged values");
    if(!is_compound(t2.tag))die("concat: expected compound, got %s",valtag_name(t2.tag));
    ValTag tag=t2.tag; int s2=val_slots(t2),l2=(int)t2.as.compound.len,b2=sp-s2;
    Value bel=stack[b2-1]; if(!is_compound(bel.tag))die("concat: expected compound below top, got %s",valtag_name(bel.tag));
    int s1=val_slots(bel),l1=(int)bel.as.compound.len,b1=b2-s1,ns=(s1-1)+(s2-1); if(ns>LOCAL_MAX)die("concat: result too large");
    Value tmp[ns]; memcpy(tmp,&stack[b1],(s1-1)*sizeof(Value)); memcpy(&tmp[s1-1],&stack[b2],(s2-1)*sizeof(Value));
    sp=b1; SPUSH(tmp,ns); spush(val_compound(tag,l1+l2,ns+1));
}
static inline void prim_nth_impl(Frame *env, int tagged) {
    int64_t idx=pop_int(); uint32_t sym=pop_sym();
    Lookup lu=frame_lookup(env,sym); if(!lu.bind) die("nth: unknown word: %s",sym_name(sym));
    Value *data=&lu.frame->vals[lu.bind->offset]; int s=lu.bind->slots;
    Value top=data[s-1]; if(!is_compound(top.tag)) die("nth: expected compound (tuple/list/record) bound to '%s, got %s", sym_name(sym), valtag_name(top.tag));
    int len=(int)top.as.compound.len;
    ElemRef ref=compound_elem(data,s,len,(int)idx);
    if(ref.base<0) { if(tagged) push_none(); else die("nth: index %lld out of bounds (len %d)",(long long)idx,len); return; }
    SPUSH(&data[ref.base],ref.slots); if(tagged) push_ok();
}
static void prim_nth(Frame *e) { prim_nth_impl(e,1); }
static void prim_nth_must(Frame *e) { prim_nth_impl(e,0); }
static void prim_slice_n(int take) {
    int64_t n=pop_int(); Value top=speek();
    const char *label=take?"take-n":"drop-n";
    if(top.tag!=VAL_LIST) die("%s: expected list, got %s", label, valtag_name(top.tag));
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    if(n<0) n=0; if(n>len) n=len;
    Value tmp[LOCAL_MAX]; int tmp_sp=0;
    int start=take?0:(int)n, end_i=take?(int)n:len;
    for(int i=start;i<end_i;i++){ElemRef r=compound_elem(&stack[base],s,len,i);VCPY(&tmp[tmp_sp],&stack[base+r.base],r.slots);tmp_sp+=r.slots;}
    sp=base; SPUSH(tmp,tmp_sp);
    spush(val_compound(VAL_LIST,end_i-start,tmp_sp+1));
}
static void prim_range(Frame *e){(void)e;int64_t end=pop_int(),start=pop_int();int count=0;for(int64_t i=start;i<end;i++){spush(val_int(i));count++;}spush(val_compound(VAL_LIST,count,count+1));}
static void deep_copy_values(Value *dst, const Value *src, int slots);
static void push_string_bytes(const char *buf, int len);
static void dict_put(DictData *dd, const char *key, int klen, Value *vals, int nvals);
static Value dict_val(DictData *dd);
static void dict_data_free(DictData *dd);
static void dict_push_kv_tuple(DictEntry *e) {
    int key_s = e->klen + 1;
    push_string_bytes(e->key, e->klen);
    Value vcopy[e->nvals]; deep_copy_values(vcopy, e->vals, e->nvals); SPUSH(vcopy, e->nvals);
    spush(val_compound(VAL_TUPLE, 2, key_s + e->nvals + 1));
}
static void prim_each(Frame *env) {
    POP_BODY(fn,"each");
    Value top=stack[sp-1];
    if(top.tag==VAL_DICT){
        Value dv=spop(); DictData *dd=(DictData*)dv.as.box;
        DictData *nd=calloc(1,sizeof(DictData));
        for(int i=0;i<dd->cap;i++){DictEntry *e=&dd->entries[i]; if(!e->key) continue;
            dict_push_kv_tuple(e);
            eval_body(fn_buf,fn_s,env);
            Value nt=stack[sp-1]; int ns=val_slots(nt);
            dict_put(nd,e->key,e->klen,&stack[sp-ns],ns); sp-=ns;
        }
        dict_data_free(dd); spush(dict_val(nd)); return;
    }
    if(top.tag==VAL_LIST){
        POP_LIST_BUF(list,"each");
        int offs[LOCAL_MAX],szs[LOCAL_MAX]; compute_offsets(list_buf,list_s,list_len,offs,szs);
        int rb=sp;
        for(int i=0;i<list_len;i++){PUSH_ELEM(i);eval_body(fn_buf,fn_s,env);}
        spush(val_compound(VAL_LIST,list_len,sp-rb+1));
    } else if(top.tag==VAL_TAGGED){
        if(top.as.compound.len==S_OK){
            sp--; /* remove tagged header, payload stays */
            eval_body(fn_buf,fn_s,env);
            Value new_top=stack[sp-1];
            int new_payload_s=val_slots(new_top);
            spush(val_compound(VAL_TAGGED,S_OK,new_payload_s+1));
        }
        /* non-ok: leave tagged value untouched */
    } else {
        die("each: expected list or tagged, got %s", valtag_name(top.tag));
    }
}
static void prim_fold(Frame *env) {
    POP_BODY(fn,"fold"); POP_VAL(init);
    Value top=speek();
    if(top.tag==VAL_DICT){
        Value dv=spop(); DictData *dd=(DictData*)dv.as.box;
        SPUSH(init_buf,init_s);
        for(int i=0;i<dd->cap;i++){DictEntry *e=&dd->entries[i]; if(!e->key) continue;
            dict_push_kv_tuple(e); eval_body(fn_buf,fn_s,env);
        }
        dict_data_free(dd); return;
    }
    POP_LIST_BUF(list,"fold");
    int offs[LOCAL_MAX],szs[LOCAL_MAX]; compute_offsets(list_buf,list_s,list_len,offs,szs);
    SPUSH(init_buf,init_s);
    for(int i=0;i<list_len;i++){PUSH_ELEM(i);eval_body(fn_buf,fn_s,env);}
}
static int val_cmp(const Value *va, const Value *vb) {
    if(va->tag==VAL_INT&&vb->tag==VAL_INT) return(va->as.i>vb->as.i)-(va->as.i<vb->as.i);
    if(va->tag==VAL_FLOAT&&vb->tag==VAL_FLOAT) return(va->as.f>vb->as.f)-(va->as.f<vb->as.f);
    if(va->tag==VAL_SYM&&vb->tag==VAL_SYM) return strcmp(sym_name(va->as.sym),sym_name(vb->as.sym));
    die("sort: mismatched or unsupported element types (got %s and %s)", valtag_name(va->tag), valtag_name(vb->tag)); return 0;
}
static int sort_cmp(const void *a,const void *b) { return val_cmp((const Value*)a,(const Value*)b); }
static void prim_sort(Frame *e){
    (void)e; Value top=speek(); if(top.tag!=VAL_LIST) die("sort: expected list, got %s",valtag_name(top.tag));
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    if(s!=len+1){ValTag bad=VAL_INT;for(int i=0;i<len;i++){ElemRef r=compound_elem(&stack[base],s,len,i);if(r.slots>1){bad=stack[base+r.base+r.slots-1].tag;break;}}
        die("sort: elements must be orderable (int, float, or symbol), got %s",valtag_name(bad));}
    qsort(&stack[base],len,sizeof(Value),sort_cmp);
}
static inline void prim_indexof_impl(Frame *e, int tagged) {
    (void)e; POP_VAL(val); Value top=speek(); if(top.tag!=VAL_LIST) die("index-of: expected list, got %s",valtag_name(top.tag));
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s,r=-1;
    for(int i=0;i<len&&r<0;i++){ElemRef ref=compound_elem(&stack[base],s,len,i);if(val_equal(&stack[base+ref.base],ref.slots,val_buf,val_s))r=i;}
    sp-=s; if(r<0) { if(tagged) push_none(); else die("index-of: element not found"); }
    else { spush(val_int(r)); if(tagged) push_ok(); }
}
static void prim_index_of(Frame *e) { prim_indexof_impl(e,1); }
static void prim_indexof_must(Frame *e) { prim_indexof_impl(e,0); }
static void prim_scan(Frame *env) {
    POP_BODY(fn,"scan"); POP_VAL(init); POP_LIST_BUF(list,"scan");
    int offs[LOCAL_MAX],szs[LOCAL_MAX]; compute_offsets(list_buf,list_s,list_len,offs,szs);
    Value acc[LOCAL_MAX]; int as=init_s; VCPY(acc,init_buf,init_s);
    int rb=sp;
    for(int i=0;i<list_len;i++){SPUSH(acc,as);PUSH_ELEM(i);eval_body(fn_buf,fn_s,env);as=val_slots(stack[sp-1]);VCPY(acc,&stack[sp-as],as);}
    spush(val_compound(VAL_LIST,list_len,sp-rb+1));
}
static inline void prim_at_impl(Frame *env, int tagged) {
    (void)env; uint32_t key=pop_sym();
    if(sp<=0) die("at: stack underflow"); Value next=stack[sp-1];
    if(next.tag!=VAL_RECORD) die("at: expected record, got %s",valtag_name(next.tag));
    int s=val_slots(next),len=(int)next.as.compound.len,base=sp-s;
    int found; ElemRef ref=record_field(&stack[base],s,len,key,&found);
    if(!found) { sp-=s; if(tagged) push_none(); else die("at: key '%s' not found in record",sym_name(key)); return; }
    Value vb[LOCAL_MAX]; VCPY(vb,&stack[base+ref.base],ref.slots);
    sp-=s; SPUSH(vb,ref.slots); if(tagged) push_ok();
}
static void prim_at(Frame *e) { prim_at_impl(e,1); }
static void prim_at_must(Frame *e) { prim_at_impl(e,0); }
static void rec_set_field(int rb, int rs, int rl, uint32_t key, Value *nv, int ns, int add) {
    Value tmp[LOCAL_MAX]; int ts=0, replaced=0;
    int kp[LOCAL_MAX],vo[LOCAL_MAX],vs[LOCAL_MAX]; record_offsets(&stack[rb],rs,rl,kp,vo,vs);
    for(int i=0;i<rl;i++){uint32_t k=stack[rb+kp[i]].as.sym;tmp[ts++]=val_sym(k);
        if(k==key){VCPY(&tmp[ts],nv,ns);ts+=ns;replaced=1;}
        else{VCPY(&tmp[ts],&stack[rb+vo[i]],vs[i]);ts+=vs[i];}}
    if(add&&!replaced){tmp[ts++]=val_sym(key);VCPY(&tmp[ts],nv,ns);ts+=ns;}
    sp=rb; SPUSH(tmp,ts); spush(val_compound(VAL_RECORD,rl+(add&&!replaced),ts+1));
}
#define REC_PREAMBLE(who) Value rec_top=stack[sp-1];if(rec_top.tag!=VAL_RECORD)die(who ": expected record, got %s",valtag_name(rec_top.tag));int rec_s=val_slots(rec_top),rec_len=(int)rec_top.as.compound.len,rec_base=sp-rec_s
static void prim_into(Frame *e) {
    (void)e; uint32_t key=pop_sym(); POP_VAL(v); REC_PREAMBLE("into");
    int found; ElemRef existing=record_field(&stack[rec_base],rec_s,rec_len,key,&found);
    if(found&&existing.slots==v_s) VCPY(&stack[rec_base+existing.base],v_buf,v_s);
    else rec_set_field(rec_base,rec_s,rec_len,key,v_buf,v_s,1);
}
static inline void prim_edit_impl(Frame *env, int tagged) {
    POP_BODY(fn,"edit"); uint32_t key=pop_sym(); REC_PREAMBLE("edit");
    int found; ElemRef ref=record_field(&stack[rec_base],rec_s,rec_len,key,&found);
    if(!found) { if(tagged) push_none(); else die("edit: key '%s' not found in record",sym_name(key)); return; }
    SPUSH(&stack[rec_base+ref.base],ref.slots);
    eval_body(fn_buf,fn_s,env);
    int ns=val_slots(stack[sp-1]);
    if(ns==ref.slots){VCPY(&stack[rec_base+ref.base],&stack[sp-ns],ns);sp-=ns;}
    else{Value nv[LOCAL_MAX];VCPY(nv,&stack[sp-ns],ns);sp-=ns;rec_set_field(rec_base,rec_s,rec_len,key,nv,ns,0);}
    if(tagged) push_ok();
}
static void prim_edit(Frame *e) { prim_edit_impl(e,1); }
static void prim_edit_must(Frame *e) { prim_edit_impl(e,0); }
typedef struct BoxData { Value *data; int slots; } BoxData;
static void prim_box(Frame *e){(void)e;Value top=stack[sp-1];int s=val_slots(top);BoxData *bd=malloc(sizeof(BoxData));bd->data=malloc(s*sizeof(Value));bd->slots=s;deep_copy_values(bd->data,&stack[sp-s],s);sp-=s;Value v;v.tag=VAL_BOX;v.loc=0;v.as.box=bd;spush(v);}
static void dict_data_free(DictData *dd);
static void prim_free(Frame *e){
    (void)e;Value v=spop();
    if(v.tag==VAL_BOX){BoxData *bd=(BoxData*)v.as.box;if(!bd->data)die("free: double-free detected (box already freed, likely captured by a closure that ran twice)");free(bd->data);bd->data=NULL;bd->slots=-1;return;}
    if(v.tag==VAL_DICT){dict_data_free((DictData*)v.as.box);return;}
    die("free: expected box or dict, got %s", valtag_name(v.tag));
}
#define BOX_UNPACK(who) POP_BODY(fn,who); Value box_val=spop(); if(box_val.tag!=VAL_BOX) die(who ": expected box, got %s", valtag_name(box_val.tag)); BoxData *bd=(BoxData*)box_val.as.box; SPUSH(bd->data,bd->slots)
static void prim_lend(Frame *env) {
    BOX_UNPACK("lend"); int sp_before=sp-bd->slots; eval_body(fn_buf,fn_s,env);
    int rs=sp-sp_before; Value results[rs]; VCPY(results,&stack[sp_before],rs); sp=sp_before;
    spush(box_val); SPUSH(results,rs);
}
static void prim_mutate(Frame *env) {
    BOX_UNPACK("mutate"); eval_body(fn_buf,fn_s,env);
    Value new_top=stack[sp-1]; int ns=val_slots(new_top);
    free(bd->data); bd->data=malloc(ns*sizeof(Value)); bd->slots=ns;
    VCPY(bd->data,&stack[sp-ns],ns); sp-=ns; spush(box_val);
}
static DictData *dict_clone(DictData *orig);
static void dict_data_free(DictData *dd);
static void deep_copy_values(Value *dst, const Value *src, int slots) {
    VCPY(dst,src,slots);
    for(int i=0;i<slots;i++){
        if(dst[i].tag==VAL_BOX){
            BoxData *o=(BoxData*)dst[i].as.box; BoxData *c=malloc(sizeof(BoxData));
            c->slots=o->slots; c->data=malloc(o->slots*sizeof(Value));
            deep_copy_values(c->data,o->data,o->slots); dst[i].as.box=c;
        } else if(dst[i].tag==VAL_DICT){
            dst[i].as.box=dict_clone((DictData*)dst[i].as.box);
        }
    }
}
static void deep_free_values(Value *vals, int slots) {
    for(int i=0;i<slots;i++){
        if(vals[i].tag==VAL_DICT) dict_data_free((DictData*)vals[i].as.box);
        /* boxes are linear and rejected at TC time; not freed here */
    }
}
static BoxData *box_clone(BoxData *orig) { BoxData *c=malloc(sizeof(BoxData));c->data=malloc(orig->slots*sizeof(Value));c->slots=orig->slots;deep_copy_values(c->data,orig->data,orig->slots);return c; }
static DictData *dict_clone(DictData *orig);
static Value dict_val(DictData *dd);
static void prim_clone(Frame *e){
    (void)e;Value v=spop();
    if(v.tag==VAL_BOX){BoxData *c=box_clone((BoxData*)v.as.box);spush(v);Value v2={0};v2.tag=VAL_BOX;v2.as.box=c;spush(v2);return;}
    if(v.tag==VAL_DICT){DictData *c=dict_clone((DictData*)v.as.box);spush(v);spush(dict_val(c));return;}
    die("clone: expected box or dict, got %s",valtag_name(v.tag));
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
static void dict_put(DictData *dd, const char *key, int klen, Value *vals, int nvals) {
    if(dd->cap==0 || (dd->len+1)*10 >= dd->cap*7) dict_grow(dd);
    int i=dict_probe(dd,key,klen); if(i<0) die("dict: probe failed (internal)");
    DictEntry *e=&dd->entries[i];
    if(e->key){
        /* existing: replace value */
        free(e->vals);
        e->vals=malloc(nvals*sizeof(Value)); e->nvals=nvals;
        deep_copy_values(e->vals,vals,nvals);
    } else {
        e->key=malloc(klen?klen:1); if(klen) memcpy(e->key,key,klen); e->klen=klen;
        e->vals=malloc(nvals*sizeof(Value)); e->nvals=nvals;
        deep_copy_values(e->vals,vals,nvals);
        dd->len++;
    }
}
static DictEntry *dict_get(DictData *dd, const char *key, int klen) {
    int i=dict_probe(dd,key,klen); if(i<0) return NULL;
    return dd->entries[i].key ? &dd->entries[i] : NULL;
}
static void dict_free_entry_contents(DictEntry *e) {
    if(!e->key) return;
    for(int i=0;i<e->nvals;i++) if(e->vals[i].tag==VAL_BOX){BoxData *bd=(BoxData*)e->vals[i].as.box;free(bd->data);free(bd);}
        else if(e->vals[i].tag==VAL_DICT){DictData *sub=(DictData*)e->vals[i].as.box;for(int j=0;j<sub->cap;j++)dict_free_entry_contents(&sub->entries[j]);free(sub->entries);free(sub);}
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
static DictData *dict_clone(DictData *orig) {
    DictData *c=calloc(1,sizeof(DictData));
    c->cap=orig->cap; c->len=0;
    if(orig->cap){
        c->entries=calloc(orig->cap,sizeof(DictEntry));
        for(int i=0;i<orig->cap;i++){
            DictEntry *e=&orig->entries[i]; if(!e->key) continue;
            dict_put(c,e->key,e->klen,e->vals,e->nvals);
        }
    }
    return c;
}
static int pop_string_bytes(const char *who, char **out, int *out_len) {
    Value top=stack[sp-1];
    if(top.tag!=VAL_LIST) die("%s: expected string (list of int), got %s", who, valtag_name(top.tag));
    int s=val_slots(top), len=(int)top.as.compound.len, base=sp-s;
    if(s != len+1) die("%s: key must be a simple string (list of int)", who);
    char *buf=malloc(len?len:1);
    for(int i=0;i<len;i++){
        if(stack[base+i].tag!=VAL_INT) die("%s: key string contains non-int at position %d", who, i);
        buf[i]=(char)stack[base+i].as.i;
    }
    sp=base; *out=buf; *out_len=len; return len;
}
static void push_string_bytes(const char *buf, int len) {
    for(int i=0;i<len;i++) spush(val_int((unsigned char)buf[i]));
    spush(val_compound(VAL_LIST,len,len+1));
}
static Value dict_val(DictData *dd){Value v={0};v.tag=VAL_DICT;v.loc=0;v.as.box=dd;return v;}
static void prim_dict(Frame *e){(void)e;DictData *dd=calloc(1,sizeof(DictData));spush(dict_val(dd));}
static void prim_insert(Frame *e) {
    (void)e; POP_VAL(val); char *key; int klen; pop_string_bytes("insert",&key,&klen);
    Value dv=stack[sp-1]; if(dv.tag!=VAL_DICT) die("insert: expected dict, got %s", valtag_name(dv.tag));
    DictData *dd=(DictData*)dv.as.box;
    dict_put(dd,key,klen,val_buf,val_s);
    free(key);
}
static void prim_of(Frame *e) {
    (void)e; char *key; int klen; pop_string_bytes("of",&key,&klen);
    Value dv=stack[sp-1]; if(dv.tag!=VAL_DICT) die("of: expected dict, got %s", valtag_name(dv.tag));
    DictData *dd=(DictData*)dv.as.box;
    DictEntry *ent=dict_get(dd,key,klen); free(key);
    if(!ent){ push_none(); return; }
    SPUSH(ent->vals,ent->nvals); push_ok();
}
static void prim_remove(Frame *e) {
    (void)e; char *key; int klen; pop_string_bytes("remove",&key,&klen);
    Value dv=stack[sp-1]; if(dv.tag!=VAL_DICT) die("remove: expected dict, got %s", valtag_name(dv.tag));
    DictData *dd=(DictData*)dv.as.box;
    dict_del(dd,key,klen); free(key);
}
static void prim_keys(Frame *e) {
    (void)e; Value dv=stack[sp-1]; if(dv.tag!=VAL_DICT) die("dict-keys: expected dict, got %s", valtag_name(dv.tag));
    DictData *dd=(DictData*)dv.as.box;
    int rb=sp, count=0;
    for(int i=0;i<dd->cap;i++){DictEntry *ent=&dd->entries[i]; if(!ent->key) continue;
        push_string_bytes(ent->key,ent->klen); count++;}
    spush(val_compound(VAL_LIST,count,sp-rb+1));
}
static void prim_values(Frame *e) {
    (void)e; Value dv=stack[sp-1]; if(dv.tag!=VAL_DICT) die("dict-values: expected dict, got %s", valtag_name(dv.tag));
    DictData *dd=(DictData*)dv.as.box;
    int rb=sp, count=0;
    for(int i=0;i<dd->cap;i++){DictEntry *ent=&dd->entries[i]; if(!ent->key) continue;
        Value copy[ent->nvals]; deep_copy_values(copy,ent->vals,ent->nvals);
        SPUSH(copy,ent->nvals); count++;}
    spush(val_compound(VAL_LIST,count,sp-rb+1));
}
static void dict_data_free(DictData *dd) {
    for(int i=0;i<dd->cap;i++) dict_free_entry_contents(&dd->entries[i]);
    free(dd->entries); free(dd);
}
typedef struct{int idx;Value val;} GradeIV;
static int grade_asc(const void *a,const void *b){return val_cmp(&((const GradeIV*)a)->val,&((const GradeIV*)b)->val);}
static int grade_desc(const void *a,const void *b){return grade_asc(b,a);}
static void prim_grade(Frame *e,int ascending) {
    (void)e; Value top=speek();
    if(top.tag!=VAL_LIST) die("%s: expected list, got %s", ascending?"rise":"fall", valtag_name(top.tag));
    int s=val_slots(top),len=(int)top.as.compound.len,base=sp-s;
    GradeIV *items=malloc(len*sizeof(GradeIV));
    for(int i=0;i<len;i++){ElemRef r=compound_elem(&stack[base],s,len,i);items[i].idx=i;items[i].val=stack[base+r.base];}
    qsort(items,len,sizeof(GradeIV),ascending?grade_asc:grade_desc);
    sp-=s; for(int i=0;i<len;i++) spush(val_int(items[i].idx));
    spush(val_compound(VAL_LIST,len,len+1)); free(items);
}
static void prim_shape(Frame *e) {
    (void)e; Value top=speek(); if(top.tag!=VAL_LIST) die("shape: expected list, got %s",valtag_name(top.tag));
    int len=(int)top.as.compound.len,s=val_slots(top),base=sp-s,dims[16],nd=0; dims[nd++]=len;
    if(len>0){ElemRef r0=compound_elem(&stack[base],s,len,0);Value e0=stack[base+r0.base+r0.slots-1];if(e0.tag==VAL_LIST)dims[nd++]=(int)e0.as.compound.len;}
    sp-=s; for(int i=0;i<nd;i++) spush(val_int(dims[i])); spush(val_compound(VAL_LIST,nd,nd+1));
}
/* ---- EVAL ---- */
/* eval_tuple_scoped: run a tuple body with proper let-scope isolation.
   Captures ee's bind_count/vals_used before the call, activates save_buf so
   any overwritten bindings are snapshotted, runs the body, then:
   - if the body returned closures over ee (new bindings escape), promotes
     those bindings into a fresh child frame the closures point at;
   - restores overwritten bindings from save_buf;
   - trims new bindings from ee.
   Shared by dispatch_word (for def-tuples), apply, and dip. Fixes the
   let-scoping-gotcha where apply previously stomped outer bindings. */
static void eval_tuple_scoped(Value *body, int slots, Frame *env) {
    Frame *ee=body[slots-1].as.compound.env?body[slots-1].as.compound.env:env;
    int sbc=ee->bind_count,svu=ee->vals_used,sp0=sp,sb0=save_buf_sp;
    int prev_active=frame_save_active; Frame *prev_target=frame_save_target; int prev_sbc=frame_save_sbc;
    frame_save_active=1; frame_save_target=ee; frame_save_sbc=sbc;
    eval_body(body,slots,env);
    frame_save_active=prev_active; frame_save_target=prev_target; frame_save_sbc=prev_sbc;
    if(ee->bind_count==sbc&&save_buf_sp==sb0) return;
    int has_closure=0;
    for(int i=sp;i>sp0;){Value vi=stack[i-1];int vs=val_slots(vi);
        if(vi.tag==VAL_TUPLE&&vi.as.compound.env==ee){has_closure=1;break;} i-=vs;}
    if(has_closure){
        Frame *cf=frame_new(ee);
        for(int i=sbc;i<ee->bind_count;i++){
            Binding *bi=&ee->bindings[i];
            frame_bind(cf,bi->sym,&ee->vals[bi->offset],bi->slots,bi->kind,bi->recur);
        }
        for(int i=sp;i>sp0;){Value vi=stack[i-1];int vs=val_slots(vi);
            if(vi.tag==VAL_TUPLE&&vi.as.compound.env==ee) stack[i-1].as.compound.env=cf;
            i-=vs;}
    }
    if(save_buf_sp>sb0){
        for(int p=sb0;p<save_buf_sp;){
            int bi=(int)save_buf[p++].as.i;
            int off=(int)save_buf[p++].as.i;
            int alloc=(int)save_buf[p++].as.i;
            int sl=(int)save_buf[p++].as.i;
            int kr=(int)save_buf[p++].as.i;
            VCPY(&ee->vals[off],&save_buf[p],sl);p+=sl;
            ee->bindings[bi].offset=off;ee->bindings[bi].allocated=alloc;
            ee->bindings[bi].slots=sl;ee->bindings[bi].kind=kr>>1;ee->bindings[bi].recur=kr&1;}
        save_buf_sp=sb0;
    }
    for(int i=sbc;i<ee->bind_count;i++){
        uint32_t h=ee->bindings[i].sym%FRAME_HASH_SIZE;
        for(int j=0;j<FRAME_HASH_SIZE;j++){uint32_t s=(h+j)%FRAME_HASH_SIZE;
            if(ee->hash[s]==i+1){ee->hash[s]=0;break;}}}
    ee->bind_count=sbc;ee->vals_used=svu;
}
static void dispatch_word(uint32_t sym, Frame *env) {
    Lookup lu=frame_lookup(env,sym);
    if(lu.bind){
        Binding *b=lu.bind; Value *v=&lu.frame->vals[b->offset];
        if(b->kind==BIND_DEF&&v[b->slots-1].tag==VAL_TUPLE){ eval_tuple_scoped(v,b->slots,env); return; }
        SPUSH(v,b->slots);
        return;
    }
    PrimFn fn=prim_lookup(sym);if(fn){fn(env);return;}
    die("unknown word: %s",sym_name(sym));
}
static void eval_body(Value *body, int slots, Frame *env) {
    if(++eval_depth > EVAL_DEPTH_MAX) die("recursion depth exceeded (%d levels)", EVAL_DEPTH_MAX);
    Value hdr=body[slots-1]; if(hdr.tag!=VAL_TUPLE) die("eval_body: expected tuple, got %s (internal: evaluator received non-tuple header)", valtag_name(hdr.tag));
    int len=(int)hdr.as.compound.len; if(len>LOCAL_MAX) die("tuple body too large");
    Frame *ee=hdr.as.compound.env?hdr.as.compound.env:env;
    int sbc=ee->bind_count,svu=ee->vals_used;
    int ob[len>0?len:1], sb[len>0?len:1];
    compute_offsets(body,slots,len,ob,sb);
    for(int k=0;k<len;k++){
        int eo=ob[k],es=sb[k]; Value elem=body[eo+es-1];
        if(elem.loc){current_fid=LOC_FID(elem.loc);current_line=LOC_LINE(elem.loc);current_col=LOC_COL(elem.loc);}
        if(elem.tag<=VAL_SYM) stack[sp++]=elem;
        else if(is_compound(elem.tag)){
            SPUSH(&body[eo],es);
            if(elem.tag==VAL_TUPLE){ee->refcount++;stack[sp-1].as.compound.env=ee;}
        } else if(elem.tag==VAL_XT) elem.as.xt.fn(ee);
        else if(elem.tag==VAL_WORD){
            uint32_t sym=elem.as.sym;
            if(sym==S_DEF){ int ds=val_slots(stack[sp-1]);
                if(recur_pending){recur_pending=0;frame_bind(ee,recur_sym,&stack[sp-ds],ds,BIND_DEF,1);sp-=ds;}
                else{Value db[ds];VCPY(db,&stack[sp-ds],ds);sp-=ds;frame_bind(ee,pop_sym(),db,ds,BIND_DEF,0);}
            } else if(sym==S_LET){ uint32_t n=pop_sym(); int ls=val_slots(stack[sp-1]); frame_bind(ee,n,&stack[sp-ls],ls,BIND_LET,0); sp-=ls;
            } else if(sym==S_RECUR){recur_sym=pop_sym();recur_pending=1;}
            else dispatch_word(sym,ee);
        } else if(elem.tag==VAL_BOX||elem.tag==VAL_DICT) spush(elem);
    }
    if(ee->refcount==0){ee->bind_count=sbc;ee->vals_used=svu;}
    eval_depth--;
}
static int find_matching(Token *toks, int start, int count, TokTag open, TokTag close) {
    int depth=1;
    for(int i=start;i<count;i++){if(toks[i].tag==open)depth++;else if(toks[i].tag==close){depth--;if(depth==0)return i;}}
    die("unmatched bracket"); return -1;
}
static void build_tuple(Token *toks, int start, int end, int tc, Frame *env) {
    int eb=sp,ec=0; Token *ft = (start < end) ? &toks[start] : NULL;
    for(int j=start;j<end;j++){
        Token *tt=&toks[j]; current_fid=tt->fid; current_line=tt->line; current_col=tt->col;
        switch(tt->tag){
        case TOK_INT:
            if(j+1<end && toks[j+1].tag==TOK_WORD) {
                uint32_t ns=toks[j+1].as.sym; PrimFn fused=NULL;
                if(tt->as.i==1 && ns==S_PLUS) fused=prim_fused_inc;
                else if(tt->as.i==1 && ns==S_SUB) fused=prim_fused_dec;
                else if(tt->as.i==0 && ns==S_EQ) fused=prim_fused_iszero;
                else if(tt->as.i==2 && ns==S_PLUS) fused=prim_fused_add2;
                else if(tt->as.i==2 && ns==S_SUB) fused=prim_fused_sub2;
                else if(tt->as.i==2 && ns==S_MUL) fused=prim_fused_mul2;
                else if(tt->as.i==2 && ns==S_DIV) fused=prim_fused_div2;
                else if(tt->as.i==2 && ns==S_MOD) fused=prim_fused_mod2;
                else if(tt->as.i==6 && ns==S_PLUS) fused=prim_fused_add6;
                else if(tt->as.i==10 && ns==S_MUL) fused=prim_fused_mul10;
                else if(tt->as.i==10 && ns==S_DIV) fused=prim_fused_div10;
                if(fused){spush(with_tok(val_xt(ns,fused),tt));ec++;j++;break;}
            }
            spush(with_tok(val_int(tt->as.i),tt)); ec++; break;
        case TOK_FLOAT: spush(with_tok(val_float(tt->as.f),tt)); ec++; break;
        case TOK_SYM: spush(with_tok(val_sym(tt->as.sym),tt)); ec++; break;
        case TOK_WORD:
            if(tt->as.sym==S_CHECK){if(ec>0&&(stack[sp-1].tag==VAL_WORD||stack[sp-1].tag==VAL_XT)){sp--;ec--;}else die("check: expected preceding type word, got %s",ec>0?valtag_name(stack[sp-1].tag):"empty stack");}
            else{
                PrimFn xf=prim_lookup(tt->as.sym);
                if(xf && j+1<end && toks[j+1].tag==TOK_WORD && toks[j+1].as.sym==S_MUST) {
                    PrimFn fused=NULL; uint32_t s=tt->as.sym;
                    if(s==S_GET) fused=prim_get_must; else if(s==S_POP) fused=prim_pop_must;
                    else if(s==S_AT) fused=prim_at_must; else if(s==S_NTH) fused=prim_nth_must;
                    else if(s==S_SET) fused=prim_set_must; else if(s==S_EDIT) fused=prim_edit_must;
                    else if(s==S_INDEXOF) fused=prim_indexof_must; else if(s==S_STRFIND) fused=prim_strfind_must;
                    if(fused){spush(with_tok(val_xt(s,fused),tt));ec++;j++;break;}
                }
                if(xf && j+1<end && toks[j+1].tag==TOK_WORD && toks[j+1].as.sym==S_DROP && tt->as.sym==S_SWAP)
                    {spush(with_tok(val_xt(tt->as.sym,prim_fused_nip),tt));ec++;j++;break;}
                spush(with_tok(xf?val_xt(tt->as.sym,xf):val_word(tt->as.sym),tt));ec++;
            }
            break;
        case TOK_STRING:
            for(int c=0;c<tt->as.str.len;c++) spush(with_tok(val_int(tt->as.str.codes[c]),tt));
            spush(with_tok(val_compound(VAL_LIST,tt->as.str.len,tt->as.str.len+1),tt)); ec++; break;
        case TOK_LPAREN:{int nc=find_matching(toks,j+1,tc,TOK_LPAREN,TOK_RPAREN);build_tuple(toks,j+1,nc,tc,env);stack[sp-1].loc=LOC_PACK(tt->fid,tt->line,tt->col);ec++;j=nc;break;}
        case TOK_LBRACKET:{
            int bc=find_matching(toks,j+1,tc,TOK_LBRACKET,TOK_RBRACKET);
            if(bc+1<tc&&toks[bc+1].tag==TOK_WORD&&toks[bc+1].as.sym==S_EFFECT){j=bc+1;break;}
            int lb=sp; eval(toks+j+1,bc-j-1,env);
            int n=0,p=sp; while(p>lb){p-=val_slots(stack[p-1]);n++;}
            spush(with_tok(val_compound(VAL_LIST,n,sp-lb+1),tt)); ec++; j=bc; break;
        }
        case TOK_LBRACE:{
            int bc=find_matching(toks,j+1,tc,TOK_LBRACE,TOK_RBRACE);
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
static void prim_find_elem(Frame *env) {
    POP_BODY(fn,"find"); POP_VAL(def); POP_LIST_BUF(list,"find");
    int offs[LOCAL_MAX],szs[LOCAL_MAX]; compute_offsets(list_buf,list_s,list_len,offs,szs);
    for(int i=0;i<list_len;i++){
        int rb=sp; SPUSH(&list_buf[offs[i]],szs[i]); SPUSH(&list_buf[offs[i]],szs[i]);
        eval_body(fn_buf,fn_s,env); if(pop_int()) return; sp=rb;
    }
    SPUSH(def_buf,def_s);
}
#ifndef SLAP_SDL
static void prim_millis(Frame *e){(void)e;struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);spush(val_int((int64_t)(ts.tv_sec*1000+ts.tv_nsec/1000000)));}
#endif
#define A2E " ['a num lent in  'a num lent in  'a num move out] effect\n"
#define I2E " [int lent in  int lent in  int move out] effect\n"
#define F1E " [float lent in  float move out] effect\n"
#define F2E " [float lent in  float lent in  float move out] effect\n"
#define LNE " ['a list own in  int lent in  'a list move out] effect\n"
#define L1E " ['a list own in  'a list move out] effect\n"
#define LIE " ['a list own in  int list own in  'a list move out] effect\n"
#define LGE " ['a list own in  int list move out] effect\n"
#define LDE " ['a list own in  int list own in  list move out] effect\n"
#define MO " move out] effect\n"
static const char *BUILTIN_TYPES =
    "'dup ['a copy in  'a copy out  'a copy out] effect\n"
    "'drop ['a copy in] effect\n'swap ['a own in  'b own in  'b own out  'a own out] effect\n"
    "'plus" A2E "'sub" A2E "'mul" A2E "'div" A2E
    "'mod" I2E "'wrap" I2E "'band" I2E "'bor" I2E "'bxor" I2E "'shl" I2E "'shr" I2E
    "'bnot [int lent in  int" MO "'divmod [int lent in  int lent in  int move out  int" MO
    "'eq [lent in  lent in  int" MO "'lt [lent in  lent in  int" MO
    "'and" I2E "'or" I2E
    "'print [own in] effect\n'assert [int own in] effect\n'millis [int" MO
    "'itof [int lent in  float" MO "'ftoi [float lent in  int" MO
    "'fsqrt" F1E "'fsin" F1E "'fcos" F1E "'ftan" F1E "'ffloor" F1E "'fceil" F1E "'fround" F1E "'fexp" F1E "'flog" F1E
    "'fpow" F2E "'fatan2" F2E
    "'list [list" MO "'len [sized auto in  int" MO "'push ['a seq own in  'a own in  'a seq" MO "'pop ['a seq own in  'a seq move out  {'ok 'a 'no ()} either move out] effect\n"
    "'get ['a seq own in  int lent in  {'ok 'a 'no ()} either move out] effect\n'nth [sym lent in  int lent in  {'ok 'a 'no ()} either move out] effect\n'set ['a seq own in  int lent in  'a own in  {'ok 'a 'no ()} either move out] effect\n'cat ['a semigroup own in  'a semigroup own in  'a semigroup" MO
    "'take-n" LNE "'drop-n" LNE "'range [int lent in  int lent in  int list" MO "'sort ['a ord list own in  'a ord list" MO
    "'reverse" L1E "'dedup" L1E "'index-of ['a list own in  'a lent in  {'ok int 'no ()} either move out] effect\n'select" LIE "'pick" LIE "'keep-mask" LIE
    "'rise" LGE "'fall" LGE "'shape" LGE "'classify" LGE "'stack [tuple" MO "'compose [tuple own in  tuple own in  tuple" MO "'rec [rec" MO
    "'random [int lent in  int" MO "'halt [] effect\n'box ['a own in  'a box" MO "'free [linear own in] effect\n"
    "'at [rec own in  sym lent in  {'ok 'a 'no ()} either move out] effect\n'into [rec own in  own in  sym lent in  rec" MO "'clone ['a linear own in  'a linear move out  'a linear" MO
    "'clear [int lent in] effect\n'pixel [int lent in  int lent in  int lent in] effect\n'fill-rect [int lent in  int lent in  int lent in  int lent in  int lent in] effect\n"
    "'rotate" LNE "'windows" LNE "'zip ['a list own in  'a list own in  list" MO "'group" LDE "'reshape" LDE "'transpose [list own in  list" MO
    "'read [list own in  {'ok list 'no list} either move out] effect\n'write [list own in  int list own in  {'ok int 'no list} either move out] effect\n'ls [list own in  {'ok list 'no list} either move out] effect\n"
    "'utf8-encode [int list own in  {'ok list 'no int} either move out] effect\n'utf8-decode [int list own in  {'ok list 'no int} either move out] effect\n'str-find [int list own in  int list own in  {'ok int 'no ()} either move out] effect\n"
    "'str-split [int list own in  int list own in  list" MO "'parse-http [int list own in  {'ok rec 'no list} either move out] effect\n'args [list" MO "'isheadless [int" MO "'cwd [list" MO
#ifndef SLAP_WASM
    "'tcp-connect [int list own in  int lent in  {'ok box 'no list} either move out] effect\n'tcp-send [int box own in  int list own in  {'ok int 'no list} either move out] effect\n"
    "'tcp-recv [int box own in  int lent in  int box move out  {'ok list 'no list} either move out] effect\n'tcp-close [int box own in] effect\n'tcp-listen [int lent in  {'ok box 'no list} either move out] effect\n'tcp-accept [int box own in  int box move out  {'ok box 'no list} either move out] effect\n"
#endif
    "'tag ['a own in  sym lent in  'a tagged" MO "'default [tagged own in  own in " MO "'must [tagged own in " MO
    "'dict ['a dict" MO "'insert ['a dict own in  list lent in  'a own in  'a dict" MO
    "'of ['a dict own in  list lent in  'a dict move out  {'ok 'a 'no list} either move out] effect\n"
    "'remove ['a dict own in  list lent in  'a dict" MO "'dict-keys ['a dict own in  'a dict move out  list" MO
    "'dict-values ['a dict own in  'a dict move out  'a list" MO
    "'pthen [tagged own in  own in  tuple own in  move out  tagged" MO
;
#undef A2E
#undef I2E
#undef F1E
#undef F2E
#undef LNE
#undef L1E
#undef LIE
#undef LGE
#undef LDE
#undef MO
static const char *PRELUDE =
    "'over (swap dup (swap) dip) def\n'nip (swap drop) def\n'rot ((swap) dip swap) def\n'tuck (swap over) def\n"
    "'not (0 eq) [int lent in  int move out] effect def\n'neq (eq not) [lent in  lent in  int move out] effect def\n"
    "'gt (swap lt) [lent in  lent in  int move out] effect def\n'ge (lt not) [lent in  lent in  int move out] effect def\n'le (swap lt not) [lent in  lent in  int move out] effect def\n"
    "'inc (1 plus) [num lent in  num move out] effect def\n'dec (1 sub) [num lent in  num move out] effect def\n'neg (0 swap sub) [num lent in  num move out] effect def\n"
    "'max (over over lt (nip) (drop) if) ['a ord lent in  'a ord lent in  'a ord move out] effect def\n'min (over over lt (drop) (nip) if) ['a ord lent in  'a ord lent in  'a ord move out] effect def\n'abs (dup 0 lt (neg) (dup drop) if) def\n"
    "'keep (over (apply) dip) def\n'bi ((keep) dip apply) def\n'repeat ('f swap def (dup 0 gt) (1 sub (f) dip) while drop) def\n"
    "'select (swap 'data swap def (data swap get must) each) def\n'reduce (swap dup 0 get must swap 1 drop-n swap rot fold) def\n'table ((dup) swap compose (couple) compose each) def\n"
    "'filter ('p swap def list (dup p (push) (drop) if) fold) ['a list own in  tuple own in  'a list move out] effect def\n"
    "'where ('p swap def 0 'idx let list ('elem swap def elem p (idx push) () if idx 1 plus 'idx let) fold) def\n"
    "'sqr (dup mul) def\n'cube (dup dup mul mul) def\n'ispos (0 swap lt) def\n'isneg (0 lt) def\n"
    "'first (0 get must) def\n'second (1 get must) def\n'third (2 get must) def\n'fourth (3 get must) def\n'fifth (4 get must) def\n"
    "'sixth (5 get must) def\n'seventh (6 get must) def\n'eighth (7 get must) def\n'ninth (8 get must) def\n'tenth (9 get must) def\n"
    "'last (dup len 1 sub get must) def\n'sum (0 (plus) fold) def\n'product (1 (mul) fold) def\n"
    "'max-of (dup first (max) fold) def\n'min-of (dup first (min) fold) def\n'member (index-of (drop 1 ok) then 0 default) def\n'couple (list rot push swap push) def\n"
    "'isany (0 (or) fold) def\n'isall (1 (and) fold) def\n'flatten (list (cat) fold) def\n'sort-desc (sort reverse) def\n'fneg (0.0 swap sub) def\n"
    "'fabs (dup 0.0 lt (fneg) () if) def\n'frecip (1.0 swap div) def\n'fsign (dup 0.0 lt (drop -1.0) (dup 0.0 eq (drop 0.0) (drop 1.0) if) if) def\n'sign (dup 0 lt (drop -1) (dup 0 eq (drop 0) (drop 1) if) if) def\n"
    "'clamp (rot swap min max) def\n'fclamp (swap min max) def\n'lerp ((over sub) dip swap mul plus) def\n'isbetween (rot dup (rot swap le) dip rot rot ge and) def\n"
    "'iszero (0 eq) [int lent in  int move out] effect def\n'iseven (2 mod 0 eq) [int lent in  int move out] effect def\n'isodd (2 mod 0 neq) [int lent in  int move out] effect def\n'divides (mod 0 eq) [int lent in  int lent in  int move out] effect def\n"
    "'ok ('ok tag) ['a own in  'a tagged move out] effect def\n'no ('no tag) ['a own in  'a tagged move out] effect def\n'none (() no) [tagged move out] effect def\n'times-i ('f swap def 'n let 0 (dup n lt) (dup (f) dip 1 plus) while drop) def\n"
    "3.14159265358979323846 'pi let\n6.28318530717958647692 'tau let\n2.71828182845904523536 'e let\n'rotate ('n let dup len 'ln let ln 0 eq not (n ln wrap 'nn let nn 0 eq not (dup ln nn sub take-n swap ln nn sub drop-n swap cat) () if) () if) def\n"
    "'zip ('b swap def 'a swap def a len b len min 'n let 0 n range (dup a swap get must swap b swap get must couple) each) def\n"
    "'windows ('n let 'l swap def l len 'll let n 0 le ll n lt or (list) (0 ll n sub 1 plus range ('i let l i drop-n n take-n) each) if) def\n"
    "'reshape ('dims swap def 'data swap def dims 0 get must 'rows let dims 1 get must 'cols let 0 rows range ('r let 0 cols range ('c let data r cols mul c plus get must) each) each) def\n"
    "'transpose ('m swap def m 0 get must len 'cols let m len 'rows let 0 cols range ('c let 0 rows range ('r let m r get must c get must) each) each) def\n"
    "'keep-mask ('mask swap def 0 mask len range (mask swap get must 0 neq) where select) def\n'group ('idx swap def 'data swap def 0 idx (max) fold 1 plus 'ng let 0 ng range ('g let 0 idx len range (idx swap get must g eq) where data swap select) each) def\n"
    "'classify (dup 'l swap def (l swap index-of must) each dup dedup 'u swap def (u swap index-of must) each) def\n'byte-mask (255 band) def\n'byte-bits ('b let 0 8 range (7 swap sub b swap shr 1 band) each) def\n"
    "'bits-byte (0 (swap 1 shl bor) fold) def\n'chunks ('n let list swap (dup len 0 eq not) (dup n take-n swap (push) dip n drop-n) while drop) def\n"
#ifndef SLAP_WASM
    "'crlf (list 13 push 10 push) def\n'int-str-digits recur ('n let n 0 gt (n 10 mod 48 plus push n 10 div int-str-digits) () if) def\n"
    "'int-str recur ('n let n 0 lt (n neg int-str list 45 push swap cat) (n 0 eq (list 48 push) (list n int-str-digits reverse) if) if) def\n'str-join ('sep let 'parts let parts len 0 eq (list) (parts 1 drop-n parts first (sep swap cat cat) fold) if) def\n"
    "'http-request ('body let 'headers let 'path let 'host let 'method let method \" \" cat path cat \" HTTP/1.1\" cat crlf cat \"Host: \" cat host cat crlf cat headers cat body len 0 gt (\"Content-Length: \" cat body len int-str cat crlf cat) () if crlf cat body cat) def\n"
#endif
;
#define LIST_POP(who,buf,len,ts,offs,szs) do{ \
    Value _t=speek();if(_t.tag!=VAL_LIST)die(who ": expected list, got %s",valtag_name(_t.tag)); \
    ts=val_slots(_t);len=(int)_t.as.compound.len;int _b=sp-ts; \
    if(ts>LOCAL_MAX)die(who ": too large");VCPY(buf,&stack[_b],ts);sp=_b; \
    compute_offsets(buf,ts,len,offs,szs);}while(0)
static void prim_reverse(Frame *e) {
    (void)e; Value buf[LOCAL_MAX]; int len,ts,offs[LOCAL_MAX],szs[LOCAL_MAX];
    LIST_POP("reverse",buf,len,ts,offs,szs);
    int rb=sp; for(int i=len-1;i>=0;i--){SPUSH(&buf[offs[i]],szs[i]);}
    spush(val_compound(VAL_LIST,len,sp-rb+1));
}
static void prim_dedup(Frame *e) {
    (void)e; Value buf[LOCAL_MAX]; int len,ts,offs[LOCAL_MAX],szs[LOCAL_MAX];
    LIST_POP("dedup",buf,len,ts,offs,szs);
    int rb=sp,rc=0;
    for(int i=0;i<len;i++){
        int dup=0; for(int j=0;j<i;j++) if(val_equal(&buf[offs[i]],szs[i],&buf[offs[j]],szs[j])){dup=1;break;}
        if(!dup){SPUSH(&buf[offs[i]],szs[i]);rc++;}
    }
    spush(val_compound(VAL_LIST,rc,sp-rb+1));
}
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
static struct{uint32_t event_sym;Value handler_body[LOCAL_MAX];int handler_slots;} event_handlers[MAX_HANDLERS];
static int handler_count=0;
static Value render_body[LOCAL_MAX]; static int render_slots=0;
static uint8_t gray_lut[4]={0,85,170,255};
static void sdl_init(void) {
    if(sdl_window) return;
    if(SDL_Init(SDL_INIT_VIDEO)<0) die("SDL_Init: %s",SDL_GetError());
    sdl_window=SDL_CreateWindow("slap",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,CANVAS_W,CANVAS_H,SDL_WINDOW_BORDERLESS|SDL_WINDOW_RESIZABLE);
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
    (void)e; Value fn_top=stack[sp-1]; if(fn_top.tag!=VAL_TUPLE) die("on: expected tuple handler, got %s", valtag_name(fn_top.tag));
    int fn_s=val_slots(fn_top); if(handler_count>=MAX_HANDLERS) die("on: too many event handlers");
    VCPY(event_handlers[handler_count].handler_body,&stack[sp-fn_s],fn_s);
    event_handlers[handler_count].handler_slots=fn_s; sp-=fn_s;
    event_handlers[handler_count].event_sym=pop_sym(); handler_count++;
}
static uint32_t sym_tick=0,sym_keydown=0,sym_mousedown=0,sym_mouseup=0,sym_mousemove=0;
static void show_intern_syms(void) {
    if(!sym_tick){sym_tick=sym_intern("tick");sym_keydown=sym_intern("keydown");sym_mousedown=sym_intern("mousedown");sym_mouseup=sym_intern("mouseup");sym_mousemove=sym_intern("mousemove");}
}
static void show_dispatch_event(SDL_Event *ev, Frame *env) {
    if(ev->type==SDL_KEYDOWN){for(int h=0;h<handler_count;h++)if(event_handlers[h].event_sym==sym_keydown){spush(val_int((int64_t)ev->key.keysym.sym));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}}
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
    if(render_slots>0){Value mt=stack[sp-1];int ms=val_slots(mt);SPUSH(&stack[sp-ms],ms);eval_body(render_body,render_slots,env);}
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
    Value fn_top=stack[sp-1]; if(fn_top.tag!=VAL_TUPLE) die("show: expected tuple render function, got %s", valtag_name(fn_top.tag));
    render_slots=val_slots(fn_top); VCPY(render_body,&stack[sp-render_slots],render_slots); sp-=render_slots;
    show_intern_syms();
    if(headless_mode){
        int64_t frame=0;
        for(;;){
            for(int h=0;h<handler_count;h++)
                if(event_handlers[h].event_sym==sym_tick){spush(val_int(frame));eval_body(event_handlers[h].handler_body,event_handlers[h].handler_slots,env);}
            frame++;
            usleep(16000);
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
    Value top=spop();if(top.tag!=VAL_LIST)die("%s: expected list",who);
    int len=(int)top.as.compound.len;if((int)top.as.compound.slots-1!=len)die("%s: list elements must all be single-slot (ints)",who);
    unsigned char *buf=malloc(len);
    for(int i=0;i<len;i++){Value v=stack[sp-len+i];if(v.tag!=VAL_INT)die("%s: byte element %d is not an int",who,i);
        if(v.as.i<0||v.as.i>255)die("%s: byte %d out of range (got %lld)",who,i,(long long)v.as.i);buf[i]=(unsigned char)v.as.i;}
    sp-=len;*out_len=len;return buf;
}
static char *pop_string_path(const char *who) {
    int len; unsigned char *raw = pop_byte_list_buf(who, &len);
    char *buf = realloc(raw, len + 1); buf[len] = '\0'; return buf;
}
static void push_c_string(const char *s);
static void push_byte_list(const unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) spush(val_int(buf[i]));
    spush(val_compound(VAL_LIST, (int)len, (int)len + 1));
}
static void prim_read(Frame *e) {
    (void)e; char *path=pop_string_path("read");
    FILE *f=fopen(path,"rb");
    if(!f) { push_c_string(path); free(path); push_no(); return; }
    fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);
    unsigned char *buf=malloc(sz);size_t n=fread(buf,1,sz,f);fclose(f);
    if((long)n!=sz) { free(buf); push_c_string(path); free(path); push_no(); return; }
    push_byte_list(buf,n);free(buf);free(path); push_ok();
}
static void prim_write(Frame *e) {
    (void)e; int len;unsigned char *buf=pop_byte_list_buf("write",&len);char *path=pop_string_path("write");
    FILE *f=fopen(path,"wb");if(!f){free(buf);push_c_string(path);free(path);push_no();return;}
    size_t n=fwrite(buf,1,len,f);fclose(f);
    if((int)n!=len){free(buf);push_c_string(path);free(path);push_no();return;}
    free(buf);free(path); spush(val_int(1)); push_ok();
}
static void prim_ls(Frame *e) {
    (void)e; char *path=pop_string_path("ls");
    DIR *d=opendir(path); if(!d) { push_c_string(path); free(path); push_no(); return; }
    struct dirent *ent; int base=sp,count=0;
    while((ent=readdir(d))!=NULL){if(strcmp(ent->d_name,".")==0||strcmp(ent->d_name,"..")==0)continue;
        push_c_string(ent->d_name);count++;}
    closedir(d);spush(val_compound(VAL_LIST,count,sp-base+1));free(path); push_ok();
}
static void prim_utf8_encode(Frame *e) {
    (void)e; Value top = spop(); if (top.tag != VAL_LIST) die("utf8-encode: expected list of codepoints, got %s", valtag_name(top.tag));
    int len = (int)top.as.compound.len; if ((int)top.as.compound.slots - 1 != len) die("utf8-encode: elements must all be ints");
    int64_t *cps = malloc(len * sizeof(int64_t));
    for (int i = 0; i < len; i++) { Value v = stack[sp-len+i]; if (v.tag != VAL_INT) die("utf8-encode: element %d is not an int", i); cps[i] = v.as.i; }
    sp -= len; int base = sp, bc = 0;
#define PB(x) spush(val_int(x))
    for (int i = 0; i < len; i++) { int64_t cp = cps[i];
        if (cp < 0 || cp > 0x10FFFF) { free(cps); sp=base; spush(val_int(i)); push_no(); return; }
        if (cp<0x80) { PB(cp); bc++; } else if (cp<0x800) { PB(0xC0|(cp>>6)); PB(0x80|(cp&0x3F)); bc+=2; }
        else if (cp<0x10000) { PB(0xE0|(cp>>12)); PB(0x80|((cp>>6)&0x3F)); PB(0x80|(cp&0x3F)); bc+=3; }
        else { PB(0xF0|(cp>>18)); PB(0x80|((cp>>12)&0x3F)); PB(0x80|((cp>>6)&0x3F)); PB(0x80|(cp&0x3F)); bc+=4; } }
#undef PB
    free(cps); spush(val_compound(VAL_LIST, bc, sp - base + 1)); push_ok();
}
static void prim_utf8_decode(Frame *e) {
    (void)e; int len; unsigned char *bytes = pop_byte_list_buf("utf8-decode", &len);
    int base = sp, nc = 0, i = 0;
    while (i < len) { int64_t cp; int ex; unsigned char b = bytes[i];
        if (b<0x80){cp=b;ex=0;}else if((b&0xE0)==0xC0){cp=b&0x1F;ex=1;}else if((b&0xF0)==0xE0){cp=b&0x0F;ex=2;}
        else if((b&0xF8)==0xF0){cp=b&0x07;ex=3;}else{free(bytes);sp=base;spush(val_int(i));push_no();return;}
        i++; for(int j=0;j<ex;j++){if(i>=len){free(bytes);sp=base;spush(val_int(i));push_no();return;}
            if((bytes[i]&0xC0)!=0x80){free(bytes);sp=base;spush(val_int(i));push_no();return;}
            cp=(cp<<6)|(bytes[i]&0x3F);i++;}
        spush(val_int(cp)); nc++; }
    free(bytes); spush(val_compound(VAL_LIST, nc, sp - base + 1)); push_ok();
}

#ifndef SLAP_WASM
static int pop_socket_fd(const char *who) {
    Value v = spop();
    if (v.tag != VAL_BOX) die("%s: expected socket (box)", who);
    BoxData *bd = (BoxData*)v.as.box;
    if (bd->slots != 1 || bd->data[0].tag != VAL_INT) die("%s: socket box must contain a single int", who);
    return (int)bd->data[0].as.i;
}
static void push_socket_box(int fd) {
    BoxData *bd=malloc(sizeof(BoxData));bd->data=malloc(sizeof(Value));bd->slots=1;bd->data[0]=val_int(fd);
    Value v;v.tag=VAL_BOX;v.loc=0;v.as.box=bd;spush(v);
}
static void prim_tcp_connect(Frame *e) {
    (void)e; int64_t port=pop_int(); char *host=pop_string_path("tcp-connect");
    struct addrinfo hints={0},*res; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    char ps[16]; snprintf(ps,sizeof(ps),"%lld",(long long)port);
    int err=getaddrinfo(host,ps,&hints,&res); if(err){free(host);push_c_string("getaddrinfo failed");push_no();return;}
    int fd=socket(res->ai_family,res->ai_socktype,res->ai_protocol); if(fd<0){freeaddrinfo(res);free(host);push_c_string("socket failed");push_no();return;}
    if(connect(fd,res->ai_addr,res->ai_addrlen)<0){close(fd);freeaddrinfo(res);free(host);push_c_string("connect failed");push_no();return;}
    freeaddrinfo(res);free(host);push_socket_box(fd);push_ok();
}
static void prim_tcp_send(Frame *e) {
    (void)e; int len; unsigned char *buf = pop_byte_list_buf("tcp-send", &len);
    int fd = pop_socket_fd("tcp-send"); push_socket_box(fd);
    size_t sent = 0; while (sent < (size_t)len) { ssize_t n = send(fd, buf + sent, len - sent, 0); if (n <= 0) { free(buf); push_c_string("send failed"); push_no(); return; } sent += n; }
    free(buf); spush(val_int(1)); push_ok();
}
static void prim_tcp_recv(Frame *e) {
    (void)e; int64_t maxlen = pop_int(); if (maxlen <= 0) maxlen = 1;
    int fd = pop_socket_fd("tcp-recv"); push_socket_box(fd);
    unsigned char *buf = malloc(maxlen); ssize_t n = recv(fd, buf, maxlen, 0);
    if (n < 0) { free(buf); push_c_string("recv failed"); push_no(); return; }
    push_byte_list(buf, n); free(buf); push_ok();
}
static void prim_tcp_close(Frame *e) { (void)e; Value v=spop(); if(v.tag!=VAL_BOX)die("tcp-close: expected box"); BoxData *bd=(BoxData*)v.as.box; close((int)bd->data[0].as.i); free(bd->data); free(bd); }
static void prim_tcp_listen(Frame *e) {
    (void)e; int64_t port=pop_int(); int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0){push_c_string("socket failed");push_no();return;}
    int opt=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=INADDR_ANY;addr.sin_port=htons((uint16_t)port);
    if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0){close(fd);push_c_string("bind failed");push_no();return;}
    if(listen(fd,128)<0){close(fd);push_c_string("listen failed");push_no();return;} push_socket_box(fd);push_ok();
}
static void prim_tcp_accept(Frame *e) {
    (void)e; int sfd=pop_socket_fd("tcp-accept"); struct sockaddr_in ca; socklen_t al=sizeof(ca);
    int cfd=accept(sfd,(struct sockaddr*)&ca,&al); if(cfd<0){push_socket_box(sfd);push_c_string("accept failed");push_no();return;} push_socket_box(sfd); push_socket_box(cfd); push_ok();
}
#endif
static inline void prim_strfind_impl(Frame *e, int tagged) {
    (void)e; int nl,hl; unsigned char *needle=pop_byte_list_buf("str-find",&nl); unsigned char *hay=pop_byte_list_buf("str-find",&hl);
    int r=-1; for(int i=0;i<=hl-nl;i++) if(memcmp(hay+i,needle,nl)==0){r=i;break;}
    free(needle);free(hay);
    if(r<0) { if(tagged) push_none(); else die("str-find: not found"); }
    else { spush(val_int(r)); if(tagged) push_ok(); }
}
static void prim_str_find(Frame *e) { prim_strfind_impl(e,1); }
static void prim_strfind_must(Frame *e) { prim_strfind_impl(e,0); }
static void prim_str_split(Frame *e) {
    (void)e; int dl,sl; unsigned char *delim=pop_byte_list_buf("str-split",&dl); unsigned char *str=pop_byte_list_buf("str-split",&sl);
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
    if(split<0){free(raw);push_c_string("no header/body separator");push_no();return;}
    int se=memfind(raw,split,"\r\n",2,0); if(se<0)se=split;
    int sp1=-1; for(int i=0;i<se;i++)if(raw[i]==' '){sp1=i;break;}
    int sc=0; if(sp1>=0)for(int i=sp1+1;i<se&&raw[i]>='0'&&raw[i]<='9';i++)sc=sc*10+(raw[i]-'0');
    spush(val_int(sc));
    uint32_t ks=sym_intern("key"),vs=sym_intern("value");
    int rb=sp,hc=0,pos=se+2;
    while(pos<split){
        int le=memfind(raw,split,"\r\n",2,pos); if(le<0)le=split;
        if(le==pos){pos+=2;continue;}
        int colon=-1;for(int i=pos;i<le-1;i++)if(raw[i]==':'&&raw[i+1]==' '){colon=i;break;}
        int kl=colon>=0?colon-pos:le-pos, vo=colon>=0?colon+2:le, vl=colon>=0?le-colon-2:0;
        spush(val_sym(ks));push_byte_list(raw+pos,kl);spush(val_sym(vs));push_byte_list(raw+vo,vl);
        spush(val_compound(VAL_RECORD,2,1+(kl+1)+1+(vl+1)+1));hc++;pos=le+2;
    }
    spush(val_compound(VAL_LIST,hc,sp-rb+1));
    push_byte_list(raw+split+4,rlen-split-4);free(raw);
    int body_s=val_slots(stack[sp-1]),hdrs_s=val_slots(stack[sp-1-body_s]);
    spush(val_compound(VAL_TAGGED,S_OK,1+hdrs_s+body_s+1));
}
static void push_c_string(const char *s) {
    int len=(int)strlen(s); for(int i=0;i<len;i++) spush(val_int((unsigned char)s[i])); spush(val_compound(VAL_LIST,len,len+1));
}
static void prim_args(Frame *e) {
    (void)e; int ts=0; for(int i=0;i<cli_argc;i++){push_c_string(cli_args[i]);ts+=(int)strlen(cli_args[i])+1;}
    spush(val_compound(VAL_LIST,cli_argc,ts+1));
}
static void prim_isheadless(Frame *e){(void)e;spush(val_int(headless_mode));}
static void prim_cwd(Frame *e){(void)e;char buf[4096];if(!getcwd(buf,sizeof(buf)))die("cwd: getcwd failed");push_c_string(buf);}
#define PRIM(nm,body) static void prim_##nm(Frame *e){(void)e;body;}
PRIM(list, spush(val_compound(VAL_LIST,0,1)))
PRIM(rec, spush(val_compound(VAL_RECORD,0,1)))
PRIM(take_n, prim_slice_n(1)) PRIM(drop_n, prim_slice_n(0))
PRIM(rise, prim_grade(e,1)) PRIM(fall, prim_grade(e,0))
#undef PRIM
#define R(n,f) {#n,prim_##f}
static void register_prims(void) {
    static struct{const char*n;PrimFn f;} t[]={
        R(dup,dup),R(drop,drop),R(swap,swap),R(dip,dip),R(apply,apply),
        R(plus,plus),R(sub,sub),R(mul,mul),R(div,div),R(mod,mod),R(divmod,divmod),R(wrap,wrap),
        R(band,band),R(bor,bor),R(bxor,bxor),R(bnot,bnot),R(shl,shl),R(shr,shr),
        R(eq,eq),R(lt,lt),R(and,and),R(or,or),
        R(print,print),R(assert,assert),R(halt,halt),R(random,random),
        R(if,if),R(cond,case),R(case,case),R(loop,loop),R(while,while),
        R(itof,itof),R(ftoi,ftoi),R(fsqrt,fsqrt),R(fsin,fsin),R(fcos,fcos),R(ftan,ftan),
        R(ffloor,ffloor),R(fceil,fceil),R(fround,fround),R(fexp,fexp),R(flog,flog),R(fpow,fpow),R(fatan2,fatan2),
        R(stack,stack),{"compose",prim_concat},R(list,list),R(len,size),R(push,push_op),R(pop,pop_op),
        {"get",prim_get},R(nth,nth),R(set,replace_at),R(cat,concat),
        {"take-n",prim_take_n},{"drop-n",prim_drop_n},R(range,range),
        R(fold,fold),R(each,each),R(sort,sort),{"index-of",prim_index_of},R(scan,scan),
        R(at,at),R(rise,rise),R(fall,fall),R(shape,shape),
        R(rec,rec),R(into,into),R(edit,edit),R(reverse,reverse),R(dedup,dedup),R(find,find_elem),
        R(millis,millis),R(box,box),R(free,free),R(lend,lend),R(mutate,mutate),R(clone,clone),
        R(dict,dict),R(insert,insert),R(of,of),R(remove,remove),
        {"dict-keys",prim_keys},{"dict-values",prim_values},
        R(tag,tag),R(untag,case),R(union,union),R(then,then),R(default,default),R(must,must),R(pthen,pthen),
        R(read,read),R(write,write),R(ls,ls),
        {"utf8-encode",prim_utf8_encode},{"utf8-decode",prim_utf8_decode},
        {"str-find",prim_str_find},{"str-split",prim_str_split},{"parse-http",prim_parse_http},
        R(args,args),R(isheadless,isheadless),R(cwd,cwd),
#ifndef SLAP_WASM
        {"tcp-connect",prim_tcp_connect},{"tcp-send",prim_tcp_send},{"tcp-recv",prim_tcp_recv},
        {"tcp-close",prim_tcp_close},{"tcp-listen",prim_tcp_listen},{"tcp-accept",prim_tcp_accept},
#endif
#ifdef SLAP_SDL
        R(clear,clear),R(pixel,pixel),{"fill-rect",prim_fill_rect},R(on,on),R(show,show),
#endif
        {NULL,NULL}};
    for(int i=0;t[i].n;i++) prim_register(t[i].n,t[i].f);
}
#undef R

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));
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
    current_fid=FID_PRELUDE; lex(PRELUDE, FID_PRELUDE); eval(tokens,tok_count,global);
    current_fid=FID_STDIN;
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
    store_source_lines(BUILTIN_TYPES, FID_BUILTIN);
    lex(BUILTIN_TYPES, FID_BUILTIN); memcpy(combined,tokens,tok_count*sizeof(Token)); cpos=tok_count;
    lex(PRELUDE, FID_PRELUDE); memcpy(&combined[cpos],tokens,tok_count*sizeof(Token)); cpos+=tok_count;
    int user_start=cpos;
    memcpy(&combined[cpos],user_tokens,user_tok_count*sizeof(Token)); cpos+=user_tok_count;
    int errors=typecheck_tokens(combined,cpos,user_start);
    if(errors>0){fprintf(stderr,"%d type error(s)\n",errors);free(src);frame_free(global);return 1;}
    if(check_only){fprintf(stderr,"type check passed\n");free(src);frame_free(global);return 0;}
    current_fid=FID_STDIN; current_line=0; current_col=0;
    eval(user_tokens,user_tok_count,global);
    free(src); frame_free(global); return 0;
}
