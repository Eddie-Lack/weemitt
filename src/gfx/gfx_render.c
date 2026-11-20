/* Draw states, combiner shaders, batching and the OpenGL draws.

   The interpreter hands over primitives one at a time. A primitive's draw state (shader, textures, uniforms and
   OpenGL state) is built from the RDP's state only when that state changed. Consecutive primitives with the same
   draw state share one draw. The frame is recorded first (vertices, states, draws and clears) and played at the
   end: the vertices go to the GPU in one upload.

   The shaders do the RDP's work per pixel:
   - texture sampling as the RDP does it: shift, tile origin, clamp, wrap and mirror, texel centres on whole
     coordinates, and 3-point (or 4-point) bilinear filtering;
   - the colour combiner, one or two cycles;
   - the alpha compare and the coverage test of cut-out textures;
   - the blender: a first cycle in two-cycle mode (fog), then the last cycle with OpenGL's blending where it reads
     the frame buffer. */
#include "gfx_host.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_internal.h"

GfxStats gfxStats;
GfxLayerState gfxLayer;

/* ---- Program keys. */

/* Combiner modes. */
#define MODE_1CYCLE 0
#define MODE_2CYCLE 1
#define MODE_COPY 2 /* texel 0 as it is */
#define MODE_FILL 3 /* the shade colour, which holds the fill colour */

/* Alpha tests. */
#define ALPHA_NONE 0
#define ALPHA_THRESHOLD 1 /* against the blend colour's alpha */
#define ALPHA_DITHER 2    /* against noise */
#define ALPHA_COPY 3      /* copy mode: against the blend colour's alpha, at least 1/255 */

/* Cut-out textures: the render mode takes coverage from alpha (CVG_X_ALPHA with ALPHA_CVG_SEL). The RDP writes a
   pixel when its coverage times alpha is at least 1/8. Over a pixel that is fully covered already (any pixel of
   earlier geometry or of a cleared frame), the pixel then replaces the colour: the alpha does not blend. */
#define COVERAGE_NONE 0
#define COVERAGE_EDGE 1      /* keep alpha 1/8 and up, drawn opaque */
#define COVERAGE_BLENDED 3   /* the blender mixes by alpha already: keep alpha 1/8 and up */

/* Blender inputs. */
#define BL_IN 0
#define BL_MEM 1
#define BL_BLEND 2
#define BL_FOG 3

typedef struct ProgramKey {
    u32 mux0, mux1;
    u8 mode;       /* MODE_ */
    u8 textures;   /* bit 0: texel 0 is sampled; bit 1: texel 1 */
    u8 persp;      /* perspective-correct texture coordinates */
    u8 alphaTest;  /* ALPHA_ */
    u8 coverage;   /* COVERAGE_ */
    u8 blend1;     /* the first blender cycle in two-cycle mode: P << 6 | A << 4 | M << 2 | B */
    u8 blend2;     /* the last blender cycle, the same way */
    u8 forceBlend; /* the last cycle blends; otherwise it passes its P input */
    u8 layer;      /* LAYER_ */
} ProgramKey;

/* The programs of a layer (gfx_draw_layer). The colour pass writes coverage to alpha. Over transparent black that
   gives premultiplied colour. The coverage is 1 where the RDP's blender does not read memory or replaces it, the
   blend weight where it mixes with memory by 1 - a, and nothing new where it adds. The depth pass writes the depth of
   each pixel that adds coverage, or colour where it adds. */
#define LAYER_NONE 0
#define LAYER_COLOR 1
#define LAYER_DEPTH 2

/* The uniforms of a draw. */
typedef struct Uniforms {
    float prim[4], env[4], fog[4], blend[4];
    float misc[4]; /* primitive LOD fraction, pixels per N64 pixel, filter (0 point, 1 3-point, 2 bilinear), seed */
    GfxSampler samp[2];
} Uniforms;

typedef struct Program {
    ProgramKey key;
    GLuint id;
    GLint locTexA, locTexB, locTexC, locPrim, locEnv, locFog, locBlend, locMisc;
    Uniforms current;
    int currentValid;
} Program;

/* Everything a draw needs. Compared with memcmp: built from a zeroed struct. */
typedef struct DrawState {
    Program *prog;
    GLuint tex[2];
    Uniforms u;
    u8 depthTest, depthCompare, depthWrite, decal;
    u8 blend;        /* OpenGL blending on */
    u8 dstFactor;    /* DST_ */
    u8 wide;         /* may reach beyond the 4:3 area (see gfx_prepare) */
    u16 scissor[4];  /* 10.2 fixed point */
} DrawState;

/* OpenGL destination factors (the source factor is always GL_ONE: the shader weighs its own colour). */
enum { DST_ZERO, DST_ONE, DST_SRC_ALPHA, DST_ONE_MINUS_SRC_ALPHA };

enum { CMD_DRAW, CMD_CLEAR_COLOR, CMD_CLEAR_DEPTH };

typedef struct Cmd {
    int type;
    int state;
    int first, count;
    float color[4];
} Cmd;

#define MAX_PROGRAMS 512

static struct {
    Program programs[MAX_PROGRAMS];
    int programCount;
    GLuint vs;
    GLuint vao, vbo;
    GLuint white;
    GLuint composite, compositeVao; /* gfx_render_composite */
    int msaa;        /* the frame buffer is multisampled */
    int filter;      /* 0 point, 1 3-point, 2 bilinear: what a primitive that asks for filtering gets */
    float seed;
    int trace;       /* GLOVER_GFX_TRACE: print every draw */

    /* The frame being recorded. */
    int width, height;
    float wide;      /* the widening of the view; the 4:3 area is width / wide pixels wide */
    GfxVertex *verts;
    int vertCount, vertCap;
    DrawState *states;
    int stateCount, stateCap;
    Cmd *cmds;
    int cmdCount, cmdCap;
    int openDraw;    /* the draw that takes the next vertices, or -1 */
    int dirty;       /* the RDP's state changed since the last draw state was built */
    int lastKind, lastTile, lastWide;

    /* The OpenGL state the played draws left. */
    struct {
        Program *prog;
        GLuint tex[2];
        int depthTest, depthCompare, depthWrite, decal, blend, dstFactor, scissorOn;
        GLint scissor[4];
    } gl;
} R;

/* ---- Growing arrays. */

static int grow(void **p, int *cap, int need, int size) {
    int n;
    void *q;

    if (need <= *cap) {
        return 1;
    }
    n = *cap ? *cap : 256;
    while (n < need) {
        n *= 2;
    }
    q = realloc(*p, (size_t)n * size);
    if (q == NULL) {
        return 0;
    }
    *p = q;
    *cap = n;
    return 1;
}

/* ---- Shader text. */

typedef struct Str {
    char buf[16384];
    int len;
} Str;

