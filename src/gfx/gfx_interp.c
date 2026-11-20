/* Interpolation between two game frames (see docs/interpolation.md).

   The game builds one display list per original frame. In "Original physics" mode the window draws it several
   times, between the previous frame and the latest one. This file keeps the matrix loads (G_MTX) of the two
   frames and blends them.

   Every G_MTX of a frame is a record: a key, the record of its parent matrix, and the matrix. The key is a hash of
   the display list call path, the kind of load and the address of the data the matrix is used for (the next vertex
   load or display list call), which is the same from frame to frame for the same object. The parent is the record
   on top of the matrix stack when the matrix was loaded, so a bone matches only inside the object whose root
   matched. A record of the latest frame takes the unused record of the previous frame that has the same key and
   the same matched parent and lies nearest to it. It blends only when the change is small.

   Two passes use the records. gfx_interp_run_record runs the latest frame's display list once, as soon as the game
   has built it, without drawing, and fills the table. A drawn frame then interprets the same list again. At each
   G_MTX it finds its record by (key, parent, occurrence number) and replaces the matrix with the blend. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_host.h"

#include "gfx_internal.h"

#define MAX_RECS 8192
#define NB 8192 /* hash buckets, a power of two */
#define MAX_VIEWPORTS 64

/* DEC_TRS: scale, rotation and translation. DEC_POLAR: a rotation and a symmetric stretch (a squashed or sheared
   model); DEC_TRS is the case of a stretch along the axes. DEC_VIEW: the camera. DEC_LINEAR: anything else. */
enum { DEC_LINEAR, DEC_TRS, DEC_VIEW, DEC_POLAR };

/* How a pair of records blends. */
enum { BLEND_NONE, BLEND_FULL, BLEND_MOVE };

/* The largest changes between two frames that still blend. A larger change is a cut: a teleport, a respawn, a
   camera cut, or something that turns more than a quarter turn in a frame (the blend could go the wrong way). */
#define MAX_TURN 2.967f          /* radians: 170 degrees, the largest turn that blends on the shortest way */
#define MAX_SPIN 5.236f          /* radians: 300 degrees, the largest turn that blends on the long way */
#define MAX_VIEW_TURN_COS 0.866f /* the camera turns up to 60 degrees */
#define MAX_MOVE 150.0f          /* world units between two frames, for a model matrix */
#define MAX_EYE_MOVE 100.0f      /* the same for the camera's position */
#define MAX_SCALE_RATIO 2.0f
#define MAX_LINEAR_REL 0.3f
#define LOOSE_MOVE 80.0f /* the largest move of a record that matches by kind and path only */

typedef struct Dec {
    float q[4]; /* rotation as a quaternion x, y, z, w */
    float s[3]; /* scale of each row; the diagonal of p */
    float p[9]; /* the stretch, row-major; the matrix's 3x3 part is p times the rotation */
    float t[3]; /* translation; the camera's position for a view matrix */
    u8 mode;
} Dec;

typedef struct Rec {
    u32 key;
    u32 lkey;       /* the key without the address the matrix is used for */
    int parent;     /* index in this table, -1 for none, -2 when the parent has no record */
    int match;      /* index in the previous table of the record to blend with, -1 for none */
    int ord;        /* number of the earlier records with the same key and parent */
    int groupCount; /* on the first of a group: the group's size so far, then the draw pass's counter */
    u32 seenStamp;
    int nextA, nextB, nextC;
    u8 kind;
    u8 used;
    u8 blendMode; /* BLEND_FULL, or BLEND_MOVE: only the translation blends */
    u8 hasTurn;   /* turn is set: this record has a partner whose rotation is known */
    float turn[3]; /* the rotation from the partner to this record, as an axis times an angle in radians; the blend
                      follows it, so it may be more than half a turn */
    Dec dec;
    float m[16];
} Rec;

typedef struct Table {
    Rec *rec;
    int count;
    int bucketA[NB]; /* by (key, parent) */
    int bucketB[NB]; /* by (key, parent, ord) */
    int bucketC[NB]; /* by (lkey, parent) */
} Table;

/* A texture rectangle: the screen coordinates of its corners. The game draws its 2D elements and its sprites
   (particles, stars) as rectangles; they are named by the texture they show. */
#define MAX_RECTS 4096
#define MAX_RECT_MOVE 80.0f /* screen pixels between two frames */
#define LOOSE_RECT_MOVE 40.0f /* the same for a rectangle that matches by its image only */
#define MAX_SPRITE_MOVE 80.0f /* world units between two frames, for a 3D sprite */

typedef struct RRec {
    u32 key, lkey; /* lkey: the image alone, without the part shown */
    int ord, groupCount, nextA, nextC, match;
    u32 seenStamp;
    u8 used;
    u8 is3d; /* a 3D sprite: placed by the game with the camera of its frame; world and dist are set */
    float r[4]; /* x0, y0, x1, y1 */
    float world[3]; /* the world position behind the centre of the rectangle */
    float dist;     /* the distance from the camera to it */
} RRec;

static struct {
    RRec rec[2][MAX_RECTS];
    int count[2];
    int bucket[2][NB];
    int bucketC[2][NB];
} *R;

static struct {
    Table *tab[2];
    int cur;
    int prevValid, curValid;
    u32 stamp;
    int drawing;      /* a drawn frame blends at t */
    float t;
    float vp[2][MAX_VIEWPORTS][4];
    int vpCount[2];
    int vpOrd;
    int vpValid;
    GfxInterpStats stats;
    int logStats;
    int debug; /* GLOVER_INTERP_DEBUG: log each cut with the numbers that caused it */
} I;

int gfxInterpMode;
u32 gfx_interp_track; /* debugging: the key of a matrix whose screen position a drawn frame logs */

float gfx_interp_time(void) {
    return I.t;
}
u32 gfxVolatileLo, gfxVolatileHi;

static u32 mix(u32 h, u32 v) {
    h = (h ^ v) * 0x01000193u;
    return h ^ (h >> 15);
}

static u32 hash_a(u32 key, int parent) {
    return mix(mix(0x811C9DC5u, key), (u32)parent) & (NB - 1);
}

static u32 hash_b(u32 key, int parent, int ord) {
    return mix(mix(mix(0x811C9DC5u, key), (u32)parent), (u32)ord) & (NB - 1);
}

u32 gfx_interp_mix(u32 h, u32 v) {
    return mix(h, v);
}

int gfx_interp_is_volatile(u32 addr) {
    return addr >= gfxVolatileLo && addr < gfxVolatileHi;
}

/* ---- Decomposition and blending. Matrices are row-major and multiply row vectors, so row i of the 3x3 part is the
   image of axis i. */

