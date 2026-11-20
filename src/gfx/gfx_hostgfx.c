/* The display lists for a host's own F3DEX2 interpreter (gfx_hostgfx.h), such as Fast3D.

   gfx_dl.c walks the game's F3DEX lists without drawing (gfx_dl_host_walk). It resolves the segments, follows the
   draw marks and blends the matrices between two recorded frames, and reports each command here. This file writes
   each command again as F3DEX2, in two words of pointer size, with host pointers in place of the game's addresses.

   - Each list that a G_DL calls is written out again in the output arena, so the call structure stays.
   - The state commands of every mark are kept. Triangles and vertex loads are kept only in the requested layers.
   - With split, the triangles that do not write depth (shadows, rings on the water, sparkles) go to a second list.
     The state commands go to both lists.
   - The game's projection groups (a projection load and the multiplies that follow it) become the host's matrices.
   - The colour and depth images, the scissor, the viewport and the segments stay the host's.
   - The texture rectangles of the requested layers (the game's 3D sprites: dust, sparkles) become two triangles in
     the world, at the place and depth the game gave them: the host's interpreter draws texture rectangles without
     depth. Fill rectangles there are counted as skipped.
   - The game keeps 16-bit and 32-bit texels and every palette in host byte order (port_convert_texture_bank). The
     host reads N64 byte order, so these get a converted copy. 4-bit and 8-bit texels at an even address are passed
     as they are. Each copy, and each texture passed as it is, is hashed once per generation. When its texels
     change, the copy is converted again in place and the entry list starts with an invalidation of its address. */
#include "gfx_host.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_hostgfx.h"
#include "gfx_internal.h"

/* F3DEX opcodes. */
#define X_MTX 0x01
#define X_MOVEMEM 0x03
#define X_VTX 0x04
#define X_TRI2 0xB1
#define X_MODIFYVTX 0xB2
#define X_CLEARGEOMETRYMODE 0xB6
#define X_SETGEOMETRYMODE 0xB7
#define X_SETOTHERMODE_L 0xB9
#define X_SETOTHERMODE_H 0xBA
#define X_TEXTURE 0xBB
#define X_MOVEWORD 0xBC
#define X_POPMTX 0xBD
#define X_CULLDL 0xBE
#define X_TRI1 0xBF
#define X_NOOP 0xC0
#define X_RDPHALF_2 0xB3
#define X_RDPHALF_1 0xB4

/* RDP opcodes. */
#define R_TEXRECT 0xE4
#define R_TEXRECTFLIP 0xE5
#define R_SETSCISSOR 0xED
#define R_LOADTLUT 0xF0
#define R_LOADBLOCK 0xF3
#define R_LOADTILE 0xF4
#define R_SETTILE 0xF5
#define R_FILLRECT 0xF6
#define R_SETTIMG 0xFD
#define R_SETZIMG 0xFE
#define R_SETCIMG 0xFF

/* F3DEX2 opcodes, and the host's command that invalidates its texture cache. */
#define F2_VTX 0x01u
#define F2_MODIFYVTX 0x02u
#define F2_CULLDL 0x03u
#define F2_TRI1 0x05u
#define F2_TRI2 0x06u
#define F2_TEXTURE 0xD7u
#define F2_POPMTX 0xD8u
#define F2_GEOMETRYMODE 0xD9u
#define F2_MTX 0xDAu
#define F2_MOVEWORD 0xDBu
#define F2_MOVEMEM 0xDCu
#define F2_DL 0xDEu
#define F2_ENDDL 0xDFu
#define F2_SETOTHERMODE_L 0xE2u
#define F2_SETOTHERMODE_H 0xE3u
#define F2_MV_LIGHT 10u
#define HOST_INVALTEXCACHE 0x34u

/* F3DEX G_MOVEMEM and G_MOVEWORD indices. */
#define X_MV_VIEWPORT 0x80
#define X_MV_LOOKATY 0x82
#define X_MV_LOOKATX 0x84
#define X_MV_L0 0x86
#define X_MV_L7 0x94
#define MW_NUMLIGHT 0x02
#define MW_CLIP 0x04
#define MW_SEGMENT 0x06
#define MW_PERSPNORM 0x0E
#define MW_FOG 0x08
#define MW_LIGHTCOL 0x0A

/* F3DEX G_MTX flags, and F3DEX2's. */
#define X_MTX_PROJECTION 0x01
#define X_MTX_LOAD 0x02
#define X_MTX_PUSH 0x04
#define F2_MTX_PUSH 0x01u
#define F2_MTX_LOAD 0x02u
#define F2_MTX_PROJECTION 0x04u

/* Geometry mode bits: F3DEX's, and F3DEX2's where they differ. */
#define X_GM_KEEP 0x009F0005u /* G_ZBUFFER, G_SHADE, G_FOG, G_LIGHTING, the texture generation, G_LOD, G_CLIPPING */
#define X_GM_SMOOTH 0x200u
#define X_GM_CULL_FRONT 0x1000u
#define X_GM_CULL_BACK 0x2000u
#define F2_GM_SMOOTH 0x200000u
#define F2_GM_CULL_FRONT 0x200u
#define F2_GM_CULL_BACK 0x400u
#define GM_FOG_BIT 0x10000u

/* The first cycle of the blender: its four inputs, and G_RM_FOG_SHADE_A's (the fog colour by the shade's alpha, which
   G_FOG makes the fog amount). */
#define RM_CYCLE1 0xCCCC0000u
#define RM_FOG_SHADE_A 0xC8000000u

#define MAX_DEPTH 18
#define ARENA_CHUNK (256 * 1024)
#define LOOKAHEAD 64
#define EVICT_AGE 3600 /* generations a texture may go unused before its copy is freed */
#define TEX_BUCKETS 4096
#define START_COMMANDS 7 /* the state an entry list starts with, and one more with the host's fog */
#define SPRITE_V0 32 /* a 3D sprite's corner slots: F3DEX uses the first 32, the interpreter needs 36 */

/* ---- The output: one growing buffer per call depth, and an arena that keeps the finished lists. */

typedef struct Buf {
    GfxHostCmd *c;
    int n, cap;
} Buf;

typedef struct Chunk {
    struct Chunk *next;
    size_t size, used;
    unsigned char data[];
} Chunk;

/* A texture as the host gets it: a copy, or the game's own texels. TEX_IA16PAL: a palette of IA16 entries, given as
   RGBA16 entries, the only palette type the host reads. */
