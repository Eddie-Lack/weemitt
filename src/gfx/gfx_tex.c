/* Textures: how a tile's texels are decoded, and the cache of decoded textures. gfx_tmem.c records where each load
   came from and describes what a tile samples.

   A decoded texture is cached by its address, layout and palette addresses. Once a frame, the first time it is
   used, the cache hashes its texels and palette entries: the game animates some textures by rewriting their
   texels, and those are decoded again when the hash changes. */
#include "gfx_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_internal.h"

#define NUM_BUCKETS 4096
#define EVICT_INTERVAL 120 /* frames between sweeps of the cache */
#define EVICT_AGE 600      /* frames a texture may go unused before it is freed */

typedef struct TexEntry {
    GfxTexDesc desc; /* the key */
    u32 keyHash;
    GLuint tex;
    u32 content;     /* hash of the texels and palette entries */
    u32 checkedFrame;
    u32 usedFrame;
    int next;        /* the next entry in the bucket, or in the free list */
} TexEntry;

static TexEntry *entries;
static int entryCount;
static int entryCap;
static int freeList = -1;
static int buckets[NUM_BUCKETS];
static int bucketsReady;
static u8 *pixels;
static int pixelsCap;
static int traceTextures = -1; /* GLOVER_GFX_TRACE: print each new texture; -1 until read */

/* ---- Decoding to RGBA8. */

static u32 hash_bytes(u32 h, const u8 *p, u32 n) {
    while (n >= 4) {
        u32 k;
        memcpy(&k, p, 4);
        k *= 0xCC9E2D51u;
        k = (k << 15) | (k >> 17);
        k *= 0x1B873593u;
        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xE6546B64u;
        p += 4;
        n -= 4;
    }
    while (n--) {
        h = (h ^ *p++) * 0x01000193u;
    }
    return h;
}

static u32 row_bytes(const GfxTexDesc *d) {
    return (d->width * gfx_tex_size_x2(d->siz) + 1) / 2;
}

/* A palette entry as RGBA8: RGBA 5551 or IA 88. Without a loaded palette, the index is an intensity. */
static u32 pal_entry(const GfxTexDesc *d, u32 i) {
    u32 block = d->siz == SIZ_4B ? 0 : (i >> 4) & 15;
    u32 addr = d->pal[block];
    const u8 *p = addr ? gfx_mem(addr + (i & 15) * 2, 2) : NULL;
    u16 v;

    if (addr == 0) {
        return (d->siz == SIZ_4B ? i * 0x11 : i) * 0x010101u | 0xFF000000u;
    }
    if (p == NULL) {
        return 0;
    }
    memcpy(&v, p, 2);
    if (d->palType == 3) {
        return (u32)(v >> 8) * 0x010101u | ((u32)(v & 0xFF) << 24);
    } else {
        u32 r = (v >> 11) & 0x1F;
        u32 g = (v >> 6) & 0x1F;
        u32 b = (v >> 1) & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        return r | (g << 8) | (b << 16) | ((v & 1) ? 0xFF000000u : 0);
    }
}

static u32 content_hash(const GfxTexDesc *d) {
    u32 h = 0x9747B28Cu;
    u32 rb = row_bytes(d);
    u32 y;
    int i;

    for (y = 0; y < d->height; y++) {
        const u8 *row = gfx_mem(d->addr + y * d->stride, rb);
        if (row != NULL) {
            h = hash_bytes(h, row, rb);
        }
    }
    for (i = 0; i < 16; i++) {
        if (d->pal[i]) {
            const u8 *p = gfx_mem(d->pal[i], 32);
            if (p != NULL) {
                h = hash_bytes(h, p, 32);
            }
        }
    }
    return h;
}

