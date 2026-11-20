/* The renderer in the library (glover.h, docs/library.md): src/gfx draws the latest display lists with the host's
   OpenGL 3.3 context. The host's loader reaches the renderer through a trampoline, which adds the host's pointer. */
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "gfx_gl.h"
#include "gfx_host.h"
#include "lib.h"
#include "ptr32.h"
#include "hostworld.h"

#ifndef GL_LESS
#define GL_LESS 0x0201
#endif

static GloverGlLoader hostLoad;
static void *hostUser;
static int glReady;
/* The framebuffer of the latest glover_gl_draw: each call leaves it bound. */
static unsigned int glTarget;
int lib_widescreen;

static void *trampoline(const char *name) {
    return hostLoad(hostUser, name);
}

/* Puts back the initial value of each OpenGL state that the renderer changes (glover.h), except the viewport and the
   scissor box. */
static void restore_gl_state(void) {
    int i;

    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0.0f, 0.0f);
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    for (i = 1; i >= 0; i--) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, glTarget);
}

typedef struct GlCall {
    int32_t rc;
    uint32_t fbo;
    int32_t w, h;
    const GloverGlOptions *opt;
    uint8_t *rgba;
    uint32_t bytes;
    int32_t *outW, *outH;
} GlCall;

static void call_init(void *arg) {
    GlCall *c = (GlCall *)arg;
    unsigned int a;
    unsigned int b;

    gfx_set_loader(trampoline);
    if (gfx_init() != 0) {
        c->rc = GLOVER_ERR_GL;
        return;
    }
    port_game_framebuffers(&a, &b);
    gfx_set_framebuffers(a, b);
    gfx_set_memory_range(PORT_PTR_BASE, PORT_PTR_BASE + 0x1800000);
    gfx_set_memory_base(PORT_PTR_DELTA);
    glReady = 1;
    glTarget = 0;
    restore_gl_state();
}

