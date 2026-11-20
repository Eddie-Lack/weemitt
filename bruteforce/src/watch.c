#include "watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *typeNames[] = {"u8", "s8", "u16", "s16", "u32", "s32", "f32", "ptr"};

/* Parses a whole number in text (base 0: decimal, or hex with 0x). Returns 0, or -1 when text is empty or has
   anything after the number. */
static int parse_num(const char *text, int base, uint32_t *out) {
    char *end;

    *out = (uint32_t)strtoul(text, &end, base);
    return end == text || *end != 0 ? -1 : 0;
}

/* Parses "NAME", "NAME+OFF" or "0xADDR" into an N64 address. */
static int parse_addr(const char *s, size_t len, uint32_t *out, char *err, size_t errSize) {
    char name[64];
    char *plus;
    uint32_t addr;

    if (len == 0 || len >= sizeof(name)) {
        snprintf(err, errSize, "bad address");
        return -1;
    }
    memcpy(name, s, len);
    name[len] = 0;
    plus = strchr(name, '+');
    if (plus) {
        *plus = 0;
    }
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        if (parse_num(name, 16, &addr) != 0 || addr < 0x80000000u) {
            snprintf(err, errSize, "%s is not an N64 address (0x80000000 and up)", name);
            return -1;
        }
    } else {
        addr = engine_symbol(name);
        if (addr == 0) {
            snprintf(err, errSize, "unknown game global %s", name);
            return -1;
        }
    }
    if (plus) {
        uint32_t off;

        if (parse_num(plus + 1, 0, &off) != 0) {
            snprintf(err, errSize, "bad offset +%s", plus + 1);
            return -1;
        }
        addr += off;
    }
    *out = addr;
    return 0;
}

int watch_parse(Watch *w, const char *text, char *err, size_t errSize) {
    const char *colon = strrchr(text, ':');
    const char *arrow = strstr(text, "->");
    unsigned t;

    memset(w, 0, sizeof(*w));
    snprintf(w->text, sizeof(w->text), "%s", text);
    if (colon == NULL) {
        snprintf(err, errSize, "%s: the type is missing (NAME:TYPE)", text);
        return -1;
    }
    for (t = 0; t < sizeof(typeNames) / sizeof(typeNames[0]); t++) {
        if (!strcmp(colon + 1, typeNames[t])) {
            break;
        }
    }
    if (t == sizeof(typeNames) / sizeof(typeNames[0])) {
        snprintf(err, errSize, "%s: unknown type", text);
        return -1;
    }
    w->type = (WatchType)t;
    if (arrow != NULL && arrow < colon) {
        if (parse_addr(text, arrow - text, &w->addr, err, errSize) != 0) {
            return -1;
        }
        char off[32];
        size_t len = (size_t)(colon - arrow - 2);

        w->deref = 1;
        if (len >= sizeof(off)) {
            snprintf(err, errSize, "%s: bad offset after ->", text);
            return -1;
        }
        memcpy(off, arrow + 2, len);
        off[len] = 0;
        if (parse_num(off, 0, &w->offset) != 0) {
            snprintf(err, errSize, "%s: bad offset after ->", text);
            return -1;
        }
    } else if (parse_addr(text, colon - text, &w->addr, err, errSize) != 0) {
        return -1;
    }
    return 0;
}

int watch_parse_list(Watch *list, int max, const char *text, char *err, size_t errSize) {
    char buf[4096];
    char *tok;
    int n = 0;

    snprintf(buf, sizeof(buf), "%s", text);
    for (tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
        if (n == max) {
            snprintf(err, errSize, "more than %d values", max);
            return -1;
        }
        if (watch_parse(&list[n], tok, err, errSize) != 0) {
            return -1;
        }
        n++;
    }
    return n;
}

uint32_t watch_read(const Engine *e, const Watch *w) {
    uint32_t addr = w->addr;

    if (w->deref) {
        addr = engine_readptr(e, addr);
        if (addr == 0) {
            return 0;
        }
        addr += w->offset;
    }
    switch (w->type) {
        case W_U8:
            return engine_read8(e, addr);
        case W_S8:
            return (uint32_t)(int32_t)(int8_t)engine_read8(e, addr);
        case W_U16:
            return engine_read16(e, addr);
        case W_S16:
            return (uint32_t)(int32_t)(int16_t)engine_read16(e, addr);
        case W_PTR:
            return engine_readptr(e, addr);
        default:
            return engine_read32(e, addr);
    }
}

int watch_write(Engine *e, const Watch *w, uint32_t bits) {
    static const int sizes[] = {1, 1, 2, 2, 4, 4, 4, 4};

    if (w->deref || w->type == W_PTR) {
        return -1;
    }
    return engine_write(e, w->addr, sizes[w->type], bits);
}

double watch_value(const Watch *w, uint32_t bits) {
    float f;

    switch (w->type) {
        case W_S8:
        case W_S16:
        case W_S32:
            return (int32_t)bits;
        case W_F32:
            memcpy(&f, &bits, 4);
            return f;
        default:
            return bits;
    }
}

void watch_format(const Watch *w, uint32_t bits, char *buf, size_t size) {
    switch (w->type) {
        case W_F32:
            snprintf(buf, size, "%.4f/%08x", watch_value(w, bits), bits);
            break;
        case W_PTR:
            snprintf(buf, size, "%08x", bits);
            break;
        case W_S8:
        case W_S16:
        case W_S32:
            snprintf(buf, size, "%d", (int32_t)bits);
            break;
        default:
            snprintf(buf, size, "%u", bits);
            break;
    }
}
