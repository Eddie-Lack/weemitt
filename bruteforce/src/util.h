/* Small helpers that the programs share: time, generated test input, files. */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "engine.h"

/* Seconds from a fixed point, with sub-microsecond resolution. */
double now_seconds(void);

/* Deterministic play-like input for tests and the benchmark: walking and running in random directions, the
   ball and camera buttons, presses released before the next one. No Start (the pause menu). The same seed gives
   the same input. */
typedef struct TestInput {
    uint32_t seed;
    int left;           /* frames left of the current stretch */
    int release;        /* frames of no buttons after it */
    EngineInput cur;
} TestInput;
void test_input_init(TestInput *t, uint32_t seed);
void test_input_next(TestInput *t, EngineInput *out);

/* Reads a whole file. Returns NULL when it cannot. */
uint8_t *read_file(const char *path, size_t *size);
int write_file(const char *path, const void *data, size_t size);

/* Prints a message and exits with status 1. */
void die(const char *fmt, ...);

#endif
