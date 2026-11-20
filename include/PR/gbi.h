/* Written for weemitt: the parts of the N64 system interface that the port's code uses. */
#ifndef WEEMITT_PR_GBI_H
#define WEEMITT_PR_GBI_H

/* The display list format of the F3DEX microcode: the data it reads, and macros that write its commands.
   mbi.h includes this file and defines _SHIFTL. ptr32.h defines PTR32 and PORT_GBI_ADDR. */

#include <PR/ultratypes.h>

/* Data that a display list points to. Each type is a union with a 64-bit member, which gives it 8-byte alignment
   on the N64. The members that are 4 bytes on the N64 have 4-byte types here on every host. */

typedef struct {
    short ob[3]; /* position */
    unsigned short flag;
    short tc[2]; /* texture coordinates */
    unsigned char cn[4]; /* color and alpha */
} Vtx_t;

/* A vertex with a normal in place of the color. */
typedef struct {
    short ob[3];
    unsigned short flag;
    short tc[2];
    signed char n[3];
    unsigned char a;
} Vtx_tn;

typedef union {
    Vtx_t v;
    Vtx_tn n;
    long long int force_structure_alignment;
} Vtx;

/* A matrix of 16.16 fixed-point numbers: the 16 integer parts, then the 16 fractions. */
typedef s32 Mtx_t[4][4];

typedef union {
    Mtx_t m;
    long long int force_structure_alignment;
} Mtx;

typedef struct {
    short vscale[4];
    short vtrans[4];
} Vp_t;

typedef union {
    Vp_t vp;
    long long int force_structure_alignment;
} Vp;

typedef struct {
    unsigned char col[3];
    char pad1;
    unsigned char colc[3]; /* the color again */
    char pad2;
    signed char dir[3];
    char pad3;
} Light_t;

typedef union {
    Light_t l;
    long long int force_structure_alignment[2];
} Light;

typedef struct {
    unsigned char col[3];
    char pad1;
    unsigned char colc[3];
    char pad2;
} Ambient_t;

typedef union {
    Ambient_t l;
    long long int force_structure_alignment[1];
} Ambient;

typedef struct {
    Light l[2];
} LookAt;

typedef struct {
    int x1, y1, x2, y2;
} Hilite_t;

typedef union {
    Hilite_t h;
    s32 force_structure_alignment[4];
} Hilite;

/* The image that the Sprite2D microcode draws. */
typedef struct {
    PTR32(void) SourceImagePointer;
    PTR32(void) TlutPointer;
    short Stride;
    short SubImageWidth;
    short SubImageHeight;
    char SourceImageType;
    char SourceImageBitSize;
    short SourceImageOffsetS;
    short SourceImageOffsetT;
    char dummy[4];
} uSprite_t;

typedef union {
    uSprite_t s;
    long long int force_structure_allignment[3];
} uSprite;

/* One command: two 32-bit words. */
typedef struct {
    unsigned int w0;
    unsigned int w1;
} Gwords;

typedef union {
    Gwords words;
    long long int force_structure_alignment;
} Gfx;

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* Command numbers. The top byte of a command's first word holds the low 8 bits. */

/* Commands that load data from an address. */
#define G_MTX 1
#define G_MOVEMEM 3
#define G_VTX 4
#define G_DL 6
#define G_SPRITE2D_BASE 9

/* Commands that hold all their data. They count down from -65. */
#define GBI_IMM(n) (-65 - (n))
#define G_TRI1 GBI_IMM(0)
#define G_POPMTX GBI_IMM(2)
#define G_MOVEWORD GBI_IMM(3)
#define G_TEXTURE GBI_IMM(4)
#define G_SETOTHERMODE_H GBI_IMM(5)
#define G_SETOTHERMODE_L GBI_IMM(6)
#define G_ENDDL GBI_IMM(7)
#define G_SETGEOMETRYMODE GBI_IMM(8)
#define G_CLEARGEOMETRYMODE GBI_IMM(9)
#define G_RDPHALF_1 GBI_IMM(11)
#define G_RDPHALF_2 GBI_IMM(12)
#define G_TRI2 GBI_IMM(14)
/* The Sprite2D microcode gives two of these numbers another meaning. */
#define G_SPRITE2D_SCALEFLIP GBI_IMM(1)
#define G_SPRITE2D_DRAW GBI_IMM(2)

/* Commands for the RDP. */
#define G_TEXRECT 0xe4
#define G_RDPLOADSYNC 0xe6
#define G_RDPPIPESYNC 0xe7
#define G_RDPTILESYNC 0xe8
#define G_RDPFULLSYNC 0xe9
#define G_SETSCISSOR 0xed
#define G_SETPRIMDEPTH 0xee
#define G_LOADTLUT 0xf0
#define G_SETTILESIZE 0xf2
#define G_LOADBLOCK 0xf3
#define G_LOADTILE 0xf4
#define G_SETTILE 0xf5
#define G_FILLRECT 0xf6
#define G_SETFILLCOLOR 0xf7
#define G_SETFOGCOLOR 0xf8
#define G_SETBLENDCOLOR 0xf9
#define G_SETPRIMCOLOR 0xfa
#define G_SETCOMBINE 0xfc
#define G_SETTIMG 0xfd
#define G_SETZIMG 0xfe
#define G_SETCIMG 0xff

/* Writes one command at pkt. pkt is evaluated once. */
#define GBI_PUT(pkt, word0, word1)                                                                                    \
    {                                                                                                                  \
        Gfx *gbi_put_cmd_ = (Gfx *)(pkt);                                                                              \
        gbi_put_cmd_->words.w0 = (word0);                                                                              \
        gbi_put_cmd_->words.w1 = (word1);                                                                              \
    }

#define GBI_OP(op) _SHIFTL(op, 24, 8)
#define GBI_HALVES(hi, lo) (_SHIFTL(hi, 16, 16) | _SHIFTL(lo, 0, 16))

/* A command that makes the RSP read len bytes at addr. */
#define GBI_LOAD(pkt, op, addr, len, par)                                                                             \
    GBI_PUT(pkt, (GBI_OP(op) | _SHIFTL((par), 16, 8) | _SHIFTL((len), 0, 16)), PORT_GBI_ADDR(addr))

