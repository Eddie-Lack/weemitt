#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>

double now_seconds(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;

    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>

double now_seconds(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

static uint32_t next_random(uint32_t *s) {
    /* xorshift32 */
    uint32_t x = *s;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

void test_input_init(TestInput *t, uint32_t seed) {
    t->seed = seed ? seed : 1;
    t->left = 0;
    t->release = 0;
    t->cur.buttons = 0;
    t->cur.x = 0;
    t->cur.y = 0;
}

void test_input_next(TestInput *t, EngineInput *out) {
    static const int8_t xs[] = {0, 0, 80, -80, 40, -40, 25, 60, -60, 70};
    static const int8_t ys[] = {0, 80, 80, -80, 40, 60, -40, 70, -70, 20};
    /* Each button and its chance in 256 of being held in a stretch. */
    static const struct {
        uint16_t bit;
        uint8_t chance;
    } buttons[] = {{BTN_A, 77}, {BTN_B, 51}, {BTN_Z, 38}, {BTN_R, 26}, {BTN_L, 13},
                   {BTN_CU, 13}, {BTN_CD, 13}, {BTN_CL, 20}, {BTN_CR, 20}};
    unsigned i;

    if (t->left == 0 && t->release == 0) {
        t->left = 8 + next_random(&t->seed) % 43;
        t->release = 2 + next_random(&t->seed) % 5;
        t->cur.x = xs[next_random(&t->seed) % 10];
        t->cur.y = ys[next_random(&t->seed) % 10];
        t->cur.buttons = 0;
        for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            if ((next_random(&t->seed) & 0xFF) < buttons[i].chance) {
                t->cur.buttons |= buttons[i].bit;
            }
        }
    }
    *out = t->cur;
    if (t->left > 0) {
        t->left--;
    } else {
        /* Release all buttons for a few frames. The buttons of the next stretch are then new presses. */
        out->buttons = 0;
        t->release--;
    }
}

uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *p;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc(n > 0 ? n : 1);
    if (p == NULL || fread(p, 1, n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    *size = n;
    return p;
}

int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        return -1;
    }
    if (fwrite(data, 1, size, f) != size) {
        fclose(f);
        return -1;
    }
    return fclose(f);
}

void die(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