static void quat_from_rows(const float u[3][3], float *q) {
    /* The usual rotation matrix (for column vectors) has u[j][i] at row i and column j. */
    float r00 = u[0][0], r01 = u[1][0], r02 = u[2][0];
    float r10 = u[0][1], r11 = u[1][1], r12 = u[2][1];
    float r20 = u[0][2], r21 = u[1][2], r22 = u[2][2];
    float tr = r00 + r11 + r22;
    float s;

    if (tr > 0.0f) {
        s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (r21 - r12) / s;
        q[1] = (r02 - r20) / s;
        q[2] = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        q[3] = (r21 - r12) / s;
        q[0] = 0.25f * s;
        q[1] = (r01 + r10) / s;
        q[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
        s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        q[3] = (r02 - r20) / s;
        q[0] = (r01 + r10) / s;
        q[1] = 0.25f * s;
        q[2] = (r12 + r21) / s;
    } else {
        s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        q[3] = (r10 - r01) / s;
        q[0] = (r02 + r20) / s;
        q[1] = (r12 + r21) / s;
        q[2] = 0.25f * s;
    }
}

/* Rows of the rotation, each scaled by sc[i], into m (the 3x3 part). */
static void rows_from_quat(const float *q, const float *sc, float *m) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float r[3][3]; /* r[i][j]: the usual rotation matrix */

    r[0][0] = 1 - 2 * (y * y + z * z), r[0][1] = 2 * (x * y - w * z), r[0][2] = 2 * (x * z + w * y);
    r[1][0] = 2 * (x * y + w * z), r[1][1] = 1 - 2 * (x * x + z * z), r[1][2] = 2 * (y * z - w * x);
    r[2][0] = 2 * (x * z - w * y), r[2][1] = 2 * (y * z + w * x), r[2][2] = 1 - 2 * (x * x + y * y);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            m[i * 4 + j] = sc[i] * r[j][i];
        }
    }
}

/* The rotation of a 3x3 matrix that is not far from one, by Newton's iteration on its polar decomposition: m
   (row-major) = p times r with r a rotation and p symmetric. Returns 0 when m is nearly singular or mirrored. */
static int polar(const float *m3, float *r, float *p) {
    double a[9];
    double norm = 0.0;
    int i;
    int j;
    int k;
    int it;

    for (i = 0; i < 9; i++) {
        norm += (double)m3[i] * m3[i];
    }
    norm = sqrt(norm);
    if (norm < 1e-5) {
        return 0;
    }
    for (i = 0; i < 9; i++) {
        a[i] = m3[i] / norm;
    }
    for (it = 0; it < 30; it++) {
        double c[9];
        double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6])
                     + a[2] * (a[3] * a[7] - a[4] * a[6]);
        double err = 0.0;

        if (det < 1e-4) {
            return 0;
        }
        /* The inverse transpose is the cofactor matrix over the determinant. */
        c[0] = (a[4] * a[8] - a[5] * a[7]) / det;
        c[1] = (a[5] * a[6] - a[3] * a[8]) / det;
        c[2] = (a[3] * a[7] - a[4] * a[6]) / det;
        c[3] = (a[2] * a[7] - a[1] * a[8]) / det;
        c[4] = (a[0] * a[8] - a[2] * a[6]) / det;
        c[5] = (a[1] * a[6] - a[0] * a[7]) / det;
        c[6] = (a[1] * a[5] - a[2] * a[4]) / det;
        c[7] = (a[2] * a[3] - a[0] * a[5]) / det;
        c[8] = (a[0] * a[4] - a[1] * a[3]) / det;
        for (i = 0; i < 9; i++) {
            double n = 0.5 * (a[i] + c[i]);
            err += fabs(n - a[i]);
            a[i] = n;
        }
        if (err < 1e-7) {
            break;
        }
    }
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            double d = 0.0;
            for (k = 0; k < 3; k++) {
                d += a[i * 3 + k] * a[j * 3 + k];
            }
            if (fabs(d - (i == j ? 1.0 : 0.0)) > 1e-3) {
                return 0;
            }
        }
    }
    for (i = 0; i < 9; i++) {
        r[i] = (float)a[i];
    }
    /* p = m times r transposed */
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            float d = 0.0f;
            for (k = 0; k < 3; k++) {
                d += m3[i * 3 + k] * r[j * 3 + k];
            }
            p[i * 3 + j] = d;
        }
    }
    return 1;
}

static void decompose(int kind, const float *m, Dec *d) {
    float u[3][3];
    float len[3];
    int i;
    int j;
    float det;
    int rigid = 1;

    d->mode = DEC_LINEAR;
    if (kind == 1 || fabsf(m[3]) > 1e-4f || fabsf(m[7]) > 1e-4f || fabsf(m[11]) > 1e-4f || fabsf(m[15] - 1.0f) > 1e-4f) {
        return;
    }
    for (i = 0; i < 3; i++) {
        len[i] = sqrtf(m[i * 4] * m[i * 4] + m[i * 4 + 1] * m[i * 4 + 1] + m[i * 4 + 2] * m[i * 4 + 2]);
        if (len[i] < 1e-5f) {
            return;
        }
        for (j = 0; j < 3; j++) {
            u[i][j] = m[i * 4 + j] / len[i];
        }
    }
    /* Rows that are not at right angles hold a shear. */
    if (fabsf(u[0][0] * u[1][0] + u[0][1] * u[1][1] + u[0][2] * u[1][2]) > 0.02f
        || fabsf(u[0][0] * u[2][0] + u[0][1] * u[2][1] + u[0][2] * u[2][2]) > 0.02f
        || fabsf(u[1][0] * u[2][0] + u[1][1] * u[2][1] + u[1][2] * u[2][2]) > 0.02f) {
        rigid = 0;
    }
    det = u[0][0] * (u[1][1] * u[2][2] - u[1][2] * u[2][1]) - u[0][1] * (u[1][0] * u[2][2] - u[1][2] * u[2][0])
          + u[0][2] * (u[1][0] * u[2][1] - u[1][1] * u[2][0]);
    if (!rigid || det < 0.9f) {
        /* A squashed or sheared model (the ball after a bounce) still has a rotation and a stretch. */
        float m3[9];
        float r[9];
        float p[9];

        if (kind != 0) {
            return;
        }
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                m3[i * 3 + j] = m[i * 4 + j];
            }
        }
        if (!polar(m3, r, p)) {
            return;
        }
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                u[i][j] = r[i * 3 + j];
            }
        }
        quat_from_rows(u, d->q);
        memcpy(d->p, p, sizeof(p));
        d->s[0] = p[0], d->s[1] = p[4], d->s[2] = p[8];
        d->t[0] = m[12], d->t[1] = m[13], d->t[2] = m[14];
        d->mode = DEC_POLAR;
        return;
    }
    quat_from_rows(u, d->q);
    d->s[0] = len[0], d->s[1] = len[1], d->s[2] = len[2];
    memset(d->p, 0, sizeof(d->p));
    d->p[0] = len[0], d->p[4] = len[1], d->p[8] = len[2];
    if (kind == 2) {
        /* A view matrix is a rigid motion. Its translation is -eye * R, so the camera's position is -t * R^T. */
        if (fabsf(len[0] - 1.0f) > 0.01f || fabsf(len[1] - 1.0f) > 0.01f || fabsf(len[2] - 1.0f) > 0.01f) {
            return;
        }
        for (i = 0; i < 3; i++) {
            d->t[i] = -(m[12] * m[i * 4] + m[13] * m[i * 4 + 1] + m[14] * m[i * 4 + 2]);
        }
        d->mode = DEC_VIEW;
    } else {
        d->t[0] = m[12], d->t[1] = m[13], d->t[2] = m[14];
        d->mode = DEC_TRS;
    }
}

