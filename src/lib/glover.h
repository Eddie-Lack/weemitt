/* glover.h: Glover (USA, NGVE) as a library with no window (docs/library.md). The host gives the ROM; the library
   holds nothing of it. One game per process. Calls come from one thread at a time; only glover_audio_read and
   glover_audio_rate may run on one other thread at the same time. Plain C, fixed-width types, no enum in a structure. A structure that may
   grow starts with its size in bytes. */
#ifndef GLOVER_H
#define GLOVER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(_WIN32)
#  if defined(GLOVER_BUILD) && defined(__GNUC__) && !defined(_WIN64)
     /* A 32-bit Windows host may call with a stack aligned on 4 bytes only; the library's SSE code needs 16. */
#    define GLOVER_API __declspec(dllexport) __attribute__((force_align_arg_pointer))
#  elif defined(GLOVER_BUILD)
#    define GLOVER_API __declspec(dllexport)
#  else
#    define GLOVER_API __declspec(dllimport)
#  endif
#else
#  define GLOVER_API __attribute__((visibility("default")))
#endif

#define GLOVER_API_VERSION 4   /* glover_create also accepts 1 to 3 */

#define GLOVER_OK 0
#define GLOVER_ERR_ARG (-1)          /* a bad argument or structure size */
#define GLOVER_ERR_VERSION (-2)      /* the host was built for another GLOVER_API_VERSION */
#define GLOVER_ERR_ROM (-3)          /* not Glover (USA, NGVE) */
#define GLOVER_ERR_BUSY (-4)         /* a game was already created in this process; load the library again */
#define GLOVER_ERR_FATAL (-5)        /* the game stopped in this call; glover_error has the reason */
#define GLOVER_ERR_DEAD (-6)         /* the game stopped earlier (glover_destroy or a fatal error) */
#define GLOVER_ERR_REENTER (-7)      /* called from a callback, or from two threads at once */
#define GLOVER_ERR_UNSUPPORTED (-8)  /* not in this build (the renderer) */
#define GLOVER_ERR_GL (-9)           /* OpenGL 3.3 functions are missing */
#define GLOVER_ERR_MEMORY (-10)      /* glover_create could not allocate its copy of the ROM, or glover_host_gfx its
                                        lists */

typedef struct Glover Glover;

typedef struct GloverConfig {
    uint32_t size;                 /* sizeof(GloverConfig) */
    uint32_t apiVersion;           /* GLOVER_API_VERSION */
    const uint8_t *rom;            /* .z64, .v64 or .n64 byte order; the library copies it */
    uint32_t romSize;
    const uint8_t *eeprom;         /* 512 bytes, or NULL: all zero, as weemitt without a save file */
    int32_t startLevel;            /* -1: power-on; 0 to 47: as weemitt's --level */
    void *user;
    void (*log)(void *user, const char *line);   /* NULL: silent. Must not call the library */
    uint64_t (*clockUs)(void *user);  /* the game's profiler clock. NULL: the library's game clock, which
                                         adds dt * retraces / 60 seconds after each update */
    int32_t ballworld;             /* 1: the Ballworld mode, every level played as the ball alone (weemitt's
                                      --ballworld); 0: off. A size that ends before it: 0 */
} GloverConfig;

GLOVER_API uint32_t glover_api_version(void);
GLOVER_API const char *glover_build_id(void);          /* git describe, system, sound player, renderer */
GLOVER_API void glover_default_config(GloverConfig *c);
/* err gets the reason of an error, or "". When the game stops while it starts, the result is GLOVER_ERR_FATAL, *out
   is NULL, and the library's copy of the ROM is freed. A second glover_create in the same process returns
   GLOVER_ERR_BUSY, also after a failed start: the game's state is in the library's globals. A new game needs the
   library unloaded and loaded again (dlclose and dlopen, FreeLibrary and LoadLibrary). */
GLOVER_API int32_t glover_create(const GloverConfig *c, Glover **out, char *err, uint32_t errSize);
/* Ends the game and frees the ROM's copy. Later calls with g return GLOVER_ERR_DEAD; g stays a valid handle.
   GLOVER_ERR_REENTER while another call runs. */
