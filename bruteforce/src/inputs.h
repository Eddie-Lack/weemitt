/* The engine's text format for inputs: one line per game frame.

       # comment
       0 80 A B
       -40 80
       12: 0 0 Z

   Each line holds the stick's X and Y (-128 to 127) and the buttons held: A B Z S (Start) L R, du dd dl dr (the
   D-pad) and cu cd cl cr (the C buttons), in any order and any case. A line may start with its frame number and a
   colon; the number must then be the line's own (the first line is frame 0). Empty lines and text after # are
   ignored. A file may hold a window of a longer movie: then its first line starts with the window's first frame
   number, and the numbers count on from there. */
#ifndef INPUTS_H
#define INPUTS_H

#include <stddef.h>
#include <stdio.h>

#include "engine.h"

typedef struct Inputs {
    EngineInput *frames;
    long count;
    long cap;
    long first; /* the movie frame of frames[0] */
} Inputs;

void inputs_init(Inputs *in);
void inputs_free(Inputs *in);
void inputs_add(Inputs *in, const EngineInput *frame);
/* Reads a file in the text format. Returns 0, or -1 with a message in err. */
int inputs_read(Inputs *in, const char *path, char *err, size_t errSize);
/* The same from an open file. */
int inputs_read_file(Inputs *in, FILE *f, char *err, size_t errSize);
/* Writes frames [from, to) with their frame numbers. Returns 0 or -1. */
int inputs_write(const Inputs *in, const char *path, long from, long to);
/* Parses one frame as a line holds it (without a frame number). Returns 0, or -1 with a message in err. */
int input_parse(const char *text, EngineInput *out, char *err, size_t errSize);
/* One frame as text, as the file holds it (without the frame number). */
void input_format(const EngineInput *frame, char *buf, size_t size);

#endif
