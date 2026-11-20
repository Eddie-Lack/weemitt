/* Small parts of libultra and the C library that the game relies on: sine and cosine as the N64 computes them,
   bzero and bcopy, the port's messages and the byte-order helpers. */
PORT_C_BEGIN
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "port.h"
#include "log.h"

/* libultra's sinf and cosf reduce the angle by pi in double precision and evaluate an odd polynomial. Their
   constants are in the game's read-only data, which the port loads from the ROM: the polynomial coefficients
   (P1-P4), 1/pi, pi in two parts, a half, a NaN and a zero. The order of the operations below is the order the
   N64 code uses, so the results are the same to the last bit. */
extern f64 D_8010C318, D_8010C320, D_8010C328, D_8010C330, D_8010C338, D_8010C340, D_8010C348, D_8010C358,
    D_8010C360;
extern f32 D_8010C350;
extern f64 D_8010C218, D_8010C220, D_8010C228, D_8010C230, D_8010C238, D_8010C240, D_8010C248, D_8010C258,
    D_8010C260;
extern f32 D_8010C250;
extern f32 D_8010C270;

typedef union {
    f32 f;
    u32 i;
} FloatBits;

float port_sinf(float x) {
    FloatBits b;
    s32 xpt;
    f64 dx;
    f64 xsq;
    f64 poly;
    f64 dn;
    s32 n;
    f64 result;

    b.f = x;
    xpt = (b.i >> 22) & 0x1FF;
    if (xpt < 0xFF) {
        if (xpt < 0xE6) {
            return x;
        }
        dx = x;
        xsq = dx * dx;
        poly = ((D_8010C330 * xsq + D_8010C328) * xsq + D_8010C320) * xsq + D_8010C318;
        return (f32)(dx + (dx * xsq) * poly);
    }
    if (xpt < 0x136) {
        dx = x;
        dn = dx * D_8010C338;
        if (dn >= 0.0) {
            n = (s32)(dn + D_8010C358);
        } else {
            n = (s32)(dn - D_8010C360);
        }
        dn = n;
        dx = dx - dn * D_8010C340;
        dx = dx - dn * D_8010C348;
        xsq = dx * dx;
        poly = ((D_8010C330 * xsq + D_8010C328) * xsq + D_8010C320) * xsq + D_8010C318;
        result = dx + (dx * xsq) * poly;
        if (n & 1) {
            return -(f32)result;
        }
        return (f32)result;
    }
    if (x != x) {
        return D_8010C350;
    }
    return D_8010C270;
}

float port_cosf(float x) {
    FloatBits b;
    f32 absx;
    f64 dx;
    f64 xsq;
    f64 poly;
    f64 dn;
    s32 n;
    f64 result;

    b.f = x;
    if (((b.i >> 22) & 0x1FF) < 0x136) {
        absx = x > 0.0f ? x : -x;
        dx = absx;
        dn = dx * D_8010C238 + D_8010C258;
        if (dn >= 0.0) {
            n = (s32)(dn + D_8010C258);
        } else {
            n = (s32)(dn - D_8010C258);
        }
        dn = (f64)n - D_8010C260;
        dx = dx - dn * D_8010C240;
        dx = dx - dn * D_8010C248;
        xsq = dx * dx;
        poly = ((D_8010C230 * xsq + D_8010C228) * xsq + D_8010C220) * xsq + D_8010C218;
        result = dx + (dx * xsq) * poly;
        if (n & 1) {
            return -(f32)result;
        }
        return (f32)result;
    }
    if (x != x) {
        return D_8010C250;
    }
    return D_8010C270;
}

void bzero(void *p, int n) {
    memset(p, 0, n);
}

void bcopy(const void *src, void *dst, int n) {
    memmove(dst, src, n);
}

int bcmp(const void *a, const void *b, int n) {
    return memcmp(a, b, n);
}

void port_log(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    plog_vprintf(fmt, ap);
    va_end(ap);
}

/* Set by a host that must not end the process (the library, src/lib/host.c). It does not return. */
void (*port_fatal_hook)(void);

void port_fatal(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    {
        char msg[900];

        vsnprintf(msg, sizeof(msg), fmt, ap);
        plog_printf("fatal: %s", msg);
    }
    va_end(ap);
    if (port_fatal_hook != NULL) {
        port_fatal_hook();
    }
    exit(1);
}

void port_swap16_array(void *p, u32 count) {
    u16 *h = (u16 *)p;
    u32 i;

    for (i = 0; i < count; i++) {
        h[i] = port_bswap16(h[i]);
    }
}

void port_swap32_array(void *p, u32 count) {
    u32 *w = (u32 *)p;
    u32 i;

    for (i = 0; i < count; i++) {
        w[i] = port_bswap32(w[i]);
    }
}

PORT_C_END