static void put(Str *s, const char *fmt, ...) {
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(s->buf + s->len, sizeof(s->buf) - s->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        s->len += n;
        if (s->len >= (int)sizeof(s->buf)) {
            s->len = sizeof(s->buf) - 1;
        }
    }
}

/* The RDP interpolates shade, and texture coordinates without perspective correction, linearly across the
   screen. GLSL's noperspective qualifier would do that, but some drivers (Mesa's llvmpipe) get it wrong for
   triangles they clip against the guard band. So the vertex shader multiplies those attributes by w, and the
   fragment shader divides them by the interpolated w: perspective-correct interpolation of a * w and w gives
   the screen-linear a. */
static const char VERTEX_SHADER[] =
    "#version 330 core\n"
    "layout(location = 0) in vec4 aPos;\n"
    "layout(location = 1) in vec4 aShade;\n"
    "layout(location = 2) in vec2 aTex;\n"
    "out vec4 vShadeW;\n"
    "out vec2 vTexP;\n"
    "out vec2 vTexAW;\n"
    "out float vW;\n"
    "void main() {\n"
    "    gl_Position = aPos;\n"
    "    vShadeW = aShade * aPos.w;\n"
    "    vTexP = aTex;\n"
    "    vTexAW = aTex * aPos.w;\n"
    "    vW = aPos.w;\n"
    "}\n";

/* The fragment shader's fixed part. The RDP's texture sampling, for one tile:
   - the coordinate is shifted, then the tile's origin is subtracted;
   - with clamping, a coordinate at or past the tile's lower right edge takes the last texel, and a negative one
     the first, both without a fraction;
   - with a mask, the texel index wraps every period texels, and mirrors every other period;
   - texel centres sit on whole coordinates, and the filter blends texel (s, t) with (s + 1, t), (s, t + 1) and
     (s + 1, t + 1), each wrapped or mirrored on its own. */
static const char FRAGMENT_HEAD[] =
    "#version 330 core\n"
    "in vec4 vShadeW;\n"
    "in vec2 vTexP;\n"
    "in vec2 vTexAW;\n"
    "in float vW;\n"
    "uniform sampler2D uTex0;\n"
    "uniform sampler2D uTex1;\n"
    "uniform vec4 uTexA[2];\n"
    "uniform vec4 uTexB[2];\n"
    "uniform ivec4 uTexC[2];\n"
    "uniform vec4 uPrim;\n"
    "uniform vec4 uEnv;\n"
    "uniform vec4 uFog;\n"
    "uniform vec4 uBlend;\n"
    "uniform vec4 uMisc;\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "\n"
    "int wrapIndex(int i, int period, int flags) {\n"
    "    if (period != 0) {\n"
    "        if ((flags & 2) != 0 && (i & period) != 0) {\n"
    "            i = ~i;\n"
    "        }\n"
    "        i &= period - 1;\n"
    "    }\n"
    "    return i;\n"
    "}\n"
    "\n"
    "void axisIndex(float c, float origin, float edge, float last, int flags, out int i, out float f) {\n"
    "    float rel = c - origin;\n"
    "    if ((flags & 1) != 0 && floor(c * 4.0) >= edge) {\n"
    "        i = int(last);\n"
    "        f = 0.0;\n"
    "    } else if ((flags & 1) != 0 && rel < 0.0) {\n"
    "        i = 0;\n"
    "        f = 0.0;\n"
    "    } else {\n"
    "        float fl = floor(rel);\n"
    "        i = int(fl);\n"
    "        f = rel - fl;\n"
    "    }\n"
    "}\n"
    "\n"
    "vec4 sampleTile(sampler2D tex, int n, vec2 st) {\n"
    "    vec4 a = uTexA[n];\n"
    "    vec4 b = uTexB[n];\n"
    "    ivec4 c = uTexC[n];\n"
    "    vec2 p = st * a.xy;\n"
    "    int s0, t0;\n"
    "    float fs, ft;\n"
    "    axisIndex(p.x, a.z, b.x, b.z, c.z, s0, fs);\n"
    "    axisIndex(p.y, a.w, b.y, b.w, c.w, t0, ft);\n"
    "    ivec2 last = textureSize(tex, 0) - 1;\n"
    "    int s1 = clamp(wrapIndex(s0 + 1, c.x, c.z), 0, last.x);\n"
    "    int t1 = clamp(wrapIndex(t0 + 1, c.y, c.w), 0, last.y);\n"
    "    s0 = clamp(wrapIndex(s0, c.x, c.z), 0, last.x);\n"
    "    t0 = clamp(wrapIndex(t0, c.y, c.w), 0, last.y);\n"
    "    vec4 c00 = texelFetch(tex, ivec2(s0, t0), 0);\n"
    "    if (uMisc.z < 0.5) {\n"
    "        return c00;\n"
    "    }\n"
    "    vec4 c10 = texelFetch(tex, ivec2(s1, t0), 0);\n"
    "    vec4 c01 = texelFetch(tex, ivec2(s0, t1), 0);\n"
    "    vec4 c11 = texelFetch(tex, ivec2(s1, t1), 0);\n"
    "    if (uMisc.z < 1.5) {\n"
    "        if (fs + ft < 1.0) {\n"
    "            return c00 + fs * (c10 - c00) + ft * (c01 - c00);\n"
    "        }\n"
    "        return c11 + (1.0 - fs) * (c01 - c11) + (1.0 - ft) * (c10 - c11);\n"
    "    }\n"
    "    return mix(mix(c00, c10, fs), mix(c01, c11, fs), ft);\n"
    "}\n"
    "\n"
    "void main() {\n";

/* Colour combiner inputs, as GLSL for one channel set. t0 and t1 name the texels this cycle sees as TEXEL0 and
   TEXEL1. */
static const char *rgb_input(char *buf, int slot, int code, const char *t0, const char *t1) {
    static const char *const common[6] = {"c", NULL, NULL, "uPrim", "shade", "uEnv"};

    if (code == 1 || code == 2) {
        return code == 1 ? t0 : t1;
    }
    if (code == 0 || (code >= 3 && code <= 5)) {
        return common[code];
    }
    switch (slot) {
        case 0: /* A: 1, noise, then 0 */
            return code == 6 ? "vec4(1.0)" : code == 7 ? "vec4(noise)" : "vec4(0.0)";
        case 1: /* B: key centre and K4 (not emulated: 0), then 0 */
            return "vec4(0.0)";
        case 2: /* C: key scale (0), alphas, LOD fractions, K5 (0), then 0 */
            switch (code) {
                case 7: return "vec4(c.a)";
                case 8: snprintf(buf, 32, "vec4(%s.a)", t0); return buf;
                case 9: snprintf(buf, 32, "vec4(%s.a)", t1); return buf;
                case 10: return "vec4(uPrim.a)";
                case 11: return "vec4(shade.a)";
                case 12: return "vec4(uEnv.a)";
                case 14: return "vec4(uMisc.x)";
                default: return "vec4(0.0)";
            }
        default: /* D: 1, then 0 */
            return code == 6 ? "vec4(1.0)" : "vec4(0.0)";
    }
}