GLOVER_API int32_t glover_destroy(Glover *g);
GLOVER_API const char *glover_error(Glover *g);         /* the fatal message, or "" */

/* Controller 1: libultra's CONT_ bits and the stick (about -80 to 80). */
typedef struct GloverInput { uint16_t buttons; int8_t stickX, stickY; } GloverInput;
#define GLOVER_BTN_CR 0x0001
#define GLOVER_BTN_CL 0x0002
#define GLOVER_BTN_CD 0x0004
#define GLOVER_BTN_CU 0x0008
#define GLOVER_BTN_R 0x0010
#define GLOVER_BTN_L 0x0020
#define GLOVER_BTN_DR 0x0100
#define GLOVER_BTN_DL 0x0200
#define GLOVER_BTN_DD 0x0400
#define GLOVER_BTN_DU 0x0800
#define GLOVER_BTN_START 0x1000
#define GLOVER_BTN_Z 0x2000
#define GLOVER_BTN_B 0x4000
#define GLOVER_BTN_A 0x8000

#define GLOVER_MAX_LISTS 16
typedef struct GloverList { uint32_t addr, size; int32_t sprite2d; } GloverList;   /* = PortFrameTask */
typedef struct GloverFrame {
    uint32_t size;
    int32_t listCount;             /* -1: no new lists; the latest stay valid */
    GloverList lists[GLOVER_MAX_LISTS];
    int32_t ticked;                /* an original frame ended in this update */
    int32_t cut;                   /* with ticked: this frame does not follow the last (port_game_cut) */
} GloverFrame;
/* Runs dt original frames, 0 < dt <= 1 (docs/delta-time.md). At dt 1 the logic is exactly the N64's. During a
   recorded demo (GloverStatus.demo) pass 1. in may be NULL (no buttons, stick centred). out may be NULL. */
GLOVER_API int32_t glover_update(Glover *g, float dt, const GloverInput *in, GloverFrame *out);

typedef struct GloverStatus {
    uint32_t size;
    int32_t mode, level, demo;
    int32_t retraces;              /* 60ths of a second per original frame: 3 in a level, 2 in the menus */
    int32_t gotoPossible;
    uint32_t frames;               /* original frames since glover_create */
    uint16_t buttonsRead, reserved;/* the word the game's latest controller read got */
    uint32_t controllerReads;
    uint32_t audioQueued, audioDropped;
} GloverStatus;
GLOVER_API int32_t glover_status(Glover *g, GloverStatus *s);

/* Grows at its end. A size of 40 (version 1: up to camera) gets only that part. */
typedef struct GloverBodies {
    uint32_t size;
    float hand[3], ball[3], camera[3];   /* positions, game units */
    float handVel[3], ballVel[3];        /* per original frame: the game's own velocity. The carry by a conveyor, a
                                            platform or a body is not in it. */
    float handYaw;                       /* radians, the game's rot.y */
    int32_t handAction;                  /* the glove's action (hand.state->mode), or -1 */
    int32_t ballHeld;                    /* 1: the glove holds the ball */
    int32_t handOnGround, ballOnGround;
    int32_t handTerrain, ballTerrain;    /* the terrain of the first contact plane, or -1 */
    int32_t ready;                       /* in a level, the glove set up and no restart running */
    uint32_t restarts;                   /* times the game began a restart of the glove at its checkpoint */
    /* The voids (GLOVER_MESH_VOID): 0; 1: the floor under it is a void triangle; 2: it touches one. */
    int32_t handVoid, ballVoid;
    uint32_t handVoids, ballVoids;       /* times it began to touch a void triangle */
    float handSafe[3], ballSafe[3];      /* where it last stood on a static triangle that is not a void, 6 or 9 */
    int32_t safeKnown;                   /* bit 0: handSafe holds a place; bit 1: ballSafe */
    /* Version 3: the game's waterState. 0: not in water; 1 paddling, 2 wading, 3 swimming, 4 floating. */
    int32_t handWater, ballWater;
} GloverBodies;
GLOVER_API int32_t glover_bodies(Glover *g, GloverBodies *b);

/* ---- The host's world (docs/host-world.md). The library does not convert units: the host scales its world into
   game units, the same for meshes, poses, places and views. */
