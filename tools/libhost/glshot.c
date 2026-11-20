/* The picture test of the library (tools/libcheck.py --gl). It opens a hidden OpenGL 3.3 core window with SDL,
   makes a 640x480 framebuffer (RGBA8, depth 24), runs a level through the library and draws the frames that
   weemitt's --shot draws. Each picture is written as raw RGBA rows from the top, PREFIX_FRAME.rgba. After each call
   of the renderer it checks the OpenGL state that glover.h promises. It also checks the renderer's argument checks
   and that glover_gl_shutdown works after glover_destroy. Only this test uses SDL.

   With --scene FILE it runs a host scene (src/port/hostscene.c, docs/host-world.md) through the host-world calls:
   its triangles, places and views. It then also checks that glover_gl_options refuses the wider view (widescreen 2)
   while a drawing view is set.

   With --layer MASK it also draws a layer (glover_gl_draw_layer) of the GLOVER_LAYER_ bits in MASK at each shot,
   writes it as PREFIX_layer_FRAME.rgba (raw RGBA rows from the top) and its depth as PREFIX_layerdepth_FRAME.f32
   (floats, rows from the top), and prints the alpha at the corners. With a scene whose view draws, it also prints
   the glove's projected pixel, the layer's alpha and depth there, and the depth made linear against the glove's
   distance along the view; it then paints the host's framebuffer red, in front of the glove left of that pixel and
   behind it to the right, composites the layer over it (glover_gl_composite), prints the colours on either side and
   writes the result as PREFIX_composite_FRAME.rgba. It composites once more with GLOVER_COMPOSITE_DEPTH and prints
   the depth on either side, and then checks that a layer that failed to draw cannot be composited.

   usage: glover-glshot --rom FILE --level N --headless FRAMES --shot FRAME... --prefix PATH [--save FILE]
          [--scene FILE] [--layer MASK]
   The library: $GLOVER_LIB, or libglover.so next to the program. */
#include <math.h>
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "glover.h"
#include "hostscene.h"

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_RGBA8 0x8058
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_VIEWPORT 0x0BA2
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_SCISSOR_TEST 0x0C11
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_CULL_FACE 0x0B44
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_DITHER 0x0BD0
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_DEPTH_FUNC 0x0B74
#define GL_LESS 0x0201
#define GL_DEPTH_CLEAR_VALUE 0x0B73
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_POLYGON_OFFSET_FACTOR 0x8038
#define GL_POLYGON_OFFSET_UNITS 0x2A00
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_SRC_ALPHA 0x80CB
#define GL_BLEND_DST_ALPHA 0x80CA
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_DEPTH_COMPONENT 0x1902
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100

static void (*glGenFramebuffers_)(GLsizei, GLuint *);
static void (*glBindFramebuffer_)(GLenum, GLuint);
static void (*glGenRenderbuffers_)(GLsizei, GLuint *);
static void (*glBindRenderbuffer_)(GLenum, GLuint);
static void (*glRenderbufferStorage_)(GLenum, GLenum, GLsizei, GLsizei);
static void (*glFramebufferRenderbuffer_)(GLenum, GLenum, GLenum, GLuint);
static GLenum (*glCheckFramebufferStatus_)(GLenum);
static void (*glGetIntegerv_)(GLenum, GLint *);
static void (*glGetFloatv_)(GLenum, float *);
static unsigned char (*glIsEnabled_)(GLenum);
static void (*glActiveTexture_)(GLenum);
static void (*glBindTexture_)(GLenum, GLuint);
static void (*glGetTexImage_)(GLenum, GLint, GLenum, GLenum, void *);
static void (*glEnable_)(GLenum);
static void (*glDisable_)(GLenum);
static void (*glScissor_)(GLint, GLint, GLsizei, GLsizei);
static void (*glClearColor_)(float, float, float, float);
static void (*glClearDepth_)(double);
static void (*glClear_)(unsigned int);
static void (*glReadPixels_)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);

/* The state that glover.h promises after each call of the renderer: OpenGL's initial values, with fb bound to
   GL_FRAMEBUFFER. With w > 0 also the viewport 0, 0, w, h. Prints the first difference; returns 1 for one. */
