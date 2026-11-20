PORT_C_BEGIN
#include "common.h"
#include "ident.h"
#include "debug.h"
#include "actor.h"
#include "game.h"
#include "texture.h"
#include "banks.h"
#include "display.h"
#include "jalloc.h"

extern s32 numCyclingTextures;
extern s32 numoftc;
extern PTR32(u32) ballTextureData;
extern PTR32(u32) ballTextureData2;
extern PTR32(u32) ballTexturePalette;
extern PTR32(u32) ballTexturePalette2;
extern PTR32(u32) ballBitTextureData;
extern PTR32(u32) ballBitTexturePalette;
extern PTR32(u32) handTextureData[2];
extern PTR32(u32) handTexturePalette[2];
extern PTR32(Texture) cyclingTextures[20];
extern u8 codeDrawingRequest;
extern u8 gfxIsDrawing;
extern u8 disableGraphics;
extern u8 onlyDrawBackdrops;
extern u8 recordKeyFilesRomStart[];
extern u8 landscapeFilesRomStart[];
extern u8 landscapeFilesRomEnd[];
extern u8 recordKeyFilesRomEnd[];
extern OSIoMesg dmaIOMessageBuf;
extern OSMesgQueue dmaMessageQ;
extern char D_80106E78[]; // format string followed by linker junk
extern char D_80106864[]; // "Whoops, texture '%X' not found!\n" followed by linker junk

extern u8 titleTexBankRomStart[];
extern u8 genericTexBankRomStart[];
extern u8 atlantisTexBankRomStart[];
extern u8 carnivalTexBankRomStart[];
extern u8 piratesTexBankRomStart[];
extern u8 prehistoricTexBankRomStart[];
extern u8 fortressTexBankRomStart[];
extern u8 ootwTexBankRomStart[];
extern u8 hubTexBankRomStart[];
extern u8 levelEndTexBankRomStart[];
extern u8 fontTexBankRomStart[];
extern u8 cameoTexBankRomStart[];
extern u8 levelEnd2TexBankRomStart[];
extern u8 presentTexBankRomStart[];
extern u8 flythruTexBankRomStart[];
extern u8 ckBonusTexBankRomStart[];
extern u8 ckCheat1TexBankRomStart[];
extern u8 ckCheat2TexBankRomStart[];
extern u8 ckCheat2TexBankRomEnd[];
extern u8 rcDemoTexBankRomStart[];
extern u8 genericObjBankRomStart[];
extern u32 sysfont[];
extern char outputMessageBuffer[];
extern char message[];
extern u8 atlantisSharedObjBankRomStart[];
extern u8 atlantisL1ObjBankRomStart[];
extern u8 atlantisL2ObjBankRomStart[];
extern u8 atlantisL3aObjBankRomStart[];
extern u8 atlantisBossObjBankRomStart[];
extern u8 atlantisBonusObjBankRomStart[];
extern u8 carnivalSharedObjBankRomStart[];
extern u8 carnivalL1ObjBankRomStart[];
extern u8 carnivalL2aObjBankRomStart[];
extern u8 carnivalL3aObjBankRomStart[];
extern u8 carnivalBossObjBankRomStart[];
extern u8 carnivalBonusObjBankRomStart[];
extern u8 piratesSharedObjBankRomStart[];
extern u8 piratesL1ObjBankRomStart[];
extern u8 piratesL2aObjBankRomStart[];
extern u8 piratesBossObjBankRomStart[];
extern u8 piratesBonusObjBankRomStart[];
extern u8 prehistoricSharedObjBankRomStart[];
extern u8 prehistoricL1aObjBankRomStart[];
extern u8 prehistoricL2aObjBankRomStart[];
extern u8 prehistoricBossObjBankRomStart[];
extern u8 prehistoricBonusObjBankRomStart[];
extern u8 fortressSharedObjBankRomStart[];
extern u8 fortressL1aObjBankRomStart[];
extern u8 fortressL2aObjBankRomStart[];
extern u8 fortressL3bObjBankRomStart[];
extern u8 fortressBonusObjBankRomStart[];
extern u8 fortressBossObjBankRomStart[];
extern u8 hubSharedObjBankRomStart[];
extern u8 hubPart1ObjBankRomStart[];
extern u8 piratesL3bObjBankRomStart[];
extern u8 prehistoricL3bObjBankRomStart[];
extern u8 ootwL2aObjBankRomStart[];
extern u8 ootwL2bObjBankRomStart[];
extern u8 ootwL3ObjBankRomStart[];
extern u8 ootwBoss1ObjBankRomStart[];
extern u8 tweenObjBankRomStart[];
extern u8 ootwBonusObjBankRomStart[];
extern u8 ootwSharedObjBankRomStart[];
extern u8 caveObjBankRomStart[];
extern u8 introObjBankRomStart[];
extern u8 wayroomObjBankRomStart[];
extern u8 hubPart2ObjBankRomStart[];
extern u8 hubPart3ObjBankRomStart[];
extern u8 hubPart4ObjBankRomStart[];
extern u8 hubPart5ObjBankRomStart[];
extern u8 hubPart6ObjBankRomStart[];
extern u8 hubPart7ObjBankRomStart[];
extern u8 hubPart8ObjBankRomStart[];
extern u8 assaultCourseObjBankRomStart[];
extern u8 titleObjBankRomStart[];
extern u8 outroObjBankRomStart[];
extern u8 systemObjBankRomStart[];
extern u8 presentationObjBankRomStart[];
extern u8 flythruObjBankRomStart[];
extern PTR32(Vtx) vtxPtr;
extern s16 xluFact;
extern u8 mirrorTextures;

void InitTextureBanks(void) {
    s32 i;

    for (i = 0; i < 5; i++) {
        textureBanks[i].data = 0;
        textureBanks[i].textures = 0;
        textureBanks[i].count = 0;
    }
    numCyclingTextures = 0;
    for (i = 0; i < 20; i++) {
        cyclingTextures[i] = NULL;
    }
}

