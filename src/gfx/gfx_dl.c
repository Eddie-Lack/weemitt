/* The display list interpreter: the F3DEX 1.21 and Sprite2D microcode commands and the RDP commands they carry.

   This file does the RSP's work on the CPU: it transforms and lights vertices, computes fog, culls triangles and
   tracks the RDP's state. Triangles and rectangles go to gfx_render.c, which batches them. */
#include <math.h>
#include <string.h>

#include "gfx_host.h"

#include "gfx_internal.h"

/* F3DEX opcodes. */
#define G_MTX 0x01
#define G_MOVEMEM 0x03
#define G_VTX 0x04
#define G_DL 0x06
#define G_SPRITE2D_BASE 0x09
#define G_TRI2 0xB1
#define G_MODIFYVTX 0xB2
#define G_CLEARGEOMETRYMODE 0xB6
#define G_SETGEOMETRYMODE 0xB7
#define G_ENDDL 0xB8
#define G_SETOTHERMODE_L 0xB9
#define G_SETOTHERMODE_H 0xBA
#define G_TEXTURE 0xBB
#define G_MOVEWORD 0xBC
#define G_POPMTX 0xBD
#define G_CULLDL 0xBE
#define G_TRI1 0xBF
/* Sprite2D opcodes that differ from F3DEX's. */
#define G_SPRITE2D_SCALEFLIP 0xBE
#define G_SPRITE2D_DRAW 0xBD
/* RDP opcodes. */
#define G_TEXRECT 0xE4
#define G_TEXRECTFLIP 0xE5
#define G_SETSCISSOR 0xED
#define G_SETPRIMDEPTH 0xEE
#define G_SETOTHERMODE 0xEF
#define G_LOADTLUT 0xF0
#define G_SETTILESIZE 0xF2
#define G_LOADBLOCK 0xF3
#define G_LOADTILE 0xF4
#define G_SETTILE 0xF5
#define G_FILLRECT 0xF6
#define G_SETFILLCOLOR 0xF7
#define G_SETFOGCOLOR 0xF8
#define G_SETBLENDCOLOR 0xF9
#define G_SETPRIMCOLOR 0xFA
#define G_SETENVCOLOR 0xFB
#define G_SETCOMBINE 0xFC
#define G_SETTIMG 0xFD
#define G_SETCIMG 0xFF

/* G_MOVEMEM and G_MOVEWORD indices. */
#define G_MV_VIEWPORT 0x80
#define G_MV_LOOKATY 0x82
#define G_MV_LOOKATX 0x84
#define G_MV_L0 0x86
#define G_MV_L7 0x94
#define G_MW_NUMLIGHT 0x02
#define G_MW_SEGMENT 0x06
#define G_MW_FOG 0x08
#define G_MW_LIGHTCOL 0x0A

/* G_MTX flags. */
#define G_MTX_PROJECTION 0x01
#define G_MTX_LOAD 0x02
#define G_MTX_PUSH 0x04

#define NUM_VERTICES 64
#define STACK_DEPTH 32
#define MAX_DL_DEPTH 16
#define MAX_COMMANDS 100000

GfxRdp gfxRdp;

/* Widescreen (GFX_WIDE_HORPLUS): the factor by which the view is widened, 1 for none. Every x coordinate that
   reaches the renderer is divided by it, which makes the N64's 4:3 screen a centred area of the wider frame. The 3D
   view (a perspective projection and a viewport over the whole screen width) also draws what lies beyond the area's
   sides, which the N64's view cut off. */
static float wideK = 1.0f;

void gfx_dl_set_wide(float wide) {
    wideK = wide < 1.0f ? 1.0f : wide;
}

/* A vertex in the RSP's buffer: as the GPU gets it, and its clip codes for G_CULLDL. */
typedef struct RspVertex {
    GfxVertex v;
    u8 clip;
    u8 wide;  /* drawn with the widened 3D view (see view_is_wide) */
} RspVertex;

typedef struct Light {
    float r, g, b;
    float x, y, z;
} Light;

GfxSceneView gfxScene;
/* 1 after the scene camera was taken from a matrix, until the next vertex load reads the viewport. */
static int scenePending;

static struct {
    u32 segments[16];
    u32 segmentSet;     /* bit n: the game set segment n */
    float mvStack[STACK_DEPTH][16];
    int mvDepth;        /* index of the top matrix */
    float proj[16];
    float mvp[16];
    int mvpValid;
    RspVertex vtx[NUM_VERTICES];
    float vpScale[2], vpTrans[2];
    float fogMul, fogOff;
    int numLights;
    Light lights[8];
    float lookAt[2][3];
    float texScale[2];
    u32 sprite;         /* Sprite2D: address of the uSprite, 0 for none */
    float spriteScale[2];
    int spriteFlip[2];
    int sprite2d;       /* the task runs the Sprite2D microcode */
    u32 cfb[2];
    u32 colorImage;
    u32 pc;             /* address of the command after the one being run */
    u32 pathHash;       /* the stable display list addresses of the call path (interpolation) */
    int mvRec[STACK_DEPTH]; /* the interpolation record of each matrix on the stack */
    int projRec;
    int dry;            /* a recording pass: matrices and clip codes only, nothing is drawn */
    int host;           /* the host translator's walk (gfx_hostgfx.c): dry, and each command is reported */
    int noCull;         /* G_CULLDL never ends a list: the host's walk follows every list, and so does its recording */
    float lastMtx[16];  /* the matrix of the latest G_MTX, after the interpolation */
    int lastMtxBlended; /* 1 when the interpolation changed it */
} rsp;

/* ---- Addresses and memory. */

/* A KSEG0 or KSEG1 address (0x80000000-0xBFFFFFFF) loses its top bits, as libultra's K0_TO_PHYSICAL does. Other
   values are host addresses and stay as they are. */
static u32 phys_addr(u32 a) {
    return (a >= 0x80000000u && a < 0xC0000000u) ? a & 0x1FFFFFFF : a;
}

/* The address an RSP command points to: a segment address when the game set that segment, otherwise as above. */
static u32 seg_addr(u32 a) {
    u32 seg = a >> 24;

    if (seg != 0 && seg < 16 && (rsp.segmentSet & (1u << seg))) {
        return rsp.segments[seg] + (a & 0xFFFFFF);
    }
    return phys_addr(a);
}

static s16 s16_at(const u8 *p) {
    s16 v;
    memcpy(&v, p, 2);
    return v;
}

static u32 u32_at(const u8 *p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

/* ---- Matrices: row-major 4x4, multiplying row vectors as the N64 does. */

static void mat_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* r = a * b. r may not be a or b. */
static void mat_mul(float *r, const float *a, const float *b) {
    int i;
    int j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j]
                           + a[i * 4 + 3] * b[12 + j];
        }
    }
}

