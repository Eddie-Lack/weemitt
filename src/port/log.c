#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "frame.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#define LOG_MAX_BYTES (1024 * 1024)

static SDL_SpinLock lock;
static FILE *file;
static const char *overridePath;   /* the --log argument; it stays in argv */
static char *ring;                  /* LOG_RING_LINES lines of LOG_LINE_SIZE bytes, allocated by plog_ring_enable */
static unsigned ringTotal;
static int verbose;
static int crashing;                /* set in the crash handler: take no lock, the crashed thread may hold it */
static Uint64 startCount;
static Uint32 tickLast;             /* plog_tick: the time of the last line and the frames since */
static long tickFrames;

static void lock_on(void) {
    if (!crashing) {
        SDL_AtomicLock(&lock);
    }
}

static void lock_off(void) {
    if (!crashing) {
        SDL_AtomicUnlock(&lock);
    }
}

static double elapsed(void) {
    Uint64 now = SDL_GetPerformanceCounter();

    if (startCount == 0) {
        startCount = now;
    }
    return (double)(now - startCount) / (double)SDL_GetPerformanceFrequency();
}

/* Stores one line (no end of line) in the file and the ring. The caller holds the lock. */
static void store_line(const char *text, double t) {
    char line[LOG_LINE_SIZE];

    snprintf(line, sizeof(line), "[%9.3f] %s", t, text);
    if (ring != NULL) {
        strcpy(ring + (size_t)(ringTotal % LOG_RING_LINES) * LOG_LINE_SIZE, line);
        ringTotal++;
    }
    if (file != NULL) {
        fputs(line, file);
        fputc('\n', file);
        fflush(file);
    }
}

void plog_vprintf(const char *fmt, va_list ap) {
    char buf[1024];
    char *p;
    double t;

    vsnprintf(buf, sizeof(buf), fmt, ap);
    fputs(buf, stderr);
    fputc('\n', stderr);
    if (file == NULL && ring == NULL) {
        return;
    }
    t = elapsed();
    lock_on();
    /* A message can hold several lines (the game's Crash message starts with blank ones). */
    for (p = buf; *p != 0;) {
        char *end = p;

        while (*end != 0 && *end != '\n') {
            end++;
        }
        if (*end == '\n') {
            *end++ = 0;
        }
        if (p[0] != 0 && p[0] != '\r') {
            store_line(p, t);
        }
        p = end;
    }
    lock_off();
}

void plog_printf(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    plog_vprintf(fmt, ap);
    va_end(ap);
}

void plog_set_override(const char *path) {
    overridePath = path;
}

void plog_path(char *out, int size) {
    char *pref;

    if (overridePath != NULL) {
        snprintf(out, size, "%s", overridePath);
        return;
    }
    pref = SDL_GetPrefPath("weemitt", "Glover");
    snprintf(out, size, "%sweemitt.log", pref ? pref : "");
    if (pref == NULL) {
        out[0] = 0;
    } else {
        SDL_free(pref);
    }
}

int plog_set_file(const char *path) {
    FILE *f = NULL;
    FILE *old;
    long size = 0;

    if (path != NULL) {
        if (path[0] == 0) {
            return -1;
        }
        f = fopen(path, "ab");
        if (f != NULL) {
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            if (size > LOG_MAX_BYTES) {
                fclose(f);
                f = fopen(path, "wb");
            }
        }
        if (f == NULL) {
            plog_printf("weemitt: cannot write the log file %s", path);
            return -1;
        }
    }
    lock_on();
    old = file;
    file = f;
    if (f != NULL) {
        store_line("---- log started ----", elapsed());
    }
    lock_off();
    if (old != NULL) {
        fclose(old);
    }
    return 0;
}

int plog_file_active(void) {
    return file != NULL;
}

void plog_open_folder(void) {
    char path[512];
    char *slash;

    plog_path(path, sizeof(path));
    if (path[0] == 0) {
        return;
    }
    slash = strrchr(path, '/');
#ifdef _WIN32
    if (strrchr(path, '\\') > slash) {
        slash = strrchr(path, '\\');
    }
#endif
    if (slash != NULL) {
        slash[1] = 0;
    }
#ifdef _WIN32
    ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
#else
    {
        char url[600];

        snprintf(url, sizeof(url), "file://%s", path);
        SDL_OpenURL(url);
    }
#endif
}

void plog_ring_enable(void) {
    if (ring == NULL) {
        ring = (char *)calloc(LOG_RING_LINES, LOG_LINE_SIZE);
    }
}

unsigned plog_ring_total(void) {
    return ringTotal;
}

int plog_ring_get(unsigned n, char *out, int size) {
    int ok = 0;

    lock_on();
    if (ring != NULL && n < ringTotal && ringTotal - n <= LOG_RING_LINES) {
        snprintf(out, size, "%s", ring + (size_t)(n % LOG_RING_LINES) * LOG_LINE_SIZE);
        ok = 1;
    }
    lock_off();
    return ok;
}