enum { TEX_RAW, TEX_COPY, TEX_SWAP16, TEX_SWAP32, TEX_IA16PAL };

typedef struct TexEntry {
    u32 addr, bytes;     /* the game's address and the bytes loaded */
    u8 kind;
    unsigned char *copy; /* NULL for TEX_RAW */
    const u8 *src;       /* the game's texels, as a host pointer */
    u32 hash;
    unsigned int checked, used; /* generations */
    const void **users;  /* a palette: the textures drawn with it */
    int nUsers, usersCap;
    int next;
} TexEntry;

static struct {
    Buf bufs[2][MAX_DEPTH]; /* the list being written at each call depth, of each pass */
    Buf entry[2];
    int passes;          /* 2 with split: the triangles that write no depth go to the second list */
    Chunk *chunks;       /* the arena; the first chunk is the one in use */
    Chunk *spare;        /* chunks of earlier calls, used again */
    const GfxHostRequest *rq;
    GfxHostResult out;
    int failed;          /* an allocation failed */
    int task;            /* the task being walked */
    int inGroup;         /* in a projection group: its multiplies are left out */
    u8 light[8][16];     /* the latest light of each slot, for G_MW_LIGHTCOL */
    /* The texture state that decides how a load is converted. */
    struct {
        u8 fmt, siz;
        u16 tmem;
        u32 w0, w1;      /* the G_SETTILE as the game wrote it */
    } tile[8];
    int texTile;
    u32 otherH;          /* the other mode's high word, for the TLUT mode */
    u32 geom;            /* the geometry mode, F3DEX bits */
    u32 texture[2];      /* the latest G_TEXTURE written, or 0, 0 */
    int pal;             /* the entry of the latest palette loaded, or -1 */
    /* The invalidations of this call. */
    const void **inval;
    int invalN, invalCap;
    /* The texture entries. */
    TexEntry *tex;
    int texN, texCap;
    int buckets[TEX_BUCKETS];
    int bucketsReady;
    unsigned int lastEvict;
} H;

static int grow(void **p, int *cap, int need, size_t elem) {
    int n;
    void *q;

    if (need <= *cap) {
        return 1;
    }
    n = *cap ? *cap : 256;
    while (n < need) {
        n *= 2;
    }
    q = realloc(*p, (size_t)n * elem);
    if (q == NULL) {
        H.failed = 1;
        return 0;
    }
    *p = q;
    *cap = n;
    return 1;
}

static void emit_to(Buf *b, uintptr_t w0, uintptr_t w1) {
    if (!grow((void **)&b->c, &b->cap, b->n + 1, sizeof(GfxHostCmd))) {
        return;
    }
    b->c[b->n].w0 = w0;
    b->c[b->n].w1 = w1;
    b->n++;
    H.out.commands++;
}

static int curDepth;

/* A state command: into the lists of both passes. */
static void emit(u32 w0, uintptr_t w1) {
    int p;

    for (p = 0; p < H.passes; p++) {
        emit_to(&H.bufs[p][curDepth], w0, w1);
    }
}

/* A triangle command: into the second pass's list when it writes no depth and the lists are split. */
static void emit_prim(u32 w0, uintptr_t w1) {
    int p = H.passes == 2 && !(gfxRdp.otherL & RM_Z_UPD) ? 1 : 0;

    emit_to(&H.bufs[p][curDepth], w0, w1);
}

/* size bytes of the arena, aligned to 16. NULL when no memory is left. */
static void *arena_alloc(size_t size) {
    Chunk *c = H.chunks;
    void *p;

    size = (size + 15) & ~(size_t)15;
    if (c == NULL || c->used + size > c->size) {
        Chunk **s = &H.spare;

        c = NULL;
        while (*s != NULL) {
            if ((*s)->size >= size) {
                c = *s;
                *s = c->next;
                break;
            }
            s = &(*s)->next;
        }
        if (c == NULL) {
            size_t want = size > ARENA_CHUNK ? size : ARENA_CHUNK;

            c = (Chunk *)malloc(sizeof(Chunk) + want + 16);
            if (c == NULL) {
                H.failed = 1;
                return NULL;
            }
            c->size = want;
        }
        c->used = 0;
        c->next = H.chunks;
        H.chunks = c;
    }
    p = (void *)(((uintptr_t)c->data + c->used + 15) & ~(uintptr_t)15);
    c->used = (size_t)((unsigned char *)p - c->data) + size;
    return p;
}

static void arena_reset(void) {
    while (H.chunks != NULL) {
        Chunk *c = H.chunks;

        H.chunks = c->next;
        c->next = H.spare;
        H.spare = c;
    }
}

/* Copies a finished list into the arena, with its G_ENDDL. */
static const GfxHostCmd *finish(Buf *b) {
    GfxHostCmd *p;

    emit_to(b, (uintptr_t)F2_ENDDL << 24, 0);
    p = (GfxHostCmd *)arena_alloc((size_t)b->n * sizeof(GfxHostCmd));
    if (p != NULL) {
        memcpy(p, b->c, (size_t)b->n * sizeof(GfxHostCmd));
    }
    b->n = 0;
    return p;
}

static void add_inval(const void *p) {
    if (grow((void **)&H.inval, &H.invalCap, H.invalN + 1, sizeof(const void *))) {
        H.inval[H.invalN++] = p;
    }
}

/* ---- Called by gfx_dl.c's walk. */

void gfx_host_task(int index) {
    int p;

    H.task = index;
    curDepth = 0;
    for (p = 0; p < H.passes; p++) {
        H.bufs[p][0].n = 0;
    }
}

void gfx_host_call(int depth) {
    int p;

    if (depth < MAX_DEPTH) {
        for (p = 0; p < H.passes; p++) {
            H.bufs[p][depth].n = 0;
        }
        curDepth = depth;
    }
}

/* The end of the list at depth: written to the arena and called from the list that called it, or from the entry. */
void gfx_host_return(u32 w0, int depth) {
    int p;

    if (depth >= MAX_DEPTH) {
        return;
    }
    H.inGroup = 0;
    for (p = 0; p < H.passes; p++) {
        const GfxHostCmd *list = finish(&H.bufs[p][depth]);

        if (list == NULL) {
            continue;
        }
        if (depth == 0) {
            emit_to(&H.entry[p], (uintptr_t)F2_DL << 24, (uintptr_t)list);
        } else {
            emit_to(&H.bufs[p][depth - 1], F2_DL << 24 | (((w0 >> 16) & 0xFF) ? 1u << 16 : 0), (uintptr_t)list);
        }
    }
    if (depth > 0) {
        curDepth = depth - 1;
    }
}