/* An Mtx: 16 integer halves in the first 32 bytes, then 16 fraction halves, two to a word. */
static int read_matrix(u32 addr, float *m) {
    const u8 *p = gfx_mem(addr, 64);
    int k;

    if (p == NULL) {
        return 0;
    }
    for (k = 0; k < 16; k++) {
        u32 wi = u32_at(p + (k >> 1) * 4);
        u32 wf = u32_at(p + 32 + (k >> 1) * 4);
        u32 hi = (k & 1) ? (wi & 0xFFFF) : (wi >> 16);
        u32 lo = (k & 1) ? (wf & 0xFFFF) : (wf >> 16);
        m[k] = (float)(s32)((hi << 16) | lo) / 65536.0f;
    }
    return 1;
}

/* The address that the matrix just loaded is used for: the next vertex load or display list call, found by reading
   ahead. 1 when that address is in a buffer that the game rebuilds every frame, 0 when there is none. */
static u32 next_use(u32 pc) {
    int k;

    for (k = 0; k < 48; k++) {
        const u8 *p = gfx_mem(pc, 8);
        u32 op;
        u32 a;

        if (p == NULL) {
            return 0;
        }
        op = u32_at(p) >> 24;
        if (op == G_VTX || op == G_DL) {
            a = seg_addr(u32_at(p + 4));
            return gfx_interp_is_volatile(a) ? 1 : a;
        }
        if (op == G_MTX || op == G_POPMTX || op == G_ENDDL) {
            return 0;
        }
        pc += 8;
    }
    return 0;
}

static void matrix(u32 w0, u32 w1) {
    u32 flags = (w0 >> 16) & 0xFF;
    float m[16];
    float r[16];
    int rec = -2;

    if (!read_matrix(seg_addr(w1), m)) {
        rsp.lastMtxBlended = -1;
        return;
    }
    rsp.lastMtxBlended = 0;
    if (gfxInterpMode) {
        int kind = (flags & G_MTX_PROJECTION) ? ((flags & G_MTX_LOAD) ? 1 : 2) : 0;
        int parent = (flags & G_MTX_LOAD) ? -1 : kind ? rsp.projRec : rsp.mvRec[rsp.mvDepth];
        u32 lkey = gfx_interp_mix(rsp.pathHash, kind);
        u32 key = gfx_interp_mix(lkey, next_use(rsp.pc));

        if (gfxInterpMode == 1) {
            rec = gfx_interp_record_mtx(key, lkey, parent, kind, m);
        } else {
            float before[16];

            memcpy(before, m, sizeof(before));
            rec = gfx_interp_draw_mtx(key, parent, kind, m);
            rsp.lastMtxBlended = memcmp(before, m, sizeof(before)) != 0;
        }
    }
    memcpy(rsp.lastMtx, m, sizeof(m));
    if (flags & G_MTX_PROJECTION) {
        if (flags & G_MTX_LOAD) {
            memcpy(rsp.proj, m, sizeof(m));
        } else {
            mat_mul(r, m, rsp.proj);
            /* The game's SetupViewing loads the perspective and multiplies the camera's view matrix into it. Its
               ClearViewing does the same with a view from the origin, which leaves the last row as it was. */
            if (!rsp.dry
                && (fabsf(r[12] - rsp.proj[12]) + fabsf(r[13] - rsp.proj[13]) + fabsf(r[14] - rsp.proj[14])) > 0.01f) {
                memcpy(gfxScene.vp, r, sizeof(r));
                gfxScene.valid = 1;
                scenePending = 1;
            }
            memcpy(rsp.proj, r, sizeof(r));
        }
        rsp.projRec = rec;
    } else {
        float *top = rsp.mvStack[rsp.mvDepth];
        if (flags & G_MTX_LOAD) {
            memcpy(r, m, sizeof(m));
        } else {
            mat_mul(r, m, top);
        }
        if ((flags & G_MTX_PUSH) && rsp.mvDepth < STACK_DEPTH - 1) {
            rsp.mvDepth++;
        }
        memcpy(rsp.mvStack[rsp.mvDepth], r, sizeof(r));
        rsp.mvRec[rsp.mvDepth] = rec;
    }
    rsp.mvpValid = 0;
    if (gfxInterpMode == 2 && gfx_interp_track != 0 && !(flags & G_MTX_PROJECTION)) {
        u32 lkey = gfx_interp_mix(rsp.pathHash, 0);
        if (gfx_interp_mix(lkey, next_use(rsp.pc)) == gfx_interp_track) {
            const float *mv = rsp.mvStack[rsp.mvDepth];
            const float *pj = rsp.proj;
            float c[4];
            int k;

            for (k = 0; k < 4; k++) {
                c[k] = mv[12] * pj[k] + mv[13] * pj[4 + k] + mv[14] * pj[8 + k] + mv[15] * pj[12 + k];
            }
            float len = sqrtf(mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2]);
            float tip[4];

            /* A point 10 units along the model's x axis shows the turn on the screen. */
            for (k = 0; k < 4; k++) {
                tip[k] = (mv[12] + 10.0f * mv[0] / len) * pj[k] + (mv[13] + 10.0f * mv[1] / len) * pj[4 + k]
                         + (mv[14] + 10.0f * mv[2] / len) * pj[8 + k] + mv[15] * pj[12 + k];
            }
            gfx_log("track t=%.3f world %.2f %.2f %.2f screen %.2f %.2f tip %.2f %.2f", gfx_interp_time(), mv[12], mv[13],
                    mv[14], rsp.vpTrans[0] + c[0] / c[3] * rsp.vpScale[0], rsp.vpTrans[1] - c[1] / c[3] * rsp.vpScale[1],
                    rsp.vpTrans[0] + tip[0] / tip[3] * rsp.vpScale[0], rsp.vpTrans[1] - tip[1] / tip[3] * rsp.vpScale[1]);
        }
    }
}

static const float *current_mvp(void) {
    if (!rsp.mvpValid) {
        mat_mul(rsp.mvp, rsp.mvStack[rsp.mvDepth], rsp.proj);
        rsp.mvpValid = 1;
    }
    return rsp.mvp;
}

/* True when what the game draws now is the 3D view over the whole screen width: a perspective projection (its
   w comes from z, not 1) and a viewport that spans the screen from its left to its right edge. Only then does a
   wider frame show more of the world. */
static int view_is_wide(void) {
    float left = rsp.vpTrans[0] - rsp.vpScale[0];
    float right = rsp.vpTrans[0] + rsp.vpScale[0];

    return wideK > 1.0f && rsp.proj[11] != 0.0f && left <= 0.5f && right >= SCREEN_W - 0.5f;
}

/* ---- Vertices. */

/* The fog amount for a vertex in clip space, from 0 to 1. */
static float fog_amount(const GfxVertex *v) {
    float f;

    if (gfxLayer.on && gfxLayer.noFog) {
        return 0.0f;
    }
    if (v->w < 0.0f) {
        return 1.0f;
    }
    f = v->w != 0.0f ? (v->z / v->w) * rsp.fogMul + rsp.fogOff : 0.0f;
    f = f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : f;
    return f / 255.0f;
}