#define GLOVER_TERRAIN_COUNT 14            /* the game's terrain table: 0 to 13 */
#define GLOVER_MESH_CAMERA_PASS 0x1        /* the game's camera passes through these triangles */
/* A void: the glove or the ball that touches it is reported in glover_bodies, and the host places it. The triangle
   collides with its terrain as any other. Over a void the game's fall damage and the ball's burst after a long fall
   do not happen. */
#define GLOVER_MESH_VOID 0x2

/* A host built for version 2 or earlier gives a GloverMesh that ends before conveyor: it has no conveyors. */
typedef struct GloverMesh {
    uint32_t size;                         /* sizeof(GloverMesh) */
    uint32_t flags;                        /* GLOVER_MESH_ bits for every triangle */
    const float *verts;                    /* x, y, z per vertex, game units, y up */
    uint32_t vertCount;
    const uint32_t *indices;               /* 3 per triangle, counter-clockwise seen from the front */
    const uint8_t *terrain;                /* 1 per triangle, 0 to 13; NULL: all 0 */
    const uint8_t *triFlags;               /* 1 per triangle, GLOVER_MESH_ bits; NULL: none */
    uint32_t triCount;
    /* Conveyors, for glover_coll_set_static only (a body's mesh with conveyor set: GLOVER_ERR_ARG): what stands on a
       conveyor triangle moves by its conveyor's velocity, x, y, z in game units per original frame, each within
       100. */
    const uint16_t *conveyor;              /* 1 per triangle: 0 none, k: conveyors[3 (k - 1)]; NULL: none */
    const float *conveyors;                /* x, y, z per conveyor */
    uint32_t conveyorCount;                /* at most GLOVER_MAX_CONVEYORS */
} GloverMesh;
#define GLOVER_MAX_CONVEYORS 4096

typedef struct GloverCollOptions {
    uint32_t size;
    float cellSize;                        /* the edge of a chunk's cell in game units; 0: 256 */
    uint32_t maxChunkTris;                 /* 0: 1024; at most 32767 */
    float linkRadius;                      /* 0: every chunk in the game's collision list; else only the chunks
                                              within this of the glove, the ball and the camera */
    uint32_t keepLevel;                    /* 1: keep the level's own landscape, water and platforms */
    uint32_t levelOff;                     /* GLOVER_OFF_ bits: the level's own things switched off while the
                                              static world is in; a size that ends before it: 0 */
} GloverCollOptions;
#define GLOVER_OFF_ENEMIES 0x1                 /* the level's enemies */
#define GLOVER_OFF_COLLECTABLES 0x2            /* its garibs, extra lives and potions */
#define GLOVER_OFF_AMBIENT 0x4                 /* its sounds at points: ambient, such as its waterfalls, and its puzzles' */

typedef struct GloverPose {
    float pos[3];
    float rot[4];                          /* a unit quaternion x, y, z, w, as the game's Quat */
    float scale[3];
} GloverPose;

typedef struct GloverCollStats {
    uint32_t size;
    uint32_t staticTris, chunks, linkedChunks, bodies;
    uint32_t heapUsed;                     /* bytes of the game's heap that the host world holds */
    uint32_t heapFree;                     /* the free gap of the game's heap */
    uint32_t rebuilds;                     /* times the host world was built again after a level reload */
    uint32_t pending;                      /* 1 while a static set or a body waits for the glove to be ready */
    uint32_t failed;                       /* waiting installs that then found too little memory */
    /* Version 3. A size of 40 gets the part up to failed. */
    uint32_t conveyorTris;                 /* static triangles on a conveyor */
    uint32_t conveyorPlatforms;            /* conveyor chunks of the static world, and bodies with a conveyor */
    uint32_t waters;                       /* the host's water volumes */
} GloverCollStats;

/* These run between updates. GLOVER_ERR_ARG: a terrain above 13, an unknown flag or levelOff bit, an index out of
   range, a coordinate that is not finite or not within 100000, a scale of 0, a quaternion that is not of unit
   length, too many triangles. GLOVER_ERR_MEMORY: the game's heap would keep less than 2 MB free. GLOVER_ERR_BUSY:
   the game is not in a level, or the glove is not ready (GloverBodies.ready); the library keeps the request and
   makes it at the first ready update. */
