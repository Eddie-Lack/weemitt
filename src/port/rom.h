/* The checks of the player's ROM (rom.c). Plain C types only: the platform layer and the library use it. */
#ifndef ROM_H
#define ROM_H

/* Puts the ROM in big-endian (.z64) byte order in place: a .v64 file swaps each pair of bytes, a .n64 file each
   word. Then checks that it is Glover (USA, game code NGVE) and has all its 8 MB. Returns 0, or -1 with the reason
   in err. The reason follows the ROM's name: "is not an N64 ROM". */
int port_rom_prepare(unsigned char *rom, unsigned long size, char *err, int errSize);

#endif
