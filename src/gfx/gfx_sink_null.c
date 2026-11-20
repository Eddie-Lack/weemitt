/* The stand-in for gfx_render.c in a build that only translates the lists for the host (RENDERER=hostgfx,
   gfx_hostgfx.c): gfx_dl.c's walk calls these, and nothing is drawn. These are the symbols that gfx_dl.c and
   gfx_interp.c take from it. gfx_tmem.c records the texture loads in both builds. */
#include "gfx_internal.h"

GfxLayerState gfxLayer;

void gfx_state_changed(void) {
}

int gfx_prepare(int kind, int tile, const GfxTexDesc *direct, const GfxSampler *directSampler, int wide) {
    (void)kind;
    (void)tile;
    (void)direct;
    (void)directSampler;
    (void)wide;
    return 0;
}

void gfx_add_vertices(const GfxVertex *v, int count) {
    (void)v;
    (void)count;
}

void gfx_clear_color(const float rgba[4]) {
    (void)rgba;
}

void gfx_clear_depth(void) {
}