/* Replaces the static world: at most 65536 triangles. The library copies the mesh. NULL m: no static world. */
GLOVER_API int32_t glover_coll_set_static(Glover *g, const GloverMesh *m, const GloverCollOptions *o);
/* A moving body (at most 256, each at most 4096 triangles): its mesh in its own space, and its first pose. *id gets
   its handle, never 0, also with GLOVER_ERR_BUSY. */
GLOVER_API int32_t glover_coll_body_add(Glover *g, const GloverMesh *m, const GloverPose *p, uint32_t *id);
/* A volume of water: the game floats, slows and carries what is under its surface, and the glove swims in it. */
typedef struct GloverWater {
    float min[3];                          /* the volume's lowest corner, game units */
    float size[3];                         /* x, y, z extent, each above 0 */
    float surface;                         /* min[1] <= surface <= min[1] + size[1], within 0.01 */
    float current[3];                      /* added to the velocity of what is under the surface, per original frame,
                                              each within 100 */
} GloverWater;
#define GLOVER_MAX_WATERS 256
/* Replaces the host's waters with count entries, elemSize bytes apart (at least sizeof(GloverWater)). NULL or 0: none.
   The game's box reaches at least 25 units below and above the surface, as for the level's own waters. */
GLOVER_API int32_t glover_coll_set_water(Glover *g, const GloverWater *w, uint32_t count, uint32_t elemSize);
/* The pose the body has at the end of the next update. */
GLOVER_API int32_t glover_coll_body_move(Glover *g, uint32_t id, const GloverPose *p);
GLOVER_API int32_t glover_coll_body_remove(Glover *g, uint32_t id);
/* A conveyor on a body: vel in the body's own space, game units per original frame, each within 100; it turns with the
   body. NULL or 0, 0, 0: none. GLOVER_ERR_BUSY while the body waits to be built; it gets the conveyor then. */
GLOVER_API int32_t glover_coll_body_conveyor(Glover *g, uint32_t id, const float vel[3]);
/* Removes all host collision and the respawn, and gives the level its own collision back. The host world stays
   when the game goes to another level: call this before glover_goto_level to a level of the game's own. */
GLOVER_API int32_t glover_coll_clear(Glover *g);
GLOVER_API int32_t glover_coll_stats(Glover *g, GloverCollStats *s);

#define GLOVER_PLACE_HAND 0x01
#define GLOVER_PLACE_BALL 0x02
#define GLOVER_PLACE_VEL 0x04                  /* also set the velocities (else 0) */
#define GLOVER_PLACE_YAW 0x08
#define GLOVER_PLACE_CAMERA 0x10               /* restart the game's camera behind the glove */
typedef struct GloverPlace {
    uint32_t size, what;
    float hand[3], ball[3], handVel[3], ballVel[3];
    float handYaw;                         /* radians, the game's rot.y */
} GloverPlace;
/* At the first ready update from the next one on. A level reload puts the glove and the ball at the level's start
   again: place them again when GloverCollStats.rebuilds rises. While the glove holds the ball, a place of one of
   the two moves both, and a place of both makes the glove let go. */
GLOVER_API int32_t glover_place(Glover *g, const GloverPlace *p);
/* Where a restart puts the glove: the game's checkpoint moves there, at the first ready update with a checkpoint. It
   goes in again after each level reload, until glover_coll_clear. */
GLOVER_API int32_t glover_set_respawn(Glover *g, const float pos[3], float yaw);

/* ---- A camera set by the host. */
#define GLOVER_VIEW_DRAW 0x1     /* draw from this view: the game's projection and view matrices are built from it */
#define GLOVER_VIEW_STEER 0x2    /* the game's camera is this view: the controls, the sound and the distance tests
                                    follow it */
/* For tests only: draw through the host's path with the game's own camera, which gives the game's own picture. The
   view's other fields must still be valid. */
