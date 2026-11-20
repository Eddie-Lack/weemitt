/* Everything the game reads from the ROM is big-endian. This file fills the game's data from the player's ROM and
   converts each bank and file the game loads to the host's byte order, before the game's own code reads it. */
PORT_C_BEGIN
#include "port.h"
#include "common.h"
#include "texture.h"
#include "banks.h"
#include "actor.h"
#include "game.h"
#include "jalloc.h"
#include "port_romsyms.h"

/* ---- The game's data: copy the main segment from the ROM, then swap and relocate (tables from gen_ram.py). */

void port_load_ram(void) {
    u8 *dst = port_ram + (port_main_vram - PORT_RAM_BASE);
    const u32 *r;
    const PortFnReloc *f;
    u32 i;

    memcpy(dst, port_rom + port_main_rom, port_main_size);
    for (r = port_swap2; r[1]; r += 2) {
        port_swap16_array(port_ram + r[0], r[1]);
    }
    for (r = port_swap4; r[1]; r += 2) {
        port_swap32_array(port_ram + r[0], r[1]);
    }
    for (r = port_swap8; r[1]; r += 2) {
        u32 *w = (u32 *)(port_ram + r[0]);
        for (i = 0; i < r[1]; i++, w += 2) {
            u32 hi = port_bswap32(w[0]);
            w[0] = port_bswap32(w[1]);
            w[1] = hi;
        }
    }
    for (r = port_ptrs; *r; r++) {
        u32 *w = (u32 *)(port_ram + *r);
        if (*w >= PORT_RAM_BASE && *w < PORT_RAM_BASE + PORT_RAM_SIZE) {
            *w = PORT_N64_TO_W(*w);
        }
    }
    for (f = port_fnptrs; f->offset; f++) {
        *(u32 *)(port_ram + f->offset) = PORT_FN_WORD(f->index);
    }
}

#if defined(PORT_PTR_ENCODED) && !defined(PORT_PTR_DISPLACED)
/* The distance from a word to its pointer (ptr32.h). port_ram lies wherever the host loads the program; gen/ram.S
   aligns it on at least 4096 bytes, so the delta is a multiple of 16, which alHeapInit needs. A C++ initializer sets
   it before any code of the program runs, also when the game is linked into another program. */
#if defined(__APPLE__)
/* Apple's chained fixups (iOS 15 and later) cannot hold an address that lies before the library's start, as this one
   does: the linker writes a wrong value. A constructor computes it when the library loads. */
uintptr_t port_ptr_delta;
__attribute__((constructor)) static void port_ptr_delta_set(void) {
    port_ptr_delta = (uintptr_t)port_ram - PORT_PTR_BASE;
}
#else
uintptr_t port_ptr_delta = (uintptr_t)port_ram - PORT_PTR_BASE;
#endif
#endif

#ifdef PORT_PTR_ENCODED
/* The word of a function that the game's data may point to. The game stores none at run time, and the sound player
   stores its functions only while it sets up its filters, so a plain search is enough. */
unsigned int port_fn_word(PortFn fn) {
    unsigned int i;

    for (i = 0; i < port_fn_count; i++) {
        if (port_fn_table[i] == fn) {
            return PORT_FN_WORD(i);
        }
    }
    for (i = 0; i < port_audio_fn_count; i++) {
        if (port_audio_fn_table[i] == fn) {
            return PORT_FN_WORD(port_fn_count + i);
        }
    }
    port_fatal("port_fn_word: %p is not in port_fn_table or port_audio_fn_table", (void *)fn);
    return 0;
}
#endif

/* A ROM rebuilt by the map editor moves the assets and patches the instructions that load their addresses: a lui
   and an addiu in the original code, a lui and an ori after libgarib's patch. ori's immediate is zero-extended;
   addiu's and a load's are sign-extended. */
