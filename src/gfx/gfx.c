/* The renderer's public functions (gfx.h): the frame buffer objects and the final picture. gfx_mem.c reads the
   game's memory.

   A frame is drawn into a frame buffer object, multisampled when the GPU allows. Its size is the largest 4:3 area
   that fits the window times the resolution scale, or the window's own shape in the widescreen mode that widens
   the view. It is then copied to the window: to the middle of it with black around (4:3), or over all of it. */
#include "gfx_host.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gfx_internal.h"

static struct {
    int ready;
    int width, height;  /* the frame's size in pixels */
    int samples;        /* samples per pixel of the render target; 0 when not multisampled */
    int wantSamples;    /* the samples the options ask for, cut to what the GPU allows */
    int builtFor;       /* the wantSamples that the targets were built for (they may have fewer) */
    int maxSamples, maxSize;
    GfxOptions opt;
    int envSamples;     /* GLOVER_MSAA when set, else -1 */
    int envFilter;      /* GLOVER_GFX_FILTER=bilinear is set */
    int drawW, drawH;   /* the window's size, as told or as at the last draw */
    GLuint fbo, color, depth;
    GLuint resolveFbo, resolveColor; /* a single-sampled copy of a multisampled frame */
    int haveFrame;
    int logStats;       /* GLOVER_GFX_STATS: log each frame's statistics */
} F;

/* The framebuffer that gfx_draw_frame draws the window's picture into (gfx_set_target). */
static GLuint target;

void gfx_set_target(unsigned int fbo) {
    target = fbo;
}

void gfx_set_framebuffers(unsigned int cfb0, unsigned int cfb1) {
    gfx_dl_set_framebuffers(cfb0, cfb1);
}

/* ---- Render targets. */

static void free_targets(void) {
    if (F.fbo) {
        glDeleteFramebuffers(1, &F.fbo);
    }
    if (F.resolveFbo) {
        glDeleteFramebuffers(1, &F.resolveFbo);
    }
    if (F.color) {
        glDeleteRenderbuffers(1, &F.color);
    }
    if (F.depth) {
        glDeleteRenderbuffers(1, &F.depth);
    }
    if (F.resolveColor) {
        glDeleteRenderbuffers(1, &F.resolveColor);
    }
    F.fbo = F.resolveFbo = F.color = F.depth = F.resolveColor = 0;
    F.haveFrame = 0;
}

static GLuint make_renderbuffer(GLenum format, int samples, int w, int h) {
    GLuint rb;

    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    if (samples > 0) {
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, format, w, h);
    } else {
        glRenderbufferStorage(GL_RENDERBUFFER, format, w, h);
    }
    return rb;
}

/* Builds the frame buffers. Returns 0 when the GPU cannot (too large, or out of memory): a buffer it could not make
   leaves the frame buffer incomplete. */
static int make_targets(int w, int h, int samples) {
    int bad;

    free_targets();
    F.width = w;
    F.height = h;
    F.samples = samples;
    glGenFramebuffers(1, &F.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, F.fbo);
    F.color = make_renderbuffer(GL_RGBA8, samples, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, F.color);
    F.depth = make_renderbuffer(GL_DEPTH_COMPONENT32F, samples, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, F.depth);
    bad = glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE;
    if (!bad && samples > 0) {
        glGenFramebuffers(1, &F.resolveFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, F.resolveFbo);
        F.resolveColor = make_renderbuffer(GL_RGBA8, 0, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, F.resolveColor);
        bad = glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE;
    }
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    if (bad) {
        free_targets();
        return 0;
    }
    return 1;
}

/* Makes the frame buffers for a size and the wanted samples, if they are not there already. The GPU may refuse a
   size: the samples then halve until it accepts them, down to none. */
static int ensure_targets(int w, int h) {
    int samples;

    if (F.fbo && F.width == w && F.height == h && F.builtFor == F.wantSamples) {
        return 1;
    }
    F.builtFor = F.wantSamples;
    for (samples = F.wantSamples; samples >= 2; samples /= 2) {
        if (make_targets(w, h, samples)) {
            gfx_render_set_msaa(1);
            return 1;
        }
    }
    if (make_targets(w, h, 0)) {
        gfx_render_set_msaa(0);
        return 1;
    }
    gfx_log("gfx: cannot create a %dx%d frame buffer", w, h);
    return 0;
}

/* The framebuffer that holds the finished frame, single-sampled. */
static GLuint frame_fbo(void) {
    return F.samples > 0 ? F.resolveFbo : F.fbo;
}

/* ---- Public functions. */

