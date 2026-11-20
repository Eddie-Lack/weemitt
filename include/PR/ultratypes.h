/* Written for weemitt: the parts of the N64 system interface that the port's code uses. */
#ifndef WEEMITT_PR_ULTRATYPES_H
#define WEEMITT_PR_ULTRATYPES_H

/*
 * Integer types of a fixed width. The 32-bit ones are int, never long:
 * int has 4 bytes on every host that the port builds for, and long has 8 on 64-bit Linux and macOS.
 */
typedef signed char s8;
typedef unsigned char u8;
typedef short s16;
typedef unsigned short u16;
typedef int s32;
typedef unsigned int u32;
typedef long long s64;
typedef unsigned long long u64;

/* Floating-point types. */
typedef float f32;
typedef double f64;

/* Truth values and the null pointer, unless the host's headers gave them already. */
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef NULL
#define NULL 0
#endif

#endif
