/* The commands of the glover program (main.c). Each takes the arguments after the program's name: argv[0] is the
   command's name. */
#ifndef TOOLS_H
#define TOOLS_H

int bench_main(int argc, char **argv);
int play_main(int argc, char **argv);
int saveload_main(int argc, char **argv);
int search_main(int argc, char **argv);

#endif