static void load_vertices(u32 addr, int n, int v0) {
    const u8 *p = gfx_mem(seg_addr(addr), (u32)n * 16);
    const float *m = current_mvp();
    const float *mv = rsp.mvStack[rsp.mvDepth];
    u32 gm = gfxRdp.geometryMode;
    /* The viewport maps clip space to the N64's screen; these factors map it on to OpenGL's clip space for a
       320x240 screen, y up. */
    float kx = rsp.vpScale[0] / (SCREEN_W / 2);
    float ox = rsp.vpTrans[0] / (SCREEN_W / 2) - 1.0f;
    float ky = rsp.vpScale[1] / (SCREEN_H / 2);
    float oy = 1.0f - rsp.vpTrans[1] / (SCREEN_H / 2);
    int wide = view_is_wide();
    /* The clip codes (for G_CULLDL) cover the wider view. */
    float reach = wide ? wideK : 1.0f;
    int i;

    if (p == NULL || rsp.host) {
        return;
    }
    if (scenePending) {
        scenePending = 0;
        gfxScene.vpScale[0] = rsp.vpScale[0];
        gfxScene.vpScale[1] = rsp.vpScale[1];
        gfxScene.vpTrans[0] = rsp.vpTrans[0];
        gfxScene.vpTrans[1] = rsp.vpTrans[1];
    }
    for (i = 0; i < n && v0 + i < NUM_VERTICES; i++, p += 16) {
        RspVertex *rv = &rsp.vtx[v0 + i];
        GfxVertex *v = &rv->v;
        float x = s16_at(p);
        float y = s16_at(p + 2);
        float z = s16_at(p + 4);
        float s = s16_at(p + 8);
        float t = s16_at(p + 10);
        float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
        float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
        float cz = x * m[2] + y * m[6] + z * m[10] + m[14];
        float cw = x * m[3] + y * m[7] + z * m[11] + m[15];

        rv->clip = (cx < -cw * reach ? 1 : 0) | (cx > cw * reach ? 2 : 0) | (cy < -cw ? 4 : 0) | (cy > cw ? 8 : 0)
                   | (cz > cw ? 16 : 0) | (cw < 0 ? 32 : 0);
        rv->wide = (u8)wide;
        if (rsp.dry) {
            continue;
        }
        v->x = (cx * kx + cw * ox) / wideK;
        v->y = cy * ky + cw * oy;
        v->z = cz;
        v->w = cw;
        v->s = s * rsp.texScale[0] / 32.0f;
        v->t = t * rsp.texScale[1] / 32.0f;
        if (gm & GM_LIGHTING) {
            /* The normal in eye space, then the ambient light plus each directional light. */
            float nx = (s8)p[12];
            float ny = (s8)p[13];
            float nz = (s8)p[14];
            float tx = nx * mv[0] + ny * mv[4] + nz * mv[8];
            float ty = nx * mv[1] + ny * mv[5] + nz * mv[9];
            float tz = nx * mv[2] + ny * mv[6] + nz * mv[10];
            float len = sqrtf(tx * tx + ty * ty + tz * tz);
            const Light *amb = &rsp.lights[rsp.numLights];
            float r = amb->r;
            float g = amb->g;
            float b = amb->b;
            int k;

            if (len > 0.0f) {
                tx /= len;
                ty /= len;
                tz /= len;
            }
            for (k = 0; k < rsp.numLights; k++) {
                const Light *l = &rsp.lights[k];
                float d = tx * l->x + ty * l->y + tz * l->z;
                if (d > 0.0f) {
                    r += l->r * d;
                    g += l->g * d;
                    b += l->b * d;
                }
            }
            v->r = r < 1.0f ? r : 1.0f;
            v->g = g < 1.0f ? g : 1.0f;
            v->b = b < 1.0f ? b : 1.0f;
            if (gm & GM_TEXGEN) {
                /* The texture coordinates follow the normal, as for a reflection map: 0 to 1 across the sphere,
                   times the G_TEXTURE scale in units of 1/1024 texel. */
                float gs = tx * rsp.lookAt[0][0] + ty * rsp.lookAt[0][1] + tz * rsp.lookAt[0][2];
                float gt = tx * rsp.lookAt[1][0] + ty * rsp.lookAt[1][1] + tz * rsp.lookAt[1][2];
                if (gm & GM_TEXGEN_LINEAR) {
                    gs = acosf(-(gs < -1.0f ? -1.0f : gs > 1.0f ? 1.0f : gs)) / 3.14159265f;
                    gt = acosf(-(gt < -1.0f ? -1.0f : gt > 1.0f ? 1.0f : gt)) / 3.14159265f;
                } else {
                    gs = gs * 0.5f + 0.5f;
                    gt = gt * 0.5f + 0.5f;
                }
                v->s = gs * rsp.texScale[0] * 1024.0f;
                v->t = gt * rsp.texScale[1] * 1024.0f;
            }
        } else {
            v->r = p[12] / 255.0f;
            v->g = p[13] / 255.0f;
            v->b = p[14] / 255.0f;
        }
        v->a = p[15] / 255.0f;
        if (gm & GM_FOG) {
            /* The RSP puts the fog amount in the shade alpha. */
            v->a = fog_amount(v);
        }
    }
}

static void modify_vertex(int i, u32 where, u32 val) {
    GfxVertex *v;

    if (i >= NUM_VERTICES) {
        return;
    }
    v = &rsp.vtx[i].v;
    if (where == 0x10) {
        v->r = (val >> 24) / 255.0f;
        v->g = ((val >> 16) & 0xFF) / 255.0f;
        v->b = ((val >> 8) & 0xFF) / 255.0f;
        v->a = (val & 0xFF) / 255.0f;
    } else if (where == 0x14) {
        v->s = (s16)(val >> 16) / 32.0f;
        v->t = (s16)(val & 0xFFFF) / 32.0f;
    }
}

/* G_CULLDL: true when every vertex in the range is outside the same side of the view volume. */
static int cull_vertices(int first, int last) {
    u8 all = 0x3F;
    int i;

    for (i = first; i <= last && i < NUM_VERTICES; i++) {
        all &= rsp.vtx[i].clip;
    }
    return all != 0;
}

/* Clips a triangle against the near plane (z >= -w) as the RSP does: a new vertex's attributes are linear in clip
   space, and its fog amount comes from its own depth. OpenGL would clip too, but it computes a new vertex's
   screen-linear attributes for screen-space interpolation across the whole triangle, which has no meaning when a
   vertex is behind the eye. Returns the number of vertices of the clipped polygon (0, 3 or 4). */
static int clip_near(const GfxVertex *in, GfxVertex *out) {
    int n = 0;
    int i;
    int k;

    for (i = 0; i < 3; i++) {
        const GfxVertex *a = &in[i];
        const GfxVertex *b = &in[(i + 1) % 3];
        float da = a->z + a->w;
        float db = b->z + b->w;

        if (da >= 0.0f) {
            out[n++] = *a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            const float *fa = &a->x;
            const float *fb = &b->x;
            float *fo = &out[n++].x;
            float t = da / (da - db);
            for (k = 0; k < (int)(sizeof(GfxVertex) / sizeof(float)); k++) {
                fo[k] = fa[k] + (fb[k] - fa[k]) * t;
            }
            if (gfxRdp.geometryMode & GM_FOG) {
                out[n - 1].a = fog_amount(&out[n - 1]);
            }
        }
    }
    return n;
}