/* Takes the options (after the environment variables) as they are now. */
static void apply_options(void) {
    int samples = F.envSamples >= 0 ? F.envSamples : F.opt.msaa;
    int filter = F.opt.filter;

    if (F.opt.scale < 50) {
        F.opt.scale = 50;
    }
    if (F.opt.scale > 400) {
        F.opt.scale = 400;
    }
    if (samples > F.maxSamples) {
        samples = F.maxSamples;
    }
    /* Samples are 2, 4 or 8. */
    F.wantSamples = samples >= 8 ? 8 : samples >= 4 ? 4 : samples >= 2 ? 2 : 0;
    if (F.envFilter) {
        filter = GFX_FILTER_BILINEAR;
    }
    gfx_render_set_filter(filter);
}

void gfx_set_options(const GfxOptions *options) {
    if (!F.ready) {
        return;
    }
    F.opt = *options;
    apply_options();
}

int gfx_max_samples(void) {
    return F.maxSamples;
}

int gfx_init(void) {
    const char *msaa = gfx_getenv("GLOVER_MSAA");
    const char *filter = gfx_getenv("GLOVER_GFX_FILTER");
    GLint maxSamples = 0;
    GLint maxSize = 0;

    memset(&F, 0, sizeof(F));
    if (gfx_gl_load() != 0 || gfx_render_init() != 0) {
        gfx_log("gfx: OpenGL 3.3 is not usable; nothing will be drawn");
        return -1;
    }
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxSize);
    F.maxSamples = maxSamples < 2 ? 0 : maxSamples;
    F.maxSize = maxSize > 0 ? maxSize : 2048;
    F.envSamples = msaa ? atoi(msaa) : -1;
    F.envFilter = filter && strcmp(filter, "bilinear") == 0;
    F.opt.scale = 100;
    F.opt.msaa = 4;
    F.opt.filter = GFX_FILTER_N64;
    F.opt.widescreen = GFX_WIDE_OFF;
    F.logStats = gfx_getenv("GLOVER_GFX_STATS") != NULL;
    F.ready = 1;
    apply_options();
    return 0;
}

/* Where a frame goes. The frame buffer is bw x bh (before the resolution scale); the window gets it in the
   rectangle at (dx, dy) of size dw x dh. wide is the factor by which the 3D view is widened (1 for none). */
typedef struct Layout {
    int bw, bh;
    int dx, dy, dw, dh;
    float wide;
} Layout;

static void make_layout(Layout *l, int windowW, int windowH) {
    /* The largest 4:3 area that fits. */
    if (windowW * 3 >= windowH * 4) {
        l->bh = windowH;
        l->bw = (windowH * 4 + 1) / 3;
    } else {
        l->bw = windowW;
        l->bh = (windowW * 3 + 2) / 4;
    }
    l->wide = 1.0f;
    l->dw = l->bw;
    l->dh = l->bh;
    if (F.opt.widescreen == GFX_WIDE_STRETCH) {
        l->dw = windowW;
        l->dh = windowH;
    } else if (F.opt.widescreen == GFX_WIDE_HORPLUS && windowW * 3 > windowH * 4) {
        l->bw = windowW;
        l->bh = windowH;
        l->dw = windowW;
        l->dh = windowH;
        l->wide = windowW * 3.0f / (windowH * 4.0f);
    }
    l->dx = (windowW - l->dw) / 2;
    l->dy = (windowH - l->dh) / 2;
}

/* The frame buffer's size for a layout and the resolution scale, within what the GPU allows. */
static void target_size(const Layout *l, int *w, int *h) {
    float scale = F.opt.scale / 100.0f;
    float limit = (float)F.maxSize;
    float fw = l->bw * scale;
    float fh = l->bh * scale;

    if (fw > limit) {
        fh *= limit / fw;
        fw = limit;
    }
    if (fh > limit) {
        fw *= limit / fh;
        fh = limit;
    }
    *w = fw < 1.0f ? 1 : (int)(fw + 0.5f);
    *h = fh < 1.0f ? 1 : (int)(fh + 0.5f);
}

/* ---- The layer's targets: multisampled colour and depth to draw into, a colour texture that the colour resolves
   into, and a depth texture that the depth pass draws into. */

static struct {
    int width, height, samples, builtFor;
    GLuint fbo, color, depth;        /* the drawing target; color is 0 when the colour texture is drawn into */
    GLuint colorFbo, colorTex;
    GLuint depthFbo, depthTex;
    int have;                        /* a layer was drawn into the textures */
} L;

static void free_layer_targets(void) {
    GLuint fbos[3] = {L.fbo, L.colorFbo, L.depthFbo};
    GLuint rbs[2] = {L.color, L.depth};
    GLuint texs[2] = {L.colorTex, L.depthTex};

    glDeleteFramebuffers(3, fbos);
    glDeleteRenderbuffers(2, rbs);
    glDeleteTextures(2, texs);
    memset(&L, 0, sizeof(L));
}

