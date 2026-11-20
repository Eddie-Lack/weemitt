/* Written for weemitt: the parts of the N64 system interface that the port's code uses. */
#ifndef WEEMITT_PR_GU_H
#define WEEMITT_PR_GU_H

#include <PR/ultratypes.h>
#include <PR/mbi.h>

/* Matrix helpers. A name that ends in F fills a float matrix. The others fill the fixed-point Mtx. */
void guMtxIdent(Mtx *m);
void guMtxIdentF(float mf[4][4]);
void guMtxF2L(float mf[4][4], Mtx *m);
void guMtxL2F(float mf[4][4], Mtx *m);
void guMtxCatL(Mtx *a, Mtx *b, Mtx *out);
void guMtxCatF(float a[4][4], float b[4][4], float out[4][4]);
void guMtxXFMF(float mf[4][4], float x, float y, float z, float *outX, float *outY, float *outZ);

void guTranslate(Mtx *m, float x, float y, float z);
void guTranslateF(float mf[4][4], float x, float y, float z);
void guScale(Mtx *m, float x, float y, float z);
void guScaleF(float mf[4][4], float x, float y, float z);
void guRotate(Mtx *m, float angle, float x, float y, float z);
void guRotateF(float mf[4][4], float angle, float x, float y, float z);

/* Projections. */
void guOrtho(Mtx *m, float left, float right, float bottom, float top, float zNear, float zFar, float scale);
void guOrthoF(float mf[4][4], float left, float right, float bottom, float top, float zNear, float zFar,
              float scale);
void guPerspective(Mtx *m, u16 *perspNorm, float fovy, float aspect, float zNear, float zFar, float scale);
void guPerspectiveF(float mf[4][4], u16 *perspNorm, float fovy, float aspect, float zNear, float zFar,
                    float scale);

/* Camera matrices: the eye position, the point it looks at, and the up direction. */
void guLookAt(Mtx *m, float xEye, float yEye, float zEye, float xAt, float yAt, float zAt, float xUp, float yUp,
              float zUp);
void guLookAtF(float mf[4][4], float xEye, float yEye, float zEye, float xAt, float yAt, float zAt, float xUp,
               float yUp, float zUp);
/* The same, and it also fills the data for two highlight lights (directions xl1.. and xl2..). */
void guLookAtHilite(Mtx *m, LookAt *l, Hilite *h, float xEye, float yEye, float zEye, float xAt, float yAt,
                    float zAt, float xUp, float yUp, float zUp, float xl1, float yl1, float zl1, float xl2,
                    float yl2, float zl2, int texWidth, int texHeight);

void guNormalize(float *x, float *y, float *z);
float sqrtf(float value);

/* Fills the structure that the Sprite2D microcode draws from. */
void guSprite2DInit(uSprite *sp, void *image, void *tlut, int stride, int width, int height, int fmt, int siz,
                    int offsetS, int offsetT);

#endif
