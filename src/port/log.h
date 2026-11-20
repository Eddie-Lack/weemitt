/* The program's log. The game's own messages (debugPrintf and the PRINTF macro in main.c), port_log and the errors
   of the platform layer all come here. Every message goes to stderr, as before. It also goes to a log file and to a
   ring of recent lines for the menu's log window, when those are on. A Windows build is a GUI program and has no
   stderr, so the file is the only place its messages can be read. Plain C: the menu and the game's port files use it. */
#ifndef PORT_LOG_H
#define PORT_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_RING_LINES 300      /* lines the log window keeps */
#define LOG_LINE_SIZE 256       /* bytes of one kept line, with its time stamp */

/* Writes one message (a printf format, no end of line needed). Safe to call from any thread. */
void plog_vprintf(const char *fmt, va_list ap);
void plog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Names the file that --log gave. It is used in place of weemitt.log in the save folder. */
void plog_set_override(const char *path);
/* The path of the log file: the override, or weemitt.log in the save folder (empty when there is none). */
void plog_path(char *out, int size);
/* Starts (path) or stops (NULL) writing the log file. Returns 0 when it worked. A file that is over 1 MB is
   started again from empty; a smaller one is appended to. */
int plog_set_file(const char *path);
int plog_file_active(void);
/* Opens the log's folder in the system's file manager. */
void plog_open_folder(void);

/* The ring of recent lines. plog_ring_total counts every line since the start (or the last clear). Line n is
   still available when total - LOG_RING_LINES <= n < total. plog_ring_get copies it; it returns 0 when gone. */
void plog_ring_enable(void);
unsigned plog_ring_total(void);
int plog_ring_get(unsigned n, char *out, int size);
void plog_ring_clear(void);

/* Verbose level: plog_tick, called once for each drawn frame, writes the frame rate, the game mode and level once
   a second. It costs one clock read when verbose is off. */
void plog_set_verbose(int on);
void plog_tick(long mode, long level);

/* Writes the exception, the place and the game's state to the log when the program crashes (Windows: an unhandled
   exception; Linux: SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGABRT). */
void plog_install_crash_handler(void);
/* Hidden test: crashes at once (WEEMITT_CRASH_TEST). */
void plog_crash_now(void);

#ifdef __cplusplus
}
#endif

#endif
