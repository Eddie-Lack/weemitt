/* log1p, expm1 and pow for the frame-rate helpers (delta.c, camera.c, credits.c), written for the port. The C
   library's versions give other last bits on each system: i386 glibc computes them on the x87 unit, x86-64 glibc
   and MinGW each in their own way. These use only double additions, multiplications and divisions, which SSE
   rounds the same way on every build, so every build gets the same bits (docs/port64.md, "Libm").

   The game calls them only when an update is shorter than an original frame. At deltaTime 1 nothing here runs.
   They are accurate to within 10 units in the last place of a double, far below what the game's float state can
   show. */
PORT_C_BEGIN
#include "port.h"
#include "portmath.h"

/* ln 2 in two parts. The high part has 32 zero bits at the end, so k * LN2_HI is exact for |k| < 2^20. */
#define LN2_HI 6.93147180369123816490e-01
#define LN2_LO 1.90821492927058770002e-10
#define INV_LN2 1.44269504088896338700e+00
#define SQRT2 1.41421356237309514547e+00

typedef union {
    f64 d;
    u64 u;
} PortF64Bits;

/* 2^k for -1022 <= k <= 1023. */
static f64 pow2(s32 k) {
    PortF64Bits b;

    b.u = (u64)(k + 1023) << 52;
    return b.d;
}

/* x * 2^k. A product that is subnormal is rounded once; outside the range it goes to 0 or infinity. */
static f64 scale(f64 x, s32 k) {
    if (k > 1023) {
        x *= pow2(1023);
        k -= 1023;
        if (k > 1023) {
            k = 1023;
        }
    } else if (k < -1022) {
        x *= pow2(-1022 + 53);
        k += 1022 - 53;
        if (k < -1022) {
            k = -1022;
        }
    }
    return x * pow2(k);
}

/* log(1 + f) for sqrt(1/2) - 1 <= f < sqrt(2) - 1. With s = f / (2 + f), log(1 + f) = 2 atanh(s) =
   2s + 2s^3/3 + 2s^5/5 + ..., and 2s = f - s * f. The sum is written as f - (f^2/2 - s * (f^2/2 + R)), where R is
   the series from 2s^3/3 on divided by s. |s| <= 0.1716, so the terms to s^24 are enough. */
static f64 log1p_reduced(f64 f) {
    f64 s = f / (2.0 + f);
    f64 z = s * s;
    f64 hfsq = 0.5 * f * f;
    f64 r = z * (2.0 / 3 + z * (2.0 / 5 + z * (2.0 / 7 + z * (2.0 / 9 + z * (2.0 / 11 + z * (2.0 / 13 +
            z * (2.0 / 15 + z * (2.0 / 17 + z * (2.0 / 19 + z * (2.0 / 21 + z * (2.0 / 23 + z * (2.0 / 25))))))))))));

    return f - (hfsq - s * (hfsq + r));
}

