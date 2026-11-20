/* The renderer's host functions (gfx_host.h). */
#include <stdarg.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#include "gfx_host.h"
#include "log.h"

static void *(*loader)(const char *name);

void gfx_log(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    plog_vprintf(fmt, ap);
    va_end(ap);
}

const char *gfx_getenv(const char *name) {
#ifdef PORT_LIB
    (void)name;
    return NULL;
#else
    return getenv(name);
#endif
}

uint64_t gfx_ticks(void) {
#ifdef _WIN32
    LARGE_INTEGER t;

    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
#else
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

uint64_t gfx_ticks_per_second(void) {
#ifdef _WIN32
    LARGE_INTEGER f;

    QueryPerformanceFrequency(&f);
    return (uint64_t)f.QuadPart;
#else
    return 1000000000u;
#endif
}

void gfx_set_loader(void *(*load)(const char *name)) {
    loader = load;
}

void *gfx_gl_proc(const char *name) {
    return loader != NULL ? loader(name) : NULL;
}
