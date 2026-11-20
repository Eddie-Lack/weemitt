/* The host scene format (docs/host-world.md). One item per line; '#' starts a comment. Coordinates are game units
   times the latest "scale" (1 by default); quaternions, scale factors, angles and the field of view are not
   scaled. A frame F is a time in original frames: a request at F is made before the first update that starts at F
   or later.

     scale S
     options [cell C] [maxtris N] [link R] [keep 0|1] [off MASK]   MASK: 1 the enemies, 2 the collectables,
                                                4 the ambient sounds at points
     static F                                   the frame of the static triangles and the bodies (default 0)
     conveyor K VX VY VZ                        conveyor K (1 to 4096): what stands on it moves V per original frame
     tri X1 Y1 Z1 X2 Y2 Z2 X3 Y3 Z3 [TERRAIN [FLAGS [CONVEYOR]]]   FLAGS: 1 the camera passes, 2 a void;
                                                CONVEYOR: K, static triangles only
     body ID X Y Z QX QY QZ QW SX SY SZ         a body; its tri lines follow, in its own space, up to "end"
     end
     move ID sine PERIOD DX DY DZ               the body at its start pose plus D * sin(2 pi t / PERIOD)
     move ID spin PERIOD                        the body turned about y, once per PERIOD frames
     pose F ID X Y Z QX QY QZ QW SX SY SZ
     bodyconveyor F ID VX VY VZ                 a conveyor on the body, in its own space; 0 0 0: none
     water X Y Z SX SY SZ SURFACE [CX CY CZ]    a water volume from X Y Z, of size SX SY SZ, with its current per
                                                original frame; the waters go in with the static triangles
     water F off                                removes the waters
     remove F ID
     place F [hand X Y Z] [ball X Y Z] [handvel X Y Z] [ballvel X Y Z] [yaw A] [camera]
     respawn F X Y Z YAW
     view F off | view F FLAGS EX EY EZ AX AY AZ UX UY UZ FOVY ASPECT NEAR FAR
     clear F
     goto F LEVEL

   Plain C: the test host (tools/libhost) compiles it too. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hostscene.h"

#define PI 3.14159265358979323846

typedef struct Tris {
    float *verts;
    unsigned int *indices;
    unsigned char *terrain;
    unsigned char *flags;
    unsigned short *conveyor;
    int anyConveyor;
    unsigned int count, cap;
} Tris;

typedef struct Body {
    int fileId;
    unsigned int id;                 /* the program's handle, 0 before it is added */
    HsPose start;
    Tris tris;
    int motion;                      /* 0 none, 1 sine, 2 spin */
    double period;
    float amp[3];
} Body;

enum {
    EV_POSE, EV_REMOVE, EV_PLACE, EV_RESPAWN, EV_VIEW, EV_VIEW_OFF, EV_CLEAR, EV_GOTO, EV_BODY_CONVEYOR, EV_WATER_OFF
};

#define MAX_CONVEYORS 4096

typedef struct Event {
    double frame;
    int type;
    int fileId;
    int level;
    HsPose pose;
    HsPlace place;
    HsView view;
    float pos[3], yaw;
} Event;

struct HostScene {
    Tris stat;
    float conveyors[3 * MAX_CONVEYORS];
    unsigned int conveyorCount;
    HsWater *waters;
    unsigned int waterCount;
    HsOptions opt;
    double staticFrame;
    int staticDone;
    Body *bodies;
    int bodyCount;
    Event *events;
    int eventCount;
    int nextEvent;
};