void LoadTextureBank(u32 num) {
    u32 *ptr;
    u32 start;
    u32 end;
    s32 size;
    s16 slot = -1;
    s16 i = 0;
    Texture *tex;
    Texture *cur;

    switch (num) {
        case 18:
            start = PORT_ROM_ADDR(rcDemoTexBankRomStart);
            end = PORT_ROM_ADDR(genericObjBankRomStart);
            sprintf(message, "RCDEMO_TEX_BANK");
            break;
        case CKBONUS_TEX_BANK:
            start = PORT_ROM_ADDR(ckBonusTexBankRomStart);
            end = PORT_ROM_ADDR(ckCheat1TexBankRomStart);
            sprintf(message, "CKBONUS_TEX_BANK");
            break;
        case 16:
            start = PORT_ROM_ADDR(ckCheat1TexBankRomStart);
            end = PORT_ROM_ADDR(ckCheat2TexBankRomStart);
            sprintf(message, "CKCHEAT1_TEX_BANK");
            break;
        case 17:
            start = PORT_ROM_ADDR(ckCheat2TexBankRomStart);
            end = PORT_ROM_ADDR(ckCheat2TexBankRomEnd);
            sprintf(message, "CKCHEAT2_TEX_BANK");
            break;
        case FONT_TEX_BANK:
            start = PORT_ROM_ADDR(fontTexBankRomStart);
            end = PORT_ROM_ADDR(cameoTexBankRomStart);
            sprintf(message, "FONT_TEX_BANK");
            slot = 0;
            break;
        case HUB_TEX_BANK:
            start = PORT_ROM_ADDR(hubTexBankRomStart);
            end = PORT_ROM_ADDR(levelEndTexBankRomStart);
            sprintf(message, "HUB_TEX_BANK");
            break;
        case CAMEO_TEX_BANK:
            start = PORT_ROM_ADDR(cameoTexBankRomStart);
            end = PORT_ROM_ADDR(levelEnd2TexBankRomStart);
            sprintf(message, "CAMEO_TEX_BANK");
            break;
        case TITLE_TEX_BANK:
            start = PORT_ROM_ADDR(titleTexBankRomStart);
            end = PORT_ROM_ADDR(genericTexBankRomStart);
            sprintf(message, "TITLE_TEX_BANK");
            break;
        case LEVEND1_TEX_BANK:
            start = PORT_ROM_ADDR(levelEndTexBankRomStart);
            end = PORT_ROM_ADDR(fontTexBankRomStart);
            sprintf(message, "LEVELEND_TEX_BANK");
            break;
        case LEVEND2_TEX_BANK:
            start = PORT_ROM_ADDR(levelEnd2TexBankRomStart);
            end = PORT_ROM_ADDR(presentTexBankRomStart);
            sprintf(message, "LEVELEND_TEX_BANK");
            break;
        case GENERIC_TEX_BANK:
            start = PORT_ROM_ADDR(genericTexBankRomStart);
            end = PORT_ROM_ADDR(atlantisTexBankRomStart);
            sprintf(message, "GENERIC_TEX_BANK");
            break;
        case ATLANTIS_TEX_BANK:
            start = PORT_ROM_ADDR(atlantisTexBankRomStart);
            end = PORT_ROM_ADDR(carnivalTexBankRomStart);
            sprintf(message, "ATLANTIS_TEX_BANK");
            break;
        case CARNIVAL_TEX_BANK:
            start = PORT_ROM_ADDR(carnivalTexBankRomStart);
            end = PORT_ROM_ADDR(piratesTexBankRomStart);
            sprintf(message, "CARNIVAL_TEX_BANK");
            break;
        case PIRATES_TEX_BANK:
            start = PORT_ROM_ADDR(piratesTexBankRomStart);
            end = PORT_ROM_ADDR(prehistoricTexBankRomStart);
            sprintf(message, "PIRATES_TEX_BANK");
            break;
        case PREHISTORIC_TEX_BANK:
            start = PORT_ROM_ADDR(prehistoricTexBankRomStart);
            end = PORT_ROM_ADDR(fortressTexBankRomStart);
            sprintf(message, "PREHISTORIC_TEX_BANK");
            break;
        case FORTRESS_TEX_BANK:
            start = PORT_ROM_ADDR(fortressTexBankRomStart);
            end = PORT_ROM_ADDR(ootwTexBankRomStart);
            sprintf(message, "FORTRESS_TEX_BANK");
            break;
        case OOTW_TEX_BANK:
            start = PORT_ROM_ADDR(ootwTexBankRomStart);
            end = PORT_ROM_ADDR(hubTexBankRomStart);
            sprintf(message, "OOTW_TEX_BANK");
            break;
        case PRESENT_TEX_BANK:
            start = PORT_ROM_ADDR(presentTexBankRomStart);
            end = PORT_ROM_ADDR(flythruTexBankRomStart);
            sprintf(message, "PRESENT_TEX_BANK");
            break;
        case FLYTHRU_TEX_BANK:
            start = PORT_ROM_ADDR(flythruTexBankRomStart);
            end = PORT_ROM_ADDR(ckBonusTexBankRomStart);
            sprintf(message, "FLYTHRU_TEX_BANK");
            break;
        default:
            PRINTF("Invalid texture bank!\n");
            Crash("LOAD TEXTURE BANK");
            break;
    }
    size = end - start;
    if (num != FONT_TEX_BANK) {
        ptr = (u32 *)JallocAlloc(DMAGetSize(start, end), 0, "TexBank");
    } else {
        ptr = sysfont;
    }
    port_load_kind = PORT_LOAD_TEXTURE_BANK;
    DMAMemory(ptr, start, end);
    if (ptr != NULL) {
        PRINTF("Loaded texture bank %s [%d]\n", message, size);
    } else {
        PRINTF("Unable to load texture bank %s\n", message);
        return;
    }
    if (num != FONT_TEX_BANK) {
        while (textureBanks[++slot].data) {
        }
        if (slot >= 5) {
            PRINTF("ERROR:too many texture banks downloaded\n");
            return;
        }
    }
    textureBanks[slot].data = PORT_P2W(ptr);
    textureBanks[slot].count = *ptr;
    ptr += 1;
    textureBanks[slot].textures = PORT_P2W(ptr);
    cur = PORT_W2P(Texture, textureBanks[slot].textures);
    for (i = 0; i < textureBanks[slot].count; i++) {
        tex = cur;
        tex->data = (u8 *)tex + sizeof(Texture);
        if (tex->palette) {
            tex->palette = (s32 *)((u8 *)tex + (u32)tex->palette);
        }
        cur = (Texture *)((u8 *)cur + tex->size);
        if (tex->flags & 4) {
            cyclingTextures[numCyclingTextures++] = tex;
        }
    }
}