/* ---- Matrices. */

/* A matrix as an Mtx: the integer halves of the 16 elements, then their fraction halves. Values out of the s15.16
   range are clamped: a float to integer conversion out of range differs between x86 and arm64. */
static void *fixed_matrix(const float *m) {
    u32 *w = (u32 *)arena_alloc(64);
    int k;

    if (w == NULL) {
        return NULL;
    }
    memset(w, 0, 64);
    for (k = 0; k < 16; k++) {
        float x = m[k] * 65536.0f;
        s32 v;
        u32 hi;
        u32 lo;

        if (x >= 2147483648.0f) {
            v = 0x7FFFFFFF;
        } else if (x > -2147483648.0f) {
            v = (s32)x;
        } else if (x == x) {
            v = (s32)0x80000000u;
        } else {
            v = 0; /* not a number */
        }
        hi = (u32)v >> 16;
        lo = (u32)v & 0xFFFF;
        w[k >> 1] |= (k & 1) ? hi : hi << 16;
        w[8 + (k >> 1)] |= (k & 1) ? lo : lo << 16;
    }
    return w;
}

static void matrix(u32 w0, u32 w1) {
    u32 flags = (w0 >> 16) & 0xFF;
    float m[16];
    int blended = gfx_dl_last_matrix(m);
    const void *p;
    u32 f2;

    if (blended < 0) {
        H.out.skipped++;
        return;
    }
    if (flags & X_MTX_PROJECTION) {
        if (H.rq->projection[0] != NULL) {
            int k;

            if (!(flags & X_MTX_LOAD)) {
                if (H.inGroup) {
                    return;
                }
                /* A multiply with no load before it: it goes onto the host's projection. */
                H.out.skipped++;
            } else {
                for (k = 0; k < 4 && H.rq->projection[k] != NULL; k++) {
                    f2 = F2_MTX_PROJECTION | (k == 0 ? F2_MTX_LOAD : 0);
                    emit(F2_MTX << 24 | 0x380000u | (f2 ^ F2_MTX_PUSH), (uintptr_t)H.rq->projection[k]);
                }
                H.inGroup = 1;
                return;
            }
        }
        f2 = F2_MTX_PROJECTION | ((flags & X_MTX_LOAD) ? F2_MTX_LOAD : 0);
    } else {
        H.inGroup = 0;
        f2 = ((flags & X_MTX_LOAD) ? F2_MTX_LOAD : 0) | ((flags & X_MTX_PUSH) ? F2_MTX_PUSH : 0);
    }
    p = blended ? fixed_matrix(m) : (const void *)gfx_mem(gfx_dl_seg_addr(w1), 64);
    if (p != NULL) {
        emit(F2_MTX << 24 | 0x380000u | (f2 ^ F2_MTX_PUSH), (uintptr_t)p);
    }
}

/* ---- Lights. */

static void move_mem(u32 w0, u32 w1) {
    u32 index = (w0 >> 16) & 0xFF;
    const u8 *p = gfx_mem(gfx_dl_seg_addr(w1), 16);
    u32 offset;

    if (p == NULL || index == X_MV_VIEWPORT) {
        return;
    }
    if (index == X_MV_LOOKATX) {
        offset = 0;
    } else if (index == X_MV_LOOKATY) {
        offset = 24;
    } else if (index >= X_MV_L0 && index <= X_MV_L7) {
        int k = (int)(index - X_MV_L0) >> 1;

        memcpy(H.light[k], p, 16);
        offset = (u32)(k + 2) * 24;
    } else {
        return;
    }
    emit(F2_MOVEMEM << 24 | ((16 - 1) / 8) << 19 | (offset / 8) << 8 | F2_MV_LIGHT, (uintptr_t)p);
}

/* G_MW_LIGHTCOL, which the host's interpreter does not take: the light again with its new colour. */
static void light_color(u32 offset, u32 data) {
    int k = (int)(offset >> 5) & 7;
    u8 *l = (u8 *)arena_alloc(16);
    u8 *c = H.light[k] + ((offset & 4) ? 4 : 0);

    if (l == NULL) {
        return;
    }
    c[0] = (u8)(data >> 24);
    c[1] = (u8)(data >> 16);
    c[2] = (u8)(data >> 8);
    c[3] = (u8)data;
    memcpy(l, H.light[k], 16);
    emit(F2_MOVEMEM << 24 | ((16 - 1) / 8) << 19 | ((u32)(k + 2) * 24 / 8) << 8 | F2_MV_LIGHT, (uintptr_t)l);
}

static void move_word(u32 w0, u32 w1) {
    u32 index = w0 & 0xFF;
    u32 offset = (w0 >> 8) & 0xFFFF;
    int n;

    switch (index) {
        case MW_NUMLIGHT:
            n = (int)((w1 - 0x80000000u) >> 5) - 1;
            n = n < 0 ? 0 : n > 7 ? 7 : n;
            emit(F2_MOVEWORD << 24 | MW_NUMLIGHT << 16, (uintptr_t)(24 * n));
            break;
        case MW_FOG:
            emit(F2_MOVEWORD << 24 | MW_FOG << 16, H.rq->hostFog ? H.rq->fogWord : w1);
            break;
        case MW_LIGHTCOL:
            light_color(offset, w1);
            break;
        case MW_CLIP:
        case MW_SEGMENT:
        case MW_PERSPNORM:
            /* The segments are resolved here; the clip ratio and the perspective normalisation have no use. */
            break;
        default:
            /* G_MW_MATRIX and G_MW_POINTS, which change a matrix or a vertex in place: not translated. */
            if (gfxLayer.kind & H.rq->mask) {
                H.out.skipped++;
            }
            break;
    }
}

static u32 geometry_bits(u32 x) {
    u32 r = x & X_GM_KEEP;

    if (x & X_GM_SMOOTH) {
        r |= F2_GM_SMOOTH;
    }
    if (x & X_GM_CULL_FRONT) {
        r |= F2_GM_CULL_FRONT;
    }
    if (x & X_GM_CULL_BACK) {
        r |= F2_GM_CULL_BACK;
    }
    return r;
}

/* The geometry mode bits set as the request wants the fog: with the host's fog every triangle has G_FOG, and with
   noFog none has. */