void port_set_rom_symbols(void) {
    const PortRomSym *s;

    for (s = port_romsyms; s->var; s++) {
        u32 value = s->original;
        if (s->hiRom) {
            u32 hi = port_be32(port_rom + s->hiRom);
            u32 lo = port_be32(port_rom + s->loRom);
            u32 imm = lo & 0xFFFF;
            value = ((hi & 0xFFFF) << 16) + ((lo >> 26) == 0x0D ? imm : (u32)(s32)(s16)imm);
        }
        *s->var = (void *)(uintptr_t)value;
    }
}

void port_swap_layout(void *p, const PortLayout *layout) {
    u8 *b = (u8 *)p;
    u32 i;

    for (i = 0; i < layout->count; i++) {
        u32 off = layout->units[i * 2];
        u32 size = layout->units[i * 2 + 1];
        if (size == 2) {
            *(u16 *)(b + off) = port_bswap16(*(u16 *)(b + off));
        } else if (size == 4) {
            *(u32 *)(b + off) = port_bswap32(*(u32 *)(b + off));
        } else if (size == 8) {
            u32 hi = port_bswap32(*(u32 *)(b + off));
            *(u32 *)(b + off) = port_bswap32(*(u32 *)(b + off + 4));
            *(u32 *)(b + off + 4) = hi;
        }
    }
}

/* ---- Level files: parse.c and nmeprocess.c read every field with CopyFromData, whose calls the build rewrites to
   pass the size of the destination's scalar type (see port_fixups.h). */

void port_copy_from_data(void *dst, u8 **src, int size, int unit) {
    memcpy(dst, *src, size);
    if (unit == 2) {
        port_swap16_array(dst, size / 2);
    } else if (unit == 4) {
        port_swap32_array(dst, size / 4);
    }
    *src += size;
}

s16 port_read_token(u8 *data) {
    return (s16)port_be16(data);
}

/* ---- DMA from the ROM (jalloc.c). FLA2 files are compressed; their header holds the size little-endian. */

extern OSIoMesg dmaIOMessageBuf;
extern OSMesgQueue dmaMessageQ;
s32 DecompressBuffer(u8 *src, u8 *dst);
void JallocFree(void *p);

int port_load_kind;

s32 DMAGetSize(u32 start, u32 end) {
    u8 header[8];

    port_dma(header, start, 8);
    if (port_be32(header) == 0x464C4132) {
        return header[4] | (header[5] << 8) | (header[6] << 16) | (header[7] << 24);
    }
    return end - start;
}

static void convert(void *dest, u32 size) {
    s32 kind = port_load_kind;

    port_load_kind = PORT_LOAD_RAW;
    if (kind == PORT_LOAD_TEXTURE_BANK) {
        port_convert_texture_bank((u8 *)dest, size);
    } else if (kind == PORT_LOAD_OBJECT_BANK) {
        port_convert_object_bank((u8 *)dest, size);
    }
}

void DMAMemory(void *dest, u32 start, u32 end) {
    u32 size = end - start;
    u8 header[8];
    u8 *buf;
    u32 result;

    port_dma(header, start, 8);
    if (port_be32(header) == 0x464C4132) {
        result = header[4] | (header[5] << 8) | (header[6] << 16) | (header[7] << 24);
        buf = (u8 *)JallocAlloc(size, 1, "tempdecomp");
        port_dma(buf, start, size);
        DecompressBuffer(buf + 8, (u8 *)dest);
        JallocFree(&buf);
        convert(dest, result);
    } else {
        port_dma(dest, start, size);
        convert(dest, size);
    }
}

/* ---- Landscape (level) and demo files (banks.c): each file starts with its size. */

extern u8 landscapeFilesRomStart[];
extern u8 landscapeFilesRomEnd[];
extern u8 recordKeyFilesRomStart[];
extern u8 recordKeyFilesRomEnd[];
void StopDrawing(char *name);

