/* The host world (docs/host-world.md): collision, moving bodies, places and a camera that a host program gives the
   game. The library (src/lib/coll.c, src/lib/view.c) and the headless mode (platform.c --host-scene, through
   hostscene.c) call it between updates.

   The game reads collision from the actors in collisionList and the platforms in platformList, each with an array
   of CollFace planes. The host's static triangles become LANDSCAPE_ACTOR pseudo-actors: one for each chunk of the
   ground plane, with its triangles' planes, built by the game's own InitPlanes. A moving body becomes a Platform,
   built by InitPlatformInfo, which a hook after UpdatePlatforms puts at the host's pose. Everything that the game
   reads lives in port_ram, in blocks from JallocAlloc. The host's own copy of its meshes is in malloc'd memory: the
   game never points at it, and it serves to rebuild the world after a level reload has freed the game's heap.

   Nothing here runs until a host asks for it: the hooks (porthooks.h) stay NULL. */
#include <time.h>
PORT_C_BEGIN
#include "port.h"
#include "common.h"
#include "macros.h"
#include "maths.h"
#include "actor.h"
#include "platform.h"
#include "hand.h"
#include "ball.h"
#include "camera.h"
#include "collision.h"
#include "jalloc.h"
#include "footprint.h"
#include "debris.h"
#include "bullet.h"
#include "collect.h"
#include "tip.h"
#include "enemy.h"
#include "overlay.h"
#include "sound.h"
#include "ambient.h"
#include "water.h"
#include "display.h"
#include "game.h"
#include "delta.h"
#include "porthooks.h"
#include "portmath.h"
#include "hostworld.h"

extern s16 fieldOfView;
extern s16 nearPlaneDist;
extern s16 farPlaneDist;
extern u8 ShadingMode;
extern u8 draw_buffer;
extern Vec3 actualCamPos[];
extern Vec3 actualCamTargetPos[];
extern Vec3 camVect;
extern Debris debris[120];
extern PTR32(Plane) actorPlane[4];
extern PTR32(CollFace) corner;
extern PTR32(CollFace) edgePlane;
extern u8 gamePaused;
extern u32 portRestarts;
extern u32 portHeapInits;
void SubPlatform(Platform *platform);

/* The heap that must stay free after an install, for the game's own allocations. */
#define HEAP_RESERVE (2 * 1024 * 1024)
/* Block table entries that must stay free after an install. */
#define BLOCK_RESERVE 64
#define MAX_CHUNKS 8192
/* The shortest edge a triangle may have. The game's inner-edge test (CheckForInnerEdges) reads outside its table
   conv[] when both ends of an edge lie within 0.01 of one vertex of another face, which needs an edge under 0.02. */
#define MIN_EDGE 0.025

/* A cleaned mesh in the host's memory: equal vertices merged, triangles of no area or with a very short edge left
   out. */
typedef struct HwCopy {
    float *verts;
    unsigned int vertCount;
    unsigned int *tris;
    unsigned char *terrain;
    unsigned char *flags;
    unsigned short *conveyor;        /* per triangle: 0 or an index into conveyors + 1; NULL: none */
    float *conveyors;
    unsigned int conveyorCount;
    unsigned int triCount;
} HwCopy;

typedef struct HwChunk {
    unsigned int first, count;       /* its triangles: hw.chunkTris[first..] */
    unsigned int vertFirst, vertCount; /* its vertices: hw.chunkVerts[vertFirst..] (indices into the copy) */
    unsigned int flags;
    unsigned int conveyor;           /* 0, or k: the chunk is a platform with conveyor k of the copy */
    Vec3 centre;
    f32 radius;
    Actor *actor;                    /* the block in port_ram, NULL when not built; a conveyor's &plat->actor */
    Platform *plat;                  /* a conveyor chunk's platform */
    unsigned int bytes;
} HwChunk;

typedef struct HwBody {
    unsigned int id;                 /* 0: a free slot */
    HwCopy mesh;
    HwPose cur, target;
    int moved;
    unsigned char *inner;            /* per face: the edges that InitPlatformInfo found inner (bits 0-2) */
    Platform *plat;                  /* NULL when not built */
    PlatformMotion *motion;          /* the turn about y, in the body's block; plat->motion while it is turned */
    ActorShadow *shadow;             /* in the body's block */
    int hasConveyor;
    float conveyor[3];               /* in the body's space, per original frame */
    unsigned int bytes;
} HwBody;

/* A water box of the level, emptied while the static world is in. */
typedef struct TakenWater {
    Water *water;
    f32 width;                       /* its box's size.x */
} TakenWater;

static struct {
    int active;                      /* the hooks are installed */
    /* The static world. */
    int wantStatic;
    HwCopy stat;
    HwOptions opt;
    HwChunk *chunks;
    unsigned int chunkCount;
    f32 lowest;                      /* the lowest height of the built static world */
    unsigned int *chunkTris;
    unsigned int *chunkVerts;
    char *block;                     /* the one heap block of every chunk, NULL when not built */
    int built;
    /* The level's own collision, taken out while the static world is in. */
    Actor **takenActors;
    unsigned int takenActorCount, takenActorCap;
    Platform **takenPlats;
    unsigned int takenPlatCount, takenPlatCap;
    TakenWater *takenWaters;
    unsigned int takenWaterCount, takenWaterCap;
    int levelTaken;
    /* The bodies. */
    HwBody bodies[HW_MAX_BODIES];
    unsigned int nextId;
    /* Requests for the next ready update. */
    int placePending;
    HwPlace place;
    int respawnSet;                  /* the host gave a respawn: it goes in again after each level reload */
    int respawnPending;
    float respawnPos[3];
    float respawnYaw;
    HwView view;
    int viewOn;
    /* While a drawing view is on, the game's draw distance is the view's far distance: viewFar is the value written,
       0 when none, and levelFar the level's own value, put back when the view goes. */
    s16 viewFar;
    s16 levelFar;
    /* Counters. */
    int lost;                        /* a level reload freed what was built */
    unsigned int rebuilds;
    unsigned int failed;
    unsigned int linked;
    unsigned int buildUs;
    u32 heapInits;                   /* portHeapInits when the hooks went in */
    /* The voids, and the latest safe places (HwBodies). Index 0 is the glove, 1 the ball. */
    int voidState[2];
    unsigned int voids[2];
    float safe[2][3];
    int safeKnown;
    /* The host's waters: the copy, and the game's Water of each in one heap block, NULL when not built. */
    HwWater *waters;
    unsigned int waterCount;
    Water *waterBlock;
} hw;

static void PreUpdate(void);
static void AfterPlatforms(void);
static void PostUpdate(void);
static void LevelFreed(void);
static void WorldLost(int intact);
static void ViewHook(Mtx *projection, Mtx *view, u16 *perspNorm);
static s32 CameraHook(void);

/* ---- Checks of the host's data. */

static int Finite(float f) {
    return f == f && f < HW_COORD_LIMIT && f > -HW_COORD_LIMIT;
}

int hw_check_mesh(const HwMesh *m, unsigned int maxTris) {
    unsigned int i;

    if (m == NULL || m->verts == NULL || m->indices == NULL || m->vertCount == 0 || m->triCount == 0
        || m->triCount > maxTris || m->vertCount > 3 * maxTris || (m->flags & ~HW_TRI_FLAGS)) {
        return HW_ERR_ARG;
    }
    for (i = 0; i < 3 * m->vertCount; i++) {
        if (!Finite(m->verts[i])) {
            return HW_ERR_ARG;
        }
    }
    for (i = 0; i < 3 * m->triCount; i++) {
        if (m->indices[i] >= m->vertCount) {
            return HW_ERR_ARG;
        }
    }
    for (i = 0; i < m->triCount; i++) {
        if ((m->terrain != NULL && m->terrain[i] >= HW_TERRAIN_COUNT)
            || (m->triFlags != NULL && (m->triFlags[i] & ~HW_TRI_FLAGS))) {
            return HW_ERR_ARG;
        }
    }
    if (m->conveyor != NULL) {
        if (m->conveyorCount > HW_MAX_CONVEYORS || (m->conveyorCount > 0 && m->conveyors == NULL)) {
            return HW_ERR_ARG;
        }
        for (i = 0; i < 3 * m->conveyorCount; i++) {
            if (!(m->conveyors[i] >= -HW_CONVEYOR_LIMIT && m->conveyors[i] <= HW_CONVEYOR_LIMIT)) {
                return HW_ERR_ARG;
            }
        }
        for (i = 0; i < m->triCount; i++) {
            if (m->conveyor[i] > m->conveyorCount) {
                return HW_ERR_ARG;
            }
        }
    }
    return HW_OK;
}

int hw_check_pose(const HwPose *p) {
    double len;
    int i;

    for (i = 0; i < 3; i++) {
        if (!Finite(p->pos[i]) || !Finite(p->scale[i]) || p->scale[i] == 0.0f) {
            return HW_ERR_ARG;
        }
    }
    for (i = 0; i < 4; i++) {
        if (!(p->rot[i] >= -1.001f && p->rot[i] <= 1.001f)) {
            return HW_ERR_ARG;
        }
    }
    len = (double)p->rot[0] * p->rot[0] + (double)p->rot[1] * p->rot[1] + (double)p->rot[2] * p->rot[2]
        + (double)p->rot[3] * p->rot[3];
    return len > 0.998 && len < 1.002 ? HW_OK : HW_ERR_ARG;
}

int hw_check_view(const HwView *v) {
    double f[3];
    double x[3];
    int i;

    if (v->flags & ~(HW_VIEW_DRAW | HW_VIEW_STEER | HW_VIEW_MIRROR)) {
        return HW_ERR_ARG;
    }
    for (i = 0; i < 3; i++) {
        if (!Finite(v->eye[i]) || !Finite(v->at[i]) || !Finite(v->up[i])) {
            return HW_ERR_ARG;
        }
    }
    if (v->eye[0] == v->at[0] && v->eye[1] == v->at[1] && v->eye[2] == v->at[2]) {
        return HW_ERR_ARG;
    }
    if (v->up[0] == 0.0f && v->up[1] == 0.0f && v->up[2] == 0.0f) {
        return HW_ERR_ARG;
    }
    /* guLookAt needs an up vector that is not parallel to the direction of view. */
    f[0] = (double)v->at[0] - v->eye[0];
    f[1] = (double)v->at[1] - v->eye[1];
    f[2] = (double)v->at[2] - v->eye[2];
    x[0] = f[1] * v->up[2] - f[2] * v->up[1];
    x[1] = f[2] * v->up[0] - f[0] * v->up[2];
    x[2] = f[0] * v->up[1] - f[1] * v->up[0];
    if (x[0] * x[0] + x[1] * x[1] + x[2] * x[2]
        <= 1e-6 * (f[0] * f[0] + f[1] * f[1] + f[2] * f[2])
               * ((double)v->up[0] * v->up[0] + (double)v->up[1] * v->up[1] + (double)v->up[2] * v->up[2])) {
        return HW_ERR_ARG;
    }
    if (!(v->fovy > 1.0f && v->fovy < 179.0f) || !(v->aspect > 0.01f && v->aspect < 100.0f)
        || !(v->nearZ > 0.0f) || !(v->farZ > v->nearZ) || !Finite(v->farZ)) {
        return HW_ERR_ARG;
    }
    return HW_OK;
}

/* ---- The host's copy of a mesh. */

static void FreeCopy(HwCopy *c) {
    free(c->verts);
    free(c->tris);
    free(c->terrain);
    free(c->flags);
    free(c->conveyor);
    free(c->conveyors);
    memset(c, 0, sizeof(*c));
}

static unsigned int HashVertex(const float *v) {
    unsigned int a;
    unsigned int b;
    unsigned int c;

    memcpy(&a, &v[0], 4);
    memcpy(&b, &v[1], 4);
    memcpy(&c, &v[2], 4);
    return (a * 73856093u) ^ (b * 19349663u) ^ (c * 83492791u);
}

/* The shortest edge of triangle t of c, with each axis scaled by scale[]. */
static double ShortestEdge(const HwCopy *c, unsigned int t, const float scale[3]) {
    double shortest = 1e30;
    int e;
    int j;

    for (e = 0; e < 3; e++) {
        const float *a = &c->verts[3 * c->tris[3 * t + e]];
        const float *b = &c->verts[3 * c->tris[3 * t + (e + 1) % 3]];
        double d = 0.0;

        for (j = 0; j < 3; j++) {
            double x = ((double)b[j] - a[j]) * scale[j];

            d += x * x;
        }
        shortest = d < shortest ? d : shortest;
    }
    return sqrt(shortest);
}

/* Copies a checked mesh: vertices with exactly equal coordinates become one, and triangles whose cross product is
   0, or with an edge shorter than MIN_EDGE, are left out. The order of what stays is the host's. Returns
   HW_ERR_MEMORY when malloc fails. */