static int AddTri(Tris *t, const float v[9], int terrain, int flags, int conveyor) {
    if (t->count == t->cap) {
        unsigned int cap = t->cap ? 2 * t->cap : 64;

        t->verts = (float *)realloc(t->verts, cap * 9 * sizeof(float));
        t->indices = (unsigned int *)realloc(t->indices, cap * 3 * sizeof(unsigned int));
        t->terrain = (unsigned char *)realloc(t->terrain, cap);
        t->flags = (unsigned char *)realloc(t->flags, cap);
        t->conveyor = (unsigned short *)realloc(t->conveyor, cap * sizeof(unsigned short));
        if (t->verts == NULL || t->indices == NULL || t->terrain == NULL || t->flags == NULL
            || t->conveyor == NULL) {
            return -1;
        }
        t->cap = cap;
    }
    t->conveyor[t->count] = (unsigned short)conveyor;
    t->anyConveyor |= conveyor != 0;
    memcpy(&t->verts[9 * t->count], v, 9 * sizeof(float));
    t->indices[3 * t->count] = 3 * t->count;
    t->indices[3 * t->count + 1] = 3 * t->count + 1;
    t->indices[3 * t->count + 2] = 3 * t->count + 2;
    t->terrain[t->count] = (unsigned char)terrain;
    t->flags[t->count] = (unsigned char)flags;
    t->count++;
    return 0;
}

static void FreeTris(Tris *t) {
    free(t->verts);
    free(t->indices);
    free(t->terrain);
    free(t->flags);
    free(t->conveyor);
}

/* conveyors: the scene's table, for the static triangles; NULL for a body. */
static HsMesh MeshOf(const Tris *t, const HostScene *s) {
    HsMesh m;

    m.verts = t->verts;
    m.vertCount = 3 * t->count;
    m.indices = t->indices;
    m.terrain = t->terrain;
    m.triFlags = t->flags;
    m.triCount = t->count;
    m.flags = 0;
    m.conveyor = s != NULL && t->anyConveyor ? t->conveyor : NULL;
    m.conveyors = s != NULL && t->anyConveyor ? s->conveyors : NULL;
    m.conveyorCount = s != NULL && t->anyConveyor ? s->conveyorCount : 0;
    return m;
}

static Event *NewEvent(HostScene *s, double frame, int type) {
    Event *e;

    s->events = (Event *)realloc(s->events, (s->eventCount + 1) * sizeof(Event));
    if (s->events == NULL) {
        return NULL;
    }
    e = &s->events[s->eventCount++];
    memset(e, 0, sizeof(*e));
    e->frame = frame;
    e->type = type;
    return e;
}

static Body *FindBody(HostScene *s, int fileId) {
    int i;

    for (i = 0; i < s->bodyCount; i++) {
        if (s->bodies[i].fileId == fileId) {
            return &s->bodies[i];
        }
    }
    return NULL;
}

static int ReadPose(const char *p, float scale, HsPose *pose) {
    if (sscanf(p, "%f %f %f %f %f %f %f %f %f %f", &pose->pos[0], &pose->pos[1], &pose->pos[2], &pose->rot[0],
               &pose->rot[1], &pose->rot[2], &pose->rot[3], &pose->scale[0], &pose->scale[1], &pose->scale[2])
        != 10) {
        return -1;
    }
    pose->pos[0] *= scale;
    pose->pos[1] *= scale;
    pose->pos[2] *= scale;
    return 0;
}

/* The place line's words after the frame. */
static int ReadPlace(char *p, float scale, HsPlace *pl) {
    char *w = strtok(p, " \t\r\n");

    while (w != NULL) {
        float *dst = NULL;
        unsigned int bit = 0;

        if (!strcmp(w, "hand")) {
            dst = pl->hand;
            bit = 0x01;
        } else if (!strcmp(w, "ball")) {
            dst = pl->ball;
            bit = 0x02;
        } else if (!strcmp(w, "handvel")) {
            dst = pl->handVel;
            bit = 0x04;
        } else if (!strcmp(w, "ballvel")) {
            dst = pl->ballVel;
            bit = 0x04;
        } else if (!strcmp(w, "yaw")) {
            w = strtok(NULL, " \t\r\n");
            if (w == NULL) {
                return -1;
            }
            pl->handYaw = (float)atof(w);
            pl->what |= 0x08;
        } else if (!strcmp(w, "camera")) {
            pl->what |= 0x10;
        } else {
            return -1;
        }
        if (dst != NULL) {
            int i;

            for (i = 0; i < 3; i++) {
                w = strtok(NULL, " \t\r\n");
                if (w == NULL) {
                    return -1;
                }
                dst[i] = (float)atof(w) * (bit == 0x04 ? 1.0f : scale);
            }
            pl->what |= bit;
        }
        w = strtok(NULL, " \t\r\n");
    }
    return 0;
}