static u32 file_offset(u32 rom, u32 end, s32 num, const char *what) {
    s32 i;
    u8 word[4];

    for (i = 0; i < num; i++) {
        port_dma(word, rom, 4);
        rom += port_be32(word);
        if (rom >= end) {
            StopDrawing("loadlandscape");
            gameInfo.mode = 13;
            sprintf(gameInfo.message, "LEVEL UNPLAYABLE - NO %s FILE", what);
            return 0;
        }
    }
    return rom;
}

void *LoadLandscapeFile(s32 num) {
    u32 rom = file_offset(PORT_ROM_ADDR(landscapeFilesRomStart), PORT_ROM_ADDR(landscapeFilesRomEnd), num, "LEV");
    u8 word[4];
    u32 size;
    void *buf;

    if (rom == 0) {
        return NULL;
    }
    port_dma(word, rom, 4);
    size = port_be32(word);
    buf = JallocAlloc(DMAGetSize(rom, rom + size), 1, "LandData");
    DMAMemory(buf, rom, rom + size);
    return buf;
}

void *LoadRecordKeyFile(s32 num) {
    u32 rom = file_offset(PORT_ROM_ADDR(recordKeyFilesRomStart), PORT_ROM_ADDR(recordKeyFilesRomEnd), num, "MOO");
    u8 word[4];
    u32 size;
    void *data;

    if (rom == 0) {
        return NULL;
    }
    port_dma(word, rom, 4);
    size = port_be32(word);
    data = JallocAlloc(size, 1, "RecKey");
    DMAMemory(data, rom + 4, rom + 4 + size);
    port_swap16_array(data, size / 2);
    return data;
}

/* ---- Texture banks: a count, then each texture's header, texels and palette. */

void port_convert_texture_bank(u8 *bank, u32 size) {
    u32 count;
    u8 *p;
    u32 i;

    *(u32 *)bank = port_bswap32(*(u32 *)bank);
    count = *(u32 *)bank;
    p = bank + 4;
    for (i = 0; i < count && p + sizeof(Texture) <= bank + size; i++) {
        Texture *tex = (Texture *)p;
        u8 *data;
        u8 *end;
        u8 *pal;

        port_swap_layout(tex, &port_layout_Texture);
        if (tex->size <= 0 || p + tex->size > bank + size) {
            port_log("texture bank: bad texture size %d at %d", tex->size, (int)(p - bank));
            break;
        }
        data = p + sizeof(Texture);
        end = p + tex->size;
        pal = tex->palette ? p + (u32)tex->palette : NULL;
        if (pal != NULL && (pal < data || pal > end)) {
            pal = NULL;
        }
        if (tex->siz == G_IM_SIZ_16b) {
            port_swap16_array(data, ((pal ? pal : end) - data) / 2);
        } else if (tex->siz == G_IM_SIZ_32b) {
            port_swap32_array(data, ((pal ? pal : end) - data) / 4);
        }
        if (pal != NULL) {
            port_swap16_array(pal, (end - pal) / 2);
        }
        p = end;
    }
}

/* ---- Object banks: entries, instances, the object trees and everything they point to. Pointers inside a bank are
   offsets in its low 24 bits (see Rom2Ram in banks.c). Parts of a bank are shared: objects share meshes, and one
   display list can be the tail of another. A bitmap with one bit per 2 bytes records what is already converted.
   Nothing is swapped twice. */

static u8 *bankBase;
static u32 bankSize;
static u8 converted[1 << 20];

static void *at(u32 ptr) {
    u32 off = ptr & 0xFFFFFF;

    if (ptr == 0 || off >= bankSize) {
        return NULL;
    }
    return bankBase + off;
}

static s32 is_converted(void *p) {
    u32 i = ((u8 *)p - bankBase) >> 1;

    return (converted[i >> 3] >> (i & 7)) & 1;
}

