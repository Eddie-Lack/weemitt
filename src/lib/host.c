/* The platform functions that the game's code calls (docs/library.md): the log, the clock, the sound device and
   the fatal error. In weemitt they are in platform.c and log.c. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "lib.h"

int lib_audio_rate;

void plog_vprintf(const char *fmt, va_list ap) {
    char line[900];
    size_t n;
    Glover *g = lib_game;

    vsnprintf(line, sizeof(line), fmt, ap);
    n = strlen(line);
    while (n > 0 && line[n - 1] == '\n') {
        line[--n] = 0;
    }
    if (g == NULL) {
        return;
    }
    if (strncmp(line, "fatal: ", 7) == 0) {
        snprintf(g->error, sizeof(g->error), "%s", line);
    }
    if (g->log != NULL) {
        g->log(g->user, line);
    }
}

void plog_printf(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    plog_vprintf(fmt, ap);
    va_end(ap);
}

/* The game's clock feeds only its profiler timers, which are in the game's memory. */
unsigned long long port_time_us(void) {
    Glover *g = lib_game;

    if (g == NULL) {
        return 0;
    }
    if (g->clockUs != NULL) {
        return g->clockUs(g->user);
    }
    return g->clock;
}

#ifdef PORT_AUDIO_MUS
/* No sound device: the sound player runs on the game's clock (musAudioAdvance) and gives its frames to
   port_audio_sink. Only the game's thread touches the synthesizer, so the lock does nothing. */
int port_audio_open(int rate, void (*render)(short *out, int frames)) {
    (void)render;
    lib_audio_rate = rate;
    return 0;
}

void port_audio_lock(void) {
}

void port_audio_unlock(void) {
}

void port_audio_yield(void) {
}
#endif

/* port_fatal calls this after its message. It jumps back to the API call that is running (glover.c, enter). */
static void lib_fatal(void) {
    LIB_LONGJMP(lib_game->jmp);
}

void lib_host_start(void) {
    port_fatal_hook = lib_fatal;
#ifdef PORT_AUDIO_MUS
    port_audio_sink = lib_audio_push;
#endif
}