static u32 rgba(u32 r, u32 g, u32 b, u32 a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* Intensity and alpha from 3-bit, 4-bit and 5-bit fields, widened as the RDP does. */
static u32 expand3(u32 v) {
    return (v << 5) | (v << 2) | (v >> 1);
}

static u32 expand5(u32 v) {
    return (v << 3) | (v >> 2);
}

static void decode(const GfxTexDesc *d, u32 *out) {
    u32 pal[256];
    u32 rb = row_bytes(d);
    u32 x;
    u32 y;

    if (d->fmt == FMT_CI) {
        for (x = 0; x < (d->siz == SIZ_4B ? 16u : 256u); x++) {
            pal[x] = pal_entry(d, x);
        }
    }
    for (y = 0; y < d->height; y++) {
        const u8 *row = gfx_mem(d->addr + y * d->stride, rb);
        u32 *o = out + y * d->width;

        if (row == NULL) {
            memset(o, 0, d->width * 4);
            continue;
        }
        for (x = 0; x < d->width; x++) {
            u32 c;
            if (d->siz == SIZ_16B) {
                u16 v;
                memcpy(&v, row + x * 2, 2);
                if (d->fmt == FMT_IA) {
                    c = (v >> 8) * 0x010101u | ((u32)(v & 0xFF) << 24);
                } else if (d->fmt == FMT_CI) {
                    c = pal[v & 0xFF];
                } else {
                    c = rgba(expand5((v >> 11) & 0x1F), expand5((v >> 6) & 0x1F), expand5((v >> 1) & 0x1F),
                             (v & 1) ? 0xFF : 0);
                }
            } else if (d->siz == SIZ_32B) {
                u32 v;
                memcpy(&v, row + x * 4, 4);
                c = rgba(v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
            } else if (d->siz == SIZ_8B) {
                u32 v = row[x];
                if (d->fmt == FMT_CI) {
                    c = pal[v];
                } else if (d->fmt == FMT_IA) {
                    c = (v >> 4) * 0x111111u | ((v & 0xF) * 0x11u << 24);
                } else {
                    c = v * 0x01010101u;
                }
            } else {
                u32 v = (x & 1) ? row[x >> 1] & 0xF : row[x >> 1] >> 4;
                if (d->fmt == FMT_CI) {
                    c = pal[v];
                } else if (d->fmt == FMT_IA) {
                    c = expand3(v >> 1) * 0x010101u | ((v & 1) ? 0xFF000000u : 0);
                } else {
                    c = v * 0x11111111u;
                }
            }
            o[x] = c;
        }
    }
}

/* ---- The cache. */

static u32 key_hash(const GfxTexDesc *d) {
    return hash_bytes(0x2545F491u, (const u8 *)d, sizeof(*d));
}

static void upload(TexEntry *e, int created) {
    int bytes = e->desc.width * e->desc.height * 4;

    if (bytes > pixelsCap) {
        u8 *p = realloc(pixels, bytes);
        if (p == NULL) {
            return;
        }
        pixels = p;
        pixelsCap = bytes;
    }
    decode(&e->desc, (u32 *)pixels);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (created) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, e->desc.width, e->desc.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, e->desc.width, e->desc.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    gfxStats.textureUploads++;
}

GLuint gfx_tex_get(const GfxTexDesc *desc) {
    u32 h = key_hash(desc);
    int *link;
    int i;
    TexEntry *e;

    if (!bucketsReady) {
        for (i = 0; i < NUM_BUCKETS; i++) {
            buckets[i] = -1;
        }
        bucketsReady = 1;
    }
    for (i = buckets[h & (NUM_BUCKETS - 1)]; i >= 0; i = entries[i].next) {
        e = &entries[i];
        if (e->keyHash == h && memcmp(&e->desc, desc, sizeof(*desc)) == 0) {
            e->usedFrame = gfx_tex_frame();
            if (e->checkedFrame != gfx_tex_frame()) {
                u32 c = content_hash(desc);
                e->checkedFrame = gfx_tex_frame();
                if (c != e->content) {
                    e->content = c;
                    upload(e, 0);
                }
            }
            return e->tex;
        }
    }
    /* A new entry: from the free list, or a new one at the end. */
    if (freeList >= 0) {
        i = freeList;
        freeList = entries[i].next;
    } else {
        if (entryCount == entryCap) {
            int cap = entryCap ? entryCap * 2 : 256;
            TexEntry *n = realloc(entries, cap * sizeof(TexEntry));
            if (n == NULL) {
                return 0;
            }
            entries = n;
            entryCap = cap;
        }
        i = entryCount++;
    }
    e = &entries[i];
    e->desc = *desc;
    e->keyHash = h;
    e->content = content_hash(desc);
    e->checkedFrame = gfx_tex_frame();
    e->usedFrame = gfx_tex_frame();
    glGenTextures(1, &e->tex);
    if (traceTextures < 0) {
        traceTextures = gfx_getenv("GLOVER_GFX_TRACE") != NULL;
    }
    if (traceTextures) {
        printf("texture %u: addr %x stride %u %ux%u fmt %d siz %d pal %d %x\n", e->tex, desc->addr, desc->stride,
               desc->width, desc->height, desc->fmt, desc->siz, desc->palType, desc->pal[0]);
    }
    upload(e, 1);
    link = &buckets[h & (NUM_BUCKETS - 1)];
    e->next = *link;
    *link = i;
    return e->tex;
}

/* Frees the textures that went unused for a while. */
void gfx_tex_end_frame(void) {
    int b;

    if (gfx_tex_frame() % EVICT_INTERVAL != 0 || !bucketsReady) {
        return;
    }
    for (b = 0; b < NUM_BUCKETS; b++) {
        int *link = &buckets[b];
        while (*link >= 0) {
            TexEntry *e = &entries[*link];
            if (gfx_tex_frame() - e->usedFrame > EVICT_AGE) {
                int i = *link;
                *link = e->next;
                glDeleteTextures(1, &e->tex);
                e->tex = 0;
                e->next = freeList;
                freeList = i;
            } else {
                link = &e->next;
            }
        }
    }
}

void gfx_tex_shutdown(void) {
    int b;

    if (bucketsReady) {
        for (b = 0; b < NUM_BUCKETS; b++) {
            int i;
            for (i = buckets[b]; i >= 0; i = entries[i].next) {
                glDeleteTextures(1, &entries[i].tex);
            }
            buckets[b] = -1;
        }
    }
    free(entries);
    free(pixels);
    entries = NULL;
    pixels = NULL;
    entryCount = entryCap = pixelsCap = 0;
    freeList = -1;
}
