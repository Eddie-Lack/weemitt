/* Matrix helpers with the behaviour of libultra's gu functions, written for the port.
   N64 matrices are row-major and multiply row vectors (v' = v * M). An Mtx holds each element as s15.16 fixed
   point: words 0-7 are the integer halves and words 8-15 the fraction halves, two elements per word. */
PORT_C_BEGIN
#include "port.h"

#define DEG2RAD (3.14159265358979323846 / 180.0)

static void identity(float mf[4][4]) {
    s32 i;
    s32 j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            mf[i][j] = i == j ? 1.0f : 0.0f;
        }
    }
}

void guMtxIdentF(float mf[4][4]) {
    identity(mf);
}

void guMtxF2L(float mf[4][4], Mtx *m) {
    u32 *w = (u32 *)m;
    s32 k;
    s32 a;
    s32 b;

    for (k = 0; k < 8; k++) {
        a = (s32)(mf[(2 * k) / 4][(2 * k) % 4] * 65536.0f);
        b = (s32)(mf[(2 * k + 1) / 4][(2 * k + 1) % 4] * 65536.0f);
        w[k] = ((u32)a & 0xFFFF0000) | (((u32)b >> 16) & 0xFFFF);
        w[k + 8] = ((u32)a << 16) | ((u32)b & 0xFFFF);
    }
}

void guMtxL2F(float mf[4][4], Mtx *m) {
    u32 *w = (u32 *)m;
    s32 k;
    s32 a;
    s32 b;

    for (k = 0; k < 8; k++) {
        a = (s32)((w[k] & 0xFFFF0000) | (w[k + 8] >> 16));
        b = (s32)((w[k] << 16) | (w[k + 8] & 0xFFFF));
        mf[(2 * k) / 4][(2 * k) % 4] = a / 65536.0f;
        mf[(2 * k + 1) / 4][(2 * k + 1) % 4] = b / 65536.0f;
    }
}

void guMtxIdent(Mtx *m) {
    float mf[4][4];

    identity(mf);
    guMtxF2L(mf, m);
}

void guMtxCatF(float a[4][4], float b[4][4], float r[4][4]) {
    float t[4][4];
    s32 i;
    s32 j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            /* libultra starts each sum from 0: the result is +0 when every product is -0. */
            t[i][j] = 0.0f + a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(r, t, sizeof(t));
}

void guMtxCatL(Mtx *a, Mtx *b, Mtx *r) {
    float fa[4][4];
    float fb[4][4];

    guMtxL2F(fa, a);
    guMtxL2F(fb, b);
    guMtxCatF(fa, fb, fa);
    guMtxF2L(fa, r);
}

void guMtxXFMF(float mf[4][4], float x, float y, float z, float *ox, float *oy, float *oz) {
    *ox = x * mf[0][0] + y * mf[1][0] + z * mf[2][0] + mf[3][0];
    *oy = x * mf[0][1] + y * mf[1][1] + z * mf[2][1] + mf[3][1];
    *oz = x * mf[0][2] + y * mf[1][2] + z * mf[2][2] + mf[3][2];
}

void guTranslateF(float mf[4][4], float x, float y, float z) {
    identity(mf);
    mf[3][0] = x;
    mf[3][1] = y;
    mf[3][2] = z;
}

void guTranslate(Mtx *m, float x, float y, float z) {
    float mf[4][4];

    guTranslateF(mf, x, y, z);
    guMtxF2L(mf, m);
}

void guScaleF(float mf[4][4], float x, float y, float z) {
    identity(mf);
    mf[0][0] = x;
    mf[1][1] = y;
    mf[2][2] = z;
}

void guScale(Mtx *m, float x, float y, float z) {
    float mf[4][4];

    guScaleF(mf, x, y, z);
    guMtxF2L(mf, m);
}

/* Scales (x, y, z) to length 1, as libultra's guNormalize does: one reciprocal, three products. */
void guNormalize(float *x, float *y, float *z) {
    float m = 1.0f / sqrtf(*x * *x + *y * *y + *z * *z);

    *x = *x * m;
    *y = *y * m;
    *z = *z * m;
}

/* A rotation of a degrees about the axis (x, y, z), for row vectors. This follows libultra's guRotateF operation by
   operation, with libultra's sine and cosine. The game's logic uses it (HandSwish builds the glove's rotation with
   it), so it must give the N64's bits. */
void guRotateF(float mf[4][4], float a, float x, float y, float z) {
    float sine;
    float cosine;
    float ab;
    float bc;
    float ca;
    float t;
    float xx;
    float yy;
    float zz;

    guNormalize(&x, &y, &z);
    a = a * 0.017453292f;
    sine = port_sinf(a);
    cosine = port_cosf(a);
    ab = x * y;
    bc = y * z;
    ca = z * x;
    t = 1.0f - cosine;
    ab = ab * t;
    bc = bc * t;
    ca = ca * t;
    guMtxIdentF(mf);
    xx = x * x;
    mf[0][0] = xx + cosine * (1.0f - xx);
    yy = y * y;
    mf[2][0] = ca + y * sine;
    mf[2][1] = bc - x * sine;
    zz = z * z;
    mf[1][2] = bc + x * sine;
    mf[0][2] = ca - y * sine;
    mf[1][1] = yy + cosine * (1.0f - yy);
    mf[1][0] = ab - z * sine;
    mf[0][1] = ab + z * sine;
    mf[2][2] = zz + cosine * (1.0f - zz);
}