static u32 fog_bits(u32 set) {
    if (H.rq->hostFog) {
        return set | GM_FOG_BIT;
    }
    if (H.rq->noFog) {
        return set & ~GM_FOG_BIT;
    }
    return set;
}

/* The low word of the other mode for the host. As the request wants the fog: with the host's fog, the first cycle of
   the blender blends in the fog colour (G_RM_FOG_SHADE_A), as the game's own render modes do in a level with fog. The
   depth source is always the pixel's: the game's primitive depth (G_ZS_PRIM) is a depth in its own projection, and
   its sprites become triangles that carry their own depth (gfx_host_texrect). weemitt's renderer uses the primitive
   depth for rectangles only; an interpreter that implements G_ZS_PRIM would use it for the triangles too. */
static u32 host_mode_l(u32 l) {
    l &= ~RM_ZS_PRIM;
    return H.rq->hostFog ? (l & ~RM_CYCLE1) | RM_FOG_SHADE_A : l;
}

/* ---- Textures. */

static u32 hash_bytes(const u8 *p, u32 n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    u32 i;

    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;

        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return (u32)(h ^ (h >> 32));
}

static void convert(TexEntry *e) {
    u32 i;

    if (e->kind == TEX_SWAP16) {
        for (i = 0; i + 2 <= e->bytes; i += 2) {
            e->copy[i] = e->src[i + 1];
            e->copy[i + 1] = e->src[i];
        }
    } else if (e->kind == TEX_IA16PAL) {
        /* Intensity and alpha bytes to 5-bit red, green and blue and a 1-bit alpha. */
        for (i = 0; i + 2 <= e->bytes; i += 2) {
            u16 v;
            u32 c;
            u16 o;

            memcpy(&v, e->src + i, 2);
            c = (u32)(v >> 8) >> 3;
            o = (u16)(c << 11 | c << 6 | c << 1 | ((v & 0xFF) >= 0x80 ? 1 : 0));
            e->copy[i] = (u8)(o >> 8);
            e->copy[i + 1] = (u8)o;
        }
    } else if (e->kind == TEX_SWAP32) {
        for (i = 0; i + 4 <= e->bytes; i += 4) {
            e->copy[i] = e->src[i + 3];
            e->copy[i + 1] = e->src[i + 2];
            e->copy[i + 2] = e->src[i + 1];
            e->copy[i + 3] = e->src[i];
        }
    } else {
        memcpy(e->copy, e->src, e->bytes);
    }
}

static const void *tex_ptr(const TexEntry *e) {
    return e->kind == TEX_RAW ? (const void *)e->src : (const void *)e->copy;
}

static u32 tex_bucket(u32 addr, u32 bytes, int kind) {
    return gfx_interp_mix(gfx_interp_mix(addr, bytes), (u32)kind) & (TEX_BUCKETS - 1);
}

static void rebuild_buckets(void) {
    int i;

    for (i = 0; i < TEX_BUCKETS; i++) {
        H.buckets[i] = -1;
    }
    for (i = 0; i < H.texN; i++) {
        u32 b = tex_bucket(H.tex[i].addr, H.tex[i].bytes, H.tex[i].kind);

        H.tex[i].next = H.buckets[b];
        H.buckets[b] = i;
    }
    H.bucketsReady = 1;
}

/* The invalidations a changed or freed entry needs: its own address, and for a palette the textures drawn with it. */
static void invalidate(TexEntry *e) {
    int k;

    add_inval(tex_ptr(e));
    for (k = 0; k < e->nUsers; k++) {
        add_inval(e->users[k]);
    }
    e->nUsers = 0;
}

static int find_texture(u32 addr, u32 bytes, int kind) {
    int i;

    if (!H.bucketsReady) {
        rebuild_buckets();
    }
    for (i = H.buckets[tex_bucket(addr, bytes, kind)]; i >= 0; i = H.tex[i].next) {
        if (H.tex[i].addr == addr && H.tex[i].bytes == bytes && H.tex[i].kind == kind) {
            return i;
        }
    }
    return -1;
}

/* The entry for bytes bytes at the game's address addr, converted as kind; NULL without memory. */
static TexEntry *texture(u32 addr, u32 bytes, int kind) {
    const u8 *src = gfx_mem(addr, bytes);
    TexEntry *e;
    u32 b;
    int i;

    if (src == NULL || bytes == 0) {
        return NULL;
    }
    if (kind == TEX_RAW && ((uintptr_t)src & 1)) {
        kind = TEX_COPY;
    }
    i = find_texture(addr, bytes, kind);
    if (i >= 0) {
        e = &H.tex[i];
        e->src = src;
        if (e->checked != H.rq->generation) {
            u32 h = hash_bytes(src, bytes);

            e->checked = H.rq->generation;
            if (h != e->hash) {
                e->hash = h;
                if (e->copy != NULL) {
                    convert(e);
                }
                invalidate(e);
            }
        }
        e->used = H.rq->generation;
        return e;
    }
    if (!grow((void **)&H.tex, &H.texCap, H.texN + 1, sizeof(TexEntry))) {
        return NULL;
    }
    e = &H.tex[H.texN];
    memset(e, 0, sizeof(*e));
    e->addr = addr;
    e->bytes = bytes;
    e->kind = (u8)kind;
    e->src = src;
    if (kind != TEX_RAW) {
        /* A little more than the texels: the host may read a whole row past the end of a load. */
        e->copy = (unsigned char *)calloc(1, bytes + 64);
        if (e->copy == NULL) {
            H.failed = 1;
            return NULL;
        }
        convert(e);
    }
    /* The host may still hold an older texture at this address: one of its own, from memory it freed. */
    add_inval(tex_ptr(e));
    e->hash = hash_bytes(src, bytes);
    e->checked = e->used = H.rq->generation;
    b = tex_bucket(addr, bytes, kind);
    e->next = H.buckets[b];
    H.buckets[b] = H.texN;
    H.texN++;
    return e;
}

/* Frees the copies that went unused for EVICT_AGE generations, with an invalidation for each. */
static void evict(void) {
    int i;
    int n = 0;

    if (H.rq->generation - H.lastEvict < EVICT_AGE / 4) {
        return;
    }
    H.lastEvict = H.rq->generation;
    for (i = 0; i < H.texN; i++) {
        TexEntry *e = &H.tex[i];

        if (H.rq->generation - e->used > EVICT_AGE) {
            invalidate(e);
            free(e->copy);
            free((void *)e->users);
            continue;
        }
        H.tex[n++] = *e;
    }
    if (n != H.texN) {
        H.texN = n;
        H.bucketsReady = 0;
    }
}

