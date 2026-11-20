/* Worker processes: start copies of this program and read what they print. */
#ifndef PROCS_H
#define PROCS_H

#include <stdio.h>

/* The full path of this program. */
const char *self_path(void);

/* Starts the command line cmd (quote paths with quote_arg) and returns its standard output, or NULL. */
FILE *proc_start(const char *cmd);
/* Waits for the process to end. Returns its exit status. */
int proc_wait(FILE *out);

/* Starts the command line cmd in the background, with its output (standard output and error) written to the file
   logPath. Returns a number for proc_running, or -1 when it cannot start. */
int proc_spawn(const char *cmd, const char *logPath);
/* 1 while the process that proc_spawn started runs. */
int proc_running(int id);

/* Appends arg to buf, always quoted for the system's shell, after a space when buf is not empty. */
void quote_arg(char *buf, size_t size, const char *arg);

/* The number of logical processors, and of physical cores (0 when unknown). */
int cpu_logical(void);
int cpu_physical(void);
/* The processor's name (from the CPUID instruction). */
void cpu_name(char *buf, size_t size);

/* Lowers this process's priority: the computer stays usable while the workers run. */
void lower_priority(void);

/* Sleeps until now_seconds() reaches t. */
void sleep_until(double t);

#endif
