/* The port's own declarations: the game's memory, the tables that fill it from the ROM, byte order, and the
   interfaces between the game, the platform layer, the renderer and the sound. */
#ifndef PORT_H
#define PORT_H

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "ultra64.h"

/* A word of game data that points to a function: its offset in port_ram and the function's index in
   port_fn_table (ptr32.h). */
typedef struct PortFnReloc {
    u32 offset;
    u32 index;
} PortFnReloc;

/* A ROM asset address and the lui/addiu pair in the code that loads it. */
typedef struct PortRomSym {
    void **var;
    u32 original;
    u32 hiRom;
    u32 loRom;
} PortRomSym;

/* The scalar fields of a structure: pairs of offset and size (2, 4 or 8 bytes). */
typedef struct PortLayout {
    u16 size;
    u16 count;
    const u16 *units;
} PortLayout;
void port_swap_layout(void *p, const PortLayout *layout);
extern const PortLayout port_layout_Texture, port_layout_ObjectBankEntry, port_layout_ObjectInstance,
    port_layout_AnimState, port_layout_AnimInfo, port_layout_Object, port_layout_Mesh, port_layout_ObjectSprite,
    port_layout_PosKey, port_layout_RotKey, port_layout_Vtx;

/* port_ram stands for RDRAM: N64 address 0x80000000 + n is port_ram + n. It also holds the heap, which runs 16 MB
   past the end of RDRAM. The program is linked at a low address, so libultra's K0-to-physical mask
   (& 0x1FFFFFFF) leaves its pointers unchanged. */
extern u8 port_ram[];
#define PORT_RAM_BASE 0x80000000u
#define PORT_RAM_SIZE 0x800000u
#define PORT_HEAP_END (PORT_RAM_SIZE + 0x1000000u)
#define PORT_N64_TO_HOST(a) ((void *)(port_ram + ((u32)(a) - PORT_RAM_BASE)))
/* The Ptr32 word (ptr32.h) of an N64 address in RDRAM. */
#define PORT_N64_TO_W(a) ((u32)(a) - PORT_RAM_BASE + PORT_PTR_BASE)

/* The player's ROM, in big-endian (.z64) byte order. */
extern u8 *port_rom;
extern u32 port_rom_size;

/* Generated tables (gen/). */
extern const u32 port_swap2[], port_swap4[], port_swap8[], port_ptrs[];
extern const PortFnReloc port_fnptrs[];
extern const PortRomSym port_romsyms[];
extern const u32 port_main_rom, port_main_vram, port_main_size;

void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Called by port_fatal after the message, in place of exit(1), when set. It must not return. */
extern void (*port_fatal_hook)(void);

/* Byte order. Everything the game reads from the ROM is big-endian; the PC is little-endian. */
static inline u16 port_bswap16(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static inline u32 port_bswap32(u32 v) { return __builtin_bswap32(v); }
static inline u32 port_be32(const void *p) {
    const u8 *b = (const u8 *)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}
static inline u16 port_be16(const void *p) {
    const u8 *b = (const u8 *)p;
    return (u16)((b[0] << 8) | b[1]);
}
void port_swap16_array(void *p, u32 count);
void port_swap32_array(void *p, u32 count);

/* What the next DMAMemory call loads, so that its data can be converted to host byte order. */
#ifndef PORT_LOAD_KIND_DEFINED
#define PORT_LOAD_KIND_DEFINED
enum PortLoadKind {
    PORT_LOAD_RAW,
    PORT_LOAD_TEXTURE_BANK,
    PORT_LOAD_OBJECT_BANK,
};
#endif
extern int port_load_kind;
void port_convert_texture_bank(u8 *bank, u32 size);
void port_convert_object_bank(u8 *bank, u32 size);

void port_load_ram(void);
void port_set_rom_symbols(void);
void port_dma(void *dst, u32 devAddr, u32 size);

/* Controller 1, set by the platform layer before each game update. inputlatch.h has the latch. */
#include "inputlatch.h"
extern u16 port_buttons;
extern u32 port_controller_reads; /* counts the game's reads of the controller */
extern s8 port_stick_x, port_stick_y;

/* Save data: the 4 Kbit EEPROM's 512 bytes, kept in a file by the platform layer. */
extern u8 port_eeprom[512];
extern s32 port_eeprom_dirty;

/* Time in microseconds since start-up (the platform layer). */
u64 port_time_us(void);

/* The sound player's clock without a sound device (src/audio/mus/mus-glue.c, or nothing in audio_stub.c). The
   frame driver calls it once per update with the update's length in video retraces. */
void musAudioAdvance(f32 retraces);

#endif