/* True when a record is a model matrix with a bottom row of 0 0 0 1, so that its translation can blend alone. */
static int is_affine(const Rec *a) {
    return a->kind == 0 && a->dec.mode != DEC_VIEW;
}

/* The distance between the translations of two records, squared. */
static float move2(const Rec *a, const Rec *b) {
    float s = 0.0f;
    int i;

    if (a->dec.mode == DEC_VIEW && b->dec.mode == DEC_VIEW) {
        for (i = 0; i < 3; i++) {
            float d = a->dec.t[i] - b->dec.t[i];
            s += d * d;
        }
        return s;
    }
    for (i = 12; i < 15; i++) {
        float d = a->m[i] - b->m[i];
        s += d * d;
    }
    return s;
}

static float dist2(const Rec *a, const Rec *b) {
    float s = 0.0f;
    int i;

    if (a->kind == b->kind && a->kind != 1) {
        return move2(a, b);
    }
    for (i = 0; i < 16; i++) {
        float d = a->m[i] - b->m[i];
        s += d * d;
    }
    return s;
}

/* The product of two quaternions (x, y, z, w): a then b in the algebra of the stored quaternions. */
static void quat_mul(const float *a, const float *b, float *o) {
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
}

/* Sets b->turn to the rotation that the blend from a to b follows, and returns its angle in radians. The quaternion
   of a matrix is only known up to its sign, so the turn from a to b is the short way round (up to half a turn) or
   the long way in the other direction. A spinning part that turns more than half a turn in a frame looks, from
   the two frames alone, like a part that turns the short way the other way. The turn of the frame before decides:
   when the long way lies nearer to it than the short way, the part keeps spinning and the blend takes the long way. */
static float step_turn(const Rec *a, Rec *b, int *longWay) {
    float ca[4] = {-a->dec.q[0], -a->dec.q[1], -a->dec.q[2], a->dec.q[3]};
    float d[4];
    float sn;
    float ang;
    int i;

    quat_mul(ca, b->dec.q, d);
    if (d[3] < 0.0f) {
        for (i = 0; i < 4; i++) {
            d[i] = -d[i];
        }
    }
    sn = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    ang = 2.0f * atan2f(sn, d[3]);
    b->hasTurn = 1;
    *longWay = 0;
    if (sn < 1e-6f) {
        b->turn[0] = b->turn[1] = b->turn[2] = 0.0f;
        return 0.0f;
    }
    for (i = 0; i < 3; i++) {
        b->turn[i] = d[i] / sn * ang;
    }
    if (a->hasTurn && ang > 2.0f * 3.14159265f - MAX_SPIN) {
        float prev2 = a->turn[0] * a->turn[0] + a->turn[1] * a->turn[1] + a->turn[2] * a->turn[2];
        float far = 2.0f * 3.14159265f - ang;
        float ds = 0.0f;
        float dl = 0.0f;

        for (i = 0; i < 3; i++) {
            float l = -d[i] / sn * far;
            ds += (b->turn[i] - a->turn[i]) * (b->turn[i] - a->turn[i]);
            dl += (l - a->turn[i]) * (l - a->turn[i]);
        }
        /* The long way must be near the last turn, within half of it and 20 degrees. */
        if (dl < ds && sqrtf(dl) < 0.5f * sqrtf(prev2) + 0.35f) {
            for (i = 0; i < 3; i++) {
                b->turn[i] = -d[i] / sn * far;
            }
            *longWay = 1;
            return far;
        }
    }
    return ang;
}

/* How the change from a to b blends. BLEND_NONE: the change is a cut. BLEND_MOVE: the translation is small but
   the turn or the stretch is not, so only the translation blends and the rest takes b's value (a ball that spins a
   quarter turn or more in a frame still moves smoothly). */
static int acceptable(const Rec *a, Rec *b) {
    int longWay;
    int i;

    if (a->kind != b->kind) {
        return BLEND_NONE;
    }
    if (a->dec.mode == DEC_VIEW || b->dec.mode == DEC_VIEW) {
        float dot;

        if (a->dec.mode != b->dec.mode) {
            return BLEND_NONE;
        }
        dot = fabsf(a->dec.q[0] * b->dec.q[0] + a->dec.q[1] * b->dec.q[1] + a->dec.q[2] * b->dec.q[2]
                    + a->dec.q[3] * b->dec.q[3]);
        step_turn(a, b, &longWay);
        return dot < MAX_VIEW_TURN_COS || move2(a, b) > MAX_EYE_MOVE * MAX_EYE_MOVE ? BLEND_NONE : BLEND_FULL;
    }
    if (is_affine(a) && is_affine(b)) {
        if (move2(a, b) > MAX_MOVE * MAX_MOVE) {
            return BLEND_NONE;
        }
        if (a->dec.mode != DEC_LINEAR && b->dec.mode != DEC_LINEAR) {
            float turn = step_turn(a, b, &longWay);
            float da = 0.0f;
            float db = 0.0f;
            float dd = 0.0f;

            if (turn > (longWay ? MAX_SPIN : MAX_TURN)) {
                return BLEND_MOVE;
            }
            for (i = 0; i < 3; i++) {
                float lo = fminf(a->dec.s[i], b->dec.s[i]);
                float hi = fmaxf(a->dec.s[i], b->dec.s[i]);
                if (hi > lo * MAX_SCALE_RATIO) {
                    return BLEND_MOVE;
                }
            }
            for (i = 0; i < 9; i++) {
                da += a->dec.p[i] * a->dec.p[i];
                db += b->dec.p[i] * b->dec.p[i];
                dd += (a->dec.p[i] - b->dec.p[i]) * (a->dec.p[i] - b->dec.p[i]);
            }
            return dd > 0.25f * fmaxf(da, db) ? BLEND_MOVE : BLEND_FULL;
        }
        /* A degenerate matrix (a scale near zero): blend the entries if they are close, else only the translation. */
        for (i = 0; i < 12; i++) {
            float big = fmaxf(fmaxf(fabsf(a->m[i]), fabsf(b->m[i])), 1e-3f);
            if (i % 4 != 3 && fabsf(a->m[i] - b->m[i]) > MAX_LINEAR_REL * big && fabsf(a->m[i] - b->m[i]) > 0.01f) {
                return BLEND_MOVE;
            }
        }
        return BLEND_FULL;
    }
    /* A projection, or a matrix with a perspective part: entry by entry. */
    for (i = 0; i < 16; i++) {
        float big = fmaxf(fmaxf(fabsf(a->m[i]), fabsf(b->m[i])), 1e-3f);
        float lim = i >= 12 && i < 15 ? MAX_MOVE : MAX_LINEAR_REL * big;
        if (fabsf(a->m[i] - b->m[i]) > lim && fabsf(a->m[i] - b->m[i]) > 0.01f) {
            return BLEND_NONE;
        }
    }
    return BLEND_FULL;
}

