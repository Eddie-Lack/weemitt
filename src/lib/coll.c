/* Collision with the host's world, the host's bodies, and places (glover.h, docs/host-world.md). The checks of the
   arguments are here; src/port/hostworld.c does the work, inside enter. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "lib.h"
#include "hostworld.h"

typedef struct CollCall {
    const GloverMesh *mesh;
    const GloverCollOptions *opt;
    const GloverPose *pose;
    uint32_t id;
    uint32_t *outId;
    const GloverPlace *place;
    const float *pos;
    float yaw;
    int32_t rc;
} CollCall;

/* A host of version 2 or earlier gives a GloverMesh that ends before conveyor: no conveyors. */
static int mesh_of(const GloverMesh *m, HwMesh *h) {
    int conveyors = m->size >= sizeof(GloverMesh);

    if (m->size < offsetof(GloverMesh, conveyor)) {
        return 0;
    }
    h->verts = m->verts;
    h->vertCount = m->vertCount;
    h->indices = m->indices;
    h->terrain = m->terrain;
    h->triFlags = m->triFlags;
    h->triCount = m->triCount;
    h->flags = m->flags;
    h->conveyor = conveyors ? m->conveyor : NULL;
    h->conveyors = conveyors ? m->conveyors : NULL;
    h->conveyorCount = conveyors ? m->conveyorCount : 0;
    return 1;
}

static void pose_of(const GloverPose *p, HwPose *h) {
    memcpy(h->pos, p->pos, sizeof(h->pos));
    memcpy(h->rot, p->rot, sizeof(h->rot));
    memcpy(h->scale, p->scale, sizeof(h->scale));
}

static void call_set_static(void *arg) {
    CollCall *c = (CollCall *)arg;
    HwMesh m;
    HwOptions o;

    if (c->mesh == NULL) {
        c->rc = hw_set_static(NULL, NULL);
        return;
    }
    if (!mesh_of(c->mesh, &m) || c->opt == NULL || c->opt->size < offsetof(GloverCollOptions, levelOff)) {
        c->rc = GLOVER_ERR_ARG;
        return;
    }
    o.cellSize = c->opt->cellSize;
    o.maxChunkTris = c->opt->maxChunkTris;
    o.linkRadius = c->opt->linkRadius;
    o.keepLevel = c->opt->keepLevel;
    /* A host built before levelOff was added gives the shorter size: nothing is switched off. */
    o.levelOff = c->opt->size >= sizeof(GloverCollOptions) ? c->opt->levelOff : 0;
    c->rc = hw_set_static(&m, &o);
}

