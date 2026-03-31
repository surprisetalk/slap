// slap SDL frontend — fantasy console with 640x480 2-bit grayscale canvas
#include "slap.c"
#include <SDL.h>

#define CANVAS_W 640
#define CANVAS_H 480
#define CANVAS_SIZE (CANVAS_W * CANVAS_H)

static uint8_t canvas[CANVAS_SIZE];

#define MAX_HANDLERS 16
typedef struct { uint32_t event_sym; Val handler; } EventHandler;
static EventHandler handlers[MAX_HANDLERS];
static int handler_count = 0;
static bool test_mode_flag = false;

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
    push(VAL_INT((int64_t)SDL_GetTicks()));
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

static EventHandler *find_handler(uint32_t sym) {
    for (int i = 0; i < handler_count; i++)
        if (handlers[i].event_sym == sym) return &handlers[i];
    return NULL;
}

static void p_show(void) {
    Val render_fn = pop();
    EXPECT(render_fn, T_TUPLE);
    Val model = pop();

    uint32_t sym_keydown = sym_intern("keydown");
    uint32_t sym_tick = sym_intern("tick");

    if (test_mode_flag) {
        EventHandler *tick_h = find_handler(sym_tick);
        if (tick_h) {
            push(VAL_INT(0));
            push(model);
            exec_tuple(val_clone(tick_h->handler));
            model = pop();
        }
        memset(canvas, 0, CANVAS_SIZE);
        Val snap = val_is_linear(model) ? linear_snapshot(model) : val_clone(model);
        push(snap);
        exec_tuple(val_clone(render_fn));
        val_free(render_fn);
        val_free(model);
        for (int i = 0; i < handler_count; i++) val_free(handlers[i].handler);
        handler_count = 0;
        return;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        slap_panic("show: SDL_Init failed: %s", SDL_GetError());
    SDL_Window *win = SDL_CreateWindow("slap",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CANVAS_W, CANVAS_H, 0);
    if (!win) slap_panic("show: SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) slap_panic("show: SDL_CreateRenderer failed: %s", SDL_GetError());
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, CANVAS_W, CANVAS_H);

    static const uint32_t palette[4] = {
        0xFF000000, 0xFF555555, 0xFFAAAAAA, 0xFFFFFFFF
    };
    uint32_t *pixels = malloc(CANVAS_SIZE * sizeof(uint32_t));
    int64_t frame = 0;

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = false; break; }
            if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE) { running = false; break; }
                EventHandler *h = find_handler(sym_keydown);
                if (h) {
                    push(VAL_INT((int64_t)ev.key.keysym.sym));
                    push(model);
                    exec_tuple(val_clone(h->handler));
                    model = pop();
                }
            }
        }
        if (!running) break;

        EventHandler *tick_h = find_handler(sym_tick);
        if (tick_h) {
            push(VAL_INT(frame));
            push(model);
            exec_tuple(val_clone(tick_h->handler));
            model = pop();
        }

        memset(canvas, 0, CANVAS_SIZE);
        Val snap = val_is_linear(model) ? linear_snapshot(model) : val_clone(model);
        push(snap);
        exec_tuple(val_clone(render_fn));

        for (int i = 0; i < CANVAS_SIZE; i++)
            pixels[i] = palette[canvas[i] & 3];
        SDL_UpdateTexture(tex, NULL, pixels, CANVAS_W * sizeof(uint32_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        frame++;
    }

    free(pixels);
    val_free(render_fn);
    val_free(model);
    for (int i = 0; i < handler_count; i++) val_free(handlers[i].handler);
    handler_count = 0;
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    exit(0);
}

static void register_console_prims(void) {
    prim_table[sym_intern("clear")] = p_clear;
    prim_table[sym_intern("pixel")] = p_pixel;
    prim_table[sym_intern("millis")] = p_millis;
    prim_table[sym_intern("on")] = p_on;
    prim_table[sym_intern("show")] = p_show;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open '%s'\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: slap <file.slap> [--test]\n"); return 1; }
    use_color = isatty(STDERR_FILENO);
    panic_stack_printer = print_stack_top;
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--test") == 0) test_mode_flag = true;

    srand((unsigned)time(NULL));
    global_scope = scope_new(NONE);
    g_scope = global_scope;
    init_primitives();
    register_console_prims();

    int prelude_count = parse_source(prelude);
    eval(0, prelude_count, global_scope);
    resolve_cached_prims(0, prelude_count);

    char *code = read_file(argv[1]);
    user_src = code;
    src = code; src_pos = 0; src_line = 1; src_col = 1;
    advance();
    int user_start = node_count;
    parse_body(TOK_EOF);
    int user_len = node_count - user_start;
    if (!tc_run(user_start, user_len)) { free(code); return 1; }

    resolve_cached_prims(user_start, user_len);
    eval(user_start, user_len, global_scope);

    free(code);
    return 0;
}