static int check_state(const char *after, GLint fb, int w, int h) {
    static const struct {
        GLenum name;
        GLint value;
    } ints[] = {
        {GL_DEPTH_WRITEMASK, 1}, {GL_DEPTH_FUNC, GL_LESS}, {GL_BLEND_SRC_RGB, 1}, {GL_BLEND_DST_RGB, 0},
        {GL_BLEND_SRC_ALPHA, 1}, {GL_BLEND_DST_ALPHA, 0}, {GL_ACTIVE_TEXTURE, GL_TEXTURE0}, {GL_TEXTURE_BINDING_2D, 0},
        {GL_ARRAY_BUFFER_BINDING, 0}, {GL_RENDERBUFFER_BINDING, 0}, {GL_CURRENT_PROGRAM, 0},
        {GL_VERTEX_ARRAY_BINDING, 0}, {GL_PACK_ALIGNMENT, 4}, {GL_UNPACK_ALIGNMENT, 4},
    };
    static const struct {
        GLenum cap;
        int on;
    } caps[] = {
        {GL_SCISSOR_TEST, 0}, {GL_DEPTH_TEST, 0}, {GL_BLEND, 0}, {GL_CULL_FACE, 0}, {GL_POLYGON_OFFSET_FILL, 0},
        {GL_DITHER, 1},
    };
    GLint v[4];
    float f[4];
    size_t i;

    for (i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
        glGetIntegerv_(ints[i].name, v);
        if (v[0] != ints[i].value) {
            printf("after %s: the GL state is not the promised one: 0x%04X is %d, not %d\n", after, ints[i].name,
                   v[0], ints[i].value);
            return 1;
        }
    }
    glActiveTexture_(GL_TEXTURE0 + 1);
    glGetIntegerv_(GL_TEXTURE_BINDING_2D, v);
    glActiveTexture_(GL_TEXTURE0);
    if (v[0] != 0) {
        printf("after %s: the GL state is not the promised one: texture %d on unit 1\n", after, v[0]);
        return 1;
    }
    for (i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        if (!glIsEnabled_(caps[i].cap) != !caps[i].on) {
            printf("after %s: the GL state is not the promised one: 0x%04X is %s\n", after, caps[i].cap,
                   caps[i].on ? "off" : "on");
            return 1;
        }
    }
    glGetIntegerv_(GL_COLOR_WRITEMASK, v);
    if (!v[0] || !v[1] || !v[2] || !v[3]) {
        printf("after %s: the GL state is not the promised one: the colour mask\n", after);
        return 1;
    }
    glGetFloatv_(GL_COLOR_CLEAR_VALUE, f);
    if (f[0] != 0.0f || f[1] != 0.0f || f[2] != 0.0f || f[3] != 0.0f) {
        printf("after %s: the GL state is not the promised one: the clear colour\n", after);
        return 1;
    }
    glGetFloatv_(GL_DEPTH_CLEAR_VALUE, f);
    glGetFloatv_(GL_POLYGON_OFFSET_FACTOR, f + 1);
    glGetFloatv_(GL_POLYGON_OFFSET_UNITS, f + 2);
    if (f[0] != 1.0f || f[1] != 0.0f || f[2] != 0.0f) {
        printf("after %s: the GL state is not the promised one: the depth clear value or the polygon offset\n",
               after);
        return 1;
    }
    glGetIntegerv_(GL_FRAMEBUFFER_BINDING, v);
    glGetIntegerv_(GL_READ_FRAMEBUFFER_BINDING, v + 1);
    if (v[0] != fb || v[1] != fb) {
        printf("after %s: the GL state is not the promised one: framebuffers %d and %d, not %d\n", after, v[0], v[1],
               fb);
        return 1;
    }
    if (w > 0) {
        glGetIntegerv_(GL_VIEWPORT, v);
        if (v[0] != 0 || v[1] != 0 || v[2] != w || v[3] != h) {
            printf("after %s: the GL state is not the promised one: viewport %d %d %d %d\n", after, v[0], v[1], v[2],
                   v[3]);
            return 1;
        }
    }
    printf("after %s: the GL state is the promised one\n", after);
    return 0;
}

static void on_log(void *user, const char *line) {
    (void)user;
    fprintf(stderr, "%s\n", line);
}

static void *gl_loader(void *user, const char *name) {
    (void)user;
    return SDL_GL_GetProcAddress(name);
}

