/* Conditions on the game's memory, written as text in a config:

       gameInfo+0x54:u8 & 2                     (a bit test: true when the value AND the number is not 0)
       player+0x10:s32 < 5 or hand+0x38:f32 < -500
       frameCount:s32 >= 100 and gameInfo:s8 == 4

   A term is a value (watch.h) compared with a number: == != < <= > >= or & (bit test). "and" binds more tightly
   than "or". There are no brackets. */
#ifndef COND_H
#define COND_H

#include <stddef.h>

#include "engine.h"
#include "watch.h"

#define COND_MAX_TERMS 16

typedef enum CondOp { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_AND } CondOp;

typedef struct CondTerm {
    Watch value;
    CondOp op;
    double number;
    int orBefore; /* 1: this term starts a new "or" group */
} CondTerm;

typedef struct Cond {
    CondTerm terms[COND_MAX_TERMS];
    int count; /* 0: no condition (never true) */
} Cond;

/* Parses text. Returns 0, or -1 with a message in err. */
int cond_parse(Cond *c, const char *text, char *err, size_t errSize);
/* 1 when the condition holds now. */
int cond_eval(const Cond *c, const Engine *e);

#endif
