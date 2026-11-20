/* The input recorder and the reader of its files (record.h).

   Format, little-endian:
     header   "WEEMREC\n", u16 version, u16 length of the build id, the build id, u32 flags (1 Space demo,
              2 fixed rate), s32 level, u32 ROM hash, u32 save hash, u8 easy ball, f32 camera follow, 512 bytes of save
     records  one tag byte, then
       'U' f32 dt, u16 buttons, s8 stick x, s8 stick y   one call of port_game_update
       'S' u8 easy ball, f32 camera follow               a setting changed; comes before the update it affects
       'R' u16 word                                      the game read the controller in the update just before
       'C' u32 hash                                      a check of the game's state after the update before
       'E' u32 updates, u32 memory hash                  the end of a run that closed normally */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "record.h"

#ifndef PORT_BUILD_ID
#define PORT_BUILD_ID "unknown"
#endif

static const char magic[8] = { 'W', 'E', 'E', 'M', 'R', 'E', 'C', '\n' };

const char *rec_build_id(void) {
    static char id[REC_BUILD_ID_SIZE];

    snprintf(id, sizeof(id), "%s %s %s%s", PORT_BUILD_ID,
#if defined(_WIN64)
             "windows64",
#elif defined(_WIN32)
             "windows",
#elif defined(__LP64__)
             "linux64",
#else
             "linux",
#endif
#ifdef PORT_AUDIO_MUS
             "mus",
#else
             "stub",
#endif
#ifdef PORT_SPACE_DEMO
             " space-demo"
#else
             ""
#endif
    );
    return id;
}

