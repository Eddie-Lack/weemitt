/* The queue of sound frames (docs/library.md): the game's thread adds the frames that the sound player makes, and
   glover_audio_read takes them, on that thread or on one other. It lies outside the game's memory. */
#include <string.h>
#include "lib.h"

#define QUEUE_FRAMES 65536

static int16_t queue[QUEUE_FRAMES * 2];
/* Frames written and read since the start. Each side writes only its own counter. */
static uint32_t written;
static uint32_t readCount;
static uint32_t dropped;

void lib_audio_push(const short *frames, int count) {
    uint32_t w = written;
    uint32_t r = __atomic_load_n(&readCount, __ATOMIC_ACQUIRE);
    int i;

    for (i = 0; i < count; i++) {
        if (w - r >= QUEUE_FRAMES) {
            dropped += count - i;
            break;
        }
        queue[(w % QUEUE_FRAMES) * 2] = frames[i * 2];
        queue[(w % QUEUE_FRAMES) * 2 + 1] = frames[i * 2 + 1];
        w++;
    }
    __atomic_store_n(&written, w, __ATOMIC_RELEASE);
}

uint32_t lib_audio_queued(void) {
    return __atomic_load_n(&written, __ATOMIC_ACQUIRE) - __atomic_load_n(&readCount, __ATOMIC_ACQUIRE);
}

uint32_t lib_audio_dropped(void) {
    return dropped;
}

int32_t lib_audio_read(int16_t *out, int32_t frames) {
    uint32_t r = readCount;
    uint32_t w = __atomic_load_n(&written, __ATOMIC_ACQUIRE);
    int32_t n = 0;

    while (n < frames && r != w) {
        out[n * 2] = queue[(r % QUEUE_FRAMES) * 2];
        out[n * 2 + 1] = queue[(r % QUEUE_FRAMES) * 2 + 1];
        r++;
        n++;
    }
    __atomic_store_n(&readCount, r, __ATOMIC_RELEASE);
    return n;
}