/* Bytes per texel times 2, as the host's load counts them: G_LOADBLOCK takes half a byte for a 4-bit texel,
   G_LOADTILE a whole byte. */
static u32 load_bytes(const u32 *cmd, u32 siz, u32 width) {
    u32 op = cmd[0] >> 24;

    if (op == R_LOADBLOCK) {
        u32 lrs = (cmd[1] >> 12) & 0xFFF;

        return ((lrs + 1) << siz) >> 1;
    }
    if (op == R_LOADTILE) {
        u32 lrt = (cmd[1] & 0xFFF) >> 2;
        u32 per = siz == SIZ_4B ? 1 : 1u << (siz - 1);

        return (lrt + 1) * width * per;
    }
    if (op == R_LOADTLUT) {
        return (((cmd[1] >> 14) & 0x3FF) + 1) * 2;
    }
    return 0;
}

/* G_SETTIMG: reads ahead in the same list for the load that uses the image and the tile that draws it, which
   decide how many bytes are loaded and whether they are swapped. pc is the game's address of the next command. */
static uintptr_t texture_image(u32 w0, u32 w1, u32 pc, int inLayer) {
    u32 siz = (w0 >> 19) & 3;
    u32 width = (w0 & 0xFFF) + 1;
    u32 addr = gfx_dl_seg_addr(w1);
    u32 load[2] = { 0, 0 };
    int loadTile = -1;
    int texTile = H.texTile;
    int renderSiz = -1;
    struct {
        u8 fmt, siz;
        u16 tmem;
        u8 set;
    } tile[8];
    int i;
    int kind;
    u32 bytes;
    TexEntry *e;

    for (i = 0; i < 8; i++) {
        tile[i].fmt = H.tile[i].fmt;
        tile[i].siz = H.tile[i].siz;
        tile[i].tmem = H.tile[i].tmem;
        tile[i].set = 0;
    }
    for (i = 0; i < LOOKAHEAD; i++) {
        const u8 *p = gfx_mem(pc + (u32)i * 8, 8);
        u32 c[2];
        u32 op;

        if (p == NULL) {
            break;
        }
        memcpy(c, p, 8);
        op = c[0] >> 24;
        if (op == R_SETTILE) {
            int t = (int)(c[1] >> 24) & 7;

            tile[t].fmt = (c[0] >> 21) & 7;
            tile[t].siz = (c[0] >> 19) & 3;
            tile[t].tmem = c[0] & 0x1FF;
            tile[t].set = 1;
        } else if (op == X_TEXTURE) {
            texTile = (int)(c[0] >> 8) & 7;
        } else if (loadTile < 0 && (op == R_LOADBLOCK || op == R_LOADTILE || op == R_LOADTLUT)) {
            load[0] = c[0];
            load[1] = c[1];
            loadTile = (int)(c[1] >> 24) & 7;
            for (int k = 0; k < 8; k++) {
                tile[k].set = 0;
            }
            if (op == R_LOADTLUT) {
                break;
            }
        } else if (op == X_TRI1 || op == X_TRI2 || op == X_VTX || op == 0x06 || op == 0xB8 || op == R_SETTIMG
                   || op == R_TEXRECT || op == R_TEXRECTFLIP) {
            break;
        }
    }
    if (loadTile < 0) {
        /* No load follows: nothing reads the image. The address is made even, as every pointer in the lists. */
        return (uintptr_t)gfx_mem(addr, 8) & ~(uintptr_t)1;
    }
    bytes = load_bytes(load, siz, width);
    if ((load[0] >> 24) == R_LOADTLUT) {
        kind = TEX_LUT(H.otherH) == 3 ? TEX_IA16PAL : TEX_SWAP16;
    } else {
        /* The tile that draws the texels: the one G_TEXTURE names, or the next, or any tile set after the load,
           that reads the TMEM address loaded. */
        u16 tmem = tile[loadTile].tmem;
        int pick = -1;

        if (texTile != loadTile && tile[texTile].tmem == tmem) {
            pick = texTile;
        } else if (texTile + 1 < 8 && texTile + 1 != loadTile && tile[texTile + 1].tmem == tmem) {
            pick = texTile + 1;
        } else {
            for (i = 0; i < 8; i++) {
                if (i != loadTile && tile[i].set && tile[i].tmem == tmem) {
                    pick = i;
                }
            }
        }
        renderSiz = pick >= 0 ? tile[pick].siz : (int)siz;
        kind = renderSiz == SIZ_16B ? TEX_SWAP16 : renderSiz == SIZ_32B ? TEX_SWAP32 : TEX_RAW;
    }
    if (!inLayer && kind == TEX_RAW) {
        /* Not drawn in this part: the game's texels where they can be passed as they are, without an entry. Swapped
           texels and palettes always go through an entry: a later part may draw with what this one loaded. */
        const u8 *src = gfx_mem(addr, bytes);

        if (src == NULL) {
            return 0;
        }
        if (!((uintptr_t)src & 1)) {
            return (uintptr_t)src;
        }
    }
    e = texture(addr, bytes, kind);
    if (e == NULL) {
        return 0;
    }
    if (inLayer) {
        H.out.textures++;
    }
    if ((load[0] >> 24) == R_LOADTLUT) {
        H.pal = (int)(e - H.tex);
    } else if (H.pal >= 0 && H.pal < H.texN && renderSiz >= 0 && renderSiz <= SIZ_8B) {
        /* A palette that changes must also drop the textures drawn with it: the host keys them by both. */
        TexEntry *pe = &H.tex[H.pal];
        const void *t = tex_ptr(e);
        int k;

        for (k = 0; k < pe->nUsers && pe->users[k] != t; k++) {
        }
        if (k == pe->nUsers && grow((void **)&pe->users, &pe->usersCap, k + 1, sizeof(const void *))) {
            pe->users[pe->nUsers++] = t;
        }
    }
    return (uintptr_t)tex_ptr(e);
}

/* ---- Commands. */

/* A G_SETTILE as the host reads it. With a TLUT the game's 4-bit and 8-bit textures are colour-indexed whatever their
   format says, and without one an RGBA format of that size is intensity (gfx_tex.c decodes them so). The host takes a
   tile's format as it is. */
