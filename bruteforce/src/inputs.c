#include "inputs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *name;
    uint16_t bit;
} names[] = {{"A", BTN_A},   {"B", BTN_B},   {"Z", BTN_Z},   {"S", BTN_START}, {"L", BTN_L},   {"R", BTN_R},
             {"du", BTN_DU}, {"dd", BTN_DD}, {"dl", BTN_DL}, {"dr", BTN_DR},   {"cu", BTN_CU}, {"cd", BTN_CD},
             {"cl", BTN_CL}, {"cr", BTN_CR}};

#define NAME_COUNT (sizeof(names) / sizeof(names[0]))

void inputs_init(Inputs *in) {
    in->frames = NULL;
    in->count = 0;
    in->cap = 0;
    in->first = 0;
}

void inputs_free(Inputs *in) {
    free(in->frames);
    inputs_init(in);
}

void inputs_add(Inputs *in, const EngineInput *frame) {
    if (in->count == in->cap) {
        in->cap = in->cap ? in->cap * 2 : 1024;
        in->frames = realloc(in->frames, in->cap * sizeof(EngineInput));
    }
    in->frames[in->count++] = *frame;
}

static int parse_line(char *line, EngineInput *out, Inputs *in, char *err, size_t errSize, long lineNo) {
    long frame = in->first + in->count;
    char *p = line;
    char *tok;
    char *hash = strchr(line, '#');
    long x;
    long y;
    char *end;

    if (hash) {
        *hash = 0;
    }
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == 0) {
        return 1; /* nothing on the line */
    }
    /* An optional frame number. */
    {
        char *colon = strchr(p, ':');

        if (colon) {
            long n = strtol(p, &end, 10);

            if (end == colon && in->count == 0 && n >= 0) {
                in->first = frame = n;
            }
            if (end != colon || n != frame) {
                snprintf(err, errSize, "line %ld: the frame number must be %ld", lineNo, frame);
                return -1;
            }
            p = colon + 1;
        }
    }
    x = strtol(p, &end, 10);
    if (end == p) {
        snprintf(err, errSize, "line %ld: the stick's X is missing", lineNo);
        return -1;
    }
    p = end;
    y = strtol(p, &end, 10);
    if (end == p) {
        snprintf(err, errSize, "line %ld: the stick's Y is missing", lineNo);
        return -1;
    }
    p = end;
    if (x < -128 || x > 127 || y < -128 || y > 127) {
        snprintf(err, errSize, "line %ld: the stick runs from -128 to 127", lineNo);
        return -1;
    }
    out->x = (int8_t)x;
    out->y = (int8_t)y;
    out->buttons = 0;
    for (tok = strtok(p, " \t\r\n"); tok != NULL; tok = strtok(NULL, " \t\r\n")) {
        unsigned i;

        for (i = 0; i < NAME_COUNT; i++) {
            if (!strcasecmp(tok, names[i].name)) {
                break;
            }
        }
        if (i == NAME_COUNT) {
            snprintf(err, errSize, "line %ld: unknown button %s", lineNo, tok);
            return -1;
        }
        out->buttons |= names[i].bit;
    }
    return 0;
}

int input_parse(const char *text, EngineInput *out, char *err, size_t errSize) {
    Inputs none;
    char line[512];
    int r;

    inputs_init(&none);
    snprintf(line, sizeof(line), "%s", text);
    r = parse_line(line, out, &none, err, errSize, 1);
    if (r == 1) {
        snprintf(err, errSize, "no input");
        return -1;
    }
    return r;
}

int inputs_read(Inputs *in, const char *path, char *err, size_t errSize) {
    FILE *f = fopen(path, "r");
    int r;

    if (f == NULL) {
        snprintf(err, errSize, "cannot open %s", path);
        return -1;
    }
    r = inputs_read_file(in, f, err, errSize);
    fclose(f);
    return r;
}

int inputs_read_file(Inputs *in, FILE *f, char *err, size_t errSize) {
    char line[512];
    long lineNo = 0;

    while (fgets(line, sizeof(line), f)) {
        EngineInput frame;
        int r;

        lineNo++;
        r = parse_line(line, &frame, in, err, errSize, lineNo);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            inputs_add(in, &frame);
        }
    }
    return 0;
}

void input_format(const EngineInput *frame, char *buf, size_t size) {
    size_t n = snprintf(buf, size, "%d %d", frame->x, frame->y);
    unsigned i;

    for (i = 0; i < NAME_COUNT && n < size; i++) {
        if (frame->buttons & names[i].bit) {
            n += snprintf(buf + n, size - n, " %s", names[i].name);
        }
    }
}

int inputs_write(const Inputs *in, const char *path, long from, long to) {
    FILE *f = fopen(path, "w");
    long i;

    if (f == NULL) {
        return -1;
    }
    fprintf(f, "# Glover inputs: one line per game frame: frame: stick X, stick Y, buttons held\n");
    for (i = from; i < to && i < in->count; i++) {
        char buf[128];

        input_format(&in->frames[i], buf, sizeof(buf));
        fprintf(f, "%ld: %s\n", in->first + i, buf);
    }
    return fclose(f);
}
