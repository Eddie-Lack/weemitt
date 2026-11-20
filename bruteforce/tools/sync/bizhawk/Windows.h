/* The few Win32 calls that BizHawk's mupen64plus core makes, for a Linux build (tools/sync/bizhawk/setup.sh).

   BizHawk steps the core one video frame at a time with two semaphores: the core waits at each vertical interrupt
   until the front end releases it. These are pthread versions with the Win32 meaning. __try and __except become
   plain code: the core's main_run is wrapped in one to swallow crashes. */
#ifndef GLOVER_BIZHAWK_WINDOWS_H
#define GLOVER_BIZHAWK_WINDOWS_H

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define INFINITE 0xFFFFFFFFu
#define EXCEPTION_EXECUTE_HANDLER 1
#define __try if (1)
#define __except(filter) else if (0)

typedef struct ShimSemaphore {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    long count;
    long max;
} ShimSemaphore;
typedef ShimSemaphore *HANDLE;

static inline HANDLE CreateSemaphore(void *attributes, long initial, long maximum, const char *name) {
    ShimSemaphore *s = (ShimSemaphore *)calloc(1, sizeof(*s));

    (void)attributes;
    (void)name;
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->count = initial;
    s->max = maximum;
    return s;
}

/* Win32 fails at once (WAIT_FAILED) for an invalid handle. The timeout is always INFINITE in the core. */
static inline unsigned WaitForSingleObject(HANDLE h, unsigned ms) {
    (void)ms;
    if (h == NULL) {
        return 0xFFFFFFFFu;
    }
    pthread_mutex_lock(&h->mutex);
    while (h->count == 0) {
        pthread_cond_wait(&h->cond, &h->mutex);
    }
    h->count--;
    pthread_mutex_unlock(&h->mutex);
    return 0;
}

/* Win32 fails, and leaves the count, when the count would pass the maximum. */
static inline int ReleaseSemaphore(HANDLE h, long n, long *previous) {
    int ok = 1;

    if (h == NULL) {
        return 0;
    }
    pthread_mutex_lock(&h->mutex);
    if (previous != NULL) {
        *previous = h->count;
    }
    if (h->count + n > h->max) {
        ok = 0;
    } else {
        h->count += n;
        pthread_cond_broadcast(&h->cond);
    }
    pthread_mutex_unlock(&h->mutex);
    return ok;
}

static inline int MessageBox(void *window, const char *text, const char *caption, unsigned flags) {
    (void)window;
    (void)flags;
    fprintf(stderr, "core message: %s: %s\n", caption, text);
    return 0;
}

#endif
