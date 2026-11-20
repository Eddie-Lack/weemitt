/* Written for weemitt: the parts of the N64 system interface that the port's code uses. */
#ifndef WEEMITT_PR_SP_H
#define WEEMITT_PR_SP_H

#include <PR/ultratypes.h>
#include <PR/mbi.h>

/* One piece of a sprite's picture. A sprite draws its pieces from left to right. */
typedef struct bitmap {
    s16 width;         /* texels to draw across */
    s16 width_img;     /* texels across in the stored picture */
    s16 s;             /* first texel across */
    s16 t;             /* first texel down */
    PTR32(void) buf;   /* the texels */
    s16 actualHeight;
    s16 LUToffset;     /* first palette entry */
} Bitmap;

/* A sprite. The game keeps it in its own memory, so the pointers are PTR32 fields. */
typedef struct sprite {
    s16 x;
    s16 y;
    s16 width;
    s16 height;
    f32 scalex;
    f32 scaley;
    s16 expx;
    s16 expy;
    u16 attr;          /* SP_ flags */
    s16 zdepth;
    u8 red;
    u8 green;
    u8 blue;
    u8 alpha;
    s16 startTLUT;
    s16 nTLUT;
    PTR32(int) LUT;    /* the palette */
    s16 istart;        /* index of the first bitmap to draw */
    s16 istep;
    s16 nbitmaps;
    s16 ndisplist;     /* room in rsp_dl, in commands */
    s16 bmheight;      /* texel rows to draw */
    s16 bmHreal;       /* texel rows stored */
    u8 bmfmt;
    u8 bmsiz;
    PTR32(Bitmap) bitmap;
    PTR32(Gfx) rsp_dl;
    PTR32(Gfx) rsp_dl_next;
    s16 frac_s;
    s16 frac_t;
} Sprite;

/* The number of display list commands to reserve for a sprite of nb bitmaps. */
#define NUM_DL(nb) ((nb) * 12 + 24)

/* Flags for Sprite.attr. */
#define SP_TRANSPARENT 0x001
#define SP_CUTOUT      0x002
#define SP_HIDDEN      0x004

void spInit(Gfx **glistp);
void spFinish(Gfx **glistp);
void spScissor(s32 xmin, s32 xmax, s32 ymin, s32 ymax);
void spSetAttribute(Sprite *sp, s32 attr);
void spClearAttribute(Sprite *sp, s32 attr);
void spMove(Sprite *sp, s32 x, s32 y);
void spScale(Sprite *sp, f32 sx, f32 sy);
void spColor(Sprite *sp, u8 red, u8 green, u8 blue, u8 alpha);
Gfx *spDraw(Sprite *sp);

#endif