/* Swaps one scalar of 2 or 4 bytes unless it was already converted. */
static void swap_unit(void *p, u32 size) {
    u32 i = ((u8 *)p - bankBase) >> 1;
    u32 n = size >> 1;
    u32 k;

    if ((u8 *)p + size > bankBase + bankSize || ((u32)((u8 *)p - bankBase) & 1)) {
        return;
    }
    for (k = 0; k < n; k++) {
        if ((converted[(i + k) >> 3] >> ((i + k) & 7)) & 1) {
            return;
        }
    }
    for (k = 0; k < n; k++) {
        converted[(i + k) >> 3] |= 1 << ((i + k) & 7);
    }
    if (size == 2) {
        *(u16 *)p = port_bswap16(*(u16 *)p);
    } else {
        *(u32 *)p = port_bswap32(*(u32 *)p);
    }
}

static void swap_struct(void *p, const PortLayout *layout) {
    u32 i;

    if (p == NULL) {
        return;
    }
    for (i = 0; i < layout->count; i++) {
        swap_unit((u8 *)p + layout->units[i * 2], layout->units[i * 2 + 1]);
    }
}

static void swap_structs(void *p, s32 count, const PortLayout *layout) {
    s32 i;

    for (i = 0; p != NULL && i < count; i++) {
        swap_struct((u8 *)p + i * layout->size, layout);
    }
}

static void swap_units(void *p, s32 count, u32 size) {
    s32 i;

    for (i = 0; p != NULL && i < count; i++) {
        swap_unit((u8 *)p + i * size, size);
    }
}

/* F3DEX display list: both words of each command until G_ENDDL, and the vertices that G_VTX loads. A command
   that is already converted belongs to a list converted before, which goes on to its end. */
static void convert_display_list(u32 *dl) {
    while (dl != NULL && (u8 *)(dl + 2) <= bankBase + bankSize && !is_converted(dl)) {
        u32 w0;
        u32 w1;

        swap_unit(&dl[0], 4);
        swap_unit(&dl[1], 4);
        w0 = dl[0];
        w1 = dl[1];
        if ((w0 >> 24) == G_VTX) {
            swap_structs(at(w1), (w0 >> 10) & 0x3F, &port_layout_Vtx);
        }
        if ((w0 >> 24) == G_ENDDL) {
            break;
        }
        dl += 2;
    }
}

static void convert_object(Object *obj) {
    Mesh *mesh;

    while (obj != NULL && !is_converted(obj)) {
        swap_struct(obj, &port_layout_Object);
        mesh = (Mesh *)at((u32)obj->mesh);
        if (mesh != NULL && !is_converted(mesh)) {
            swap_struct(mesh, &port_layout_Mesh);
            swap_units(at((u32)mesh->verts), mesh->numVerts * 3, 4);
            if (mesh->numFaces > 0) {
                swap_units(at((u32)mesh->faces), mesh->numFaces * 3, 2);
                swap_units(at((u32)mesh->uv), mesh->numFaces * 6, 2);
                swap_units(at((u32)mesh->textures), mesh->numFaces, 4);
            }
        }
        convert_display_list((u32 *)at((u32)obj->displayList));
        swap_structs(at((u32)obj->scaleKeys), obj->numScaleKeys, &port_layout_PosKey);
        swap_structs(at((u32)obj->posKeys), obj->numPosKeys, &port_layout_PosKey);
        swap_structs(at((u32)obj->rotKeys), obj->numRotKeys, &port_layout_RotKey);
        swap_structs(at((u32)obj->sprites), obj->numSprites, &port_layout_ObjectSprite);
        convert_object((Object *)at((u32)obj->child));
        obj = (Object *)at((u32)obj->sibling);
    }
}