static void triangle(int i0, int i1, int i2) {
    const GfxVertex *a;
    u32 cull = gfxRdp.geometryMode & (GM_CULL_FRONT | GM_CULL_BACK);
    GfxVertex tri[3];
    GfxVertex clipped[6];
    GfxVertex *poly = tri;
    int n = 3;

    if (i0 >= NUM_VERTICES || i1 >= NUM_VERTICES || i2 >= NUM_VERTICES) {
        return;
    }
    a = &rsp.vtx[i0].v;
    tri[0] = *a;
    tri[1] = rsp.vtx[i1].v;
    tri[2] = rsp.vtx[i2].v;
    if (!(gfxRdp.geometryMode & GM_SMOOTH)) {
        /* Flat shading: the first vertex's colour for the whole triangle. */
        tri[1].r = tri[2].r = a->r;
        tri[1].g = tri[2].g = a->g;
        tri[1].b = tri[2].b = a->b;
    }
    if (tri[0].z + tri[0].w < 0.0f || tri[1].z + tri[1].w < 0.0f || tri[2].z + tri[2].w < 0.0f) {
        poly = clipped;
        n = clip_near(tri, clipped);
        if (n < 3) {
            return;
        }
    }
    /* Culling, in screen space after clipping. */
    if (cull && poly[0].w > 0.0f && poly[1].w > 0.0f && poly[2].w > 0.0f) {
        float ax = poly[0].x / poly[0].w;
        float ay = poly[0].y / poly[0].w;
        float area = (poly[1].x / poly[1].w - ax) * (poly[2].y / poly[2].w - ay)
                     - (poly[1].y / poly[1].w - ay) * (poly[2].x / poly[2].w - ax);
        if ((area < 0.0f && (cull & GM_CULL_BACK)) || (area > 0.0f && (cull & GM_CULL_FRONT))) {
            return;
        }
    }
    if (!gfx_prepare(GFX_PRIM_TRIANGLE, 0, NULL, NULL, rsp.vtx[i0].wide)) {
        return;
    }
    gfx_add_vertices(poly, 3);
    if (n == 4) {
        /* The second half of a quadrilateral: vertices 0, 2 and 3. */
        poly[4] = poly[0];
        poly[5] = poly[2];
        gfx_add_vertices(poly + 3, 3);
    }
}

/* ---- Rectangles. Coordinates are N64 screen pixels. */

static void screen_vertex(GfxVertex *v, float x, float y, float z, float s, float t, const float *shade) {
    v->x = (x / (SCREEN_W / 2) - 1.0f) / wideK;
    v->y = 1.0f - y / (SCREEN_H / 2);
    v->z = z;
    v->w = 1.0f;
    v->r = shade[0];
    v->g = shade[1];
    v->b = shade[2];
    v->a = shade[3];
    v->s = s;
    v->t = t;
}

/* Two triangles. st holds the texture coordinates at the corners (x0, y0), (x1, y0), (x1, y1), (x0, y1). */
static void quad(float x0, float y0, float x1, float y1, float z, const float st[4][2], const float *shade) {
    GfxVertex v[6];

    screen_vertex(&v[0], x0, y0, z, st[0][0], st[0][1], shade);
    screen_vertex(&v[1], x1, y0, z, st[1][0], st[1][1], shade);
    screen_vertex(&v[2], x1, y1, z, st[2][0], st[2][1], shade);
    v[3] = v[0];
    v[4] = v[2];
    screen_vertex(&v[5], x0, y1, z, st[3][0], st[3][1], shade);
    gfx_add_vertices(v, 6);
}

/* A rectangle's depth in clip space: the primitive depth with G_ZS_PRIM, otherwise the nearest. The game computes
   a 3D sprite's primitive depth as (z_ndc * 511 + 511) * 32 (PrintSprite), so z_ndc is the depth / 16352 - 1. A
   divisor of 16384 puts a sprite nearer than its place: a sprite 500 units away then shows through a wall up to 20 units in front of it. */
static float rect_depth(void) {
    if (gfxRdp.otherL & RM_ZS_PRIM) {
        return gfxRdp.primDepth / 16352.0f - 1.0f;
    }
    return -1.0f;
}

/* A rectangle that spans the whole screen width (a fade, a bar, a backdrop) is stretched to the whole widened
   frame. Returns 1 when it was. */
static int stretch_to_frame(float *x0, float *x1) {
    float extra = (wideK - 1.0f) * (SCREEN_W / 2);

    if (wideK > 1.0f && *x0 <= 0.0f && *x1 >= SCREEN_W) {
        *x0 -= extra;
        *x1 += extra;
        return 1;
    }
    return 0;
}

static int is_framebuffer(u32 addr) {
    addr &= 0x1FFFFFFF;
    return (rsp.cfb[0] && addr == (rsp.cfb[0] & 0x1FFFFFFF)) || (rsp.cfb[1] && addr == (rsp.cfb[1] & 0x1FFFFFFF));
}

static void fill_rect(float ulx, float uly, float lrx, float lry) {
    static const float noTex[4][2];
    static const float noShade[4];

    if (CYCLE_TYPE(gfxRdp.otherH) == CYC_FILL) {
        u32 c = gfxRdp.fillColor >> 16;
        float col[4];
        int depth;

        /* Fill mode includes the lower right edge. */
        lrx += 1.0f;
        lry += 1.0f;
        /* A fill of anything but a frame buffer clears the depth buffer. Without the frame buffers' addresses,
           the depth buffer's clear value (G_MAXFBZ) tells. */
        if (rsp.cfb[0] || rsp.cfb[1]) {
            depth = !is_framebuffer(rsp.colorImage);
        } else {
            depth = gfxRdp.fillColor == 0xFFFCFFFCu;
        }
        if (depth) {
            gfx_clear_depth();
            return;
        }
        col[0] = ((c >> 11) & 0x1F) / 31.0f;
        col[1] = ((c >> 6) & 0x1F) / 31.0f;
        col[2] = ((c >> 1) & 0x1F) / 31.0f;
        col[3] = 1.0f;
        if (ulx <= 0 && uly <= 0 && lrx >= SCREEN_W && lry >= SCREEN_H && gfxRdp.scissor[0] == 0
            && gfxRdp.scissor[1] == 0 && gfxRdp.scissor[2] >= SCREEN_W * 4 && gfxRdp.scissor[3] >= SCREEN_H * 4) {
            gfx_clear_color(col);
            return;
        }
        {
            int wide = stretch_to_frame(&ulx, &lrx);

            if (gfx_prepare(GFX_PRIM_FILL, 0, NULL, NULL, wide)) {
                quad(ulx, uly, lrx, lry, -1.0f, noTex, col);
            }
        }
        return;
    }
    /* In 1-cycle and 2-cycle mode the RDP leaves out the lower right edge. Here it is included, as the browser
       port's renderer does: a full-screen fade then covers the last row and column too. */
    {
        float x1 = lrx + 1.0f;
        int wide = !(gfxRdp.otherL & RM_ZS_PRIM) ? stretch_to_frame(&ulx, &x1) : view_is_wide();

        if (gfx_prepare(GFX_PRIM_RECT, 0, NULL, NULL, wide)) {
            quad(ulx, uly, x1, lry + 1.0f, rect_depth(), noTex, noShade);
        }
    }
}