/* out = the blend of a (t = 0) and b (t = 1). */
static void blend(float *out, const Rec *a, const Rec *b, float t) {
    int i;

    if (b->blendMode == BLEND_MOVE) {
        memcpy(out, b->m, 16 * sizeof(float));
        for (i = 12; i < 15; i++) {
            out[i] = a->m[i] + (b->m[i] - a->m[i]) * t;
        }
        return;
    }
    if (a->dec.mode == DEC_LINEAR || b->dec.mode == DEC_LINEAR || (a->dec.mode == DEC_VIEW) != (b->dec.mode == DEC_VIEW)) {
        for (i = 0; i < 16; i++) {
            out[i] = a->m[i] + (b->m[i] - a->m[i]) * t;
        }
        return;
    }
    {
        float q[4];
        float dq[4];
        float pm[9];
        float rm[9];
        float tr[3];
        static const float one[3] = {1.0f, 1.0f, 1.0f};
        float ang = sqrtf(b->turn[0] * b->turn[0] + b->turn[1] * b->turn[1] + b->turn[2] * b->turn[2]);
        int j;
        int k;

        /* The blended rotation is a's rotation followed by the part t of the turn from a to b. */
        if (ang > 1e-6f) {
            float sn = sinf(0.5f * ang * t) / ang;

            dq[0] = b->turn[0] * sn, dq[1] = b->turn[1] * sn, dq[2] = b->turn[2] * sn;
            dq[3] = cosf(0.5f * ang * t);
            quat_mul(a->dec.q, dq, q);
        } else {
            memcpy(q, a->dec.q, sizeof(q));
        }
        for (i = 0; i < 3; i++) {
            tr[i] = a->dec.t[i] + (b->dec.t[i] - a->dec.t[i]) * t;
        }
        for (i = 0; i < 9; i++) {
            pm[i] = a->dec.p[i] + (b->dec.p[i] - a->dec.p[i]) * t;
        }
        memset(out, 0, 16 * sizeof(float));
        out[15] = 1.0f;
        {
            float full[16] = {0};

            rows_from_quat(q, one, full);
            for (i = 0; i < 3; i++) {
                for (j = 0; j < 3; j++) {
                    rm[i * 3 + j] = full[i * 4 + j];
                }
            }
        }
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                float d = 0.0f;
                for (k = 0; k < 3; k++) {
                    d += pm[i * 3 + k] * rm[k * 3 + j];
                }
                out[i * 4 + j] = d;
            }
        }
        if (a->dec.mode == DEC_VIEW) {
            for (i = 0; i < 3; i++) {
                out[12 + i] = -(tr[0] * out[i] + tr[1] * out[4 + i] + tr[2] * out[8 + i]);
            }
        } else {
            out[12] = tr[0], out[13] = tr[1], out[14] = tr[2];
        }
    }
}

/* ---- The tables. */

static int ensure_tables(void) {
    int i;

    if (I.tab[0] != NULL) {
        return 1;
    }
    R = calloc(1, sizeof(*R));
    if (R == NULL) {
        return 0;
    }
    for (i = 0; i < 2; i++) {
        I.tab[i] = calloc(1, sizeof(Table));
        if (I.tab[i] != NULL) {
            I.tab[i]->rec = calloc(MAX_RECS, sizeof(Rec));
        }
        if (I.tab[i] == NULL || I.tab[i]->rec == NULL) {
            return 0;
        }
    }
    return 1;
}

static void clear_table(Table *t) {
    int i;

    t->count = 0;
    for (i = 0; i < NB; i++) {
        t->bucketA[i] = t->bucketB[i] = t->bucketC[i] = -1;
    }
}

static int find_b(const Table *t, u32 key, int parent, int ord) {
    int i = t->bucketB[hash_b(key, parent, ord)];

    while (i >= 0) {
        const Rec *r = &t->rec[i];

        if (r->key == key && r->parent == parent && r->ord == ord) {
            return i;
        }
        i = r->nextB;
    }
    return -1;
}

static void match_all(void);
static void match_rects(void);

void gfx_interp_begin_record(int cut) {
    if (!ensure_tables()) {
        I.curValid = I.prevValid = 0;
        return;
    }
    I.prevValid = I.curValid && !cut;
    I.cur ^= 1;
    clear_table(I.tab[I.cur]);
    R->count[I.cur] = 0;
    memset(R->bucket[I.cur], 0xFF, sizeof(R->bucket[I.cur]));
    memset(R->bucketC[I.cur], 0xFF, sizeof(R->bucketC[I.cur]));
    I.curValid = 1;
    I.stamp++;
    I.vpOrd = 0;
    I.vpCount[I.cur] = 0;
    I.vpValid = I.prevValid;
    memset(&I.stats, 0, sizeof(I.stats));
    gfxInterpMode = 1;
}

void gfx_interp_end_record(void) {
    gfxInterpMode = 0;
    if (I.curValid) {
        match_all();
        if (I.prevValid) {
            match_rects();
        }
    }
    I.debug = gfx_getenv("GLOVER_INTERP_DEBUG") != NULL;
    I.logStats = I.logStats || gfx_getenv("GLOVER_INTERP_STATS") != NULL;
    if (I.logStats) {
        gfx_log("interp: %d of %d rectangles blended", I.stats.rectsBlended, I.stats.rects);
        gfx_log("interp: %d matrices, %d blended (%d moved, %d by kind only), %d without a partner, %d cut, %d viewports%s",
                I.stats.total, I.stats.blended, I.stats.moved, I.stats.loose, I.stats.noPartner, I.stats.cut, I.vpCount[I.cur],
                I.prevValid ? "" : " (no previous frame)");
    }
}

/* Called for each G_MTX while recording. Returns the record's index, or -2 when there is none. The partners are
   found when the frame is complete (match_all). */
