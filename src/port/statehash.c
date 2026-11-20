/* The hash of the game's memory that the headless mode prints (--hash, --hash-every) and the recordings hold
   (statehash.h). */
#include "ptr32.h"
#include "statehash.h"

extern unsigned char port_ram[];

/* A hash of the game's RDRAM copy and heap, a word at a time. A pointer into the game's memory is a Ptr32 word
   (ptr32.h), and the hash counts it as its offset. On the normal builds the game's memory also holds pointers to
   the program's code (the function pointers in tables), which move whenever the program changes. The hash counts
   any such pointer, and on the encoded builds a function's handle, as one marker value. Two builds with the same
   logic then give the same hash. */
#if defined(PORT_PTR_ENCODED)
/* On the encoded builds a word of the game's memory is never a host address. The range is the baseline build's
   image, and the hash is the same as the normal Linux build's. */
#define IMAGE_START 0x08048000u
#define IMAGE_END 0x0d80110cu
#elif defined(__linux__)
extern char __executable_start[];
#define IMAGE_START ((unsigned int)__executable_start)
/* The image ends where the baseline build's did (its _end is 0x0d80110c, with the image at 0x08048000). The
   program's own _end moves whenever a global is added, and a word of game data that happens to hold an address
   between the two would then hash differently. */
#define IMAGE_END (IMAGE_START + (0x0d80110cu - 0x08048000u))
#elif defined(_WIN32)
#include <windows.h>
extern IMAGE_DOS_HEADER __ImageBase;
#define IMAGE_START ((unsigned int)&__ImageBase)
#define IMAGE_END (IMAGE_START + image_size())

/* The compiler sees __ImageBase as a single header and warns about reading the headers that follow it. */
#pragma GCC diagnostic ignored "-Warray-bounds"
static unsigned int image_size(void) {
    const char *base = (const char *)&__ImageBase;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);

    return nt->OptionalHeader.SizeOfImage;
}
#else
#define IMAGE_START 0u
#define IMAGE_END 0u
#endif

unsigned int port_hash_ram(void) {
    const unsigned int *p = (const unsigned int *)port_ram;
    unsigned int ramStart = PORT_PTR_BASE;
    unsigned int ramEnd = ramStart + 0x1800000;
    unsigned int h = 2166136261u;
    unsigned int i;

    for (i = 0; i < 0x1800000 / 4; i++) {
        unsigned int w = p[i];

        if (w >= ramStart && w < ramEnd) {
            w = (w - ramStart) ^ 0xA5000000u;
        } else if (w >= IMAGE_START && w < IMAGE_END) {
            w = 0xC0DEC0DEu;
        }
#ifdef PORT_PTR_ENCODED
        else if (w >= PORT_FN_BASE && w < PORT_FN_BASE + 4 * port_fn_count) {
            /* A function's word counts as a pointer into the program, as on the normal builds. */
            w = 0xC0DEC0DEu;
        }
#endif
        h = (h ^ w) * 16777619u;
    }
    return h;
}