static GLuint make_texture(GLint format, GLenum pixelFormat, GLenum type, int w, int h) {
    GLuint t;

    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, pixelFormat, type, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

static int make_layer_targets(int w, int h, int samples) {
    int bad;

    free_layer_targets();
    L.width = w;
    L.height = h;
    L.samples = samples;
    L.colorTex = make_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h);
    L.depthTex = make_texture(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, w, h);
    glGenFramebuffers(1, &L.colorFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, L.colorFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, L.colorTex, 0);
    if (samples == 0) {
        /* The colour texture is the drawing target, with a depth buffer of its own. */
        L.depth = make_renderbuffer(GL_DEPTH_COMPONENT32F, 0, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, L.depth);
        L.fbo = L.colorFbo;
        L.colorFbo = 0;
    }
    bad = glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE;
    if (!bad && samples > 0) {
        glGenFramebuffers(1, &L.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, L.fbo);
        L.color = make_renderbuffer(GL_RGBA8, samples, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, L.color);
        L.depth = make_renderbuffer(GL_DEPTH_COMPONENT32F, samples, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, L.depth);
        bad = glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE;
    }
    if (!bad) {
        glGenFramebuffers(1, &L.depthFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, L.depthFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, L.depthTex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        bad = glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE;
    }
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    if (bad) {
        free_layer_targets();
        return 0;
    }
    return 1;
}

static int ensure_layer_targets(int w, int h) {
    int samples;

    if (L.depthFbo && L.width == w && L.height == h && L.builtFor == F.wantSamples) {
        return 1;
    }
    for (samples = F.wantSamples; samples >= 2; samples /= 2) {
        if (make_layer_targets(w, h, samples)) {
            L.builtFor = F.wantSamples;
            return 1;
        }
    }
    if (make_layer_targets(w, h, 0)) {
        L.builtFor = F.wantSamples;
        return 1;
    }
    gfx_log("gfx: cannot create a %dx%d layer", w, h);
    return 0;
}

int gfx_draw_layer(const GfxTask *tasks, int count, const GfxMark *marks, int markCount, unsigned int mask,
                   int noFog, int drawableW, int drawableH, GfxLayerOut *out) {
    Layout lay;
    int w;
    int h;
    int i;

    memset(out, 0, sizeof(*out));
    L.have = 0;
    if (!F.ready || count <= 0 || drawableW <= 0 || drawableH <= 0) {
        return 0;
    }
    /* The 4:3 screen stretched over the whole drawable, as GFX_WIDE_STRETCH shows it. */
    memset(&lay, 0, sizeof(lay));
    lay.bw = lay.dw = drawableW;
    lay.bh = lay.dh = drawableH;
    lay.wide = 1.0f;
    target_size(&lay, &w, &h);
    if (!ensure_layer_targets(w, h)) {
        return 0;
    }
    gfxLayer.on = 1;
    gfxLayer.marks = marks;
    gfxLayer.count = markCount;
    gfxLayer.mask = mask;
    gfxLayer.noFog = noFog;
    gfxLayer.kind = 0;
    gfxLayer.next = 0;
    gfx_mem_next_frame();
    gfx_render_set_msaa(L.samples > 0);
    gfx_dl_set_wide(1.0f);
    gfx_render_begin(w, h, 1.0f);
    gfx_interp_begin_draw();
    gfx_dl_begin_frame();
    for (i = 0; i < count; i++) {
        gfx_dl_run_task(tasks[i].addr, tasks[i].sprite2d);
    }
    gfx_interp_end_draw();
    glBindFramebuffer(GL_FRAMEBUFFER, L.fbo);
    gfx_render_end();
    if (L.samples > 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, L.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, L.colorFbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, L.depthFbo);
    gfx_render_depth_pass();
    gfx_tex_end_frame();
    gfxLayer.on = 0;
    gfxLayer.marks = NULL;
    gfx_render_set_msaa(F.samples > 0);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    L.have = 1;
    out->colorTexture = L.colorTex;
    out->depthTexture = L.depthTex;
    out->width = w;
    out->height = h;
    return 1;
}

int gfx_composite_layer(int w, int h, const float planes[2], const float projZ[2], float bias, int writeDepth) {
    if (!F.ready || !L.have) {
        return 0;
    }
    return gfx_render_composite(L.colorTex, L.depthTex, L.width, L.height, w, h, planes, projZ, bias, writeDepth);
}

void gfx_set_drawable_size(int drawableW, int drawableH) {
    F.drawW = drawableW;
    F.drawH = drawableH;
}

void gfx_interp_record(const GfxTask *tasks, int count, int cut) {
    Layout lay;

    if (!F.ready || count <= 0) {
        return;
    }
    uint64_t t0 = gfx_ticks();

    gfx_mem_next_frame();
    /* The recording must widen the view as the drawing will, or it culls other parts of the scene. */
    if (F.drawW > 0 && F.drawH > 0) {
        make_layout(&lay, F.drawW, F.drawH);
        gfx_dl_set_wide(lay.wide);
    }
    gfx_dl_record(tasks, count, cut);
    if (F.logStats) {
        gfx_log("gfx: %.3f ms to record the matrices", (gfx_ticks() - t0) * 1000.0 / gfx_ticks_per_second());
    }
}

void gfx_draw_frame(const GfxTask *tasks, int count, int drawableW, int drawableH) {
    Layout lay;
    int w;
    int h;
    int i;

    if (!F.ready || drawableW <= 0 || drawableH <= 0) {
        return;
    }
    make_layout(&lay, drawableW, drawableH);
    target_size(&lay, &w, &h);
    F.drawW = drawableW;
    F.drawH = drawableH;
    /* Where the N64's 4:3 screen lies in the window, for the Developer overlays. With Hor+ it is the middle part of
       the widened frame. */
    gfxScene.frameW = (float)lay.dw / lay.wide / drawableW;
    gfxScene.frameX = (lay.dx + (lay.dw - lay.dw / lay.wide) / 2.0f) / drawableW;
    gfxScene.frameY = (float)lay.dy / drawableH;
    gfxScene.frameH = (float)lay.dh / drawableH;
    if (count > 0 && ensure_targets(w, h)) {
        uint64_t t0 = gfx_ticks();
        uint64_t t1;

        gfx_mem_next_frame();
        gfx_dl_set_wide(lay.wide);
        gfx_render_begin(w, h, lay.wide);
        gfx_interp_begin_draw();
        gfx_dl_begin_frame();
        for (i = 0; i < count; i++) {
            gfx_dl_run_task(tasks[i].addr, tasks[i].sprite2d);
        }
        gfx_interp_end_draw();
        t1 = gfx_ticks();
        glBindFramebuffer(GL_FRAMEBUFFER, F.fbo);
        gfx_render_end();
        gfx_debug_draw(F.fbo, w, h);
        if (F.samples > 0) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, F.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, F.resolveFbo);
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        gfx_tex_end_frame();
        F.haveFrame = 1;
        if (F.logStats) {
            gfx_log("gfx: %dx%d, %d samples: %d triangles, %d rectangles, %d draws, %d states, %d new programs, "
                    "%d texture uploads; %.3f ms to interpret, %.3f ms to submit", w, h, F.samples,
                    gfxStats.triangles, gfxStats.rects, gfxStats.draws, gfxStats.states, gfxStats.programs,
                    gfxStats.textureUploads, (t1 - t0) * 1000.0 / gfx_ticks_per_second(),
                    (gfx_ticks() - t1) * 1000.0 / gfx_ticks_per_second());
        }
    }

    /* The window: black, with the frame in the rectangle that the layout gives. */
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, drawableW, drawableH);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (F.haveFrame) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, frame_fbo());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
        glBlitFramebuffer(0, 0, F.width, F.height, lay.dx, lay.dy, lay.dx + lay.dw, lay.dy + lay.dh,
                          GL_COLOR_BUFFER_BIT,
                          (F.width == lay.dw && F.height == lay.dh) ? GL_NEAREST : GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        /* The frame's alpha is the game's; the window's must be opaque. */
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
}