static u32 set_tile(u32 w0) {
    u32 fmt = (w0 >> 21) & 7;
    u32 siz = (w0 >> 19) & 3;

    if (siz <= SIZ_8B) {
        if (TEX_LUT(H.otherH) != 0) {
            fmt = FMT_CI;
        } else if (fmt == FMT_RGBA) {
            fmt = FMT_I;
        }
    }
    return (w0 & ~(7u << 21)) | fmt << 21;
}

/* A change of the other mode's high word. When the TLUT mode changes, the tiles of 4-bit and 8-bit textures are set
   again with the format that it gives them. */
static void other_h(u32 v) {
    int changed = (TEX_LUT(v) != 0) != (TEX_LUT(H.otherH) != 0);
    int t;

    H.otherH = v;
    if (!changed) {
        return;
    }
    for (t = 0; t < 8; t++) {
        if (H.tile[t].w0 != 0 && H.tile[t].siz <= SIZ_8B) {
            emit(set_tile(H.tile[t].w0), H.tile[t].w1);
        }
    }
}

void gfx_host_command(u32 op, u32 w0, u32 w1, u32 pc) {
    int inLayer = (gfxLayer.kind & H.rq->mask) != 0;
    uintptr_t p;
    u32 set;

    if (op != X_MTX && op != X_MOVEWORD && op != X_SETGEOMETRYMODE && op != X_CLEARGEOMETRYMODE
        && op != X_SETOTHERMODE_H && op != X_SETOTHERMODE_L && op != X_NOOP) {
        H.inGroup = 0;
    }
    switch (op) {
        case X_MTX:
            matrix(w0, w1);
            break;
        case X_POPMTX:
            emit(F2_POPMTX << 24 | 0x380002u, 64);
            break;
        case X_VTX:
            if (inLayer) {
                u32 n = (w0 >> 10) & 0x3F;
                u32 v0 = ((w0 >> 16) & 0xFF) >> 1;
                const u8 *v = gfx_mem(gfx_dl_seg_addr(w1), n * 16);

                if (v != NULL && n > 0) {
                    emit(F2_VTX << 24 | n << 12 | ((v0 + n) & 0x7F) << 1, (uintptr_t)v);
                }
            }
            break;
        case X_MODIFYVTX:
            if (inLayer) {
                if (((w0 >> 16) & 0xFF) == 0x14) {
                    emit(F2_MODIFYVTX << 24 | (w0 & 0xFFFFFF), w1);
                } else {
                    H.out.skipped++;
                }
            }
            break;
        case X_CULLDL:
            if (inLayer) {
                emit(F2_CULLDL << 24 | (w0 & 0xFFFF), w1 & 0xFFFF);
            }
            break;
        case X_TRI1:
            if (inLayer) {
                emit_prim(F2_TRI1 << 24 | (w1 & 0xFFFFFF), 0);
                H.out.triangles++;
            }
            break;
        case X_TRI2:
            if (inLayer) {
                emit_prim(F2_TRI2 << 24 | (w0 & 0xFFFFFF), w1 & 0xFFFFFF);
                H.out.triangles += 2;
            }
            break;
        case X_SETGEOMETRYMODE:
            H.geom |= w1;
            emit(F2_GEOMETRYMODE << 24 | 0xFFFFFFu, fog_bits(geometry_bits(w1)));
            break;
        case X_CLEARGEOMETRYMODE:
            H.geom &= ~w1;
            set = geometry_bits(w1);
            if (H.rq->hostFog) {
                set &= ~GM_FOG_BIT;
            }
            emit(F2_GEOMETRYMODE << 24 | (~set & 0xFFFFFFu), 0);
            break;
        case X_SETOTHERMODE_L:
        case X_SETOTHERMODE_H: {
            u32 shift = (w0 >> 8) & 0xFF;
            u32 len = w0 & 0xFF;

            if (len > 0 && shift + len <= 32) {
                u32 mask = (len >= 32 ? 0xFFFFFFFFu : (1u << len) - 1) << shift;

                if (op == X_SETOTHERMODE_L) {
                    w1 = (w1 & ~mask) | (host_mode_l(w1) & mask);
                }
                emit((op == X_SETOTHERMODE_L ? F2_SETOTHERMODE_L : F2_SETOTHERMODE_H) << 24 | (32 - shift - len) << 8
                         | (len - 1),
                     w1);
                if (op == X_SETOTHERMODE_H) {
                    other_h((H.otherH & ~mask) | (w1 & mask));
                }
            }
            break;
        }
        case 0xEF: /* G_RDPSETOTHERMODE */
            emit(w0, host_mode_l(w1));
            other_h(w0 & 0xFFFFFF);
            break;
        case X_TEXTURE:
            H.texTile = (int)(w0 >> 8) & 7;
            H.texture[0] = F2_TEXTURE << 24 | (w0 & 0x3F00) | ((w0 & 0xFF) ? 2u : 0);
            H.texture[1] = w1;
            emit(H.texture[0], H.texture[1]);
            break;
        case X_MOVEWORD:
            move_word(w0, w1);
            break;
        case X_MOVEMEM:
            move_mem(w0, w1);
            break;
        case X_NOOP:
        case X_RDPHALF_1:
        case X_RDPHALF_2:
            break;
        case R_FILLRECT:
            if (inLayer) {
                H.out.skipped++;
            }
            break;
        case R_SETSCISSOR:
        case R_SETZIMG:
        case R_SETCIMG:
            break;
        case R_SETTILE: {
            int t = (int)(w1 >> 24) & 7;

            H.tile[t].fmt = (w0 >> 21) & 7;
            H.tile[t].siz = (w0 >> 19) & 3;
            H.tile[t].tmem = w0 & 0x1FF;
            H.tile[t].w0 = w0;
            H.tile[t].w1 = w1;
            emit(set_tile(w0), w1);
            break;
        }
        case R_SETTIMG:
            p = texture_image(w0, w1, pc, inLayer);
            if (p != 0) {
                emit(w0, p);
            } else {
                H.out.skipped++;
            }
            break;
        case 0xF8: /* G_SETFOGCOLOR */
            emit(w0, H.rq->hostFog ? H.rq->fogColor : w1);
            break;
        case 0xE6: /* the syncs */
        case 0xE7:
        case 0xE8:
        case 0xE9:
        case 0xEE: /* G_SETPRIMDEPTH */
        case R_LOADTLUT:
        case 0xF2: /* G_SETTILESIZE */
        case R_LOADBLOCK:
        case R_LOADTILE:
        case 0xF7: /* the colours and the combiner */
        case 0xF9:
        case 0xFA:
        case 0xFB:
        case 0xFC:
            emit(w0, w1);
            break;
        default:
            H.out.unknown++;
            break;
    }
}

