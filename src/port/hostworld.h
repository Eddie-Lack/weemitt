/* The host world (hostworld.c, docs/host-world.md): collision, moving bodies, places and a camera that a host
   program gives the game. Plain C types only: the library (src/lib) and the platform layer (platform.c) include it
   without the game's headers. The result codes are glover.h's. */
#ifndef HOSTWORLD_H
#define HOSTWORLD_H

#define HW_OK 0
#define HW_ERR_ARG (-1)
#define HW_ERR_BUSY (-4)
#define HW_ERR_MEMORY (-10)

#define HW_TERRAIN_COUNT 14       /* the game's terrain table: 0 to 13 */
#define HW_CAMERA_PASS 0x1        /* a mesh or triangle flag: the game's camera passes through */
#define HW_VOID 0x2               /* a mesh or triangle flag: a void, reported in HwBodies */
#define HW_TRI_FLAGS (HW_CAMERA_PASS | HW_VOID)
#define HW_MAX_STATIC_TRIS 65536
#define HW_MAX_BODIES 256
#define HW_MAX_BODY_TRIS 4096
#define HW_MAX_CHUNK_TRIS 32767
#define HW_COORD_LIMIT 100000.0f
#define HW_MAX_CONVEYORS 4096
#define HW_CONVEYOR_LIMIT 100.0f  /* a conveyor's speed per axis, game units per original frame */
#define HW_MAX_WATERS 256

typedef struct HwMesh {
    const float *verts;               /* x, y, z per vertex, game units */
    unsigned int vertCount;
    const unsigned int *indices;      /* 3 per triangle, counter-clockwise seen from the front */
    const unsigned char *terrain;     /* 1 per triangle, 0 to 13; NULL: all 0 */
    const unsigned char *triFlags;    /* 1 per triangle, HW_TRI_FLAGS bits; NULL: none */
    unsigned int triCount;
    unsigned int flags;               /* HW_TRI_FLAGS bits for every triangle */
    const unsigned short *conveyor;   /* 1 per triangle: 0 none, k: conveyors[k - 1]; NULL: none. Static only */
    const float *conveyors;           /* x, y, z per conveyor, game units per original frame */
    unsigned int conveyorCount;       /* at most HW_MAX_CONVEYORS */
} HwMesh;

/* A water volume: the layout of GloverWater and HsWater. */
typedef struct HwWater {
    float min[3];                     /* the lowest corner */
    float size[3];                    /* each above 0 */
    float surface;                    /* within the box's height */
    float current[3];                 /* added to the velocity of what is under the surface, per original frame */
} HwWater;

typedef struct HwOptions {
    float cellSize;                   /* 0: 256 */
    unsigned int maxChunkTris;        /* 0: 1024 */
    float linkRadius;                 /* 0: every chunk linked */
    unsigned int keepLevel;
    unsigned int levelOff;            /* HW_OFF_ bits: the level's own things that are switched off */
} HwOptions;

#define HW_OFF_ENEMIES 0x1
#define HW_OFF_COLLECTABLES 0x2
#define HW_OFF_AMBIENT 0x4

typedef struct HwPose {
    float pos[3];
    float rot[4];                     /* quaternion x, y, z, w */
    float scale[3];
} HwPose;

typedef struct HwStats {
    unsigned int staticTris, chunks, linkedChunks, bodies;
    unsigned int heapUsed, heapFree, rebuilds;
    unsigned int pending;             /* 1 while a static set or a body waits for the glove to be ready */
    unsigned int failed;              /* installs that waited and then found too little memory */
    unsigned int buildUs;             /* processor time of the latest build of the static world, in microseconds; for
                                         reports only, the game never reads it */
    unsigned int enemiesOn;           /* the level's enemies that are in the game now */
    unsigned int collectablesOn;      /* the level's garibs and potions that are in the game now */
    unsigned int conveyorTris;        /* static triangles on a conveyor */
    unsigned int conveyorPlatforms;   /* the conveyor chunks of the static world, and the bodies with a conveyor */
    unsigned int waters;              /* the host's water volumes */
} HwStats;

#define HW_PLACE_HAND 0x01
#define HW_PLACE_BALL 0x02
#define HW_PLACE_VEL 0x04
#define HW_PLACE_YAW 0x08
#define HW_PLACE_CAMERA 0x10
typedef struct HwPlace {
    unsigned int what;
    float hand[3], ball[3], handVel[3], ballVel[3];
    float handYaw;
} HwPlace;

#define HW_VIEW_DRAW 0x1
#define HW_VIEW_STEER 0x2
/* A test flag: draw with the game's own camera through the host's path (docs/testing.md). */
#define HW_VIEW_MIRROR 0x80000000u
typedef struct HwView {
    unsigned int flags;
    float eye[3], at[3], up[3];
    float fovy, aspect, nearZ, farZ;
} HwView;

typedef struct HwBodies {
    float hand[3], ball[3], camera[3];
    float handVel[3], ballVel[3];
    float handYaw;
    int handAction, ballHeld, handOnGround, ballOnGround, handTerrain, ballTerrain, ready;
    unsigned int restarts;
    int handVoid, ballVoid;           /* 0; 1: the floor under it is a void; 2: it touches a void */
    unsigned int handVoids, ballVoids;/* times it began to touch a void */
    float handSafe[3], ballSafe[3];   /* where it last stood on a static triangle that is safe */
    int safeKnown;                    /* bit 0: handSafe holds a place; bit 1: ballSafe */
    int handWater, ballWater;         /* the game's waterState: 0, or 1 to 4 (paddling, wading, swimming, floating) */
} HwBodies;

/* Each returns HW_OK or an HW_ERR_ code. They run between updates. */
int hw_check_mesh(const HwMesh *m, unsigned int maxTris);
int hw_set_static(const HwMesh *m, const HwOptions *o);   /* m NULL: no static world */
int hw_body_add(const HwMesh *m, const HwPose *p, unsigned int *id);
int hw_body_move(unsigned int id, const HwPose *p);
int hw_body_remove(unsigned int id);
int hw_body_conveyor(unsigned int id, const float vel[3]);   /* vel NULL or 0, 0, 0: none */
int hw_set_water(const HwWater *w, unsigned int count);      /* replaces the host's waters; NULL or 0: none */
int hw_clear(void);
void hw_forget(void);                                     /* frees the host's memory only */
void hw_stats(HwStats *s);
int hw_place(const HwPlace *p);
int hw_set_respawn(const float pos[3], float yaw);
int hw_set_view(const HwView *v);                         /* v NULL: the game's own camera */
int hw_view_draws(void);                                  /* 1 while a view with HW_VIEW_DRAW is set */
/* The planes of the projection the game draws with: the drawing view's, or the game camera's. projZ gets the
   projection's terms m[2][2] and m[3][2] as the game's s15.16 matrix holds them. */
void hw_view_planes(float *nearZ, float *farZ, float projZ[2]);
int hw_check_view(const HwView *v);
int hw_check_pose(const HwPose *p);
void hw_bodies(HwBodies *b);
int hw_ready(void);

#endif