void FindTexture(PTR32(Texture) *texPtr, u32 texID, s32 verbose) {
    s32 i;
    s32 j;
    Texture *tex;
    u8 *p;

    for (i = 4; i >= 0; i--) {
        if (textureBanks[i].data) {
            p = PORT_W2P(u8, textureBanks[i].textures);
            for (j = 0; j < textureBanks[i].count; j++) {
                tex = (Texture *)p;
                if (tex->crc == texID) {
                    *texPtr = tex;
                    return;
                }
                p += tex->size;
            }
        }
    }
    *texPtr = NULL;
    if (verbose) {
        PRINTF(D_80106864, texID);
    }
    return; // forces the original jump to the epilogue
}

void CycleTextures(void) {
    s32 i;
    s32 j;
    Texture *tex;
    u16 *pal;
    u16 tmp;

    for (i = 0; i < numCyclingTextures; i++) {
        tex = cyclingTextures[i];
        pal = (u16 *)tex->palette;
        tex->cycleCount += (tex->cycleSpeed >= 0) ? tex->cycleSpeed : -tex->cycleSpeed;
        while (tex->cycleCount >= 64) {
            if (tex->cycleSpeed > 0) {
                tmp = pal[tex->cycleStart];
                for (j = tex->cycleStart; j < tex->cycleEnd; j++) {
                    pal[j] = pal[j + 1];
                }
                pal[tex->cycleEnd] = tmp;
                tex->cycleCount -= 64;
            } else {
                tmp = pal[tex->cycleEnd];
                for (j = tex->cycleEnd; j > tex->cycleStart; j--) {
                    pal[j] = pal[j - 1];
                }
                pal[tex->cycleStart] = tmp;
                tex->cycleCount -= 64;
            }
        }
    }
}

s32 Rom2Ram(s32 addr, s32 base) {
    s32 ram;

    ram = addr & 0xFFFFFF;
    ram += base;
    return ram;
}

/* RestoreDrawList: the port's version, which reads big-endian headers and converts what it loads, is in src/port/loaders.c. */
void RestoreDrawList(u8 *list, s32 base, Object *obj);

INCLUDE_RODATA("asm/nonmatchings/banks", D_80106864);

INCLUDE_RODATA("asm/nonmatchings/banks", ident_80106890);

void RestoreObjectPointers(Object *obj, s32 base) {
    void *p;
    s32 i;
    char name[16];

    p = obj->mesh;
    if (p != NULL) {
        obj->mesh = PORT_W2P(Mesh, Rom2Ram((s32)obj->mesh, base));
        obj->mesh->verts = PORT_W2P(Vec3, Rom2Ram((s32)obj->mesh->verts, base));
        if (obj->mesh->numFaces) {
            obj->mesh->faces = PORT_W2P(MeshFace, Rom2Ram((s32)obj->mesh->faces, base));
            if (obj->mesh->faceColours) {
                obj->mesh->faceColours = PORT_W2P(void, Rom2Ram((s32)obj->mesh->faceColours, base));
            }
            if (obj->mesh->uv) {
                obj->mesh->uv = PORT_W2P(MeshUV, Rom2Ram((s32)obj->mesh->uv, base));
                sprintf(name, "OFTC%d", numoftc++);
                obj->mesh->origUV = (u16 (*)[2])JallocAlloc(obj->mesh->numFaces * 12, 1, name);
                memcpy(obj->mesh->origUV, obj->mesh->uv, obj->mesh->numFaces * 12);
            }
            if (obj->mesh->faceFlags) {
                obj->mesh->faceFlags = PORT_W2P(u8, Rom2Ram((s32)obj->mesh->faceFlags, base));
            }
            if (obj->mesh->textures) {
                obj->mesh->textures = PORT_W2P(PTR32(Texture), Rom2Ram((s32)obj->mesh->textures, base));
                for (i = 0; i < obj->mesh->numFaces; i++) {
                    FindTexture(&obj->mesh->textures[i], (u32)obj->mesh->textures[i], 1);
                }
            }
        }
        if (obj->mesh->colours) {
            obj->mesh->colours = PORT_W2P(u8, Rom2Ram((s32)obj->mesh->colours, base));
        }
    }
    p = obj->sprites;
    if (p != NULL) {
        obj->sprites = PORT_W2P(ObjectSprite, Rom2Ram((s32)obj->sprites, base));
        for (i = 0; i < obj->numSprites; i++) {
            FindTexture(&obj->sprites[i].tex, (u32)obj->sprites[i].tex, 1);
        }
    }
    p = obj->displayList;
    if (p != NULL) {
        obj->displayList = PORT_W2P(u8, Rom2Ram((s32)obj->displayList, base));
    }
    p = obj->scaleKeys;
    if (p != NULL) {
        obj->scaleKeys = PORT_W2P(PosKey, Rom2Ram((s32)obj->scaleKeys, base));
    }
    p = obj->posKeys;
    if (p != NULL) {
        obj->posKeys = PORT_W2P(PosKey, Rom2Ram((s32)obj->posKeys, base));
    }
    p = obj->rotKeys;
    if (p != NULL) {
        obj->rotKeys = PORT_W2P(RotKey, Rom2Ram((s32)obj->rotKeys, base));
    }
    p = obj->child;
    if (p != NULL) {
        obj->child = PORT_W2P(Object, Rom2Ram((s32)obj->child, base));
    }
    p = obj->sibling;
    if (p != NULL) {
        obj->sibling = PORT_W2P(Object, Rom2Ram((s32)obj->sibling, base));
    }
    if (obj->displayList) {
        RestoreDrawList(obj->displayList, base, obj);
    }
    if (obj->child) {
        RestoreObjectPointers(obj->child, base);
    }
    if (obj->sibling) {
        RestoreObjectPointers(obj->sibling, base);
    }
}