int gfx_interp_record_mtx(u32 key, u32 lkey, int parent, int kind, const float *m) {
    Table *c = I.tab[I.cur];
    Rec *r;
    int idx;
    int first;
    u32 h;

    I.stats.total++;
    if (c->count >= MAX_RECS) {
        return -2;
    }
    idx = c->count++;
    r = &c->rec[idx];
    memset(r, 0, offsetof(Rec, m));
    memcpy(r->m, m, sizeof(r->m));
    r->key = key;
    r->lkey = lkey;
    r->parent = parent;
    r->match = -1;
    r->kind = (u8)kind;
    decompose(kind, m, &r->dec);
    first = find_b(c, key, parent, 0);
    if (first < 0) {
        r->ord = 0;
        r->groupCount = 1;
    } else {
        r->ord = c->rec[first].groupCount++;
    }
    h = hash_a(key, parent);
    r->nextA = c->bucketA[h];
    c->bucketA[h] = idx;
    h = hash_b(key, parent, r->ord);
    r->nextB = c->bucketB[h];
    c->bucketB[h] = idx;
    h = hash_a(lkey, parent);
    r->nextC = c->bucketC[h];
    c->bucketC[h] = idx;
    return idx;
}

/* Finds the record of the previous frame that each record of the latest frame blends with. A record takes the
   unused one with the same key and the same matched parent that lies nearest to it. A record without one then
   takes the nearest unused record of the same kind and path, within LOOSE_MOVE units: the address a mesh is used
   from changes from frame to frame when the game keeps two copies of it. The parent comes before its children in
   the table, so a parent's match is known first. */
static void debug_pair(const char *what, int i, const Rec *q, const Rec *r) {
    float dot = fabsf(q->dec.q[0] * r->dec.q[0] + q->dec.q[1] * r->dec.q[1] + q->dec.q[2] * r->dec.q[2]
                      + q->dec.q[3] * r->dec.q[3]);

    gfx_log("%s f%u rec %d key %08x parent %d mode %d/%d dist %.1f turn %.1f (blend %.1f) scale %.2f %.2f %.2f -> %.2f %.2f %.2f pos %.1f %.1f %.1f",
            what, I.stamp, i, r->key, r->parent, q->dec.mode, r->dec.mode, sqrtf(dist2(q, r)),
            2.0f * acosf(fminf(dot, 1.0f)) * 57.2958f,
            r->hasTurn ? 57.2958f * sqrtf(r->turn[0] * r->turn[0] + r->turn[1] * r->turn[1] + r->turn[2] * r->turn[2]) : 0.0f,
            q->dec.s[0], q->dec.s[1], q->dec.s[2], r->dec.s[0], r->dec.s[1],
            r->dec.s[2], r->m[12], r->m[13], r->m[14]);
}

static void match_all(void) {
    Table *c = I.tab[I.cur];
    Table *p = I.tab[I.cur ^ 1];
    int pass;
    int i;

    if (!I.prevValid) {
        return;
    }
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < c->count; i++) {
            Rec *r = &c->rec[i];
            int pp;
            int best = -1;
            float bestD = 0.0f;
            int n = 0;
            int j;

            if (r->match >= 0 || r->parent < -1) {
                continue;
            }
            pp = r->parent >= 0 ? c->rec[r->parent].match : -1;
            if (r->parent >= 0 && pp < 0) {
                continue;
            }
            for (j = pass == 0 ? p->bucketA[hash_a(r->key, pp)] : p->bucketC[hash_a(r->lkey, pp)]; j >= 0 && n < 600;
                 n++) {
                Rec *q = &p->rec[j];

                if (!q->used && q->parent == pp && (pass == 0 ? q->key == r->key : q->lkey == r->lkey)) {
                    /* The same occurrence wins a tie, so that identical matrices pair in order. */
                    float d = dist2(q, r) + (q->ord != r->ord ? 1e-3f : 0.0f);

                    if ((best < 0 || d < bestD) && (pass == 0 || d < LOOSE_MOVE * LOOSE_MOVE)) {
                        best = j;
                        bestD = d;
                    }
                }
                j = pass == 0 ? q->nextA : q->nextC;
            }
            if (best >= 0 && (r->blendMode = (u8)acceptable(&p->rec[best], r)) == BLEND_NONE) {
                r->hasTurn = 0;
            }
            if (best >= 0 && r->blendMode != BLEND_NONE) {
                if (I.debug && r->blendMode == BLEND_MOVE) {
                    debug_pair("move", i, &p->rec[best], r);
                } else if (I.debug && gfx_getenv("GLOVER_INTERP_DEBUG")[0] == '2' && r->hasTurn
                           && r->turn[0] * r->turn[0] + r->turn[1] * r->turn[1] + r->turn[2] * r->turn[2] > 0.5f) {
                    debug_pair("full", i, &p->rec[best], r);
                }
                r->match = best;
                p->rec[best].used = 1;
                I.stats.blended++;
                if (memcmp(p->rec[best].m, r->m, sizeof(r->m)) != 0) {
                    I.stats.moved++;
                }
                if (pass == 1) {
                    I.stats.loose++;
                }
            } else if (pass == 1) {
                if (best >= 0) {
                    I.stats.cut++;
                    if (I.debug) {
                        debug_pair("cut", i, &p->rec[best], r);
                    }
                } else {
                    I.stats.noPartner++;
                    if (I.debug) {
                        gfx_log("nopartner f%u rec %d key %08x parent %d (parent match %d) pos %.1f %.1f %.1f", I.stamp, i,
                                r->key, r->parent, pp, r->m[12], r->m[13], r->m[14]);
                    }
                }
            }
        }
    }
}

/* Called for each G_MTX in a drawn frame. Blends m in place when the record has a partner. Returns the record's
   index, or -2 when there is none. */
int gfx_interp_draw_mtx(u32 key, int parent, int kind, float *m) {
    Table *c = I.tab[I.cur];
    Table *p = I.tab[I.cur ^ 1];
    int first;
    int idx;
    Rec *r;

    if (parent < -1) {
        return -2;
    }
    first = find_b(c, key, parent, 0);
    if (first < 0) {
        return -2;
    }
    if (c->rec[first].seenStamp != I.stamp) {
        c->rec[first].seenStamp = I.stamp;
        c->rec[first].groupCount = 0;
    }
    idx = find_b(c, key, parent, c->rec[first].groupCount++);
    if (idx < 0) {
        return -2;
    }
    r = &c->rec[idx];
    /* The matrix in memory must still be the one that was recorded. */
    if (r->match >= 0 && r->kind == kind && memcmp(m, r->m, sizeof(r->m)) == 0) {
        blend(m, &p->rec[r->match], r, I.t);
    }
    return idx;
}

/* ---- Texture rectangles. */

/* A view-projection matrix, its inverse and the camera position that goes with it. The matrices are row-major and
   multiply row vectors. */
typedef struct VpInfo {
    float vp[16];
    float inv[16];
    float eye[3];
    int valid;
    int known; /* the fields above belong to vp */
} VpInfo;