static const char *alpha_input(char *buf, int mul, int code, const char *t0, const char *t1) {
    static const char *const abd[8] = {"c.a", NULL, NULL, "uPrim.a", "shade.a", "uEnv.a", "1.0", "0.0"};
    static const char *const c[8] = {"0.0", NULL, NULL, "uPrim.a", "shade.a", "uEnv.a", "uMisc.x", "0.0"};

    if (code == 1 || code == 2) {
        snprintf(buf, 32, "%s.a", code == 1 ? t0 : t1);
        return buf;
    }
    return mul ? c[code] : abd[code];
}

/* One combiner cycle: (A - B) * C + D for colour and alpha. cycle selects the mux fields. */
static void put_cycle(Str *s, u32 mux0, u32 mux1, int cycle, const char *t0, const char *t1) {
    char b[8][32];
    int a, bb, c, d, aa, ab, ac, ad;

    if (cycle == 0) {
        a = (mux0 >> 20) & 0xF;
        c = (mux0 >> 15) & 0x1F;
        aa = (mux0 >> 12) & 7;
        ac = (mux0 >> 9) & 7;
        bb = (mux1 >> 28) & 0xF;
        d = (mux1 >> 15) & 7;
        ab = (mux1 >> 12) & 7;
        ad = (mux1 >> 9) & 7;
    } else {
        a = (mux0 >> 5) & 0xF;
        c = mux0 & 0x1F;
        aa = (mux1 >> 21) & 7;
        ac = (mux1 >> 18) & 7;
        bb = (mux1 >> 24) & 0xF;
        d = (mux1 >> 6) & 7;
        ab = (mux1 >> 3) & 7;
        ad = mux1 & 7;
    }
    put(s, "    c = vec4((%s.rgb - %s.rgb) * %s.rgb + %s.rgb, (%s - %s) * %s + %s);\n",
        rgb_input(b[0], 0, a, t0, t1), rgb_input(b[1], 1, bb, t0, t1), rgb_input(b[2], 2, c, t0, t1),
        rgb_input(b[3], 3, d, t0, t1), alpha_input(b[4], 0, aa, t0, t1), alpha_input(b[5], 0, ab, t0, t1),
        alpha_input(b[6], 1, ac, t0, t1), alpha_input(b[7], 0, ad, t0, t1));
}

/* Which texels a combiner cycle reads: bit 0 TEXEL0, bit 1 TEXEL1. */
static int cycle_texels(u32 mux0, u32 mux1, int cycle) {
    int codes[8];
    int i;
    int used = 0;

    if (cycle == 0) {
        codes[0] = (mux0 >> 20) & 0xF, codes[1] = (mux1 >> 28) & 0xF, codes[2] = (mux0 >> 15) & 0x1F;
        codes[3] = (mux1 >> 15) & 7, codes[4] = (mux0 >> 12) & 7, codes[5] = (mux1 >> 12) & 7;
        codes[6] = (mux0 >> 9) & 7, codes[7] = (mux1 >> 9) & 7;
    } else {
        codes[0] = (mux0 >> 5) & 0xF, codes[1] = (mux1 >> 24) & 0xF, codes[2] = mux0 & 0x1F;
        codes[3] = (mux1 >> 6) & 7, codes[4] = (mux1 >> 21) & 7, codes[5] = (mux1 >> 3) & 7;
        codes[6] = (mux1 >> 18) & 7, codes[7] = mux1 & 7;
    }
    for (i = 0; i < 8; i++) {
        if (codes[i] == 1 || (i == 2 && codes[i] == 8)) {
            used |= 1;
        }
        if (codes[i] == 2 || (i == 2 && codes[i] == 9)) {
            used |= 2;
        }
    }
    return used;
}

static const char *blend_color(int input) {
    static const char *const names[4] = {"inColor", "inColor", "uBlend.rgb", "uFog.rgb"};
    return names[input];
}

static const char *blend_alpha(int input) {
    static const char *const names[4] = {"inAlpha", "uFog.a", "shade.a", "0.0"};
    return names[input];
}

static const char *blend_weight(int input) {
    /* The memory's alpha is the pixel's coverage, full here. */
    static const char *const names[4] = {"1.0 - a", "1.0", "1.0", "0.0"};
    return names[input];
}