/* G_TEXRECT and G_TEXRECTFLIP; h1 and h2 are the two words that follow (s and t; dsdx and dtdy). */
static void tex_rect(u32 w0, u32 w1, u32 h1, u32 h2, int flip) {
    static const float noShade[4];
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f;
    float lry = (w0 & 0xFFF) / 4.0f;
    int tile = (w1 >> 24) & 7;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f;
    float uly = (w1 & 0xFFF) / 4.0f;
    float s = (s16)(h1 >> 16) / 32.0f;
    float t = (s16)(h1 & 0xFFFF) / 32.0f;
    float dsdx = (s16)(h2 >> 16) / 1024.0f;
    float dtdy = (s16)(h2 & 0xFFFF) / 1024.0f;
    float st[4][2];
    float ds;
    float dt;
    float rectZ = 0.0f;
    int haveZ = 0;

    if (CYCLE_TYPE(gfxRdp.otherH) == CYC_COPY) {
        /* Copy mode steps four texels per dsdx unit and includes the lower right edge. */
        dsdx /= 4.0f;
        lrx += 1.0f;
        lry += 1.0f;
    }
    if (lrx <= ulx || lry <= uly) {
        return;
    }
    if (gfxInterpMode) {
        /* The rectangle is named by the texture it shows and the part of it (s, t and their steps). */
        GfxTexDesc desc;
        GfxSampler samp;

        if (gfx_tex_describe_tile(tile, &desc, &samp)) {
            const GfxTile *tl = &gfxRdp.tiles[tile];
            u32 key = gfx_interp_mix(gfx_interp_mix(gfx_interp_mix(desc.addr, desc.stride), h1), h2);
            float rc[4];
            GfxRectView view;
            const GfxRectView *v3d = NULL;

            key = gfx_interp_mix(gfx_interp_mix(key, tl->uls | tl->ult << 12), tl->lrs | tl->lrt << 12);
            key = gfx_interp_mix(key, desc.fmt | desc.siz << 4 | flip << 8);
            rc[0] = ulx, rc[1] = uly, rc[2] = lrx, rc[3] = lry;
            if (gfxRdp.otherL & RM_ZS_PRIM) {
                /* A 3D sprite: the game placed it with the latest camera. */
                memcpy(view.vp, rsp.proj, sizeof(view.vp));
                view.vpScale[0] = rsp.vpScale[0], view.vpScale[1] = rsp.vpScale[1];
                view.vpTrans[0] = rsp.vpTrans[0], view.vpTrans[1] = rsp.vpTrans[1];
                view.z = rect_depth();
                v3d = &view;
            }
            if (gfxInterpMode == 1) {
                gfx_interp_record_rect(key, gfx_interp_mix(desc.addr, desc.stride), rc, v3d);
            } else if (gfx_interp_draw_rect(key, rc, v3d, &rectZ)) {
                haveZ = 1;
            }
            ulx = rc[0], uly = rc[1], lrx = rc[2], lry = rc[3];
        }
    }
    if (rsp.host) {
        /* The host draws it as two triangles at this depth: the primitive depth, or near the front. */
        gfx_host_texrect(ulx, uly, lrx, lry, haveZ ? rectZ : (gfxRdp.otherL & RM_ZS_PRIM) ? rect_depth() : -0.99f, tile,
                         s, t, dsdx, dtdy, flip);
        return;
    }
    /* A 3D sprite lies in the widened view; other rectangles stay in the 4:3 area. */
    if (rsp.dry || !gfx_prepare(GFX_PRIM_TEXRECT, tile, NULL, NULL, (gfxRdp.otherL & RM_ZS_PRIM) && view_is_wide())) {
        return;
    }
    if (!flip) {
        ds = (lrx - ulx) * dsdx;
        dt = (lry - uly) * dtdy;
        st[0][0] = s, st[0][1] = t;
        st[1][0] = s + ds, st[1][1] = t;
        st[2][0] = s + ds, st[2][1] = t + dt;
        st[3][0] = s, st[3][1] = t + dt;
    } else {
        /* s runs down the screen and t across it. */
        ds = (lry - uly) * dsdx;
        dt = (lrx - ulx) * dtdy;
        st[0][0] = s, st[0][1] = t;
        st[1][0] = s, st[1][1] = t + dt;
        st[2][0] = s + ds, st[2][1] = t + dt;
        st[3][0] = s + ds, st[3][1] = t;
    }
    quad(ulx, uly, lrx, lry, haveZ ? rectZ : rect_depth(), st, noShade);
}

/* ---- Sprite2D microcode (backdrops). The microcode loads the image in strips and draws a texture rectangle for
   each, with the RDP state the game set up. Here the whole image is one texture. */

static void sprite_draw(u32 w1) {
    static const float noShade[4];
    const u8 *p = rsp.sprite ? gfx_mem(rsp.sprite, 20) : NULL;
    GfxTexDesc desc;
    GfxSampler samp;
    u32 src;
    u32 tlut;
    int stride;
    int width;
    int height;
    int fmt;
    int siz;
    int offS;
    int offT;
    float x0;
    float y0;
    float x1;
    float y1;
    float st[4][2];
    float s0;
    float s1;
    float t0;
    float t1;
    int wide;
    int i;

    if (p == NULL) {
        return;
    }
    src = seg_addr(u32_at(p));
    tlut = seg_addr(u32_at(p + 4));
    stride = s16_at(p + 8);
    width = s16_at(p + 10);
    height = s16_at(p + 12);
    fmt = p[14] & 7;
    siz = p[15] & 3;
    offS = s16_at(p + 16);
    offT = s16_at(p + 18);
    if (width <= 0 || height <= 0 || stride <= 0 || src == 0) {
        return;
    }
    memset(&desc, 0, sizeof(desc));
    desc.fmt = fmt;
    desc.siz = siz;
    desc.width = width > 1024 ? 1024 : width;
    desc.height = height > 1024 ? 1024 : height;
    /* Bytes per row and the sub-image's first texel; a 4-bit texel is half a byte. */
    desc.stride = siz == SIZ_4B ? (u32)stride / 2 : (u32)stride << (siz - 1);
    desc.addr = src + desc.stride * offT + (siz == SIZ_4B ? offS / 2 : offS << (siz - 1));
    if (siz <= SIZ_8B && TEX_LUT(gfxRdp.otherH) != 0 && tlut != 0) {
        desc.fmt = FMT_CI;
        desc.palType = TEX_LUT(gfxRdp.otherH);
        for (i = 0; i < (siz == SIZ_4B ? 1 : 16); i++) {
            desc.pal[i] = tlut + i * 32;
        }
    }
    gfx_tex_clamp_sampler(&samp, desc.width, desc.height);

    /* The position is in quarter pixels; the scale is texels per pixel in 1/1024 units. */
    x0 = (s16)(w1 >> 16) / 4.0f;
    y0 = (s16)(w1 & 0xFFFF) / 4.0f;
    x1 = x0 + width / rsp.spriteScale[0];
    y1 = y0 + height / rsp.spriteScale[1];
    s0 = rsp.spriteFlip[0] ? width - 1 : 0;
    s1 = rsp.spriteFlip[0] ? -1 : width;
    t0 = rsp.spriteFlip[1] ? height - 1 : 0;
    t1 = rsp.spriteFlip[1] ? -1 : height;
    st[0][0] = s0, st[0][1] = t0;
    st[1][0] = s1, st[1][1] = t0;
    st[2][0] = s1, st[2][1] = t1;
    st[3][0] = s0, st[3][1] = t1;
    if (gfxInterpMode) {
        /* A backdrop is named by its image and its place in it. */
        u32 key = gfx_interp_mix(gfx_interp_mix(desc.addr, desc.stride), desc.width | desc.height << 16);
        float rc[4];

        key = gfx_interp_mix(key, desc.fmt | desc.siz << 4 | rsp.spriteFlip[0] << 8 | rsp.spriteFlip[1] << 9);
        rc[0] = x0, rc[1] = y0, rc[2] = x1, rc[3] = y1;
        if (gfxInterpMode == 1) {
            gfx_interp_record_rect(key, gfx_interp_mix(desc.addr, desc.stride), rc, NULL);
        } else {
            gfx_interp_draw_rect(key, rc, NULL, NULL);
            x0 = rc[0], y0 = rc[1], x1 = rc[2], y1 = rc[3];
        }
    }
    /* A backdrop that fills the screen is stretched to the widened frame. */
    wide = stretch_to_frame(&x0, &x1);
    if (!rsp.dry && gfx_prepare(GFX_PRIM_TEXRECT, 0, &desc, &samp, wide)) {
        quad(x0, y0, x1, y1, -1.0f, st, noShade);
    }
}

