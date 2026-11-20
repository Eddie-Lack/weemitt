#ifndef BANKS_H
#define BANKS_H

#include "common.h"
#include "actor.h"

typedef struct TextureBank {
    /* 0x00 */ s32 textures;
    /* 0x04 */ s32 data;
    /* 0x08 */ s32 count;
} TextureBank; /* size = 0xC */

extern TextureBank textureBanks[5];

typedef struct ObjectBankEntry {
    /* 0x00 */ u32 crc;
    /* 0x04 */ PTR32(ObjectInstance) inst;
} ObjectBankEntry; /* size = 0x8 */

typedef struct ObjectBank {
    /* 0x00 */ s32 unk00;
    /* 0x04 */ PTR32(void) data;
    /* 0x08 */ s32 count;
    /* 0x0C */ PTR32(ObjectBankEntry) entries;
} ObjectBank; /* size = 0x10 */

extern ObjectBank objectBanks[5];

void CycleTextures(void);
/* Stores the loaded object with this CRC in *objCPtr, a slot in the game's memory. */
void FindObject(PTR32(ObjectInstance) *objCPtr, u32 objID, char *name, s32 vital);
#if defined(__cplusplus) && !defined(PORT_PTR32_ALIAS)
extern "C++" {
/* The same for a slot that holds a plain pointer. On the normal builds a plain pointer is its own word. */
inline void FindObject(ObjectInstance **objCPtr, u32 objID, char *name, s32 vital) {
    FindObject(PORT_SLOT(ObjectInstance, objCPtr), objID, name, vital);
}
}
#endif
Object * FindSubObjectFromObject(Object *obj, u32 objID);
void * LoadLandscapeFile(s32 num);
void WriteObjectDisplayListFlat(Object *obj);
void WriteObjectDisplayListGouraud(Object *obj);
void StopDrawing(char *name);
void StartDrawing(char *name);

#endif