static void make_fragment_shader(Str *s, const ProgramKey *k) {
    const char *st = k->persp ? "vTexP" : "(vTexAW / vW)";
    int p;
    int a;
    int m;
    int b;

    s->len = 0;
    put(s, "%s", FRAGMENT_HEAD);
    put(s, "    vec4 shade = vShadeW / vW;\n");
    put(s, "    float noise = fract(sin(dot(gl_FragCoord.xy + uMisc.w, vec2(12.9898, 78.233))) * 43758.5453);\n");
    if (k->textures) {
        /* Move the coordinates to the pixel's upper left corner: uMisc.y is the pixel size in N64 pixels, or 0
           (see build_state). */
        put(s, "    vec2 st = %s;\n", st);
        put(s, "    st -= 0.5 * uMisc.y * (dFdx(st) - dFdy(st));\n");
    }
    put(s, "    vec4 t0 = %s;\n", (k->textures & 1) ? "sampleTile(uTex0, 0, st)" : "vec4(0.0)");
    put(s, "    vec4 t1 = %s;\n", (k->textures & 2) ? "sampleTile(uTex1, 1, st)" : "vec4(0.0)");
    put(s, "    vec4 c = vec4(0.0);\n");
    switch (k->mode) {
        case MODE_COPY:
            put(s, "    c = t0;\n");
            break;
        case MODE_FILL:
            put(s, "    c = vec4(shade.rgb, 1.0);\n");
            break;
        case MODE_1CYCLE:
            /* One-cycle mode uses the second cycle's inputs. TEXEL1 is texel 0 again. */
            put_cycle(s, k->mux0, k->mux1, 1, "t0", "t0");
            break;
        default:
            /* In the second cycle TEXEL0 is the second tile's texel, and TEXEL1 the next pixel's first texel. */
            put_cycle(s, k->mux0, k->mux1, 0, "t0", "t1");
            put(s, "    c = clamp(c, 0.0, 1.0);\n");
            put(s, "    float acAlpha = c.a;\n");
            put_cycle(s, k->mux0, k->mux1, 1, "t1", "t0");
            break;
    }
    put(s, "    c = clamp(c, 0.0, 1.0);\n");
    if (k->mode != MODE_2CYCLE) {
        put(s, "    float acAlpha = c.a;\n");
    }

    /* The alpha compare (two-cycle mode compares the first cycle's alpha) and the coverage test. */
    switch (k->alphaTest) {
        case ALPHA_THRESHOLD:
            put(s, "    if (floor(acAlpha * 255.0 + 0.5) < floor(uBlend.a * 255.0 + 0.5)) discard;\n");
            break;
        case ALPHA_DITHER:
            put(s, "    if (acAlpha < noise) discard;\n");
            break;
        case ALPHA_COPY:
            put(s, "    if (acAlpha < max(uBlend.a, 1.0 / 255.0)) discard;\n");
            break;
        default:
            break;
    }
    if (k->coverage != COVERAGE_NONE) {
        put(s, "    if (c.a < 0.125) discard;\n");
    }

    /* The blender. */
    put(s, "    vec3 inColor = c.rgb;\n");
    put(s, "    float inAlpha = c.a;\n");
    if (k->mode == MODE_2CYCLE) {
        p = k->blend1 >> 6, a = (k->blend1 >> 4) & 3, m = (k->blend1 >> 2) & 3, b = k->blend1 & 3;
        put(s, "    {\n");
        put(s, "        float a = %s;\n", blend_alpha(a));
        put(s, "        inColor = %s * a + %s * (%s);\n", blend_color(p), blend_color(m), blend_weight(b));
        put(s, "    }\n");
    }
    p = k->blend2 >> 6, a = (k->blend2 >> 4) & 3, m = (k->blend2 >> 2) & 3, b = k->blend2 & 3;
    if (k->mode == MODE_COPY || k->mode == MODE_FILL) {
        put(s, "    fragColor = vec4(inColor, inAlpha);\n");
    } else if (!k->forceBlend) {
        /* Without blending the last cycle passes its first colour. */
        put(s, "    fragColor = vec4(%s, inAlpha);\n", blend_color(p));
    } else {
        /* The blender weighs with the top 5 bits of alpha: n / 32 for an alpha of 8n to 8n + 7. An alpha below 8
           adds nothing. */
        put(s, "    float a = floor(floor(%s * 255.0 + 0.5) / 8.0) / 32.0;\n", blend_alpha(a));
        put(s, "    float b = %s;\n", blend_weight(b));
        if (p != BL_MEM && m != BL_MEM) {
            put(s, "    fragColor = vec4(%s * a + %s * b, inAlpha);\n", blend_color(p), blend_color(m));
        } else if (p != BL_MEM) {
            put(s, "    fragColor = vec4(%s * a, a);\n", blend_color(p));
        } else if (m != BL_MEM) {
            put(s, "    fragColor = vec4(%s * b, a);\n", blend_color(m));
        } else {
            put(s, "    fragColor = vec4(0.0, 0.0, 0.0, a + b);\n");
        }
    }
    if (k->layer != LAYER_NONE) {
        int memRead = k->mode != MODE_COPY && k->mode != MODE_FILL && k->forceBlend && (p == BL_MEM || m == BL_MEM);
        int dst = DST_ZERO;

        /* The destination factor of build_state. */
        if (memRead) {
            dst = p == BL_MEM ? DST_SRC_ALPHA : b == 0 ? DST_ONE_MINUS_SRC_ALPHA : b == 3 ? DST_ZERO : DST_ONE;
        }

        if (dst == DST_ZERO) {
            put(s, "    fragColor.a = 1.0;\n");
        } else if (k->layer == LAYER_DEPTH && dst == DST_ONE_MINUS_SRC_ALPHA) {
            put(s, "    if (fragColor.a < 1.0 / 255.0) discard;\n");
        } else if (k->layer == LAYER_DEPTH) {
            put(s, "    if (max(max(fragColor.r, fragColor.g), fragColor.b) < 1.0 / 255.0) discard;\n");
        }
    }
    put(s, "}\n");
}

static GLuint compile(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        gfx_log("gfx: shader compile error: %s\n%s", log, src);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static Program *get_program(const ProgramKey *key) {
    static Str src;
    Program *p;
    GLuint fs;
    GLint ok = 0;
    int i;

    for (i = 0; i < R.programCount; i++) {
        if (memcmp(&R.programs[i].key, key, sizeof(*key)) == 0) {
            return R.programs[i].id ? &R.programs[i] : NULL;
        }
    }
    if (R.programCount == MAX_PROGRAMS) {
        return NULL;
    }
    p = &R.programs[R.programCount++];
    memset(p, 0, sizeof(*p));
    p->key = *key;
    make_fragment_shader(&src, key);
    fs = compile(GL_FRAGMENT_SHADER, src.buf);
    if (fs == 0) {
        return NULL;
    }
    p->id = glCreateProgram();
    glAttachShader(p->id, R.vs);
    glAttachShader(p->id, fs);
    glLinkProgram(p->id);
    glDeleteShader(fs);
    glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p->id, sizeof(log), NULL, log);
        gfx_log("gfx: shader link error: %s", log);
        glDeleteProgram(p->id);
        p->id = 0;
        return NULL;
    }
    glUseProgram(p->id);
    glUniform1i(glGetUniformLocation(p->id, "uTex0"), 0);
    glUniform1i(glGetUniformLocation(p->id, "uTex1"), 1);
    p->locTexA = glGetUniformLocation(p->id, "uTexA");
    p->locTexB = glGetUniformLocation(p->id, "uTexB");
    p->locTexC = glGetUniformLocation(p->id, "uTexC");
    p->locPrim = glGetUniformLocation(p->id, "uPrim");
    p->locEnv = glGetUniformLocation(p->id, "uEnv");
    p->locFog = glGetUniformLocation(p->id, "uFog");
    p->locBlend = glGetUniformLocation(p->id, "uBlend");
    p->locMisc = glGetUniformLocation(p->id, "uMisc");
    R.gl.prog = NULL;
    gfxStats.programs++;
    return p;
}

/* ---- Draw states. */

static u8 blender_cycle(u32 l, int cycle) {
    if (cycle == 0) {
        return (u8)((((l >> 30) & 3) << 6) | (((l >> 26) & 3) << 4) | (((l >> 22) & 3) << 2) | ((l >> 18) & 3));
    }
    return (u8)((((l >> 28) & 3) << 6) | (((l >> 24) & 3) << 4) | (((l >> 20) & 3) << 2) | ((l >> 16) & 3));
}