/* A command that stores one word in the RSP's state. */
#define GBI_MOVEWORD(pkt, index, offset, data)                                                                        \
    GBI_PUT(pkt, (GBI_OP(G_MOVEWORD) | _SHIFTL((offset), 8, 16) | _SHIFTL((index), 0, 8)), (unsigned int)(data))

#define G_MW_NUMLIGHT 0x02
#define G_MW_CLIP 0x04
#define G_MW_FOG 0x08
#define G_MW_PERSPNORM 0x0e

/* Matrices. The flags are gSPMatrix's last argument. */
#define G_MTX_MODELVIEW 0x00
#define G_MTX_PROJECTION 0x01
#define G_MTX_MUL 0x00
#define G_MTX_LOAD 0x02
#define G_MTX_NOPUSH 0x00
#define G_MTX_PUSH 0x04

#define gSPMatrix(pkt, m, p) GBI_LOAD(pkt, G_MTX, m, sizeof(Mtx), p)
#define gSPPopMatrix(pkt, n) GBI_PUT(pkt, GBI_OP(G_POPMTX), PORT_GBI_ADDR(n))
#define gSPPerspNormalize(pkt, s) GBI_MOVEWORD(pkt, G_MW_PERSPNORM, 0, (s))

/* The viewport, and the clip ratio r: one of FRUSTRATIO_1 to FRUSTRATIO_6. */
#define G_MV_VIEWPORT 0x80
#define gSPViewport(pkt, v) GBI_LOAD((pkt), G_MOVEMEM, (v), sizeof(Vp), G_MV_VIEWPORT)

