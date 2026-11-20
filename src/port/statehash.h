/* The hash of the game's memory (statehash.c). */
#ifndef STATEHASH_H
#define STATEHASH_H

/* A hash of the game's RDRAM copy and heap (port_ram, 24 MB). Two builds with the same logic give the same hash. */
unsigned int port_hash_ram(void);

#endif
