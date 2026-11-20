/* What each TMEM load came from, and what a tile samples. gfx_tex.c decodes and caches the textures with this; the
   build without OpenGL (RENDERER=hostgfx) uses it alone, for the names of the texture rectangles that the interpolation
   pairs.

   The renderer does not copy texels into a model of TMEM. It records where each load came from in memory and
   decodes a tile's texels straight from there, as the browser port's renderer does. The game's sprite code loads
   tiles with a zero line width, which only works this way. */
#include "gfx_host.h"
#include <string.h>

#include "gfx_internal.h"

#define TMEM_WORDS 512
#define MAX_TEXTURE_SIZE 1024

enum { LOAD_NONE, LOAD_BLOCK, LOAD_TILE, LOAD_TLUT };

/* Where the data at a TMEM address came from. */
typedef struct LoadRecord {
    u32 frame; /* the frame that made the load; a record from an older frame is empty */
    u8 kind;
    u32 src;      /* memory address of the first texel loaded */
    u32 rowBytes; /* LOAD_TILE: bytes from one row to the next in memory */
} LoadRecord;

static struct {
    u32 fmt, siz, width, addr; /* from G_SETTIMG */
} timg;

static LoadRecord tmem[TMEM_WORDS];
static u32 palSrc[16]; /* where each block of 16 palette entries was loaded from; 0 for none */
static u32 frame = 1;

u32 gfx_tex_frame(void) {
    return frame;
}

/* ---- Loads. */

/* Texel size in bytes, times 2: a 4-bit texel is half a byte. */
u32 gfx_tex_size_x2(u32 siz) {
    return 1u << siz;
}

void gfx_tex_begin_frame(void) {
    int i;

    frame++;
    for (i = 0; i < 16; i++) {
        palSrc[i] = 0;
    }
    memset(&timg, 0, sizeof(timg));
}

void gfx_tex_set_image(u32 fmt, u32 siz, u32 width, u32 addr) {
    timg.fmt = fmt;
    timg.siz = siz;
    timg.width = width;
    timg.addr = addr;
}

static void record(u32 tmemAddr, u8 kind, u32 src, u32 rowBytes) {
    LoadRecord *r = &tmem[tmemAddr & (TMEM_WORDS - 1)];

    r->frame = frame;
    r->kind = kind;
    r->src = src;
    r->rowBytes = rowBytes;
}

/* uls, ult and lrs are texels. */
void gfx_tex_load_block(int tile, u32 uls, u32 ult, u32 lrs, u32 dxt) {
    (void)lrs;
    (void)dxt;
    record(gfxRdp.tiles[tile].tmem, LOAD_BLOCK, timg.addr + (ult * timg.width + uls) * gfx_tex_size_x2(timg.siz) / 2, 0);
}

/* The coordinates are 10.2 fixed point. */
void gfx_tex_load_tile(int tile, u32 uls, u32 ult, u32 lrs, u32 lrt) {
    u32 rowBytes = timg.width * gfx_tex_size_x2(timg.siz) / 2;

    (void)lrs;
    (void)lrt;
    record(gfxRdp.tiles[tile].tmem, LOAD_TILE, timg.addr + (ult >> 2) * rowBytes + (uls >> 2) * gfx_tex_size_x2(timg.siz) / 2,
           rowBytes);
}

/* Palettes sit in the upper half of TMEM, one entry per 64-bit word. count is the number of entries minus 1. */
void gfx_tex_load_tlut(int tile, u32 count) {
    const GfxTile *t = &gfxRdp.tiles[tile];
    u32 k;

    record(t->tmem, LOAD_TLUT, timg.addr, 0);
    if (t->tmem < 256) {
        return;
    }
    for (k = 0; k <= count && t->tmem - 256 + k < 256; k += 16) {
        palSrc[(t->tmem - 256 + k) >> 4] = timg.addr + k * 2;
    }
}

/* ---- What a tile samples. */

/* The factor for a tile's shift: 1-10 divide, 11-15 multiply. */
static float shift_factor(int shift) {
    if (shift == 0) {
        return 1.0f;
    }
    if (shift <= 10) {
        return 1.0f / (float)(1 << shift);
    }
    return (float)(1 << (16 - shift));
}

