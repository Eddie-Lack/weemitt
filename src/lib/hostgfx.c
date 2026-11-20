/* glover_host_gfx (glover.h, docs/library.md): the latest display lists translated for the host's own F3DEX2
   interpreter by src/gfx/gfx_hostgfx.c. */
#include <math.h>
#include <string.h>
#include "gfx_hostgfx.h"
#include "lib.h"
#include "ptr32.h"

int lib_host_interp;
static int setUp;

static void set_up(void) {
    unsigned int lo;
    unsigned int hi;

    if (setUp) {
        return;
    }
    port_game_dynamic_range(&lo, &hi);
    gfx_host_setup(PORT_PTR_BASE, PORT_PTR_BASE + 0x1800000, PORT_PTR_DELTA, lo, hi);
    setUp = 1;
}

void lib_host_record(const PortFrameTask *tasks, int count, int cut) {
    set_up();
    gfx_host_record((const GfxTask *)tasks, count, cut);
}

void lib_host_shutdown(void) {
    gfx_host_shutdown();
    lib_host_interp = 0;
    setUp = 0;
}

typedef struct HostCall {
    GloverHostGfx *h;
    int32_t rc;
} HostCall;

static void call_host_gfx(void *arg) {
    HostCall *c = (HostCall *)arg;
    GloverHostGfx *h = c->h;
    Glover *g = lib_game;
    GfxHostRequest rq;
    GfxHostResult out;
    int k;

    set_up();
    if (h->t >= 0.0f) {
        lib_host_interp = 1;
    }
    memset(&rq, 0, sizeof(rq));
    rq.tasks = (const GfxTask *)g->lists;
    rq.count = g->listCount;
    rq.marks = (const GfxMark *)g->marks;
    rq.markCount = g->markCount;
    rq.mask = h->layers & 0xF;
    rq.noFog = (h->layers & GLOVER_LAYER_NO_FOG) != 0;
    rq.t = h->t;
    for (k = 0; k < 4 && h->projection[k] != NULL; k++) {
        rq.projection[k] = h->projection[k];
    }
    rq.generation = g->updates;
    rq.split = (h->flags & GLOVER_HOSTGFX_SPLIT) != 0;
    if (h->flags & GLOVER_HOSTGFX_FOG) {
        rq.hostFog = 1;
        rq.fogColor = h->fogColor;
        rq.fogWord = (uint32_t)(uint16_t)h->fogMultiplier << 16 | (uint16_t)h->fogOffset;
    }
    if (!gfx_host_build(&rq, &out)) {
        c->rc = GLOVER_ERR_MEMORY;
        return;
    }
    h->list = (const GloverGfx *)out.list;
    h->xlu = (const GloverGfx *)out.xlu;
    h->commands = out.commands;
    h->triangles = out.triangles;
    h->textures = out.textures;
    h->skipped = out.skipped;
    h->unknown = out.unknown;
    h->rects = out.rects;
}

GLOVER_API int32_t glover_host_gfx(Glover *g, GloverHostGfx *h) {
    HostCall c;
    int32_t rc;

    if (h == NULL || h->size < GLOVER_HOSTGFX_SIZE_V4) {
        return GLOVER_ERR_ARG;
    }
    h->list = NULL;
    h->xlu = NULL;
    h->commands = h->triangles = h->textures = h->skipped = h->unknown = h->rects = 0;
    if (h->gbi != GLOVER_GBI_F3DEX2 || (h->flags & ~(GLOVER_HOSTGFX_SPLIT | GLOVER_HOSTGFX_FOG))
        || (h->layers & ~(0xFu | GLOVER_LAYER_NO_FOG)) || !(h->layers & 0xF) || isnan(h->t) || g == NULL
        || g != lib_game || g->listCount <= 0) {
        return GLOVER_ERR_ARG;
    }
    if ((h->flags & GLOVER_HOSTGFX_FOG) && (h->size < sizeof(GloverHostGfx) || (h->layers & GLOVER_LAYER_NO_FOG))) {
        return GLOVER_ERR_ARG;
    }
    c.h = h;
    c.rc = GLOVER_OK;
    rc = enter(g, call_host_gfx, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}