#define GLOVER_VIEW_TEST_GAME 0x80000000u
typedef struct GloverView {
    uint32_t size, flags;
    float eye[3], at[3], up[3];            /* game units */
    float fovy;                            /* degrees, as guPerspective */
    float aspect;                          /* width / height */
    float nearZ, farZ;
} GloverView;
/* From the next update until it changes. NULL (or flags 0): the game's own camera. GLOVER_ERR_ARG also for an up
   vector parallel to the direction of view. With GLOVER_VIEW_DRAW, draw with glover_gl_options widescreen 0 or 1:
   mode 2 gives GLOVER_ERR_ARG while the view is set, and the view does too while mode 2 is on. The game still leaves
   out the actors farther than its far distance from its own camera, which is at the eye only with GLOVER_VIEW_STEER.
   With GLOVER_VIEW_STEER the game's camera logic does not run: its pending changes of the camera wait until the view
   goes. */
GLOVER_API int32_t glover_set_view(Glover *g, const GloverView *v);

#define GLOVER_OPT_EASY_BALL 1
#define GLOVER_OPT_CAMERA_FOLLOW 2   /* 1.0 to 3.0 */
#define GLOVER_OPT_SKIP_INTRO 3
#define GLOVER_OPT_PAUSE_CHEATS 4    /* 1 by default, as in weemitt */
#define GLOVER_OPT_CHEAT_HOLD 5      /* a mask of the game's cheat bits */
#define GLOVER_OPT_BALLWORLD 6       /* GloverConfig.ballworld; glover_get_option only */
#define GLOVER_OPT_MUSIC_VOLUME 7    /* 0 to 100 percent, 100 by default: the songs in the sound frames */
#define GLOVER_OPT_EFFECTS_VOLUME 8  /* 0 to 100 percent, 100 by default: the sound effects */
#define GLOVER_OPT_CAMERA_DISTANCE_SCALE 9   /* 0.01 to 100, 1 by default: multiplies the camera's distances in the
                                                sound, the wakes, the debris and the shadows (library.md, "Sound") */
GLOVER_API int32_t glover_set_option(Glover *g, int32_t option, double value);   /* from the next update */
GLOVER_API int32_t glover_get_option(Glover *g, int32_t option, double *value);
/* Starts level 0 to 42 at the next update. GLOVER_ERR_ARG for another level, or where gotoPossible is 0. */
GLOVER_API int32_t glover_goto_level(Glover *g, int32_t level);

/* The parts of the picture (version 3). */
#define GLOVER_LAYER_ACTORS 0x1    /* the glove, the ball, enemies, collectables, bullets, debris, sprites, shadows,
                                      footprints */
#define GLOVER_LAYER_SCENERY 0x2   /* the level's LANDSCAPE_ACTOR, WATER_ACTOR and PLAT_ACTOR actors */
#define GLOVER_LAYER_SKY 0x4       /* the backdrops */
#define GLOVER_LAYER_HUD 0x8       /* the overlays, the text, the fade */
/* For hosts that read the display lists themselves: from the command at word addr on (as GloverList.addr), the
   latest lists' commands belong to layer, one GLOVER_LAYER_ bit, or 0: the frame's setup (the clears, the matrices,
   the modes). The marks come in the order of their addresses; a list that a command calls belongs to that command's
   layer. A frame has at most GLOVER_MAX_DRAW_MARKS marks. When its actors need more, the rest of them form one mark
   with the layer GLOVER_LAYER_ACTORS | GLOVER_LAYER_SCENERY, and the HUD still gets its own. */
typedef struct GloverDrawMark { uint32_t addr; uint32_t layer; } GloverDrawMark;
#define GLOVER_MAX_DRAW_MARKS 4096
/* The marks of the latest lists (those of glover_update's latest GloverFrame with listCount >= 0): up to max of them
   into out, and their count into *count. out may be NULL with max 0. */
GLOVER_API int32_t glover_draw_marks(Glover *g, GloverDrawMark *out, uint32_t max, uint32_t *count);

/* 1: the save changed since the last call; 0: not. out gets the 512 bytes either way. */
GLOVER_API int32_t glover_eeprom(Glover *g, uint8_t out[512]);

/* The game's memory, read-only, for hosts that read state or draw for themselves. N64 address 0x80000000 + n is
   ram[n]; values are little-endian; a pointer or list address is a word w at ram + (w - wordBase). */