void LoadObjectBank(u32 num) {
    s32 ptr;
    u32 start;
    u32 end;
    s32 size;
    s16 slot = -1;
    s16 i = 0;
    s32 rom;
    ObjectInstance *inst;

    switch (num) {
        case 71:
            start = PORT_ROM_ADDR(tweenObjBankRomStart);
            end = PORT_ROM_ADDR(ootwBonusObjBankRomStart);
            sprintf(message, "TWEEN");
            break;
        case 0:
            start = PORT_ROM_ADDR(systemObjBankRomStart);
            end = PORT_ROM_ADDR(presentationObjBankRomStart);
            sprintf(message, "SYSTEM");
            break;
        case 67:
            start = PORT_ROM_ADDR(wayroomObjBankRomStart);
            end = PORT_ROM_ADDR(hubPart2ObjBankRomStart);
            sprintf(message, "WAYROOM");
            break;
        case 66:
            start = PORT_ROM_ADDR(outroObjBankRomStart);
            end = PORT_ROM_ADDR(systemObjBankRomStart);
            sprintf(message, "OUTRO");
            break;
        case 68:
            start = PORT_ROM_ADDR(presentationObjBankRomStart);
            end = PORT_ROM_ADDR(flythruObjBankRomStart);
            sprintf(message, "PRESENTATION");
            break;
        case 69:
            start = PORT_ROM_ADDR(flythruObjBankRomStart);
            end = PORT_ROM_ADDR(landscapeFilesRomStart);
            sprintf(message, "FLYTHRU");
            break;
        case 65:
            start = PORT_ROM_ADDR(introObjBankRomStart);
            end = PORT_ROM_ADDR(wayroomObjBankRomStart);
            sprintf(message, "INTRO");
            break;
        case 64:
            start = PORT_ROM_ADDR(hubSharedObjBankRomStart);
            end = PORT_ROM_ADDR(hubPart1ObjBankRomStart);
            sprintf(message, "HUB_SHARED");
            break;
        case 2:
            start = PORT_ROM_ADDR(hubPart1ObjBankRomStart);
            end = PORT_ROM_ADDR(piratesL3bObjBankRomStart);
            sprintf(message, "HUB_PART1");
            break;
        case 3:
            start = PORT_ROM_ADDR(hubPart2ObjBankRomStart);
            end = PORT_ROM_ADDR(hubPart3ObjBankRomStart);
            sprintf(message, "HUB_PART2");
            break;
        case 4:
            start = PORT_ROM_ADDR(hubPart3ObjBankRomStart);
            end = PORT_ROM_ADDR(hubPart4ObjBankRomStart);
            sprintf(message, "HUB_PART3");
            break;
        case 5:
            start = PORT_ROM_ADDR(hubPart4ObjBankRomStart);
            end = PORT_ROM_ADDR(hubPart5ObjBankRomStart);
            sprintf(message, "HUB_PART4");
            break;
        case 6:
            start = PORT_ROM_ADDR(hubPart5ObjBankRomStart);
            end = PORT_ROM_ADDR(hubPart6ObjBankRomStart);
            sprintf(message, "HUB_PART5");
            break;
        case 7:
            start = PORT_ROM_ADDR(hubPart6ObjBankRomStart);
            end = PORT_ROM_ADDR(hubPart7ObjBankRomStart);
            sprintf(message, "HUB_PART6");
            break;
        case 8:
            start = PORT_ROM_ADDR(hubPart7ObjBankRomStart);
            end = PORT_ROM_ADDR(hubPart8ObjBankRomStart);
            sprintf(message, "HUB_PART7");
            break;
        case 9:
            start = PORT_ROM_ADDR(hubPart8ObjBankRomStart);
            end = PORT_ROM_ADDR(assaultCourseObjBankRomStart);
            sprintf(message, "HUB_PART8");
            break;
        case 10:
            start = PORT_ROM_ADDR(caveObjBankRomStart);
            end = PORT_ROM_ADDR(introObjBankRomStart);
            sprintf(message, "CAVE");
            break;
        case 11:
            start = PORT_ROM_ADDR(assaultCourseObjBankRomStart);
            end = PORT_ROM_ADDR(titleObjBankRomStart);
            sprintf(message, "ASSAULT COURSE");
            break;
        case 1:
            start = PORT_ROM_ADDR(genericObjBankRomStart);
            end = PORT_ROM_ADDR(atlantisSharedObjBankRomStart);
            sprintf(message, "GENERIC");
            break;
        case 12:
            start = PORT_ROM_ADDR(atlantisSharedObjBankRomStart);
            end = PORT_ROM_ADDR(atlantisL1ObjBankRomStart);
            sprintf(message, "ATLANTIS_SHARED");
            break;
        case 13:
            start = PORT_ROM_ADDR(atlantisL1ObjBankRomStart);
            end = PORT_ROM_ADDR(atlantisL2ObjBankRomStart);
            sprintf(message, "ATLANTIS_L1");
            break;
        case 14:
            start = PORT_ROM_ADDR(atlantisL2ObjBankRomStart);
            end = PORT_ROM_ADDR(atlantisL3aObjBankRomStart);
            sprintf(message, "ATLANTIS_L2");
            break;
        case 15:
            start = PORT_ROM_ADDR(atlantisL3aObjBankRomStart);
            end = PORT_ROM_ADDR(atlantisBossObjBankRomStart);
            sprintf(message, "ATLANTIS_L3A");
            break;
        case 17:
            start = PORT_ROM_ADDR(atlantisBossObjBankRomStart);
            end = PORT_ROM_ADDR(atlantisBonusObjBankRomStart);
            sprintf(message, "ATLANTIS_BOSS");
            break;
        case 18:
            start = PORT_ROM_ADDR(atlantisBonusObjBankRomStart);
            end = PORT_ROM_ADDR(carnivalSharedObjBankRomStart);
            sprintf(message, "ATLANTIS_BONUS");
            break;
        case 19:
            start = PORT_ROM_ADDR(carnivalSharedObjBankRomStart);
            end = PORT_ROM_ADDR(carnivalL1ObjBankRomStart);
            sprintf(message, "CARNIVAL_SHARED");
            break;
        case 20:
            start = PORT_ROM_ADDR(carnivalL1ObjBankRomStart);
            end = PORT_ROM_ADDR(carnivalL2aObjBankRomStart);
            sprintf(message, "CARNIVAL_L1");
            break;
        case 21:
            start = PORT_ROM_ADDR(carnivalL2aObjBankRomStart);
            end = PORT_ROM_ADDR(carnivalL3aObjBankRomStart);
            sprintf(message, "CARNIVAL_L2A");
            break;
        case 23:
            start = PORT_ROM_ADDR(carnivalL3aObjBankRomStart);
            end = PORT_ROM_ADDR(carnivalBossObjBankRomStart);
            sprintf(message, "CARNIVAL_L3A");
            break;
        case 25:
            start = PORT_ROM_ADDR(carnivalBossObjBankRomStart);
            end = PORT_ROM_ADDR(carnivalBonusObjBankRomStart);
            sprintf(message, "CARNIVAL_BOSS");
            break;
        case 26:
            start = PORT_ROM_ADDR(carnivalBonusObjBankRomStart);
            end = PORT_ROM_ADDR(piratesSharedObjBankRomStart);
            sprintf(message, "CARNIVAL_BONUS");
            break;
        case 38:
            start = PORT_ROM_ADDR(piratesSharedObjBankRomStart);
            end = PORT_ROM_ADDR(piratesL1ObjBankRomStart);
            sprintf(message, "PIRATES_SHARED");
            break;
        case 39:
            start = PORT_ROM_ADDR(piratesL1ObjBankRomStart);
            end = PORT_ROM_ADDR(piratesL2aObjBankRomStart);
            sprintf(message, "PIRATES_L1");
            break;
        case 40:
            start = PORT_ROM_ADDR(piratesL2aObjBankRomStart);
            end = PORT_ROM_ADDR(piratesBossObjBankRomStart);
            sprintf(message, "PIRATES_L2A");
            break;
        case 43:
            start = PORT_ROM_ADDR(piratesL3bObjBankRomStart);
            end = PORT_ROM_ADDR(prehistoricL3bObjBankRomStart);
            sprintf(message, "PIRATES_L3B");
            break;
        case 44:
            start = PORT_ROM_ADDR(piratesBossObjBankRomStart);
            end = PORT_ROM_ADDR(piratesBonusObjBankRomStart);
            sprintf(message, "PIRATES_BOSS");
            break;
        case 45:
            start = PORT_ROM_ADDR(piratesBonusObjBankRomStart);
            end = PORT_ROM_ADDR(prehistoricSharedObjBankRomStart);
            sprintf(message, "PIRATES_BONUS");
            break;
        case 46:
            start = PORT_ROM_ADDR(prehistoricSharedObjBankRomStart);
            end = PORT_ROM_ADDR(prehistoricL1aObjBankRomStart);
            sprintf(message, "PREHISTORIC_SHARED");
            break;
        case 47:
            start = PORT_ROM_ADDR(prehistoricL1aObjBankRomStart);
            end = PORT_ROM_ADDR(prehistoricL2aObjBankRomStart);
            sprintf(message, "PREHISTORIC_L1A");
            break;
        case 49:
            start = PORT_ROM_ADDR(prehistoricL2aObjBankRomStart);
            end = PORT_ROM_ADDR(prehistoricBossObjBankRomStart);
            sprintf(message, "PREHISTORIC_L2A");
            break;
        case 52:
            start = PORT_ROM_ADDR(prehistoricL3bObjBankRomStart);
            end = PORT_ROM_ADDR(ootwL2aObjBankRomStart);
            sprintf(message, "PREHISTORIC_L3B");
            break;
        case 53:
            start = PORT_ROM_ADDR(prehistoricBossObjBankRomStart);
            end = PORT_ROM_ADDR(prehistoricBonusObjBankRomStart);
            sprintf(message, "PREHISTORIC_BOSS");
            break;
        case 54:
            start = PORT_ROM_ADDR(prehistoricBonusObjBankRomStart);
            end = PORT_ROM_ADDR(fortressSharedObjBankRomStart);
            sprintf(message, "PREHISTORIC_BONUS");
            break;
        case 55:
            start = PORT_ROM_ADDR(fortressSharedObjBankRomStart);
            end = PORT_ROM_ADDR(fortressL1aObjBankRomStart);
            sprintf(message, "FORTRESS_SHARED");
            break;
        case 56:
            start = PORT_ROM_ADDR(fortressL1aObjBankRomStart);
            end = PORT_ROM_ADDR(fortressL2aObjBankRomStart);
            sprintf(message, "FORTRESS_L1A");
            break;
        case 58:
            start = PORT_ROM_ADDR(fortressL2aObjBankRomStart);
            end = PORT_ROM_ADDR(fortressL3bObjBankRomStart);
            sprintf(message, "FORTRESS_L2A");
            break;
        case 61:
            start = PORT_ROM_ADDR(fortressL3bObjBankRomStart);
            end = PORT_ROM_ADDR(fortressBonusObjBankRomStart);
            sprintf(message, "FORTRESS_L3B");
            break;
        case 63:
            start = PORT_ROM_ADDR(fortressBonusObjBankRomStart);
            end = PORT_ROM_ADDR(fortressBossObjBankRomStart);
            sprintf(message, "FORTRESS_BONUS");
            break;
        case 62:
            start = PORT_ROM_ADDR(fortressBossObjBankRomStart);
            end = PORT_ROM_ADDR(hubSharedObjBankRomStart);
            sprintf(message, "FORTRESS_BOSS");
            break;
        case 27:
            start = PORT_ROM_ADDR(ootwSharedObjBankRomStart);
            end = PORT_ROM_ADDR(caveObjBankRomStart);
            sprintf(message, "OOTW_SHARED");
            break;
        case 29:
            start = PORT_ROM_ADDR(ootwL2aObjBankRomStart);
            end = PORT_ROM_ADDR(ootwL2bObjBankRomStart);
            sprintf(message, "OOTW_L2A");
            break;
        case 30:
            start = PORT_ROM_ADDR(ootwL2bObjBankRomStart);
            end = PORT_ROM_ADDR(ootwL3ObjBankRomStart);
            sprintf(message, "OOTW_L2B");
            break;
        case 31:
            start = PORT_ROM_ADDR(ootwL3ObjBankRomStart);
            end = PORT_ROM_ADDR(ootwBoss1ObjBankRomStart);
            sprintf(message, "OOTW_L3");
            break;
        case 33:
            start = PORT_ROM_ADDR(ootwBoss1ObjBankRomStart);
            end = PORT_ROM_ADDR(tweenObjBankRomStart);
            sprintf(message, "OOTW_BOSS1");
            break;
        case 35:
            start = PORT_ROM_ADDR(ootwBonusObjBankRomStart);
            end = PORT_ROM_ADDR(ootwSharedObjBankRomStart);
            sprintf(message, "OOTW_BONUS");
            break;
        case 70:
            start = PORT_ROM_ADDR(titleObjBankRomStart);
            end = PORT_ROM_ADDR(outroObjBankRomStart);
            sprintf(message, "TITLE_BANK");
            break;
        default:
            PRINTF("ERROR: no object bank specified....\n");
            port_fatal("no object bank specified");
            break;
    }
    size = end - start;
    ptr = PORT_P2W(JallocAlloc(DMAGetSize(start, end), 0, "ObjBank"));
    port_load_kind = PORT_LOAD_OBJECT_BANK;
    DMAMemory(PORT_W2P(void, ptr), start, end);
    if (ptr != 0) {
        PRINTF("Loaded object bank %s (size %d)\n", message, size);
    } else {
        PRINTF("Unable to load object bank %s\n", message);
        return;
    }
    while (objectBanks[++slot].data) {
    }
    if (slot >= 5) {
        PRINTF("ERROR:too many object banks\n");
        return;
    }
    objectBanks[slot].data = PORT_W2P(void, ptr);
    objectBanks[slot].entries = PORT_W2P(ObjectBankEntry, ptr);
    for (i = 0; objectBanks[slot].entries[i].inst; i++) {
        rom = (s32)objectBanks[slot].entries[i].inst;
        objectBanks[slot].entries[i].inst = PORT_W2P(ObjectInstance, Rom2Ram(rom, ptr));
        inst = objectBanks[slot].entries[i].inst;
        inst->obj = PORT_W2P(Object, Rom2Ram((s32)inst->obj, ptr));
        if (inst->anim) {
            inst->anim = PORT_W2P(AnimState, Rom2Ram((s32)inst->anim, ptr));
            if (inst->anim->anims) {
                inst->anim->anims = PORT_W2P(struct AnimInfo, Rom2Ram((s32)inst->anim->anims, ptr));
            }
        }
        RestoreObjectPointers(inst->obj, ptr);
    }
    objectBanks[slot].count = i;
    if (num == 0) {
        InitFadeOverlay();
    }
}