GLOVER_API int32_t glover_coll_set_static(Glover *g, const GloverMesh *m, const GloverCollOptions *o) {
    CollCall c;
    int32_t rc;

    memset(&c, 0, sizeof(c));
    c.mesh = m;
    c.opt = o;
    rc = enter(g, call_set_static, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

static void call_body_add(void *arg) {
    CollCall *c = (CollCall *)arg;
    HwMesh m;
    HwPose p;

    if (c->mesh == NULL || !mesh_of(c->mesh, &m) || c->pose == NULL) {
        c->rc = GLOVER_ERR_ARG;
        return;
    }
    pose_of(c->pose, &p);
    c->rc = hw_body_add(&m, &p, c->outId);
}

GLOVER_API int32_t glover_coll_body_add(Glover *g, const GloverMesh *m, const GloverPose *p, uint32_t *id) {
    CollCall c;
    int32_t rc;

    if (id == NULL) {
        return GLOVER_ERR_ARG;
    }
    *id = 0;
    memset(&c, 0, sizeof(c));
    c.mesh = m;
    c.pose = p;
    c.outId = id;
    rc = enter(g, call_body_add, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

static void call_body_move(void *arg) {
    CollCall *c = (CollCall *)arg;
    HwPose p;

    if (c->pose == NULL) {
        c->rc = GLOVER_ERR_ARG;
        return;
    }
    pose_of(c->pose, &p);
    c->rc = hw_body_move(c->id, &p);
}

GLOVER_API int32_t glover_coll_body_move(Glover *g, uint32_t id, const GloverPose *p) {
    CollCall c;
    int32_t rc;

    memset(&c, 0, sizeof(c));
    c.id = id;
    c.pose = p;
    rc = enter(g, call_body_move, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

static void call_body_remove(void *arg) {
    CollCall *c = (CollCall *)arg;

    c->rc = hw_body_remove(c->id);
}

static void call_body_conveyor(void *arg) {
    CollCall *c = (CollCall *)arg;

    c->rc = hw_body_conveyor(c->id, c->pos);
}

GLOVER_API int32_t glover_coll_body_conveyor(Glover *g, uint32_t id, const float vel[3]) {
    CollCall c;
    int32_t rc;

    memset(&c, 0, sizeof(c));
    c.id = id;
    c.pos = vel;
    rc = enter(g, call_body_conveyor, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

typedef struct WaterCall {
    const GloverWater *w;
    uint32_t count, elemSize;
    int32_t rc;
} WaterCall;

static void call_set_water(void *arg) {
    WaterCall *c = (WaterCall *)arg;
    HwWater *h = NULL;
    uint32_t i;

    if (c->count > 0) {
        h = (HwWater *)malloc(c->count * sizeof(HwWater));
        if (h == NULL) {
            c->rc = GLOVER_ERR_MEMORY;
            return;
        }
        for (i = 0; i < c->count; i++) {
            memcpy(&h[i], (const char *)c->w + (size_t)i * c->elemSize, sizeof(HwWater));
        }
    }
    c->rc = hw_set_water(h, c->count);
    free(h);
}

GLOVER_API int32_t glover_coll_set_water(Glover *g, const GloverWater *w, uint32_t count, uint32_t elemSize) {
    WaterCall c;
    int32_t rc;

    memset(&c, 0, sizeof(c));
    c.w = w;
    c.count = w != NULL ? count : 0;
    c.elemSize = elemSize;
    if (c.count > GLOVER_MAX_WATERS || (c.count > 0 && elemSize < sizeof(GloverWater))) {
        return GLOVER_ERR_ARG;
    }
    rc = enter(g, call_set_water, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

GLOVER_API int32_t glover_coll_body_remove(Glover *g, uint32_t id) {
    CollCall c;
    int32_t rc;

    memset(&c, 0, sizeof(c));
    c.id = id;
    rc = enter(g, call_body_remove, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

static void call_clear(void *arg) {
    ((CollCall *)arg)->rc = hw_clear();
}

GLOVER_API int32_t glover_coll_clear(Glover *g) {
    CollCall c;
    int32_t rc;

    memset(&c, 0, sizeof(c));
    rc = enter(g, call_clear, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

/* The size of version 2's GloverCollStats. */
#define STATS_V2_SIZE 40

static void call_stats(void *arg) {
    GloverCollStats *out = (GloverCollStats *)arg;
    GloverCollStats full;
    GloverCollStats *s = &full;
    HwStats h;

    hw_stats(&h);
    s->staticTris = h.staticTris;
    s->chunks = h.chunks;
    s->linkedChunks = h.linkedChunks;
    s->bodies = h.bodies;
    s->heapUsed = h.heapUsed;
    s->heapFree = h.heapFree;
    s->rebuilds = h.rebuilds;
    s->pending = h.pending;
    s->failed = h.failed;
    s->conveyorTris = h.conveyorTris;
    s->conveyorPlatforms = h.conveyorPlatforms;
    s->waters = h.waters;
    /* A host of an earlier version gets the part that its structure holds. */
    memcpy((char *)out + 4, (char *)s + 4, (out->size < sizeof(full) ? out->size : sizeof(full)) - 4);
}

GLOVER_API int32_t glover_coll_stats(Glover *g, GloverCollStats *s) {
    if (s == NULL || s->size < STATS_V2_SIZE) {
        return GLOVER_ERR_ARG;
    }
    return enter(g, call_stats, s);
}

static void call_place(void *arg) {
    CollCall *c = (CollCall *)arg;
    HwPlace p;

    p.what = c->place->what;
    memcpy(p.hand, c->place->hand, sizeof(p.hand));
    memcpy(p.ball, c->place->ball, sizeof(p.ball));
    memcpy(p.handVel, c->place->handVel, sizeof(p.handVel));
    memcpy(p.ballVel, c->place->ballVel, sizeof(p.ballVel));
    p.handYaw = c->place->handYaw;
    c->rc = hw_place(&p);
}

GLOVER_API int32_t glover_place(Glover *g, const GloverPlace *p) {
    CollCall c;
    int32_t rc;

    if (p == NULL || p->size < sizeof(GloverPlace)) {
        return GLOVER_ERR_ARG;
    }
    memset(&c, 0, sizeof(c));
    c.place = p;
    rc = enter(g, call_place, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}

static void call_respawn(void *arg) {
    CollCall *c = (CollCall *)arg;

    c->rc = hw_set_respawn(c->pos, c->yaw);
}

GLOVER_API int32_t glover_set_respawn(Glover *g, const float pos[3], float yaw) {
    CollCall c;
    int32_t rc;

    if (pos == NULL) {
        return GLOVER_ERR_ARG;
    }
    memset(&c, 0, sizeof(c));
    c.pos = pos;
    c.yaw = yaw;
    rc = enter(g, call_respawn, &c);
    return rc != GLOVER_OK ? rc : c.rc;
}