static int invert4(const float *m, float *out) {
    double a[4][8];
    int i;
    int j;
    int k;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            a[i][j] = m[i * 4 + j];
            a[i][4 + j] = i == j;
        }
    }
    for (i = 0; i < 4; i++) {
        int piv = i;
        double d;

        for (k = i + 1; k < 4; k++) {
            if (fabs(a[k][i]) > fabs(a[piv][i])) {
                piv = k;
            }
        }
        if (fabs(a[piv][i]) < 1e-12) {
            return 0;
        }
        if (piv != i) {
            for (j = 0; j < 8; j++) {
                double t = a[i][j];
                a[i][j] = a[piv][j];
                a[piv][j] = t;
            }
        }
        d = a[i][i];
        for (j = 0; j < 8; j++) {
            a[i][j] /= d;
        }
        for (k = 0; k < 4; k++) {
            if (k != i) {
                double f = a[k][i];
                for (j = 0; j < 8; j++) {
                    a[k][j] -= f * a[i][j];
                }
            }
        }
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            out[i * 4 + j] = (float)a[i][4 + j];
        }
    }
    return 1;
}

/* The inverse and the eye of a view-projection matrix. A camera position is the point that maps to the clip
   coordinates (0, 0, z, 0): the third row of the inverse. The last matrix asked for stays in the cache. */
static const VpInfo *vp_info(const float *vp) {
    static VpInfo c;

    if (c.known && memcmp(c.vp, vp, sizeof(c.vp)) == 0) {
        return &c;
    }
    memcpy(c.vp, vp, sizeof(c.vp));
    c.known = 1;
    c.valid = invert4(vp, c.inv) && fabsf(c.inv[11]) > 1e-12f;
    if (c.valid) {
        c.eye[0] = c.inv[8] / c.inv[11], c.eye[1] = c.inv[9] / c.inv[11], c.eye[2] = c.inv[10] / c.inv[11];
    }
    return &c;
}

/* Clip coordinates of a world point (w = 1). */
static void project(const float *vp, const float *p, float *c) {
    int k;

    for (k = 0; k < 4; k++) {
        c[k] = p[0] * vp[k] + p[1] * vp[4 + k] + p[2] * vp[8 + k] + vp[12 + k];
    }
}

/* The world point behind pixel (x, y) of the screen at clip depth z. */
static int unproject(const GfxRectView *v, float x, float y, float *world) {
    const VpInfo *info = vp_info(v->vp);
    float n[4];
    float h[4];
    int k;

    if (!info->valid || v->vpScale[0] == 0.0f || v->vpScale[1] == 0.0f) {
        return 0;
    }
    n[0] = (x - v->vpTrans[0]) / v->vpScale[0];
    n[1] = (v->vpTrans[1] - y) / v->vpScale[1];
    n[2] = v->z;
    n[3] = 1.0f;
    for (k = 0; k < 4; k++) {
        h[k] = n[0] * info->inv[k] + n[1] * info->inv[4 + k] + n[2] * info->inv[8 + k] + n[3] * info->inv[12 + k];
    }
    if (fabsf(h[3]) < 1e-9f) {
        return 0;
    }
    world[0] = h[0] / h[3], world[1] = h[1] / h[3], world[2] = h[2] / h[3];
    return 1;
}

static float dist3(const float *a, const float *b) {
    float x = a[0] - b[0];
    float y = a[1] - b[1];
    float z = a[2] - b[2];

    return sqrtf(x * x + y * y + z * z);
}

/* Called for each texture rectangle while recording. r holds the corners. */
void gfx_interp_record_rect(u32 key, u32 lkey, const float *r, const GfxRectView *view) {
    int c = I.cur;
    RRec *q;
    int i;
    int n = 0;
    u32 h = key & (NB - 1);

    if (R->count[c] >= MAX_RECTS) {
        return;
    }
    for (i = R->bucket[c][h]; i >= 0; i = R->rec[c][i].nextA) {
        n += R->rec[c][i].key == key;
    }
    i = R->count[c]++;
    q = &R->rec[c][i];
    memset(q, 0, sizeof(*q));
    q->key = key;
    q->lkey = lkey;
    q->ord = n;
    q->match = -1;
    memcpy(q->r, r, sizeof(q->r));
    if (view != NULL && unproject(view, (r[0] + r[2]) * 0.5f, (r[1] + r[3]) * 0.5f, q->world)) {
        q->is3d = 1;
        q->dist = dist3(q->world, vp_info(view->vp)->eye);
    }
    q->nextA = R->bucket[c][h];
    R->bucket[c][h] = i;
    q->nextC = R->bucketC[c][lkey & (NB - 1)];
    R->bucketC[c][lkey & (NB - 1)] = i;
}

/* A 3D sprite takes the nearest unused 3D sprite of the previous frame in the world. A sprite with the same image
   is preferred, since the game changes the image of a spinning or animated sprite in every frame. */
static void match_sprites(void) {
    int c = I.cur;
    int i;
    int j;
    static int cand[MAX_RECTS];
    int nc = 0;

    for (j = 0; j < R->count[c ^ 1]; j++) {
        if (R->rec[c ^ 1][j].is3d) {
            cand[nc++] = j;
        }
    }
    for (i = 0; i < R->count[c] && nc > 0; i++) {
        RRec *q = &R->rec[c][i];
        int best = -1;
        float bestD = 0.0f;
        int k;

        if (!q->is3d) {
            continue;
        }
        for (k = 0; k < nc; k++) {
            const RRec *p = &R->rec[c ^ 1][cand[k]];
            float d;

            if (p->used) {
                continue;
            }
            d = dist3(p->world, q->world);
            if (p->lkey != q->lkey) {
                d += MAX_SPRITE_MOVE * 0.5f;
            }
            if (best < 0 || d < bestD) {
                best = cand[k];
                bestD = d;
            }
        }
        if (best >= 0 && bestD < MAX_SPRITE_MOVE) {
            q->match = best;
            R->rec[c ^ 1][best].used = 1;
            I.stats.rectsBlended++;
        }
        I.stats.rects++;
    }
}

/* Whether rectangle q may take p as its partner: alike in size, and moved less than lim pixels. dx and dy get the
   move of the centre. */
static int rect_pair_ok(const RRec *p, const RRec *q, float lim, float *dx, float *dy) {
    float pw = p->r[2] - p->r[0], ph = p->r[3] - p->r[1];
    float qw = q->r[2] - q->r[0], qh = q->r[3] - q->r[1];

    *dx = (q->r[0] + q->r[2] - p->r[0] - p->r[2]) * 0.5f;
    *dy = (q->r[1] + q->r[3] - p->r[1] - p->r[3]) * 0.5f;
    return *dx * *dx + *dy * *dy < lim * lim && fabsf(pw - qw) < 4.0f + 0.5f * qw && fabsf(ph - qh) < 4.0f + 0.5f * qh;
}