f32 CalcRadius(Mesh *mesh) {
    s32 i;
    f32 radius;
    f32 r;
    Vec3 *v;

    radius = 0;
    for (i = 0; i < mesh->numVerts; i++) {
        v = &mesh->verts[i];
        r = LENGTH(v);
        if (r > radius) {
            radius = r;
        }
    }
    return radius;
}

void FindObject(PTR32(ObjectInstance) *objCPtr, u32 objID, char *name, s32 vital) {
    s32 x;
    s32 y;
    s32 bank;

    for (x = 0; x < 5; x++) {
        if (objectBanks[x].data) {
            bank = objectBanks[x].unk00;
            for (y = 0; y < objectBanks[x].count; y++) {
                if (objectBanks[x].entries[y].crc == objID) {
                    *objCPtr = objectBanks[x].entries[y].inst;
                    return;
                }
            }
        }
    }
    *objCPtr = NULL;
    if (name) {
        if (vital) {
            PRINTF("Object '%s' not found\n", name);
            sprintf(gameInfo.message, "LEVEL UNPLAYABLE - %s MISSING", name);
        }
    } else {
        PRINTF("Whoops, object '%X' not found!\n", objID);
        if (vital) {
            sprintf(gameInfo.message, "LEVEL UNPLAYABLE - OBJECT MISSING");
        }
    }
    if (vital) {
        StopDrawing("fondobj");
        gameInfo.mode = 13;
    }
    return; // forces the original jump to the epilogue
}