#define F(name) __typeof__(&name) name
static struct {
    F(glover_default_config);
    F(glover_create);
    F(glover_error);
    F(glover_update);
    F(glover_gl_init);
    F(glover_gl_options);
    F(glover_gl_draw);
    F(glover_gl_read_frame);
    F(glover_gl_shutdown);
    F(glover_destroy);
    F(glover_coll_set_static);
    F(glover_coll_body_add);
    F(glover_coll_body_move);
    F(glover_coll_body_remove);
    F(glover_coll_body_conveyor);
    F(glover_coll_set_water);
    F(glover_coll_clear);
    F(glover_place);
    F(glover_set_respawn);
    F(glover_set_view);
    F(glover_goto_level);
    F(glover_bodies);
    F(glover_gl_draw_layer);
    F(glover_gl_composite);
} lib;
#undef F

/* ---- --scene: the scene's requests through the library (as tools/libhost/libhost.c). */

static Glover *game;
static int viewDraws;
static HsView lastView;   /* the latest drawing view of the scene */

static GloverMesh glover_mesh(const HsMesh *m) {
    GloverMesh g;

    memset(&g, 0, sizeof(g));
    g.size = sizeof(g);
    g.flags = m->flags;
    g.verts = m->verts;
    g.vertCount = m->vertCount;
    g.indices = m->indices;
    g.terrain = m->terrain;
    g.triFlags = m->triFlags;
    g.triCount = m->triCount;
    g.conveyor = m->conveyor;
    g.conveyors = m->conveyors;
    g.conveyorCount = m->conveyorCount;
    return g;
}

static int scene_static(void *u, const HsMesh *m, const HsOptions *o) {
    GloverMesh g = glover_mesh(m);
    GloverCollOptions c;

    (void)u;
    memset(&c, 0, sizeof(c));
    c.size = sizeof(c);
    c.cellSize = o->cellSize;
    c.maxChunkTris = o->maxChunkTris;
    c.linkRadius = o->linkRadius;
    c.keepLevel = o->keepLevel;
    return lib.glover_coll_set_static(game, &g, &c);
}

static int scene_body_add(void *u, const HsMesh *m, const HsPose *p, unsigned int *id) {
    GloverMesh g = glover_mesh(m);
    GloverPose q;
    uint32_t got = 0;
    int rc;

    (void)u;
    memcpy(&q, p, sizeof(q));
    rc = lib.glover_coll_body_add(game, &g, &q, &got);
    *id = got;
    return rc;
}

static int scene_body_move(void *u, unsigned int id, const HsPose *p) {
    GloverPose q;

    (void)u;
    memcpy(&q, p, sizeof(q));
    return lib.glover_coll_body_move(game, id, &q);
}

static int scene_body_remove(void *u, unsigned int id) {
    (void)u;
    return lib.glover_coll_body_remove(game, id);
}

static int scene_clear(void *u) {
    (void)u;
    return lib.glover_coll_clear(game);
}

static int scene_place(void *u, const HsPlace *p) {
    GloverPlace g;

    (void)u;
    memset(&g, 0, sizeof(g));
    g.size = sizeof(g);
    g.what = p->what;
    memcpy(g.hand, p->hand, sizeof(g.hand));
    memcpy(g.ball, p->ball, sizeof(g.ball));
    memcpy(g.handVel, p->handVel, sizeof(g.handVel));
    memcpy(g.ballVel, p->ballVel, sizeof(g.ballVel));
    g.handYaw = p->handYaw;
    return lib.glover_place(game, &g);
}

static int scene_respawn(void *u, const float pos[3], float yaw) {
    (void)u;
    return lib.glover_set_respawn(game, pos, yaw);
}

static int scene_view(void *u, const HsView *v) {
    GloverView g;
    int rc;

    (void)u;
    if (v == NULL) {
        viewDraws = 0;
        return lib.glover_set_view(game, NULL);
    }
    memset(&g, 0, sizeof(g));
    g.size = sizeof(g);
    g.flags = v->flags;
    memcpy(g.eye, v->eye, sizeof(g.eye));
    memcpy(g.at, v->at, sizeof(g.at));
    memcpy(g.up, v->up, sizeof(g.up));
    g.fovy = v->fovy;
    g.aspect = v->aspect;
    g.nearZ = v->nearZ;
    g.farZ = v->farZ;
    rc = lib.glover_set_view(game, &g);
    if (rc == GLOVER_OK) {
        viewDraws = (v->flags & GLOVER_VIEW_DRAW) != 0;
        lastView = *v;
    }
    return rc;
}

static int scene_goto(void *u, int level) {
    (void)u;
    return lib.glover_goto_level(game, level);
}

static void scene_log(void *u, const char *line) {
    (void)u;
    fprintf(stderr, "%s\n", line);
}