/* Reads w x h pixels of the bound read framebuffer as RGBA rows from the top. */
static unsigned char *read_rows(int w, int h) {
    unsigned char *pixels;
    unsigned char *row;
    int stride = w * 4;
    int y;

    pixels = malloc((size_t)stride * h);
    row = malloc(stride);
    if (pixels == NULL || row == NULL) {
        free(pixels);
        free(row);
        return NULL;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    /* OpenGL's rows start at the bottom. */
    for (y = 0; y < h / 2; y++) {
        memcpy(row, pixels + y * stride, stride);
        memcpy(pixels + y * stride, pixels + (h - 1 - y) * stride, stride);
        memcpy(pixels + (h - 1 - y) * stride, row, stride);
    }
    free(row);
    return pixels;
}

unsigned char *gfx_read_frame(int *w, int *h) {
    unsigned char *pixels;

    if (!F.haveFrame) {
        return NULL;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, frame_fbo());
    pixels = read_rows(F.width, F.height);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
    *w = F.width;
    *h = F.height;
    return pixels;
}

unsigned char *gfx_read_window(int *w, int *h) {
    unsigned char *pixels;

    if (!F.haveFrame) {
        return NULL;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
    pixels = read_rows(F.drawW, F.drawH);
    *w = F.drawW;
    *h = F.drawH;
    return pixels;
}

void gfx_shutdown(void) {
    if (!F.ready) {
        return;
    }
    free_targets();
    free_layer_targets();
    gfx_debug_shutdown();
    gfx_interp_shutdown();
    gfx_tex_shutdown();
    gfx_render_shutdown();
    F.ready = 0;
}
