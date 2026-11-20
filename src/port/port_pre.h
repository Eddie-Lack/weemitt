/* Included before every source file that uses the game's headers (gcc -include). It puts the host's C library in
   place of the N64's and renames the game's functions whose names the host's C library also uses. */
#ifndef PORT_PRE_H
#define PORT_PRE_H

#ifndef PORT_TYPES_ONLY
#ifdef __cplusplus
/* The game's code is C, compiled as C++. libstdc++'s <math.h> and <stdlib.h> add the C++ overloads to the global
   namespace: sqrt(float) and fmod(float, float) would compute in float, and abs(float) would not truncate to int.
   This macro makes them include the C library's own headers, which declare only the C functions. */
#define _GLIBCXX_INCLUDE_NEXT_C_HEADERS
/* MinGW uses its own printf functions in C++11 and the C runtime's (msvcrt) in C89. The game keeps the C runtime's. */
#define __USE_MINGW_ANSI_STDIO 0
#endif
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
/* libultra's controller structures have a field named errno, which the C library may define as a macro. */
#undef errno

#ifdef __cplusplus
/* Each check fails when a C++ overload of the function is in the global namespace. */
extern "C++" {
template <typename A, typename B> struct PortSameType { enum { value = 0 }; };
template <typename A> struct PortSameType<A, A> { enum { value = 1 }; };
}
static_assert(PortSameType<decltype(sqrt(1.0f)), double>::value, "sqrt(float) must be the C function");
static_assert(PortSameType<decltype(fmod(1.0f, 1.0f)), double>::value, "fmod(float, float) must be the C function");
static_assert(PortSameType<decltype(pow(1.0f, 1.0f)), double>::value, "pow(float, float) must be the C function");
static_assert(PortSameType<decltype(fabs(1.0f)), double>::value, "fabs(float) must be the C function");
static_assert(PortSameType<decltype(abs(1.0f)), int>::value, "abs(float) must be the C function");
static_assert(PortSameType<decltype(abs(1L)), int>::value, "abs(long) must be the C function");
#endif
#else
#include <stdint.h>
#endif

/* The game's sources are C. Compiled as C++, each one is wrapped in these, and its functions and variables keep their
   C names. */
#ifdef __cplusplus
#define PORT_C_BEGIN extern "C" {
#define PORT_C_END }
#else
#define PORT_C_BEGIN
#define PORT_C_END
#endif

PORT_C_BEGIN

/* The game's libc.h declares the N64's C library. The host's takes its place. */
#define LIBC_H
#ifdef PORT_TYPES_ONLY
void *memcpy(void *dst, const void *src, unsigned int n);
unsigned int strlen(const char *s);
float sqrtf(float);
int abs(int);
#endif

/* Game functions with the names of C library functions, renamed so that neither replaces the other. maths.c
   defines float versions of atan, atan2 and acos; text.c its own strncmp. */
#define atan game_atan
#define atan2 game_atan2
#define acos game_acos
#define strncmp game_strncmp

/* libultra's sine and cosine are polynomial approximations. The port reproduces them (src/port/libultra.c). */
float port_sinf(float x);
float port_cosf(float x);
#define sinf port_sinf
#define cosf port_cosf
#define __sinf port_sinf
#define __cosf port_cosf

/* libultra's bzero, bcopy and bcmp take an int count (os.h). */
#define bzero port_bzero
#define bcopy port_bcopy
#define bcmp port_bcmp

/* bullet.c calls BulletAcquireTarget with two arguments before its one-parameter definition. The extra argument
   is ignored on the N64. */
struct Bullet;
void BulletAcquireTarget(struct Bullet *bullet);
#define BulletAcquireTarget(bullet, ...) BulletAcquireTarget(bullet)

/* frontend2.c calls TimeTrialScr with two arguments; the function takes none. */
#define TimeTrialScr(...) TimeTrialScr()

#ifndef PORT_TYPES_ONLY
#include "port_romsyms.h"
#endif
#include "port_fixups.h"
#include "ptr32.h"

PORT_C_END

#endif