Object *FindSubObjectFromObject(Object *obj, u32 objID) {
    Object *o = NULL;

    o = obj;
    if (o->crc == objID) {
        return o;
    }
    if (obj->child) {
        o = FindSubObjectFromObject(obj->child, objID);
        if (o && o->crc == objID) {
            return o;
        }
    }
    if (obj->sibling) {
        o = FindSubObjectFromObject(obj->sibling, objID);
        if (o && o->crc == objID) {
            return o;
        }
    }
    return NULL;
}

void FindSubObject(Object **out, u32 crc) {
    s32 i;
    s32 j;
    Object *obj;
    Object *found;

    for (i = 0; i < 5; i++) {
        if (objectBanks[i].data) {
            for (j = 0; j < objectBanks[i].count; j++) {
                obj = objectBanks[i].entries[j].inst->obj;
                found = FindSubObjectFromObject(obj, crc);
                if (found) {
                    *out = found;
                    return;
                }
            }
        }
    }
    *out = NULL;
    PRINTF("Whoops, object '%X' not found!\n", crc);
}

/* LoadLandscapeFile: the port's version, which reads big-endian headers and converts what it loads, is in src/port/loaders.c. */

/* LoadRecordKeyFile: the port's version, which reads big-endian headers and converts what it loads, is in src/port/loaders.c. */