/* The 2D rectangles (the HUD and text) are matched by their order of drawing, not by distance. Many letters of a text
   share one key, and the nearest letter with the same key is often a letter of another line or another text box:
   the letters then seem to swap places. The game draws a screen's rectangles in the same order in every frame, so
   the two frames' lists of keys are aligned (a longest common subsequence). An aligned pair is kept when it
   barely moves, or when it belongs to a run of three or more pairs that follow each other in both lists and moves
   with the run. A text keeps its letters together even when it waves or grows; one or two letters that only happen
   to align with letters of another text are left unblended. */
#define RECT_STILL 3.0f /* pixels: a pair that moves less is kept on its own */
#define RECT_RUN_MOVE 40.0f /* pixels: the most a run of letters may move as a whole */
#define RECT_RUN_SPREAD 8.0f /* pixels: how far a letter's move may be from its run's */
#define RECT_LCS_MAX 2000000 /* cells of the alignment table; longer lists are aligned by position */

/* The number of 3D sprites between rectangles i and j of frame f. They are not in the lists of 2D rectangles, so a
   run of 2D rectangles may step over them. */
static int skipped_flat(int f, int i, int j) {
    int n = 0;
    int k;

    for (k = i + 1; k < j; k++) {
        n += R->rec[f][k].is3d;
    }
    return n;
}

static void match_flat_rects(void) {
    int c = I.cur;
    static int pa[MAX_RECTS];
    static int qa[MAX_RECTS];
    static int pairP[MAX_RECTS];
    static int pairQ[MAX_RECTS];
    static float mx[MAX_RECTS];
    static float my[MAX_RECTS];
    static u8 okPair[MAX_RECTS];
    static unsigned short *table;
    static long tableSize;
    int np = 0;
    int nq = 0;
    int pairs = 0;
    int i;
    int j;
    int k;

    for (j = 0; j < R->count[c ^ 1]; j++) {
        if (!R->rec[c ^ 1][j].is3d) {
            pa[np++] = j;
        }
    }
    for (i = 0; i < R->count[c]; i++) {
        if (!R->rec[c][i].is3d) {
            qa[nq++] = i;
        }
    }
    if (np == 0 || nq == 0) {
        return;
    }
    if ((long)(np + 1) * (nq + 1) <= RECT_LCS_MAX) {
        long need = (long)(np + 1) * (nq + 1);

        if (tableSize < need) {
            free(table);
            table = malloc(need * sizeof(*table));
            tableSize = table != NULL ? need : 0;
        }
    }
    if (table != NULL && (long)(np + 1) * (nq + 1) <= tableSize) {
        /* table[a][b]: the length of the common subsequence of pa[a..] and qa[b..]. */
#define T(a, b) table[(long)(a) * (nq + 1) + (b)]
        for (i = np; i >= 0; i--) {
            for (j = nq; j >= 0; j--) {
                if (i == np || j == nq) {
                    T(i, j) = 0;
                } else if (R->rec[c ^ 1][pa[i]].key == R->rec[c][qa[j]].key) {
                    T(i, j) = (unsigned short)(T(i + 1, j + 1) + 1);
                } else {
                    T(i, j) = T(i + 1, j) >= T(i, j + 1) ? T(i + 1, j) : T(i, j + 1);
                }
            }
        }
        i = 0;
        j = 0;
        while (i < np && j < nq) {
            if (R->rec[c ^ 1][pa[i]].key == R->rec[c][qa[j]].key) {
                pairP[pairs] = pa[i++];
                pairQ[pairs++] = qa[j++];
            } else if (T(i + 1, j) >= T(i, j + 1)) {
                i++;
            } else {
                j++;
            }
        }
#undef T
    } else {
        for (k = 0; k < np && k < nq; k++) {
            if (R->rec[c ^ 1][pa[k]].key == R->rec[c][qa[k]].key) {
                pairP[pairs] = pa[k];
                pairQ[pairs++] = qa[k];
            }
        }
    }
    for (k = 0; k < pairs; k++) {
        okPair[k] = (u8)rect_pair_ok(&R->rec[c ^ 1][pairP[k]], &R->rec[c][pairQ[k]], MAX_RECT_MOVE, &mx[k], &my[k]);
    }
    /* Runs: aligned pairs that follow each other in both lists, such as the letters of one text. */
    k = 0;
    while (k < pairs) {
        int end = k + 1;
        float sx[MAX_RECTS > 64 ? 64 : MAX_RECTS];
        float sy[MAX_RECTS > 64 ? 64 : MAX_RECTS];
        float medX;
        float medY;
        int n;
        int a;
        int b;

        while (end < pairs && pairP[end] == pairP[end - 1] + 1 + skipped_flat(c ^ 1, pairP[end - 1], pairP[end])
               && pairQ[end] == pairQ[end - 1] + 1 + skipped_flat(c, pairQ[end - 1], pairQ[end])) {
            end++;
        }
        /* The median move of the run (of its first 64 pairs). */
        n = end - k > 64 ? 64 : end - k;
        for (a = 0; a < n; a++) {
            sx[a] = mx[k + a];
            sy[a] = my[k + a];
        }
        for (a = 1; a < n; a++) {
            for (b = a; b > 0 && sx[b - 1] > sx[b]; b--) {
                float t = sx[b];
                sx[b] = sx[b - 1];
                sx[b - 1] = t;
            }
            for (b = a; b > 0 && sy[b - 1] > sy[b]; b--) {
                float t = sy[b];
                sy[b] = sy[b - 1];
                sy[b - 1] = t;
            }
        }
        medX = sx[n / 2];
        medY = sy[n / 2];
        for (a = k; a < end; a++) {
            int keep;

            if (!okPair[a]) {
                continue;
            }
            /* A pair that barely moves is kept on its own. A pair of a run of three or more is kept when the run as a
               whole moves less than RECT_RUN_MOVE and the pair keeps close to the run's move (a text that waves or
               grows moves its letters a little apart). */
            keep = mx[a] * mx[a] + my[a] * my[a] < RECT_STILL * RECT_STILL;
            if (!keep && end - k >= 3 && medX * medX + medY * medY < RECT_RUN_MOVE * RECT_RUN_MOVE
                && fabsf(mx[a] - medX) < RECT_RUN_SPREAD && fabsf(my[a] - medY) < RECT_RUN_SPREAD) {
                keep = 1;
            }
            if (keep) {
                R->rec[c][pairQ[a]].match = pairP[a];
                R->rec[c ^ 1][pairP[a]].used = 1;
                I.stats.rectsBlended++;
            }
        }
        k = end;
    }
}

/* The number of 2D rectangles of frame f that show image lkey. */
static int flat_image_count(int f, u32 lkey) {
    int n = 0;
    int i;

    for (i = R->bucketC[f][lkey & (NB - 1)]; i >= 0; i = R->rec[f][i].nextC) {
        n += !R->rec[f][i].is3d && R->rec[f][i].lkey == lkey;
    }
    return n;
}

