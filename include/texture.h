#ifndef TEXTURE_H
#define TEXTURE_H

#include "common.h"

typedef struct Texture {
    /* 0x00 */ u32 crc;
    /* 0x04 */ u8 cycleStart;
    /* 0x05 */ u8 cycleEnd;
    /* 0x06 */ s16 flags;
    /* 0x08 */ s16 cycleSpeed;
    /* 0x0A */ s16 cycleCount;
    /* 0x0C */ u16 width;
    /* 0x0E */ u16 height;
    /* 0x10 */ u16 maskS;
    /* 0x12 */ u16 maskT;
    /* 0x14 */ s32 size;
    /* 0x18 */ u16 fmt;
    /* 0x1A */ u16 siz;
    /* 0x1C */ PTR32(u8) data;
    /* 0x20 */ PTR32(s32) palette;
} Texture;

void LoadTexture(Texture *tex);
/* Stores the loaded texture with this CRC in *texPtr, or NULL when there is none. */
void FindTexture(PTR32(Texture) *texPtr, u32 texID, s32 verbose);
#ifdef __cplusplus
extern "C++" {
/* The same for a slot that holds a plain pointer. On the normal builds a plain pointer is its own word. */
template <class T> inline void FindTexture(T **texPtr, u32 texID, s32 verbose) {
#if defined(PORT_PTR_ENCODED) && !defined(PORT_PTR32_ALIAS)
    Ptr32<Texture> slot;

    FindTexture(&slot, texID, verbose);
    *texPtr = (T *)(Texture *)slot;
#else
    FindTexture((PTR32(Texture) *)texPtr, texID, verbose);
#endif
}
#ifndef PORT_PTR32_ALIAS
/* A Ptr32 slot of another type: every Ptr32 holds the same kind of word. */
template <class T> inline void FindTexture(Ptr32<T> *texPtr, u32 texID, s32 verbose) {
    FindTexture((Ptr32<Texture> *)texPtr, texID, verbose);
}
#endif
}
#endif

#endif
