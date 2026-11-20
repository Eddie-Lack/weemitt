#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Parser {
    const char *p;
    const char *start;
    char *err;
    size_t errSize;
    int failed;
} Parser;

static void fail(Parser *ps, const char *what) {
    int line = 1;
    const char *c;

    if (ps->failed) {
        return;
    }
    for (c = ps->start; c < ps->p; c++) {
        line += *c == '\n';
    }
    snprintf(ps->err, ps->errSize, "line %d: %s", line, what);
    ps->failed = 1;
}

static void skip(Parser *ps) {
    for (;;) {
        while (isspace((unsigned char)*ps->p)) {
            ps->p++;
        }
        /* Comments are not JSON, but a config is easier to read with them: // to the end of the line. */
        if (ps->p[0] == '/' && ps->p[1] == '/') {
            while (*ps->p && *ps->p != '\n') {
                ps->p++;
            }
            continue;
        }
        return;
    }
}

static int parse_value(Parser *ps, Json *out);

static char *parse_string(Parser *ps) {
    size_t cap = 32;
    size_t n = 0;
    char *s = malloc(cap);

    ps->p++; /* the opening quote */
    while (*ps->p && *ps->p != '"') {
        char c = *ps->p++;

        if (c == '\\') {
            c = *ps->p++;
            switch (c) {
                case 'n':
                    c = '\n';
                    break;
                case 't':
                    c = '\t';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case '"':
                case '\\':
                case '/':
                    break;
                default:
                    fail(ps, "this escape in a string is not supported");
                    free(s);
                    return NULL;
            }
        }
        if (n + 2 > cap) {
            cap *= 2;
            s = realloc(s, cap);
        }
        s[n++] = c;
    }
    if (*ps->p != '"') {
        fail(ps, "a string has no end");
        free(s);
        return NULL;
    }
    ps->p++;
    s[n] = 0;
    return s;
}

static int parse_value(Parser *ps, Json *out) {
    memset(out, 0, sizeof(*out));
    skip(ps);
    if (*ps->p == '{' || *ps->p == '[') {
        int object = *ps->p == '{';
        char close = object ? '}' : ']';
        int cap = 0;

        out->type = object ? JSON_OBJECT : JSON_ARRAY;
        ps->p++;
        skip(ps);
        if (*ps->p == close) {
            ps->p++;
            return 0;
        }
        for (;;) {
            if (out->count == cap) {
                cap = cap ? cap * 2 : 8;
                out->items = realloc(out->items, cap * sizeof(Json));
                if (object) {
                    out->keys = realloc(out->keys, cap * sizeof(char *));
                }
            }
            skip(ps);
            if (object) {
                if (*ps->p != '"') {
                    fail(ps, "a key must be a string in quotes");
                    return -1;
                }
                out->keys[out->count] = parse_string(ps);
                if (out->keys[out->count] == NULL) {
                    return -1;
                }
                skip(ps);
                if (*ps->p != ':') {
                    fail(ps, "a ':' must follow a key");
                    return -1;
                }
                ps->p++;
            }
            if (parse_value(ps, &out->items[out->count]) != 0) {
                return -1;
            }
            out->count++;
            skip(ps);
            if (*ps->p == ',') {
                ps->p++;
                continue;
            }
            if (*ps->p == close) {
                ps->p++;
                return 0;
            }
            fail(ps, object ? "expected ',' or '}'" : "expected ',' or ']'");
            return -1;
        }
    }
    if (*ps->p == '"') {
        out->type = JSON_STRING;
        out->string = parse_string(ps);
        return out->string ? 0 : -1;
    }
    if (!strncmp(ps->p, "true", 4) || !strncmp(ps->p, "false", 5)) {
        out->type = JSON_BOOL;
        out->number = ps->p[0] == 't';
        ps->p += ps->p[0] == 't' ? 4 : 5;
        return 0;
    }
    if (!strncmp(ps->p, "null", 4)) {
        out->type = JSON_NULL;
        ps->p += 4;
        return 0;
    }
    {
        char *end;
        double v = strtod(ps->p, &end);

        if (end == ps->p) {
            fail(ps, "expected a value");
            return -1;
        }
        out->type = JSON_NUMBER;
        out->number = v;
        ps->p = end;
        return 0;
    }
}

static void free_inner(Json *j) {
    int i;

    free(j->string);
    for (i = 0; i < j->count; i++) {
        free_inner(&j->items[i]);
        if (j->keys) {
            free(j->keys[i]);
        }
    }
    free(j->items);
    free(j->keys);
}

Json *json_parse(const char *text, char *err, size_t errSize) {
    Parser ps;
    Json *j = calloc(1, sizeof(Json));

    ps.p = ps.start = text;
    ps.err = err;
    ps.errSize = errSize;
    ps.failed = 0;
    if (parse_value(&ps, j) != 0) {
        json_free(j);
        return NULL;
    }
    skip(&ps);
    if (*ps.p) {
        fail(&ps, "text after the end");
        json_free(j);
        return NULL;
    }
    return j;
}

void json_free(Json *j) {
    if (j) {
        free_inner(j);
        free(j);
    }
}

const Json *json_get(const Json *object, const char *key) {
    int i;

    if (object == NULL || object->type != JSON_OBJECT) {
        return NULL;
    }
    for (i = 0; i < object->count; i++) {
        if (!strcmp(object->keys[i], key)) {
            return &object->items[i];
        }
    }
    return NULL;
}

double json_number(const Json *object, const char *key, double def) {
    const Json *v = json_get(object, key);

    return v != NULL && (v->type == JSON_NUMBER || v->type == JSON_BOOL) ? v->number : def;
}

const char *json_string(const Json *object, const char *key, const char *def) {
    const Json *v = json_get(object, key);

    return v != NULL && v->type == JSON_STRING ? v->string : def;
}
