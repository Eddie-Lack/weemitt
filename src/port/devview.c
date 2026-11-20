/* What the Developer menu draws over the 3D view: the collision data near the glove and the names of the actors. It
   reads the game's state and changes nothing in it. The renderer and the menu turn the world positions into
   screen positions. */
PORT_C_BEGIN
#include "port.h"
#include "common.h"
#include "macros.h"
#include "maths.h"
#include "actor.h"
#include "platform.h"
#include "hand.h"
#include "ball.h"
#include "game.h"
#include "frame.h"

void QuaternionToMatrix(Quat *q, f32 mf[4][4]);

extern Actor camera;

/* Colours as the renderer's line colour: red in the lowest byte, alpha in the highest. */
#define RGBA(r, g, b, a) ((unsigned int)(r) | ((unsigned int)(g) << 8) | ((unsigned int)(b) << 16) | ((unsigned int)(a) << 24))
#define COLOR_FLOOR RGBA(60, 230, 80, 255)
#define COLOR_WALL RGBA(255, 150, 30, 255)
#define COLOR_CEILING RGBA(220, 80, 240, 255)
#define COLOR_PLATFORM RGBA(70, 140, 255, 255)
#define COLOR_WATER RGBA(40, 230, 240, 255)
#define COLOR_HURT RGBA(255, 50, 50, 255)
#define COLOR_HAND RGBA(255, 235, 60, 255)
#define COLOR_BALL RGBA(255, 255, 255, 255)
#define COLOR_ENEMY RGBA(255, 110, 110, 200)

/* The terrain numbers that the game's code tests (collision.c, hand.c). */
#define TERRAIN_WATER 5
#define TERRAIN_DEATH 6
#define TERRAIN_HURT 9
#define TERRAIN_WATER_SURFACE 12

/* The names of the actor types, from the game's ActorType enumeration. C++ has no designated array initializers. */
static const struct {
    int type;
    const char *name;
} actorNames[] = {
    {HAND, "Glover"}, {BALL, "Ball"}, {SNEEZY, "Sneezy"}, {BOVVA, "Bovva"}, {CANNONBALL, "Cannonball"},
    {SAMTEX, "Samtex"}, {MALLET, "Mallet"}, {GENERALWU, "General Wu"}, {LIONFISH, "Lionfish"}, {CHESTER, "Chester"},
    {KEG, "Keg"}, {REGGIE, "Reggie"}, {SWISH, "Swish"}, {KNIGHT, "Knight"}, {ROBES, "Robes"}, {FUMBLE, "Fumble"},
    {MIKE, "Mike"}, {RAPTOR, "Raptor"}, {CRUMPET, "Crumpet"}, {TRACEY, "Tracey"}, {YOOFOW, "Yoofow"}, {OPEC, "Opec"},
    {CYMON, "Cymon"}, {SUCKER, "Sucker"}, {BUGLE, "Bugle"}, {DENIS, "Denis"}, {CHUCK, "Chuck"},
    {HUBCHICKEN, "Hub chicken"}, {FRANKIE, "Frankie"}, {KLOSET, "Kloset"}, {FLANNEL, "Flannel"},
    {SELWYN_WHALE, "Selwyn whale"}, {SELWYN_CRAB, "Selwyn crab"}, {SELWYN_FISH, "Selwyn fish"}, {ROBOT, "Robot"},
    {EVIL_ROBOT, "Evil robot"}, {SPANK, "Spank"}, {BABY_SPANK, "Baby Spank"}, {EVIL_GLOVE, "Evil glove"},
    {DIBBER, "Dibber"}, {FLY, "Fly"}, {SNAIL, "Snail"}, {BIGFISH, "Big fish"}, {LITTLEFISH, "Little fish"},
    {LADYBIRD, "Ladybird"}, {WEEVIL, "Weevil"}, {CATERPILLAR, "Caterpillar"}, {BUTTERFLY, "Butterfly"},
    {SPIDER, "Spider"}, {BAT, "Bat"}, {FROG, "Frog"}, {DRAGFLY, "Dragonfly"}, {NME_FROG, "Frog (spell)"},
};

static const char *actor_name(int type) {
    unsigned int i;

    for (i = 0; i < sizeof(actorNames) / sizeof(actorNames[0]); i++) {
        if (actorNames[i].type == type) {
            return actorNames[i].name;
        }
    }
    return NULL;
}

static int is_enemy_type(int type) {
    return (type >= SNEEZY && type <= DRAGFLY && type != 6) || type == NME_FROG;
}

typedef struct LineOut {
    PortDebugLine *lines;
    int count;
    int max;
} LineOut;

static void add_line(LineOut *o, const Vec3 *a, const Vec3 *b, unsigned int color) {
    PortDebugLine *l;

    if (o->count >= o->max) {
        return;
    }
    l = &o->lines[o->count++];
    l->a[0] = a->x;
    l->a[1] = a->y;
    l->a[2] = a->z;
    l->b[0] = b->x;
    l->b[1] = b->y;
    l->b[2] = b->z;
    l->color = color;
}