/* Builds the draw state for a primitive from the RDP's state. Returns 0 when it cannot be drawn. */
static int build_state(DrawState *ds, int kind, int tile, const GfxTexDesc *direct, const GfxSampler *directSamp,
                       int wide) {
    const GfxRdp *rdp = &gfxRdp;
    u32 h = rdp->otherH;
    u32 l = rdp->otherL;
    int cycle = CYCLE_TYPE(h);
    ProgramKey key;
    int texels = 0;
    int firstTile = kind == GFX_PRIM_TRIANGLE ? rdp->texTile : tile;
    int i;

    memset(ds, 0, sizeof(*ds));
    memset(&key, 0, sizeof(key));

    if (kind == GFX_PRIM_FILL || cycle == CYC_FILL) {
        key.mode = MODE_FILL;
    } else if (cycle == CYC_COPY) {
        key.mode = MODE_COPY;
        texels = kind == GFX_PRIM_TEXRECT ? 1 : 0;
        key.alphaTest = RM_ALPHA_COMPARE(l) ? ALPHA_COPY : ALPHA_NONE;
    } else {
        key.mode = cycle == CYC_2 ? MODE_2CYCLE : MODE_1CYCLE;
        key.mux0 = rdp->mux0;
        key.mux1 = rdp->mux1;
        if (key.mode == MODE_1CYCLE) {
            texels = cycle_texels(rdp->mux0, rdp->mux1, 1) ? 1 : 0;
        } else {
            int c0 = cycle_texels(rdp->mux0, rdp->mux1, 0);
            int c1 = cycle_texels(rdp->mux0, rdp->mux1, 1);
            /* The second cycle's TEXEL0 is the second tile's texel; its TEXEL1 is the first tile's. */
            texels = (c0 & 1) | ((c1 & 2) ? 1 : 0) | (c0 & 2) | ((c1 & 1) ? 2 : 0);
        }
        if (kind == GFX_PRIM_RECT || (kind == GFX_PRIM_TRIANGLE && !rdp->texOn)) {
            texels = 0;
        }
        switch (RM_ALPHA_COMPARE(l)) {
            case 1:
                key.alphaTest = ALPHA_THRESHOLD;
                break;
            case 3:
                key.alphaTest = ALPHA_DITHER;
                break;
            default:
                break;
        }
        key.blend1 = blender_cycle(l, 0);
        key.blend2 = blender_cycle(l, key.mode == MODE_2CYCLE ? 1 : 0);
        key.forceBlend = (l & RM_FORCE_BL) != 0;
        if ((l & RM_CVG_X_ALPHA) && (l & RM_ALPHA_CVG_SEL)) {
            if (key.forceBlend && ((key.blend2 >> 6) == BL_MEM || ((key.blend2 >> 2) & 3) == BL_MEM)) {
                key.coverage = COVERAGE_BLENDED;
            } else {
                key.coverage = COVERAGE_EDGE;
            }
        }
        key.persp = kind == GFX_PRIM_TRIANGLE && TEX_PERSP(h);
    }
    key.layer = gfxLayer.on ? LAYER_COLOR : LAYER_NONE;

    /* Textures. A tile with nothing loaded samples white. */
    for (i = 0; i < 2; i++) {
        GfxTexDesc desc;
        GfxSampler *samp = &ds->u.samp[i];
        int found;

        if (!(texels & (1 << i))) {
            continue;
        }
        if (direct != NULL && i == 0) {
            desc = *direct;
            *samp = *directSamp;
            found = 1;
        } else {
            found = gfx_tex_describe_tile((firstTile + i) & 7, &desc, samp);
        }
        if (found) {
            ds->tex[i] = gfx_tex_get(&desc);
        }
        if (!found || ds->tex[i] == 0) {
            ds->tex[i] = R.white;
            gfx_tex_clamp_sampler(samp, 1, 1);
        }
    }
    key.textures = (u8)texels;
    if (texels == 0) {
        key.persp = 0;
    }

    ds->prog = get_program(&key);
    if (ds->prog == NULL) {
        return 0;
    }
    memcpy(ds->u.prim, rdp->prim, sizeof(rdp->prim));
    memcpy(ds->u.env, rdp->env, sizeof(rdp->env));
    memcpy(ds->u.fog, rdp->fog, sizeof(rdp->fog));
    memcpy(ds->u.blend, rdp->blend, sizeof(rdp->blend));
    ds->u.misc[0] = rdp->primLod;
    ds->u.misc[2] = (key.mode == MODE_COPY || TEX_FILTER(h) == 0) ? 0.0f : (float)R.filter;
    /* Bilinear filtering takes a pixel's texture coordinates at its upper left corner, as the RDP does. Point
       sampling takes them at the pixel's centre: at a higher resolution, texel edges then stay on the N64's
       pixel edges. */
    ds->u.misc[1] = ds->u.misc[2] != 0.0f ? R.height / (float)SCREEN_H : 0.0f;
    ds->u.misc[3] = R.seed;

    /* Depth. Triangles use the depth buffer when the geometry mode asks for it. Rectangles have no depth of their
       own: they use the primitive depth (G_ZS_PRIM), as the game's 3D sprites do. Copy and fill mode do not. */
    if (key.mode != MODE_COPY && key.mode != MODE_FILL) {
        int zbuf = kind == GFX_PRIM_TRIANGLE ? (rdp->geometryMode & GM_ZBUFFER) != 0 : (l & RM_ZS_PRIM) != 0;
        if (zbuf && (l & (RM_Z_CMP | RM_Z_UPD))) {
            ds->depthTest = 1;
            ds->depthCompare = (l & RM_Z_CMP) != 0;
            ds->depthWrite = (l & RM_Z_UPD) != 0;
        }
        ds->decal = RM_ZMODE(l) == RM_ZMODE_DEC;
    }

    /* Blending with the frame buffer: the last cycle reads memory colour. */
    if (key.mode != MODE_COPY && key.mode != MODE_FILL && key.forceBlend) {
        int p = key.blend2 >> 6;
        int m = (key.blend2 >> 2) & 3;
        int b = key.blend2 & 3;
        if (p == BL_MEM || m == BL_MEM) {
            ds->blend = 1;
            if (p == BL_MEM) {
                ds->dstFactor = DST_SRC_ALPHA;
            } else {
                ds->dstFactor = b == 0 ? DST_ONE_MINUS_SRC_ALPHA : b == 3 ? DST_ZERO : DST_ONE;
            }
        }
    }
    memcpy(ds->scissor, rdp->scissor, sizeof(ds->scissor));
    ds->wide = (u8)wide;
    return 1;
}

/* ---- Recording. */

void gfx_state_changed(void) {
    R.dirty = 1;
}

static int push_cmd(int type) {
    Cmd *c;

    if (!grow((void **)&R.cmds, &R.cmdCap, R.cmdCount + 1, sizeof(Cmd))) {
        return -1;
    }
    c = &R.cmds[R.cmdCount];
    memset(c, 0, sizeof(*c));
    c->type = type;
    c->state = R.stateCount - 1;
    c->first = R.vertCount;
    return R.cmdCount++;
}