static int CopyMesh(const HwMesh *m, HwCopy *c) {
    static const float unitScale[3] = { 1.0f, 1.0f, 1.0f };
    unsigned int hashSize = 1;
    unsigned int *hash;
    unsigned int *remap;
    unsigned int i;
    unsigned int j;

    memset(c, 0, sizeof(*c));
    while (hashSize < 2 * m->vertCount) {
        hashSize <<= 1;
    }
    hash = (unsigned int *)calloc(hashSize, sizeof(unsigned int));
    remap = (unsigned int *)malloc(m->vertCount * sizeof(unsigned int));
    c->verts = (float *)malloc(m->vertCount * 3 * sizeof(float));
    c->tris = (unsigned int *)malloc(m->triCount * 3 * sizeof(unsigned int));
    c->terrain = (unsigned char *)malloc(m->triCount);
    c->flags = (unsigned char *)malloc(m->triCount);
    if (m->conveyor != NULL) {
        c->conveyor = (unsigned short *)malloc(m->triCount * sizeof(unsigned short));
        c->conveyors = (float *)malloc((3 * m->conveyorCount + 1) * sizeof(float));
        c->conveyorCount = m->conveyorCount;
        if (c->conveyors != NULL && m->conveyorCount > 0) {
            memcpy(c->conveyors, m->conveyors, 3 * m->conveyorCount * sizeof(float));
        }
    }
    if (hash == NULL || remap == NULL || c->verts == NULL || c->tris == NULL || c->terrain == NULL
        || c->flags == NULL || (m->conveyor != NULL && (c->conveyor == NULL || c->conveyors == NULL))) {
        free(hash);
        free(remap);
        FreeCopy(c);
        return HW_ERR_MEMORY;
    }
    for (i = 0; i < m->vertCount; i++) {
        float v[3];
        unsigned int slot;

        /* -0 and 0 are one coordinate. */
        for (j = 0; j < 3; j++) {
            v[j] = m->verts[3 * i + j] == 0.0f ? 0.0f : m->verts[3 * i + j];
        }
        for (slot = HashVertex(v) & (hashSize - 1); hash[slot]; slot = (slot + 1) & (hashSize - 1)) {
            const float *w = &c->verts[3 * (hash[slot] - 1)];

            if (w[0] == v[0] && w[1] == v[1] && w[2] == v[2]) {
                break;
            }
        }
        if (hash[slot] == 0) {
            memcpy(&c->verts[3 * c->vertCount], v, sizeof(v));
            hash[slot] = ++c->vertCount;
        }
        remap[i] = hash[slot] - 1;
    }
    for (i = 0; i < m->triCount; i++) {
        unsigned int a = remap[m->indices[3 * i]];
        unsigned int b = remap[m->indices[3 * i + 1]];
        unsigned int d = remap[m->indices[3 * i + 2]];
        const float *pa = &c->verts[3 * a];
        const float *pb = &c->verts[3 * b];
        const float *pd = &c->verts[3 * d];
        double e1[3];
        double e2[3];
        double n[3];

        for (j = 0; j < 3; j++) {
            e1[j] = (double)pb[j] - pa[j];
            e2[j] = (double)pd[j] - pa[j];
        }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        if (a == b || b == d || a == d || (n[0] == 0.0 && n[1] == 0.0 && n[2] == 0.0)) {
            continue;
        }
        c->tris[3 * c->triCount] = a;
        c->tris[3 * c->triCount + 1] = b;
        c->tris[3 * c->triCount + 2] = d;
        if (ShortestEdge(c, c->triCount, unitScale) < MIN_EDGE) {
            continue;
        }
        c->terrain[c->triCount] = m->terrain != NULL ? m->terrain[i] : 0;
        c->flags[c->triCount] = (m->triFlags != NULL ? m->triFlags[i] : 0) | m->flags;
        if (c->conveyor != NULL) {
            c->conveyor[c->triCount] = m->conveyor[i];
        }
        c->triCount++;
    }
    free(hash);
    free(remap);
    return HW_OK;
}

/* ---- Chunks. */

typedef struct SortItem {
    double key[3];
    unsigned int index;
} SortItem;

static int CompareItems(const void *pa, const void *pb) {
    const SortItem *a = (const SortItem *)pa;
    const SortItem *b = (const SortItem *)pb;
    int i;

    for (i = 0; i < 3; i++) {
        if (a->key[i] != b->key[i]) {
            return a->key[i] < b->key[i] ? -1 : 1;
        }
    }
    return a->index < b->index ? -1 : a->index > b->index ? 1 : 0;
}

static void Centroid(const HwCopy *c, unsigned int t, double out[3]) {
    int j;

    for (j = 0; j < 3; j++) {
        out[j] = ((double)c->verts[3 * c->tris[3 * t] + j] + c->verts[3 * c->tris[3 * t + 1] + j]
                  + c->verts[3 * c->tris[3 * t + 2] + j]) / 3.0;
    }
}

/* The number of distinct vertices of triangles tris[0..n). mark is a scratch array of the copy's vertex count, all
   0 on entry and on return. */
static unsigned int CountVerts(const HwCopy *c, const unsigned int *tris, unsigned int n, unsigned char *mark) {
    unsigned int count = 0;
    unsigned int i;
    unsigned int j;

    for (i = 0; i < n; i++) {
        for (j = 0; j < 3; j++) {
            unsigned int v = c->tris[3 * tris[i] + j];

            if (!mark[v]) {
                mark[v] = 1;
                count++;
            }
        }
    }
    for (i = 0; i < n; i++) {
        for (j = 0; j < 3; j++) {
            mark[c->tris[3 * tris[i] + j]] = 0;
        }
    }
    return count;
}

/* Adds the chunks for triangles tris[0..n), which are in the host's order, to hw.chunks. A set with more triangles
   than maxTris, or more vertices than a mesh holds, is cut in two at the median of its longest axis. key holds the
   chunk's HW_CAMERA_PASS bit and, above it, its conveyor. */
static int AddChunks(unsigned int *tris, unsigned int n, unsigned int key, unsigned int maxTris,
                     unsigned int *chunkTris, unsigned int *used, unsigned char *mark) {
    const HwCopy *c = &hw.stat;
    double lo[3] = { 1e30, 1e30, 1e30 };
    double hi[3] = { -1e30, -1e30, -1e30 };
    SortItem *items;
    unsigned int i;
    int axis;
    int j;
    int rc;

    if (n <= maxTris && CountVerts(c, tris, n, mark) <= 32767) {
        HwChunk *k;

        if (hw.chunkCount >= MAX_CHUNKS) {
            return HW_ERR_MEMORY;
        }
        k = &hw.chunks[hw.chunkCount++];
        memset(k, 0, sizeof(*k));
        k->first = *used;
        k->count = n;
        k->flags = key & HW_CAMERA_PASS;
        k->conveyor = key >> 1;
        memcpy(&chunkTris[*used], tris, n * sizeof(unsigned int));
        *used += n;
        return HW_OK;
    }
    for (i = 0; i < n; i++) {
        double p[3];

        Centroid(c, tris[i], p);
        for (j = 0; j < 3; j++) {
            lo[j] = p[j] < lo[j] ? p[j] : lo[j];
            hi[j] = p[j] > hi[j] ? p[j] : hi[j];
        }
    }
    axis = 0;
    for (j = 1; j < 3; j++) {
        if (hi[j] - lo[j] > hi[axis] - lo[axis]) {
            axis = j;
        }
    }
    items = (SortItem *)malloc(n * sizeof(SortItem));
    if (items == NULL) {
        return HW_ERR_MEMORY;
    }
    for (i = 0; i < n; i++) {
        double p[3];

        Centroid(c, tris[i], p);
        items[i].key[0] = p[axis];
        items[i].key[1] = items[i].key[2] = 0.0;
        items[i].index = tris[i];
    }
    qsort(items, n, sizeof(SortItem), CompareItems);
    /* Each half keeps the host's order: a triangle is in the lower half when its place in the sorted list is. */
    for (i = 0; i < n; i++) {
        items[i].key[0] = i < n / 2 ? 0.0 : 1.0;
        items[i].key[1] = 0.0;
    }
    for (i = 0; i < n; i++) {
        items[i].key[1] = (double)items[i].index;
    }
    qsort(items, n, sizeof(SortItem), CompareItems);
    for (i = 0; i < n; i++) {
        tris[i] = items[i].index;
    }
    free(items);
    rc = AddChunks(tris, n / 2, key, maxTris, chunkTris, used, mark);
    if (rc == HW_OK) {
        rc = AddChunks(tris + n / 2, n - n / 2, key, maxTris, chunkTris, used, mark);
    }
    return rc;
}

static void FreeChunkPlan(void) {
    free(hw.chunks);
    free(hw.chunkTris);
    free(hw.chunkVerts);
    hw.chunks = NULL;
    hw.chunkTris = NULL;
    hw.chunkVerts = NULL;
    hw.chunkCount = 0;
}

/* Cuts hw.stat into chunks: each triangle goes to the cell of the ground plane that holds its centroid, the cells
   in the order of their z and then x index, and the triangles of a cell in the host's order. Triangles that the
   camera passes through form chunks of their own. The triangles of each conveyor form chunks of their own after all
   the cells, cut only when they are too many. Without conveyors the chunks and their order are as before conveyors
   were added. */
static int PlanChunks(void) {
    const HwCopy *c = &hw.stat;
    double cell = hw.opt.cellSize > 0.0f ? hw.opt.cellSize : 256.0;
    unsigned int maxTris = hw.opt.maxChunkTris ? hw.opt.maxChunkTris : 1024;
    SortItem *items;
    unsigned int *order;
    unsigned char *mark;
    unsigned int used = 0;
    unsigned int i;
    unsigned int j;
    int rc = HW_OK;

    hw.chunks = (HwChunk *)calloc(MAX_CHUNKS, sizeof(HwChunk));
    hw.chunkTris = (unsigned int *)malloc(c->triCount * sizeof(unsigned int));
    hw.chunkVerts = (unsigned int *)malloc(c->triCount * 3 * sizeof(unsigned int));
    items = (SortItem *)malloc(c->triCount * sizeof(SortItem));
    order = (unsigned int *)malloc(c->triCount * sizeof(unsigned int));
    mark = (unsigned char *)calloc(c->vertCount, 1);
    if (hw.chunks == NULL || hw.chunkTris == NULL || hw.chunkVerts == NULL || items == NULL || order == NULL
        || mark == NULL) {
        free(items);
        free(order);
        free(mark);
        FreeChunkPlan();
        return HW_ERR_MEMORY;
    }
    for (i = 0; i < c->triCount; i++) {
        double p[3];

        Centroid(c, i, p);
        items[i].key[0] = floor(p[2] / cell);
        items[i].key[1] = floor(p[0] / cell);
        items[i].key[2] = (double)(c->flags[i] & HW_CAMERA_PASS);
        /* A conveyor's triangles stay together, after every cell: the game carries what leaves one conveyor platform
           for another with the first one's speed, which would add a step at each cut. */
        if (c->conveyor != NULL && c->conveyor[i] != 0) {
            items[i].key[0] = 1e300;
            items[i].key[1] = 0.0;
            items[i].key[2] += (double)(c->conveyor[i] << 1);
        }
        items[i].index = i;
    }
    qsort(items, c->triCount, sizeof(SortItem), CompareItems);
    for (i = 0; i < c->triCount && rc == HW_OK; i = j) {
        for (j = i; j < c->triCount && items[j].key[0] == items[i].key[0] && items[j].key[1] == items[i].key[1]
                    && items[j].key[2] == items[i].key[2];
             j++) {
            order[j - i] = items[j].index;
        }
        rc = AddChunks(order, j - i, (unsigned int)items[i].key[2], maxTris, hw.chunkTris, &used, mark);
    }
    free(items);
    free(order);
    if (rc != HW_OK) {
        free(mark);
        FreeChunkPlan();
        return rc;
    }
    /* Each chunk's vertices, in the order of first use, and its sphere. */
    used = 0;
    for (i = 0; i < hw.chunkCount; i++) {
        HwChunk *k = &hw.chunks[i];
        double sum[3] = { 0.0, 0.0, 0.0 };
        double r2 = 0.0;
        unsigned int t;
        int a;

        k->vertFirst = used;
        for (t = 0; t < k->count; t++) {
            for (a = 0; a < 3; a++) {
                unsigned int v = c->tris[3 * hw.chunkTris[k->first + t] + a];

                if (!mark[v]) {
                    mark[v] = 1;
                    hw.chunkVerts[used++] = v;
                }
            }
        }
        k->vertCount = used - k->vertFirst;
        for (t = 0; t < k->vertCount; t++) {
            for (a = 0; a < 3; a++) {
                sum[a] += c->verts[3 * hw.chunkVerts[k->vertFirst + t] + a];
            }
            mark[hw.chunkVerts[k->vertFirst + t]] = 0;
        }
        k->centre.x = (f32)(sum[0] / k->vertCount);
        k->centre.y = (f32)(sum[1] / k->vertCount);
        k->centre.z = (f32)(sum[2] / k->vertCount);
        for (t = 0; t < k->vertCount; t++) {
            const float *v = &c->verts[3 * hw.chunkVerts[k->vertFirst + t]];
            double d = ((double)v[0] - k->centre.x) * ((double)v[0] - k->centre.x)
                     + ((double)v[1] - k->centre.y) * ((double)v[1] - k->centre.y)
                     + ((double)v[2] - k->centre.z) * ((double)v[2] - k->centre.z);

            r2 = d > r2 ? d : r2;
        }
        k->radius = (f32)(sqrt(r2) + 1.0);
    }
    free(mark);
    return HW_OK;
}

