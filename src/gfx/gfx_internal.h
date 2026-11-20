/* Declarations shared by the renderer's files. Nothing outside src/gfx includes this header.

   gfx.c          the public functions, the frame buffer objects and the final picture
   gfx_mem.c      reads of the game's memory
   gfx_dl.c       the display list interpreter: the RSP's work and the RDP's state commands
   gfx_tex.c      texture loads, texture decoding and the texture cache
   gfx_render.c   draw states, combiner shaders, batching and the OpenGL draws
   gfx_gl.c       the OpenGL function loader */
#ifndef GFX_INTERNAL_H
#define GFX_INTERNAL_H

#include <stdint.h>

#include "gfx.h"
#include "gfx_gl.h"

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;

/* The N64's frame. */
#define SCREEN_W 320
#define SCREEN_H 240

/* ---- Memory (gfx_mem.c). The game's data is in host byte order. */

/* A host pointer to size bytes at an address the display lists hold, after segment and KSEG translation. NULL
   when the address is 0 or outside the memory range. */
const u8 *gfx_mem(u32 addr, u32 size);
/* A new frame: a page that did not exist is asked about again. */
void gfx_mem_next_frame(void);

/* ---- The RDP's state (gfx_dl.c). The draw state is built from it. */

/* Other mode H fields. */
#define CYCLE_TYPE(h) (((h) >> 20) & 3)
#define CYC_1 0
#define CYC_2 1
#define CYC_COPY 2
#define CYC_FILL 3
#define TEX_FILTER(h) (((h) >> 12) & 3) /* 0 point, 2 bilinear, 3 average */
#define TEX_LUT(h) (((h) >> 14) & 3)    /* 0 none, 2 RGBA16, 3 IA16 */
#define TEX_PERSP(h) (((h) >> 19) & 1)

/* Other mode L bits. */
#define RM_ALPHA_COMPARE(l) ((l) & 3) /* 0 none, 1 threshold, 3 dither */
#define RM_ZS_PRIM 0x4
#define RM_AA_EN 0x8
#define RM_Z_CMP 0x10
#define RM_Z_UPD 0x20
#define RM_ZMODE(l) ((l) & 0xC00)
#define RM_ZMODE_DEC 0xC00
#define RM_CVG_X_ALPHA 0x1000
#define RM_ALPHA_CVG_SEL 0x2000
#define RM_FORCE_BL 0x4000

/* Geometry mode bits (F3DEX). */
#define GM_ZBUFFER 0x1
#define GM_SHADE 0x4
#define GM_SMOOTH 0x200
#define GM_CULL_FRONT 0x1000
#define GM_CULL_BACK 0x2000
#define GM_FOG 0x10000
#define GM_LIGHTING 0x20000
#define GM_TEXGEN 0x40000
#define GM_TEXGEN_LINEAR 0x80000

/* Texture formats and sizes. */
#define FMT_RGBA 0
#define FMT_YUV 1
#define FMT_CI 2
#define FMT_IA 3
#define FMT_I 4
#define SIZ_4B 0
#define SIZ_8B 1
#define SIZ_16B 2
#define SIZ_32B 3

typedef struct GfxTile {
    u8 fmt, siz, pal;
    u8 cms, cmt;       /* bit 0 mirror, bit 1 clamp */
    u8 masks, maskt;   /* wrap every 1 << mask texels; 0 for none */
    u8 shifts, shiftt; /* 1-10: divide by 1 << shift; 11-15: multiply by 1 << (16 - shift) */
    u16 line;          /* 64-bit words per row in TMEM */
    u16 tmem;          /* TMEM address in 64-bit words */
    u16 uls, ult, lrs, lrt; /* from G_SETTILESIZE, 10.2 fixed point */
} GfxTile;

typedef struct GfxRdp {
    u32 otherH, otherL;
    u32 mux0, mux1;
    u32 geometryMode;
    float prim[4], env[4], fog[4], blend[4];
    float primLod;
    u32 fillColor;
    u32 primDepth;     /* 15-bit screen depth from G_SETPRIMDEPTH */
    u16 scissor[4];    /* ulx, uly, lrx, lry in 10.2 fixed point */
    GfxTile tiles[8];
    int texOn, texTile; /* from G_TEXTURE */
} GfxRdp;

extern GfxRdp gfxRdp;

void gfx_dl_set_framebuffers(u32 cfb0, u32 cfb1);
void gfx_dl_begin_frame(void);
void gfx_dl_record(const GfxTask *tasks, int count, int cut);
void gfx_dl_run_task(u32 addr, int sprite2d);
/* Widescreen: the factor by which the 3D view is widened (1 for none, see GFX_WIDE_HORPLUS). The display list
   interpreter then draws the picture's x coordinates 1/wide as wide, around the middle of the frame buffer. */