typedef struct GloverMemory {
    uint32_t size;
    const uint8_t *ram;
    uint32_t ramSize;              /* 0x1800000 */
    uint32_t wordBase;             /* 0x0C000000 */
    uint32_t framebuffer[2];
    uint32_t dynamicLo, dynamicHi;
} GloverMemory;
GLOVER_API int32_t glover_memory(Glover *g, GloverMemory *m);
GLOVER_API int32_t glover_trace(Glover *g, char *buf, uint32_t size);   /* weemitt's --trace line */
GLOVER_API uint32_t glover_hash_memory(Glover *g);                       /* weemitt's --hash-every value */

/* Sound: 16-bit stereo frames at glover_audio_rate Hz (22047; 0 in a build without the sound player). The updates
   make them on the game's clock, as weemitt's headless mode does. The logic and the memory are exact. The library
   queues 65536 frames; when the queue is full, new frames are dropped and counted. */
GLOVER_API int32_t glover_audio_rate(Glover *g);   /* also on the sound thread, also after the game stopped */
GLOVER_API int32_t glover_audio_read(Glover *g, int16_t *out, int32_t frames);   /* frames copied */

/* The OpenGL 3.3 renderer. These calls go on the thread whose context is current, never during glover_update. Each
   call leaves OpenGL's state at its initial values (as in a new context), except the viewport, the scissor box and
   the framebuffer bound to GL_FRAMEBUFFER: the fbo of the latest glover_gl_draw or glover_gl_composite, or 0. */
typedef void *(*GloverGlLoader)(void *user, const char *name);
/* scale: 50 to 400 (percent of the window); msaa: 0, 2, 4 or 8; filter: 0 the N64's, 1 bilinear, 2 nearest;
   widescreen: 0 off, 1 stretched, 2 a wider view. Defaults: 100, 4, 0, 0. Another value: GLOVER_ERR_ARG. */
typedef struct GloverGlOptions { uint32_t size; int32_t scale, msaa, filter, widescreen; } GloverGlOptions;
/* Once; GLOVER_ERR_ARG when the renderer is already set up. */
GLOVER_API int32_t glover_gl_init(Glover *g, GloverGlLoader load, void *user);
GLOVER_API int32_t glover_gl_options(Glover *g, const GloverGlOptions *o);
/* Draws the latest lists into framebuffer fbo (0: the default) of w x h pixels. Sets the viewport to 0, 0, w, h. */
GLOVER_API int32_t glover_gl_draw(Glover *g, uint32_t fbo, int32_t w, int32_t h);
/* The latest picture at its own size, RGBA rows from the top. GLOVER_ERR_ARG with *w and *h set when bytes is too
   small, and with *w and *h 0 before the first picture or before glover_gl_init. */
GLOVER_API int32_t glover_gl_read_frame(Glover *g, uint8_t *rgba, uint32_t bytes, int32_t *w, int32_t *h);
/* Frees the renderer's OpenGL objects. It works also after glover_destroy or a fatal error. */
GLOVER_API int32_t glover_gl_shutdown(Glover *g);

/* ---- A layer drawn over the host's picture (version 3, docs/library.md): parts of the latest picture with alpha, and
   the depth of each pixel in the projection of the drawing view. */
#define GLOVER_LAYER_NO_FOG 0x100  /* with the GLOVER_LAYER_ bits: draw without the game's fog */
typedef struct GloverGlLayer {
    uint32_t size;                 /* sizeof(GloverGlLayer) */
    uint32_t layers;               /* GLOVER_LAYER_ bits, at least one of the four parts */
    int32_t width, height;         /* pixels; the scale of glover_gl_options applies */
    uint32_t colorTexture;         /* out: GL_TEXTURE_2D, RGBA8, premultiplied alpha, rows from the bottom; 0, 0, 0, 0
                                      where nothing is drawn */
    uint32_t depthTexture;         /* out: GL_TEXTURE_2D, DEPTH_COMPONENT32F: the window depth (0 near, 1 far) of the
                                      nearest drawn pixel; 1 where nothing is drawn */
    int32_t texWidth, texHeight;   /* out: the textures' size */
    float nearZ, farZ;             /* out: the planes of the projection (the drawing view's, or the game camera's) */
    float projZ[2];                /* out: that projection's terms m[2][2] and m[3][2] as the game's s15.16 matrix
                                      holds them. A pixel of window depth d lies at the view depth (the distance
                                      along the view axis) e = projZ[1] / (2d - 1 + projZ[0]). */
} GloverGlLayer;
/* Draws the parts of the latest lists that layers names into the library's textures, which stay valid until the next
   glover_gl_draw_layer or glover_gl_shutdown. The 4:3 picture fills width x height, as with widescreen 1: the view's
   aspect is in its projection. A size beyond the GPU's limit is scaled down to it, keeping its shape: texWidth and
   texHeight tell. GLOVER_ERR_ARG for a bad size or bit, before glover_gl_init and before the first lists; the latest
   layer is then gone. Until GloverBodies.ready is 1 the game shows its own level start at its own places: a host
   leaves the layer out until then. */
