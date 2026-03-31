// slap WASM frontend — draws to canvas via JS, no SDL
#include "slap.c"
#include <emscripten.h>

#define CANVAS_W 640
#define CANVAS_H 480
#define CANVAS_SIZE (CANVAS_W * CANVAS_H)

static uint8_t canvas[CANVAS_SIZE];

#define MAX_HANDLERS 16
typedef struct { uint32_t event_sym; Val handler; } EventHandler;
static EventHandler handlers[MAX_HANDLERS];
static int handler_count = 0;

static Val wasm_model;
static Val wasm_render_fn;
static bool wasm_running = false;
static int64_t wasm_frame = 0;

static EventHandler *find_handler(uint32_t sym) {
    for (int i = 0; i < handler_count; i++)
        if (handlers[i].event_sym == sym) return &handlers[i];
    return NULL;
}

static void p_clear(void) {
    int64_t color = pop_int();
    memset(canvas, (uint8_t)(color & 3), CANVAS_SIZE);
}

static void p_pixel(void) {
    int64_t color = pop_int();
    int64_t y = pop_int();
    int64_t x = pop_int();
    if (x >= 0 && x < CANVAS_W && y >= 0 && y < CANVAS_H)
        canvas[y * CANVAS_W + x] = (uint8_t)(color & 3);
}

static void p_millis(void) {
    push(VAL_INT(wasm_frame * 16));
}

static void p_on(void) {
    Val handler = pop();
    EXPECT(handler, T_TUPLE);
    uint32_t event_name = pop_sym();
    for (int i = 0; i < handler_count; i++) {
        if (handlers[i].event_sym == event_name) {
            val_free(handlers[i].handler);
            handlers[i].handler = handler;
            return;
        }
    }
    if (handler_count >= MAX_HANDLERS)
        slap_panic("on: too many event handlers (max %d)", MAX_HANDLERS);
    handlers[handler_count++] = (EventHandler){event_name, handler};
}

static void p_show(void) {
    wasm_render_fn = pop();
    EXPECT(wasm_render_fn, T_TUPLE);
    wasm_model = pop();
    wasm_running = true;
}

static void register_console_prims(void) {
    prim_table[sym_intern("clear")] = p_clear;
    prim_table[sym_intern("pixel")] = p_pixel;
    prim_table[sym_intern("millis")] = p_millis;
    prim_table[sym_intern("on")] = p_on;
    prim_table[sym_intern("show")] = p_show;
}

EMSCRIPTEN_KEEPALIVE
int slap_init(const char *src_code) {
    use_color = false;
    panic_stack_printer = print_stack_top;
    srand(42);
    global_scope = scope_new(NONE);
    g_scope = global_scope;
    init_primitives();
    register_console_prims();

    int prelude_count = parse_source(prelude);
    eval(0, prelude_count, global_scope);
    resolve_cached_prims(0, prelude_count);

    user_src = src_code;
    src = src_code; src_pos = 0; src_line = 1; src_col = 1;
    advance();
    int user_start = node_count;
    parse_body(TOK_EOF);
    int user_len = node_count - user_start;
    if (!tc_run(user_start, user_len)) return 1;

    resolve_cached_prims(user_start, user_len);
    eval(user_start, user_len, global_scope);
    return wasm_running ? 0 : 2;
}

EMSCRIPTEN_KEEPALIVE
void slap_frame(void) {
    if (!wasm_running) return;

    uint32_t sym_tick = sym_intern("tick");
    EventHandler *tick_h = find_handler(sym_tick);
    if (tick_h) {
        push(VAL_INT(wasm_frame));
        push(wasm_model);
        exec_tuple(val_clone(tick_h->handler));
        wasm_model = pop();
    }

    memset(canvas, 0, CANVAS_SIZE);
    Val snap = val_is_linear(wasm_model)
        ? linear_snapshot(wasm_model) : val_clone(wasm_model);
    push(snap);
    exec_tuple(val_clone(wasm_render_fn));
    wasm_frame++;
}

EMSCRIPTEN_KEEPALIVE
void slap_keydown(int keycode) {
    if (!wasm_running) return;
    uint32_t sym_kd = sym_intern("keydown");
    EventHandler *h = find_handler(sym_kd);
    if (h) {
        push(VAL_INT((int64_t)keycode));
        push(wasm_model);
        exec_tuple(val_clone(h->handler));
        wasm_model = pop();
    }
}

EMSCRIPTEN_KEEPALIVE
uint8_t *slap_canvas_ptr(void) { return canvas; }