void plog_ring_clear(void) {
    lock_on();
    if (ring != NULL) {
        memset(ring, 0, (size_t)LOG_RING_LINES * LOG_LINE_SIZE);
    }
    ringTotal = 0;
    lock_off();
}

void plog_set_verbose(int on) {
    verbose = on;
}

void plog_tick(long mode, long level) {
    Uint32 now;

    if (!verbose) {
        return;
    }
    tickFrames++;
    now = SDL_GetTicks();
    if (tickLast == 0) {
        tickLast = now;
    } else if (now - tickLast >= 1000) {
        plog_printf("%.1f fps, mode %ld, level %ld", tickFrames * 1000.0 / (now - tickLast), mode, level);
        tickLast = now;
        tickFrames = 0;
    }
}

/* ---- The crash handler. It only writes lines; the place of the fault is the line to read first. */

static void log_game_state(void) {
    char trace[256];

    trace[0] = 0;
    port_game_trace(trace, sizeof(trace));
    plog_printf("game: mode %d, level %d, %s", port_game_mode(), port_game_level(), trace);
}

/* Opens the log file if it is off, so that a crash is always written. */
static void crash_open(char *path, int size) {
    plog_path(path, size);
    if (file == NULL) {
        plog_set_file(path);
    }
}

#ifdef _WIN32

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    static volatile LONG entered;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    const CONTEXT *c = ep->ContextRecord;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t at = (uintptr_t)er->ExceptionAddress;
    char path[512];
    char msg[800];
#ifdef _WIN64
    const uintptr_t *sp = (const uintptr_t *)c->Rsp;
#else
    const uintptr_t *sp = (const uintptr_t *)c->Esp;
#endif
    int i;
    int shown = 0;

    if (InterlockedExchange(&entered, 1)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    crashing = 1;
    crash_open(path, sizeof(path));
    plog_printf("CRASH: exception %08lX at %p (exe base %p, offset +%X)", er->ExceptionCode, (void *)at,
                (void *)base, (unsigned)(at - base));
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        plog_printf("CRASH: %s address %p", er->ExceptionInformation[0] == 0 ? "read from" :
                    er->ExceptionInformation[0] == 1 ? "write to" : "execute at", (void *)er->ExceptionInformation[1]);
    }
#ifdef _WIN64
    plog_printf("CRASH: rax %p rbx %p rcx %p rdx %p rsi %p rdi %p rbp %p rsp %p", (void *)c->Rax, (void *)c->Rbx,
                (void *)c->Rcx, (void *)c->Rdx, (void *)c->Rsi, (void *)c->Rdi, (void *)c->Rbp, (void *)c->Rsp);
#else
    plog_printf("CRASH: eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX esp %08lX", c->Eax, c->Ebx,
                c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
#endif
    /* Words on the stack that point into the program are likely return addresses. */
    if (!IsBadReadPtr(sp, 512 * sizeof(*sp))) {
        for (i = 0; i < 512 && shown < 16; i++) {
            if (sp[i] > base + 0x1000 && sp[i] < base + 0x1000000) {
                plog_printf("CRASH: stack +%03X: %p (exe offset +%X)", i * (int)sizeof(*sp), (void *)sp[i],
                            (unsigned)(sp[i] - base));
                shown++;
            }
        }
    }
    log_game_state();
    if (file != NULL) {
        fflush(file);
    }
    snprintf(msg, sizeof(msg), "Glover has crashed (exception %08lX).\n\nThe details are in the log file:\n%s",
             er->ExceptionCode, path);
    MessageBoxA(NULL, msg, "Glover", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

void plog_install_crash_handler(void) {
    SetUnhandledExceptionFilter(crash_filter);
}

#else

static void crash_signal(int sig, siginfo_t *si, void *uc) {
    char path[512];
    const ucontext_t *ctx = (const ucontext_t *)uc;
#if defined(__x86_64__)
    uintptr_t ip = (uintptr_t)ctx->uc_mcontext.gregs[REG_RIP];
#else
    uintptr_t ip = (uintptr_t)ctx->uc_mcontext.gregs[REG_EIP];
#endif
    extern char __executable_start[];
    uintptr_t base = (uintptr_t)__executable_start;

    crashing = 1;
    crash_open(path, sizeof(path));
    plog_printf("CRASH: signal %d at %p (exe base %p, offset +%X), fault address %p", sig, (void *)ip, (void *)base,
                (unsigned)(ip - base), si->si_addr);
    log_game_state();
    if (file != NULL) {
        fflush(file);
    }
    plog_printf("CRASH: details are in %s", path);
    /* SA_RESETHAND restored the default action; returning runs the faulting instruction again and ends the program. */
}

void plog_install_crash_handler(void) {
    struct sigaction sa;
    static const int sigs[] = {SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGABRT};
    unsigned i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        sigaction(sigs[i], &sa, NULL);
    }
}

#endif

void plog_crash_now(void) {
    volatile int *p = NULL;

    *p = 1;
}
