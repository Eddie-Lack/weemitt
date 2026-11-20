#include "cond.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cond_parse(Cond *c, const char *text, char *err, size_t errSize) {
    static const char *ops[] = {"==", "!=", "<", "<=", ">", ">=", "&"};
    char buf[1024];
    char *tok[64];
    int n = 0;
    int i = 0;
    char *t;

    memset(c, 0, sizeof(*c));
    snprintf(buf, sizeof(buf), "%s", text);
    for (t = strtok(buf, " \t\r\n"); t != NULL && n < 64; t = strtok(NULL, " \t\r\n")) {
        tok[n++] = t;
    }
    while (i < n) {
        CondTerm *term;
        unsigned op;
        char *end;

        if (c->count == COND_MAX_TERMS) {
            snprintf(err, errSize, "%s: more than %d terms", text, COND_MAX_TERMS);
            return -1;
        }
        term = &c->terms[c->count];
        if (c->count > 0) {
            if (!strcmp(tok[i], "or")) {
                term->orBefore = 1;
            } else if (strcmp(tok[i], "and") != 0) {
                snprintf(err, errSize, "%s: expected 'and' or 'or' before %s", text, tok[i]);
                return -1;
            }
            i++;
        }
        if (i + 3 > n) {
            snprintf(err, errSize, "%s: a term is VALUE OP NUMBER", text);
            return -1;
        }
        if (watch_parse(&term->value, tok[i], err, errSize) != 0) {
            return -1;
        }
        for (op = 0; op < sizeof(ops) / sizeof(ops[0]); op++) {
            if (!strcmp(tok[i + 1], ops[op])) {
                break;
            }
        }
        if (op == sizeof(ops) / sizeof(ops[0])) {
            snprintf(err, errSize, "%s: unknown operator %s", text, tok[i + 1]);
            return -1;
        }
        term->op = (CondOp)op;
        term->number = strtod(tok[i + 2], &end);
        if (strncmp(tok[i + 2], "0x", 2) == 0 || strncmp(tok[i + 2], "0X", 2) == 0) {
            term->number = (double)strtoul(tok[i + 2], &end, 16);
        }
        if (*end) {
            snprintf(err, errSize, "%s: %s is not a number", text, tok[i + 2]);
            return -1;
        }
        c->count++;
        i += 3;
    }
    return 0;
}

static int term_eval(const CondTerm *t, const Engine *e) {
    uint32_t bits = watch_read(e, &t->value);
    double v = watch_value(&t->value, bits);

    switch (t->op) {
        case OP_EQ:
            return v == t->number;
        case OP_NE:
            return v != t->number;
        case OP_LT:
            return v < t->number;
        case OP_LE:
            return v <= t->number;
        case OP_GT:
            return v > t->number;
        case OP_GE:
            return v >= t->number;
        default:
            return (bits & (uint32_t)t->number) != 0;
    }
}

int cond_eval(const Cond *c, const Engine *e) {
    int i = 0;

    /* Groups joined by "or"; within a group, terms joined by "and". */
    while (i < c->count) {
        int all = 1;

        do {
            if (all && !term_eval(&c->terms[i], e)) {
                all = 0;
            }
            i++;
        } while (i < c->count && !c->terms[i].orBefore);
        if (all) {
            return 1;
        }
    }
    return 0;
}