/* ---- The game's heap. */

static unsigned int Align16(unsigned int n) {
    return (n + 15) & ~15u;
}

/* The free gap between the heap's static and dynamic blocks, in bytes. */
static unsigned int HeapGap(void) {
    JallocControl *jc = &jallocControl;
    unsigned int top = jc->lastStatic == -1 ? jc->heapStart + jc->heapSize : jc->blocks[jc->lastStatic].addr;
    unsigned int bottom = jc->firstDynamic == JALLOC_NUM_BLOCKS
                              ? jc->heapStart
                              : jc->blocks[jc->firstDynamic].addr + jc->blocks[jc->firstDynamic].size;

    return top > bottom ? top - bottom : 0;
}

/* HW_OK when blocks of these total bytes fit and leave the reserve. */
static int HeapFits(unsigned int bytes, unsigned int blocks) {
    JallocControl *jc = &jallocControl;
    int freeSlots = jc->firstDynamic - jc->lastStatic - 1;

    if ((double)bytes + HEAP_RESERVE > (double)HeapGap() || (int)blocks + BLOCK_RESERVE > freeSlots) {
        return HW_ERR_MEMORY;
    }
    return HW_OK;
}


/* ---- Pointers into blocks that are about to be freed. */

typedef struct Range {
    const char *lo, *hi;
} Range;

static Range *ranges;
static unsigned int rangeCount;

static int InRanges(const void *p) {
    unsigned int i;

    if (p == NULL) {
        return 0;
    }
    for (i = 0; i < rangeCount; i++) {
        if ((const char *)p >= ranges[i].lo && (const char *)p < ranges[i].hi) {
            return 1;
        }
    }
    return 0;
}

#define SCRUB(slot)                                                                                                 \
    do {                                                                                                            \
        if (InRanges((const void *)(slot))) {                                                                       \
            (slot) = NULL;                                                                                          \
        }                                                                                                           \
    } while (0)

static void ScrubObj(ActorObject *obj) {
    int k;

    if (obj != NULL) {
        for (k = 0; k < 4; k++) {
            SCRUB(obj->planes[k]);
        }
    }
}

static void ScrubActor(Actor *a) {
    ScrubObj(a->obj);
    if (a->shadow != NULL) {
        SCRUB(a->shadow->floor);
    }
}

/* Clears every pointer of the game's state that points into ranges[]: the contact planes of the actors, the
   floors of their shadows, the glove's cling planes, and the normals of the garibs, tips and footprints. */
static void ScrubPointers(void) {
    Actor *a;
    Bullet *bullet;
    GaribGroup *group;
    Garib *garib;
    Tip *tip;
    Footprint *fp;
    int i;

    if (rangeCount == 0) {
        return;
    }
    for (a = actorList.head.next; a != &actorList.head; a = a->next) {
        ScrubActor(a);
    }
    ScrubActor(&hand);
    ScrubActor(&ball);
    ScrubActor(&camera);
    ScrubActor(&cameraInfo.actor);
    ScrubActor(&handControl.edgeProbe);
    ScrubActor(&handControl.frontProbe);
    ScrubActor(&handControl.orbiter);
    ScrubActor(&handControl.orbiter2);
    ScrubActor(&handControl.orbiter3);
    for (bullet = bulletList.head.next; bullet != &bulletList.head; bullet = bullet->next) {
        ScrubActor(&bullet->actor);
    }
    for (i = 0; i < 120; i++) {
        if (debris[i].active) {
            ScrubObj(debris[i].obj);
            if (debris[i].shadow != NULL) {
                SCRUB(debris[i].shadow->floor);
            }
        }
    }
    if (handControl.fakeball != NULL && handControl.fakeball->shadow != NULL) {
        SCRUB(handControl.fakeball->shadow->floor);
    }
    for (i = 0; i < 4; i++) {
        SCRUB(handControl.clingPlanes[i]);
        SCRUB(actorPlane[i]);
    }
    SCRUB(handControl.suctionPlane);
    SCRUB(corner);
    SCRUB(edgePlane);
    for (group = garibGroupList.head.next; group != &garibGroupList.head; group = group->next) {
        for (garib = group->garibs.head.next; garib != &group->garibs.head; garib = garib->next) {
            SCRUB(garib->normal);
        }
    }
    for (tip = tipList.head.next; tip != &tipList.head; tip = tip->next) {
        SCRUB(tip->normal);
    }
    /* A footprint on a body is left to KillPlatform, which turns the glove's into a footprint in world space and
       removes the others. */
    for (fp = footprintList.head.next; fp != &footprintList.head; fp = fp->next) {
        if (!InRanges((const void *)fp->parent) && InRanges((const void *)fp->normalPtr)) {
            fp->normal = *fp->normalPtr;
            fp->normalPtr = NULL;
            fp->parent = NULL;
        }
    }
}

static void AddRange(void *p, unsigned int bytes) {
    ranges[rangeCount].lo = (const char *)p;
    ranges[rangeCount].hi = (const char *)p + bytes;
    rangeCount++;
}

/* ---- The level's own collision. */

/* Takes the level's landscape and water out of collisionList (they stay in actorList), empties its water boxes, and
   makes the level's platforms pass-through with the game's flag 0x80. Boss and bonus levels find platforms by their
   tags in platformList, and waters by their tags in waterList, so both stay in their lists. */
/* Makes room for one more entry in a list of the host's memory. */
static void *Grow(void *list, unsigned int *cap, unsigned int count, size_t size) {
    if (count < *cap) {
        return list;
    }
    *cap = *cap ? 2 * *cap : 64;
    list = realloc(list, *cap * size);
    if (list == NULL) {
        port_fatal("hostworld: out of memory");
    }
    return list;
}

static int IsHostWater(const Water *w) {
    return hw.waterBlock != NULL && w >= hw.waterBlock && w < hw.waterBlock + hw.waterCount;
}

static void TakeLevel(void) {
    Actor *a;
    Actor *next;
    Platform *p;
    Water *w;
    unsigned int i;
    int ours;

    for (a = collisionList.head.collNext; a != &collisionList.head; a = next) {
        next = a->collNext;
        ours = 0;
        for (i = 0; i < hw.chunkCount; i++) {
            ours |= a == hw.chunks[i].actor;
        }
        if (!ours && (a->type == LANDSCAPE_ACTOR || a->type == WATER_ACTOR)) {
            hw.takenActors = (Actor **)Grow(hw.takenActors, &hw.takenActorCap, hw.takenActorCount, sizeof(Actor *));
            a->collPrev->collNext = a->collNext;
            a->collNext->collPrev = a->collPrev;
            a->collNext = NULL;
            collisionList.count--;
            hw.takenActors[hw.takenActorCount++] = a;
        }
    }
    /* A water box floats the ball, splashes and fogs the camera. With a negative width it holds no point. The game
       changes only its height. */
    for (w = waterList.head.next; w != &waterList.head; w = w->next) {
        if (w->box.size.x >= 0.0f && !IsHostWater(w)) {
            hw.takenWaters = (TakenWater *)Grow(hw.takenWaters, &hw.takenWaterCap, hw.takenWaterCount,
                                                 sizeof(TakenWater));
            hw.takenWaters[hw.takenWaterCount].water = w;
            hw.takenWaters[hw.takenWaterCount].width = w->box.size.x;
            hw.takenWaterCount++;
            w->box.size.x = -1.0f;
        }
    }
    for (p = platformList.head.next; p != &platformList.head; p = p->next) {
        ours = 0;
        for (i = 0; i < HW_MAX_BODIES; i++) {
            ours |= hw.bodies[i].id != 0 && p == hw.bodies[i].plat;
        }
        for (i = 0; i < hw.chunkCount; i++) {
            ours |= p == hw.chunks[i].plat;
        }
        if (!ours && !(p->flags & 0x80)) {
            hw.takenPlats = (Platform **)Grow(hw.takenPlats, &hw.takenPlatCap, hw.takenPlatCount, sizeof(Platform *));
            p->flags |= 0x80;
            hw.takenPlats[hw.takenPlatCount++] = p;
        }
    }
    hw.levelTaken = 1;
}

static void GiveLevelBack(void) {
    Platform *p;
    unsigned int i;

    for (i = 0; i < hw.takenActorCount; i++) {
        Actor *a = hw.takenActors[i];

        if (a->collNext == NULL) {
            a->collNext = &collisionList.head;
            a->collPrev = collisionList.head.collPrev;
            collisionList.head.collPrev->collNext = a;
            collisionList.head.collPrev = a;
            collisionList.count++;
        }
    }
    for (i = 0; i < hw.takenWaterCount; i++) {
        hw.takenWaters[i].water->box.size.x = hw.takenWaters[i].width;
    }
    for (p = platformList.head.next; p != &platformList.head; p = p->next) {
        for (i = 0; i < hw.takenPlatCount; i++) {
            if (p == hw.takenPlats[i]) {
                p->flags &= ~0x80;
            }
        }
    }
    hw.takenActorCount = hw.takenPlatCount = hw.takenWaterCount = 0;
    hw.levelTaken = 0;
}

/* ---- Building the static world. */

/* The part of the static block for one chunk: the actor (a platform and its shadow for a conveyor), its instance,
   object and mesh, the faces, the vertices and the indices. */
static unsigned int ChunkBytes(const HwChunk *k) {
    return Align16(k->conveyor ? sizeof(Platform) + sizeof(ActorShadow) : sizeof(Actor))
         + Align16(sizeof(ObjectInstance)) + Align16(sizeof(Object)) + Align16(sizeof(Mesh))
         + Align16(k->count * sizeof(CollFace)) + Align16(k->vertCount * sizeof(Vec3))
         + Align16(k->count * sizeof(MeshFace));
}

/* The game's inner-edge test (CheckForInnerEdges) for one pair of faces, plane before other in the order of a single
   mesh. */
static void InnerEdgePair(CollFace *plane, CollFace *other) {
    u8 conv[3] = { 0, 2, 1 };
    f32 t;
    u32 j;
    u32 k;
    u32 l;
    u32 n;

    if (DistanceBetweenPoints(&plane->edgeStart[0], &plane->edgeStart[1]) < 0.001
        || DistanceBetweenPoints(&plane->edgeStart[0], &plane->edgeStart[2]) < 0.001
        || DistanceBetweenPoints(&plane->edgeStart[2], &plane->edgeStart[1]) < 0.001) {
        return;
    }
    for (j = 0; j < 3; j++) {
        t = DOT_PRODUCT(&other->edgeStart[j], &plane->plane) + plane->plane.d;
        if (ABS(t) > 0.01) {
            return;
        }
    }
    for (j = 0; j < 3; j++) {
        for (k = 0; k < 3; k++) {
            if (DistanceBetweenPoints(&plane->edgeStart[j], &other->edgeStart[k]) < 0.01) {
                n = (j + 1) % 3;
                for (l = 0; l < 3; l++) {
                    if (DistanceBetweenPoints(&plane->edgeStart[n], &other->edgeStart[l]) < 0.01) {
                        plane->edgeLen[j] = -1.0f;
                        other->edgeLen[conv[k + l - 1]] = -1.0f;
                    }
                }
            }
        }
    }
}

typedef struct EdgeEntry {
    unsigned int a, b;               /* the edge's vertices, a < b */
    unsigned int chunk, face;
} EdgeEntry;

static int CompareEdges(const void *pa, const void *pb) {
    const EdgeEntry *x = (const EdgeEntry *)pa;
    const EdgeEntry *y = (const EdgeEntry *)pb;

    if (x->a != y->a) {
        return x->a < y->a ? -1 : 1;
    }
    if (x->b != y->b) {
        return x->b < y->b ? -1 : 1;
    }
    if (x->chunk != y->chunk) {
        return x->chunk < y->chunk ? -1 : 1;
    }
    return x->face < y->face ? -1 : x->face > y->face ? 1 : 0;
}

/* InitPlanes finds the inner edges within one chunk. This pass applies the same test to the faces of different
   chunks that share an edge, with the earlier chunk's face as the game's plane. */
static void SeamPass(void) {
    const HwCopy *c = &hw.stat;
    EdgeEntry *e = (EdgeEntry *)malloc(c->triCount * 3 * sizeof(EdgeEntry));
    unsigned int n = 0;
    unsigned int i;
    unsigned int j;
    unsigned int k;

    if (e == NULL || hw.chunkCount < 2) {
        free(e);
        return;
    }
    for (i = 0; i < hw.chunkCount; i++) {
        for (j = 0; j < hw.chunks[i].count; j++) {
            unsigned int t = hw.chunkTris[hw.chunks[i].first + j];

            for (k = 0; k < 3; k++) {
                unsigned int a = c->tris[3 * t + k];
                unsigned int b = c->tris[3 * t + (k + 1) % 3];

                e[n].a = a < b ? a : b;
                e[n].b = a < b ? b : a;
                e[n].chunk = i;
                e[n].face = j;
                n++;
            }
        }
    }
    qsort(e, n, sizeof(EdgeEntry), CompareEdges);
    for (i = 0; i < n; i = j) {
        for (j = i; j < n && e[j].a == e[i].a && e[j].b == e[i].b; j++) {
        }
        for (k = i; k < j; k++) {
            unsigned int m;

            for (m = k + 1; m < j; m++) {
                if (e[m].chunk != e[k].chunk) {
                    InnerEdgePair(&hw.chunks[e[k].chunk].actor->faces[e[k].face],
                                  &hw.chunks[e[m].chunk].actor->faces[e[m].face]);
                }
            }
        }
    }
    free(e);
}

