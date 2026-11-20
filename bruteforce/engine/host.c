/* What the game's runtime asks of the platform layer (weemitt's src/port/log.h and port.h): its messages and the
   clock. */
#include <stdarg.h>
#include <stdio.h>

extern int engine_verbose;

void plog_vprintf(const char *fmt, va_list ap) {
    if (engine_verbose) {
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
}

void plog_printf(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    plog_vprintf(fmt, ap);
    va_end(ap);
}

/* The clock feeds only the game's profiler timers (timer.c), never its logic. A fixed time keeps them the same in
   every run. */
unsigned long long port_time_us(void) {
    return 0;
}