static void add_triangle(LineOut *o, const Vec3 *v0, const Vec3 *v1, const Vec3 *v2, unsigned int color) {
    add_line(o, v0, v1, color);
    add_line(o, v1, v2, color);
    add_line(o, v2, v0, color);
}

/* The colour of a landscape face: by terrain first, then by how steep it is (the game treats a normal with y above
   0.3 as ground). */
static unsigned int face_kind(const CollFace *face, const Vec3 *normal) {
    switch (face->terrain) {
        case TERRAIN_WATER:
        case TERRAIN_WATER_SURFACE:
            return PORT_COLL_WATER;
        case TERRAIN_DEATH:
        case TERRAIN_HURT:
            return PORT_COLL_HURT;
    }
    if (normal->y > 0.3f) {
        return PORT_COLL_FLOOR;
    }
    if (normal->y < -0.3f) {
        return PORT_COLL_CEILING;
    }
    return PORT_COLL_WALL;
}

static unsigned int kind_color(unsigned int kind) {
    switch (kind) {
        case PORT_COLL_WATER:
            return COLOR_WATER;
        case PORT_COLL_HURT:
            return COLOR_HURT;
        case PORT_COLL_FLOOR:
            return COLOR_FLOOR;
        case PORT_COLL_CEILING:
            return COLOR_CEILING;
    }
    return COLOR_WALL;
}

static int near_point(const Vec3 *centre, const Vec3 *p, f32 rangeSq) {
    f32 dx = p->x - centre->x;
    f32 dy = p->y - centre->y;
    f32 dz = p->z - centre->z;

    return dx * dx + dy * dy + dz * dz <= rangeSq;
}

/* The world position of a collision sphere, as CalcCollisionSpheres finds it. */
static void sphere_centre(Actor *actor, s32 j, Vec3 *out) {
    CollSphere *s = &actor->obj->spheres[j];

    if (s->flags & 0x20) {
        *out = actor->pos;
    } else if (s->flags & 0x10) {
        *out = s->pos;
    } else {
        Vec3 r;

        RotateVectorByQuaternion(&r, &s->pos, &actor->rotQuat);
        out->x = r.x + actor->pos.x;
        out->y = r.y + actor->pos.y;
        out->z = r.z + actor->pos.z;
    }
}

/* Three circles around the sphere, one in each axis plane. */
static void add_sphere(LineOut *o, const Vec3 *c, f32 radius, unsigned int color) {
    enum { STEPS = 16 };
    static f32 sinTable[STEPS + 1];
    static f32 cosTable[STEPS + 1];
    s32 i;
    s32 axis;

    if (sinTable[1] == 0.0f) {
        for (i = 0; i <= STEPS; i++) {
            sinTable[i] = sin(i * 6.283185307 / STEPS);
            cosTable[i] = cos(i * 6.283185307 / STEPS);
        }
    }
    if (radius <= 0.0f || radius > 10000.0f) {
        return;
    }
    for (axis = 0; axis < 3; axis++) {
        Vec3 prev;

        for (i = 0; i <= STEPS; i++) {
            Vec3 p = *c;
            f32 a = cosTable[i] * radius;
            f32 b = sinTable[i] * radius;

            if (axis == 0) {
                p.x += a;
                p.y += b;
            } else if (axis == 1) {
                p.y += a;
                p.z += b;
            } else {
                p.z += a;
                p.x += b;
            }
            if (i > 0) {
                add_line(o, &prev, &p, color);
            }
            prev = p;
        }
    }
}

static void add_actor_spheres(LineOut *o, Actor *actor, unsigned int color) {
    s32 j;

    if (actor->obj == NULL || actor->obj->numSpheres > 8) {
        return;
    }
    for (j = 0; j < actor->obj->numSpheres; j++) {
        Vec3 c;

        sphere_centre(actor, j, &c);
        add_sphere(o, &c, actor->obj->spheres[j].radius, color);
    }
}

/* The faces of one platform, worked out from its mesh and its place now. The stored faces (actor.faces) are only
   brought up to date when something is near, and reading them must not change the game. */
static void add_platform(LineOut *o, Platform *platform, const Vec3 *centre, f32 rangeSq) {
    Mesh *mesh = platform->actor.inst->obj->mesh;
    f32 matrix[4][4];
    s32 i;

    QuaternionToMatrix(&platform->actor.rotQuat, matrix);
    for (i = 0; i < mesh->numFaces; i++) {
        Vec3 v[3];
        s32 j;
        s32 near = 0;

        for (j = 0; j < 3; j++) {
            Vec3 *vert = &mesh->verts[mesh->faces[i][j]];
            f32 x = platform->actor.scale.x * vert->x;
            f32 y = platform->actor.scale.y * vert->y;
            f32 z = platform->actor.scale.z * vert->z;

            v[j].x = matrix[0][0] * x + matrix[1][0] * y + matrix[2][0] * z + platform->actor.pos.x;
            v[j].y = matrix[0][1] * x + matrix[1][1] * y + matrix[2][1] * z + platform->actor.pos.y;
            v[j].z = matrix[0][2] * x + matrix[1][2] * y + matrix[2][2] * z + platform->actor.pos.z;
            near |= near_point(centre, &v[j], rangeSq);
        }
        if (near) {
            add_triangle(o, &v[0], &v[1], &v[2], COLOR_PLATFORM);
        }
    }
}