/* ---- Texture rectangles. */

static int invert4(const float *m, float *o) {
    float inv[16];
    float det;
    int i;

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11]
             - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11]
             + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11]
             - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10]
              + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11]
             + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11]
             - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11]
             + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10]
              - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7]
             - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7]
             + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7]
              - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6]
              + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7]
             + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7]
             - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7]
              + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6]
              - m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        return 0;
    }
    for (i = 0; i < 16; i++) {
        o[i] = inv[i] / det;
    }
    return 1;
}

/* A texture rectangle of a requested part becomes two triangles in the game's world: its corners, taken back through
   the game's projection at the rectangle's depth z (clip space), are drawn with the host's projection, so it lies where
   the game put it and is hidden behind what is nearer. The corners are in pixels of the 320x240 screen, after the
   interpolation. */
void gfx_host_texrect(float ulx, float uly, float lrx, float lry, float z, int tile, float s, float t, float dsdx,
                      float dtdy, int flip) {
    float proj[16];
    float inv[16];
    float vs[2];
    float vt[2];
    float corner[4][3];
    float st[4][2];
    float centre[3] = { 0.0f, 0.0f, 0.0f };
    float model[16];
    u8 *vtx;
    void *mtx;
    void *back;
    int i;
    int k;

    H.inGroup = 0;
    if (!(gfxLayer.kind & H.rq->mask)) {
        return;
    }
    if (lrx <= ulx || lry <= uly) {
        /* Nothing to draw, as in gfx_dl.c. */
        return;
    }
    gfx_dl_view(proj, vs, vt);
    if (CYCLE_TYPE(gfxRdp.otherH) == CYC_COPY || CYCLE_TYPE(gfxRdp.otherH) == CYC_FILL || vs[0] == 0.0f || vs[1] == 0.0f
        || !invert4(proj, inv)) {
        H.out.skipped++;
        return;
    }
    /* As gfx_dl.c's tex_rect: the texels at the corners (x0, y0), (x1, y0), (x1, y1), (x0, y1). */
    if (!flip) {
        st[0][0] = s, st[0][1] = t;
        st[1][0] = s + (lrx - ulx) * dsdx, st[1][1] = t;
        st[2][0] = s + (lrx - ulx) * dsdx, st[2][1] = t + (lry - uly) * dtdy;
        st[3][0] = s, st[3][1] = t + (lry - uly) * dtdy;
    } else {
        st[0][0] = s, st[0][1] = t;
        st[1][0] = s, st[1][1] = t + (lrx - ulx) * dtdy;
        st[2][0] = s + (lry - uly) * dsdx, st[2][1] = t + (lrx - ulx) * dtdy;
        st[3][0] = s + (lry - uly) * dsdx, st[3][1] = t;
    }
    for (i = 0; i < 4; i++) {
        float x = (i == 1 || i == 2) ? lrx : ulx;
        float y = i >= 2 ? lry : uly;
        float n[4] = { (x - vt[0]) / vs[0], -(y - vt[1]) / vs[1], z, 1.0f };
        float wv[4];

        for (k = 0; k < 4; k++) {
            wv[k] = n[0] * inv[k] + n[1] * inv[4 + k] + n[2] * inv[8 + k] + n[3] * inv[12 + k];
        }
        if (wv[3] == 0.0f) {
            H.out.skipped++;
            return;
        }
        for (k = 0; k < 3; k++) {
            corner[i][k] = wv[k] / wv[3];
            centre[k] += corner[i][k] / 4.0f;
        }
    }
    /* The vertices in sixteenths of a unit about the centre, which the model matrix puts back. */
    vtx = (u8 *)arena_alloc(64);
    if (vtx == NULL) {
        return;
    }
    memset(vtx, 0, 64);
    for (i = 0; i < 4; i++) {
        s16 v[6];

        for (k = 0; k < 3; k++) {
            float d = (corner[i][k] - centre[k]) * 16.0f;

            if (!(d > -32767.0f && d < 32767.0f)) {
                H.out.skipped++;
                return;
            }
            v[k] = (s16)lroundf(d);
        }
        v[3] = 0;
        v[4] = (s16)lroundf(st[i][0] * 32.0f);
        v[5] = (s16)lroundf(st[i][1] * 32.0f);
        memcpy(vtx + i * 16, v, 12);
    }
    memset(model, 0, sizeof(model));
    model[0] = model[5] = model[10] = 1.0f / 16.0f;
    model[12] = centre[0];
    model[13] = centre[1];
    model[14] = centre[2];
    model[15] = 1.0f;
    mtx = fixed_matrix(model);
    /* The game's modelview, loaded again after the sprite. A push would be refused when the host's stack is full. */
    gfx_dl_modelview(model);
    back = fixed_matrix(model);
    if (mtx == NULL || back == NULL) {
        return;
    }
    /* No lighting, fog, culling or texture generation, as for a rectangle; the depth test as its other mode says. The
       host's fog stays: the blender reads the fog amount from the corners, as from any triangle. The corners go into
       vertex slots the game's microcode does not have, which keeps the game's own vertices. */
    emit(F2_GEOMETRYMODE << 24 | (~(0x00FF0000u | F2_GM_CULL_FRONT | F2_GM_CULL_BACK | 0x1u) & 0xFFFFFFu),
         ((gfxRdp.otherL & RM_Z_CMP) ? 0x1u : 0u) | (H.rq->hostFog ? GM_FOG_BIT : 0u));
    emit(F2_TEXTURE << 24 | (u32)tile << 8 | 2u, 0xFFFFFFFFu);
    emit(F2_MTX << 24 | 0x380000u | (F2_MTX_LOAD ^ F2_MTX_PUSH), (uintptr_t)mtx);
    emit(F2_VTX << 24 | 4u << 12 | (SPRITE_V0 + 4u) << 1, (uintptr_t)vtx);
    emit_prim(F2_TRI2 << 24 | (SPRITE_V0 * 2u) << 16 | (SPRITE_V0 * 2u + 2u) << 8 | (SPRITE_V0 * 2u + 4u),
              (SPRITE_V0 * 2u) << 16 | (SPRITE_V0 * 2u + 4u) << 8 | (SPRITE_V0 * 2u + 6u));
    emit(F2_MTX << 24 | 0x380000u | (F2_MTX_LOAD ^ F2_MTX_PUSH), (uintptr_t)back);
    /* The game's state again. */
    emit(F2_GEOMETRYMODE << 24, fog_bits(geometry_bits(H.geom)));
    if (H.texture[0] != 0) {
        emit(H.texture[0], H.texture[1]);
    }
    H.out.rects++;
}