void WriteObjectDisplayListFlat(Object *obj) {
    s32 i;
    s32 j;
    s32 face = 0;
    s32 k;
    Mesh *mesh = obj->mesh;
    u16 (*uv)[2] = mesh->uv;
    Vec3 *verts = mesh->verts;
    Vec3 *v;
    FaceIndices *faces = (FaceIndices *)mesh->faces;
    FaceIndices *f;
    Vtx *start;
    s32 groups = mesh->numFaces / 10;
    s32 rem = mesh->numFaces % 10;
    s32 n;
    Texture *tex = NULL;
    MeshColour *col;
    MeshColour *colours = (MeshColour *)mesh->faceColours;

    gDPPipeSync(glistp++);
    for (i = 0; i < groups; i++) {
        start = vtxPtr;
        for (j = 0; j < 10; j++) {
            f = &faces[face];
            col = &colours[face];
            v = &verts[f->v0];
            vtxPtr[0].v.ob[0] = v->x;
            vtxPtr[0].v.ob[1] = v->y;
            vtxPtr[0].v.ob[2] = v->z;
            vtxPtr[0].v.flag = 0;
            vtxPtr[0].v.tc[0] = (*uv)[0];
            vtxPtr[0].v.tc[1] = (*uv++)[1];
            vtxPtr[0].v.cn[0] = col->r;
            vtxPtr[0].v.cn[1] = col->g;
            vtxPtr[0].v.cn[2] = col->b;
            vtxPtr[0].v.cn[3] = xluFact;
            v = &verts[f->v1];
            vtxPtr[1].v.ob[0] = v->x;
            vtxPtr[1].v.ob[1] = v->y;
            vtxPtr[1].v.ob[2] = v->z;
            vtxPtr[1].v.flag = 0;
            vtxPtr[1].v.tc[0] = (*uv)[0];
            vtxPtr[1].v.tc[1] = (*uv++)[1];
            vtxPtr[1].v.cn[0] = col->r;
            vtxPtr[1].v.cn[1] = col->g;
            vtxPtr[1].v.cn[2] = col->b;
            vtxPtr[1].v.cn[3] = xluFact;
            v = &verts[f->v2];
            vtxPtr[2].v.ob[0] = v->x;
            vtxPtr[2].v.ob[1] = v->y;
            vtxPtr[2].v.ob[2] = v->z;
            vtxPtr[2].v.flag = 0;
            vtxPtr[2].v.tc[0] = (*uv)[0];
            vtxPtr[2].v.tc[1] = (*uv++)[1];
            vtxPtr[2].v.cn[0] = col->r;
            vtxPtr[2].v.cn[1] = col->g;
            vtxPtr[2].v.cn[2] = col->b;
            vtxPtr[2].v.cn[3] = xluFact;
            face++;
            vtxPtr += 3;
        }
        gSPVertex(glistp++, start, 30, 0);
        for (k = 0; k < 10; k++) {
            if (tex != obj->mesh->textures[face - 10 + k]) {
                tex = obj->mesh->textures[face - 10 + k];
                gDPPipeSync(glistp++);
                LoadTexture(tex);
            }
            gSP1Triangle(glistp++, k * 3, k * 3 + 1, k * 3 + 2, 2);
        }
    }
    start = vtxPtr;
    for (j = 0; j < rem; j++) {
        f = &faces[face];
        col = &colours[face];
        v = &verts[f->v0];
        vtxPtr[0].v.ob[0] = v->x;
        vtxPtr[0].v.ob[1] = v->y;
        vtxPtr[0].v.ob[2] = v->z;
        vtxPtr[0].v.flag = 0;
        vtxPtr[0].v.tc[0] = (*uv)[0];
        vtxPtr[0].v.tc[1] = (*uv++)[1];
        vtxPtr[0].v.cn[0] = col->r;
        vtxPtr[0].v.cn[1] = col->g;
        vtxPtr[0].v.cn[2] = col->b;
        vtxPtr[0].v.cn[3] = xluFact;
        v = &verts[f->v1];
        vtxPtr[1].v.ob[0] = v->x;
        vtxPtr[1].v.ob[1] = v->y;
        vtxPtr[1].v.ob[2] = v->z;
        vtxPtr[1].v.flag = 0;
        vtxPtr[1].v.tc[0] = (*uv)[0];
        vtxPtr[1].v.tc[1] = (*uv++)[1];
        vtxPtr[1].v.cn[0] = col->r;
        vtxPtr[1].v.cn[1] = col->g;
        vtxPtr[1].v.cn[2] = col->b;
        vtxPtr[1].v.cn[3] = xluFact;
        v = &verts[f->v2];
        vtxPtr[2].v.ob[0] = v->x;
        vtxPtr[2].v.ob[1] = v->y;
        vtxPtr[2].v.ob[2] = v->z;
        vtxPtr[2].v.flag = 0;
        vtxPtr[2].v.tc[0] = (*uv)[0];
        vtxPtr[2].v.tc[1] = (*uv++)[1];
        vtxPtr[2].v.cn[0] = col->r;
        vtxPtr[2].v.cn[1] = col->g;
        vtxPtr[2].v.cn[2] = col->b;
        vtxPtr[2].v.cn[3] = xluFact;
        face++;
        vtxPtr += 3;
    }
    if (rem) {
        face -= rem;
        gSPVertex(glistp++, start, rem * 3, 0);
        n = 0;
        for (i = 0; i < rem; i++) {
            if (tex != obj->mesh->textures[face]) {
                tex = obj->mesh->textures[face];
                gDPPipeSync(glistp++);
                LoadTexture(tex);
            }
            gSP1Triangle(glistp++, n++, n++, n++, 0);
            face++;
        }
    }
}

void WriteObjectDisplayListGouraud(Object *obj) {
    s32 i;
    s32 j;
    s32 face = 0;
    s32 k;
    Mesh *mesh = obj->mesh;
    u16 (*uv)[2] = mesh->uv;
    Vec3 *verts = mesh->verts;
    Vec3 *v;
    FaceIndices *faces = (FaceIndices *)mesh->faces;
    FaceIndices *f;
    Vtx *start;
    s32 groups = mesh->numFaces / 10;
    s32 rem = mesh->numFaces % 10;
    s32 n;
    Texture *tex = NULL;
    MeshColour *col;
    MeshColour *colours = (MeshColour *)mesh->colours;

    gDPPipeSync(glistp++);
    for (i = 0; i < groups; i++) {
        start = vtxPtr;
        for (j = 0; j < 10; j++) {
            f = &faces[face];
            v = &verts[f->v0];
            col = &colours[f->v0];
            vtxPtr[0].v.ob[0] = v->x;
            vtxPtr[0].v.ob[1] = v->y;
            vtxPtr[0].v.ob[2] = v->z;
            vtxPtr[0].v.flag = 0;
            vtxPtr[0].v.tc[0] = (*uv)[0];
            vtxPtr[0].v.tc[1] = (*uv++)[1];
            vtxPtr[0].v.cn[0] = col->r;
            vtxPtr[0].v.cn[1] = col->g;
            vtxPtr[0].v.cn[2] = col->b;
            vtxPtr[0].v.cn[3] = xluFact;
            v = &verts[f->v1];
            col = &colours[f->v1];
            vtxPtr[1].v.ob[0] = v->x;
            vtxPtr[1].v.ob[1] = v->y;
            vtxPtr[1].v.ob[2] = v->z;
            vtxPtr[1].v.flag = 0;
            vtxPtr[1].v.tc[0] = (*uv)[0];
            vtxPtr[1].v.tc[1] = (*uv++)[1];
            vtxPtr[1].v.cn[0] = col->r;
            vtxPtr[1].v.cn[1] = col->g;
            vtxPtr[1].v.cn[2] = col->b;
            vtxPtr[1].v.cn[3] = xluFact;
            v = &verts[f->v2];
            col = &colours[f->v2];
            vtxPtr[2].v.ob[0] = v->x;
            vtxPtr[2].v.ob[1] = v->y;
            vtxPtr[2].v.ob[2] = v->z;
            vtxPtr[2].v.flag = 0;
            vtxPtr[2].v.tc[0] = (*uv)[0];
            vtxPtr[2].v.tc[1] = (*uv++)[1];
            vtxPtr[2].v.cn[0] = col->r;
            vtxPtr[2].v.cn[1] = col->g;
            vtxPtr[2].v.cn[2] = col->b;
            vtxPtr[2].v.cn[3] = xluFact;
            face++;
            vtxPtr += 3;
        }
        gSPVertex(glistp++, start, 30, 0);
        gDPPipeSync(glistp++);
        for (k = 0; k < 10; k++) {
            if (tex != obj->mesh->textures[face - 10 + k]) {
                tex = obj->mesh->textures[face - 10 + k];
                gDPPipeSync(glistp++);
                LoadTexture(tex);
            }
            gSP1Triangle(glistp++, k * 3, k * 3 + 1, k * 3 + 2, 2);
        }
    }
    start = vtxPtr;
    for (j = 0; j < rem; j++) {
        f = &faces[face];
        v = &verts[f->v0];
        col = &colours[f->v0];
        vtxPtr[0].v.ob[0] = v->x;
        vtxPtr[0].v.ob[1] = v->y;
        vtxPtr[0].v.ob[2] = v->z;
        vtxPtr[0].v.flag = 0;
        vtxPtr[0].v.tc[0] = (*uv)[0];
        vtxPtr[0].v.tc[1] = (*uv++)[1];
        vtxPtr[0].v.cn[0] = col->r;
        vtxPtr[0].v.cn[1] = col->g;
        vtxPtr[0].v.cn[2] = col->b;
        vtxPtr[0].v.cn[3] = xluFact;
        v = &verts[f->v1];
        col = &colours[f->v1];
        vtxPtr[1].v.ob[0] = v->x;
        vtxPtr[1].v.ob[1] = v->y;
        vtxPtr[1].v.ob[2] = v->z;
        vtxPtr[1].v.flag = 0;
        vtxPtr[1].v.tc[0] = (*uv)[0];
        vtxPtr[1].v.tc[1] = (*uv++)[1];
        vtxPtr[1].v.cn[0] = col->r;
        vtxPtr[1].v.cn[1] = col->g;
        vtxPtr[1].v.cn[2] = col->b;
        vtxPtr[1].v.cn[3] = xluFact;
        v = &verts[f->v2];
        col = &colours[f->v2];
        vtxPtr[2].v.ob[0] = v->x;
        vtxPtr[2].v.ob[1] = v->y;
        vtxPtr[2].v.ob[2] = v->z;
        vtxPtr[2].v.flag = 0;
        vtxPtr[2].v.tc[0] = (*uv)[0];
        vtxPtr[2].v.tc[1] = (*uv++)[1];
        vtxPtr[2].v.cn[0] = col->r;
        vtxPtr[2].v.cn[1] = col->g;
        vtxPtr[2].v.cn[2] = col->b;
        vtxPtr[2].v.cn[3] = xluFact;
        face++;
        vtxPtr += 3;
    }
    if (rem) {
        face -= rem;
        gSPVertex(glistp++, start, rem * 3, 0);
        n = 0;
        for (i = 0; i < rem; i++) {
            if (tex != obj->mesh->textures[face]) {
                tex = obj->mesh->textures[face];
                gDPPipeSync(glistp++);
                LoadTexture(tex);
            }
            gSP1Triangle(glistp++, n++, n++, n++, 0);
            face++;
        }
    }
    gDPPipeSync(glistp++);
}