int port_debug_collision(PortDebugLine *out, int max, float range, unsigned int types) {
    LineOut o;
    Actor *actor;
    Platform *platform;
    Vec3 centre;
    f32 rangeSq = range * range;

    o.lines = out;
    o.count = 0;
    o.max = max;
    if (!GameBeingPlayed() || hand.obj == NULL || hand.inst == NULL) {
        return 0;
    }
    centre = hand.pos;
    /* The landscape: every actor of the collision list that has faces. */
    for (actor = collisionList.head.collNext; actor != &collisionList.head && o.count < o.max; actor = actor->collNext) {
        s32 i;

        if (actor->type != LANDSCAPE_ACTOR || actor->faces == NULL || actor->inst == NULL) {
            continue;
        }
        if (DistanceBetweenPoints(&centre, &actor->pos) > range + actor->radius) {
            continue;
        }
        for (i = 0; i < actor->inst->obj->mesh->numFaces; i++) {
            CollFace *face = &actor->faces[i];
            unsigned int kind = face_kind(face, (Vec3 *)&face->plane);

            if (!(types & kind)) {
                continue;
            }
            if (near_point(&centre, &face->edgeStart[0], rangeSq) || near_point(&centre, &face->edgeStart[1], rangeSq)
                || near_point(&centre, &face->edgeStart[2], rangeSq)) {
                add_triangle(&o, &face->edgeStart[0], &face->edgeStart[1], &face->edgeStart[2],
                             kind_color(kind));
            }
        }
    }
    for (platform = platformList.head.next; (types & PORT_COLL_PLATFORM) && platform != &platformList.head && o.count < o.max;
         platform = platform->next) {
        if ((platform->flags & 0x84) || platform->actor.inst == NULL || platform->actor.faces == NULL) {
            continue;
        }
        if (DistanceBetweenPoints(&centre, &platform->actor.pos) > range + platform->actor.radius) {
            continue;
        }
        add_platform(&o, platform, &centre, rangeSq);
    }
    /* The spheres: the glove's and the ball's, and those of the actors near them. */
    if (types & PORT_COLL_HAND) {
        add_actor_spheres(&o, &hand, COLOR_HAND);
    }
    if (types & PORT_COLL_BALL) {
        add_actor_spheres(&o, &ball, COLOR_BALL);
    }
    for (actor = actorList.head.next; (types & PORT_COLL_ENEMY) && actor != &actorList.head; actor = actor->next) {
        if (is_enemy_type(actor->type) && DistanceBetweenPoints(&centre, &actor->pos) <= range) {
            add_actor_spheres(&o, actor, COLOR_ENEMY);
        }
    }
    return o.count;
}

static void copy_name(char *dst, const char *src, int size, int maxLen) {
    int i;

    for (i = 0; i < size - 1 && i < maxLen && src[i] >= 32 && src[i] < 127; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

int port_debug_labels(PortDebugLabel *out, int max, float range) {
    Actor *actor;
    int n = 0;

    if (!GameBeingPlayed() || hand.obj == NULL || hand.inst == NULL) {
        return 0;
    }
    for (actor = actorList.head.next; actor != &actorList.head && n < max; actor = actor->next) {
        PortDebugLabel *l = &out[n];
        int type = actor->type;
        f32 dist = DistanceBetweenPoints(&hand.pos, &actor->pos);
        const char *model = NULL;

        if (dist > range || type == CAMERA || type == DEBRIS || type == BULLET || type == LANDSCAPE_ACTOR
            || type == WATER_ACTOR) {
            continue;
        }
        if (actor->inst != NULL && actor->inst->obj != NULL) {
            model = actor->inst->obj->name;
        }
        l->pos[0] = actor->pos.x;
        l->pos[1] = actor->pos.y;
        l->pos[2] = actor->pos.z;
        l->distance = dist;
        if (type == HAND || type == BALL) {
            l->kind = type == HAND ? PORT_DEBUG_HAND : PORT_DEBUG_BALL;
            copy_name(l->name, actor_name(type), sizeof(l->name), 23);
        } else if (is_enemy_type(type)) {
            l->kind = PORT_DEBUG_ENEMY;
            copy_name(l->name, actor_name(type) != NULL ? actor_name(type) : "Enemy", sizeof(l->name), 23);
        } else {
            l->kind = type == PLAT_ACTOR ? PORT_DEBUG_PLATFORM : type == COLLECTABLE_ACTOR ? PORT_DEBUG_COLLECTABLE
                                                                                           : PORT_DEBUG_OTHER;
            if (model != NULL && model[0] != 0) {
                copy_name(l->name, model, sizeof(l->name), 8);
            } else {
                l->name[0] = 0;
            }
        }
        if (l->name[0] == 0) {
            continue;
        }
        n++;
    }
    return n;
}

PORT_C_END