int gfx_prepare(int kind, int tile, const GfxTexDesc *direct, const GfxSampler *directSamp, int wide) {
    DrawState ds;

    /* A layer leaves out what is not in it. */
    if (gfxLayer.on && !(gfxLayer.kind & gfxLayer.mask)) {
        return 0;
    }
    if (!R.dirty && direct == NULL && kind == R.lastKind && tile == R.lastTile && wide == R.lastWide
        && R.stateCount > 0) {
        if (R.openDraw < 0) {
            R.openDraw = push_cmd(CMD_DRAW);
        }
        return R.openDraw >= 0;
    }
    if (!build_state(&ds, kind, tile, direct, directSamp, wide)) {
        return 0;
    }
    R.dirty = direct != NULL; /* a Sprite2D draw's texture is not in the RDP's state */
    R.lastKind = kind;
    R.lastTile = tile;
    R.lastWide = wide;
    if (R.stateCount > 0 && memcmp(&R.states[R.stateCount - 1], &ds, sizeof(ds)) == 0) {
        if (R.openDraw < 0) {
            R.openDraw = push_cmd(CMD_DRAW);
        }
        return R.openDraw >= 0;
    }
    if (!grow((void **)&R.states, &R.stateCap, R.stateCount + 1, sizeof(DrawState))) {
        return 0;
    }
    R.states[R.stateCount++] = ds;
    R.openDraw = push_cmd(CMD_DRAW);
    return R.openDraw >= 0;
}

void gfx_add_vertices(const GfxVertex *v, int count) {
    if (R.openDraw < 0 || !grow((void **)&R.verts, &R.vertCap, R.vertCount + count, sizeof(GfxVertex))) {
        return;
    }
    memcpy(R.verts + R.vertCount, v, count * sizeof(GfxVertex));
    R.vertCount += count;
    R.cmds[R.openDraw].count += count;
    if (count == 6) {
        gfxStats.rects++;
    } else {
        gfxStats.triangles += count / 3;
    }
}

void gfx_clear_color(const float rgba[4]) {
    static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    /* The setup's clear is the background of the sky. */
    unsigned int kind = gfxLayer.kind ? gfxLayer.kind : GFX_LAYER_SKY;
    int left = gfxLayer.on && !(kind & gfxLayer.mask);
    int i;

    /* A full-screen fill of a part that the layer leaves out is left out too. */
    if (left && gfxLayer.kind != 0) {
        R.openDraw = -1;
        return;
    }
    i = push_cmd(CMD_CLEAR_COLOR);
    if (i >= 0) {
        /* A layer is transparent where its own parts draw nothing. */
        memcpy(R.cmds[i].color, left ? clear : rgba, 4 * sizeof(float));
    }
    R.openDraw = -1;
}

void gfx_clear_depth(void) {
    push_cmd(CMD_CLEAR_DEPTH);
    R.openDraw = -1;
}

void gfx_render_set_filter(int filter) {
    R.filter = filter == GFX_FILTER_NEAREST ? 0 : filter == GFX_FILTER_BILINEAR ? 2 : 1;
}

void gfx_render_begin(int width, int height, float wide) {
    static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    R.width = width;
    R.height = height;
    R.wide = wide < 1.0f ? 1.0f : wide;
    R.vertCount = 0;
    R.stateCount = 0;
    R.cmdCount = 0;
    R.openDraw = -1;
    R.dirty = 1;
    R.lastKind = -1;
    R.lastWide = 0;
    R.seed = (float)(((int)R.seed + 1) & 1023);
    memset(&gfxStats, 0, sizeof(gfxStats));
    gfx_clear_color(black);
    gfx_clear_depth();
}

/* ---- Playing the frame. */

static void set_uniforms(Program *p, const Uniforms *u) {
    GLfloat a[8];
    GLfloat b[8];
    GLint c[8];
    int i;

    if (p->currentValid && memcmp(&p->current, u, sizeof(*u)) == 0) {
        return;
    }
    for (i = 0; i < 2; i++) {
        memcpy(a + i * 4, u->samp[i].a, 4 * sizeof(float));
        memcpy(b + i * 4, u->samp[i].b, 4 * sizeof(float));
        memcpy(c + i * 4, u->samp[i].c, 4 * sizeof(int));
    }
    glUniform4fv(p->locTexA, 2, a);
    glUniform4fv(p->locTexB, 2, b);
    glUniform4iv(p->locTexC, 2, c);
    glUniform4fv(p->locPrim, 1, u->prim);
    glUniform4fv(p->locEnv, 1, u->env);
    glUniform4fv(p->locFog, 1, u->fog);
    glUniform4fv(p->locBlend, 1, u->blend);
    glUniform4fv(p->locMisc, 1, u->misc);
    p->current = *u;
    p->currentValid = 1;
}

static void enable(GLenum cap, int on) {
    if (on) {
        glEnable(cap);
    } else {
        glDisable(cap);
    }
}