static int scene_body_conveyor(void *u, unsigned int id, const float vel[3]) {
    (void)u;
    return lib.glover_coll_body_conveyor(game, id, vel);
}

/* HsWater has GloverWater's layout. */
static int scene_set_water(void *u, const HsWater *w, unsigned int count) {
    (void)u;
    return lib.glover_coll_set_water(game, (const GloverWater *)w, count, sizeof(GloverWater));
}

static const HsOps sceneOps = { scene_static, scene_body_add, scene_body_move, scene_body_remove, scene_clear,
                                scene_place, scene_respawn, scene_view, scene_goto, scene_log,
                                scene_body_conveyor, scene_set_water };

/* The pixel (from the bottom left) and the distance along the view of a point, for the view's projection on a
   w x h picture. Returns 0 when the point is behind the eye. */
static int project(const HsView *v, const float p[3], int w, int h, int *px, int *py, double *dist) {
    double f[3];
    double s[3];
    double u[3];
    double d[3];
    double len;
    double k = 1.0 / tan(v->fovy * 3.14159265358979323846 / 360.0);
    double xe;
    double ye;
    int i;

    for (i = 0; i < 3; i++) {
        f[i] = (double)v->at[i] - v->eye[i];
        d[i] = (double)p[i] - v->eye[i];
    }
    len = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (i = 0; i < 3; i++) {
        f[i] /= len;
    }
    s[0] = f[1] * v->up[2] - f[2] * v->up[1];
    s[1] = f[2] * v->up[0] - f[0] * v->up[2];
    s[2] = f[0] * v->up[1] - f[1] * v->up[0];
    len = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (i = 0; i < 3; i++) {
        s[i] /= len;
    }
    u[0] = s[1] * f[2] - s[2] * f[1];
    u[1] = s[2] * f[0] - s[0] * f[2];
    u[2] = s[0] * f[1] - s[1] * f[0];
    *dist = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    if (*dist <= 0.0) {
        return 0;
    }
    xe = (d[0] * s[0] + d[1] * s[1] + d[2] * s[2]) * k / v->aspect / *dist;
    ye = (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) * k / *dist;
    *px = (int)((xe + 1.0) * 0.5 * w);
    *py = (int)((ye + 1.0) * 0.5 * h);
    return *px >= 0 && *px < w && *py >= 0 && *py < h;
}

/* --layer: draws the layer at a shot, writes it, and with a drawing view checks the glove's pixel and the composite
   (see the top of the file). Returns the number of failures. */
