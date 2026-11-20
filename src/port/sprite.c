/* The sprite functions of libultra (sp.h), which the game uses for text and 2D overlays, written for the port.
   A sprite is a grid of bitmaps drawn left to right; a row ends when it reaches the sprite's width. Each bitmap
   becomes one textured rectangle.

   The render mode is set only when a sprite's transparency attributes differ from the last sprite's. Until then
   the mode the game set up before drawing (rdpInitForOverlays_dl, a blending mode) applies. */
PORT_C_BEGIN
#include "port.h"

#define SP_BLEND_ATTRS (SP_TRANSPARENT | SP_CUTOUT)

static s32 currentAttrs;

void spInit(Gfx **glistp) {
    Gfx *g = *glistp;

    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetTexturePersp(g++, G_TP_NONE);
    gDPSetTextureDetail(g++, G_TD_CLAMP);
    gDPSetTextureLOD(g++, G_TL_TILE);
    gDPSetTextureConvert(g++, G_TC_FILT);
    gDPSetCombineKey(g++, G_CK_NONE);
    gDPSetAlphaCompare(g++, G_AC_NONE);
    gSPClearGeometryMode(g++, G_ZBUFFER | G_FOG | G_LIGHTING | G_CULL_BOTH);
    currentAttrs = 0;
    *glistp = g;
}

void spFinish(Gfx **glistp) {
    Gfx *g = *glistp;

    gDPPipeSync(g++);
    gDPSetTexturePersp(g++, G_TP_PERSP);
    gSPEndDisplayList(g++);
    *glistp = g;
}

void spSetAttribute(Sprite *sp, s32 attr) {
    sp->attr |= attr;
}

void spClearAttribute(Sprite *sp, s32 attr) {
    sp->attr &= ~attr;
}

void spMove(Sprite *sp, s32 x, s32 y) {
    sp->x = x;
    sp->y = y;
}

void spScale(Sprite *sp, f32 sx, f32 sy) {
    sp->scalex = sx;
    sp->scaley = sy;
}

void spColor(Sprite *sp, u8 red, u8 green, u8 blue, u8 alpha) {
    sp->red = red;
    sp->green = green;
    sp->blue = blue;
    sp->alpha = alpha;
}

void spScissor(s32 xmin, s32 xmax, s32 ymin, s32 ymax) {
}

static s32 clamp_coord(f32 v) {
    s32 c = (s32)(v * 4.0f);

    return c < 0 ? 0 : c > 0xFFF ? 0xFFF : c;
}

Gfx *spDraw(Sprite *sp) {
    Gfx *start = sp->rsp_dl_next;
    Gfx *g = start;
    f32 sx = sp->scalex > 0.0f ? sp->scalex : 1.0f;
    f32 sy = sp->scaley > 0.0f ? sp->scaley : 1.0f;
    s32 tx = 0;
    s32 ty = 0;
    s32 i;
    s32 ci = sp->bmfmt == G_IM_FMT_CI;

    if (sp->attr & SP_HIDDEN) {
        gSPEndDisplayList(g++);
        sp->rsp_dl_next = g;
        return start;
    }
    gDPPipeSync(g++);
    gDPSetTextureFilter(g++, (sx == 1.0f && sy == 1.0f) ? G_TF_POINT : G_TF_BILERP);
    gDPSetCombineMode(g++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
    gDPSetPrimColor(g++, 0, 0, sp->red, sp->green, sp->blue, sp->alpha);
    if ((sp->attr & SP_BLEND_ATTRS) != currentAttrs) {
        currentAttrs = sp->attr & SP_BLEND_ATTRS;
        if (sp->attr & SP_TRANSPARENT) {
            gDPSetRenderMode(g++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
        } else {
            gDPSetRenderMode(g++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
        }
        gDPSetAlphaCompare(g++, (sp->attr & SP_CUTOUT) ? G_AC_THRESHOLD : G_AC_NONE);
        gDPSetBlendColor(g++, 0, 0, 0, 1);
    }
    if (ci && sp->LUT != NULL) {
        gDPSetTextureLUT(g++, G_TT_RGBA16);
        gDPLoadTLUT_pal256(g++, sp->LUT);
    } else {
        gDPSetTextureLUT(g++, G_TT_NONE);
    }
    for (i = 0; i < sp->nbitmaps; i++) {
        Bitmap *bm = &sp->bitmap[i];
        s32 h;
        f32 x0;
        f32 y0;

        if (bm->width < 0) {
            break;
        }
        if (tx >= sp->width && tx > 0) {
            tx = 0;
            ty += sp->bmheight + sp->expy;
        }
        h = bm->actualHeight > 0 ? bm->actualHeight : sp->bmheight;
        if (bm->buf != NULL && bm->width > 0 && h > 0) {
            x0 = sp->x + tx * sx;
            y0 = sp->y + ty * sy;
            gDPSetTextureImage(g++, sp->bmfmt, sp->bmsiz, bm->width_img, bm->buf);
            gDPSetTile(g++, sp->bmfmt, sp->bmsiz, 0, 0, G_TX_LOADTILE, 0, G_TX_CLAMP, 0, 0, G_TX_CLAMP, 0, 0);
            gDPLoadSync(g++);
            gDPLoadTile(g++, G_TX_LOADTILE, bm->s << 2, bm->t << 2, (bm->s + bm->width - 1) << 2,
                        (bm->t + h - 1) << 2);
            gDPPipeSync(g++);
            gDPSetTile(g++, sp->bmfmt, sp->bmsiz, 0, 0, G_TX_RENDERTILE, ci ? (bm->LUToffset >> 4) & 0xF : 0,
                       G_TX_CLAMP, 0, 0, G_TX_CLAMP, 0, 0);
            gDPSetTileSize(g++, G_TX_RENDERTILE, bm->s << 2, bm->t << 2, (bm->s + bm->width - 1) << 2,
                           (bm->t + h - 1) << 2);
            gSPTextureRectangle(g++, clamp_coord(x0), clamp_coord(y0), clamp_coord(x0 + bm->width * sx),
                                clamp_coord(y0 + h * sy), G_TX_RENDERTILE, bm->s << 5, bm->t << 5,
                                (s32)(1024.0f / sx) & 0xFFFF, (s32)(1024.0f / sy) & 0xFFFF);
        }
        tx += bm->width + sp->expx;
    }
    gSPEndDisplayList(g++);
    sp->rsp_dl_next = g;
    return start;
}

PORT_C_END