/* ---- Other RSP commands. */

static void move_mem(u32 index, u32 addr) {
    const u8 *p;

    addr = seg_addr(addr);
    if (index == G_MV_VIEWPORT) {
        if ((p = gfx_mem(addr, 16)) != NULL) {
            rsp.vpScale[0] = s16_at(p) / 4.0f;
            rsp.vpScale[1] = s16_at(p + 2) / 4.0f;
            rsp.vpTrans[0] = s16_at(p + 8) / 4.0f;
            rsp.vpTrans[1] = s16_at(p + 10) / 4.0f;
            if (gfxInterpMode) {
                gfx_interp_viewport(rsp.vpScale, rsp.vpTrans, gfxInterpMode == 1);
            }
        }
    } else if (index == G_MV_LOOKATX || index == G_MV_LOOKATY) {
        /* A LookAt direction: signed bytes at offset 8, like a light's. */
        if ((p = gfx_mem(addr, 16)) != NULL) {
            float *d = rsp.lookAt[index == G_MV_LOOKATY ? 1 : 0];
            d[0] = (s8)p[8] / 127.0f;
            d[1] = (s8)p[9] / 127.0f;
            d[2] = (s8)p[10] / 127.0f;
        }
    } else if (index >= G_MV_L0 && index <= G_MV_L7) {
        /* A Light: colour bytes, then a normalised direction as signed bytes at offset 8. */
        if ((p = gfx_mem(addr, 16)) != NULL) {
            Light *l = &rsp.lights[(index - G_MV_L0) >> 1];
            l->r = p[0] / 255.0f;
            l->g = p[1] / 255.0f;
            l->b = p[2] / 255.0f;
            l->x = (s8)p[8] / 127.0f;
            l->y = (s8)p[9] / 127.0f;
            l->z = (s8)p[10] / 127.0f;
        }
    }
}

static void move_word(u32 index, u32 offset, u32 data) {
    switch (index) {
        case G_MW_NUMLIGHT:
            /* (lights + 1) * 32 + 0x80000000 */
            rsp.numLights = (int)((data - 0x80000000u) >> 5) - 1;
            if (rsp.numLights < 0) {
                rsp.numLights = 0;
            }
            if (rsp.numLights > 7) {
                rsp.numLights = 7;
            }
            break;
        case G_MW_SEGMENT:
            rsp.segments[(offset >> 2) & 0xF] = phys_addr(data);
            rsp.segmentSet |= 1u << ((offset >> 2) & 0xF);
            break;
        case G_MW_FOG:
            rsp.fogMul = (s16)(data >> 16);
            rsp.fogOff = (s16)(data & 0xFFFF);
            break;
        case G_MW_LIGHTCOL:
            if ((offset & 7) == 0) {
                Light *l = &rsp.lights[(offset >> 5) & 7];
                l->r = (data >> 24) / 255.0f;
                l->g = ((data >> 16) & 0xFF) / 255.0f;
                l->b = ((data >> 8) & 0xFF) / 255.0f;
            }
            break;
        default:
            break;
    }
}

static void unpack_color(float *c, u32 w) {
    c[0] = (w >> 24) / 255.0f;
    c[1] = ((w >> 16) & 0xFF) / 255.0f;
    c[2] = ((w >> 8) & 0xFF) / 255.0f;
    c[3] = (w & 0xFF) / 255.0f;
}

static u32 othermode_mask(u32 w0) {
    u32 shift = (w0 >> 8) & 0xFF;
    u32 len = w0 & 0xFF;
    u32 bits = len >= 32 ? 0xFFFFFFFFu : (1u << len) - 1;

    return shift >= 32 ? 0 : bits << shift;
}

