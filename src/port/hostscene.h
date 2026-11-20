/* A host scene file (hostscene.c, docs/host-world.md): triangles, bodies and timed requests that a test gives the
   host world. weemitt's headless mode (--host-scene) and the library's test host (--scene) read the same files and
   make the same requests at the same moments: the headless mode calls hostworld.c, the test host the library. */
#ifndef HOSTSCENE_H
#define HOSTSCENE_H

typedef struct HsMesh {
    const float *verts;
    unsigned int vertCount;
    const unsigned int *indices;
    const unsigned char *terrain;
    const unsigned char *triFlags;
    unsigned int triCount;
    unsigned int flags;
    const unsigned short *conveyor;
    const float *conveyors;
    unsigned int conveyorCount;
} HsMesh;

typedef struct HsOptions {
    float cellSize;
    unsigned int maxChunkTris;
    float linkRadius;
    unsigned int keepLevel;
    unsigned int levelOff;
} HsOptions;

/* pos[3], rot[4] (x, y, z, w), scale[3]: the layout of GloverPose and HwPose. */
typedef struct HsPose {
    float pos[3], rot[4], scale[3];
} HsPose;

typedef struct HsPlace {
    unsigned int what;
    float hand[3], ball[3], handVel[3], ballVel[3];
    float handYaw;
} HsPlace;

/* The layout of HwWater and GloverWater. */
typedef struct HsWater {
    float min[3], size[3], surface, current[3];
} HsWater;

typedef struct HsView {
    unsigned int flags;
    float eye[3], at[3], up[3];
    float fovy, aspect, nearZ, farZ;
} HsView;

/* What a program does for each request. Each returns 0 or a negative code, which the scene prints. */
typedef struct HsOps {
    int (*setStatic)(void *user, const HsMesh *m, const HsOptions *o);
    int (*bodyAdd)(void *user, const HsMesh *m, const HsPose *p, unsigned int *id);
    int (*bodyMove)(void *user, unsigned int id, const HsPose *p);
    int (*bodyRemove)(void *user, unsigned int id);
    int (*clear)(void *user);
    int (*place)(void *user, const HsPlace *p);
    int (*respawn)(void *user, const float pos[3], float yaw);
    int (*setView)(void *user, const HsView *v);   /* v NULL: the game's own camera */
    int (*gotoLevel)(void *user, int level);
    void (*log)(void *user, const char *line);
    int (*bodyConveyor)(void *user, unsigned int id, const float vel[3]);   /* vel in the body's space */
    int (*setWater)(void *user, const HsWater *w, unsigned int count);      /* w NULL: none */
} HsOps;

/* The state that a program reads back (GloverBodies, HwBodies) after the version 1 part. */
typedef struct HsBodies {
    float handVel[3], ballVel[3];
    float handYaw;
    int handAction, ballHeld, handOnGround, ballOnGround, handTerrain, ballTerrain, ready;
    unsigned int restarts;
    int handVoid, ballVoid;
    unsigned int handVoids, ballVoids;
    float handSafe[3], ballSafe[3];
    int safeKnown;
    int handWater, ballWater;
} HsBodies;

/* The line that --trace prints after each frame's line while a scene runs. The floats are their bits in hex, as in
   the trace: every C library prints them alike. */
void hs_bodies_line(char *out, int size, long frame, const HsBodies *b);

typedef struct HostScene HostScene;

/* NULL with a message in err when the file cannot be read. */
HostScene *hs_load(const char *path, char *err, int errSize);
/* Before each update: the requests due at tStart original frames, and the moving bodies' poses for the end of the
   update, tStart + step. */
void hs_update(HostScene *s, double tStart, double step, const HsOps *ops, void *user);
void hs_free(HostScene *s);

#endif
