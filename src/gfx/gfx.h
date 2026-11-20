/* The renderer: draws the display lists the game builds with OpenGL 3.3 core.

   It interprets the F3DEX 1.21 and Sprite2D microcode commands and the RDP commands they carry. The CPU does the
   RSP's work (vertex transform, lighting, fog, clipping, culling). Shaders do the RDP's colour combiner, texture
   sampling and blender; OpenGL state does depth, scissor and blending with the frame buffer.

   The game's 320x240 frame is drawn into an offscreen frame buffer, which is then scaled to the window. The
   options of gfx_set_options choose its size (a share of the window's), its anti-aliasing, the texture filter and
   what happens when the window is wider than 4:3: black bars (the default), a stretched picture, or a wider view.

   Call order: gfx_init once the GL context is current; gfx_set_framebuffers (and gfx_set_memory_range, and
   gfx_set_memory_base if the display lists hold offsets rather than host pointers) once the game is set up;
   then gfx_draw_frame once per drawn frame, before the buffer swap; gfx_shutdown before the context is destroyed.

   gfx_draw_frame sets every piece of OpenGL state it uses. It returns with the target framebuffer bound (0 unless
   gfx_set_target chose another), the viewport on the whole drawable, no program, vertex array or scissor test, and
   depth testing and blending off.

   Environment variables: GLOVER_MSAA (samples per pixel, 0 for none) and GLOVER_GFX_FILTER=bilinear (4-point
   instead of the N64's 3-point bilinear filter) win over the options. GLOVER_GFX_STATS logs each frame's
   statistics and GLOVER_GFX_TRACE prints every draw and texture. */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One RSP task: the address and size in bytes of its display list, and 1 when it runs the Sprite2D microcode. */
typedef struct GfxTask {
    unsigned int addr, size;
    int sprite2d;
} GfxTask;

/* Needs a current OpenGL 3.3 core context, and the loader of its functions (gfx_set_loader, gfx_host.h). When OpenGL
   lacks a function the renderer needs, it logs the problem, returns -1, and gfx_draw_frame draws nothing. Returns 0
   otherwise. */
int gfx_init(void);

/* How the picture is drawn. */
enum { GFX_FILTER_N64, GFX_FILTER_BILINEAR, GFX_FILTER_NEAREST };
/* GFX_WIDE_OFF: black bars at the sides of a window wider than 4:3. GFX_WIDE_STRETCH: the 4:3 picture is stretched
   to the window. GFX_WIDE_HORPLUS: the 3D view is widened to the window's shape (the horizontal field of view
   grows). The 2D parts (the HUD, menus and texture rectangles) keep their 4:3 shape in the middle. A window
   narrower than 4:3 gets bars above and below in the OFF and HORPLUS modes. */
enum { GFX_WIDE_OFF, GFX_WIDE_STRETCH, GFX_WIDE_HORPLUS };

typedef struct GfxOptions {
    int scale;      /* the frame buffer's size in percent of the window's (50 to 400) */
    int msaa;       /* samples per pixel: 0 (none), 2, 4 or 8; a larger value is cut to what the GPU allows */
    int filter;     /* GFX_FILTER_ */
    int widescreen; /* GFX_WIDE_ */
} GfxOptions;

/* The options in force before gfx_set_options is called: 100, 4 samples, the N64 filter, no widescreen. A change
   takes effect with the next gfx_draw_frame. */
void gfx_set_options(const GfxOptions *options);

/* Tells the renderer the window's size ahead of gfx_draw_frame, which also sets it. gfx_interp_record needs it to
   widen the view as the drawing will. */
void gfx_set_drawable_size(int drawableW, int drawableH);

/* The most samples per pixel that the GPU allows (0 when it has no multisampling). Valid after gfx_init. */
int gfx_max_samples(void);

/* The addresses of the game's two colour images (cfb_16_a and cfb_16_b). A fill of any other colour image clears the
   depth buffer. */
void gfx_set_framebuffers(unsigned int cfb0, unsigned int cfb1);

/* Added to every address the display lists hold: PORT_PTR_DELTA (ptr32.h). It is 0 on the normal builds, where
   the addresses are host pointers. */
void gfx_set_memory_base(uintptr_t base);

/* The addresses the renderer may read: [lo, hi), before the base is added. Some of the game's display lists
   point at memory that does not exist (the N64 reads junk there). Without a range (hi == 0, the default), the
   renderer asks the system which pages exist, once per page. With the range of the game's memory (port_ram), it
   checks less and reads nothing outside it. */
void gfx_set_memory_range(unsigned int lo, unsigned int hi);

/* The framebuffer that gfx_draw_frame draws into and leaves bound, and that gfx_read_window reads: 0 (the window's
   default framebuffer) unless set. */
void gfx_set_target(unsigned int fbo);

/* Draws one frame into the target framebuffer (gfx_set_target), which is drawableW x drawableH pixels. With count == 0 it shows
   the last frame again. Swap the window's buffers afterwards. */
void gfx_draw_frame(const GfxTask *tasks, int count, int drawableW, int drawableH);

/* Reads back the last frame at its own resolution (the frame buffer, without the window's bars), as RGBA rows from
   the top. Sets *w and *h. Returns a buffer the caller frees with free(), or NULL. For tests and screenshots. */
