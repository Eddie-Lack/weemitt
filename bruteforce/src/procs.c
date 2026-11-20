#include "procs.h"

#include <cpuid.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>

const char *self_path(void) {
    static char path[MAX_PATH];

    if (path[0] == 0) {
        GetModuleFileNameA(NULL, path, sizeof(path));
    }
    return path;
}

/* The processes started with proc_start, by their output. */
#define MAX_PROCS 256
static struct {
    FILE *out;
    HANDLE process;
} procs[MAX_PROCS];

/* CreateProcess runs the program without a shell, and without a console window: the studio is a window program,
   and _popen's cmd.exe would open one for each worker. */
FILE *proc_start(const char *cmd) {
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE readEnd;
    HANDLE writeEnd;
    char *line = _strdup(cmd);
    FILE *f;
    int fd;
    int i;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (line == NULL || !CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        free(line);
        return NULL;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writeEnd;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        free(line);
        return NULL;
    }
    free(line);
    CloseHandle(writeEnd);
    CloseHandle(pi.hThread);
    fd = _open_osfhandle((intptr_t)readEnd, 0);
    f = fd >= 0 ? _fdopen(fd, "r") : NULL;
    for (i = 0; f != NULL && i < MAX_PROCS; i++) {
        if (procs[i].out == NULL) {
            procs[i].out = f;
            procs[i].process = pi.hProcess;
            return f;
        }
    }
    CloseHandle(pi.hProcess);
    return f;
}

int proc_wait(FILE *out) {
    DWORD code = 1;
    int i;

    fclose(out);
    for (i = 0; i < MAX_PROCS; i++) {
        if (procs[i].out == out) {
            WaitForSingleObject(procs[i].process, INFINITE);
            GetExitCodeProcess(procs[i].process, &code);
            CloseHandle(procs[i].process);
            procs[i].out = NULL;
            break;
        }
    }
    return (int)code;
}

void quote_arg(char *buf, size_t size, const char *arg) {
    size_t n = strlen(buf);

    snprintf(buf + n, size - n, "%s\"%s\"", n ? " " : "", arg);
}

int cpu_logical(void) {
    SYSTEM_INFO si;

    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

int cpu_physical(void) {
    DWORD len = 0;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info;
    DWORD i;
    int cores = 0;

    GetLogicalProcessorInformation(NULL, &len);
    info = malloc(len);
    if (info == NULL || !GetLogicalProcessorInformation(info, &len)) {
        free(info);
        return 0;
    }
    for (i = 0; i < len / sizeof(*info); i++) {
        cores += info[i].Relationship == RelationProcessorCore;
    }
    free(info);
    return cores;
}

void lower_priority(void) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
}

void sleep_until(double t) {
    double left;

    while ((left = t - now_seconds()) > 0) {
        Sleep(left > 0.02 ? (DWORD)(left * 1000) - 10 : 0);
    }
}

/* The processes started with proc_spawn. */
static HANDLE spawned[MAX_PROCS];

int proc_spawn(const char *cmd, const char *logPath) {
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE log;
    char *line = _strdup(cmd);
    int i;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    log = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (line == NULL || log == INVALID_HANDLE_VALUE) {
        free(line);
        return -1;
    }
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL;
    si.hStdOutput = log;
    si.hStdError = log;
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(log);
        free(line);
        return -1;
    }
    free(line);
    CloseHandle(log);
    CloseHandle(pi.hThread);
    for (i = 0; i < MAX_PROCS; i++) {
        if (spawned[i] == NULL) {
            spawned[i] = pi.hProcess;
            return i;
        }
    }
    CloseHandle(pi.hProcess);
    return -1;
}

int proc_running(int id) {
    if (id < 0 || id >= MAX_PROCS || spawned[id] == NULL) {
        return 0;
    }
    if (WaitForSingleObject(spawned[id], 0) == WAIT_TIMEOUT) {
        return 1;
    }
    CloseHandle(spawned[id]);
    spawned[id] = NULL;
    return 0;
}
#else
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const char *self_path(void) {
    static char path[4096];

    if (path[0] == 0) {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);

        path[n > 0 ? n : 0] = 0;
    }
    return path;
}

FILE *proc_start(const char *cmd) {
    return popen(cmd, "r");
}

int proc_wait(FILE *out) {
    int status = pclose(out);

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void quote_arg(char *buf, size_t size, const char *arg) {
    size_t n = strlen(buf);
    const char *p;

    /* Room for a space, two quotes and the end of the string. */
    if (n + 3 < size) {
        if (n) {
            buf[n++] = ' ';
        }
        buf[n++] = '\'';
        for (p = arg; *p && n + 5 < size; p++) {
            if (*p == '\'') {
                memcpy(buf + n, "'\\''", 4);
                n += 4;
            } else {
                buf[n++] = *p;
            }
        }
        buf[n++] = '\'';
        buf[n] = 0;
    }
}

int cpu_logical(void) {
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
}

int cpu_physical(void) {
    /* Count the distinct (package, core) pairs. */
    int n = cpu_logical();
    int seen[1024][2];
    int count = 0;
    int i;
    int j;

    for (i = 0; i < n && i < 1024; i++) {
        char path[128];
        FILE *f;
        int pkg = 0;
        int core = 0;

        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
        if ((f = fopen(path, "r")) == NULL || fscanf(f, "%d", &core) != 1) {
            if (f) {
                fclose(f);
            }
            return 0;
        }
        fclose(f);
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);
        if ((f = fopen(path, "r")) != NULL) {
            if (fscanf(f, "%d", &pkg) != 1) {
                pkg = 0;
            }
            fclose(f);
        }
        for (j = 0; j < count; j++) {
            if (seen[j][0] == pkg && seen[j][1] == core) {
                break;
            }
        }
        if (j == count) {
            seen[count][0] = pkg;
            seen[count][1] = core;
            count++;
        }
    }
    return count;
}

void lower_priority(void) {
    setpriority(PRIO_PROCESS, 0, 10);
}

void sleep_until(double t) {
    double left;

    while ((left = t - now_seconds()) > 0) {
        struct timespec ts;

        ts.tv_sec = (time_t)left;
        ts.tv_nsec = (long)((left - ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }
}

/* The processes started with proc_spawn. */
#define MAX_SPAWNED 64
static pid_t spawned[MAX_SPAWNED];

int proc_spawn(const char *cmd, const char *logPath) {
    pid_t pid;
    int i;

    for (i = 0; i < MAX_SPAWNED && spawned[i] != 0; i++) {
    }
    if (i == MAX_SPAWNED) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        FILE *log = freopen(logPath, "w", stdout);

        if (log != NULL) {
            dup2(fileno(stdout), 2);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    spawned[i] = pid;
    return i;
}

int proc_running(int id) {
    int status;

    if (id < 0 || id >= MAX_SPAWNED || spawned[id] == 0) {
        return 0;
    }
    if (waitpid(spawned[id], &status, WNOHANG) == 0) {
        return 1;
    }
    spawned[id] = 0;
    return 0;
}

#endif

void cpu_name(char *buf, size_t size) {
    unsigned int r[12];
    unsigned int i;
    char *p;

    if (__get_cpuid_max(0x80000000, NULL) < 0x80000004) {
        snprintf(buf, size, "unknown");
        return;
    }
    for (i = 0; i < 3; i++) {
        __get_cpuid(0x80000002 + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
    }
    p = (char *)r;
    while (*p == ' ') {
        p++;
    }
    snprintf(buf, size, "%.48s", p);
    p = buf + strlen(buf);
    while (p > buf && p[-1] == ' ') {
        *--p = 0;
    }
}