static int CompareEvents(const void *a, const void *b) {
    const Event *x = (const Event *)a;
    const Event *y = (const Event *)b;

    if (x->frame != y->frame) {
        return x->frame < y->frame ? -1 : 1;
    }
    return 0;
}

HostScene *hs_load(const char *path, char *err, int errSize) {
    HostScene *s;
    FILE *f = fopen(path, "r");
    char line[512];
    float scale = 1.0f;
    Body *body = NULL;
    int lineNo = 0;
    int i;

    if (f == NULL) {
        snprintf(err, errSize, "cannot open %s", path);
        return NULL;
    }
    s = (HostScene *)calloc(1, sizeof(HostScene));
    if (s == NULL) {
        fclose(f);
        snprintf(err, errSize, "out of memory");
        return NULL;
    }
    while (fgets(line, sizeof(line), f)) {
        char word[32];
        char *hash = strchr(line, '#');
        char *rest;
        double frame;
        int n;
        int id;
        Event *e;

        lineNo++;
        if (hash != NULL) {
            *hash = 0;
        }
        if (sscanf(line, "%31s%n", word, &n) != 1) {
            continue;
        }
        rest = line + n;
        if (!strcmp(word, "tri")) {
            float v[9];
            int terrain = 0;
            int flags = 0;
            int conveyor = 0;
            int got = sscanf(rest, "%f %f %f %f %f %f %f %f %f %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                             &v[6], &v[7], &v[8], &terrain, &flags, &conveyor);

            if (got < 9 || conveyor < 0 || conveyor > MAX_CONVEYORS || (body != NULL && conveyor != 0)) {
                goto bad;
            }
            for (i = 0; i < 9; i++) {
                v[i] *= scale;
            }
            if (AddTri(body != NULL ? &body->tris : &s->stat, v, terrain, flags, conveyor) != 0) {
                goto bad;
            }
            continue;
        }
        if (!strcmp(word, "water")) {
            HsWater w;
            char off[8];
            int got;

            memset(&w, 0, sizeof(w));
            if (sscanf(rest, "%lf %7s", &frame, off) == 2 && !strcmp(off, "off")) {
                if (NewEvent(s, frame, EV_WATER_OFF) == NULL) {
                    goto bad;
                }
                continue;
            }
            got = sscanf(rest, "%f %f %f %f %f %f %f %f %f %f", &w.min[0], &w.min[1], &w.min[2], &w.size[0],
                         &w.size[1], &w.size[2], &w.surface, &w.current[0], &w.current[1], &w.current[2]);
            if (got != 7 && got != 10) {
                goto bad;
            }
            for (i = 0; i < 3; i++) {
                w.min[i] *= scale;
                w.size[i] *= scale;
                w.current[i] *= scale;
            }
            w.surface *= scale;
            s->waters = (HsWater *)realloc(s->waters, (s->waterCount + 1) * sizeof(HsWater));
            if (s->waters == NULL) {
                goto bad;
            }
            s->waters[s->waterCount++] = w;
            continue;
        }
        if (!strcmp(word, "conveyor")) {
            float v[3];

            if (sscanf(rest, "%d %f %f %f", &id, &v[0], &v[1], &v[2]) != 4 || id < 1 || id > MAX_CONVEYORS) {
                goto bad;
            }
            for (i = 0; i < 3; i++) {
                s->conveyors[3 * (id - 1) + i] = v[i] * scale;
            }
            if ((unsigned int)id > s->conveyorCount) {
                s->conveyorCount = (unsigned int)id;
            }
            continue;
        }
        if (!strcmp(word, "scale")) {
            if (sscanf(rest, "%f", &scale) != 1) {
                goto bad;
            }
        } else if (!strcmp(word, "options")) {
            char *w = strtok(rest, " \t\r\n");

            while (w != NULL) {
                char *v = strtok(NULL, " \t\r\n");

                if (v == NULL) {
                    goto bad;
                }
                if (!strcmp(w, "cell")) {
                    s->opt.cellSize = (float)atof(v);
                } else if (!strcmp(w, "maxtris")) {
                    s->opt.maxChunkTris = (unsigned int)atol(v);
                } else if (!strcmp(w, "link")) {
                    s->opt.linkRadius = (float)atof(v);
                } else if (!strcmp(w, "keep")) {
                    s->opt.keepLevel = (unsigned int)atol(v);
                } else if (!strcmp(w, "off")) {
                    s->opt.levelOff = (unsigned int)atol(v);
                } else {
                    goto bad;
                }
                w = strtok(NULL, " \t\r\n");
            }
        } else if (!strcmp(word, "static")) {
            if (sscanf(rest, "%lf", &s->staticFrame) != 1) {
                goto bad;
            }
        } else if (!strcmp(word, "body")) {
            if (body != NULL || sscanf(rest, "%d%n", &id, &n) != 1 || FindBody(s, id) != NULL) {
                goto bad;
            }
            s->bodies = (Body *)realloc(s->bodies, (s->bodyCount + 1) * sizeof(Body));
            if (s->bodies == NULL) {
                goto bad;
            }
            body = &s->bodies[s->bodyCount++];
            memset(body, 0, sizeof(*body));
            body->fileId = id;
            if (ReadPose(rest + n, scale, &body->start) != 0) {
                goto bad;
            }
        } else if (!strcmp(word, "end")) {
            if (body == NULL || body->tris.count == 0) {
                goto bad;
            }
            body = NULL;
        } else if (!strcmp(word, "move")) {
            char kind[16];
            Body *b;

            if (sscanf(rest, "%d %15s %lf%n", &id, kind, &frame, &n) != 3 || (b = FindBody(s, id)) == NULL
                || !(frame > 0.0)) {
                goto bad;
            }
            b->period = frame;
            if (!strcmp(kind, "sine")) {
                if (sscanf(rest + n, "%f %f %f", &b->amp[0], &b->amp[1], &b->amp[2]) != 3) {
                    goto bad;
                }
                for (i = 0; i < 3; i++) {
                    b->amp[i] *= scale;
                }
                b->motion = 1;
            } else if (!strcmp(kind, "spin")) {
                b->motion = 2;
            } else {
                goto bad;
            }
        } else {
            if (sscanf(rest, "%lf%n", &frame, &n) != 1) {
                goto bad;
            }
            rest += n;
            if (!strcmp(word, "pose")) {
                if ((e = NewEvent(s, frame, EV_POSE)) == NULL || sscanf(rest, "%d%n", &e->fileId, &n) != 1
                    || ReadPose(rest + n, scale, &e->pose) != 0) {
                    goto bad;
                }
            } else if (!strcmp(word, "bodyconveyor")) {
                if ((e = NewEvent(s, frame, EV_BODY_CONVEYOR)) == NULL
                    || sscanf(rest, "%d %f %f %f", &e->fileId, &e->pos[0], &e->pos[1], &e->pos[2]) != 4) {
                    goto bad;
                }
                for (i = 0; i < 3; i++) {
                    e->pos[i] *= scale;
                }
            } else if (!strcmp(word, "remove")) {
                if ((e = NewEvent(s, frame, EV_REMOVE)) == NULL || sscanf(rest, "%d", &e->fileId) != 1) {
                    goto bad;
                }
            } else if (!strcmp(word, "place")) {
                if ((e = NewEvent(s, frame, EV_PLACE)) == NULL || ReadPlace(rest, scale, &e->place) != 0) {
                    goto bad;
                }
            } else if (!strcmp(word, "respawn")) {
                if ((e = NewEvent(s, frame, EV_RESPAWN)) == NULL
                    || sscanf(rest, "%f %f %f %f", &e->pos[0], &e->pos[1], &e->pos[2], &e->yaw) != 4) {
                    goto bad;
                }
                for (i = 0; i < 3; i++) {
                    e->pos[i] *= scale;
                }
            } else if (!strcmp(word, "view")) {
                char off[8];

                if (sscanf(rest, "%7s", off) == 1 && !strcmp(off, "off")) {
                    if (NewEvent(s, frame, EV_VIEW_OFF) == NULL) {
                        goto bad;
                    }
                    continue;
                }
                if ((e = NewEvent(s, frame, EV_VIEW)) == NULL) {
                    goto bad;
                }
                {
                    HsView *v = &e->view;

                    if (sscanf(rest, "%i %f %f %f %f %f %f %f %f %f %f %f %f %f", (int *)&v->flags, &v->eye[0],
                               &v->eye[1], &v->eye[2], &v->at[0], &v->at[1], &v->at[2], &v->up[0], &v->up[1],
                               &v->up[2], &v->fovy, &v->aspect, &v->nearZ, &v->farZ)
                        != 14) {
                        goto bad;
                    }
                    for (i = 0; i < 3; i++) {
                        v->eye[i] *= scale;
                        v->at[i] *= scale;
                    }
                    v->nearZ *= scale;
                    v->farZ *= scale;
                }
            } else if (!strcmp(word, "clear")) {
                if (NewEvent(s, frame, EV_CLEAR) == NULL) {
                    goto bad;
                }
            } else if (!strcmp(word, "goto")) {
                if ((e = NewEvent(s, frame, EV_GOTO)) == NULL || sscanf(rest, "%d", &e->level) != 1) {
                    goto bad;
                }
            } else {
                goto bad;
            }
        }
    }
    fclose(f);
    if (body != NULL) {
        snprintf(err, errSize, "%s: a body without its end", path);
        hs_free(s);
        return NULL;
    }
    /* A stable sort by frame: insertion, the events are few. */
    for (i = 1; i < s->eventCount; i++) {
        Event e = s->events[i];
        int j = i - 1;

        while (j >= 0 && CompareEvents(&s->events[j], &e) > 0) {
            s->events[j + 1] = s->events[j];
            j--;
        }
        s->events[j + 1] = e;
    }
    return s;

bad:
    fclose(f);
    snprintf(err, errSize, "%s:%d: cannot read the line", path, lineNo);
    hs_free(s);
    return NULL;
}

void hs_free(HostScene *s) {
    int i;

    if (s == NULL) {
        return;
    }
    FreeTris(&s->stat);
    for (i = 0; i < s->bodyCount; i++) {
        FreeTris(&s->bodies[i].tris);
    }
    free(s->bodies);
    free(s->events);
    free(s->waters);
    free(s);
}

static void Report(const HsOps *ops, void *user, double frame, const char *what, int rc) {
    char line[128];

    snprintf(line, sizeof(line), "hostscene: frame %.2f: %s -> %d", frame, what, rc);
    if (ops->log != NULL) {
        ops->log(user, line);
    }
}

/* sin and cos from + and * alone: every C library and compiler gives the same bits, so the headless mode and every
   test host compute the same poses. x is in [-pi, pi]. */
static double Sine(double x) {
    double x2 = x * x;
    double term = x;
    double sum = x;
    int n;

    for (n = 1; n < 12; n++) {
        term = -term * x2 / (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

/* The angle 2 pi t / period, brought into [-pi, pi]. */
static double Phase(double t, double period) {
    double f = fmod(t, period) / period;

    return 2.0 * PI * (f > 0.5 ? f - 1.0 : f);
}

/* The pose of a moving body at time t. */
static HsPose MotionPose(const Body *b, double t) {
    HsPose p = b->start;
    int i;

    if (b->motion == 1) {
        double k = Sine(Phase(t, b->period));

        for (i = 0; i < 3; i++) {
            p.pos[i] = (float)(b->start.pos[i] + b->amp[i] * k);
        }
    } else if (b->motion == 2) {
        /* Half the turn's angle: the quaternion (0, sin(a / 2), 0, cos(a / 2)). */
        double a = Phase(t, b->period) / 2.0;

        p.rot[0] = 0.0f;
        p.rot[1] = (float)Sine(a);
        p.rot[2] = 0.0f;
        p.rot[3] = (float)Sine(PI / 2.0 - a);
    }
    return p;
}

void hs_update(HostScene *s, double tStart, double step, const HsOps *ops, void *user) {
    int i;

    if (!s->staticDone && tStart >= s->staticFrame) {
        s->staticDone = 1;
        if (s->stat.count > 0) {
            HsMesh m = MeshOf(&s->stat, s);

            Report(ops, user, tStart, "static", ops->setStatic(user, &m, &s->opt));
        }
        if (s->waterCount > 0) {
            Report(ops, user, tStart, "water", ops->setWater(user, s->waters, s->waterCount));
        }
        for (i = 0; i < s->bodyCount; i++) {
            HsMesh m = MeshOf(&s->bodies[i].tris, NULL);
            HsPose p = MotionPose(&s->bodies[i], tStart);

            Report(ops, user, tStart, "body", ops->bodyAdd(user, &m, &p, &s->bodies[i].id));
        }
    }
    while (s->nextEvent < s->eventCount && tStart >= s->events[s->nextEvent].frame) {
        Event *e = &s->events[s->nextEvent++];
        Body *b = FindBody(s, e->fileId);

        switch (e->type) {
        case EV_POSE:
            Report(ops, user, tStart, "pose", b != NULL ? ops->bodyMove(user, b->id, &e->pose) : -1);
            break;
        case EV_REMOVE:
            Report(ops, user, tStart, "remove", b != NULL ? ops->bodyRemove(user, b->id) : -1);
            if (b != NULL) {
                b->id = 0;
                b->motion = 0;
            }
            break;
        case EV_PLACE:
            Report(ops, user, tStart, "place", ops->place(user, &e->place));
            break;
        case EV_RESPAWN:
            Report(ops, user, tStart, "respawn", ops->respawn(user, e->pos, e->yaw));
            break;
        case EV_VIEW:
            Report(ops, user, tStart, "view", ops->setView(user, &e->view));
            break;
        case EV_VIEW_OFF:
            Report(ops, user, tStart, "view off", ops->setView(user, NULL));
            break;
        case EV_CLEAR:
            Report(ops, user, tStart, "clear", ops->clear(user));
            for (i = 0; i < s->bodyCount; i++) {
                s->bodies[i].id = 0;
                s->bodies[i].motion = 0;
            }
            break;
        case EV_GOTO:
            Report(ops, user, tStart, "goto", ops->gotoLevel(user, e->level));
            break;
        case EV_WATER_OFF:
            Report(ops, user, tStart, "water off", ops->setWater(user, NULL, 0));
            break;
        case EV_BODY_CONVEYOR:
            Report(ops, user, tStart, "body conveyor", b != NULL ? ops->bodyConveyor(user, b->id, e->pos) : -1);
            break;
        }
    }
    for (i = 0; i < s->bodyCount; i++) {
        Body *b = &s->bodies[i];

        if (b->id != 0 && b->motion != 0) {
            HsPose p = MotionPose(b, tStart + step);
            int rc = ops->bodyMove(user, b->id, &p);

            if (rc != 0) {
                Report(ops, user, tStart, "move", rc);
            }
        }
    }
}

static unsigned int Bits(float f) {
    unsigned int u;

    memcpy(&u, &f, sizeof(u));
    return u;
}

void hs_bodies_line(char *out, int size, long frame, const HsBodies *b) {
    snprintf(out, size,
             "bodies %ld action %d held %d ground %d %d terrain %d %d ready %d restarts %u yaw %08x handvel %08x %08x "
             "%08x ballvel %08x %08x %08x void %d %d voids %u %u safe %d %08x %08x %08x %08x %08x %08x water %d %d",
             frame, b->handAction, b->ballHeld, b->handOnGround, b->ballOnGround, b->handTerrain, b->ballTerrain,
             b->ready, b->restarts, Bits(b->handYaw), Bits(b->handVel[0]), Bits(b->handVel[1]), Bits(b->handVel[2]),
             Bits(b->ballVel[0]), Bits(b->ballVel[1]), Bits(b->ballVel[2]), b->handVoid, b->ballVoid, b->handVoids,
             b->ballVoids, b->safeKnown, Bits(b->handSafe[0]), Bits(b->handSafe[1]), Bits(b->handSafe[2]),
             Bits(b->ballSafe[0]), Bits(b->ballSafe[1]), Bits(b->ballSafe[2]), b->handWater, b->ballWater);
}