static void match_rects(void) {
    int c = I.cur;
    int i;

    match_sprites();
    match_flat_rects();
    /* An image whose part changes in every frame (a spinning star on the HUD) has a new key in every frame. It may
       take the previous frame's rectangle of the same image when the image is drawn once in each frame. */
    for (i = 0; i < R->count[c]; i++) {
        RRec *q = &R->rec[c][i];
        float dx;
        float dy;
        int j;

        if (q->is3d) {
            continue;
        }
        I.stats.rects++;
        if (q->match >= 0 || flat_image_count(c, q->lkey) != 1 || flat_image_count(c ^ 1, q->lkey) != 1) {
            continue;
        }
        for (j = R->bucketC[c ^ 1][q->lkey & (NB - 1)]; j >= 0; j = R->rec[c ^ 1][j].nextC) {
            const RRec *p = &R->rec[c ^ 1][j];

            if (!p->used && !p->is3d && p->lkey == q->lkey && rect_pair_ok(p, q, LOOSE_RECT_MOVE, &dx, &dy)) {
                q->match = j;
                R->rec[c ^ 1][j].used = 1;
                I.stats.rectsBlended++;
                break;
            }
        }
    }
}

/* Called for each texture rectangle in a drawn frame: blends r in place when the rectangle has a partner. A 3D
   sprite is instead put at its world position (blended with its partner's) as the drawn frame's camera sees it. Its
   size follows its distance to the camera. Returns 1 and the clip depth in *z when it did that. */
int gfx_interp_draw_rect(u32 key, float *r, const GfxRectView *view, float *z) {
    int c = I.cur;
    int first = -1;
    int want;
    int i;
    int hit = -1;
    u32 h = key & (NB - 1);

    for (i = R->bucket[c][h]; i >= 0; i = R->rec[c][i].nextA) {
        if (R->rec[c][i].key == key && R->rec[c][i].ord == 0) {
            first = i;
        }
    }
    if (first < 0) {
        return 0;
    }
    if (R->rec[c][first].seenStamp != I.stamp) {
        R->rec[c][first].seenStamp = I.stamp;
        R->rec[c][first].groupCount = 0;
    }
    want = R->rec[c][first].groupCount++;
    for (i = R->bucket[c][h]; i >= 0; i = R->rec[c][i].nextA) {
        if (R->rec[c][i].key == key && R->rec[c][i].ord == want) {
            hit = i;
            break;
        }
    }
    if (hit < 0) {
        return 0;
    }
    {
        const RRec *q = &R->rec[c][hit];

        if (q->is3d && view != NULL) {
            float world[3];
            float clip[4];
            float cx;
            float cy;
            float hw;
            float hh;
            float k;
            int m;

            memcpy(world, q->world, sizeof(world));
            if (q->match >= 0) {
                const float *p = R->rec[c ^ 1][q->match].world;

                for (m = 0; m < 3; m++) {
                    world[m] = p[m] + (q->world[m] - p[m]) * I.t;
                }
            }
            project(view->vp, world, clip);
            if (clip[3] <= 1e-6f) {
                return 0;
            }
            k = vp_info(view->vp)->valid ? dist3(world, vp_info(view->vp)->eye) : 0.0f;
            k = k > 1e-3f ? q->dist / k : 1.0f;
            cx = view->vpTrans[0] + clip[0] / clip[3] * view->vpScale[0];
            cy = view->vpTrans[1] - clip[1] / clip[3] * view->vpScale[1];
            hw = (q->r[2] - q->r[0]) * 0.5f * k;
            hh = (q->r[3] - q->r[1]) * 0.5f * k;
            r[0] = cx - hw, r[1] = cy - hh, r[2] = cx + hw, r[3] = cy + hh;
            *z = clip[2] / clip[3];
            return 1;
        }
        if (q->match >= 0) {
            const float *p = R->rec[c ^ 1][q->match].r;
            int k;

            for (k = 0; k < 4; k++) {
                r[k] = p[k] + (q->r[k] - p[k]) * I.t;
            }
        }
    }
    return 0;
}

/* ---- The viewport. */

void gfx_interp_viewport(float *scale, float *trans, int recording) {
    float *v;
    int k;

    if (I.vpOrd >= MAX_VIEWPORTS) {
        return;
    }
    if (recording) {
        v = I.vp[I.cur][I.vpOrd++];
        v[0] = scale[0], v[1] = scale[1], v[2] = trans[0], v[3] = trans[1];
        I.vpCount[I.cur] = I.vpOrd;
        return;
    }
    {
        const float *prev = I.vp[I.cur ^ 1][I.vpOrd];
        float cur[4];

        cur[0] = scale[0], cur[1] = scale[1], cur[2] = trans[0], cur[3] = trans[1];
        if (I.vpValid && I.vpOrd < I.vpCount[I.cur ^ 1] && I.vpOrd < I.vpCount[I.cur]) {
            int ok = 1;

            for (k = 0; k < 4; k++) {
                if (fabsf(cur[k] - prev[k]) > 60.0f) {
                    ok = 0;
                }
            }
            if (ok) {
                scale[0] = prev[0] + (cur[0] - prev[0]) * I.t;
                scale[1] = prev[1] + (cur[1] - prev[1]) * I.t;
                trans[0] = prev[2] + (cur[2] - prev[2]) * I.t;
                trans[1] = prev[3] + (cur[3] - prev[3]) * I.t;
            }
        }
        I.vpOrd++;
    }
}

/* ---- Public interface (gfx.h). */

void gfx_interp_set(float t) {
    I.t = t;
    I.drawing = t >= 0.0f && t < 1.0f;
}

/* Called by a drawn frame before it interprets the lists: 1 when matrices are to be blended. */
int gfx_interp_begin_draw(void) {
    static int trackRead;

    I.vpOrd = 0;
    I.stamp++;
    if (!trackRead) {
        trackRead = 1;
        gfx_interp_track = gfx_getenv("GLOVER_INTERP_TRACK") != NULL ? (u32)strtoul(gfx_getenv("GLOVER_INTERP_TRACK"), NULL, 16) : 0;
    }
    if (I.drawing && I.prevValid && I.curValid) {
        gfxInterpMode = 2;
        return 1;
    }
    gfxInterpMode = 0;
    return 0;
}

void gfx_interp_end_draw(void) {
    gfxInterpMode = 0;
}

void gfx_interp_set_volatile(unsigned int lo, unsigned int hi) {
    gfxVolatileLo = lo;
    gfxVolatileHi = hi;
}

void gfx_interp_stats(GfxInterpStats *s) {
    *s = I.stats;
}

void gfx_interp_shutdown(void) {
    int i;

    for (i = 0; i < 2; i++) {
        if (I.tab[i] != NULL) {
            free(I.tab[i]->rec);
            free(I.tab[i]);
            I.tab[i] = NULL;
        }
    }
    I.curValid = I.prevValid = 0;
}