/* The number of texels the shader can reach along one axis of a tile, as the RDP's clamp and wrap allow. */
static int reach(int lo, int hi, int mask, int cm) {
    int clampW = ((((hi >> 2) - (lo >> 2)) & 0x3FF) + 1);
    int n = clampW;

    if (mask) {
        int maskW = 1 << (mask > 10 ? 10 : mask);
        n = (cm & 2) ? (maskW < clampW ? maskW : clampW) : maskW;
    }
    return n < 1 ? 1 : n > MAX_TEXTURE_SIZE ? MAX_TEXTURE_SIZE : n;
}

static void axis_sampler(GfxSampler *samp, int axis, int lo, int hi, int mask, int shift, int cm) {
    int m = mask > 10 ? 10 : mask;

    samp->a[axis] = shift_factor(shift);
    samp->a[2 + axis] = lo / 4.0f;
    samp->b[axis] = (float)hi;
    samp->b[2 + axis] = (float)(((hi >> 2) - (lo >> 2)) & 0x3FF);
    samp->c[axis] = mask ? 1 << m : 0;
    /* The RDP clamps when asked to, and always when the tile does not wrap. */
    samp->c[2 + axis] = (((cm & 2) || !mask) ? 1 : 0) | (((cm & 1) && mask) ? 2 : 0);
}

void gfx_tex_clamp_sampler(GfxSampler *samp, int width, int height) {
    memset(samp, 0, sizeof(*samp));
    axis_sampler(samp, 0, 0, (width - 1) << 2, 0, 0, 2);
    axis_sampler(samp, 1, 0, (height - 1) << 2, 0, 0, 2);
}

int gfx_tex_describe_tile(int tileIndex, GfxTexDesc *desc, GfxSampler *samp) {
    const GfxTile *t = &gfxRdp.tiles[tileIndex & 7];
    const LoadRecord *r = &tmem[t->tmem];
    u32 offset = 0;
    u32 lut = TEX_LUT(gfxRdp.otherH);
    int i;

    if (r->frame != frame || r->kind == LOAD_NONE) {
        /* A tile may start inside a block loaded at a lower TMEM address. */
        int k;
        for (k = t->tmem - 1; k >= 0; k--) {
            if (tmem[k].frame == frame && (tmem[k].kind == LOAD_BLOCK || tmem[k].kind == LOAD_TILE)) {
                break;
            }
        }
        if (k < 0) {
            return 0;
        }
        offset = (t->tmem - k) * 8;
        r = &tmem[k];
    }
    if (r->kind == LOAD_TLUT) {
        return 0;
    }
    memset(desc, 0, sizeof(*desc));
    desc->fmt = t->fmt;
    desc->siz = t->siz;
    desc->width = reach(t->uls, t->lrs, t->masks, t->cms);
    desc->height = reach(t->ult, t->lrt, t->maskt, t->cmt);
    desc->addr = r->src + offset;
    if (r->kind == LOAD_BLOCK) {
        desc->stride = t->line * 8;
        if (desc->stride == 0) {
            desc->stride = (desc->width * gfx_tex_size_x2(t->siz) + 1) / 2;
        }
    } else {
        desc->stride = r->rowBytes;
    }
    /* With a TLUT, any 4-bit or 8-bit texture is colour-indexed. */
    if (lut != 0 && t->siz <= SIZ_8B) {
        desc->fmt = FMT_CI;
    }
    if (desc->fmt == FMT_CI) {
        desc->palType = lut ? lut : 2;
        if (t->siz == SIZ_4B) {
            desc->pal[0] = palSrc[t->pal];
        } else {
            for (i = 0; i < 16; i++) {
                desc->pal[i] = palSrc[i];
            }
        }
    }
    memset(samp, 0, sizeof(*samp));
    axis_sampler(samp, 0, t->uls, t->lrs, t->masks, t->shifts, t->cms);
    axis_sampler(samp, 1, t->ult, t->lrt, t->maskt, t->shiftt, t->cmt);
    return 1;
}