#define GBI_CLIP_FRUSTRATIO_1 1
#define GBI_CLIP_FRUSTRATIO_2 2
#define GBI_CLIP_FRUSTRATIO_3 3
#define GBI_CLIP_FRUSTRATIO_4 4
#define GBI_CLIP_FRUSTRATIO_5 5
#define GBI_CLIP_FRUSTRATIO_6 6
#define gSPClipRatio(pkt, r)                                                                                          \
    {                                                                                                                  \
        GBI_MOVEWORD(pkt, G_MW_CLIP, 0x04, GBI_CLIP_##r);                                                              \
        GBI_MOVEWORD(pkt, G_MW_CLIP, 0x0c, GBI_CLIP_##r);                                                              \
        GBI_MOVEWORD(pkt, G_MW_CLIP, 0x14, 0x10000 - GBI_CLIP_##r);                                                    \
        GBI_MOVEWORD(pkt, G_MW_CLIP, 0x1c, 0x10000 - GBI_CLIP_##r);                                                    \
    }

/* Vertices and triangles. A triangle holds three indices into the vertex buffer; flag says which comes first. */
#define gSPVertex(pkt, v, n, v0) GBI_LOAD((pkt), G_VTX, (v), ((n) << 10) | (sizeof(Vtx) * (n) - 1), (v0) * 2)

#define GBI_TRI(a, b, c) (_SHIFTL((a) * 2, 16, 8) | _SHIFTL((b) * 2, 8, 8) | _SHIFTL((c) * 2, 0, 8))
#define GBI_TRI_FROM(v0, v1, v2, flag)                                                                                \
    (((flag) == 0) ? GBI_TRI(v0, v1, v2) : ((flag) == 1) ? GBI_TRI(v1, v2, v0) : GBI_TRI(v2, v0, v1))

#define gSP1Triangle(pkt, v0, v1, v2, flag) GBI_PUT(pkt, GBI_OP(G_TRI1), GBI_TRI_FROM(v0, v1, v2, flag))
#define gSP2Triangles(pkt, v00, v01, v02, flag0, v10, v11, v12, flag1)                                                \
    GBI_PUT(pkt, (GBI_OP(G_TRI2) | GBI_TRI_FROM(v00, v01, v02, flag0)), GBI_TRI_FROM(v10, v11, v12, flag1))

/* Display lists. */
#define gSPDisplayList(pkt, dl) GBI_LOAD(pkt, G_DL, dl, 0, 0)
#define gSPEndDisplayList(pkt) GBI_PUT(pkt, GBI_OP(G_ENDDL), 0)

/* The geometry mode: flags for the RSP's work on vertices and triangles. */
#define G_ZBUFFER 0x00000001
#define G_SHADE 0x00000004
#define G_SHADING_SMOOTH 0x00000200
#define G_CULL_BACK 0x00002000
#define G_CULL_BOTH 0x00003000
#define G_FOG 0x00010000
#define G_LIGHTING 0x00020000
#define G_TEXTURE_GEN 0x00040000
#define G_TEXTURE_GEN_LINEAR 0x00080000
#define G_LOD 0x00100000

#define gSPSetGeometryMode(pkt, word) GBI_PUT(pkt, GBI_OP(G_SETGEOMETRYMODE), (unsigned int)(word))
#define gSPClearGeometryMode(pkt, word) GBI_PUT(pkt, GBI_OP(G_CLEARGEOMETRYMODE), (unsigned int)(word))

/* Lights and fog. Light n is 1 for the first light. */
#define G_MV_L0 0x86
#define gSPLight(pkt, l, n) GBI_LOAD(pkt, G_MOVEMEM, l, sizeof(Light), ((n) - 1) * 2 + G_MV_L0)
#define gSPNumLights(pkt, n) GBI_MOVEWORD(pkt, G_MW_NUMLIGHT, 0, (((n) + 1) * 32 + 0x80000000))
#define gSPFogPosition(pkt, min, max)                                                                                 \
    GBI_MOVEWORD(pkt, G_MW_FOG, 0,                                                                                     \
                 (_SHIFTL((128000 / ((max) - (min))), 16, 16)                                                          \
                  | _SHIFTL(((500 - (min)) * 256 / ((max) - (min))), 0, 16)))

/* Texture scale and tile. */
#define gSPTexture(pkt, s, t, level, tile, on)                                                                        \
    GBI_PUT(pkt, (GBI_OP(G_TEXTURE) | _SHIFTL((level), 11, 3) | _SHIFTL((tile), 8, 3) | _SHIFTL((on), 0, 8)),         \
            GBI_HALVES((s), (t)))

/* The Sprite2D microcode. */
#define gSPSprite2DBase(pkt, s) GBI_LOAD(pkt, G_SPRITE2D_BASE, s, sizeof(uSprite), 0)
#define gSPSprite2DScaleFlip(pkt, sx, sy, fx, fy)                                                                     \
    GBI_PUT(pkt, (GBI_OP(G_SPRITE2D_SCALEFLIP) | _SHIFTL((fx), 8, 8) | _SHIFTL((fy), 0, 8)), GBI_HALVES((sx), (sy)))
#define gSPSprite2DDraw(pkt, px, py) GBI_PUT(pkt, (GBI_OP(G_SPRITE2D_DRAW)), GBI_HALVES((px), (py)))

/* The RDP's other modes. Each is a field of a 64-bit word: the upper half (H) or the lower half (L). A value is
   already at its field's place. */
#define GBI_MODE(pkt, op, shift, len, data)                                                                           \
    GBI_PUT(pkt, (GBI_OP(op) | _SHIFTL(shift, 8, 8) | _SHIFTL(len, 0, 8)), (unsigned int)(data))

/* The first bit of each field. */
#define GBI_AT_ALPHACOMPARE 0
#define GBI_AT_DEPTHSOURCE 2
#define GBI_AT_RENDERMODE 3
#define GBI_AT_DITHER 6
#define GBI_AT_COMBINEKEY 8
#define GBI_AT_CONVERT 9
#define GBI_AT_FILTER 12
#define GBI_AT_LUT 14
#define GBI_AT_LOD 16
#define GBI_AT_DETAIL 17
#define GBI_AT_PERSP 19
#define GBI_AT_CYCLE 20
#define GBI_AT_PIPELINE 23

#define G_PM_NPRIMITIVE (0 << GBI_AT_PIPELINE)
#define G_PM_1PRIMITIVE (1 << GBI_AT_PIPELINE)
#define G_CYC_1CYCLE (0 << GBI_AT_CYCLE)
#define G_CYC_2CYCLE (1 << GBI_AT_CYCLE)
#define G_CYC_FILL (3 << GBI_AT_CYCLE)
#define G_TP_NONE (0 << GBI_AT_PERSP)
#define G_TP_PERSP (1 << GBI_AT_PERSP)
#define G_TD_CLAMP (0 << GBI_AT_DETAIL)
#define G_TL_TILE (0 << GBI_AT_LOD)
#define G_TL_LOD (1 << GBI_AT_LOD)
#define G_TT_NONE (0 << GBI_AT_LUT)
#define G_TT_RGBA16 (2 << GBI_AT_LUT)
#define G_TF_POINT (0 << GBI_AT_FILTER)
#define G_TF_BILERP (2 << GBI_AT_FILTER)
#define G_TC_FILT (6 << GBI_AT_CONVERT)
#define G_CK_NONE (0 << GBI_AT_COMBINEKEY)
#define G_CD_MAGICSQ (0 << GBI_AT_DITHER)
#define G_ZS_PIXEL (0 << GBI_AT_DEPTHSOURCE)
#define G_AC_NONE (0 << GBI_AT_ALPHACOMPARE)
#define G_AC_THRESHOLD (1 << GBI_AT_ALPHACOMPARE)

#define gDPPipelineMode(pkt, mode) GBI_MODE(pkt, G_SETOTHERMODE_H, GBI_AT_PIPELINE, 1, mode)
#define gDPSetCycleType(pkt, type) GBI_MODE(pkt, G_SETOTHERMODE_H, GBI_AT_CYCLE, 2, type)
#define gDPSetTexturePersp(pkt, type) GBI_MODE(pkt, G_SETOTHERMODE_H, GBI_AT_PERSP, 1, type)
#define gDPSetTextureDetail(pkt, type) GBI_MODE(pkt, G_SETOTHERMODE_H, GBI_AT_DETAIL, 2, type)
#define gDPSetTextureLOD(pkt, type) GBI_MODE(pkt, G_SETOTHERMODE_H, GBI_AT_LOD, 1, type)
#define gDPSetTextureLUT(pkt, type) GBI_MODE(pkt, G_SETOTHERMODE_H, GBI_AT_LUT, 2, type)
#define gDPSetTextureFilter(pkt, type) GBI_MODE(pkt, G_SETOTHERMODE_H, GBI_AT_FILTER, 2, type)
#define gDPSetTextureConvert(pkt, type) GBI_MODE(pkt, G_SETOTHERMODE_H, GBI_AT_CONVERT, 3, type)
#define gDPSetCombineKey(pkt, type) GBI_MODE(pkt, G_SETOTHERMODE_H, GBI_AT_COMBINEKEY, 1, type)
#define gDPSetColorDither(pkt, mode) GBI_MODE(pkt, G_SETOTHERMODE_H, GBI_AT_DITHER, 2, mode)
#define gDPSetAlphaCompare(pkt, type) GBI_MODE(pkt, G_SETOTHERMODE_L, GBI_AT_ALPHACOMPARE, 2, type)
#define gDPSetDepthSource(pkt, src) GBI_MODE(pkt, G_SETOTHERMODE_L, GBI_AT_DEPTHSOURCE, 1, src)
/* c0 is a render mode for the first cycle. c1 is one for the second cycle: a name that ends in 2. */
#define gDPSetRenderMode(pkt, c0, c1) GBI_MODE(pkt, G_SETOTHERMODE_L, GBI_AT_RENDERMODE, 29, (c0) | (c1))

/* Render modes. A render mode is a set of flags and the four inputs of the blender for one cycle. */
#define GBI_RM_AA 0x8             /* antialiasing */
#define GBI_RM_Z_CMP 0x10         /* compare with the depth buffer */
#define GBI_RM_Z_UPD 0x20         /* write the depth buffer */
#define GBI_RM_IM_RD 0x40         /* read the color image */
#define GBI_RM_CLR_ON_CVG 0x80    /* write color only when coverage overflows */
#define GBI_RM_CVG_WRAP 0x100     /* coverage: clamp is 0 */
#define GBI_RM_CVG_FULL 0x200
#define GBI_RM_CVG_SAVE 0x300
#define GBI_RM_Z_XLU 0x800        /* the depth mode for translucent surfaces: opaque is 0 */
#define GBI_RM_CVG_X_ALPHA 0x1000 /* coverage times alpha */
#define GBI_RM_ALPHA_CVG 0x2000   /* coverage as alpha */
#define GBI_RM_FORCE_BL 0x4000    /* blend every pixel */

/* The blender computes (p * a + m * b) / (a + b). The inputs of cycle 1 start at bits 30, 26, 22 and 18, and those
   of cycle 2 two bits lower. */
#define GBI_BLEND(cycle, p, a, m, b)                                                                                  \
    ((p) << (32 - 2 * (cycle)) | (a) << (28 - 2 * (cycle)) | (m) << (24 - 2 * (cycle)) | (b) << (20 - 2 * (cycle)))
/* p and m: 0 the pixel, 1 the color image, 3 the fog color. a: 0 the pixel's alpha, 1 the fog alpha, 2 the shade
   alpha, 3 zero. b: 0 one minus a, 1 the image's coverage, 2 one. */
#define GBI_BLEND_OPA(cycle) GBI_BLEND(cycle, 0, 0, 1, 1)
#define GBI_BLEND_XLU(cycle) GBI_BLEND(cycle, 0, 0, 1, 0)
#define GBI_BLEND_NONE(cycle) GBI_BLEND(cycle, 0, 3, 0, 2)

#define GBI_AA_ZB_OPA_SURF(cycle)                                                                                     \
    (GBI_RM_AA | GBI_RM_Z_CMP | GBI_RM_Z_UPD | GBI_RM_IM_RD | GBI_RM_ALPHA_CVG | GBI_BLEND_OPA(cycle))
#define GBI_RA_ZB_OPA_SURF(cycle) (GBI_RM_AA | GBI_RM_Z_CMP | GBI_RM_Z_UPD | GBI_RM_ALPHA_CVG | GBI_BLEND_OPA(cycle))
#define GBI_AA_ZB_XLU_SURF(cycle)                                                                                     \
    (GBI_RM_AA | GBI_RM_Z_CMP | GBI_RM_IM_RD | GBI_RM_CVG_WRAP | GBI_RM_CLR_ON_CVG | GBI_RM_FORCE_BL | GBI_RM_Z_XLU   \
     | GBI_BLEND_XLU(cycle))
#define GBI_AA_ZB_TEX_EDGE(cycle)                                                                                     \
    (GBI_RM_AA | GBI_RM_Z_CMP | GBI_RM_Z_UPD | GBI_RM_IM_RD | GBI_RM_CVG_X_ALPHA | GBI_RM_ALPHA_CVG                   \
     | GBI_BLEND_OPA(cycle))
#define GBI_AA_OPA_SURF(cycle) (GBI_RM_AA | GBI_RM_IM_RD | GBI_RM_ALPHA_CVG | GBI_BLEND_OPA(cycle))
#define GBI_RA_OPA_SURF(cycle) (GBI_RM_AA | GBI_RM_ALPHA_CVG | GBI_BLEND_OPA(cycle))
#define GBI_AA_XLU_SURF(cycle)                                                                                        \
    (GBI_RM_AA | GBI_RM_IM_RD | GBI_RM_CVG_WRAP | GBI_RM_CLR_ON_CVG | GBI_RM_FORCE_BL | GBI_BLEND_XLU(cycle))
#define GBI_AA_TEX_EDGE(cycle)                                                                                        \
    (GBI_RM_AA | GBI_RM_IM_RD | GBI_RM_CVG_X_ALPHA | GBI_RM_ALPHA_CVG | GBI_BLEND_OPA(cycle))
#define GBI_ZB_OPA_SURF(cycle)                                                                                        \
    (GBI_RM_Z_CMP | GBI_RM_Z_UPD | GBI_RM_CVG_FULL | GBI_RM_ALPHA_CVG | GBI_BLEND_OPA(cycle))
#define GBI_ZB_XLU_SURF(cycle)                                                                                        \
    (GBI_RM_Z_CMP | GBI_RM_IM_RD | GBI_RM_CVG_FULL | GBI_RM_FORCE_BL | GBI_RM_Z_XLU | GBI_BLEND_XLU(cycle))
#define GBI_ZB_CLD_SURF(cycle)                                                                                        \
    (GBI_RM_Z_CMP | GBI_RM_IM_RD | GBI_RM_CVG_SAVE | GBI_RM_FORCE_BL | GBI_RM_Z_XLU | GBI_BLEND_XLU(cycle))
#define GBI_OPA_SURF(cycle) (GBI_RM_FORCE_BL | GBI_BLEND_NONE(cycle))
#define GBI_XLU_SURF(cycle) (GBI_RM_IM_RD | GBI_RM_CVG_FULL | GBI_RM_FORCE_BL | GBI_BLEND_XLU(cycle))
#define GBI_TEX_EDGE(cycle)                                                                                           \
    (GBI_RM_CVG_X_ALPHA | GBI_RM_ALPHA_CVG | GBI_RM_FORCE_BL | GBI_RM_AA | GBI_BLEND_NONE(cycle))

#define G_RM_AA_ZB_OPA_SURF GBI_AA_ZB_OPA_SURF(1)
#define G_RM_AA_ZB_OPA_SURF2 GBI_AA_ZB_OPA_SURF(2)
#define G_RM_RA_ZB_OPA_SURF GBI_RA_ZB_OPA_SURF(1)
#define G_RM_RA_ZB_OPA_SURF2 GBI_RA_ZB_OPA_SURF(2)
#define G_RM_AA_ZB_XLU_SURF GBI_AA_ZB_XLU_SURF(1)
#define G_RM_AA_ZB_XLU_SURF2 GBI_AA_ZB_XLU_SURF(2)
#define G_RM_AA_ZB_TEX_EDGE GBI_AA_ZB_TEX_EDGE(1)
#define G_RM_AA_ZB_TEX_EDGE2 GBI_AA_ZB_TEX_EDGE(2)
#define G_RM_AA_OPA_SURF GBI_AA_OPA_SURF(1)
#define G_RM_AA_OPA_SURF2 GBI_AA_OPA_SURF(2)
#define G_RM_RA_OPA_SURF GBI_RA_OPA_SURF(1)
#define G_RM_RA_OPA_SURF2 GBI_RA_OPA_SURF(2)
#define G_RM_AA_XLU_SURF GBI_AA_XLU_SURF(1)
#define G_RM_AA_XLU_SURF2 GBI_AA_XLU_SURF(2)
#define G_RM_AA_TEX_EDGE GBI_AA_TEX_EDGE(1)
#define G_RM_AA_TEX_EDGE2 GBI_AA_TEX_EDGE(2)
#define G_RM_ZB_OPA_SURF GBI_ZB_OPA_SURF(1)
#define G_RM_ZB_OPA_SURF2 GBI_ZB_OPA_SURF(2)
#define G_RM_ZB_XLU_SURF GBI_ZB_XLU_SURF(1)
#define G_RM_ZB_XLU_SURF2 GBI_ZB_XLU_SURF(2)
#define G_RM_ZB_CLD_SURF GBI_ZB_CLD_SURF(1)
#define G_RM_ZB_CLD_SURF2 GBI_ZB_CLD_SURF(2)
#define G_RM_OPA_SURF GBI_OPA_SURF(1)
#define G_RM_OPA_SURF2 GBI_OPA_SURF(2)
#define G_RM_XLU_SURF GBI_XLU_SURF(1)
#define G_RM_XLU_SURF2 GBI_XLU_SURF(2)
#define G_RM_TEX_EDGE GBI_TEX_EDGE(1)
#define G_RM_TEX_EDGE2 GBI_TEX_EDGE(2)

/* First-cycle modes that have no flags: fog by the shade alpha, fog by the fog alpha, and no change. */
#define G_RM_FOG_SHADE_A GBI_BLEND(1, 3, 2, 0, 0)
#define G_RM_FOG_PRIM_A GBI_BLEND(1, 3, 1, 0, 0)
#define G_RM_PASS GBI_BLEND_NONE(1)

/* The color combiner computes (a - b) * c + d for color and for alpha, in each of the two cycles. These are the
   numbers of the inputs that the game uses. gDPSetCombineLERP takes the names without the prefix. */
#define G_CCMUX_COMBINED 0
#define G_CCMUX_TEXEL0 1
#define G_CCMUX_TEXEL1 2
#define G_CCMUX_PRIMITIVE 3
#define G_CCMUX_SHADE 4
#define G_CCMUX_1 6
#define G_CCMUX_LOD_FRACTION 13
#define G_CCMUX_0 31

#define G_ACMUX_COMBINED 0
#define G_ACMUX_LOD_FRACTION 0
#define G_ACMUX_TEXEL0 1
#define G_ACMUX_TEXEL1 2
#define G_ACMUX_PRIMITIVE 3
#define G_ACMUX_SHADE 4
#define G_ACMUX_0 7

/* The first word holds a and c of both cycles, the second word b and d. */
#define gDPSetCombineLERP(pkt, a0, b0, c0, d0, Aa0, Ab0, Ac0, Ad0, a1, b1, c1, d1, Aa1, Ab1, Ac1, Ad1)                \
    GBI_PUT(pkt,                                                                                                       \
            GBI_OP(G_SETCOMBINE)                                                                                       \
                | _SHIFTL((_SHIFTL(G_CCMUX_##a0, 20, 4) | _SHIFTL(G_CCMUX_##c0, 15, 5)                                 \
                           | _SHIFTL(G_ACMUX_##Aa0, 12, 3) | _SHIFTL(G_ACMUX_##Ac0, 9, 3))                             \
                              | (_SHIFTL(G_CCMUX_##a1, 5, 4) | _SHIFTL(G_CCMUX_##c1, 0, 5)),                           \
                          0, 24),                                                                                      \
            (unsigned int)((_SHIFTL(G_CCMUX_##b0, 28, 4) | _SHIFTL(G_CCMUX_##d0, 15, 3)                                \
                            | _SHIFTL(G_ACMUX_##Ab0, 12, 3) | _SHIFTL(G_ACMUX_##Ad0, 9, 3))                            \
                           | (_SHIFTL(G_CCMUX_##b1, 24, 4) | _SHIFTL(G_ACMUX_##Aa1, 21, 3)                             \
                              | _SHIFTL(G_ACMUX_##Ac1, 18, 3) | _SHIFTL(G_CCMUX_##d1, 6, 3)                            \
                              | _SHIFTL(G_ACMUX_##Ab1, 3, 3) | _SHIFTL(G_ACMUX_##Ad1, 0, 3))))

/* A combine mode is the eight inputs of one cycle. gDPSetCombineMode takes one for each cycle. */
#define gDPSetCombineMode(pkt, a, b) gDPSetCombineLERP(pkt, a, b)

#define G_CC_PRIMITIVE 0, 0, 0, PRIMITIVE, 0, 0, 0, PRIMITIVE
#define G_CC_SHADE 0, 0, 0, SHADE, 0, 0, 0, SHADE
#define G_CC_DECALRGB 0, 0, 0, TEXEL0, 0, 0, 0, SHADE
#define G_CC_MODULATEI TEXEL0, 0, SHADE, 0, 0, 0, 0, SHADE
#define G_CC_MODULATERGB G_CC_MODULATEI
#define G_CC_MODULATEIA TEXEL0, 0, SHADE, 0, TEXEL0, 0, SHADE, 0
#define G_CC_MODULATEIDECALA TEXEL0, 0, SHADE, 0, 0, 0, 0, TEXEL0
#define G_CC_MODULATEIA_PRIM TEXEL0, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0
#define G_CC_TRILERP TEXEL1, TEXEL0, LOD_FRACTION, TEXEL0, TEXEL1, TEXEL0, LOD_FRACTION, TEXEL0
/* For the second cycle. */
#define G_CC_PASS2 0, 0, 0, COMBINED, 0, 0, 0, COMBINED
#define G_CC_DECALRGB2 0, 0, 0, COMBINED, 0, 0, 0, SHADE
#define G_CC_MODULATERGB2 COMBINED, 0, SHADE, 0, 0, 0, 0, SHADE

/* Colors and the primitive depth. */
#define GBI_RGBA(r, g, b, a) (_SHIFTL(r, 24, 8) | _SHIFTL(g, 16, 8) | _SHIFTL(b, 8, 8) | _SHIFTL(a, 0, 8))
#define GBI_COLOR(pkt, op, word) GBI_PUT(pkt, GBI_OP(op), (unsigned int)(word))

#define gDPSetBlendColor(pkt, r, g, b, a) GBI_COLOR(pkt, G_SETBLENDCOLOR, GBI_RGBA(r, g, b, a))
#define gDPSetFogColor(pkt, r, g, b, a) GBI_COLOR(pkt, G_SETFOGCOLOR, GBI_RGBA(r, g, b, a))
#define gDPSetFillColor(pkt, d) GBI_COLOR(pkt, G_SETFILLCOLOR, (d))
#define gDPSetPrimDepth(pkt, z, dz) GBI_COLOR(pkt, G_SETPRIMDEPTH, _SHIFTL(z, 16, 16) | _SHIFTL(dz, 0, 16))
#define gDPSetPrimColor(pkt, m, l, r, g, b, a)                                                                        \
    GBI_PUT(pkt, (GBI_OP(G_SETPRIMCOLOR) | _SHIFTL(m, 8, 8) | _SHIFTL(l, 0, 8)), GBI_RGBA(r, g, b, a))

/* A 16-bit pixel: 5 bits each of red, green and blue from 8-bit values, and 1 bit of alpha. */
#define GPACK_RGBA5551(r, g, b, a) ((((r) << 8) & 0xf800) | (((g) << 3) & 0x7c0) | (((b) >> 2) & 0x3e) | ((a) & 0x1))

/* Images: their formats and pixel sizes, and the commands that name the color, depth and texture images. */
#define G_IM_FMT_RGBA 0
#define G_IM_FMT_CI 2
#define G_IM_FMT_IA 3

#define G_IM_SIZ_4b 0
#define G_IM_SIZ_8b 1
#define G_IM_SIZ_16b 2
#define G_IM_SIZ_32b 3

#define GBI_IMAGE(pkt, op, fmt, siz, width, img)                                                                      \
    GBI_PUT(pkt, GBI_OP(op) | _SHIFTL(fmt, 21, 3) | _SHIFTL(siz, 19, 2) | _SHIFTL((width) - 1, 0, 12),                \
            PORT_GBI_ADDR(img))

#define gDPSetColorImage(pkt, f, s, w, i) GBI_IMAGE(pkt, G_SETCIMG, f, s, w, i)
#define gDPSetDepthImage(pkt, i) GBI_IMAGE(pkt, G_SETZIMG, 0, 0, 1, i)
#define gDPSetTextureImage(pkt, f, s, w, i) GBI_IMAGE(pkt, G_SETTIMG, f, s, w, i)

/* Waits. */
#define GBI_BARE(pkt, op) GBI_PUT(pkt, GBI_OP(op), 0)
#define gDPFullSync(pkt) GBI_BARE(pkt, G_RDPFULLSYNC)
#define gDPTileSync(pkt) GBI_BARE(pkt, G_RDPTILESYNC)
#define gDPPipeSync(pkt) GBI_BARE(pkt, G_RDPPIPESYNC)
#define gDPLoadSync(pkt) GBI_BARE(pkt, G_RDPLOADSYNC)

/* Tiles. A tile describes a texture in texture memory. Textures are loaded through one tile and drawn through
   another. */
#define G_TX_RENDERTILE 0
#define G_TX_LOADTILE 7

#define G_TX_WRAP 0
#define G_TX_MIRROR 0x1
#define G_TX_CLAMP 0x2
#define G_TX_NOMASK 0
#define G_TX_NOLOD 0

#define gDPSetTile(pkt, fmt, siz, line, tmem, tile, palette, cmt, maskt, shiftt, cms, masks, shifts)                  \
    GBI_PUT(pkt,                                                                                                       \
            GBI_OP(G_SETTILE) | _SHIFTL(fmt, 21, 3) | _SHIFTL(siz, 19, 2) | _SHIFTL(line, 9, 9) | _SHIFTL(tmem, 0, 9), \
            _SHIFTL(tile, 24, 3) | _SHIFTL(palette, 20, 4) | _SHIFTL(cmt, 18, 2) | _SHIFTL(maskt, 14, 4)               \
                | _SHIFTL(shiftt, 10, 4) | _SHIFTL(cms, 8, 2) | _SHIFTL(masks, 4, 4) | _SHIFTL(shifts, 0, 4))

/* A command with a tile and two pairs of 12-bit numbers. */
#define GBI_TILE_RECT(pkt, op, tile, a0, b0, a1, b1)                                                                  \
    GBI_PUT(pkt, GBI_OP(op) | _SHIFTL(a0, 12, 12) | _SHIFTL(b0, 0, 12),                                               \
            _SHIFTL(tile, 24, 3) | _SHIFTL(a1, 12, 12) | _SHIFTL(b1, 0, 12))

#define gDPSetTileSize(pkt, t, uls, ult, lrs, lrt) GBI_TILE_RECT(pkt, G_SETTILESIZE, t, uls, ult, lrs, lrt)
#define gDPLoadTile(pkt, t, uls, ult, lrs, lrt) GBI_TILE_RECT(pkt, G_LOADTILE, t, uls, ult, lrs, lrt)
/* Loads texels uls to lrs as one row: 2047 at most. dxt is the step in t for each 64-bit word. */
#define GBI_LOAD_BLOCK(pkt, tile, uls, ult, lrs, dxt)                                                                 \
    GBI_TILE_RECT(pkt, G_LOADBLOCK, tile, uls, ult, (MIN(lrs, 2047)), dxt)
#define GBI_LOAD_TLUT(pkt, tile, count)                                                                               \
    GBI_PUT(pkt, GBI_OP(G_LOADTLUT), _SHIFTL((tile), 24, 3) | _SHIFTL((count), 14, 10))

/* Facts about a pixel size that the texture loads below need. They append these suffixes to their siz argument.
   LOAD_BLOCK is the size that the load uses. INCR and SHIFT turn a count of texels into a count of 16-bit units.
   The BYTES values are bytes for a texel: 32-bit textures count 2, because their load splits them in two. */
#define G_IM_SIZ_8b_LOAD_BLOCK G_IM_SIZ_16b
#define G_IM_SIZ_8b_INCR 1
#define G_IM_SIZ_8b_SHIFT 1
#define G_IM_SIZ_8b_BYTES 1
#define G_IM_SIZ_8b_LINE_BYTES 1
#define G_IM_SIZ_8b_TILE_BYTES 1

#define G_IM_SIZ_16b_LOAD_BLOCK G_IM_SIZ_16b
#define G_IM_SIZ_16b_INCR 0
#define G_IM_SIZ_16b_SHIFT 0
#define G_IM_SIZ_16b_BYTES 2
#define G_IM_SIZ_16b_LINE_BYTES 2
#define G_IM_SIZ_16b_TILE_BYTES 2

#define G_IM_SIZ_32b_LINE_BYTES 2
#define G_IM_SIZ_32b_TILE_BYTES 2

/* The step in t for a row that takes the given number of 64-bit words, as a fraction of 2048. */
#define GBI_DXT(words) (((1 << 11) + words - 1) / words)
#define GBI_ROW_WORDS(width, bytes) MAX(1, ((width) * (bytes) / 8))
#define GBI_ROW_WORDS_4b(width) MAX(1, ((width) / 16))

/* Loads a whole texture and sets the tile that draws it. */
#define gDPLoadTextureBlock(pkt, timg, fmt, siz, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt)          \
    {                                                                                                                  \
        gDPSetTextureImage(pkt, fmt, siz##_LOAD_BLOCK, 1, timg);                                                       \
        gDPSetTile(pkt, fmt, siz##_LOAD_BLOCK, 0, 0, G_TX_LOADTILE, 0, cmt, maskt, shiftt, cms, masks, shifts);        \
        gDPLoadSync(pkt);                                                                                              \
        GBI_LOAD_BLOCK(pkt, G_TX_LOADTILE, 0, 0, (((width) * (height) + siz##_INCR) >> siz##_SHIFT) - 1,               \
                       GBI_DXT(GBI_ROW_WORDS(width, siz##_BYTES)));                                                    \
        gDPPipeSync(pkt);                                                                                              \
        gDPSetTile(pkt, fmt, siz, (((width) * siz##_LINE_BYTES) + 7) >> 3, 0, G_TX_RENDERTILE, pal, cmt, maskt,        \
                   shiftt, cms, masks, shifts);                                                                        \
        gDPSetTileSize(pkt, G_TX_RENDERTILE, 0, 0, ((width) - 1) << 2, ((height) - 1) << 2)                            \
    }

/* The same for a texture with 4 bits for each texel. */
#define gDPLoadTextureBlock_4b(pkt, timg, fmt, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt)            \
    {                                                                                                                  \
        gDPSetTextureImage(pkt, fmt, G_IM_SIZ_16b, 1, timg);                                                           \
        gDPSetTile(pkt, fmt, G_IM_SIZ_16b, 0, 0, G_TX_LOADTILE, 0, cmt, maskt, shiftt, cms, masks, shifts);            \
        gDPLoadSync(pkt);                                                                                              \
        GBI_LOAD_BLOCK(pkt, G_TX_LOADTILE, 0, 0, (((width) * (height) + 3) >> 2) - 1,                                  \
                       GBI_DXT(GBI_ROW_WORDS_4b(width)));                                                              \
        gDPPipeSync(pkt);                                                                                              \
        gDPSetTile(pkt, fmt, G_IM_SIZ_4b, ((((width) >> 1) + 7) >> 3), 0, G_TX_RENDERTILE, pal, cmt, maskt, shiftt,    \
                   cms, masks, shifts);                                                                                \
        gDPSetTileSize(pkt, G_TX_RENDERTILE, 0, 0, ((width) - 1) << 2, ((height) - 1) << 2)                            \
    }

/* Loads the part of a texture from (uls, ult) to (lrs, lrt). */
#define gDPLoadTextureTile(pkt, timg, fmt, siz, width, height, uls, ult, lrs, lrt, pal, cms, cmt, masks, maskt,       \
                           shifts, shiftt)                                                                             \
    {                                                                                                                  \
        gDPSetTextureImage(pkt, fmt, siz, width, timg);                                                                \
        gDPSetTile(pkt, fmt, siz, (((((lrs) - (uls) + 1) * siz##_TILE_BYTES) + 7) >> 3), 0, G_TX_LOADTILE, 0, cmt,     \
                   maskt, shiftt, cms, masks, shifts);                                                                 \
        gDPLoadSync(pkt);                                                                                              \
        gDPLoadTile(pkt, G_TX_LOADTILE, (uls) << 2, (ult) << 2, (lrs) << 2, (lrt) << 2);                               \
        gDPPipeSync(pkt);                                                                                              \
        gDPSetTile(pkt, fmt, siz, (((((lrs) - (uls) + 1) * siz##_LINE_BYTES) + 7) >> 3), 0, G_TX_RENDERTILE, pal,      \
                   cmt, maskt, shiftt, cms, masks, shifts);                                                            \
        gDPSetTileSize(pkt, G_TX_RENDERTILE, (uls) << 2, (ult) << 2, (lrs) << 2, (lrt) << 2)                           \
    }

/* The same for a texture with 4 bits for each texel. It is loaded as 8-bit texels of half the width. */
#define gDPLoadTextureTile_4b(pkt, timg, fmt, width, height, uls, ult, lrs, lrt, pal, cms, cmt, masks, maskt,         \
                              shifts, shiftt)                                                                          \
    {                                                                                                                  \
        gDPSetTextureImage(pkt, fmt, G_IM_SIZ_8b, ((width) >> 1), timg);                                               \
        gDPSetTile(pkt, fmt, G_IM_SIZ_8b, (((((lrs) - (uls) + 1) >> 1) + 7) >> 3), 0, G_TX_LOADTILE, 0, cmt, maskt,    \
                   shiftt, cms, masks, shifts);                                                                        \
        gDPLoadSync(pkt);                                                                                              \
        gDPLoadTile(pkt, G_TX_LOADTILE, (uls) << (2 - 1), (ult) << (2), (lrs) << (2 - 1), (lrt) << (2));               \
        gDPPipeSync(pkt);                                                                                              \
        gDPSetTile(pkt, fmt, G_IM_SIZ_4b, (((((lrs) - (uls) + 1) >> 1) + 7) >> 3), 0, G_TX_RENDERTILE, pal, cmt,       \
                   maskt, shiftt, cms, masks, shifts);                                                                 \
        gDPSetTileSize(pkt, G_TX_RENDERTILE, (uls) << 2, (ult) << 2, (lrs) << 2, (lrt) << 2)                           \
    }

/* Loads a palette of 16-bit colors into the upper half of texture memory: 16 colors at palette pal, or all 256. */
#define GBI_LOAD_PALETTE(pkt, dram, tmem, last)                                                                       \
    {                                                                                                                  \
        gDPSetTextureImage(pkt, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, dram);                                                 \
        gDPTileSync(pkt);                                                                                              \
        gDPSetTile(pkt, 0, 0, 0, tmem, G_TX_LOADTILE, 0, 0, 0, 0, 0, 0, 0);                                            \
        gDPLoadSync(pkt);                                                                                              \
        GBI_LOAD_TLUT(pkt, G_TX_LOADTILE, last);                                                                       \
        gDPPipeSync(pkt)                                                                                               \
    }
#define gDPLoadTLUT_pal16(pkt, pal, dram) GBI_LOAD_PALETTE(pkt, dram, (256 + (((pal) & 0xf) * 16)), 15)
#define gDPLoadTLUT_pal256(pkt, dram) GBI_LOAD_PALETTE(pkt, dram, 256, 255)

/* The scissor, in pixels. */
#define G_SC_NON_INTERLACE 0

#define GBI_QUARTERS(x) ((int)((float)(x) * 4.0F))
#define gDPSetScissor(pkt, mode, ulx, uly, lrx, lry)                                                                  \
    GBI_PUT(pkt, GBI_OP(G_SETSCISSOR) | _SHIFTL(GBI_QUARTERS(ulx), 12, 12) | _SHIFTL(GBI_QUARTERS(uly), 0, 12),       \
            _SHIFTL(mode, 24, 2) | _SHIFTL(GBI_QUARTERS(lrx), 12, 12) | _SHIFTL(GBI_QUARTERS(lry), 0, 12))

/* Rectangles. The fill rectangle takes pixels. The texture rectangles take quarters of a pixel, texture
   coordinates with 5 bits of fraction and steps with 10 bits of fraction. */
#define gDPFillRectangle(pkt, ulx, uly, lrx, lry)                                                                     \
    GBI_PUT(pkt, (GBI_OP(G_FILLRECT) | _SHIFTL((lrx), 14, 10) | _SHIFTL((lry), 2, 10)),                               \
            (_SHIFTL((ulx), 14, 10) | _SHIFTL((uly), 2, 10)))

/* A texture rectangle is three commands: the corners, the texture coordinates and the steps. */
#define GBI_TEXRECT(pkt, word0, word1, st, steps)                                                                     \
    {                                                                                                                  \
        GBI_PUT(pkt, word0, word1)                                                                                     \
        GBI_PUT(pkt, GBI_OP(G_RDPHALF_1), PORT_GBI_ADDR(st))                                                           \
        GBI_PUT(pkt, GBI_OP(G_RDPHALF_2), PORT_GBI_ADDR(steps))                                                        \
    }

#define gSPTextureRectangle(pkt, xl, yl, xh, yh, tile, s, t, dsdx, dtdy)                                              \
    GBI_TEXRECT(pkt, (GBI_OP(G_TEXRECT) | _SHIFTL(xh, 12, 12) | _SHIFTL(yh, 0, 12)),                                  \
                (_SHIFTL(tile, 24, 3) | _SHIFTL(xl, 12, 12) | _SHIFTL(yl, 0, 12)),                                     \
                (_SHIFTL(s, 16, 16) | _SHIFTL(t, 0, 16)), (_SHIFTL(dsdx, 16, 16) | _SHIFTL(dtdy, 0, 16)))

/* A texture rectangle that may start left of or above the screen. The corners stop at 0, and the texture
   coordinates move by the part that was cut off. */
#define GBI_NOT_BELOW_0(v) MAX((s16)(v), 0)
#define GBI_CUT_OFF(negative, edge, step)                                                                             \
    ((negative) ? (((s16)(step) < 0) ? (MAX((((s16)(edge) * (s16)(step)) >> 7), 0))                                   \
                                     : (MIN((((s16)(edge) * (s16)(step)) >> 7), 0)))                                   \
                : 0)

#define gSPScisTextureRectangle(pkt, xl, yl, xh, yh, tile, s, t, dsdx, dtdy)                                          \
    GBI_TEXRECT(pkt,                                                                                                   \
                (GBI_OP(G_TEXRECT) | _SHIFTL(GBI_NOT_BELOW_0(xh), 12, 12) | _SHIFTL(GBI_NOT_BELOW_0(yh), 0, 12)),      \
                (_SHIFTL((tile), 24, 3) | _SHIFTL(GBI_NOT_BELOW_0(xl), 12, 12) | _SHIFTL(GBI_NOT_BELOW_0(yl), 0, 12)), \
                (_SHIFTL(((s) - GBI_CUT_OFF((s16)(xl) < 0, xl, dsdx)), 16, 16)                                         \
                 | _SHIFTL(((t) - GBI_CUT_OFF((yl) < 0, yl, dtdy)), 0, 16)),                                           \
                (_SHIFTL((dsdx), 16, 16) | _SHIFTL((dtdy), 0, 16)))

#endif
