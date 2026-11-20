/* The checks of the player's ROM (rom.h). */
#include <stdio.h>
#include <string.h>
#include "rom.h"

/* Glover (USA) is a 64-megabit ROM. The game reads its data up to the end without checking the size. */
#define ROM_SIZE 0x800000

int port_rom_prepare(unsigned char *rom, unsigned long size, char *err, int errSize) {
    unsigned char t;
    unsigned long i;

    if (size < 0x100000 || size > 0x4000000) {
        snprintf(err, errSize, "is not an N64 ROM");
        return -1;
    }
    if (rom[0] == 0x37 && rom[1] == 0x80) {
        for (i = 0; i + 1 < size; i += 2) {
            t = rom[i];
            rom[i] = rom[i + 1];
            rom[i + 1] = t;
        }
    } else if (rom[0] == 0x40 && rom[1] == 0x12) {
        for (i = 0; i + 3 < size; i += 4) {
            t = rom[i];
            rom[i] = rom[i + 3];
            rom[i + 3] = t;
            t = rom[i + 1];
            rom[i + 1] = rom[i + 2];
            rom[i + 2] = t;
        }
    }
    if (rom[0] != 0x80 || rom[1] != 0x37) {
        snprintf(err, errSize, "is not an N64 ROM");
        return -1;
    }
    if (memcmp(rom + 0x3B, "NGVE", 4) != 0) {
        snprintf(err, errSize, "is not Glover (USA). The port needs the North American ROM (game code NGVE).");
        return -1;
    }
    if (size < ROM_SIZE) {
        snprintf(err, errSize, "is too short: it has %lu bytes, and Glover (USA) has %d", size, ROM_SIZE);
        return -1;
    }
    return 0;
}