static void LinkChunks(void) {
    Actor *prev = &collisionList.head;
    Vec3 points[3];
    float link = hw.opt.linkRadius;
    unsigned int i;
    int p;

    points[0] = hand.pos;
    points[1] = ball.pos;
    points[2] = camera.pos;
    hw.linked = 0;
    for (i = 0; i < hw.chunkCount; i++) {
        Actor *a = hw.chunks[i].actor;
        int want = link <= 0.0f;

        /* A conveyor chunk is a platform: the game finds it in platformList. */
        if (hw.chunks[i].conveyor) {
            continue;
        }
        if (a->collNext != NULL) {
            a->collPrev->collNext = a->collNext;
            a->collNext->collPrev = a->collPrev;
            a->collNext = NULL;
            collisionList.count--;
        }
        for (p = 0; p < 3 && !want; p++) {
            want = DistanceBetweenPoints(&points[p], &a->pos) - a->radius < link;
        }
        if (want) {
            a->collNext = prev->collNext;
            a->collPrev = prev;
            prev->collNext->collPrev = a;
            prev->collNext = a;
            collisionList.count++;
            prev = a;
            hw.linked++;
        }
    }
}

static unsigned int StaticBytes(void) {
    unsigned int bytes = 0;
    unsigned int i;

    for (i = 0; i < hw.chunkCount; i++) {
        bytes += ChunkBytes(&hw.chunks[i]);
    }
    return bytes;
}

static int BuildStatic(void) {
    Vec3 origin = { 0.0f, 0.0f, 0.0f };
    Vec3 one = { 1.0f, 1.0f, 1.0f };
    Quat identity = { 0.0f, 0.0f, 0.0f, 1.0f };
    const HwCopy *c = &hw.stat;
    unsigned int i;
    unsigned int t;

    char *p;
    clock_t start = clock();

    /* One block for all the chunks: the heap's block table has room for few more. */
    if (HeapFits(StaticBytes(), 1) != HW_OK) {
        return HW_ERR_MEMORY;
    }
    hw.block = (char *)JallocAlloc(StaticBytes(), 1, (char *)"HostWorld");
    p = hw.block;
    for (i = 0; i < hw.chunkCount; i++) {
        HwChunk *k = &hw.chunks[i];
        Actor *a;
        ObjectInstance *inst;
        Object *obj;
        Mesh *mesh;
        CollFace *faces;
        Vec3 *verts;
        s16 *idx;
        unsigned int *local;
        Platform *plat;
        Vec3 at;

        k->bytes = ChunkBytes(k);
        a = (Actor *)p;
        plat = k->conveyor ? (Platform *)p : NULL;
        if (plat != NULL) {
            a = &plat->actor;
        }
        p += Align16(k->conveyor ? sizeof(Platform) + sizeof(ActorShadow) : sizeof(Actor));
        inst = (ObjectInstance *)p;
        p += Align16(sizeof(ObjectInstance));
        obj = (Object *)p;
        p += Align16(sizeof(Object));
        mesh = (Mesh *)p;
        p += Align16(sizeof(Mesh));
        faces = (CollFace *)p;
        p += Align16(k->count * sizeof(CollFace));
        verts = (Vec3 *)p;
        p += Align16(k->vertCount * sizeof(Vec3));
        idx = (s16 *)p;
        p += Align16(k->count * sizeof(MeshFace));
        /* The vertices keep their coordinates (no offset to the chunk's centre): the planes are then the same, bit
           for bit, however the triangles are cut into chunks. */
        local = (unsigned int *)malloc(c->vertCount * sizeof(unsigned int));
        if (local == NULL) {
            port_fatal("hostworld: out of memory");
        }
        /* A platform's vertices are in its own space: a conveyor chunk's are relative to its centre, rounded to whole
           units. */
        at.x = plat != NULL ? (f32)floor(k->centre.x + 0.5) : 0.0f;
        at.y = plat != NULL ? (f32)floor(k->centre.y + 0.5) : 0.0f;
        at.z = plat != NULL ? (f32)floor(k->centre.z + 0.5) : 0.0f;
        for (t = 0; t < k->vertCount; t++) {
            unsigned int v = hw.chunkVerts[k->vertFirst + t];

            verts[t].x = c->verts[3 * v] - at.x;
            verts[t].y = c->verts[3 * v + 1] - at.y;
            verts[t].z = c->verts[3 * v + 2] - at.z;
            local[v] = t;
        }
        for (t = 0; t < k->count; t++) {
            unsigned int tri = hw.chunkTris[k->first + t];

            idx[3 * t] = (s16)local[c->tris[3 * tri]];
            idx[3 * t + 1] = (s16)local[c->tris[3 * tri + 1]];
            idx[3 * t + 2] = (s16)local[c->tris[3 * tri + 2]];
        }
        free(local);
        mesh->numFaces = (s16)k->count;
        mesh->numVerts = (s16)k->vertCount;
        mesh->verts = verts;
        mesh->faces = (MeshFace *)idx;
        obj->mesh = mesh;
        inst->obj = obj;
        if (plat != NULL) {
            const float *v = &c->conveyors[3 * (k->conveyor - 1)];

            /* As a body (BuildBody), with the game's conveyor flag 0x200 and its speed. */
            plat->flags = 0x200;
            plat->conveyorVel.x = v[0];
            plat->conveyorVel.y = v[1];
            plat->conveyorVel.z = v[2];
            a->inst = inst;
            a->faces = faces;
            a->shadow = (ActorShadow *)(plat + 1);
            a->type = PLAT_ACTOR;
            a->flags = 0x10004 | ((k->flags & HW_CAMERA_PASS) ? 0x20000000 : 0);
            a->active = 1;
            a->alpha = 100;
            a->friction = 1.0f;
            a->scale = one;
            a->rotQuat = identity;
            a->pos = at;
            a->prevPos = at;
            AddPlatform(plat);
            InitPlatformInfo(plat, 0.0f);
            for (t = 0; t < k->count; t++) {
                faces[t].terrain = c->terrain[hw.chunkTris[k->first + t]];
            }
            k->actor = a;
            k->plat = plat;
            continue;
        }
        a->type = LANDSCAPE_ACTOR;
        a->active = 1;
        a->alpha = 100;
        a->inst = inst;
        a->faces = faces;
        a->scale = one;
        a->rotQuat = identity;
        a->pos = k->centre;
        a->prevPos = k->centre;
        a->flags = (k->flags & HW_CAMERA_PASS) ? 0x20000000 : 0;
        InitPlanes(mesh, &origin, faces, &one, &identity, 1);
        a->radius = k->radius;
        for (t = 0; t < k->count; t++) {
            faces[t].terrain = c->terrain[hw.chunkTris[k->first + t]];
        }
        k->actor = a;
    }
    hw.lowest = c->verts[1];
    for (t = 0; t < c->vertCount; t++) {
        hw.lowest = c->verts[3 * t + 1] < hw.lowest ? c->verts[3 * t + 1] : hw.lowest;
    }
    SeamPass();
    if (!hw.opt.keepLevel) {
        TakeLevel();
    }
    LinkChunks();
    hw.built = 1;
    hw.buildUs = (unsigned int)((double)(clock() - start) * 1e6 / CLOCKS_PER_SEC);
    return HW_OK;
}

static void ForgetObjPlatform(ActorObject *obj, Platform *platform) {
    if (obj->platform == platform) {
        obj->platform = NULL;
    }
    if (obj->prevPlatform == platform) {
        obj->prevPlatform = NULL;
    }
    if (obj->contactPlatform == platform) {
        obj->contactPlatform = NULL;
    }
    if (obj->prevContactPlatform == platform) {
        obj->prevContactPlatform = NULL;
    }
}

/* Clears the game's pointers to a platform that the library frees itself, as the game's KillPlatform does, and takes it
   out of platformList: the platforms of the actors and the debris, the footprints (the glove's goes into world space,
   the others are removed) and the glove's platforms. */
static void ForgetPlatform(Platform *platform) {
    Footprint *fp;
    Footprint *next;
    Actor *a;
    Actor *own[2] = { &hand, &ball };
    int i;

    for (fp = footprintList.head.next; fp != &footprintList.head; fp = next) {
        next = fp->next;
        if (fp->parent == platform) {
            if (fp == handControl.footprint) {
                RotateVectorByQuaternion(&fp->pos, &fp->pos, &platform->actor.rotQuat);
                ADD_TO_VECTOR(&fp->pos, &platform->actor.pos);
                fp->normal = *fp->normalPtr;
                fp->parent = NULL;
                fp->normalPtr = NULL;
            } else {
                RemoveFootprint(fp);
            }
        }
    }
    for (i = 0; i < 2; i++) {
        if (own[i]->obj != NULL) {
            ForgetObjPlatform(own[i]->obj, platform);
        }
    }
    for (a = actorList.head.next; a != &actorList.head; a = a->next) {
        if (a->obj != NULL) {
            ForgetObjPlatform(a->obj, platform);
        }
    }
    for (i = 0; i < 120; i++) {
        if (debris[i].active && debris[i].obj != NULL) {
            ForgetObjPlatform(debris[i].obj, platform);
        }
    }
    if (handControl.climbPlatform == platform) {
        handControl.climbPlatform = NULL;
    }
    if (handControl.lastGroundPlatform == platform) {
        handControl.lastGroundPlatform = NULL;
    }
    if (handControl.edgePlatform == platform) {
        handControl.edgePlatform = NULL;
    }
    if (handControl.catapult == platform) {
        handControl.catapult = NULL;
    }
    SubPlatform(platform);
}

/* Unlinks and frees the built static world. With freeIt 0 the blocks are already gone (a level reload). With intact 1
   the game's lists still hold the conveyor chunks' platforms: they leave platformList. */
static void TearDownStatic(int freeIt, int intact) {
    unsigned int i;

    if (!hw.built) {
        return;
    }
    if (intact) {
        for (i = 0; i < hw.chunkCount; i++) {
            if (hw.chunks[i].plat != NULL) {
                ForgetPlatform(hw.chunks[i].plat);
            }
        }
    }
    if (freeIt) {
        Range whole;

        for (i = 0; i < hw.chunkCount; i++) {
            Actor *a = hw.chunks[i].actor;

            if (a->collNext != NULL) {
                a->collPrev->collNext = a->collNext;
                a->collNext->collPrev = a->collPrev;
                a->collNext = NULL;
                collisionList.count--;
            }
        }
        ranges = &whole;
        rangeCount = 0;
        AddRange(hw.block, StaticBytes());
        ScrubPointers();
        JallocFree(&hw.block);
        ranges = NULL;
        rangeCount = 0;
        if (hw.levelTaken) {
            GiveLevelBack();
        }
    }
    for (i = 0; i < hw.chunkCount; i++) {
        hw.chunks[i].actor = NULL;
        hw.chunks[i].plat = NULL;
    }
    hw.block = NULL;
    hw.built = 0;
    hw.linked = 0;
}

/* ---- Bodies. */

static unsigned int BodyBytes(const HwBody *b) {
    return Align16(sizeof(Platform)) + Align16(sizeof(ObjectInstance)) + Align16(sizeof(Object)) + Align16(sizeof(Mesh))
         + Align16(b->mesh.vertCount * sizeof(Vec3)) + Align16(b->mesh.triCount * sizeof(MeshFace))
         + Align16(sizeof(PlatformMotion)) + Align16(sizeof(ActorShadow));
}

static void SetPose(Platform *p, const HwPose *pose) {
    p->actor.pos.x = pose->pos[0];
    p->actor.pos.y = pose->pos[1];
    p->actor.pos.z = pose->pos[2];
    p->actor.rotQuat.x = pose->rot[0];
    p->actor.rotQuat.y = pose->rot[1];
    p->actor.rotQuat.z = pose->rot[2];
    p->actor.rotQuat.w = pose->rot[3];
    p->actor.scale.x = pose->scale[0];
    p->actor.scale.y = pose->scale[1];
    p->actor.scale.z = pose->scale[2];
}

/* HW_OK when no edge of the body is shorter than MIN_EDGE at this pose's scale. InitPlatformInfo runs the game's
   inner-edge test on the scaled faces. */
static int BodyEdgesFit(const HwBody *b, const HwPose *pose) {
    unsigned int t;

    for (t = 0; t < b->mesh.triCount; t++) {
        if (ShortestEdge(&b->mesh, t, pose->scale) < MIN_EDGE) {
            return HW_ERR_ARG;
        }
    }
    return HW_OK;
}