unsigned rec_hash_bytes(const void *data, unsigned long size) {
    const unsigned char *p = data;
    unsigned h = 2166136261u;
    unsigned long i;

    for (i = 0; i < size; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* ---- Writing. */

static FILE *out;
static char outPath[1024];
static unsigned long updateCount;
static int lastEasy = -1;
static float lastCamera = -1.0f;

static void put8(unsigned v) {
    fputc((int)(v & 0xFF), out);
}

static void put16(unsigned v) {
    put8(v);
    put8(v >> 8);
}

static void put32(unsigned v) {
    put16(v & 0xFFFF);
    put16(v >> 16);
}

static void putf(float f) {
    unsigned v;

    memcpy(&v, &f, 4);
    put32(v);
}

void rec_folder(char *dir, int size) {
    char *pref = SDL_GetPrefPath("weemitt", "Glover");

    snprintf(dir, size, "%s", pref ? pref : "");
    if (pref) {
        SDL_free(pref);
    }
}

void rec_default_path(char *path, int size) {
    char dir[900];
    char stamp[32];
    time_t t = time(NULL);

    rec_folder(dir, sizeof(dir));
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
    snprintf(path, size, "%sweemitt-%s.rec", dir, stamp);
}

int rec_start(const char *path, const RecInfo *info) {
    const char *id = info->buildId[0] ? info->buildId : rec_build_id();

    out = fopen(path, "wb");
    if (out == NULL) {
        return -1;
    }
    snprintf(outPath, sizeof(outPath), "%s", path);
    fwrite(magic, 1, 8, out);
    put16(REC_VERSION);
    put16((unsigned)strlen(id));
    fwrite(id, 1, strlen(id), out);
    put32((info->spaceDemo ? 1 : 0) | (info->fixedRate ? 2 : 0));
    put32((unsigned)info->level);
    put32(info->romHash);
    put32(info->eepromHash);
    put8(info->easyBall != 0);
    putf(info->cameraFollow);
    fwrite(info->eeprom, 1, 512, out);
    fflush(out);
    lastEasy = info->easyBall != 0;
    lastCamera = info->cameraFollow;
    updateCount = 0;
    return 0;
}

int rec_active(void) {
    return out != NULL;
}

const char *rec_path(void) {
    return outPath;
}

unsigned long rec_updates(void) {
    return updateCount;
}

void rec_update(float dt, int easyBall, float cameraFollow, unsigned short buttons, signed char x, signed char y) {
    if (out == NULL) {
        return;
    }
    if ((easyBall != 0) != lastEasy || cameraFollow != lastCamera) {
        lastEasy = easyBall != 0;
        lastCamera = cameraFollow;
        put8('S');
        put8(lastEasy);
        putf(cameraFollow);
    }
    put8('U');
    putf(dt);
    put16(buttons);
    put8((unsigned char)x);
    put8((unsigned char)y);
    updateCount++;
    fflush(out);
}

void rec_read(unsigned short received) {
    if (out != NULL) {
        put8('R');
        put16(received);
        fflush(out);
    }
}

void rec_check(unsigned hash) {
    if (out != NULL) {
        put8('C');
        put32(hash);
        fflush(out);
    }
}

void rec_end(unsigned ramHash) {
    if (out != NULL) {
        put8('E');
        put32((unsigned)updateCount);
        put32(ramHash);
        fclose(out);
        out = NULL;
    }
}

/* ---- Reading. */

static unsigned char *data;
static unsigned long dataSize;
static unsigned long pos;

static unsigned get8(void) {
    return pos < dataSize ? data[pos++] : 0;
}

static unsigned get16(void) {
    unsigned a = get8();

    return a | get8() << 8;
}

static unsigned get32(void) {
    unsigned a = get16();

    return a | get16() << 16;
}

static float getf(void) {
    unsigned v = get32();
    float f;

    memcpy(&f, &v, 4);
    return f;
}

int rec_open(const char *path, RecInfo *info, char *err, int errSize) {
    FILE *f = fopen(path, "rb");
    unsigned version;
    unsigned idLen;
    unsigned flags;
    unsigned long headerEnd;

    memset(info, 0, sizeof(*info));
    if (f == NULL) {
        snprintf(err, errSize, "cannot open the recording %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    dataSize = (unsigned long)ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(dataSize + 1);
    if (fread(data, 1, dataSize, f) != dataSize) {
        fclose(f);
        snprintf(err, errSize, "cannot read the recording %s", path);
        return -1;
    }
    fclose(f);
    pos = 0;
    if (dataSize < 8 || memcmp(data, magic, 8) != 0) {
        snprintf(err, errSize, "%s is not a weemitt recording", path);
        return -1;
    }
    pos = 8;
    version = get16();
    if (version != REC_VERSION) {
        snprintf(err, errSize, "%s has format version %u. This program reads version %d.", path, version, REC_VERSION);
        return -1;
    }
    idLen = get16();
    if (pos + idLen > dataSize) {
        snprintf(err, errSize, "%s is cut short in its header", path);
        return -1;
    }
    memcpy(info->buildId, data + pos, idLen < REC_BUILD_ID_SIZE ? idLen : REC_BUILD_ID_SIZE - 1);
    pos += idLen;
    flags = get32();
    info->spaceDemo = (flags & 1) != 0;
    info->fixedRate = (flags & 2) != 0;
    info->level = (int)get32();
    info->romHash = get32();
    info->eepromHash = get32();
    info->easyBall = get8();
    info->cameraFollow = getf();
    headerEnd = pos + 512;
    if (headerEnd > dataSize) {
        snprintf(err, errSize, "%s is cut short in its header", path);
        return -1;
    }
    memcpy(info->eeprom, data + pos, 512);
    pos = headerEnd;
    return 0;
}

void rec_next(RecEvent *ev) {
    unsigned tag;

    memset(ev, 0, sizeof(*ev));
    if (pos >= dataSize) {
        ev->type = REC_EV_END;
        return;
    }
    tag = data[pos++];
    /* A record cut short at the end of the file counts as the end. */
    switch (tag) {
    case 'U':
        if (pos + 8 > dataSize) break;
        ev->type = REC_EV_UPDATE;
        ev->dt = getf();
        ev->buttons = (unsigned short)get16();
        ev->x = (signed char)get8();
        ev->y = (signed char)get8();
        return;
    case 'S':
        if (pos + 5 > dataSize) break;
        ev->type = REC_EV_SETTINGS;
        ev->easyBall = get8();
        ev->cameraFollow = getf();
        return;
    case 'R':
        if (pos + 2 > dataSize) break;
        ev->type = REC_EV_READ;
        ev->buttons = (unsigned short)get16();
        return;
    case 'C':
        if (pos + 4 > dataSize) break;
        ev->type = REC_EV_CHECK;
        ev->hash = get32();
        return;
    case 'E':
        if (pos + 8 > dataSize) break;
        ev->type = REC_EV_FINISH;
        ev->updates = get32();
        ev->hash = get32();
        return;
    default:
        break;
    }
    pos = dataSize;
    ev->type = REC_EV_END;
}

void rec_close(void) {
    free(data);
    data = NULL;
}