void gfx_dl_set_wide(float wide);

/* The camera of the latest scene (gfx_dl.c) and the debug lines (gfx_debug.c). */
extern GfxSceneView gfxScene;
void gfx_debug_draw(GLuint fbo, int width, int height);
void gfx_debug_shutdown(void);

/* ---- The layer (gfx.c, gfx_draw_layer): which primitives a layer draws. */
typedef struct GfxLayerState {
    int on;               /* 1 while a layer is drawn */
    const GfxMark *marks;
    int count;
    unsigned int mask;    /* the GFX_LAYER_ bits to draw */
    int noFog;
    unsigned int kind;    /* the layer of the command that runs now */
    int next;             /* the next mark */
} GfxLayerState;

extern GfxLayerState gfxLayer;

/* ---- The host's lists (gfx_hostgfx.c). gfx_dl_host_walk walks the F3DEX tasks without drawing and reports each
   command: gfx_host_task before a task's list, gfx_host_call before a list that a G_DL calls and gfx_host_return
   after it (w0 the G_DL's first word, or 0 at the end of a task's list), gfx_host_texrect for a texture rectangle, and
   gfx_host_command for every other command (pc the address of the command after it). */
void gfx_dl_host_walk(const GfxTask *tasks, int count);
/* The address an RSP command points to, with the segments set so far. */
u32 gfx_dl_seg_addr(u32 addr);
/* The matrix of the latest G_MTX, after the interpolation: 1 when the interpolation changed it, 0 when it is the one in
   memory, -1 when it could not be read. */
int gfx_dl_last_matrix(float *m);
/* 0: G_CULLDL never ends a list (the host's recording pass, which must follow the lists its walk follows); 1: as the
   microcode does. */
void gfx_dl_set_cull(int on);
/* The modelview matrix on top of the stack, after the interpolation. */
void gfx_dl_modelview(float *m);
void gfx_host_task(int index);
void gfx_host_call(int depth);
void gfx_host_return(u32 w0, int depth);
void gfx_host_command(u32 op, u32 w0, u32 w1, u32 pc);
/* A G_TEXRECT or G_TEXRECTFLIP, after the interpolation: its corners in pixels of the 320x240 screen, its depth in clip
   space, its tile, its texture coordinates at the upper left corner and their steps a pixel. */
void gfx_host_texrect(float ulx, float uly, float lrx, float lry, float z, int tile, float s, float t, float dsdx,
                      float dtdy, int flip);
/* The projection the game's commands have loaded so far (row-major, for row vectors) and the viewport in pixels of the
   320x240 screen. */
void gfx_dl_view(float *proj, float *vpScale, float *vpTrans);

/* ---- Interpolation between game frames (gfx_interp.c). */

/* 0: off; 1: recording a frame's matrices; 2: drawing a frame between two game frames. */
extern int gfxInterpMode;
extern u32 gfx_interp_track;
float gfx_interp_time(void);
int gfx_interp_is_volatile(u32 addr);
u32 gfx_interp_mix(u32 h, u32 v);
void gfx_interp_begin_record(int cut);
void gfx_interp_end_record(void);
int gfx_interp_record_mtx(u32 key, u32 lkey, int parent, int kind, const float *m);
int gfx_interp_draw_mtx(u32 key, int parent, int kind, float *m);
/* What a 3D sprite's rectangle needs to be moved with the camera: the view-projection matrix in use, the viewport
   (scale and translation in pixels) and the rectangle's depth in clip space. */
typedef struct GfxRectView {
    float vp[16];
    float vpScale[2], vpTrans[2];
    float z;
} GfxRectView;

void gfx_interp_record_rect(u32 key, u32 lkey, const float *r, const GfxRectView *view);
int gfx_interp_draw_rect(u32 key, float *r, const GfxRectView *view, float *z);
void gfx_interp_viewport(float *scale, float *trans, int recording);
int gfx_interp_begin_draw(void);
void gfx_interp_end_draw(void);
void gfx_interp_shutdown(void);

/* ---- Textures (gfx_tex.c). */

/* A texture as it is decoded from memory: texel (s, t) of the tile is at addr + t * stride + s * size. */
typedef struct GfxTexDesc {
    u32 addr;
    u32 stride;
    u16 width, height;
    u8 fmt, siz;
    u8 palType;        /* TEX_LUT: 0 none, 2 RGBA16, 3 IA16 */
    u8 pad;
    u32 pal[16];       /* addresses of the 16 blocks of 16 palette entries; a 4-bit texture uses pal[0] */
} GfxTexDesc;