/* The game's conveyor (UpdateActorPlatform, AddConveyorSpeed): with flag 0x200, what stands on the platform moves by
   conveyorVel per original frame. With a motion the game turns the speed by the platform's rotation. */
static void SetBodyConveyor(HwBody *b, Platform *plat) {
    if (b->hasConveyor) {
        plat->flags |= 0x200;
        plat->conveyorVel.x = b->conveyor[0];
        plat->conveyorVel.y = b->conveyor[1];
        plat->conveyorVel.z = b->conveyor[2];
    } else {
        plat->flags &= ~0x200;
        plat->conveyorVel.x = plat->conveyorVel.y = plat->conveyorVel.z = 0.0f;
    }
}

static int BuildBody(HwBody *b) {
    unsigned int faceBytes = b->mesh.triCount * sizeof(CollFace);
    unsigned int flags = 0;
    char *p;
    Platform *plat;
    ObjectInstance *inst;
    Object *obj;
    Mesh *mesh;
    Vec3 *verts;
    s16 *idx;
    unsigned int t;
    int j;

    if (BodyEdgesFit(b, &b->cur) != HW_OK) {
        return HW_ERR_ARG;
    }
    b->bytes = BodyBytes(b) + Align16(faceBytes);
    if (HeapFits(b->bytes, 2) != HW_OK) {
        return HW_ERR_MEMORY;
    }
    p = (char *)JallocAlloc(BodyBytes(b), 1, (char *)"HostBody");
    plat = (Platform *)p;
    p += Align16(sizeof(Platform));
    inst = (ObjectInstance *)p;
    p += Align16(sizeof(ObjectInstance));
    obj = (Object *)p;
    p += Align16(sizeof(Object));
    mesh = (Mesh *)p;
    p += Align16(sizeof(Mesh));
    verts = (Vec3 *)p;
    p += Align16(b->mesh.vertCount * sizeof(Vec3));
    idx = (s16 *)p;
    p += Align16(b->mesh.triCount * sizeof(MeshFace));
    /* JallocAlloc cleared it: no speed, no limits. The game's SpinPlatform and RotatePlatform leave it as it is. */
    b->motion = (PlatformMotion *)p;
    b->motion->damping = 1.0f;
    p += Align16(sizeof(PlatformMotion));
    /* A shadow with no floor, which the game never draws: on level 24 ProcessShadows reads the shadow of every
       platform above the water. */
    b->shadow = (ActorShadow *)p;
    for (t = 0; t < b->mesh.vertCount; t++) {
        verts[t].x = b->mesh.verts[3 * t];
        verts[t].y = b->mesh.verts[3 * t + 1];
        verts[t].z = b->mesh.verts[3 * t + 2];
    }
    for (t = 0; t < 3 * b->mesh.triCount; t++) {
        idx[t] = (s16)b->mesh.tris[t];
    }
    for (t = 0; t < b->mesh.triCount; t++) {
        flags |= b->mesh.flags[t];
    }
    mesh->numFaces = (s16)b->mesh.triCount;
    mesh->numVerts = (s16)b->mesh.vertCount;
    mesh->verts = verts;
    mesh->faces = (MeshFace *)idx;
    obj->mesh = mesh;
    inst->obj = obj;
    plat->actor.inst = inst;
    plat->actor.faces = (CollFace *)JallocAlloc(faceBytes, 1, (char *)"HostFaces");
    plat->actor.type = PLAT_ACTOR;
    plat->actor.shadow = b->shadow;
    plat->actor.flags = 0x10004 | ((flags & HW_CAMERA_PASS) ? 0x20000000 : 0);
    plat->actor.active = 1;
    plat->actor.alpha = 100;
    plat->actor.friction = 1.0f;
    SetPose(plat, &b->cur);
    plat->actor.prevPos = plat->actor.pos;
    AddPlatform(plat);
    InitPlatformInfo(plat, 0.0f);
    SetBodyConveyor(b, plat);
    for (t = 0; t < b->mesh.triCount; t++) {
        plat->actor.faces[t].terrain = b->mesh.terrain[t];
        b->inner[t] = 0;
        for (j = 0; j < 3; j++) {
            if (plat->actor.faces[t].edgeLen[j] < 0.0f) {
                b->inner[t] |= 1 << j;
            }
        }
    }
    b->plat = plat;
    return HW_OK;
}

/* Clears the game's pointers to a body and frees it with the game's KillPlatform. */
static void KillBody(HwBody *b) {
    Platform *p = b->plat;

    ranges = (Range *)malloc(2 * sizeof(Range));
    if (ranges == NULL) {
        port_fatal("hostworld: out of memory");
    }
    rangeCount = 0;
    AddRange(p, BodyBytes(b));
    AddRange(p->actor.faces, b->mesh.triCount * sizeof(CollFace));
    ScrubPointers();
    free(ranges);
    ranges = NULL;
    rangeCount = 0;
    if (handControl.climbPlatform == p) {
        handControl.climbPlatform = NULL;
    }
    if (handControl.lastGroundPlatform == p) {
        handControl.lastGroundPlatform = NULL;
    }
    if (handControl.edgePlatform == p) {
        handControl.edgePlatform = NULL;
    }
    if (handControl.catapult == p) {
        handControl.catapult = NULL;
    }
    /* The motion and the shadow lie in the body's block: KillPlatform must not free them on their own. */
    p->motion = NULL;
    p->actor.shadow = NULL;
    KillPlatform(p, 0);
    b->plat = NULL;
    b->motion = NULL;
}

/* After a change of scale, or a return to no rotation (where RecalculatePlatformInfo keeps the normals it has),
   the faces are built again in full, and the inner edges that InitPlatformInfo found are marked again. */
static void RebuildBodyFaces(HwBody *b) {
    Platform *p = b->plat;
    Mesh *mesh = p->actor.inst->obj->mesh;
    f32 longest = 0.0f;
    unsigned int t;
    int j;

    CalculatePlatformInfo(p, 0.0f);
    for (t = 0; t < b->mesh.triCount; t++) {
        for (j = 0; j < 3; j++) {
            if (b->inner[t] & (1 << j)) {
                p->actor.faces[t].edgeLen[j] = -1.0f;
            }
        }
    }
    for (t = 0; t < (unsigned int)mesh->numVerts; t++) {
        Vec3 v = mesh->verts[t];
        f32 d;

        v.x *= p->actor.scale.x;
        v.y *= p->actor.scale.y;
        v.z *= p->actor.scale.z;
        d = LENGTH(&v);
        longest = d > longest ? d : longest;
    }
    p->actor.radius = longest;
}

static int IsIdentity(const float rot[4]) {
    return rot[0] == 0.0f && rot[1] == 0.0f && rot[2] == 0.0f && rot[3] == 1.0f;
}

/* The turn of a rotation about y (its twist), in [0, 2 pi). */
static f32 Yaw(const float rot[4]) {
    return Aabs((f32)(2.0 * port_atan2(rot[1], rot[3])));
}

/* The game's carry turns what stands on a platform by motion->rot.y - motion->oldRot.y (UpdateActorPlatform): its
   offset, its velocity and its yaw. A body gets the motion while it is turned; a body that only moves keeps none, and
   RecalculatePlatformInfo then keeps its fast path. Only the turn about y is carried: the game's carry turns about one
   axis. */
static void SetTurn(HwBody *b, const HwPose *t, int carry) {
    Platform *p = b->plat;
    f32 from = Yaw(b->cur.rot);
    f32 d = Yaw(t->rot) - from;

    if (IsIdentity(b->cur.rot) && IsIdentity(t->rot)) {
        p->motion = NULL;
        return;
    }
    p->motion = b->motion;
    /* The shorter way round. */
    if (d > PI) {
        d -= TWOPI;
    } else if (d < -PI) {
        d += TWOPI;
    }
    b->motion->rot.y = from + d;
    /* Without a carry nothing on the body turns. */
    b->motion->oldRot.y = carry ? from : b->motion->rot.y;
}

/* Puts a body at its target pose. With carry 0 the body jumps there with no velocity, and nothing on it is carried. */
static void MoveBody(HwBody *b, int carry) {
    Platform *p = b->plat;
    HwPose *t = b->moved ? &b->target : &b->cur;
    int full;

    full = memcmp(t->scale, b->cur.scale, sizeof(t->scale)) != 0
        || (t->rot[3] == 1.0f && memcmp(t->rot, b->cur.rot, sizeof(t->rot)) != 0);
    p->actor.prevPos.x = b->cur.pos[0];
    p->actor.prevPos.y = b->cur.pos[1];
    p->actor.prevPos.z = b->cur.pos[2];
    SetPose(p, t);
    if (carry) {
        /* The game's carry reads the velocity per original frame. */
        p->actor.vel.x = DeltaPerFrame(t->pos[0] - b->cur.pos[0]);
        p->actor.vel.y = DeltaPerFrame(t->pos[1] - b->cur.pos[1]);
        p->actor.vel.z = DeltaPerFrame(t->pos[2] - b->cur.pos[2]);
    } else {
        p->actor.prevPos = p->actor.pos;
        p->actor.vel.x = p->actor.vel.y = p->actor.vel.z = 0.0f;
    }
    p->actor.speed = LENGTH(&p->actor.vel);
    SetTurn(b, t, carry);
    if (full) {
        RebuildBodyFaces(b);
    } else {
        RecalculatePlatformInfo(p);
    }
    b->cur = *t;
    b->moved = 0;
}

static void MoveBodies(void) {
    int i;

    for (i = 0; i < HW_MAX_BODIES; i++) {
        if (hw.bodies[i].id != 0 && hw.bodies[i].plat != NULL) {
            MoveBody(&hw.bodies[i], 1);
        }
    }
}

static void FreeBody(HwBody *b) {
    FreeCopy(&b->mesh);
    free(b->inner);
    memset(b, 0, sizeof(*b));
}

/* ---- Voids. */

#define FACE_STATIC 0x100
#define FACE_BODY 0x200

/* What p points at: 0 when it is not a face of the host's world, else FACE_STATIC or FACE_BODY with the triangle's
   HW_TRI_FLAGS bits. */