/* Sets the OpenGL state for a draw, changing only what differs from the last draw. */
static void apply(const DrawState *ds) {
    static const GLenum factors[4] = {GL_ZERO, GL_ONE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA};
    /* The N64's screen is the middle of the frame when the view is widened. */
    float areaW = R.width / R.wide;
    float areaX = (R.width - areaW) / 2.0f;
    float sx = areaW / (float)SCREEN_W;
    float sy = R.height / (float)SCREEN_H;
    GLint sc[4];
    int i;

    if (R.gl.prog != ds->prog) {
        glUseProgram(ds->prog->id);
        R.gl.prog = ds->prog;
    }
    set_uniforms(ds->prog, &ds->u);
    for (i = 0; i < 2; i++) {
        if (ds->tex[i] && R.gl.tex[i] != ds->tex[i]) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, ds->tex[i]);
            R.gl.tex[i] = ds->tex[i];
        }
    }
    if (R.gl.depthTest != ds->depthTest) {
        enable(GL_DEPTH_TEST, ds->depthTest);
        R.gl.depthTest = ds->depthTest;
    }
    if (R.gl.depthCompare != ds->depthCompare) {
        glDepthFunc(ds->depthCompare ? GL_LEQUAL : GL_ALWAYS);
        R.gl.depthCompare = ds->depthCompare;
    }
    if (R.gl.depthWrite != ds->depthWrite) {
        glDepthMask(ds->depthWrite ? GL_TRUE : GL_FALSE);
        R.gl.depthWrite = ds->depthWrite;
    }
    if (R.gl.decal != ds->decal) {
        enable(GL_POLYGON_OFFSET_FILL, ds->decal);
        R.gl.decal = ds->decal;
    }
    if (R.gl.blend != ds->blend) {
        enable(GL_BLEND, ds->blend);
        R.gl.blend = ds->blend;
    }
    if (ds->blend && R.gl.dstFactor != ds->dstFactor) {
        if (ds->prog->key.layer != LAYER_NONE) {
            /* Alpha: the blend weight over by 1 - a; a pixel that adds or keeps memory adds no coverage. */
            static const GLenum alphaSrc[4] = {GL_ONE, GL_ZERO, GL_ZERO, GL_ONE};
            static const GLenum alphaDst[4] = {GL_ZERO, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA};

            glBlendFuncSeparate(GL_ONE, factors[ds->dstFactor], alphaSrc[ds->dstFactor], alphaDst[ds->dstFactor]);
        } else {
            glBlendFunc(GL_ONE, factors[ds->dstFactor]);
        }
        R.gl.dstFactor = ds->dstFactor;
    }
    /* The scissor, from N64 pixels (10.2) to the frame's pixels; OpenGL counts rows from the bottom. */
    sc[0] = (GLint)(areaX + ds->scissor[0] / 4.0f * sx + 0.5f);
    sc[2] = (GLint)(areaX + ds->scissor[2] / 4.0f * sx + 0.5f);
    if (ds->wide && ds->scissor[0] == 0 && ds->scissor[2] >= SCREEN_W * 4) {
        /* A scissor of the whole screen width covers the whole widened frame. */
        sc[0] = 0;
        sc[2] = R.width;
    }
    sc[1] = (GLint)(ds->scissor[1] / 4.0f * sy + 0.5f);
    sc[3] = (GLint)(ds->scissor[3] / 4.0f * sy + 0.5f);
    sc[2] = sc[2] > sc[0] ? sc[2] - sc[0] : 0;
    sc[3] = sc[3] > sc[1] ? sc[3] - sc[1] : 0;
    sc[1] = R.height - sc[1] - sc[3];
    if (!R.gl.scissorOn) {
        glEnable(GL_SCISSOR_TEST);
        R.gl.scissorOn = 1;
    }
    if (memcmp(sc, R.gl.scissor, sizeof(sc)) != 0) {
        glScissor(sc[0], sc[1], sc[2], sc[3]);
        memcpy(R.gl.scissor, sc, sizeof(sc));
    }
}

/* Prints a draw's state, for comparing with the browser port's renderer (screenshot.py --dump-draws). */
static void trace_draw(const Cmd *c) {
    const DrawState *ds = &R.states[c->state];
    const ProgramKey *k = &ds->prog->key;
    const GfxVertex *v = &R.verts[c->first];

    printf("cmd %d draw %d: mode %d mux %06x %08x tex %d persp %d alpha %d cvg %d blend %02x %02x force %d | "
           "depth %d%d%d decal %d glblend %d dst %d | prim %d %d %d %d env %d %d %d %d fog %d %d %d %d | "
           "first %.2f %.2f %.2f %.2f st %.2f %.2f shade %.2f %.2f %.2f %.2f\n",
           (int)(c - R.cmds), c->count, k->mode, k->mux0, k->mux1, k->textures, k->persp, k->alphaTest, k->coverage,
           k->blend1, k->blend2, k->forceBlend, ds->depthTest, ds->depthCompare, ds->depthWrite, ds->decal, ds->blend,
           ds->dstFactor, (int)(ds->u.prim[0] * 255 + 0.5f), (int)(ds->u.prim[1] * 255 + 0.5f),
           (int)(ds->u.prim[2] * 255 + 0.5f), (int)(ds->u.prim[3] * 255 + 0.5f), (int)(ds->u.env[0] * 255 + 0.5f),
           (int)(ds->u.env[1] * 255 + 0.5f), (int)(ds->u.env[2] * 255 + 0.5f), (int)(ds->u.env[3] * 255 + 0.5f),
           (int)(ds->u.fog[0] * 255 + 0.5f), (int)(ds->u.fog[1] * 255 + 0.5f), (int)(ds->u.fog[2] * 255 + 0.5f),
           (int)(ds->u.fog[3] * 255 + 0.5f), v->x, v->y, v->z, v->w, v->s, v->t, v->r, v->g, v->b, v->a);
    if (k->textures & 1) {
        const GfxSampler *sm = &ds->u.samp[0];
        printf("    tex %u: shift %g %g origin %g %g edge %g %g last %g %g period %d %d flags %d %d\n", ds->tex[0],
               sm->a[0], sm->a[1], sm->a[2], sm->a[3], sm->b[0], sm->b[1], sm->b[2], sm->b[3], sm->c[0], sm->c[1],
               sm->c[2], sm->c[3]);
    }
}

/* Puts every piece of OpenGL state that apply() tracks into a known state. */
static void reset_gl_state(void) {
    int i;

    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_FALSE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -2.0f);
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    memset(&R.gl, 0, sizeof(R.gl));
    R.gl.dstFactor = -1;
    R.gl.scissor[0] = -1;
    for (i = 0; i < R.programCount; i++) {
        R.programs[i].currentValid = 0;
    }
}

void gfx_render_end(void) {
    int i;

    reset_gl_state();
    glViewport(0, 0, R.width, R.height);
    glBindVertexArray(R.vao);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)R.vertCount * sizeof(GfxVertex), R.verts, GL_STREAM_DRAW);
    for (i = 0; i < R.cmdCount; i++) {
        const Cmd *c = &R.cmds[i];
        switch (c->type) {
            case CMD_CLEAR_COLOR:
                if (R.gl.scissorOn) {
                    glDisable(GL_SCISSOR_TEST);
                    R.gl.scissorOn = 0;
                }
                glClearColor(c->color[0], c->color[1], c->color[2], c->color[3]);
                glClear(GL_COLOR_BUFFER_BIT);
                break;
            case CMD_CLEAR_DEPTH:
                if (R.gl.scissorOn) {
                    glDisable(GL_SCISSOR_TEST);
                    R.gl.scissorOn = 0;
                }
                if (!R.gl.depthWrite) {
                    glDepthMask(GL_TRUE);
                    R.gl.depthWrite = 1;
                }
                glClearDepth(1.0);
                glClear(GL_DEPTH_BUFFER_BIT);
                break;
            default:
                if (c->count > 0) {
                    if (R.trace) {
                        trace_draw(c);
                    }
                    apply(&R.states[c->state]);
                    glDrawArrays(GL_TRIANGLES, c->first, c->count);
                    gfxStats.draws++;
                }
                break;
        }
    }
    gfxStats.states = R.stateCount;
    reset_gl_state();
    glBindVertexArray(0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
}

