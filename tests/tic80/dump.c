// Runs a TIC-80 Lua cart headless and prints its screen: `dump cart.lua frames`.
// Output: one line per trace() call, then 136 lines of 240 hex digits, one per
// pixel, read from VRAM. The cart is the code chunk only, so the palette is the
// default (sweetie16, chunk DEFAULT). Built by tests/tic80/build.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/core.h"

static u64 now;
static u64 counter(void *d) { (void)d; return now; }
static u64 freq(void *d) { (void)d; return 60000; }
static void trace(const char *s, u8 c) { (void)c; puts(s); }
static void error(const char *s) { fprintf(stderr, "tic80 error: %s\n", s); exit(2); }

int main(int argc, char **argv) {
  if (argc != 3) { fprintf(stderr, "usage: dump cart.lua frames\n"); return 1; }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 1; }
  static u8 buf[4 + 4 + 65536];
  // Chunk header: type (bits 0-4), bank (5-7), size u16 LE, one spare byte.
  buf[0] = 17; // CHUNK_DEFAULT: sweetie16 and the default waveforms
  size_t n = fread(buf + 8, 1, 65536, f);
  if (n == 0 || n >= 65536) { fprintf(stderr, "%s: code must be 1..65535 bytes, got %zu\n", argv[1], n); return 1; }
  buf[4] = 5; // CHUNK_CODE
  buf[5] = n & 255; buf[6] = n >> 8;
  tic80 *t = tic80_create(44100, TIC80_PIXEL_COLOR_RGBA8888);
  t->callback.trace = trace;
  t->callback.error = error;
  tic80_load(t, buf, 8 + n);
  tic80_input in;
  memset(&in, 0, sizeof in);
  int frames = atoi(argv[2]);
  for (int i = 0; i < frames; i++) {
    now = (u64)i * 1000; // freq 60000: one tick is 1/60 s
    tic80_tick(t, in, counter, freq);
  }
  tic_mem *m = (tic_mem *)t;
  for (int y = 0; y < TIC80_HEIGHT; y++) {
    for (int x = 0; x < TIC80_WIDTH; x++) putchar("0123456789abcdef"[tic_tool_peek4(m->ram->vram.screen.data, y * TIC80_WIDTH + x)]);
    putchar('\n');
  }
  return 0;
}