/* How the shader turns a texture coordinate into texels, as the RDP does (see sampleTile in gfx_render.c). */
typedef struct GfxSampler {
    float a[4]; /* shift factor s, t; tile origin s, t (texels) */
    float b[4]; /* clamp threshold s, t (quarter texels, before the origin is subtracted); last texel s, t */
    int c[4];   /* wrap period s, t (0 for none); flags s, t: 1 clamp, 2 mirror */
} GfxSampler;

void gfx_tex_begin_frame(void);
/* gfx_tmem.c: the frame count of the load records, and a texel's size in bytes times 2 (a 4-bit texel is half a byte). */
u32 gfx_tex_frame(void);
u32 gfx_tex_size_x2(u32 siz);
void gfx_tex_set_image(u32 fmt, u32 siz, u32 width, u32 addr);
void gfx_tex_load_block(int tile, u32 uls, u32 ult, u32 lrs, u32 dxt);
void gfx_tex_load_tile(int tile, u32 uls, u32 ult, u32 lrs, u32 lrt);
void gfx_tex_load_tlut(int tile, u32 count);
/* Describes what a tile samples. Returns 0 when nothing was loaded for it. */
int gfx_tex_describe_tile(int tile, GfxTexDesc *desc, GfxSampler *samp);
/* Fills a sampler that clamps a whole image of width x height texels (for Sprite2D). */
void gfx_tex_clamp_sampler(GfxSampler *samp, int width, int height);
/* The OpenGL texture for a description, decoded again when its texels or palette changed since the last frame. */
GLuint gfx_tex_get(const GfxTexDesc *desc);
void gfx_tex_end_frame(void);
void gfx_tex_shutdown(void);

/* ---- Drawing (gfx_render.c). */

/* A vertex as the GPU gets it: clip-space position (the N64 viewport already applied, for a 320x240 screen),
   shade colour, and texture coordinates in texels. With G_FOG, the shade alpha holds the fog amount, as the RSP
   puts it there. */
typedef struct GfxVertex {
    float x, y, z, w;
    float r, g, b, a;
    float s, t;
} GfxVertex;

/* What a primitive is: it decides which state applies. */
enum {
    GFX_PRIM_TRIANGLE, /* F3DEX triangles: depth from G_ZBUFFER, texture tile from G_TEXTURE */
    GFX_PRIM_TEXRECT,  /* texture rectangles: depth only from G_ZS_PRIM */
    GFX_PRIM_RECT,     /* fill rectangles in 1-cycle and 2-cycle mode */
    GFX_PRIM_FILL      /* fill rectangles in fill mode: the fill colour, no blending, no depth */
};

int gfx_render_init(void);
void gfx_render_shutdown(void);
/* Tells the draw states whether the frame buffer is multisampled (for alpha to coverage). */
void gfx_render_set_msaa(int on);
/* Sets the texture filter for the following frames: GFX_FILTER_N64, GFX_FILTER_BILINEAR or GFX_FILTER_NEAREST. */
void gfx_render_set_filter(int filter);
/* Starts recording a frame drawn at width x height pixels. The N64's 320x240 screen is drawn in the middle of
   the frame, width / wide pixels wide and the full height. */
void gfx_render_begin(int width, int height, float wide);
/* Plays the recorded frame into the bound framebuffer. */
void gfx_render_end(void);
/* Plays the recorded frame of a layer again into the bound framebuffer's depth buffer only: every drawn pixel, the
   nearest wins. A pixel that the colour pass would leave without coverage (alpha under 1/255, or black where it
   adds) writes no depth. */
void gfx_render_depth_pass(void);
/* The composite of a layer's textures (gfx_composite_layer). */
int gfx_render_composite(unsigned int color, unsigned int depth, int texW, int texH, int w, int h,
                         const float planes[2], const float projZ[2], float bias, int writeDepth);
/* Marks the RDP or RSP state as changed: the next primitive builds its draw state again. */
void gfx_state_changed(void);
/* Starts or continues a batch for a primitive. tile is the texture rectangle's tile; direct, when not NULL, is a
   texture that comes straight from memory (Sprite2D). wide is 1 for a primitive that may reach beyond the 4:3
   area when the view is widened: its scissor then covers the whole frame width where it covers the N64's screen
   width. Returns 0 when the primitive must be dropped. */
int gfx_prepare(int kind, int tile, const GfxTexDesc *direct, const GfxSampler *directSampler, int wide);
void gfx_add_vertices(const GfxVertex *v, int count);
void gfx_clear_color(const float rgba[4]);
void gfx_clear_depth(void);

/* Statistics of the last frame. */
typedef struct GfxStats {
    int triangles, rects, draws, states, programs, textureUploads;
} GfxStats;
extern GfxStats gfxStats;

#endif