unsigned char *gfx_read_frame(int *w, int *h);

/* Reads back the window as the last gfx_draw_frame left it (before the buffer swap), the same way. */
unsigned char *gfx_read_window(int *w, int *h);

/* Interpolation between game frames (docs/interpolation.md). Call gfx_interp_record once for each new game frame,
   right after the game built its display lists and before the next game update. It reads the lists without
   drawing and keeps their matrices. cut is 1 when the frame does not follow the previous one (a level or mode
   change). gfx_interp_set(t) then makes the following gfx_draw_frame calls draw the picture a fraction t (0 to 1)
   of the way from the previous game frame to the latest; a t below 0 or at 1 or more draws the latest frame as it
   is. The display lists must be unchanged until the next game update. */
typedef struct GfxInterpStats {
    int total, blended, moved, loose, noPartner, cut;
    int rects, rectsBlended;
} GfxInterpStats;

void gfx_interp_record(const GfxTask *tasks, int count, int cut);
void gfx_interp_set(float t);
/* The addresses of the buffers that the game rebuilds in every frame (its dynamic display list and matrices): a
   matrix is not named by an address in them. */
void gfx_interp_set_volatile(unsigned int lo, unsigned int hi);
/* What the latest gfx_interp_record found. */
void gfx_interp_stats(GfxInterpStats *s);

/* The camera of the latest drawn 3D scene, for overlays that follow the picture (the Developer menu's collision and
   object names). It is the matrix that the game's SetupViewing builds, as the renderer used it for the latest
   gfx_draw_frame: the interpolated one when gfx_interp_set blended two game frames. */
typedef struct GfxSceneView {
    int valid;                     /* 0 until a frame with a 3D scene was drawn */
    float vp[16];                  /* world to clip space, row-major, for the row vector (x, y, z, 1) */
    float vpScale[2], vpTrans[2];  /* the N64 viewport in pixels of the game's 320x240 screen */
    float frameX, frameY, frameW, frameH; /* where the picture lies in the window, as fractions of the drawable */
} GfxSceneView;

int gfx_scene_view(GfxSceneView *v);
/* The place of a world point in the window, as fractions of the drawable (x right, y down), and its depth in the
   game's clip space (-1 near, 1 far). Returns 0 when the point is behind the camera. */
int gfx_scene_project(const GfxSceneView *v, float x, float y, float z, float *sx, float *sy, float *depth);

/* A line in world space with an RGBA colour (red in the lowest byte). */
typedef struct GfxDebugLine {
    float a[3], b[3];
    unsigned int color;
} GfxDebugLine;

/* Lines for the following gfx_draw_frame calls, drawn over the 3D scene (until the next call). With depth set a
   line is hidden where the scene's surface is in front of it. Nothing is drawn when no 3D scene was drawn. */
void gfx_debug_lines(const GfxDebugLine *lines, int count, int depth);

/* ---- The layer: a part of the picture drawn for a host to put over its own (docs/library.md). */

/* The marks of a frame's lists (src/port/drawmark.h): from the command at addr on, the commands belong to layer, one
   GFX_LAYER_ bit, or 0 for the frame's setup. */
typedef struct GfxMark {
    unsigned int addr, layer;
} GfxMark;
#define GFX_LAYER_ACTORS 0x1
#define GFX_LAYER_SCENERY 0x2
#define GFX_LAYER_SKY 0x4
#define GFX_LAYER_HUD 0x8

/* What gfx_draw_layer leaves: the textures (owned by the renderer, valid until the next gfx_draw_layer or
   gfx_shutdown) and their size. */
typedef struct GfxLayerOut {
    unsigned int colorTexture;   /* RGBA8, premultiplied alpha, rows from the bottom */
    unsigned int depthTexture;   /* DEPTH_COMPONENT32F: the window depth of the nearest drawn pixel, 1 elsewhere */
    int width, height;
} GfxLayerOut;

/* Draws the parts of the frame whose mark's layer is in mask, into textures of the frame buffer's size for a
   drawable of drawableW x drawableH (the resolution scale applies), with the 4:3 screen stretched over all of it.
   noFog 1 draws without the game's fog. The other primitives are left out; the state commands still run. A colour
   clear outside the mask clears to 0, 0, 0, 0. Returns 0 when it could not draw. */
int gfx_draw_layer(const GfxTask *tasks, int count, const GfxMark *marks, int markCount, unsigned int mask,
                   int noFog, int drawableW, int drawableH, GfxLayerOut *out);
/* Draws the latest layer over the bound framebuffer's w x h pixels: premultiplied blending, the depth test LEQUAL
   against the framebuffer's depth. The layer's depth comes back to view depth with projZ, the terms m[2][2] and
   m[3][2] of the projection it was drawn with, and goes into the framebuffer's projection with the planes
   planes[0] (near) and planes[1] (far). Each layer pixel is pulled towards the eye by bias, a share of its view depth.
   writeDepth 1 also writes the depth of the pixels whose alpha is at least 0.5. Returns 0 without a layer. */
int gfx_composite_layer(int w, int h, const float planes[2], const float projZ[2], float bias, int writeDepth);

void gfx_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