GLOVER_API int32_t glover_gl_init(Glover *g, GloverGlLoader load, void *user) {
    GlCall c;
    int32_t rc;

    if (load == NULL || glReady) {
        return GLOVER_ERR_ARG;
    }
    hostLoad = load;
    hostUser = user;
    memset(&c, 0, sizeof(c));
    rc = enter(g, call_init, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

static void call_options(void *arg) {
    GlCall *c = (GlCall *)arg;
    GfxOptions o;

    /* A host's view holds its own aspect: the wider view would widen the picture a second time. */
    if (c->opt->widescreen == GFX_WIDE_HORPLUS && hw_view_draws()) {
        c->rc = GLOVER_ERR_ARG;
        return;
    }
    o.scale = c->opt->scale;
    o.msaa = c->opt->msaa;
    o.filter = c->opt->filter;
    o.widescreen = c->opt->widescreen;
    gfx_set_options(&o);
    lib_widescreen = o.widescreen;
    restore_gl_state();
}

GLOVER_API int32_t glover_gl_options(Glover *g, const GloverGlOptions *o) {
    GlCall c;
    int32_t rc;

    if (o == NULL || o->size < sizeof(GloverGlOptions) || !glReady || o->scale < 50 || o->scale > 400
        || (o->msaa != 0 && o->msaa != 2 && o->msaa != 4 && o->msaa != 8) || o->filter < GFX_FILTER_N64
        || o->filter > GFX_FILTER_NEAREST || o->widescreen < GFX_WIDE_OFF || o->widescreen > GFX_WIDE_HORPLUS) {
        return GLOVER_ERR_ARG;
    }
    memset(&c, 0, sizeof(c));
    c.opt = o;
    rc = enter(g, call_options, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

static void call_draw(void *arg) {
    GlCall *c = (GlCall *)arg;
    Glover *g = lib_game;

    glTarget = c->fbo;
    gfx_set_target(c->fbo);
    gfx_interp_set(-1.0f);
    gfx_draw_frame((const GfxTask *)g->lists, g->listCount, c->w, c->h);
    restore_gl_state();
}

GLOVER_API int32_t glover_gl_draw(Glover *g, uint32_t fbo, int32_t w, int32_t h) {
    GlCall c;

    if (!glReady || w <= 0 || h <= 0) {
        return GLOVER_ERR_ARG;
    }
    memset(&c, 0, sizeof(c));
    c.fbo = fbo;
    c.w = w;
    c.h = h;
    return enter(g, call_draw, &c);
}

static void call_read(void *arg) {
    GlCall *c = (GlCall *)arg;
    int w;
    int h;
    unsigned char *p = gfx_read_frame(&w, &h);

    restore_gl_state();
    if (p == NULL) {
        *c->outW = 0;
        *c->outH = 0;
        c->rc = GLOVER_ERR_ARG;
        return;
    }
    *c->outW = w;
    *c->outH = h;
    if ((uint64_t)w * h * 4 > c->bytes) {
        c->rc = GLOVER_ERR_ARG;
    } else {
        memcpy(c->rgba, p, (size_t)w * h * 4);
    }
    free(p);
}

GLOVER_API int32_t glover_gl_read_frame(Glover *g, uint8_t *rgba, uint32_t bytes, int32_t *w, int32_t *h) {
    GlCall c;
    int32_t rc;

    if (rgba == NULL || w == NULL || h == NULL) {
        return GLOVER_ERR_ARG;
    }
    if (!glReady) {
        *w = 0;
        *h = 0;
        return GLOVER_ERR_ARG;
    }
    memset(&c, 0, sizeof(c));
    c.rgba = rgba;
    c.bytes = bytes;
    c.outW = w;
    c.outH = h;
    rc = enter(g, call_read, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

/* The latest layer: 1 when the latest glover_gl_draw_layer drew it, and its projection's near and far planes and
   terms m[2][2] and m[3][2]. */
static int layerHave;
static float layerPlanes[2];
static float layerProjZ[2];

typedef struct LayerCall {
    GloverGlLayer *l;
    int32_t rc;
} LayerCall;

static void call_draw_layer(void *arg) {
    LayerCall *c = (LayerCall *)arg;
    Glover *g = lib_game;
    GloverGlLayer *l = c->l;
    GfxLayerOut out;

    gfx_interp_set(-1.0f);
    if (g->listCount <= 0 || !gfx_draw_layer((const GfxTask *)g->lists, g->listCount, (const GfxMark *)g->marks,
                                             g->markCount, l->layers & 0xF, (l->layers & GLOVER_LAYER_NO_FOG) != 0,
                                             l->width, l->height, &out)) {
        c->rc = GLOVER_ERR_ARG;
    }
    layerHave = c->rc == GLOVER_OK;
    /* The composite uses the projection of this layer, whatever the view is by then. */
    hw_view_planes(&layerPlanes[0], &layerPlanes[1], layerProjZ);
    l->colorTexture = out.colorTexture;
    l->depthTexture = out.depthTexture;
    l->texWidth = out.width;
    l->texHeight = out.height;
    l->nearZ = layerPlanes[0];
    l->farZ = layerPlanes[1];
    l->projZ[0] = layerProjZ[0];
    l->projZ[1] = layerProjZ[1];
    restore_gl_state();
}

GLOVER_API int32_t glover_gl_draw_layer(Glover *g, GloverGlLayer *l) {
    LayerCall c;
    int32_t rc;

    if (l == NULL || l->size < sizeof(GloverGlLayer) || !glReady || l->width <= 0 || l->height <= 0
        || (l->layers & ~(0xFu | GLOVER_LAYER_NO_FOG)) || !(l->layers & 0xF)) {
        layerHave = 0;
        return GLOVER_ERR_ARG;
    }
    c.l = l;
    c.rc = GLOVER_OK;
    rc = enter(g, call_draw_layer, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

typedef struct CompositeCall {
    uint32_t fbo;
    int32_t w, h;
    float bias;
    uint32_t flags;
    int32_t rc;
} CompositeCall;

static void call_composite(void *arg) {
    CompositeCall *c = (CompositeCall *)arg;

    if (!layerHave) {
        c->rc = GLOVER_ERR_ARG;
        return;
    }
    glTarget = c->fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    if (!gfx_composite_layer(c->w, c->h, layerPlanes, layerProjZ, c->bias, (c->flags & GLOVER_COMPOSITE_DEPTH) != 0)) {
        c->rc = GLOVER_ERR_ARG;
    }
    restore_gl_state();
}

GLOVER_API int32_t glover_gl_composite(Glover *g, uint32_t fbo, int32_t w, int32_t h, float bias, uint32_t flags) {
    CompositeCall c;
    int32_t rc;

    if (!glReady || w <= 0 || h <= 0 || !(bias >= 0.0f && bias < 1.0f) || (flags & ~(uint32_t)GLOVER_COMPOSITE_DEPTH)) {
        return GLOVER_ERR_ARG;
    }
    c.fbo = fbo;
    c.w = w;
    c.h = h;
    c.bias = bias;
    c.flags = flags;
    c.rc = GLOVER_OK;
    rc = enter(g, call_composite, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

/* The renderer's objects need no game state: this runs also after glover_destroy or a fatal error, without enter. It
   still keeps out a second thread or a callback. */
GLOVER_API int32_t glover_gl_shutdown(Glover *g) {
    if (g == NULL || g != lib_game) {
        return GLOVER_ERR_ARG;
    }
    if (__atomic_exchange_n(&g->busy, 1, __ATOMIC_ACQUIRE)) {
        return GLOVER_ERR_REENTER;
    }
    if (glReady) {
        gfx_shutdown();
        restore_gl_state();
        glReady = 0;
        layerHave = 0;
    }
    __atomic_store_n(&g->busy, 0, __ATOMIC_RELEASE);
    return GLOVER_OK;
}