GLOVER_API int32_t glover_gl_draw_layer(Glover *g, GloverGlLayer *l);
#define GLOVER_COMPOSITE_DEPTH 0x1 /* glover_gl_composite also writes the depth of the pixels with alpha >= 0.5 */
/* Draws the latest layer over framebuffer fbo (0: the default) of w x h pixels: premultiplied blending and the depth
   test LEQUAL against fbo's depth buffer (OpenGL's default range 0 to 1). The depth is the layer's own, in the
   projection of that layer's view, whatever the view is now. Each layer pixel is pulled towards the eye by bias, a
   share of its view depth, 0 <= bias < 1 (0.0005, say), against the s15.16 matrices of the game's actors. flags 0
   or GLOVER_COMPOSITE_DEPTH: without it no depth is written. Sets the viewport to 0, 0, w, h. GLOVER_ERR_ARG without
   a layer. */
GLOVER_API int32_t glover_gl_composite(Glover *g, uint32_t fbo, int32_t w, int32_t h, float bias, uint32_t flags);

/* ---- The display lists for the host's own interpreter (version 4, docs/library.md): the latest lists translated into
   F3DEX2, the microcode the host's interpreter reads, with host pointers. */
#define GLOVER_GBI_F3DEX2 1
#define GLOVER_HOSTGFX_SPLIT 1u        /* GloverHostGfx.flags: the triangles that write no depth go to xlu */
#define GLOVER_HOSTGFX_FOG 2u          /* GloverHostGfx.flags: every triangle takes the host's fog, not the game's */
typedef struct GloverGfx { uintptr_t w0, w1; } GloverGfx;   /* the interpreter's Gfx: one command */
typedef struct GloverHostGfx {
    uint32_t size;                 /* sizeof(GloverHostGfx) */
    uint32_t gbi;                  /* GLOVER_GBI_F3DEX2 */
    uint32_t layers;               /* GLOVER_LAYER_ bits, at least one of the four parts, and GLOVER_LAYER_NO_FOG */
    float t;                       /* below 0: the latest frame; 0 to 1: blended from the previous frame */
    const void *projection[4];     /* host Mtx pointers that replace each projection group of the game, NULL-ended;
                                      projection[0] NULL: the game's own */
    uint32_t flags;                /* GLOVER_HOSTGFX_SPLIT and GLOVER_HOSTGFX_FOG, or 0 */
    const GloverGfx *list;         /* out: the entry list; it ends with G_ENDDL */
    const GloverGfx *xlu;          /* out: with GLOVER_HOSTGFX_SPLIT, the entry list of the triangles that write no
                                      depth, to run after list; else NULL */
    uint32_t commands;             /* out: the commands of every list written */
    uint32_t triangles;            /* out: the triangles in the requested layers */
    uint32_t textures;             /* out: the texture images loaded in them */
    uint32_t skipped;              /* out: what the translation leaves out of them: fill rectangles, texture rectangles
                                      in the copy mode, colour changes of a vertex, and matrices it could not read */
    uint32_t unknown;              /* out: commands of no known microcode, left out */
    uint32_t rects;                /* out: texture rectangles of them (the 3D sprites) written as two triangles */
    /* Added later in version 4. A structure that ends before them has no host fog. */
    uint32_t fogColor;             /* with GLOVER_HOSTGFX_FOG: the fog colour, as G_SETFOGCOLOR's second word
                                      (red in the high byte, then green, blue and alpha) */
    int16_t fogMultiplier;         /* with GLOVER_HOSTGFX_FOG: the fog's multiplier and offset, as G_MW_FOG's data */
    int16_t fogOffset;
} GloverHostGfx;
#define GLOVER_HOSTGFX_SIZE_V4 ((uint32_t)offsetof(GloverHostGfx, fogColor)) /* the size before the host fog */
/* Translates the latest lists. The state commands of every part run; the triangles only in the parts that layers
   names. A projection group is a projection load and the projection multiplies that follow it, as the game's
   SetupViewing writes them: each becomes projection[0] loaded and the rest multiplied in order, which gives
   P = projection[3] x projection[2] x projection[1] x projection[0] for row vectors. The game's colour and depth images,
   scissor, viewport and segments are left out: the host's stay. Pointers in the lists:
   - matrices, vertices, lights and 4-bit and 8-bit texels point into the game's memory, valid until the next
     glover_update;
   - the lists and the blended matrices are the library's, valid until the next glover_host_gfx;
   - texture copies (16-bit and 32-bit texels and palettes, in N64 byte order) are valid for the game's life, or
     until a list carries an invalidation (opcode 0x34) of their address;
   - every pointer is even.
   A texture rectangle of the requested parts (the game's 3D sprites: dust, sparkles) becomes two triangles in the
   world, where the game's projection puts its corners at its depth: an interpreter may draw rectangles without depth.
   With GLOVER_HOSTGFX_SPLIT the triangles whose render mode does not write depth (the shadows, the rings on the water,
   the sprites) go to a second list, xlu, and the rest stay in list. Both lists carry every state command. A host
   that draws its own translucent surfaces after its opaque ones runs list among the opaque ones and xlu after its
   translucent ones: a flat effect then lies on the host's translucent ground and water instead of under it. xlu
   needs no invalidation of its own; run list first in the same drawn frame.
   GLOVER_HOSTGFX_FOG gives every triangle the host's fog in place of the game's: G_FOG in each geometry mode, the
   fog's blend (G_RM_FOG_SHADE_A) in the first cycle of each render mode, the host's fogColor for each fog colour and
   its fogMultiplier and fogOffset for each fog position, also at the start of each entry list. The shade's alpha is
   then the fog amount, as in the game's own levels with fog. The fog depends on the depth in the host's projection:
   with the host's own fog values the glove fogs as the host's world does at the same depth. GLOVER_ERR_ARG with
   GLOVER_LAYER_NO_FOG, or for a structure that ends before fogColor.
   t >= 0 also makes each later update record its new lists; t then blends the matrices between the last two
   recorded frames, with the cuts of GloverFrame.cut. Call between updates, on the game's thread. GLOVER_ERR_ARG for
   a bad structure, gbi or layer bit, or before the first lists; GLOVER_ERR_MEMORY when the lists could not be
   allocated; GLOVER_ERR_UNSUPPORTED in a build without src/gfx. Until GloverBodies.ready is 1 the game shows its own
   level start at its own places: a host leaves the lists out until then. */
GLOVER_API int32_t glover_host_gfx(Glover *g, GloverHostGfx *h);

#ifdef __cplusplus
}
static_assert(sizeof(GloverInput) == 4, "GloverInput");
static_assert(sizeof(GloverList) == 12, "GloverList");
static_assert(sizeof(GloverPose) == 40, "GloverPose");
static_assert(sizeof(GloverWater) == 40, "GloverWater");
static_assert(sizeof(GloverDrawMark) == 8, "GloverDrawMark");
static_assert(sizeof(GloverGlLayer) == 48, "GloverGlLayer");
static_assert(sizeof(GloverGfx) == 2 * sizeof(uintptr_t), "GloverGfx");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(GloverInput) == 4, "GloverInput");
_Static_assert(sizeof(GloverList) == 12, "GloverList");
_Static_assert(sizeof(GloverPose) == 40, "GloverPose");
_Static_assert(sizeof(GloverWater) == 40, "GloverWater");
_Static_assert(sizeof(GloverDrawMark) == 8, "GloverDrawMark");
_Static_assert(sizeof(GloverGlLayer) == 48, "GloverGlLayer");
_Static_assert(sizeof(GloverGfx) == 2 * sizeof(uintptr_t), "GloverGfx");
#endif
#endif
