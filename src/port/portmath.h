/* log1p, expm1, pow and atan2 that give the same bits on every build (portmath.c, docs/port64.md "Libm"). */
#ifndef PORTMATH_H
#define PORTMATH_H

#include "ultra64.h"

f64 port_log1p(f64 x);
f64 port_expm1(f64 x);
/* x^y for x >= 0. */
f64 port_pow(f64 x, f64 y);
/* atan2 from additions, multiplications and divisions: the same bits on every build. */
f64 port_atan2(f64 y, f64 x);

#endif