/* ---- The entry. */

int gfx_host_build(const GfxHostRequest *rq, GfxHostResult *out) {
    int i;
    int p;

    memset(&H.out, 0, sizeof(H.out));
    memset(out, 0, sizeof(*out));
    H.rq = rq;
    H.passes = rq->split ? 2 : 1;
    H.failed = 0;
    H.inGroup = 0;
    H.texTile = 0;
    H.otherH = 0;
    H.geom = 0;
    H.texture[0] = H.texture[1] = 0;
    H.pal = -1;
    H.invalN = 0;
    memset(H.tile, 0, sizeof(H.tile));
    memset(H.light, 0, sizeof(H.light));
    for (p = 0; p < 2; p++) {
        H.entry[p].n = 0;
        for (i = 0; i < MAX_DEPTH; i++) {
            H.bufs[p][i].n = 0;
        }
    }
    arena_reset();
    evict();

    gfxLayer.on = 1;
    gfxLayer.marks = rq->marks;
    gfxLayer.count = rq->markCount;
    gfxLayer.mask = rq->mask;
    gfxLayer.noFog = rq->noFog;
    gfx_dl_set_wide(1.0f);
    gfx_mem_next_frame();
    gfx_interp_set(rq->t);
    gfx_interp_begin_draw();
    gfx_dl_host_walk(rq->tasks, rq->count);
    gfx_interp_end_draw();
    gfx_interp_set(-1.0f);
    gfxLayer.on = 0;
    gfxLayer.marks = NULL;

    /* The entry of each pass: the invalidations (in the first pass's only), the state the game's lists start from,
       then each task's list. */
    for (p = 0; p < H.passes; p++) {
        Buf calls = H.entry[p];
        int inval = p == 0 ? H.invalN : 0;
        int starts = START_COMMANDS + (rq->hostFog ? 1 : 0);
        int n = inval + starts + calls.n + 1;
        GfxHostCmd *start;
        GfxHostCmd *list = (GfxHostCmd *)arena_alloc((size_t)n * sizeof(GfxHostCmd));

        if (list == NULL || H.failed) {
            return 0;
        }
        for (i = 0; i < inval; i++) {
            list[i].w0 = (uintptr_t)HOST_INVALTEXCACHE << 24;
            list[i].w1 = (uintptr_t)H.inval[i];
        }
        start = list + inval;
        /* The host's state where the list runs is the host's own. The game's lists start from the state that
           gfx_dl_begin_frame gives weemitt's renderer: no geometry mode, other modes or combiner, white primitive and
           environment colours, an opaque black fog colour, and the texture off. With the host's fog, G_FOG, the
           fog's blend, and the host's fog colour and position instead. */
        start[0].w0 = (uintptr_t)F2_GEOMETRYMODE << 24;
        start[0].w1 = rq->hostFog ? GM_FOG_BIT : 0;
        start[1].w0 = (uintptr_t)0xEFu << 24; /* G_RDPSETOTHERMODE */
        start[1].w1 = rq->hostFog ? RM_FOG_SHADE_A : 0;
        start[2].w0 = (uintptr_t)0xFCu << 24; /* G_SETCOMBINE */
        start[2].w1 = 0;
        start[3].w0 = (uintptr_t)0xFAu << 24; /* G_SETPRIMCOLOR */
        start[3].w1 = 0xFFFFFFFFu;
        start[4].w0 = (uintptr_t)0xFBu << 24; /* G_SETENVCOLOR */
        start[4].w1 = 0xFFFFFFFFu;
        start[5].w0 = (uintptr_t)0xF8u << 24; /* G_SETFOGCOLOR */
        start[5].w1 = rq->hostFog ? rq->fogColor : 0x000000FFu;
        start[6].w0 = (uintptr_t)F2_TEXTURE << 24;
        start[6].w1 = 0;
        if (rq->hostFog) {
            start[7].w0 = (uintptr_t)(F2_MOVEWORD << 24 | MW_FOG << 16);
            start[7].w1 = rq->fogWord;
        }
        if (calls.n > 0) {
            memcpy(list + inval + starts, calls.c, (size_t)calls.n * sizeof(GfxHostCmd));
        }
        list[n - 1].w0 = (uintptr_t)F2_ENDDL << 24;
        list[n - 1].w1 = 0;
        H.out.commands += (unsigned int)(inval + starts) + 1;
        if (p == 0) {
            H.out.list = list;
        } else {
            H.out.xlu = list;
        }
    }
    *out = H.out;
    return 1;
}

void gfx_host_setup(unsigned int lo, unsigned int hi, uintptr_t base, unsigned int volatileLo, unsigned int volatileHi) {
    gfx_set_memory_range(lo, hi);
    gfx_set_memory_base(base);
    gfx_interp_set_volatile(volatileLo, volatileHi);
}

void gfx_host_record(const GfxTask *tasks, int count, int cut) {
    gfx_dl_set_wide(1.0f);
    gfx_mem_next_frame();
    /* The walk follows every list (the host culls with its own view): the recording does too, else a list culled in
       the game's view would have no record to blend. */
    gfx_dl_set_cull(0);
    gfx_dl_record(tasks, count, cut);
    gfx_dl_set_cull(1);
}

void gfx_host_shutdown(void) {
    int i;
    int p;

    for (i = 0; i < H.texN; i++) {
        free(H.tex[i].copy);
        free((void *)H.tex[i].users);
    }
    free(H.tex);
    arena_reset();
    while (H.spare != NULL) {
        Chunk *c = H.spare;

        H.spare = c->next;
        free(c);
    }
    for (p = 0; p < 2; p++) {
        for (i = 0; i < MAX_DEPTH; i++) {
            free(H.bufs[p][i].c);
        }
        free(H.entry[p].c);
    }
    free(H.inval);
    /* Everything else as at the start, the texture generations too: a new game counts its own from 0. */
    memset(&H, 0, sizeof(H));
    curDepth = 0;
}
