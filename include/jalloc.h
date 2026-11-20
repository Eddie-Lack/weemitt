#ifndef JALLOC_H
#define JALLOC_H

#include "types.h"

#define JALLOC_NUM_BLOCKS 0x500

typedef struct JallocBlock {
    /* 0x00 */ u32 addr;
    /* 0x04 */ s32 size;
    /* 0x08 */ char name[11];
    /* 0x13 */ u8 used;
} JallocBlock; /* size = 0x14 */

typedef struct JallocControl {
    /* 0x0000 */ JallocBlock blocks[JALLOC_NUM_BLOCKS];
    /* 0x6400 */ u32 heapStart;
    /* 0x6404 */ s32 heapSize;
    /* 0x6408 */ s32 lastStatic;
    /* 0x640C */ s32 firstDynamic;
    /* 0x6410 */ s32 staticUsed;
    /* 0x6414 */ s32 dynamicUsed;
} JallocControl;

extern JallocControl jallocControl;

void memclear(void *ptr, s32 size);
void JallocInit(u32 start, u32 size);
void *JallocAlloc(s32 size, s32 clear, char *name);
void *JallocAllocStatic(s32 size, s32 clear, char *name);
void *JallocAllocDynamic(s32 size, s32 clear, char *name);
/* ptrRef points to a 32-bit word that holds the block's address: a Ptr32 (ptr32.h). The word becomes 0. */
s32 JallocFreeStatic(void *ptrRef);
s32 JallocFreeDynamic(void *ptrRef);
void JallocFree(void *ptrRef);
#if defined(__cplusplus) && defined(PORT_PTR_ENCODED) && !defined(PORT_PTR32_ALIAS)
extern "C++" {
/* The same for a slot that holds a plain pointer, such as a local. On the normal builds a plain pointer is its own
   word and needs none of this. */
template <class T> inline s32 JallocFreeStatic(T **ptrRef) {
    Ptr32<T> slot;
    s32 result;

    slot = *ptrRef;
    result = JallocFreeStatic((void *)&slot);
    *ptrRef = slot;
    return result;
}
template <class T> inline s32 JallocFreeDynamic(T **ptrRef) {
    Ptr32<T> slot;
    s32 result;

    slot = *ptrRef;
    result = JallocFreeDynamic((void *)&slot);
    *ptrRef = slot;
    return result;
}
template <class T> inline void JallocFree(T **ptrRef) {
    Ptr32<T> slot;

    slot = *ptrRef;
    JallocFree((void *)&slot);
    *ptrRef = slot;
}
}
#endif
void JallocReset(void);
void ShowJalloc(void);

s32 DMAGetSize(u32 start, u32 end);
void DMAMemory(void *dest, u32 start, u32 end);

#endif