static int HostFace(const void *p) {
    const char *q = (const char *)p;
    unsigned int lo = 0;
    unsigned int hi = hw.chunkCount;
    unsigned int i;

    if (q == NULL) {
        return 0;
    }
    if (hw.built && hw.chunkCount > 0) {
        /* The chunks' faces lie in one block, in the order of the chunks. */
        while (hi - lo > 1) {
            unsigned int mid = (lo + hi) / 2;

            if ((const char *)hw.chunks[mid].actor->faces <= q) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        {
            const HwChunk *k = &hw.chunks[lo];
            const char *f = (const char *)k->actor->faces;

            if (q >= f && q < f + k->count * sizeof(CollFace) && (q - f) % sizeof(CollFace) == 0) {
                return FACE_STATIC | hw.stat.flags[hw.chunkTris[k->first + (q - f) / sizeof(CollFace)]];
            }
        }
    }
    for (i = 0; i < HW_MAX_BODIES; i++) {
        const HwBody *b = &hw.bodies[i];
        const char *f;

        if (b->id == 0 || b->plat == NULL) {
            continue;
        }
        f = (const char *)b->plat->actor.faces;
        if (q >= f && q < f + b->mesh.triCount * sizeof(CollFace) && (q - f) % sizeof(CollFace) == 0) {
            return FACE_BODY | b->mesh.flags[(q - f) / sizeof(CollFace)];
        }
    }
    return 0;
}

/* 2 when the actor touches a void triangle, 1 when the floor under it is one, else 0. */
static int VoidState(Actor *a) {
    int k;

    if (a->obj == NULL) {
        return 0;
    }
    for (k = 0; k < 4; k++) {
        if (HostFace(a->obj->planes[k]) & HW_VOID) {
            return 2;
        }
    }
    if (a->shadow != NULL && (HostFace(a->shadow->floor) & HW_VOID)) {
        return 1;
    }
    return 0;
}

/* Over a void, or in one, the game's fall counters stay at 0. The game hurts the glove when it lands after a long
   fall (handControl.fallTime), kills it when it falls with no floor under it, and bursts the ball after a long fall
   (ballControl.fallTime). A void instead returns the glove and the ball to the host, which places them. This runs
   after the game's restart and before the glove and the ball move. */
static void HoldFallCounters(void) {
    int ballVoid = VoidState(&ball);

    if (VoidState(&hand) || ((ball.link == &hand || hand.link == &ball) && ballVoid)) {
        handControl.fallTime = 0;
    }
    if (ballVoid) {
        ballControl.fallTime = 0;
    }
}

/* A crystal ball does not shatter on a void: the host places it back. */
static s32 NoShatter(void *face) {
    return (HostFace(face) & HW_VOID) != 0;
}

/* After each update: the void states, the touches of a void, and the latest safe places. A safe place is where the
   glove or the ball stood on a static triangle that is not a void, death (6) or hurting (9). */
static void PostUpdate(void) {
    Actor *bodies[2] = { &hand, &ball };
    int states[2];
    int i;

    if (!GameBeingPlayed() || hand.state == NULL) {
        hw.voidState[0] = hw.voidState[1] = 0;
        return;
    }
    states[0] = VoidState(&hand);
    states[1] = VoidState(&ball);
    /* The glove that holds the ball, or stands on it, falls with it: it reports the ball's void as its own. */
    if ((ball.link == &hand || hand.link == &ball) && states[1] > states[0]) {
        states[0] = states[1];
    }
    for (i = 0; i < 2; i++) {
        Actor *a = bodies[i];
        int state = states[i];
        int face;

        if (state == 2 && hw.voidState[i] != 2) {
            hw.voids[i]++;
        }
        hw.voidState[i] = state;
        if (a->obj == NULL || !a->obj->onGround || state != 0) {
            continue;
        }
        face = HostFace(a->obj->planes[0]);
        if ((face & FACE_STATIC) && ((CollFace *)a->obj->planes[0])->terrain != 6
            && ((CollFace *)a->obj->planes[0])->terrain != 9) {
            hw.safe[i][0] = a->pos.x;
            hw.safe[i][1] = a->pos.y;
            hw.safe[i][2] = a->pos.z;
            hw.safeKnown |= 1 << i;
        }
    }
}

/* ---- The level's own enemies, collectables and ambient sounds. */

extern Vec3 zero;

/* Takes an enemy out of the game as KillEnemy does, without its score, its garib, its sounds and its effects: it
   lets go of the ball and the glove, and leaves actorList and collisionList. UpdateEnemy then skips it. A restart
   (RestartEnemy) or a puzzle brings it back, and the next sweep takes it out again. */
static void SwitchOffEnemy(Enemy *e) {
    if (ball.link == &e->actor) {
        DropBall(5);
        ball.flags |= 0x18;
    }
    if (handControl.attachedTo == &e->actor) {
        EnemyDropHand();
    }
    if (e->samples[0].handle) {
        StopContinuousSample(&e->samples[0]);
    }
    if (e->samples[1].handle) {
        StopContinuousSample(&e->samples[1]);
    }
    e->actor.active = 0;
    SubActor(&e->actor);
}

/* Switches off what HwOptions.levelOff names, while a static world is in: the enemies, the garibs, extra lives
   and potions, and the ambient sounds at points. A garib goes as when it is collected, without its score; a potion
   leaves actorList but stays in collectableList, where the puzzles look for it. An ambient sound at a point stops,
   and becomes one of the level's sounds without a place, as ClearAmbientSfx leaves them: UpdateAmbientSounds then
   skips its timers. It stays in ambientSoundList, where the puzzles look for it. This runs before each update and
   again after the game's restart, which brings the enemies back. */
static s32 PointSoundOff(void) {
    return hw.wantStatic && (hw.opt.levelOff & HW_OFF_AMBIENT) && GameBeingPlayed();
}

static void SwitchOffLevel(void) {
    GaribGroup *group;
    Garib *garib;
    Collectable *c;
    AmbientSound *sound;
    s32 i;

    if (!hw.wantStatic || hw.opt.levelOff == 0 || !GameBeingPlayed()) {
        return;
    }
    if (hw.opt.levelOff & HW_OFF_ENEMIES) {
        for (i = 0; i < numberOfEnemies; i++) {
            Enemy *e = &enemies[i];

            if (e->actor.active != 0 || e->actor.next != NULL || e->actor.collNext != NULL) {
                SwitchOffEnemy(e);
            }
        }
    }
    if (hw.opt.levelOff & HW_OFF_COLLECTABLES) {
        for (group = garibGroupList.head.next; group != &garibGroupList.head; group = group->next) {
            for (garib = group->garibs.head.next; garib != &group->garibs.head; garib = garib->next) {
                if (garib->active != 0) {
                    garib->active = 0;
                    garib->shadowTarget = 0;
                    SubSprite(&garib->sprite);
                }
            }
        }
        for (c = collectableList.head.next; c != &collectableList.head; c = c->next) {
            if (c->active != 0 || c->timer != 0 || c->actor.next != NULL) {
                c->active = 0;
                c->timer = 0;
                if (c->actor.shadow != NULL) {
                    c->actor.shadow->shadowTarget = 0;
                }
                SubActor(&c->actor);
            }
        }
    }
    if (hw.opt.levelOff & HW_OFF_AMBIENT) {
        for (sound = ambientSoundList.head.next; sound != &ambientSoundList.head; sound = sound->next) {
            if (sound->sample.pos != &zero) {
                StopContinuousSample(&sound->sample);
                sound->sample.pos = &zero;
            }
        }
    }
}

/* Without the level's landscape and water, a bullet that would have hit them may fall past the host's world. Level
   8 makes drops that live 1000 original frames, one a frame, and they fill the heap's block table. A bullet 500
   units below the lowest point of the static world ends in the next update, as when it hits something: its
   lifeTimer becomes 1. */
static void EndLostBullets(void) {
    Bullet *b;

    if (!hw.levelTaken || !hw.built) {
        return;
    }
    for (b = bulletList.head.next; b != &bulletList.head; b = b->next) {
        if (b->actor.pos.y < hw.lowest - 500.0f) {
            b->actor.lifeTimer = 1;
        }
    }
}

/* Right after the game's UpdatePlatforms, in a played and unpaused game. */
static void AfterPlatforms(void) {
    MoveBodies();
    HoldFallCounters();
    SwitchOffLevel();
    EndLostBullets();
}

/* ---- Water. */

/* The game's Water of each host water, in one heap block, at the tail of waterList: the level's own waters come first,
   and FindTaggedWater finds them by their tags as before. The tag of a host water is -1. */
static int BuildWaters(void) {
    unsigned int i;

    if (HeapFits(hw.waterCount * sizeof(Water), 1) != HW_OK) {
        return HW_ERR_MEMORY;
    }
    hw.waterBlock = (Water *)JallocAlloc(hw.waterCount * sizeof(Water), 1, (char *)"HostWater");
    for (i = 0; i < hw.waterCount; i++) {
        const HwWater *h = &hw.waters[i];
        Water *w = &hw.waterBlock[i];

        w->box.pos.x = h->min[0];
        w->box.pos.y = h->min[1];
        w->box.pos.z = h->min[2];
        w->box.size.x = h->size[0];
        w->box.size.y = h->size[1];
        w->box.size.z = h->size[2];
        w->height = w->targetHeight = h->surface;
        w->currentX = h->current[0];
        w->currentY = h->current[1];
        w->currentZ = h->current[2];
        w->tag = -1;
        /* The level's waters get the same rule when they are read (TOKEN_WATER in parse.c): the box reaches at least
           25 units below and above the surface. */
        if (w->height - 25.0f < w->box.pos.y) {
            w->box.pos.y = w->height - 25.0f;
        }
        if (w->box.size.y < w->height + 25.0f - w->box.pos.y) {
            w->box.size.y = w->height + 25.0f - w->box.pos.y;
        }
        w->next = &waterList.head;
        w->prev = waterList.head.prev;
        waterList.head.prev->next = w;
        waterList.head.prev = w;
        waterList.count++;
    }
    return HW_OK;
}

/* Takes the host's waters out of waterList. With intact 0 the heap was set up again and the list with it: nothing of
   the old heap is written. With freeIt 1 the block is freed too. */
static void TearDownWaters(int intact, int freeIt) {
    unsigned int i;

    if (hw.waterBlock == NULL) {
        return;
    }
    for (i = 0; intact && i < hw.waterCount; i++) {
        Water *w = &hw.waterBlock[i];

        w->prev->next = w->next;
        w->next->prev = w->prev;
        w->next = w->prev = NULL;
        waterList.count--;
    }
    if (freeIt) {
        JallocFree(&hw.waterBlock);
    }
    hw.waterBlock = NULL;
}

/* ---- Readiness and the steps of each update. */

/* In a level, the glove set up, and no restart running. */
int hw_ready(void) {
    Platform *cp;

    if (!GameBeingPlayed() || hand.state == NULL || hand.obj == NULL) {
        return 0;
    }
    cp = handControl.checkpoint;
    return cp == NULL || cp->restartGate == NULL || cp->restartGate->restartTimer == 0;
}

static void Activate(void) {
    if (!hw.active) {
        hw.active = 1;
        hw.heapInits = portHeapInits;
        port_pre_update_hook = PreUpdate;
        port_platforms_hook = AfterPlatforms;
        port_free_level_hook = LevelFreed;
        port_post_update_hook = PostUpdate;
        port_no_shatter_hook = NoShatter;
        port_point_sound_off_hook = PointSoundOff;
    }
}

static int AnyBody(void) {
    int i;

    for (i = 0; i < HW_MAX_BODIES; i++) {
        if (hw.bodies[i].id != 0) {
            return 1;
        }
    }
    return 0;
}

/* The hooks go when nothing is left for them. */
static void MaybeDeactivate(void) {
    if (hw.active && !hw.wantStatic && !AnyBody() && !hw.placePending && !hw.respawnSet && hw.waterCount == 0
        && !(hw.viewOn && (hw.view.flags & HW_VIEW_STEER))) {
        hw.active = 0;
        port_pre_update_hook = NULL;
        port_platforms_hook = NULL;
        port_free_level_hook = NULL;
        port_post_update_hook = NULL;
        port_no_shatter_hook = NULL;
        port_point_sound_off_hook = NULL;
    }
}

static void ApplyPlace(const HwPlace *request) {
    Actor *bodies[2] = { &hand, &ball };
    HwPlace held = *request;
    const HwPlace *pl = request;
    const float *pos[2];
    const float *vel[2];
    int i;
    int k;

    /* A glove that holds the ball and is placed with it lets go, as when it turns into a frog (MaintainAction), and
       each goes to its place. Placed alone, either of the two takes the other along: both move by the same distance
       and get the same velocity. Without this the game's hold would pull them back together. IsHandHoldingBall leaves
       out the glove that walks on the ball (HAND_BALLWALK), whose link pulls it back onto the ball too. */
    int holds = IsHandHoldingBall() || (ball.link == &hand && hand.state->mode == HAND_BALLWALK);

    if ((request->what & HW_PLACE_HAND) && (request->what & HW_PLACE_BALL) && holds) {
        ReleaseBall();
        handControl.releaseTimer = 0;
        ball.link = NULL;
        hand.obj->spheres[0].radius = hand.obj->spheres[0].targetRadius = hand.phys->radius;
        ApplyAction(HAND_IDLE);
    } else if ((request->what & (HW_PLACE_HAND | HW_PLACE_BALL)) && holds) {
        int byHand = (request->what & HW_PLACE_HAND) != 0;
        const float *to = byHand ? request->hand : request->ball;
        const float *v = byHand ? request->handVel : request->ballVel;
        Actor *lead = byHand ? &hand : &ball;
        float d[3];

        d[0] = to[0] - lead->pos.x;
        d[1] = to[1] - lead->pos.y;
        d[2] = to[2] - lead->pos.z;
        held.hand[0] = hand.pos.x + d[0];
        held.hand[1] = hand.pos.y + d[1];
        held.hand[2] = hand.pos.z + d[2];
        held.ball[0] = ball.pos.x + d[0];
        held.ball[1] = ball.pos.y + d[1];
        held.ball[2] = ball.pos.z + d[2];
        memcpy(held.handVel, v, sizeof(held.handVel));
        memcpy(held.ballVel, v, sizeof(held.ballVel));
        held.what |= HW_PLACE_HAND | HW_PLACE_BALL;
        pl = &held;
    }
    pos[0] = pl->hand;
    pos[1] = pl->ball;
    vel[0] = pl->handVel;
    vel[1] = pl->ballVel;

    for (i = 0; i < 2; i++) {
        Actor *a = bodies[i];

        if (!(pl->what & (i == 0 ? HW_PLACE_HAND : HW_PLACE_BALL))) {
            continue;
        }
        a->pos.x = pos[i][0];
        a->pos.y = pos[i][1];
        a->pos.z = pos[i][2];
        a->prevPos = a->pos;
        if (pl->what & HW_PLACE_VEL) {
            a->vel.x = vel[i][0];
            a->vel.y = vel[i][1];
            a->vel.z = vel[i][2];
        } else {
            a->vel.x = a->vel.y = a->vel.z = 0.0f;
        }
        a->speed = LENGTH(&a->vel);
        if (a->obj != NULL) {
            for (k = 0; k < 4; k++) {
                a->obj->planes[k] = NULL;
            }
            a->obj->platform = NULL;
            a->obj->prevPlatform = NULL;
            a->obj->contactPlatform = NULL;
            a->obj->prevContactPlatform = NULL;
        }
        /* The glove's footprint leaves its platform as KillPlatform makes it: in world space. */
        if (i == 0 && handControl.footprint != NULL && handControl.footprint->parent != NULL) {
            Footprint *fp = handControl.footprint;

            RotateVectorByQuaternion(&fp->pos, &fp->pos, &fp->parent->actor.rotQuat);
            ADD_TO_VECTOR(&fp->pos, &fp->parent->actor.pos);
            fp->normal = *fp->normalPtr;
            fp->parent = NULL;
            fp->normalPtr = NULL;
        }
    }
    if (pl->what & HW_PLACE_YAW) {
        hand.rot.y = hand.rotAim.y = pl->handYaw;
    }
    if (pl->what & HW_PLACE_CAMERA) {
        RestartCamera();
        cameraInfo.actor.pos = hand.pos;
        cameraInfo.actor.prevPos = hand.pos;
        camera.pos.x = hand.pos.x - sinf(hand.rot.y) * 100.0f;
        camera.pos.y = hand.pos.y + 40.0f;
        camera.pos.z = hand.pos.z - cosf(hand.rot.y) * 100.0f;
        camera.prevPos = camera.pos;
        camera.vel.x = camera.vel.y = camera.vel.z = 0.0f;
    }
}

static void Build(void) {
    int i;

    if (hw.waterCount > 0 && hw.waterBlock == NULL && BuildWaters() != HW_OK) {
        hw.failed++;
        free(hw.waters);
        hw.waters = NULL;
        hw.waterCount = 0;
    }
    if (hw.wantStatic && !hw.built) {
        if (BuildStatic() != HW_OK) {
            hw.wantStatic = 0;
            hw.failed++;
            FreeChunkPlan();
            FreeCopy(&hw.stat);
        }
    }
    for (i = 0; i < HW_MAX_BODIES; i++) {
        HwBody *b = &hw.bodies[i];

        if (b->id != 0 && b->plat == NULL && BuildBody(b) != HW_OK) {
            hw.failed++;
            FreeBody(b);
        }
    }
}

static void PreUpdate(void) {
    int i;

    /* The heap was set up again without a level reload (the title): what was built is gone. */
    if (portHeapInits != hw.heapInits) {
        WorldLost(0);
        hw.heapInits = portHeapInits;
    }
    /* The game does not move its platforms while it is paused. A body moved meanwhile jumps to its pose: a carry of
       the whole distance would throw what stands on it. */
    if (gamePaused && GameBeingPlayed()) {
        for (i = 0; i < HW_MAX_BODIES; i++) {
            if (hw.bodies[i].id != 0 && hw.bodies[i].plat != NULL && hw.bodies[i].moved) {
                MoveBody(&hw.bodies[i], 0);
            }
        }
    }
    SwitchOffLevel();
    if (hw.viewOn && (hw.view.flags & HW_VIEW_STEER) && GameBeingPlayed()) {
        camera.pos.x = camera.prevPos.x = hw.view.eye[0];
        camera.pos.y = camera.prevPos.y = hw.view.eye[1];
        camera.pos.z = camera.prevPos.z = hw.view.eye[2];
        camera.vel.x = camera.vel.y = camera.vel.z = 0.0f;
        cameraInfo.actor.pos.x = hw.view.at[0];
        cameraInfo.actor.pos.y = hw.view.at[1];
        cameraInfo.actor.pos.z = hw.view.at[2];
    }
    if (!hw_ready()) {
        return;
    }
    if (hw.lost) {
        Build();
        hw.lost = 0;
        hw.rebuilds++;
    } else {
        Build();
    }
    if (hw.placePending) {
        ApplyPlace(&hw.place);
        hw.placePending = 0;
    }
    if (hw.respawnPending && handControl.checkpoint != NULL && handControl.checkpoint->restartGate != NULL) {
        Platform *cp = handControl.checkpoint;

        cp->actor.pos.x = hw.respawnPos[0];
        cp->actor.pos.y = hw.respawnPos[1];
        cp->actor.pos.z = hw.respawnPos[2];
        cp->restartGate->yaw = hw.respawnYaw;
        cp->restartGate->cameraPos.x = hw.respawnPos[0] - sinf(hw.respawnYaw) * 100.0f;
        cp->restartGate->cameraPos.y = hw.respawnPos[1] + 60.0f;
        cp->restartGate->cameraPos.z = hw.respawnPos[2] - cosf(hw.respawnYaw) * 100.0f;
        hw.respawnPending = 0;
    }
    if (hw.built && hw.opt.linkRadius > 0.0f) {
        LinkChunks();
    }
    MaybeDeactivate();
}

/* What was built is gone, or about to go. With intact 1 the level's memory is about to be freed (FreeLevel2): the
   game's own loop frees the bodies with KillPlatform, and JallocInit the chunks. With intact 0 the heap was set up again
   without it, and nothing of the old heap may be written. The host's copy stays, and the world is built again at the
   first ready update. */
static void WorldLost(int intact) {
    int any = hw.built || hw.waterBlock != NULL;
    int i;

    /* FreeLevel2 frees each water of waterList on its own: the host's leave the list first. */
    TearDownWaters(intact, 0);
    TearDownStatic(0, intact);
    hw.takenActorCount = hw.takenPlatCount = hw.takenWaterCount = 0;
    hw.levelTaken = 0;
    for (i = 0; i < HW_MAX_BODIES; i++) {
        if (hw.bodies[i].plat != NULL) {
            any = 1;
            /* The game's loop frees the body with KillPlatform, which must not free the motion and the shadow in its
               block. */
            if (intact) {
                hw.bodies[i].plat->motion = NULL;
                hw.bodies[i].plat->actor.shadow = NULL;
            }
            hw.bodies[i].plat = NULL;
            hw.bodies[i].motion = NULL;
        }
    }
    if (any) {
        hw.lost = 1;
    }
    /* The new level's checkpoint gets the host's respawn again. */
    hw.respawnPending = hw.respawnSet;
}

/* The hook at the start of FreeLevel2. */
static void LevelFreed(void) {
    WorldLost(1);
}

/* A call that frees what was built first notes a heap that was set up again since the latest update. */
static void CheckHeap(void) {
    if (hw.active && portHeapInits != hw.heapInits) {
        WorldLost(0);
        hw.heapInits = portHeapInits;
    }
}

/* ---- The interface. */

/* A static set in the host's memory: the copy, its options and its chunk plan. */
typedef struct StaticSet {
    HwCopy stat;
    HwOptions opt;
    HwChunk *chunks;
    unsigned int chunkCount;
    unsigned int *chunkTris;
    unsigned int *chunkVerts;
    int want;
} StaticSet;

/* Exchanges the set in hw with *s. What is built belongs to the set in hw. */
static void SwapStatic(StaticSet *s) {
    StaticSet t;

    t.stat = hw.stat;
    t.opt = hw.opt;
    t.chunks = hw.chunks;
    t.chunkCount = hw.chunkCount;
    t.chunkTris = hw.chunkTris;
    t.chunkVerts = hw.chunkVerts;
    t.want = hw.wantStatic;
    hw.stat = s->stat;
    hw.opt = s->opt;
    hw.chunks = s->chunks;
    hw.chunkCount = s->chunkCount;
    hw.chunkTris = s->chunkTris;
    hw.chunkVerts = s->chunkVerts;
    hw.wantStatic = s->want;
    *s = t;
}

/* Frees a set that is not in hw and not built. */
static void ReleaseStatic(StaticSet *s) {
    FreeCopy(&s->stat);
    free(s->chunks);
    free(s->chunkTris);
    free(s->chunkVerts);
    memset(s, 0, sizeof(*s));
}

/* Frees the set in hw, built or not. */
static void DropStatic(void) {
    TearDownStatic(1, 1);
    FreeChunkPlan();
    FreeCopy(&hw.stat);
    hw.wantStatic = 0;
}

/* A failed call leaves the static world as it was. */
int hw_set_static(const HwMesh *m, const HwOptions *o) {
    StaticSet old;
    int wasBuilt;
    int rc;

    CheckHeap();
    if (m == NULL) {
        DropStatic();
        MaybeDeactivate();
        return HW_OK;
    }
    if (hw_check_mesh(m, HW_MAX_STATIC_TRIS) != HW_OK || o == NULL || !(o->cellSize >= 0.0f)
        || o->maxChunkTris > HW_MAX_CHUNK_TRIS || !(o->linkRadius >= 0.0f) || o->keepLevel > 1
        || (o->levelOff & ~(HW_OFF_ENEMIES | HW_OFF_COLLECTABLES | HW_OFF_AMBIENT))) {
        return HW_ERR_ARG;
    }
    memset(&old, 0, sizeof(old));
    rc = CopyMesh(m, &old.stat);
    if (rc != HW_OK) {
        return rc;
    }
    if (old.stat.triCount == 0) {
        FreeCopy(&old.stat);
        return HW_ERR_ARG;
    }
    old.opt = *o;
    old.want = 1;
    /* hw now holds the new set and old the one in place. */
    SwapStatic(&old);
    rc = PlanChunks();
    if (rc != HW_OK) {
        FreeCopy(&hw.stat);
        SwapStatic(&old);
        return rc;
    }
    SwapStatic(&old);
    wasBuilt = hw.built;
    TearDownStatic(1, 1);
    SwapStatic(&old);
    /* The safe places belong to the old world. */
    hw.safeKnown = 0;
    Activate();
    if (!hw_ready()) {
        ReleaseStatic(&old);
        return HW_ERR_BUSY;
    }
    rc = BuildStatic();
    if (rc != HW_OK) {
        /* The old set goes back in; it fitted before, in the room it has just left. */
        DropStatic();
        SwapStatic(&old);
        if (wasBuilt && BuildStatic() != HW_OK) {
            DropStatic();
            hw.failed++;
        }
        MaybeDeactivate();
        return rc;
    }
    ReleaseStatic(&old);
    return HW_OK;
}

static HwBody *FindBody(unsigned int id) {
    int i;

    for (i = 0; id != 0 && i < HW_MAX_BODIES; i++) {
        if (hw.bodies[i].id == id) {
            return &hw.bodies[i];
        }
    }
    return NULL;
}

int hw_body_add(const HwMesh *m, const HwPose *p, unsigned int *id) {
    HwBody *b = NULL;
    int rc;
    int i;

    if (id == NULL || p == NULL || hw_check_mesh(m, HW_MAX_BODY_TRIS) != HW_OK || hw_check_pose(p) != HW_OK
        || m->conveyor != NULL) {
        return HW_ERR_ARG;
    }
    for (i = 0; i < HW_MAX_BODIES && b == NULL; i++) {
        if (hw.bodies[i].id == 0) {
            b = &hw.bodies[i];
        }
    }
    if (b == NULL) {
        return HW_ERR_MEMORY;
    }
    rc = CopyMesh(m, &b->mesh);
    if (rc != HW_OK) {
        return rc;
    }
    if (b->mesh.triCount == 0) {
        FreeCopy(&b->mesh);
        return HW_ERR_ARG;
    }
    b->inner = (unsigned char *)calloc(b->mesh.triCount, 1);
    if (b->inner == NULL) {
        FreeCopy(&b->mesh);
        return HW_ERR_MEMORY;
    }
    b->cur = *p;
    b->target = *p;
    if (BodyEdgesFit(b, p) != HW_OK) {
        FreeBody(b);
        return HW_ERR_ARG;
    }
    b->id = ++hw.nextId;
    *id = b->id;
    Activate();
    if (!hw_ready()) {
        return HW_ERR_BUSY;
    }
    rc = BuildBody(b);
    if (rc != HW_OK) {
        FreeBody(b);
        *id = 0;
        MaybeDeactivate();
    }
    return rc;
}

int hw_body_move(unsigned int id, const HwPose *p) {
    HwBody *b = FindBody(id);

    if (b == NULL || p == NULL || hw_check_pose(p) != HW_OK) {
        return HW_ERR_ARG;
    }
    if (b->plat == NULL) {
        b->cur = *p;
    }
    b->target = *p;
    b->moved = 1;
    return HW_OK;
}

int hw_body_conveyor(unsigned int id, const float vel[3]) {
    HwBody *b = FindBody(id);
    int i;

    if (b == NULL) {
        return HW_ERR_ARG;
    }
    for (i = 0; vel != NULL && i < 3; i++) {
        if (!(vel[i] >= -HW_CONVEYOR_LIMIT && vel[i] <= HW_CONVEYOR_LIMIT)) {
            return HW_ERR_ARG;
        }
    }
    b->hasConveyor = vel != NULL && (vel[0] != 0.0f || vel[1] != 0.0f || vel[2] != 0.0f);
    for (i = 0; i < 3; i++) {
        b->conveyor[i] = b->hasConveyor ? vel[i] : 0.0f;
    }
    if (b->plat == NULL) {
        return HW_ERR_BUSY;
    }
    SetBodyConveyor(b, b->plat);
    return HW_OK;
}

int hw_body_remove(unsigned int id) {
    HwBody *b = FindBody(id);

    if (b == NULL) {
        return HW_ERR_ARG;
    }
    CheckHeap();
    if (b->plat != NULL) {
        KillBody(b);
    }
    FreeBody(b);
    MaybeDeactivate();
    return HW_OK;
}

int hw_clear(void) {
    int i;

    for (i = 0; i < HW_MAX_BODIES; i++) {
        if (hw.bodies[i].id != 0) {
            hw_body_remove(hw.bodies[i].id);
        }
    }
    hw_set_static(NULL, NULL);
    hw_set_water(NULL, 0);
    if (hw.levelTaken) {
        GiveLevelBack();
    }
    hw.placePending = 0;
    hw.respawnSet = 0;
    hw.respawnPending = 0;
    hw.lost = 0;
    hw.voidState[0] = hw.voidState[1] = 0;
    hw.safeKnown = 0;
    MaybeDeactivate();
    return HW_OK;
}

/* Frees the host's memory and takes the hooks out, without touching the game's memory (glover_destroy). */
void hw_forget(void) {
    int i;

    FreeChunkPlan();
    FreeCopy(&hw.stat);
    for (i = 0; i < HW_MAX_BODIES; i++) {
        FreeBody(&hw.bodies[i]);
    }
    free(hw.takenActors);
    free(hw.takenPlats);
    free(hw.takenWaters);
    free(hw.waters);
    port_pre_update_hook = NULL;
    port_platforms_hook = NULL;
    port_free_level_hook = NULL;
    port_post_update_hook = NULL;
    port_no_shatter_hook = NULL;
    port_point_sound_off_hook = NULL;
    port_view_hook = NULL;
    port_camera_hook = NULL;
    memset(&hw, 0, sizeof(hw));
}

void hw_stats(HwStats *s) {
    unsigned int used = 0;
    int i;

    memset(s, 0, sizeof(*s));
    s->staticTris = hw.wantStatic ? hw.stat.triCount : 0;
    s->chunks = hw.wantStatic ? hw.chunkCount : 0;
    for (i = 0; hw.wantStatic && hw.stat.conveyor != NULL && i < (int)hw.stat.triCount; i++) {
        s->conveyorTris += hw.stat.conveyor[i] != 0;
    }
    for (i = 0; hw.wantStatic && i < (int)hw.chunkCount; i++) {
        s->conveyorPlatforms += hw.chunks[i].conveyor != 0;
    }
    for (i = 0; i < HW_MAX_BODIES; i++) {
        s->conveyorPlatforms += hw.bodies[i].id != 0 && hw.bodies[i].hasConveyor;
    }
    s->linkedChunks = hw.built ? hw.linked : 0;
    if (hw.built) {
        used += StaticBytes();
    }
    for (i = 0; i < HW_MAX_BODIES; i++) {
        if (hw.bodies[i].id != 0) {
            s->bodies++;
            if (hw.bodies[i].plat != NULL) {
                used += hw.bodies[i].bytes;
            } else {
                s->pending = 1;
            }
        }
    }
    if (hw.wantStatic && !hw.built) {
        s->pending = 1;
    }
    if (hw.waterBlock != NULL) {
        used += Align16(hw.waterCount * sizeof(Water));
    } else if (hw.waterCount > 0) {
        s->pending = 1;
    }
    s->waters = hw.waterCount;
    s->heapUsed = used;
    s->heapFree = HeapGap();
    s->rebuilds = hw.rebuilds;
    s->failed = hw.failed;
    s->buildUs = hw.buildUs;
    if (GameBeingPlayed()) {
        GaribGroup *group;
        Garib *garib;
        Collectable *c;

        for (i = 0; i < numberOfEnemies; i++) {
            s->enemiesOn += enemies[i].actor.active != 0 || enemies[i].actor.next != NULL;
        }
        for (group = garibGroupList.head.next; group != &garibGroupList.head; group = group->next) {
            for (garib = group->garibs.head.next; garib != &group->garibs.head; garib = garib->next) {
                s->collectablesOn += garib->active != 0;
            }
        }
        for (c = collectableList.head.next; c != &collectableList.head; c = c->next) {
            s->collectablesOn += c->active != 0 || c->actor.next != NULL;
        }
    }
}

int hw_set_water(const HwWater *w, unsigned int count) {
    HwWater *copy = NULL;
    unsigned int i;
    int j;

    if (w == NULL) {
        count = 0;
    }
    if (count > HW_MAX_WATERS) {
        return HW_ERR_ARG;
    }
    for (i = 0; i < count; i++) {
        for (j = 0; j < 3; j++) {
            if (!Finite(w[i].min[j]) || !Finite(w[i].size[j]) || !(w[i].size[j] > 0.0f)
                || !(w[i].current[j] >= -HW_CONVEYOR_LIMIT && w[i].current[j] <= HW_CONVEYOR_LIMIT)) {
                return HW_ERR_ARG;
            }
        }
        /* Within 0.01: a host that scales the corner, the size and the surface each on its own rounds each. */
        if (!Finite(w[i].surface) || !((double)w[i].surface >= (double)w[i].min[1] - 0.01)
            || !((double)w[i].surface <= (double)w[i].min[1] + w[i].size[1] + 0.01)) {
            return HW_ERR_ARG;
        }
    }
    CheckHeap();
    if (count > 0) {
        copy = (HwWater *)malloc(count * sizeof(HwWater));
        if (copy == NULL) {
            return HW_ERR_MEMORY;
        }
        memcpy(copy, w, count * sizeof(HwWater));
    }
    TearDownWaters(1, 1);
    free(hw.waters);
    hw.waters = copy;
    hw.waterCount = count;
    if (count == 0) {
        MaybeDeactivate();
        return HW_OK;
    }
    Activate();
    if (!hw_ready()) {
        return HW_ERR_BUSY;
    }
    if (BuildWaters() != HW_OK) {
        free(hw.waters);
        hw.waters = NULL;
        hw.waterCount = 0;
        MaybeDeactivate();
        return HW_ERR_MEMORY;
    }
    return HW_OK;
}

int hw_place(const HwPlace *p) {
    if (p == NULL || (p->what & ~0x1Fu)) {
        return HW_ERR_ARG;
    }
    for (int i = 0; i < 3; i++) {
        if (!Finite(p->hand[i]) || !Finite(p->ball[i]) || !Finite(p->handVel[i]) || !Finite(p->ballVel[i])) {
            return HW_ERR_ARG;
        }
    }
    if (!Finite(p->handYaw)) {
        return HW_ERR_ARG;
    }
    hw.place = *p;
    hw.placePending = 1;
    Activate();
    return HW_OK;
}

int hw_set_respawn(const float pos[3], float yaw) {
    int i;

    if (pos == NULL || !Finite(yaw)) {
        return HW_ERR_ARG;
    }
    for (i = 0; i < 3; i++) {
        if (!Finite(pos[i])) {
            return HW_ERR_ARG;
        }
        hw.respawnPos[i] = pos[i];
    }
    hw.respawnYaw = yaw;
    hw.respawnSet = 1;
    hw.respawnPending = 1;
    Activate();
    return HW_OK;
}

/* The level's own draw distance again, unless the level has written its own since the view wrote one. */
static void RestoreFarPlane(void) {
    if (hw.viewFar != 0 && farPlaneDist == hw.viewFar) {
        farPlaneDist = hw.levelFar;
    }
    hw.viewFar = 0;
}

int hw_set_view(const HwView *v) {
    if (v != NULL && hw_check_view(v) != HW_OK) {
        return HW_ERR_ARG;
    }
    if (v == NULL || v->flags == 0 || !(v->flags & HW_VIEW_DRAW) || (v->flags & HW_VIEW_MIRROR)) {
        RestoreFarPlane();
    }
    if (v == NULL || v->flags == 0) {
        hw.viewOn = 0;
        port_view_hook = NULL;
        port_camera_hook = NULL;
        MaybeDeactivate();
        return HW_OK;
    }
    hw.view = *v;
    hw.viewOn = 1;
    port_view_hook = (v->flags & (HW_VIEW_DRAW | HW_VIEW_MIRROR)) ? ViewHook : NULL;
    port_camera_hook = (v->flags & HW_VIEW_STEER) ? CameraHook : NULL;
    if (v->flags & HW_VIEW_STEER) {
        Activate();
    }
    return HW_OK;
}

int hw_view_draws(void) {
    return hw.viewOn && (hw.view.flags & HW_VIEW_DRAW);
}

void hw_view_planes(float *nearZ, float *farZ, float projZ[2]) {
    float fovy;
    float aspect;
    float mf[4][4];
    Mtx m;
    u16 perspNorm;

    if (hw_view_draws() && !(hw.view.flags & HW_VIEW_MIRROR) && GameBeingPlayed()) {
        *nearZ = hw.view.nearZ;
        *farZ = hw.view.farZ;
        fovy = hw.view.fovy;
        aspect = hw.view.aspect;
    } else {
        *nearZ = nearPlaneDist;
        *farZ = farPlaneDist;
        fovy = fieldOfView;
        aspect = 4.0f / 3.0f;
    }
    /* The projection as ClearViewing and ViewHook make it: guMtxF2L cuts its terms to s15.16. */
    guPerspective(&m, &perspNorm, fovy, aspect, *nearZ, *farZ, 1.0f);
    guMtxL2F(mf, &m);
    projZ[0] = mf[2][2];
    projZ[1] = mf[3][2];
}

static void ViewHook(Mtx *projection, Mtx *view, u16 *perspNorm) {
    float eye[3];
    float at[3];
    float up[3];
    float fovy;
    float aspect;
    float nearZ;
    float farZ;

    if (!GameBeingPlayed()) {
        return;
    }
    if (hw.view.flags & HW_VIEW_MIRROR) {
        /* The game's own values, as SetupViewing has just used them. */
        eye[0] = actualCamPos[draw_buffer].x;
        eye[1] = actualCamPos[draw_buffer].y;
        eye[2] = actualCamPos[draw_buffer].z;
        at[0] = actualCamTargetPos[draw_buffer].x;
        at[1] = actualCamTargetPos[draw_buffer].y;
        at[2] = actualCamTargetPos[draw_buffer].z;
        up[0] = camVect.x;
        up[1] = camVect.y;
        up[2] = camVect.z;
        fovy = fieldOfView;
        aspect = 4.0f / 3.0f;
        nearZ = nearPlaneDist;
        farZ = farPlaneDist;
    } else {
        memcpy(eye, hw.view.eye, sizeof(eye));
        memcpy(at, hw.view.at, sizeof(at));
        memcpy(up, hw.view.up, sizeof(up));
        fovy = hw.view.fovy;
        aspect = hw.view.aspect;
        nearZ = hw.view.nearZ;
        farZ = hw.view.farZ;
        /* The game leaves out the actors and sprites beyond its draw distance (IsActorVisible, PrintSprite). With
           the host's view that distance is the view's: a host at a large scale puts its far plane well beyond the
           level's. A level load writes the level's value again; it is kept for the view's end. */
        if (hw.viewFar == 0 || farPlaneDist != hw.viewFar) {
            hw.levelFar = farPlaneDist;
        }
        hw.viewFar = (s16)MIN(MAX(farZ, 1.0f), 32767.0f);
        farPlaneDist = hw.viewFar;
    }
    guPerspective(projection, perspNorm, fovy, aspect, nearZ, farZ, 1.0f);
    if (view == NULL) {
        return;
    }
    if (ShadingMode == 2) {
        guLookAtHilite(view, &dynamicp->lookAt, &dynamicp->hilite, eye[0], eye[1], eye[2], at[0], at[1], at[2],
                       up[0], up[1], up[2], 0, 0, 127.0f, 0, 0, 127.0f, 32, 32);
    } else {
        guLookAt(view, eye[0], eye[1], eye[2], at[0], at[1], at[2], up[0], up[1], up[2]);
    }
}

static s32 CameraHook(void) {
    return GameBeingPlayed() && hw.viewOn && (hw.view.flags & HW_VIEW_STEER);
}

void hw_bodies(HwBodies *b) {
    memset(b, 0, sizeof(*b));
    b->hand[0] = hand.pos.x;
    b->hand[1] = hand.pos.y;
    b->hand[2] = hand.pos.z;
    b->ball[0] = ball.pos.x;
    b->ball[1] = ball.pos.y;
    b->ball[2] = ball.pos.z;
    b->camera[0] = camera.pos.x;
    b->camera[1] = camera.pos.y;
    b->camera[2] = camera.pos.z;
    b->handVel[0] = hand.vel.x;
    b->handVel[1] = hand.vel.y;
    b->handVel[2] = hand.vel.z;
    b->ballVel[0] = ball.vel.x;
    b->ballVel[1] = ball.vel.y;
    b->ballVel[2] = ball.vel.z;
    b->handYaw = hand.rot.y;
    b->handAction = hand.state != NULL ? hand.state->mode : -1;
    b->ballHeld = ball.link == &hand;
    b->handOnGround = hand.obj != NULL && hand.obj->onGround;
    b->ballOnGround = ball.obj != NULL && ball.obj->onGround;
    b->handTerrain = hand.obj != NULL && hand.obj->planes[0] != NULL ? ((CollFace *)hand.obj->planes[0])->terrain : -1;
    b->ballTerrain = ball.obj != NULL && ball.obj->planes[0] != NULL ? ((CollFace *)ball.obj->planes[0])->terrain : -1;
    b->ready = hw_ready();
    b->restarts = portRestarts;
    b->handVoid = hw.voidState[0];
    b->ballVoid = hw.voidState[1];
    b->handVoids = hw.voids[0];
    b->ballVoids = hw.voids[1];
    memcpy(b->handSafe, hw.safe[0], sizeof(b->handSafe));
    memcpy(b->ballSafe, hw.safe[1], sizeof(b->ballSafe));
    b->safeKnown = hw.safeKnown;
    b->handWater = hand.state != NULL ? hand.state->waterState : 0;
    b->ballWater = ball.state != NULL ? ball.state->waterState : 0;
}

PORT_C_END
