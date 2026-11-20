/* glover: the brute forcer's tools in one program. The game's code and variables then sit at the same addresses for
   every tool, and a state that one tool saves loads in another.

   glover COMMAND [OPTIONS]   (glover COMMAND --help for a command's options) */
#include <stdio.h>
#include <string.h>

#include "tools.h"

static const struct {
    const char *name;
    int (*run)(int argc, char **argv);
    const char *what;
} commands[] = {
    {"bench", bench_main, "measure the engine's speed on this computer"},
    {"play", play_main, "play inputs in the engine and print the game's state per game frame"},
    {"saveload", saveload_main, "the save and load test"},
    {"search", search_main, "change the inputs of a window of a run and keep what reaches the goal sooner"},
};

/* In the studio (weemitt's window program, src/port/studio.c) these commands run when the first argument names one,
   and main is the window program's. */
#ifdef GLOVER_EMBEDDED
#define main glover_main

int glover_has_command(const char *name) {
    unsigned i;

    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (!strcmp(name, commands[i].name)) {
            return 1;
        }
    }
    return 0;
}
#endif

int main(int argc, char **argv) {
    unsigned i;

    if (argc >= 2) {
        for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
            if (!strcmp(argv[1], commands[i].name)) {
                return commands[i].run(argc - 1, argv + 1);
            }
        }
    }
    fprintf(stderr, "usage: glover COMMAND [OPTIONS]\n\ncommands:\n");
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        fprintf(stderr, "  %-10s %s\n", commands[i].name, commands[i].what);
    }
    return 1;
}