void guRotate(Mtx *m, float a, float x, float y, float z) {
    float mf[4][4];

    guRotateF(mf, a, x, y, z);
    guMtxF2L(mf, m);
}

void guPerspectiveF(float mf[4][4], u16 *perspNorm, float fovy, float aspect, float near, float far, float scale) {
    float f = (float)(1.0 / tan(fovy * DEG2RAD / 2.0));
    s32 i;
    s32 j;

    identity(mf);
    mf[0][0] = f / aspect;
    mf[1][1] = f;
    mf[2][2] = (near + far) / (near - far);
    mf[2][3] = -1.0f;
    mf[3][2] = 2.0f * near * far / (near - far);
    mf[3][3] = 0.0f;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            mf[i][j] *= scale;
        }
    }
    if (perspNorm != NULL) {
        if (near + far <= 2.0f) {
            *perspNorm = 65535;
        } else {
            float n = 131072.0f / (near + far);
            *perspNorm = n < 1.0f ? 1 : n > 65535.0f ? 65535 : (u16)n;
        }
    }
}

void guPerspective(Mtx *m, u16 *perspNorm, float fovy, float aspect, float near, float far, float scale) {
    float mf[4][4];

    guPerspectiveF(mf, perspNorm, fovy, aspect, near, far, scale);
    guMtxF2L(mf, m);
}

void guOrthoF(float mf[4][4], float l, float r, float b, float t, float n, float f, float scale) {
    s32 i;
    s32 j;

    identity(mf);
    mf[0][0] = 2.0f / (r - l);
    mf[1][1] = 2.0f / (t - b);
    mf[2][2] = -2.0f / (f - n);
    mf[3][0] = -(r + l) / (r - l);
    mf[3][1] = -(t + b) / (t - b);
    mf[3][2] = -(f + n) / (f - n);
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            mf[i][j] *= scale;
        }
    }
}

void guOrtho(Mtx *m, float l, float r, float b, float t, float n, float f, float scale) {
    float mf[4][4];

    guOrthoF(mf, l, r, b, t, n, f, scale);
    guMtxF2L(mf, m);
}

/* The camera matrix: x is the camera's right, y its up and z points from the target back to the eye. */
void guLookAtF(float mf[4][4], float xEye, float yEye, float zEye, float xAt, float yAt, float zAt, float xUp,
               float yUp, float zUp) {
    float bx = xEye - xAt;
    float by = yEye - yAt;
    float bz = zEye - zAt;
    float rx;
    float ry;
    float rz;
    float ux;
    float uy;
    float uz;
    float len;

    len = sqrtf(bx * bx + by * by + bz * bz);
    if (len > 0.0f) {
        bx /= len;
        by /= len;
        bz /= len;
    }
    rx = yUp * bz - zUp * by;
    ry = zUp * bx - xUp * bz;
    rz = xUp * by - yUp * bx;
    len = sqrtf(rx * rx + ry * ry + rz * rz);
    if (len > 0.0f) {
        rx /= len;
        ry /= len;
        rz /= len;
    }
    ux = by * rz - bz * ry;
    uy = bz * rx - bx * rz;
    uz = bx * ry - by * rx;
    identity(mf);
    mf[0][0] = rx;
    mf[1][0] = ry;
    mf[2][0] = rz;
    mf[3][0] = -(xEye * rx + yEye * ry + zEye * rz);
    mf[0][1] = ux;
    mf[1][1] = uy;
    mf[2][1] = uz;
    mf[3][1] = -(xEye * ux + yEye * uy + zEye * uz);
    mf[0][2] = bx;
    mf[1][2] = by;
    mf[2][2] = bz;
    mf[3][2] = -(xEye * bx + yEye * by + zEye * bz);
}

void guLookAt(Mtx *m, float xEye, float yEye, float zEye, float xAt, float yAt, float zAt, float xUp, float yUp,
              float zUp) {
    float mf[4][4];

    guLookAtF(mf, xEye, yEye, zEye, xAt, yAt, zAt, xUp, yUp, zUp);
    guMtxF2L(mf, m);
}

/* The renderer does not use specular highlights; only the camera matrix matters here. */
void guLookAtHilite(Mtx *m, LookAt *l, Hilite *h, float xEye, float yEye, float zEye, float xAt, float yAt,
                    float zAt, float xUp, float yUp, float zUp, float xl1, float yl1, float zl1, float xl2, float yl2,
                    float zl2, int twidth, int theight) {
    guLookAt(m, xEye, yEye, zEye, xAt, yAt, zAt, xUp, yUp, zUp);
    memset(l, 0, sizeof(LookAt));
    memset(h, 0, sizeof(Hilite));
}

/* Fills a Sprite2D microcode sprite description. */
void guSprite2DInit(uSprite *sp, void *source, void *tlut, int stride, int width, int height, int fmt, int siz,
                    int sx, int sy) {
    sp->s.SourceImagePointer = source;
    sp->s.TlutPointer = tlut;
    sp->s.Stride = stride;
    sp->s.SubImageWidth = width;
    sp->s.SubImageHeight = height;
    sp->s.SourceImageType = fmt;
    sp->s.SourceImageBitSize = siz;
    sp->s.SourceImageOffsetS = sx;
    sp->s.SourceImageOffsetT = sy;
}

PORT_C_END