void gfx_render_depth_pass(void) {
    int i;

    reset_gl_state();
    glViewport(0, 0, R.width, R.height);
    glBindVertexArray(R.vao);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    for (i = 0; i < R.cmdCount; i++) {
        const Cmd *c = &R.cmds[i];
        DrawState ds;
        ProgramKey key;

        /* The depth clears of the frame are left out: the layer keeps the nearest pixel of all. */
        if (c->type != CMD_DRAW || c->count == 0) {
            continue;
        }
        ds = R.states[c->state];
        key = ds.prog->key;
        key.layer = LAYER_DEPTH;
        ds.prog = get_program(&key);
        if (ds.prog == NULL) {
            continue;
        }
        ds.depthTest = 1;
        ds.depthCompare = 1;
        ds.depthWrite = 1;
        ds.blend = 0;
        apply(&ds);
        glDepthFunc(GL_LESS);
        glDrawArrays(GL_TRIANGLES, c->first, c->count);
    }
    reset_gl_state();
    glBindVertexArray(0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
}

/* A full-screen triangle that draws a layer over a framebuffer: its colour premultiplied, and its depth pulled
   towards the eye by a share of the view depth (gfx_composite_layer). */
static const char COMPOSITE_VS[] =
    "#version 330 core\n"
    "void main() {\n"
    "    gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0, 0.0, 1.0);\n"
    "}\n";

static const char COMPOSITE_FS[] =
    "#version 330 core\n"
    "uniform sampler2D uColor;\n"
    "uniform sampler2D uDepth;\n"
    "uniform vec4 uSize;\n"   /* the target's width and height, the textures' width and height */
    "uniform vec4 uPlanes;\n" /* the target's near and far, the bias, the least alpha that is drawn */
    "uniform vec4 uProjZ;\n"  /* the layer's projection terms m[2][2] and m[3][2] */
    "layout(location = 0) out vec4 fragColor;\n"
    "void main() {\n"
    "    ivec2 t = ivec2(gl_FragCoord.xy * uSize.zw / uSize.xy);\n"
    "    vec4 c = texelFetch(uColor, t, 0);\n"
    "    float d = texelFetch(uDepth, t, 0).r;\n"
    "    if (uPlanes.w > 0.0 ? c.a < uPlanes.w : c.a <= 0.0 && max(max(c.r, c.g), c.b) <= 0.0) discard;\n"
    "    if (d > 0.0 && d < 1.0) {\n"
    "        float n = uPlanes.x;\n"
    "        float f = uPlanes.y;\n"
    "        float e = uProjZ.y / (2.0 * d - 1.0 + uProjZ.x);\n"
    "        e = max(e * (1.0 - uPlanes.z), n);\n"
    "        d = ((f + n) - 2.0 * n * f / e) / (f - n) * 0.5 + 0.5;\n"
    "    }\n"
    "    gl_FragDepth = d;\n"
    "    fragColor = c;\n"
    "}\n";

int gfx_render_composite(unsigned int color, unsigned int depth, int texW, int texH, int w, int h,
                         const float planes[2], const float projZ[2], float bias, int writeDepth) {
    float size[4];
    float u[4];
    float pz[4];

    if (R.composite == 0) {
        GLuint vs = compile(GL_VERTEX_SHADER, COMPOSITE_VS);
        GLuint fs = compile(GL_FRAGMENT_SHADER, COMPOSITE_FS);
        GLint ok = 0;

        if (vs == 0 || fs == 0) {
            return 0;
        }
        R.composite = glCreateProgram();
        glAttachShader(R.composite, vs);
        glAttachShader(R.composite, fs);
        glLinkProgram(R.composite);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glGetProgramiv(R.composite, GL_LINK_STATUS, &ok);
        if (!ok) {
            glDeleteProgram(R.composite);
            R.composite = 0;
            return 0;
        }
        glUseProgram(R.composite);
        glUniform1i(glGetUniformLocation(R.composite, "uColor"), 0);
        glUniform1i(glGetUniformLocation(R.composite, "uDepth"), 1);
        glGenVertexArrays(1, &R.compositeVao);
    }
    size[0] = (float)w;
    size[1] = (float)h;
    size[2] = (float)texW;
    size[3] = (float)texH;
    u[0] = planes[0];
    u[1] = planes[1];
    u[2] = bias;
    u[3] = 0.0f;
    glUseProgram(R.composite);
    glUniform4fv(glGetUniformLocation(R.composite, "uSize"), 1, size);
    glUniform4fv(glGetUniformLocation(R.composite, "uPlanes"), 1, u);
    pz[0] = projZ[0];
    pz[1] = projZ[1];
    pz[2] = 0.0f;
    pz[3] = 0.0f;
    glUniform4fv(glGetUniformLocation(R.composite, "uProjZ"), 1, pz);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, color);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, depth);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(R.compositeVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (writeDepth) {
        /* The depth of the pixels that are at least half covered, for the host's later passes. */
        u[3] = 0.5f;
        glUniform4fv(glGetUniformLocation(R.composite, "uPlanes"), 1, u);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_TRUE);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glBindVertexArray(0);
    glUseProgram(0);
    return 1;
}

/* ---- Set-up. */

int gfx_render_init(void) {
    static const u8 white[4] = {255, 255, 255, 255};

    memset(&R, 0, sizeof(R));
    R.filter = 1;
    R.trace = gfx_getenv("GLOVER_GFX_TRACE") != NULL;
    R.vs = compile(GL_VERTEX_SHADER, VERTEX_SHADER);
    if (R.vs == 0) {
        return -1;
    }
    glGenVertexArrays(1, &R.vao);
    glGenBuffers(1, &R.vbo);
    glBindVertexArray(R.vao);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GfxVertex), (const void *)offsetof(GfxVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GfxVertex), (const void *)offsetof(GfxVertex, r));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GfxVertex), (const void *)offsetof(GfxVertex, s));
    glBindVertexArray(0);

    glGenTextures(1, &R.white);
    glBindTexture(GL_TEXTURE_2D, R.white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glBindTexture(GL_TEXTURE_2D, 0);
    return 0;
}

void gfx_render_set_msaa(int on) {
    R.msaa = on;
}

void gfx_render_shutdown(void) {
    int i;

    for (i = 0; i < R.programCount; i++) {
        if (R.programs[i].id) {
            glDeleteProgram(R.programs[i].id);
        }
    }
    if (R.vs) {
        glDeleteShader(R.vs);
    }
    if (R.vbo) {
        glDeleteBuffers(1, &R.vbo);
    }
    if (R.vao) {
        glDeleteVertexArrays(1, &R.vao);
    }
    if (R.white) {
        glDeleteTextures(1, &R.white);
    }
    if (R.composite) {
        glDeleteProgram(R.composite);
    }
    if (R.compositeVao) {
        glDeleteVertexArrays(1, &R.compositeVao);
    }
    free(R.verts);
    free(R.states);
    free(R.cmds);
    memset(&R, 0, sizeof(R));
}
