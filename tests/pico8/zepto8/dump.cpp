#include <lol/msg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "zepto8.h"
#include "pico8/vm.h"

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: dump cart.p8 frames\n"); return 2; }
    int n = atoi(argv[2]);
    auto vm = std::make_unique<z8::pico8::vm>();
    vm->load(argv[1]);
    vm->run();
    for (int i = 0; i < n; i++)
        if (!vm->step(1.f / 60.f)) { fprintf(stderr, "dump: cart stopped after %d of %d steps\n", i, n); break; }
    auto [ram, size] = vm->ram();
    if (size < 0x8000) { fprintf(stderr, "dump: ram too small\n"); return 1; }
    for (int y = 0; y < 128; y++)
    {
        for (int x = 0; x < 64; x++)
        {
            uint8_t b = ram[0x6000 + y * 64 + x];
            printf("%x%x", b & 15, b >> 4);
        }
        putchar(0x0a);
    }
    return 0;
}