/* Every command but the display list controls and the ones that take more than one word. */
static void command(u32 op, u32 w0, u32 w1) {
    GfxTile *t;

    if (rsp.dry && op != G_MTX && op != G_VTX && op != G_POPMTX && op != G_MOVEWORD && op != G_MOVEMEM
        && op != G_SETTIMG && op != G_LOADBLOCK && op != G_LOADTILE && op != G_LOADTLUT && op != G_SETTILE
        && op != G_SETTILESIZE && op != G_SPRITE2D_BASE && op != G_CULLDL && op != G_SETOTHERMODE_H
        && op != G_SETOTHERMODE_L && op != G_SETOTHERMODE && op != G_SETPRIMDEPTH) {
        /* The rectangles need the cycle type, the depth source and the primitive depth. */
        return;
    }

    switch (op) {
        case G_MTX:
            matrix(w0, w1);
            break;
        case G_MOVEMEM:
            move_mem((w0 >> 16) & 0xFF, w1);
            break;
        case G_VTX:
            load_vertices(w1, (w0 >> 10) & 0x3F, ((w0 >> 16) & 0xFF) >> 1);
            break;
        case G_SPRITE2D_BASE:
            if (rsp.sprite2d) {
                rsp.sprite = seg_addr(w1);
            }
            break;
        case G_TRI1:
            if (!rsp.sprite2d) {
                triangle(((w1 >> 16) & 0xFF) >> 1, ((w1 >> 8) & 0xFF) >> 1, (w1 & 0xFF) >> 1);
            }
            break;
        case G_TRI2:
            triangle(((w0 >> 16) & 0xFF) >> 1, ((w0 >> 8) & 0xFF) >> 1, (w0 & 0xFF) >> 1);
            triangle(((w1 >> 16) & 0xFF) >> 1, ((w1 >> 8) & 0xFF) >> 1, (w1 & 0xFF) >> 1);
            break;
        case G_MODIFYVTX:
            modify_vertex((w0 & 0xFFFF) >> 1, (w0 >> 16) & 0xFF, w1);
            break;
        case G_POPMTX: /* G_SPRITE2D_DRAW in the Sprite2D microcode */
            if (rsp.sprite2d) {
                sprite_draw(w1);
            } else if (rsp.mvDepth > 0) {
                rsp.mvDepth--;
                rsp.mvpValid = 0;
            }
            break;
        case G_CULLDL: /* G_SPRITE2D_SCALEFLIP in the Sprite2D microcode; G_CULLDL is handled by run_list */
            if (rsp.sprite2d) {
                u32 sx = w1 >> 16;
                u32 sy = w1 & 0xFFFF;
                rsp.spriteScale[0] = (sx ? sx : 1024) / 1024.0f;
                rsp.spriteScale[1] = (sy ? sy : 1024) / 1024.0f;
                rsp.spriteFlip[0] = ((w0 >> 8) & 0xFF) != 0;
                rsp.spriteFlip[1] = (w0 & 0xFF) != 0;
            }
            break;
        case G_MOVEWORD:
            /* F3DEX 1.x keeps the index in the low byte and the offset above it (G_MOVEMEM is the other way). */
            move_word(w0 & 0xFF, (w0 >> 8) & 0xFFFF, w1);
            break;
        case G_TEXTURE:
            rsp.texScale[0] = (w1 >> 16) ? (w1 >> 16) / 65536.0f : 1.0f / 65536.0f;
            rsp.texScale[1] = (w1 & 0xFFFF) ? (w1 & 0xFFFF) / 65536.0f : 1.0f / 65536.0f;
            gfxRdp.texTile = (w0 >> 8) & 7;
            gfxRdp.texOn = (w0 & 0xFF) != 0;
            gfx_state_changed();
            break;
        case G_SETOTHERMODE_H: {
            u32 mask = othermode_mask(w0);
            gfxRdp.otherH = (gfxRdp.otherH & ~mask) | (w1 & mask);
            gfx_state_changed();
            break;
        }
        case G_SETOTHERMODE_L: {
            u32 mask = othermode_mask(w0);
            gfxRdp.otherL = (gfxRdp.otherL & ~mask) | (w1 & mask);
            gfx_state_changed();
            break;
        }
        case G_SETOTHERMODE:
            gfxRdp.otherH = w0 & 0xFFFFFF;
            gfxRdp.otherL = w1;
            gfx_state_changed();
            break;
        case G_SETGEOMETRYMODE:
            gfxRdp.geometryMode |= w1;
            gfx_state_changed();
            break;
        case G_CLEARGEOMETRYMODE:
            gfxRdp.geometryMode &= ~w1;
            gfx_state_changed();
            break;
        case G_SETSCISSOR:
            gfxRdp.scissor[0] = (w0 >> 12) & 0xFFF;
            gfxRdp.scissor[1] = w0 & 0xFFF;
            gfxRdp.scissor[2] = (w1 >> 12) & 0xFFF;
            gfxRdp.scissor[3] = w1 & 0xFFF;
            gfx_state_changed();
            break;
        case G_SETPRIMDEPTH:
            gfxRdp.primDepth = w1 >> 16;
            break;
        case G_LOADTLUT:
            gfx_tex_load_tlut((w1 >> 24) & 7, (w1 >> 14) & 0x3FF);
            gfx_state_changed();
            break;
        case G_SETTILESIZE:
            t = &gfxRdp.tiles[(w1 >> 24) & 7];
            t->uls = (w0 >> 12) & 0xFFF;
            t->ult = w0 & 0xFFF;
            t->lrs = (w1 >> 12) & 0xFFF;
            t->lrt = w1 & 0xFFF;
            gfx_state_changed();
            break;
        case G_LOADBLOCK:
            gfx_tex_load_block((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            gfx_state_changed();
            break;
        case G_LOADTILE:
            gfx_tex_load_tile((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            gfx_state_changed();
            break;
        case G_SETTILE:
            t = &gfxRdp.tiles[(w1 >> 24) & 7];
            t->fmt = (w0 >> 21) & 7;
            t->siz = (w0 >> 19) & 3;
            t->line = (w0 >> 9) & 0x1FF;
            t->tmem = w0 & 0x1FF;
            t->pal = (w1 >> 20) & 0xF;
            t->cmt = (w1 >> 18) & 3;
            t->maskt = (w1 >> 14) & 0xF;
            t->shiftt = (w1 >> 10) & 0xF;
            t->cms = (w1 >> 8) & 3;
            t->masks = (w1 >> 4) & 0xF;
            t->shifts = w1 & 0xF;
            gfx_state_changed();
            break;
        case G_FILLRECT:
            fill_rect(((w1 >> 12) & 0xFFF) / 4.0f, (w1 & 0xFFF) / 4.0f, ((w0 >> 12) & 0xFFF) / 4.0f,
                      (w0 & 0xFFF) / 4.0f);
            break;
        case G_SETFILLCOLOR:
            gfxRdp.fillColor = w1;
            break;
        case G_SETFOGCOLOR:
            unpack_color(gfxRdp.fog, w1);
            gfx_state_changed();
            break;
        case G_SETBLENDCOLOR:
            unpack_color(gfxRdp.blend, w1);
            gfx_state_changed();
            break;
        case G_SETPRIMCOLOR:
            unpack_color(gfxRdp.prim, w1);
            gfxRdp.primLod = (w0 & 0xFF) / 255.0f;
            gfx_state_changed();
            break;
        case G_SETENVCOLOR:
            unpack_color(gfxRdp.env, w1);
            gfx_state_changed();
            break;
        case G_SETCOMBINE:
            if (gfxRdp.mux0 != (w0 & 0xFFFFFF) || gfxRdp.mux1 != w1) {
                gfxRdp.mux0 = w0 & 0xFFFFFF;
                gfxRdp.mux1 = w1;
                gfx_state_changed();
            }
            break;
        case G_SETTIMG:
            gfx_tex_set_image((w0 >> 21) & 7, (w0 >> 19) & 3, (w0 & 0xFFF) + 1, seg_addr(w1));
            break;
        case G_SETCIMG:
            rsp.colorImage = seg_addr(w1);
            break;
        default:
            break;
    }
}

/* A layer: the mark that holds the command at addr, a word of the game's memory as the marks have it. A list that a
   command calls keeps that command's layer. */
static void layer_at(u32 addr) {
    while (gfxLayer.next < gfxLayer.count && gfxLayer.marks[gfxLayer.next].addr <= addr) {
        gfxLayer.kind = gfxLayer.marks[gfxLayer.next].layer;
        gfxLayer.next++;
    }
}

static void run_list(u32 addr, int depth) {
    u32 pc = seg_addr(addr);
    u32 start = pc;
    int n;

    for (n = 0; n < MAX_COMMANDS; n++) {
        const u8 *p = gfx_mem(pc, 8);
        u32 w0;
        u32 w1;
        u32 op;

        if (p == NULL) {
            return;
        }
        if (gfxLayer.on && depth == 0) {
            layer_at(addr + (pc - start));
        }
        w0 = u32_at(p);
        w1 = u32_at(p + 4);
        pc += 8;
        op = w0 >> 24;
        if (op == G_DL) {
            if (depth < MAX_DL_DEPTH) {
                u32 savedPath = rsp.pathHash;
                u32 target = seg_addr(w1);

                if (gfxInterpMode && !gfx_interp_is_volatile(target)) {
                    rsp.pathHash = gfx_interp_mix(savedPath, target);
                }
                if (rsp.host) {
                    gfx_host_call(depth + 1);
                }
                run_list(w1, depth + 1);
                if (rsp.host) {
                    gfx_host_return(w0, depth + 1);
                }
                rsp.pathHash = savedPath;
            }
            if ((w0 >> 16) & 0xFF) {
                return; /* a branch: the list does not come back */
            }
            continue;
        }
        if (op == G_ENDDL) {
            return;
        }
        if (op == G_CULLDL && rsp.host) {
            /* The host's microcode tests the clip codes of its own view. */
            gfx_host_command(op, w0, w1, pc);
            continue;
        }
        if (op == G_CULLDL && !rsp.sprite2d && !rsp.noCull) {
            if (cull_vertices((w0 & 0xFFFF) >> 1, (w1 & 0xFFFF) >> 1)) {
                return;
            }
            continue;
        }
        if (op == G_TEXRECT || op == G_TEXRECTFLIP) {
            /* The texture coordinates follow in the next two commands' second words. */
            const u8 *h = gfx_mem(pc, 16);
            if (h == NULL) {
                return;
            }
            tex_rect(w0, w1, u32_at(h + 4), u32_at(h + 12), op == G_TEXRECTFLIP);
            pc += 16;
            continue;
        }
        rsp.pc = pc;
        command(op, w0, w1);
        if (rsp.host) {
            gfx_host_command(op, w0, w1, pc);
        }
    }
}

/* ---- Frames and tasks. */

void gfx_dl_set_framebuffers(u32 cfb0, u32 cfb1) {
    rsp.cfb[0] = cfb0;
    rsp.cfb[1] = cfb1;
}

/* The state every frame starts with, as the browser port's renderer resets it. */
void gfx_dl_begin_frame(void) {
    int i;

    if (!rsp.dry) {
        gfxScene.valid = 0;
        scenePending = 0;
    }
    rsp.segmentSet = 0;
    memset(rsp.segments, 0, sizeof(rsp.segments));
    rsp.mvDepth = 0;
    mat_identity(rsp.mvStack[0]);
    mat_identity(rsp.proj);
    rsp.mvpValid = 0;
    memset(rsp.vtx, 0, sizeof(rsp.vtx));
    for (i = 0; i < NUM_VERTICES; i++) {
        rsp.vtx[i].v.w = 1.0f;
        rsp.vtx[i].v.r = rsp.vtx[i].v.g = rsp.vtx[i].v.b = rsp.vtx[i].v.a = 1.0f;
    }
    rsp.vpScale[0] = rsp.vpTrans[0] = SCREEN_W / 2;
    rsp.vpScale[1] = rsp.vpTrans[1] = SCREEN_H / 2;
    rsp.fogMul = rsp.fogOff = 0.0f;
    rsp.numLights = 0;
    memset(rsp.lights, 0, sizeof(rsp.lights));
    for (i = 0; i < 8; i++) {
        rsp.lights[i].z = 1.0f;
    }
    memset(rsp.lookAt, 0, sizeof(rsp.lookAt));
    rsp.lookAt[0][0] = 1.0f;
    rsp.lookAt[1][1] = 1.0f;
    rsp.texScale[0] = rsp.texScale[1] = 1.0f;
    rsp.sprite = 0;
    rsp.spriteScale[0] = rsp.spriteScale[1] = 1.0f;
    rsp.spriteFlip[0] = rsp.spriteFlip[1] = 0;
    rsp.colorImage = 0;
    rsp.pathHash = 0;
    gfxLayer.kind = 0;
    gfxLayer.next = 0;
    rsp.projRec = -1;
    for (i = 0; i < STACK_DEPTH; i++) {
        rsp.mvRec[i] = -1;
    }

    memset(&gfxRdp, 0, sizeof(gfxRdp));
    for (i = 0; i < 4; i++) {
        gfxRdp.prim[i] = gfxRdp.env[i] = 1.0f;
    }
    gfxRdp.fog[3] = 1.0f;
    gfxRdp.scissor[2] = SCREEN_W * 4;
    gfxRdp.scissor[3] = SCREEN_H * 4;
    gfx_tex_begin_frame();
    gfx_state_changed();
}

void gfx_dl_run_task(u32 addr, int sprite2d) {
    rsp.sprite2d = sprite2d;
    run_list(addr, 0);
}

/* The host translator's walk of the F3DEX tasks (gfx_hostgfx.c): a dry pass that reports each command, blending the
   matrices when gfx_interp_begin_draw allows. */
void gfx_dl_host_walk(const GfxTask *tasks, int count) {
    int i;

    rsp.dry = 1;
    rsp.host = 1;
    gfx_dl_begin_frame();
    for (i = 0; i < count; i++) {
        if (!tasks[i].sprite2d) {
            gfx_host_task(i);
            gfx_dl_run_task(tasks[i].addr, 0);
            gfx_host_return(0, 0);
        }
    }
    rsp.host = 0;
    rsp.dry = 0;
}

void gfx_dl_set_cull(int on) {
    rsp.noCull = !on;
}

void gfx_dl_view(float *proj, float *vpScale, float *vpTrans) {
    memcpy(proj, rsp.proj, sizeof(rsp.proj));
    vpScale[0] = rsp.vpScale[0];
    vpScale[1] = rsp.vpScale[1];
    vpTrans[0] = rsp.vpTrans[0];
    vpTrans[1] = rsp.vpTrans[1];
}

u32 gfx_dl_seg_addr(u32 addr) {
    return seg_addr(addr);
}

void gfx_dl_modelview(float *m) {
    memcpy(m, rsp.mvStack[rsp.mvDepth], sizeof(rsp.mvStack[0]));
}

int gfx_dl_last_matrix(float *m) {
    memcpy(m, rsp.lastMtx, sizeof(rsp.lastMtx));
    return rsp.lastMtxBlended;
}

/* Runs the tasks as a recording pass for the interpolation: the matrices and the clip codes, nothing drawn. */
void gfx_dl_record(const GfxTask *tasks, int count, int cut) {
    int i;

    gfx_interp_begin_record(cut);
    if (gfxInterpMode == 1) {
        rsp.dry = 1;
        gfx_dl_begin_frame();
        for (i = 0; i < count; i++) {
            gfx_dl_run_task(tasks[i].addr, tasks[i].sprite2d);
        }
        rsp.dry = 0;
    }
    gfx_interp_end_record();
}
