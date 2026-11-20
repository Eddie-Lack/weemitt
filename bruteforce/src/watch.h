/* Values of the game's memory named in text: "NAME[+OFFSET]:TYPE" or "0xADDRESS:TYPE".

   NAME is a game global (gloveworld's names, engine_symbol). TYPE is one of u8 s8 u16 s16 u32 s32 f32 ptr. A ptr is
   read as an N64 address. With "NAME->OFFSET" the value at OFFSET is read from where the pointer at NAME points, for
   example "hand+0xfc->0x0:s32" (the glove's action, hand.state->mode). */
#ifndef WATCH_H
#define WATCH_H

#include <stddef.h>
#include <stdint.h>

#include "engine.h"

typedef enum WatchType { W_U8, W_S8, W_U16, W_S16, W_U32, W_S32, W_F32, W_PTR } WatchType;

typedef struct Watch {
    char text[64];
    uint32_t addr;  /* N64 address, or of the pointer when deref is set */
    int deref;
    uint32_t offset; /* after the pointer */
    WatchType type;
} Watch;

/* Parses one value. Returns 0, or -1 with a message in err. */
int watch_parse(Watch *w, const char *text, char *err, size_t errSize);
/* Parses a comma-separated list. Returns the count, or -1. */
int watch_parse_list(Watch *list, int max, const char *text, char *err, size_t errSize);
/* The value's bits, sign-extended to 32 for the signed types; 0 when a pointer on the way is NULL. */
uint32_t watch_read(const Engine *e, const Watch *w);
/* The value as text: decimal for integers, a decimal and the bits for a float, hex for a pointer. */
void watch_format(const Watch *w, uint32_t bits, char *buf, size_t size);
/* Writes a value (not through a pointer; not a ptr). Returns 0 or -1. */
int watch_write(Engine *e, const Watch *w, uint32_t bits);
/* The value as a number. */
double watch_value(const Watch *w, uint32_t bits);

#endif