void LoadTexture(Texture *tex) {
    if (tex == NULL) {
        return;
    }
    switch ((s16)tex->siz) {
        case G_IM_SIZ_16b:
            gDPSetTextureLUT(glistp++, G_TT_NONE);
            if (mirrorTextures) {
                gDPLoadTextureBlock(glistp++, tex->data, (s16)tex->fmt, G_IM_SIZ_16b, tex->width, tex->height, 0,
                                    G_TX_MIRROR, G_TX_MIRROR, tex->maskS, tex->maskT, G_TX_NOLOD, G_TX_NOLOD);
            } else {
                gDPLoadTextureBlock(glistp++, tex->data, (s16)tex->fmt, G_IM_SIZ_16b, tex->width, tex->height, 0,
                                    G_TX_WRAP, G_TX_WRAP, tex->maskS, tex->maskT, G_TX_NOLOD, G_TX_NOLOD);
            }
            break;
        case G_IM_SIZ_8b:
            gDPSetTextureLUT(glistp++, G_TT_RGBA16);
            gDPLoadTLUT_pal256(glistp++, tex->palette);
            if (mirrorTextures) {
                gDPLoadTextureBlock(glistp++, tex->data, (s16)tex->fmt, G_IM_SIZ_8b, tex->width, tex->height, 0,
                                    G_TX_MIRROR, G_TX_MIRROR, tex->maskS, tex->maskT, G_TX_NOLOD, G_TX_NOLOD);
            } else {
                gDPLoadTextureBlock(glistp++, tex->data, (s16)tex->fmt, G_IM_SIZ_8b, tex->width, tex->height, 0,
                                    G_TX_WRAP, G_TX_WRAP, tex->maskS, tex->maskT, G_TX_NOLOD, G_TX_NOLOD);
            }
            break;
        case G_IM_SIZ_4b:
            gDPSetTextureLUT(glistp++, G_TT_RGBA16);
            gDPLoadTLUT_pal16(glistp++, 0, tex->palette);
            if (mirrorTextures) {
                gDPLoadTextureBlock_4b(glistp++, tex->data, (s16)tex->fmt, tex->width, tex->height, 0,
                                       G_TX_MIRROR, G_TX_MIRROR, tex->maskS, tex->maskT, G_TX_NOLOD, G_TX_NOLOD);
            } else {
                gDPLoadTextureBlock_4b(glistp++, tex->data, (s16)tex->fmt, tex->width, tex->height, 0,
                                       G_TX_WRAP, G_TX_WRAP, tex->maskS, tex->maskT, G_TX_NOLOD, G_TX_NOLOD);
            }
            break;
    }
}

void StopDrawing(char *name) {
    if (codeDrawingRequest == TRUE) {
        PRINTF("Stop Drawing Requested from '%s'\n", name);
    }
    codeDrawingRequest = FALSE;
    /* The N64 waits here for the graphics thread to stop drawing at its next frame. The port has one thread. */
    gfxIsDrawing = FALSE;
    disableGraphics = TRUE;
}

void StartDrawing(char *name) {
    if (!codeDrawingRequest) {
        PRINTF(D_80106E78, name);
    }
    codeDrawingRequest = TRUE;
    /* The N64 waits here for the graphics thread to start drawing at its next frame. The port has one thread. */
    gfxIsDrawing = TRUE;
    disableGraphics = FALSE;
    onlyDrawBackdrops = FALSE;
}

INCLUDE_ASM("asm/nonmatchings/banks", pad_80155B84);

INCLUDE_RODATA("asm/nonmatchings/banks", D_80106E78);

PORT_C_END