void port_convert_object_bank(u8 *bank, u32 size) {
    ObjectBankEntry *e;
    u32 clear = (size >> 4) + 1;

    bankBase = bank;
    bankSize = size < 0x1000000 ? size : 0x1000000;
    memset(converted, 0, clear < sizeof(converted) ? clear : sizeof(converted));
    for (e = (ObjectBankEntry *)bank; (u8 *)(e + 1) <= bank + bankSize; e++) {
        ObjectInstance *inst;
        AnimState *anim;

        swap_struct(e, &port_layout_ObjectBankEntry);
        if (e->inst == NULL) {
            break;
        }
        inst = (ObjectInstance *)at((u32)e->inst);
        if (inst == NULL || is_converted((u8 *)inst + 12)) {
            continue;
        }
        swap_struct(inst, &port_layout_ObjectInstance);
        convert_object((Object *)at((u32)inst->obj));
        anim = (AnimState *)at((u32)inst->anim);
        if (anim != NULL && !is_converted(anim)) {
            swap_struct(anim, &port_layout_AnimState);
            swap_structs(at((u32)anim->anims), anim->numAnims, &port_layout_AnimInfo);
        }
    }
}

/* RestoreDrawList (banks.c) reads each command's opcode as the first byte, which is the high byte of the command's
   first word on the N64. This is the same function with word reads. */
extern PTR32(u32) ballTextureData;
extern PTR32(u32) ballTextureData2;
extern PTR32(u32) ballTexturePalette;
extern PTR32(u32) ballTexturePalette2;
extern PTR32(u32) ballBitTextureData;
extern PTR32(u32) ballBitTexturePalette;
extern PTR32(u32) handTextureData[2];
extern PTR32(u32) handTexturePalette[2];
s32 Rom2Ram(s32 addr, s32 base);

static s32 name_is(Object *obj, const char *name) {
    s32 i;

    for (i = 0; name[i]; i++) {
        if (obj->name[i] != name[i]) {
            return 0;
        }
    }
    return obj->name[i] == 0;
}

void RestoreDrawList(u8 *list, s32 base, Object *obj) {
    u32 *dl = (u32 *)list;
    Texture *tex;

    while ((dl[0] >> 24) != 0xB8) {
        switch (dl[0] >> 24) {
            case 0x04:
                dl[1] = Rom2Ram(dl[1], base);
                dl += 2;
                break;
            case 0xFD:
                if ((dl[2] >> 24) == 0xF5) {
                    FindTexture(&tex, dl[1], 1);
                    if (tex != NULL) {
                        dl[1] = (u32)tex->data;
                    }
                    if (name_is(obj, "gball")) {
                        ballTextureData = &dl[1];
                    } else if (name_is(obj, "cball")) {
                        ballTextureData2 = &dl[1];
                    } else if (name_is(obj, "GShinL")) {
                        handTextureData[0] = &dl[1];
                    } else if (name_is(obj, "GShinR")) {
                        handTextureData[1] = &dl[1];
                    } else if (ballBitTextureData == NULL && name_is(obj, "gbl_bt")) {
                        ballBitTextureData = &dl[1];
                    }
                    dl += 0x38 / 4;
                } else if ((dl[2] >> 24) == 0xE8) {
                    FindTexture(&tex, dl[1], 1);
                    if (tex != NULL) {
                        dl[1] = (u32)tex->palette;
                    }
                    if (name_is(obj, "gball")) {
                        ballTexturePalette = &dl[1];
                    } else if (name_is(obj, "cball")) {
                        ballTexturePalette2 = &dl[1];
                    } else if (name_is(obj, "GShinL")) {
                        handTexturePalette[0] = &dl[1];
                    } else if (name_is(obj, "GShinR")) {
                        handTexturePalette[1] = &dl[1];
                    } else if (ballBitTexturePalette == NULL && name_is(obj, "gbl_bt")) {
                        ballBitTexturePalette = &dl[1];
                    }
                    dl += 0x30 / 4;
                } else {
                    dl += 0x10 / 4;
                }
                break;
            default:
                dl += 2;
                break;
        }
    }
}

PORT_C_END