static int layer_shot(Glover *g, GLuint fbo, unsigned int mask, long frame, const char *prefix) {
    static unsigned char rgba[640 * 480 * 4];
    static unsigned char rows[640 * 480 * 4];
    static float depth[640 * 480];
    GloverGlLayer l;
    char name[512];
    FILE *f;
    int bad = 0;
    int32_t rc;
    int y;

    memset(&l, 0, sizeof(l));
    l.size = sizeof(l);
    l.layers = mask;
    l.width = 640;
    l.height = 480;
    rc = lib.glover_gl_draw_layer(g, &l);
    snprintf(name, sizeof(name), "glover_gl_draw_layer at frame %ld", frame);
    bad += check_state(name, (GLint)fbo, 0, 0);
    if (rc != GLOVER_OK || l.texWidth != 640 || l.texHeight != 480 || l.colorTexture == 0 || l.depthTexture == 0) {
        printf("layer %ld: glover_gl_draw_layer returned %d, %dx%d, textures %u %u\n", frame, (int)rc,
               (int)l.texWidth, (int)l.texHeight, (unsigned)l.colorTexture, (unsigned)l.depthTexture);
        return bad + 1;
    }
    glBindTexture_(GL_TEXTURE_2D, l.colorTexture);
    glGetTexImage_(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture_(GL_TEXTURE_2D, l.depthTexture);
    glGetTexImage_(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
    glBindTexture_(GL_TEXTURE_2D, 0);
    for (y = 0; y < 480; y++) {
        memcpy(rows + (size_t)y * 640 * 4, rgba + (size_t)(479 - y) * 640 * 4, 640 * 4);
    }
    snprintf(name, sizeof(name), "%s_layer_%05ld.rgba", prefix, frame);
    f = fopen(name, "wb");
    if (f != NULL) {
        fwrite(rows, 1, sizeof(rows), f);
        fclose(f);
    }
    printf("layer %ld: near %g far %g projZ %.9g %.9g, alpha at the corners %d %d %d %d, written to %s\n", frame,
           l.nearZ, l.farZ, l.projZ[0], l.projZ[1], rgba[3], rgba[639 * 4 + 3], rgba[479 * 640 * 4 + 3],
           rgba[(479 * 640 + 639) * 4 + 3], name);
    snprintf(name, sizeof(name), "%s_layerdepth_%05ld.f32", prefix, frame);
    f = fopen(name, "wb");
    if (f != NULL) {
        for (y = 479; y >= 0; y--) {
            fwrite(depth + (size_t)y * 640, sizeof(float), 640, f);
        }
        fclose(f);
    }
    if (viewDraws) {
        GloverBodies b;
        double dist;
        int px;
        int py;

        memset(&b, 0, sizeof(b));
        b.size = sizeof(b);
        if (lib.glover_bodies(g, &b) == GLOVER_OK && project(&lastView, b.hand, 640, 480, &px, &py, &dist)
            && px >= 8 && px < 632) {
            double d = depth[py * 640 + px];
            double eye = l.projZ[1] / (2.0 * d - 1.0 + l.projZ[0]);
            double behind = d + (1.0 - d) * 0.5;
            float depthAt[3];
            unsigned char left[4];
            unsigned char right[4];
            const unsigned char *lr = &rgba[(py * 640 + px + 4) * 4];

            printf("glove %ld: pixel %d %d alpha %d depth %.6f, made linear %.2f, the glove's distance %.2f\n", frame,
                   px, py, rgba[(py * 640 + px) * 4 + 3], d, eye, dist);
            /* The host's picture: red, in front of the glove to the left of its pixel and behind it to the right. */
            glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
            glEnable_(GL_SCISSOR_TEST);
            glClearColor_(1.0f, 0.0f, 0.0f, 1.0f);
            glScissor_(0, 0, px, 480);
            glClearDepth_(d * 0.5);
            glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glScissor_(px, 0, 640 - px, 480);
            glClearDepth_(behind);
            glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDisable_(GL_SCISSOR_TEST);
            glClearColor_(0.0f, 0.0f, 0.0f, 0.0f);
            glClearDepth_(1.0);
            rc = lib.glover_gl_composite(g, fbo, 640, 480, 0.0005f, 0);
            snprintf(name, sizeof(name), "glover_gl_composite at frame %ld", frame);
            bad += check_state(name, (GLint)fbo, 640, 480);
            glReadPixels_(px - 4, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
            glReadPixels_(px + 4, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right);
            glReadPixels_(px + 4, py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depthAt[0]);
            /* The host's picture after the composite, rows from the top. */
            glReadPixels_(0, 0, 640, 480, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            for (y = 0; y < 480; y++) {
                memcpy(rows + (size_t)y * 640 * 4, rgba + (size_t)(479 - y) * 640 * 4, 640 * 4);
            }
            snprintf(name, sizeof(name), "%s_composite_%05ld.rgba", prefix, frame);
            f = fopen(name, "wb");
            if (f != NULL) {
                fwrite(rows, 1, sizeof(rows), f);
                fclose(f);
            }
            glBindTexture_(GL_TEXTURE_2D, l.colorTexture);
            glGetTexImage_(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            glBindTexture_(GL_TEXTURE_2D, 0);
            printf("composite %ld: %d; in front %d %d %d, behind %d %d %d, the layer there %d %d %d %d\n", frame,
                   (int)rc, left[0], left[1], left[2], right[0], right[1], right[2], lr[0], lr[1], lr[2], lr[3]);
            /* Again with GLOVER_COMPOSITE_DEPTH: the glove's depth replaces the host's where the glove is nearer. */
            glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
            rc = lib.glover_gl_composite(g, fbo, 640, 480, 0.0005f, GLOVER_COMPOSITE_DEPTH);
            snprintf(name, sizeof(name), "glover_gl_composite with its depth at frame %ld", frame);
            bad += check_state(name, (GLint)fbo, 640, 480);
            glReadPixels_(px + 4, py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depthAt[1]);
            glReadPixels_(px - 4, py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depthAt[2]);
            printf("composite depth %ld: %d; behind %.6f, then %.6f, the host's %.6f; in front %.6f, the host's %.6f\n",
                   frame, (int)rc, depthAt[0], depthAt[1], behind, depthAt[2], d * 0.5);
            /* A layer that failed to draw leaves nothing to composite. */
            l.width = 0;
            rc = lib.glover_gl_draw_layer(g, &l);
            printf("stale %ld: glover_gl_draw_layer of width 0 %d, then glover_gl_composite %d\n", frame, (int)rc,
                   (int)lib.glover_gl_composite(g, fbo, 640, 480, 0.0005f, 0));
        } else {
            printf("glove %ld: not in the picture\n", frame);
        }
    }
    return bad;
}

static unsigned char *read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    unsigned char *p;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc(*size);
    if (fread(p, 1, *size, f) != (size_t)*size) {
        free(p);
        p = NULL;
    }
    fclose(f);
    return p;
}

int main(int argc, char **argv) {
    const char *rom = NULL;
    const char *prefix = "glshot";
    const char *save = NULL;
    const char *scenePath = NULL;
    unsigned int layerMask = 0;
    HostScene *scene = NULL;
    int wideChecked = 0;
    const char *path = getenv("GLOVER_LIB");
    long frames = 300;
    long level = 5;
    long shots[16];
    int shotCount = 0;
    char buf[4096];
    void *h;
    int i;
    long romSize = 0;
    unsigned char *romData;
    unsigned char eeprom[512];
    GloverConfig c;
    Glover *g = NULL;
    char err[256];
    SDL_Window *win;
    SDL_GLContext ctx;
    GLuint fbo;
    GLuint rb[2];
    GloverGlOptions o;
    GloverInput in = {0, 0, 0};
    GloverFrame out;
    long frame = 0;
    int bad = 0;
    int32_t rc;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) {
            rom = argv[++i];
        } else if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            level = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--headless") && i + 1 < argc) {
            frames = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc && shotCount < 16) {
            shots[shotCount++] = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--prefix") && i + 1 < argc) {
            prefix = argv[++i];
        } else if (!strcmp(argv[i], "--save") && i + 1 < argc) {
            save = argv[++i];
        } else if (!strcmp(argv[i], "--scene") && i + 1 < argc) {
            scenePath = argv[++i];
        } else if (!strcmp(argv[i], "--layer") && i + 1 < argc) {
            layerMask = (unsigned int)strtoul(argv[++i], NULL, 0);
        } else {
            fprintf(stderr, "usage: glover-glshot --rom FILE --level N --headless FRAMES --shot FRAME... --prefix PATH\n");
            return 2;
        }
    }
    if (path == NULL) {
        const char *slash = strrchr(argv[0], '/');

        snprintf(buf, sizeof(buf), "%.*slibglover.so", slash ? (int)(slash - argv[0] + 1) : 0, argv[0]);
        path = buf;
    }
    h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        fprintf(stderr, "glover-glshot: %s\n", dlerror());
        return 2;
    }
#define L(name) *(void **)&lib.name = dlsym(h, #name)
    L(glover_default_config);
    L(glover_create);
    L(glover_error);
    L(glover_update);
    L(glover_gl_init);
    L(glover_gl_options);
    L(glover_gl_draw);
    L(glover_gl_read_frame);
    L(glover_gl_shutdown);
    L(glover_destroy);
    L(glover_coll_set_static);
    L(glover_coll_body_add);
    L(glover_coll_body_move);
    L(glover_coll_body_remove);
    L(glover_coll_body_conveyor);
    L(glover_coll_set_water);
    L(glover_coll_clear);
    L(glover_place);
    L(glover_set_respawn);
    L(glover_set_view);
    L(glover_goto_level);
    L(glover_bodies);
    L(glover_gl_draw_layer);
    L(glover_gl_composite);
#undef L
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "glover-glshot: SDL: %s\n", SDL_GetError());
        return 2;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    win = SDL_CreateWindow("glshot", 0, 0, 640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    ctx = win ? SDL_GL_CreateContext(win) : NULL;
    if (ctx == NULL) {
        fprintf(stderr, "glover-glshot: no OpenGL 3.3 context: %s\n", SDL_GetError());
        return 2;
    }
#define G(name) *(void **)&name##_ = SDL_GL_GetProcAddress(#name)
    G(glGenFramebuffers);
    G(glBindFramebuffer);
    G(glGenRenderbuffers);
    G(glBindRenderbuffer);
    G(glRenderbufferStorage);
    G(glFramebufferRenderbuffer);
    G(glCheckFramebufferStatus);
    G(glGetIntegerv);
    G(glGetFloatv);
    G(glIsEnabled);
    G(glActiveTexture);
    G(glBindTexture);
    G(glGetTexImage);
    G(glEnable);
    G(glDisable);
    G(glScissor);
    G(glClearColor);
    G(glClearDepth);
    G(glClear);
    G(glReadPixels);
#undef G
    glGenFramebuffers_(1, &fbo);
    glGenRenderbuffers_(2, rb);
    glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
    glBindRenderbuffer_(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_RGBA8, 640, 480);
    glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glBindRenderbuffer_(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 640, 480);
    glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "glover-glshot: the framebuffer is not complete\n");
        return 2;
    }
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);

    romData = read_file(rom, &romSize);
    if (romData == NULL) {
        fprintf(stderr, "glover-glshot: cannot read the ROM\n");
        return 2;
    }
    lib.glover_default_config(&c);
    c.rom = romData;
    c.romSize = (uint32_t)romSize;
    c.startLevel = (int32_t)level;
    if (getenv("GLSHOT_LOG") != NULL) {
        c.log = on_log;
    }
    if (save != NULL) {
        FILE *f = fopen(save, "rb");

        memset(eeprom, 0, sizeof(eeprom));
        if (f != NULL) {
            if (fread(eeprom, 1, sizeof(eeprom), f) == 0) {
                memset(eeprom, 0, sizeof(eeprom));
            }
            fclose(f);
        }
        c.eeprom = eeprom;
    }
    if (scenePath != NULL) {
        scene = hs_load(scenePath, err, sizeof(err));
        if (scene == NULL) {
            fprintf(stderr, "glover-glshot: %s\n", err);
            return 2;
        }
    }
    rc = lib.glover_create(&c, &g, err, sizeof(err));
    if (rc != GLOVER_OK) {
        fprintf(stderr, "glover-glshot: glover_create returned %d: %s\n", (int)rc, err);
        return 3;
    }
    game = g;
    rc = lib.glover_gl_init(g, gl_loader, NULL);
    printf("glover_gl_init returned %d\n", (int)rc);
    if (rc != GLOVER_OK) {
        return rc == GLOVER_ERR_UNSUPPORTED ? 5 : 3;
    }
    bad += check_state("glover_gl_init", 0, 0, 0);
    {
        static unsigned char none[16];
        int32_t w = -1;
        int32_t hh = -1;
        int32_t again = lib.glover_gl_init(g, gl_loader, NULL);
        int32_t read = lib.glover_gl_read_frame(g, none, sizeof(none), &w, &hh);
        int32_t badOpt[4];

        o.size = sizeof(o);
        o.scale = 5000, o.msaa = 4, o.filter = 0, o.widescreen = 0;
        badOpt[0] = lib.glover_gl_options(g, &o);
        o.scale = 100, o.msaa = 16;
        badOpt[1] = lib.glover_gl_options(g, &o);
        o.msaa = 4, o.filter = 9;
        badOpt[2] = lib.glover_gl_options(g, &o);
        o.filter = 0, o.widescreen = 3;
        badOpt[3] = lib.glover_gl_options(g, &o);
        GloverGlLayer l;
        int32_t badLayer[4];

        printf("a second glover_gl_init %d; glover_gl_read_frame before a picture %d, %dx%d; options scale 5000 %d, "
               "msaa 16 %d, filter 9 %d, widescreen 3 %d\n", (int)again, (int)read, (int)w, (int)hh, (int)badOpt[0],
               (int)badOpt[1], (int)badOpt[2], (int)badOpt[3]);
        if (again != GLOVER_ERR_ARG || read != GLOVER_ERR_ARG || w != 0 || hh != 0 || badOpt[0] != GLOVER_ERR_ARG
            || badOpt[1] != GLOVER_ERR_ARG || badOpt[2] != GLOVER_ERR_ARG || badOpt[3] != GLOVER_ERR_ARG) {
            bad++;
        }
        /* The layer's checks: before the first lists, an unknown bit, no part, a short size; a composite without a
           layer. */
        memset(&l, 0, sizeof(l));
        l.size = sizeof(l);
        l.layers = GLOVER_LAYER_ACTORS;
        l.width = 640;
        l.height = 480;
        badLayer[0] = lib.glover_gl_draw_layer(g, &l);
        l.layers = 0x10 | GLOVER_LAYER_ACTORS;
        badLayer[1] = lib.glover_gl_draw_layer(g, &l);
        l.layers = GLOVER_LAYER_NO_FOG;
        badLayer[2] = lib.glover_gl_draw_layer(g, &l);
        l.layers = GLOVER_LAYER_ACTORS;
        l.size = 36;
        badLayer[3] = lib.glover_gl_draw_layer(g, &l);
        printf("glover_gl_draw_layer before the first lists %d, with bit 0x10 %d, with no part %d, size 36 %d; "
               "glover_gl_composite without a layer %d\n", (int)badLayer[0], (int)badLayer[1], (int)badLayer[2],
               (int)badLayer[3], (int)lib.glover_gl_composite(g, 0, 640, 480, 0.0005f, 0));
        bad += check_state("the layer's checks", 0, 0, 0);
        for (i = 0; i < 4; i++) {
            bad += badLayer[i] != GLOVER_ERR_ARG;
        }
    }
    /* The options of weemitt's settings_defaults (settings.c). */
    o.size = sizeof(o);
    o.scale = 100;
    o.msaa = 4;
    o.filter = 0;
    o.widescreen = 0;
    if (lib.glover_gl_options(g, &o) != GLOVER_OK) {
        printf("glover_gl_options refused the defaults\n");
        bad++;
    }
    bad += check_state("glover_gl_options", 0, 0, 0);
    memset(&out, 0, sizeof(out));
    out.size = sizeof(out);
    while (frame < frames) {
        if (scene != NULL) {
            hs_update(scene, (double)frame, 1.0, &sceneOps, NULL);
            if (viewDraws && !wideChecked) {
                GloverGlOptions wide = o;
                int32_t refused;

                wide.widescreen = 2;
                refused = lib.glover_gl_options(g, &wide);
                printf("glover_gl_options with widescreen 2 and a drawing view: %d\n", (int)refused);
                bad += refused != GLOVER_ERR_ARG;
                wideChecked = 1;
            }
        }
        rc = lib.glover_update(g, 1.0f, &in, &out);
        if (rc != GLOVER_OK) {
            fprintf(stderr, "glover-glshot: glover_update returned %d: %s\n", (int)rc, lib.glover_error(g));
            return 3;
        }
        if (!out.ticked) {
            continue;
        }
        for (i = 0; i < shotCount; i++) {
            if (shots[i] == frame) {
                static unsigned char rgba[4096 * 4096 * 4];
                int32_t w;
                int32_t hh;
                FILE *f;

                /* The host's own state before the call must not matter. */
                glBindRenderbuffer_(GL_RENDERBUFFER, rb[0]);
                if (lib.glover_gl_draw(g, fbo, 640, 480) != GLOVER_OK) {
                    printf("frame %ld: glover_gl_draw failed\n", frame);
                    bad++;
                }
                snprintf(buf, sizeof(buf), "glover_gl_draw at frame %ld", frame);
                bad += check_state(buf, (GLint)fbo, 640, 480);
                rc = lib.glover_gl_read_frame(g, rgba, sizeof(rgba), &w, &hh);
                snprintf(buf, sizeof(buf), "glover_gl_read_frame at frame %ld", frame);
                bad += check_state(buf, (GLint)fbo, 640, 480);
                if (rc != GLOVER_OK) {
                    printf("frame %ld: glover_gl_read_frame returned %d\n", frame, (int)rc);
                    bad++;
                    continue;
                }
                snprintf(buf, sizeof(buf), "%s_%05ld.rgba", prefix, frame);
                f = fopen(buf, "wb");
                if (f != NULL) {
                    fwrite(rgba, 1, (size_t)w * hh * 4, f);
                    fclose(f);
                }
                printf("frame %ld: %dx%d written to %s\n", frame, (int)w, (int)hh, buf);
                if (layerMask != 0) {
                    bad += layer_shot(g, fbo, layerMask, frame, prefix);
                }
            }
        }
        frame++;
    }
    /* The renderer's objects can be freed after the game is gone. */
    rc = lib.glover_destroy(g);
    {
        int32_t shutdown = lib.glover_gl_shutdown(g);
        int32_t init = lib.glover_gl_init(g, gl_loader, NULL);

        printf("glover_destroy %d, then glover_gl_shutdown %d, glover_gl_init %d\n", (int)rc, (int)shutdown,
               (int)init);
        if (rc != GLOVER_OK || shutdown != GLOVER_OK || init != GLOVER_ERR_DEAD) {
            bad++;
        }
        bad += check_state("glover_gl_shutdown", (GLint)fbo, 0, 0);
    }
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return bad ? 4 : 0;
}