/* Splits u > 0 into 2^k * m with sqrt(1/2) <= m < sqrt(2). */
static f64 split(f64 u, s32 *k) {
    PortF64Bits b;
    s32 e;

    b.d = u;
    if (b.u >> 52 == 0) {
        /* Subnormal: make it normal first. */
        b.d = u * pow2(54);
        e = (s32)(b.u >> 52) - 1023 - 54;
    } else {
        e = (s32)(b.u >> 52) - 1023;
    }
    b.u = (b.u & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;
    if (b.d >= SQRT2) {
        b.d *= 0.5;
        e++;
    }
    *k = e;
    return b.d;
}

f64 port_log1p(f64 x) {
    f64 u;
    f64 m;
    f64 c;
    s32 k;

    if (x != x || x == 0.0) {
        return x;
    }
    if (x <= -1.0) {
        return x == -1.0 ? -__builtin_inf() : __builtin_nan("");
    }
    if (x > -0.29 && x < 0.41) {
        return log1p_reduced(x);
    }
    u = 1.0 + x;
    if (u > 1.0e300) {
        c = 0.0;
    } else {
        /* The error of the sum 1 + x, which the logarithm of u leaves out. */
        c = (u > 2.0 ? 1.0 - (u - x) : x - (u - 1.0)) / u;
    }
    m = split(u, &k);
    return (k * LN2_HI + (log1p_reduced(m - 1.0) + (k * LN2_LO + c)));
}

/* e^r - 1 for |r| <= ln(2) / 2 + a little: the Taylor series to r^18 / 18!. */
static f64 expm1_reduced(f64 r) {
    f64 p = 1.0 / 6402373705728000.0;

    p = 1.0 / 355687428096000.0 + r * p;
    p = 1.0 / 20922789888000.0 + r * p;
    p = 1.0 / 1307674368000.0 + r * p;
    p = 1.0 / 87178291200.0 + r * p;
    p = 1.0 / 6227020800.0 + r * p;
    p = 1.0 / 479001600.0 + r * p;
    p = 1.0 / 39916800.0 + r * p;
    p = 1.0 / 3628800.0 + r * p;
    p = 1.0 / 362880.0 + r * p;
    p = 1.0 / 40320.0 + r * p;
    p = 1.0 / 5040.0 + r * p;
    p = 1.0 / 720.0 + r * p;
    p = 1.0 / 120.0 + r * p;
    p = 1.0 / 24.0 + r * p;
    p = 1.0 / 6.0 + r * p;
    p = 0.5 + r * p;
    return r + r * r * p;
}

/* x = k ln 2 + r, with r as hi + lo. */
static s32 reduce(f64 x, f64 *r) {
    f64 t = x * INV_LN2;
    s32 k = (s32)(t < 0.0 ? t - 0.5 : t + 0.5);

    *r = (x - k * LN2_HI) - k * LN2_LO;
    return k;
}

f64 port_expm1(f64 x) {
    f64 r;
    f64 e;
    s32 k;

    if (x != x) {
        return x;
    }
    if (x > 709.8) {
        return __builtin_inf();
    }
    if (x < -40.0) {
        return -1.0;
    }
    if (x > -0.35 && x < 0.35) {
        return expm1_reduced(x);
    }
    k = reduce(x, &r);
    e = expm1_reduced(r);
    if (k >= -1 && k <= 53) {
        /* 2^k (1 + e) - 1 = 2^k (e + (1 - 2^-k)), and 1 - 2^-k is exact. */
        return scale(e + (1.0 - pow2(-k)), k);
    }
    return scale(e + 1.0, k) - 1.0;
}

/* a * b = hi + lo exactly (Dekker's product, with Veltkamp's split of each factor in two halves). */
static f64 exact_mul(f64 a, f64 b, f64 *lo) {
    f64 c = 134217729.0 * a;
    f64 ah = c - (c - a);
    f64 al = a - ah;
    f64 bh;
    f64 bl;
    f64 hi = a * b;

    c = 134217729.0 * b;
    bh = c - (c - b);
    bl = b - bh;
    *lo = ((ah * bh - hi) + ah * bl + al * bh) + al * bl;
    return hi;
}

f64 port_pow(f64 x, f64 y) {
    f64 m;
    f64 hi;
    f64 lo;
    f64 l;
    f64 le;
    f64 p;
    f64 pe;
    f64 r;
    s32 k;

    if (y == 0.0 || x == 1.0) {
        return 1.0;
    }
    if (x != x || y != y) {
        return x + y;
    }
    if (x == 0.0) {
        return y > 0.0 ? 0.0 : __builtin_inf();
    }
    if (x < 0.0) {
        /* The game raises only positive numbers. */
        return __builtin_nan("");
    }
    if (x == __builtin_inf()) {
        return y > 0.0 ? x : 0.0;
    }
    /* log x = k ln 2 + log m, as the sum l + le. */
    m = split(x, &k);
    hi = k * LN2_HI;
    lo = log1p_reduced(m - 1.0) + k * LN2_LO;
    l = hi + lo;
    le = lo - (l - hi);
    /* y log x = p + pe. */
    p = exact_mul(y, l, &pe);
    pe += y * le;
    if (p > 709.8) {
        return __builtin_inf();
    }
    if (p < -745.2) {
        return 0.0;
    }
    k = reduce(p, &r);
    return scale(1.0 + expm1_reduced(r + pe), k);
}
/* atan(t) for 0 <= t <= 1. Three steps of atan t = c + atan((t - tan c) / (1 + t tan c)), with c = pi / 4, pi / 8
   and pi / 16 or their negatives, bring |t| to about 0.1 or less. The series then needs few terms. */
static f64 atan_unit(f64 t) {
    static const f64 tans[3] = { 1.0, 4.14213562373095034470e-01, 1.98912367379658006911e-01 };
    static const f64 angles[3] = { 7.85398163397448278999e-01, 3.92699081698724139500e-01,
                                   1.96349540849362069750e-01 };
    f64 base = 0.0;
    f64 t2;
    f64 term;
    f64 sum;
    s32 i;
    s32 n;

    for (i = 0; i < 3; i++) {
        if (t > tans[i] / 2.0) {
            base += angles[i];
            t = (t - tans[i]) / (1.0 + t * tans[i]);
        } else if (t < -tans[i] / 2.0) {
            base -= angles[i];
            t = (t + tans[i]) / (1.0 - t * tans[i]);
        }
    }
    /* Now |t| <= 0.1. */
    t2 = t * t;
    term = t;
    sum = t;
    for (n = 1; n < 12; n++) {
        term = -term * t2;
        sum += term / (f64)(2 * n + 1);
    }
    return base + sum;
}

f64 port_atan2(f64 y, f64 x) {
    f64 ay = y < 0.0 ? -y : y;
    f64 ax = x < 0.0 ? -x : x;
    f64 a;

    if (y != y || x != x) {
        return x + y;
    }
    if (ax == 0.0 && ay == 0.0) {
        return 0.0;
    }
    /* The angle from the nearer axis. */
    if (ay <= ax) {
        a = atan_unit(ay / ax);
    } else {
        a = 1.57079632679489655800e+00 - atan_unit(ax / ay);
    }
    if (x < 0.0) {
        a = 3.14159265358979311600e+00 - a;
    }
    return y < 0.0 ? -a : a;
}
PORT_C_END
