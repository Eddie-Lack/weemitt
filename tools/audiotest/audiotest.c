/*
 * audiotest: checks the native interpreter of the audio command lists
 * (src/audio/synth/rsp_audio.c).
 *
 *   audiotest           decodes known ADPCM frames with the interpreter and
 *                       checks them against a straightforward reference
 *                       decoder, and runs a volume ramp. Needs no ROM.
 *   audiotest ROM       also runs the ROM's own audio microcode on a model of
 *                       the RSP (rspsim.c) and checks that the interpreter
 *                       gives the same DMEM, the same states and the same
 *                       output for random commands, for lists like the
 *                       synthesizer's and for every wave of the game's sound
 *                       banks. Then it resamples a sine wave.
 *
 * build.sh builds and runs it. The program prints one line per check and
 * exits with 1 when any check fails.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsp_audio.h"
#include "rspsim.h"

/* What rsp_audio.c needs from the port. */
u8 *port_rom;
u32 port_rom_size;

void port_log(const char *fmt, ...) {
    (void)fmt;
}

static int failures;

static void check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

/* ------------------------------------------------------------------------
 * Random numbers (xorshift32): every run is the same
 * ---------------------------------------------------------------------- */

static uint32_t rng = 0x12345678;

static uint32_t rand32(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int32_t randRange(int32_t lo, int32_t hi) {
    return lo + (int32_t)(rand32() % (uint32_t)(hi - lo + 1));
}

static int16_t rand16(void) {
    return (int16_t)rand32();
}

/* ------------------------------------------------------------------------
 * The interpreter, driven with command lists in host memory
 * ---------------------------------------------------------------------- */

#define HOST_RAM_SIZE (4 << 20)
#define DMEM_BUFFERS 0x5C0 /* DMEM offset 0 of the commands */

static uint8_t *hram;      /* the interpreter's host memory */
static uint8_t *rdram;     /* the RSP model's RDRAM: the same contents, big-endian */
static uint32_t ramTop;    /* the next free byte of both */

static Acmd hostList[8192];
static uint32_t rspW1[8192]; /* w1 of each command as the RSP model sees it */
static int listLen;

static uint32_t ramAlloc(uint32_t bytes, uint32_t align) {
    uint32_t off = (ramTop + align - 1) & ~(align - 1);
    uint32_t end = off + bytes + 16;

    if (end > HOST_RAM_SIZE) {
        fprintf(stderr, "audiotest: out of test memory\n");
        exit(2);
    }
    /* Clear the block and the bytes around it in both memories: a DMA may read a few bytes past either end. */
    memset(hram + ramTop, 0, end - ramTop);
    memset(rdram + ramTop, 0, end - ramTop);
    ramTop = off + bytes;
    return off;
}

static void ramReset(void) {
    ramTop = 0x20000; /* below: the RSP model's copy of the command list */
}

static uint32_t hostAddr(uint32_t off) {
    return (uint32_t)(uintptr_t)(hram + off);
}

/* A command without an address. */
static void emit(uint32_t w0, uint32_t w1) {
    hostList[listLen].words.w0 = w0;
    hostList[listLen].words.w1 = w1;
    rspW1[listLen] = w1;
    listLen++;
}

/* A command whose w1 is an address in the test memory. */
static void emitAddr(uint32_t w0, uint32_t off) {
    hostList[listLen].words.w0 = w0;
    hostList[listLen].words.w1 = hostAddr(off);
    rspW1[listLen] = off;
    listLen++;
}

/* The same 16-bit value in both memories, each in its own byte order. */
static void putS16(uint32_t off, int16_t v) {
    memcpy(hram + off, &v, 2);
    rdram[off] = (uint8_t)((uint16_t)v >> 8);
    rdram[off + 1] = (uint8_t)v;
}

static void putByte(uint32_t off, uint8_t b) {
    hram[off] = b;
    rdram[off] = b;
}

static int16_t hostS16(uint32_t off) {
    int16_t v;

    memcpy(&v, hram + off, 2);
    return v;
}

static int16_t rspS16(uint32_t off) {
    return (int16_t)((rdram[off] << 8) | rdram[off + 1]);
}

static void runInterpreter(void) {
    rspAudioRunTask(hostList, listLen);
}

/* ------------------------------------------------------------------------
 * Part 1: ADPCM against a straightforward reference
 * ---------------------------------------------------------------------- */

/*
 * The reference decoder. One frame is a header byte (scale shift in the high
 * nibble, predictor in the low one) and 16 signed nibbles. Each half of 8
 * samples is predicted from the two samples before it, with the predictor's
 * two rows of 8 coefficients (11 fraction bits); the second row is also the
 * response to the earlier residuals of the half. The sum keeps 32 bits.
 * Shifts above 12 act as 12.
 */
static void referenceDecode(const int16_t book[][2][8], const int16_t hist[2], const uint8_t *frame, int16_t out[16]) {
    int shift = frame[0] >> 4 > 12 ? 12 : frame[0] >> 4;
    const int16_t *c0 = book[frame[0] & 0xF][0];
    const int16_t *c1 = book[frame[0] & 0xF][1];
    int half, i, j;

    for (half = 0; half < 2; half++) {
        int16_t last2 = half == 0 ? hist[0] : out[6];
        int16_t last1 = half == 0 ? hist[1] : out[7];
        int32_t r[8];

        for (i = 0; i < 8; i++) {
            int n = half * 8 + i;
            int nibble = (n & 1) ? frame[1 + n / 2] & 0xF : frame[1 + n / 2] >> 4;

            if (nibble >= 8) {
                nibble -= 16;
            }
            r[i] = nibble * (1 << shift);
        }
        for (i = 0; i < 8; i++) {
            int64_t total = (int64_t)c0[i] * last2 + (int64_t)c1[i] * last1 + (int64_t)r[i] * 2048;
            int32_t v;

            for (j = 0; j < i; j++) {
                total += (int64_t)c1[i - 1 - j] * r[j];
            }
            v = (int32_t)(uint32_t)total >> 11;
            out[half * 8 + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
}

/*
 * Decodes nframes frames with the interpreter, as the synthesizer asks for
 * them: codebook, compressed bytes, then A_ADPCM into DMEM 0x140. out gets the
 * 16 samples before the frames and then the decoded ones. state is the
 * voice's ADPCM state (16 samples), updated.
 */
static void interpreterDecode(const int16_t book[][2][8], int npred, const uint8_t *frames, int nframes, int16_t *state,
                              uint32_t flags, int16_t *out) {
    uint32_t bookOff = ramAlloc(npred * 32, 16);
    uint32_t dataOff = ramAlloc(nframes * 9 + 16, 16);
    uint32_t stateOff = ramAlloc(32, 16);
    uint32_t nbytes = nframes * 9;
    uint8_t *dmem = rspAudioDmem();
    int i;

    memcpy(hram + bookOff, book, npred * 32);
    memcpy(hram + dataOff, frames, nbytes);
    memcpy(hram + stateOff, state, 32);
    listLen = 0;
    emitAddr(0x0B000000 | (npred * 32), bookOff);
    emit(0x08000000, nbytes + 8 - (nbytes & 7));
    emitAddr(0x04000000, dataOff);
    emit(0x08000000, (0x140 << 16) | (nframes * 32));
    emitAddr(0x01000000 | (flags << 16), stateOff);
    runInterpreter();
    for (i = 0; i < 16 + nframes * 16; i++) {
        memcpy(&out[i], dmem + DMEM_BUFFERS + 0x140 + i * 2, 2);
    }
    memcpy(state, hram + stateOff, 32);
}

static int sameSamples(const int16_t *a, const int16_t *b, int n) {
    return memcmp(a, b, n * sizeof(int16_t)) == 0;
}

static void testAdpcmKnownFrames(void) {
    static int16_t book[8][2][8];
    static const uint8_t ramp[9] = { 0x20, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
    static const uint8_t ones[9] = { 0x01, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 };
    static const uint8_t loud[9] = { 0xC1, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77 };
    int16_t state[16], out[32], expect[16];
    int i;

    /* Predictor 0 is silent: the samples are the nibbles times 4 (shift 2). */
    memset(book, 0, sizeof(book));
    memset(state, 0, sizeof(state));
    for (i = 0; i < 16; i++) {
        expect[i] = (int16_t)((i < 8 ? i : i - 16) * 4);
    }
    ramReset();
    interpreterDecode(book, 2, ramp, 1, state, A_INIT, out);
    check(sameSamples(out + 16, expect, 16), "ADPCM known frame: no prediction, nibbles 0..7, -8..-1, shift 2");
    check(sameSamples(state, expect, 16), "ADPCM known frame: the state holds the frame's samples");

    /* Predictor 1 adds every residual to the last sample (c1 = 1.0 for every lag): a running sum from 100. */
    for (i = 0; i < 8; i++) {
        book[1][1][i] = 2048;
    }
    memset(state, 0, sizeof(state));
    state[15] = 100;
    for (i = 0; i < 16; i++) {
        expect[i] = (int16_t)(101 + i);
    }
    ramReset();
    interpreterDecode(book, 2, ones, 1, state, 0, out);
    check(sameSamples(out + 16, expect, 16), "ADPCM known frame: running sum from the state's last sample");

    /* The same with shift 12 and nibble 7 saturates at 32767 after the first sample. */
    state[15] = 100;
    for (i = 0; i < 16; i++) {
        expect[i] = (int16_t)(i == 0 ? 100 + 7 * 4096 : 32767);
    }
    ramReset();
    interpreterDecode(book, 2, loud, 1, state, 0, out);
    check(sameSamples(out + 16, expect, 16), "ADPCM known frame: saturation at 32767 (shift 12)");
}

static void testAdpcmReference(void) {
    static int16_t book[8][2][8];
    static uint8_t frames[24 * 9];
    int16_t state[16], before[16], zeros[16] = { 0 }, hist[2], expect[24 * 16], out[16 + 24 * 16];
    int iter, f, i, ok = 1;

    for (iter = 0; iter < 2000 && ok; iter++) {
        int npred = randRange(1, 8);
        int nframes = randRange(1, 24);
        int bound = iter % 3 == 0 ? 32767 : 4096;
        uint32_t flags = iter % 4 == 0 ? A_INIT : 0;

        for (i = 0; i < npred * 16; i++) {
            ((int16_t *)book)[i] = (int16_t)randRange(-bound, bound);
        }
        for (f = 0; f < nframes; f++) {
            frames[f * 9] = (uint8_t)((randRange(0, 15) << 4) | randRange(0, npred - 1));
            for (i = 1; i < 9; i++) {
                frames[f * 9 + i] = (uint8_t)rand32();
            }
        }
        for (i = 0; i < 16; i++) {
            state[i] = rand16();
            before[i] = state[i];
        }
        hist[0] = flags & A_INIT ? 0 : state[14];
        hist[1] = flags & A_INIT ? 0 : state[15];
        for (f = 0; f < nframes; f++) {
            referenceDecode((const int16_t(*)[2][8])book, hist, frames + f * 9, expect + f * 16);
            hist[0] = expect[f * 16 + 14];
            hist[1] = expect[f * 16 + 15];
        }
        ramReset();
        interpreterDecode((const int16_t(*)[2][8])book, npred, frames, nframes, state, flags, out);
        ok = sameSamples(out + 16, expect, nframes * 16) && sameSamples(state, expect + (nframes - 1) * 16, 16) &&
             sameSamples(out, flags & A_INIT ? zeros : before, 16);
    }
    check(ok, "ADPCM: 2000 random codebooks and frames match the reference decoder");
}

/* ------------------------------------------------------------------------
 * Part 2: the interpreter against the microcode on the RSP model
 * ---------------------------------------------------------------------- */

#define ROM_TEXT 0xBF9E0
#define ROM_TEXT_SIZE 0xE20

static RspSim rsp;

/* Fills DMEM above the microcode's data with the same random 16-bit values in both. */
static void fillDmem(void) {
    uint8_t *dmem = rspAudioDmem();
    uint32_t a;

    for (a = RSP_AUDIO_DATA_SIZE; a < RSP_AUDIO_DMEM_SIZE; a += 2) {
        int16_t v = rand16();

        memcpy(dmem + a, &v, 2);
        rsp.dmem[a] = (uint8_t)((uint16_t)v >> 8);
        rsp.dmem[a + 1] = (uint8_t)v;
    }
}

/* Sets a 16-bit value of DMEM in both. */
static void setDmem16(uint32_t a, int16_t v) {
    memcpy(rspAudioDmem() + a, &v, 2);
    rsp.dmem[a] = (uint8_t)((uint16_t)v >> 8);
    rsp.dmem[a + 1] = (uint8_t)v;
}

static void runModel(void) {
    int i;

    for (i = 0; i < listLen; i++) {
        uint32_t w0 = hostList[i].words.w0, w1 = rspW1[i];
        uint8_t *p = rdram + i * 8;

        p[0] = (uint8_t)(w0 >> 24);
        p[1] = (uint8_t)(w0 >> 16);
        p[2] = (uint8_t)(w0 >> 8);
        p[3] = (uint8_t)w0;
        p[4] = (uint8_t)(w1 >> 24);
        p[5] = (uint8_t)(w1 >> 16);
        p[6] = (uint8_t)(w1 >> 8);
        p[7] = (uint8_t)w1;
    }
    if (rspSimRunTask(&rsp, 0, listLen * 8) != 0) {
        fprintf(stderr, "audiotest: the RSP model stopped with an error\n");
        exit(2);
    }
}

static void runBoth(void) {
    runModel();
    runInterpreter();
}

/* DMEM bytes byteLo to byteHi hold compressed ADPCM bytes, which neither side swaps. */
static uint32_t byteLo, byteHi;

/*
 * Compares DMEM. A 16-bit value matches when it is the same number in both
 * byte orders (sample data), or, between byteLo and byteHi, when its bytes
 * are the same. Not compared: the segment table and the command buffer
 * (0x320-0x4BF, the interpreter has neither), the loop address that
 * A_SETLOOP stores (0x370-0x373, a host pointer in one and an RDRAM address in
 * the other), and the resampler's scratch and the task structure
 * (0xFB0-0xFFF). The states in that range are compared in memory.
 */
static int dmemDiffs(const char *what) {
    uint8_t *h = rspAudioDmem();
    uint32_t a;
    int diffs = 0;

    for (a = 0; a < RSP_AUDIO_DMEM_SIZE; a += 2) {
        int16_t hv, rv;

        if ((a >= 0x320 && a < 0x4C0) || (a >= 0xFB0)) {
            continue;
        }
        memcpy(&hv, h + a, 2);
        rv = (int16_t)((rsp.dmem[a] << 8) | rsp.dmem[a + 1]);
        if (hv != rv && !(a >= byteLo && a < byteHi && h[a] == rsp.dmem[a] && h[a + 1] == rsp.dmem[a + 1])) {
            if (diffs < 4) {
                printf("      %s: DMEM %03X: interpreter %04X, microcode %04X\n", what, a, (uint16_t)hv, (uint16_t)rv);
            }
            diffs++;
        }
    }
    return diffs;
}

/* Compares n 16-bit values of test memory. */
static int ramDiffs(const char *what, uint32_t off, int n) {
    int i, diffs = 0;

    for (i = 0; i < n; i++) {
        if (hostS16(off + i * 2) != rspS16(off + i * 2)) {
            if (diffs < 4) {
                printf("      %s: +%X: interpreter %04X, microcode %04X\n", what, i * 2, (uint16_t)hostS16(off + i * 2),
                       (uint16_t)rspS16(off + i * 2));
            }
            diffs++;
        }
    }
    return diffs;
}

static void putRandomS16(uint32_t off, int n, int16_t lo, int16_t hi) {
    int i;

    for (i = 0; i < n; i++) {
        putS16(off + i * 2, (int16_t)randRange(lo, hi));
    }
}

/* Loads n random samples into DMEM at a command offset, through A_LOADBUFF. */
static void loadRandomSamples(uint32_t dmemOff, int n, int16_t lo, int16_t hi) {
    uint32_t off = ramAlloc(n * 2 + 8, 16);

    putRandomS16(off, n, lo, hi);
    emit(0x08000000 | dmemOff, (uint32_t)(n * 2 + 7) & ~7u);
    emitAddr(0x04000000, off);
}

/* --- One test per command; each runs many random cases and counts the failing ones. --- */

static void testRandomAdpcm(void) {
    int iter, bad = 0;

    for (iter = 0; iter < 3000; iter++) {
        int npred = randRange(1, 8);
        int tsam = randRange(0, 330);
        int nframes = (tsam + 15) >> 4;
        uint32_t nbytes = nframes * 9;
        uint32_t align = randRange(0, 7);
        uint32_t flags = randRange(0, 3);
        uint32_t out = (uint32_t)randRange(0x10, 0x18) << 4;
        uint32_t bookOff, dataOff, stateOff, loopOff;
        int i, d;

        ramReset();
        listLen = 0;
        fillDmem();
        bookOff = ramAlloc(256, 16);
        dataOff = ramAlloc(nbytes + 32, 16) + align;
        stateOff = ramAlloc(32, 16);
        loopOff = ramAlloc(32, 16);
        putRandomS16(bookOff, npred * 16, iter & 1 ? -32768 : -6000, iter & 1 ? 32767 : 6000);
        for (i = 0; i < (int)nbytes; i++) {
            /*
             * Mostly headers that name a loaded predictor, sometimes one of the 8 that fit in DMEM_CODEBOOK.
             * Predictors 8 to 15 would read their coefficients from the compressed bytes, which the RSP reads
             * as big-endian 16-bit values and the interpreter as host-order ones. Valid data never names them.
             */
            putByte(dataOff + i, (uint8_t)(i % 9 == 0 ? (rand32() & 0xF0) | randRange(0, iter % 7 ? npred - 1 : 7)
                                                      : rand32()));
        }
        putRandomS16(stateOff, 16, -32768, 32767);
        putRandomS16(loopOff, 16, -32768, 32767);
        emitAddr(0x0B000000 | (npred * 32), bookOff);
        byteLo = byteHi = 0;
        if (nbytes > 0) {
            uint32_t load = nbytes + align;

            emit(0x08000000, load + 8 - (load & 7));
            emitAddr(0x04000000, dataOff - align);
            byteLo = DMEM_BUFFERS;
            byteHi = DMEM_BUFFERS + load + 8;
        }
        if (flags & A_LOOP) {
            emitAddr(0x0F000000, loopOff);
        }
        emit(0x08000000 | align, (out << 16) | ((tsam * 2) & 0xFFFF));
        emitAddr(0x01000000 | (flags << 16), stateOff);
        runBoth();
        d = dmemDiffs("ADPCM") + ramDiffs("ADPCM state", stateOff, 16);
        if (d) {
            printf("      case %d: flags %u, %d samples, %d predictors\n", iter, flags, tsam, npred);
            bad++;
            if (bad > 3) {
                break;
            }
        }
    }
    byteLo = byteHi = 0;
    check(bad == 0, "A_ADPCM: 3000 random cases (flags, alignment, counts, codebooks) match the microcode");
}

static void testRandomResample(void) {
    int iter, bad = 0;

    for (iter = 0; iter < 3000; iter++) {
        uint32_t flags = randRange(0, 3);
        uint32_t pitch = iter % 5 == 0 ? (uint32_t)randRange(0, 0xFFFF) : (uint32_t)randRange(0x2000, 0xFFFE);
        int count = iter % 3 == 0 ? randRange(0, 170) * 2 : randRange(1, 10) * 32;
        uint32_t in = 0x140 + randRange(0, 15) * 2;
        uint32_t out = 0;
        uint32_t stateOff;
        int d;

        ramReset();
        listLen = 0;
        fillDmem();
        stateOff = ramAlloc(32, 16);
        putRandomS16(stateOff, 16, -32768, 32767);
        /* The loop skip is always even: an odd one would read samples across two DMEM words. */
        putS16(stateOff + 0x0A, (int16_t)(randRange(0, 7) * 2));
        setDmem16(0xF9A, (int16_t)(randRange(0, 7) * 2));
        loadRandomSamples(0x140, 340, -32768, 32767);
        emit(0x08000000 | in, (out << 16) | (uint32_t)count);
        emitAddr(0x05000000 | (flags << 16) | pitch, stateOff);
        runBoth();
        d = dmemDiffs("RESAMPLE") + ramDiffs("RESAMPLE state", stateOff, 16);
        if (d) {
            printf("      case %d: flags %u, pitch %04X, count %d, in %X\n", iter, flags, pitch, count, in);
            if (++bad > 3) {
                break;
            }
        }
    }
    check(bad == 0, "A_RESAMPLE: 3000 random cases (pitch, phase, flags, counts) match the microcode");
}

/* A ramp rate as synGetRate makes them: mostly 0.x or 1.x per 8 samples, sometimes the jump to the target. */
static uint32_t randRate(void) {
    switch (randRange(0, 5)) {
        case 0:
            return 0x7FFFFFFF;
        case 1:
            return 0;
        case 2:
            return (uint32_t)rand32();
        default:
            return ((uint32_t)randRange(0, 1) << 16) | (uint32_t)randRange(0, 0xFFFF);
    }
}

static void testRandomEnvmixer(void) {
    int iter, bad = 0;

    for (iter = 0; iter < 3000; iter++) {
        uint32_t flags = (uint32_t)(randRange(0, 1) | (iter % 4 ? A_AUX : 0));
        int big = iter % 3 == 0;
        int count = iter % 4 == 0 ? randRange(1, 160) * 2 : randRange(1, 10) * 32;
        uint32_t rateL = randRate(), rateR = randRate();
        uint32_t stateOff;
        int i, d;

        ramReset();
        listLen = 0;
        fillDmem();
        stateOff = ramAlloc(80, 16);
        for (i = 0; i < 40; i++) {
            /* Volumes, fractions and parameters: realistic ones, or any. */
            putS16(stateOff + i * 2, big ? rand16() : (int16_t)randRange(0, 0x7FFF));
        }
        if (!big) {
            putS16(stateOff + 0x42, (int16_t)randRange(0, 1));
            putS16(stateOff + 0x48, (int16_t)randRange(0, 1));
        }
        loadRandomSamples(0x000, 168, -32768, 32767);
        if (!big) {
            for (i = 0; i < 4; i++) {
                static const uint32_t bufs[4] = { 0x440, 0x580, 0x6C0, 0x800 };

                loadRandomSamples(bufs[i], 168, -8000, 8000);
            }
        }
        emit(0x08000000 | 0x000, (0x440 << 16) | (uint32_t)count);
        emit(0x08080000 | 0x580, (0x6C0 << 16) | 0x800);
        if (flags & A_INIT) {
            emit(0x09060000 | (uint16_t)(big ? rand16() : randRange(0, 0x7FFF)), 0);
            emit(0x09040000 | (uint16_t)(big ? rand16() : randRange(0, 0x7FFF)), 0);
            emit(0x09020000 | (uint16_t)randRange(big ? -32768 : 0, 0x7FFF), rateL);
            emit(0x09000000 | (uint16_t)randRange(big ? -32768 : 0, 0x7FFF), rateR);
            emit(0x09080000 | (uint16_t)randRange(0, 0x7FFF), (uint32_t)randRange(0, 0x7FFF));
        }
        emitAddr(0x03000000 | (flags << 16), stateOff);
        runBoth();
        d = dmemDiffs("ENVMIXER") + ramDiffs("ENVMIXER state", stateOff, 40);
        if (d) {
            printf("      case %d: flags %u, count %d, rates %08X %08X\n", iter, flags, count, rateL, rateR);
            if (++bad > 3) {
                break;
            }
        }
    }
    check(bad == 0, "A_ENVMIXER: 3000 random cases (ramps, targets, flags, counts) match the microcode");
}

static void testRandomPolef(void) {
    int iter, bad = 0;

    for (iter = 0; iter < 2000; iter++) {
        uint32_t flags = randRange(0, 1);
        uint32_t gain = iter % 4 == 0 ? (uint32_t)randRange(0, 0xFFFF) : (uint32_t)randRange(0, 0x4000);
        int count = iter % 3 == 0 ? randRange(0, 160) * 2 : randRange(1, 10) * 32;
        uint32_t buf = 0x6C0, out = iter % 2 ? 0x6C0 : 0x140;
        uint32_t coefOff, stateOff;
        int i, d;

        ramReset();
        listLen = 0;
        fillDmem();
        coefOff = ramAlloc(32, 16);
        stateOff = ramAlloc(8, 16);
        for (i = 0; i < 16; i++) {
            putS16(coefOff + i * 2, iter % 3 ? (int16_t)(i < 8 ? 0 : randRange(0, 16384)) : rand16());
        }
        putRandomS16(stateOff, 4, -32768, 32767);
        loadRandomSamples(buf, 168, -32768, 32767);
        emit(0x08000000 | buf, (out << 16) | (uint32_t)count);
        emitAddr(0x0B000020, coefOff);
        emitAddr(0x0E000000 | (flags << 16) | gain, stateOff);
        runBoth();
        d = dmemDiffs("POLEF") + ramDiffs("POLEF state", stateOff, 4);
        if (d) {
            printf("      case %d: flags %u, gain %04X, count %d\n", iter, flags, gain, count);
            if (++bad > 3) {
                break;
            }
        }
    }
    check(bad == 0, "A_POLEF: 2000 random cases (gains, coefficients, in place or not) match the microcode");
}

/* A_MIXER, A_INTERLEAVE, A_DMEMMOVE, A_CLEARBUFF, A_LOADBUFF and A_SAVEBUFF. */
static void testRandomSimple(void) {
    int iter, bad = 0;

    for (iter = 0; iter < 3000; iter++) {
        int count = iter % 3 == 0 ? randRange(0, 320) * 2 : randRange(1, 10) * 32;
        uint32_t a = (uint32_t)randRange(0, 0x60) << 4, b = (uint32_t)randRange(0, 0x60) << 4;
        uint32_t saveOff, d = 0;
        int which = iter % 6;

        ramReset();
        listLen = 0;
        fillDmem();
        saveOff = ramAlloc(0x600, 16);
        putRandomS16(saveOff, 0x300, -32768, 32767);
        loadRandomSamples(0x400, 320, -32768, 32767);
        switch (which) {
            case 0:
                emit(0x08000000, (uint32_t)count);
                emit(0x0C000000 | (uint16_t)rand16(), (a << 16) | (iter % 2 ? a : b));
                break;
            case 1:
                emit(0x08000000, (a << 16) | (uint32_t)(count / 2));
                emit(0x0D000000, (0x440 << 16) | 0x580);
                break;
            case 2:
                /* Any even offsets, also overlapping ones. Odd ones would mix bytes of two samples. */
                emit(0x0A000000 | (a + randRange(0, 7) * 2), ((b + randRange(0, 7) * 2) << 16) | (uint32_t)count);
                break;
            case 3:
                emit(0x02000000 | (a + randRange(0, 7) * 2), (uint32_t)count);
                break;
            case 4:
                emit(0x08000000 | (a + randRange(0, 7)), (uint32_t)count);
                emitAddr(0x04000000, saveOff + randRange(0, 0x100) * 8);
                break;
            case 5:
                emit(0x08000000, ((a + randRange(0, 7)) << 16) | (uint32_t)count);
                emitAddr(0x06000000, saveOff + randRange(0, 0x100) * 8);
                break;
        }
        runBoth();
        d = dmemDiffs("simple") + ramDiffs("saved", saveOff, 0x300);
        if (d) {
            printf("      case %d: command %d, count %d, a %X, b %X\n", iter, which, count, a, b);
            if (++bad > 3) {
                break;
            }
        }
    }
    check(bad == 0, "A_MIXER, A_INTERLEAVE, A_DMEMMOVE, A_CLEARBUFF, A_LOADBUFF, A_SAVEBUFF: 3000 random cases");
}

/*
 * Lists like the synthesizer's (docs/audio-synth-map.md, sections 8 and 14):
 * voices that decode looping ADPCM, resample and mix, then a reverb with a
 * delay line, a low-pass section and feedback, the main mix and the
 * interleaved output. Several tasks run in a row, and the states carry over.
 */
typedef struct TestVoice {
    uint32_t state, resState, envState, loopState, wave, book;
    int npred, sample, first, waveFrames;
} TestVoice;

static void testFrames(void) {
    enum { VOICES = 6, FRAMES = 40, CHUNK = 160, DELAY = 24 * CHUNK };
    TestVoice voice[VOICES];
    uint32_t outOff, delayOff, lpCoef, lpState, inputPos;
    int frame, v, i, bad = 0;

    ramReset();
    fillDmem();
    for (v = 0; v < VOICES; v++) {
        TestVoice *t = &voice[v];

        t->npred = randRange(1, 4);
        t->book = ramAlloc(t->npred * 32, 16);
        putRandomS16(t->book, t->npred * 16, -5000, 5000);
        t->waveFrames = randRange(20, 60);
        t->wave = ramAlloc(t->waveFrames * 9 + 32, 16);
        for (i = 0; i < t->waveFrames * 9; i++) {
            putByte(t->wave + i, (uint8_t)(i % 9 == 0 ? (randRange(0, 11) << 4) | randRange(0, t->npred - 1) : rand32()));
        }
        t->state = ramAlloc(32, 16);
        t->resState = ramAlloc(32, 16);
        t->envState = ramAlloc(80, 16);
        t->loopState = ramAlloc(32, 16);
        putRandomS16(t->loopState, 16, -2000, 2000);
        t->sample = 0;
        t->first = 1;
    }
    outOff = ramAlloc(CHUNK * 4 * 8, 16);
    delayOff = ramAlloc(DELAY * 2, 16);
    putRandomS16(delayOff, DELAY, 0, 0);
    lpCoef = ramAlloc(32, 16);
    for (i = 0; i < 16; i++) {
        static const int16_t lp[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 10240, 6400, 4000, 2500, 1562, 976, 610, 381 };

        putS16(lpCoef + i * 2, lp[i]);
    }
    lpState = ramAlloc(8, 16);
    inputPos = 0;

    for (frame = 0; frame < FRAMES; frame++) {
        int count = CHUNK;

        listLen = 0;
        emit(0x07000000, 0);
        emit(0x02000440, count * 2);
        emit(0x02000580, count * 2);
        emit(0x020006C0, count * 2);
        emit(0x02000800, count * 2);
        for (v = 0; v < VOICES; v++) {
            TestVoice *t = &voice[v];
            uint32_t pitch = 0x4000 + v * 0x1800;
            int incount = (count * pitch) >> 15;
            int frames = (incount + 15) / 16 + 1;
            int start = (t->sample / 16) % (t->waveFrames - frames - 1);
            uint32_t addr = t->wave + start * 9;
            uint32_t align = addr & 7;
            uint32_t nbytes = frames * 9 + align;

            /* Decode as synDecodeChunk does: the codebook, the bytes from an 8-byte boundary, A_ADPCM. */
            emitAddr(0x0B000000 | (t->npred * 32), t->book);
            emit(0x08000000, nbytes + 8 - (nbytes & 7));
            emitAddr(0x04000000, addr - align);
            if (frame % 7 == 3) {
                emitAddr(0x0F000000, t->loopState);
            }
            emit(0x08000000 | align, (0x140 << 16) | (frames * 32));
            emitAddr(0x01000000 | ((t->first ? A_INIT : 0) | (frame % 7 == 3 ? A_LOOP : 0)) << 16, t->state);
            /* Resample from just after the 16 samples of the state. */
            emit(0x08000000 | (0x140 + 32), (0x000 << 16) | (count * 2));
            emitAddr(0x05000000 | ((t->first ? A_INIT : 0) << 16) | pitch, t->resState);
            /* Mix, with new volumes on the first frame and every 9 frames. */
            emit(0x08000000, (0x440 << 16) | (count * 2));
            emit(0x08080580, (0x6C0 << 16) | 0x800);
            if (t->first || frame % 9 == 0) {
                emit(0x09060000 | (uint16_t)randRange(0, 0x7FFF), 0);
                emit(0x09040000 | (uint16_t)randRange(0, 0x7FFF), 0);
                emit(0x09020000 | (uint16_t)randRange(0, 0x7FFF), randRate());
                emit(0x09000000 | (uint16_t)randRange(0, 0x7FFF), randRate());
                emit(0x09080000 | (uint16_t)randRange(0, 0x7FFF), (uint32_t)randRange(0, 0x7FFF));
                emitAddr(0x03090000, t->envState);
            } else {
                emitAddr(0x03080000, t->envState);
            }
            t->first = 0;
            t->sample += incount;
        }
        /* The reverb: mono input, into the delay line, one section with feedback and a low-pass filter. */
        emit(0x08000000, count * 2);
        emit(0x0C00DA83, 0x06C006C0);
        emit(0x0C005A82, 0x080006C0);
        emit(0x08000000, (0x6C0 << 16) | (count * 2));
        emitAddr(0x06000000, delayOff + inputPos * 2);
        emit(0x02000800, count * 2);
        {
            uint32_t tap = (inputPos + DELAY - 10 * CHUNK) % DELAY;

            emit(0x08000000 | 0x000, count * 2);
            emitAddr(0x04000000, delayOff + tap * 2);
            emit(0x08000000 | 0x140, count * 2);
            emitAddr(0x04000000, delayOff + ((tap + DELAY - 5 * CHUNK) % DELAY) * 2);
            emit(0x08000000, count * 2);
            emit(0x0C001000, (0x140 << 16) | 0x000);
            emit(0x0C00F000, (0x000 << 16) | 0x140);
            emit(0x08000000, (0x000 << 16) | (count * 2));
            emitAddr(0x06000000, delayOff + tap * 2);
            emit(0x08000140, (0x140 << 16) | (count * 2));
            emitAddr(0x0B000020, lpCoef);
            emitAddr(0x0E000000 | ((frame == 0 ? A_INIT : 0) << 16) | 6144, lpState);
            emit(0x08000000, count * 2);
            emit(0x0C003000, (0x140 << 16) | 0x800);
        }
        emit(0x0A000800, (0x6C0 << 16) | (count * 2));
        inputPos = (inputPos + count) % DELAY;
        /* The main mix and the output. */
        emit(0x08000000, count * 2);
        emit(0x0C007FFF, 0x06C00440);
        emit(0x0C007FFF, 0x08000580);
        emit(0x08000000, count * 2);
        emit(0x0D000000, 0x04400580);
        emit(0x08000000, count * 4);
        emitAddr(0x06000000, outOff + (frame % 8) * CHUNK * 4);
        runBoth();

        i = dmemDiffs("frame") + ramDiffs("output", outOff + (frame % 8) * CHUNK * 4, CHUNK * 2) +
            ramDiffs("delay line", delayOff, DELAY) + ramDiffs("low-pass state", lpState, 4);
        for (v = 0; v < VOICES; v++) {
            i += ramDiffs("ADPCM state", voice[v].state, 16) + ramDiffs("resampler state", voice[v].resState, 16) +
                 ramDiffs("envelope state", voice[v].envState, 40);
        }
        if (i) {
            printf("      task %d differs\n", frame);
            if (++bad > 2) {
                break;
            }
        }
    }
    check(bad == 0, "40 tasks like the synthesizer's (6 voices, reverb, output) match the microcode");
}

/* ------------------------------------------------------------------------
 * Part 3: what the commands do, on simple signals
 * ---------------------------------------------------------------------- */

/*
 * A sine of period 40 samples through the resampler at pitches 1.0 and 0.5.
 * The output follows the input 3 samples late (the 4 samples of history
 * come first) and differs by little: the filter is a gentle low-pass.
 */
static void testResampleSine(void) {
    static const uint32_t pitches[2] = { 0x8000, 0x4000 };
    uint8_t *dmem = rspAudioDmem();
    int p, i;

    for (p = 0; p < 2; p++) {
        uint32_t stateOff;
        double worst = 0;

        ramReset();
        listLen = 0;
        stateOff = ramAlloc(32, 16);
        for (i = 0; i < 200; i++) {
            int16_t v = (int16_t)lrint(10000 * sin(i * 2 * M_PI / 40));

            memcpy(dmem + DMEM_BUFFERS + 0x140 + i * 2, &v, 2);
        }
        emit(0x08000000 | 0x140, (0x000 << 16) | 320);
        emitAddr(0x05010000 | pitches[p], stateOff);
        runInterpreter();
        for (i = 0; i < 160; i++) {
            double pos = i * pitches[p] / 32768.0 - 3;
            int16_t v;

            memcpy(&v, dmem + DMEM_BUFFERS + i * 2, 2);
            if (pos >= 1 && fabs(v - 10000 * sin(pos * 2 * M_PI / 40)) > worst) {
                worst = fabs(v - 10000 * sin(pos * 2 * M_PI / 40));
            }
        }
        check(worst < 50, p == 0 ? "A_RESAMPLE: a sine at pitch 1.0 comes out 3 samples late, within 0.5%"
                                 : "A_RESAMPLE: a sine at pitch 0.5 comes out stretched, within 0.5%");
    }
}

/*
 * The ramp of docs/audio-synth-map.md, section 10.4: from 1 to 32767 at a
 * ratio of 1 + 17408 / 65536 per 8 samples. It reaches the target after 44
 * blocks and stays there. The right side falls from 20000 to 1000 at a ratio
 * of 0.9.
 */
static void testEnvelopeRamp(void) {
    uint32_t stateOff;
    int16_t left[8], right[8];
    int i, block, reachedLeft = -1, reachedRight = -1;

    ramReset();
    stateOff = ramAlloc(80, 16);
    for (block = 0; block < 60; block++) {
        listLen = 0;
        emit(0x02000000, 16);
        emit(0x02000440, 16);
        emit(0x02000580, 16);
        emit(0x08000000, (0x440 << 16) | 16);
        emit(0x08080580, (0x6C0 << 16) | 0x800);
        if (block == 0) {
            emit(0x09060001, 0);
            emit(0x09040000 | 20000, 0);
            emit(0x09020000 | 32767, (1 << 16) | 17408);
            emit(0x09000000 | 1000, 58982);
            emit(0x09080000 | 0x7FFF, 0);
            emitAddr(0x03090000, stateOff);
        } else {
            emitAddr(0x03080000, stateOff);
        }
        runInterpreter();
        for (i = 0; i < 8; i++) {
            left[i] = hostS16(stateOff + i * 2);
            right[i] = hostS16(stateOff + 0x20 + i * 2);
        }
        if (reachedLeft < 0 && left[0] == 32767) {
            reachedLeft = block;
        }
        if (reachedRight < 0 && right[0] == 1000) {
            reachedRight = block;
        }
    }
    check(reachedLeft >= 40 && reachedLeft <= 46 && left[7] == 32767,
          "A_ENVMIXER: a ramp from 1 to 32767 reaches the target after about 352 samples and stays");
    check(reachedRight > 0 && right[7] == 1000, "A_ENVMIXER: a falling ramp stops at its target");
}

/*
 * Every wave of the game's two sound banks, decoded through both in pieces
 * of up to 24 frames, as the load filter asks for them. The banks' layout is
 * in docs/audio-libmus-map.md, section 5.5.
 */
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void testRomWaves(const uint8_t *rom) {
    static const uint32_t pbanks[2] = { 0x4D58A0, 0x4DA9A0 };
    static const uint32_t wbanks[2] = { 0x4E3EE0, 0x607280 };
    int bank, waves = 0, frames = 0, bad = 0;

    for (bank = 0; bank < 2 && bad == 0; bank++) {
        const uint8_t *pb = rom + pbanks[bank];
        uint32_t count = be32(pb + 0x20), table = be32(pb + 0x2C), w;

        for (w = 0; w < count && bad == 0; w++) {
            const uint8_t *entry = pb + be32(pb + table + w * 4);
            const uint8_t *book = pb + be32(entry + 0x10);
            const uint8_t *data = rom + wbanks[bank] + be32(entry);
            uint32_t len = be32(entry + 4), bookBytes = be32(book) * be32(book + 4) * 16;
            uint32_t bookOff, dataOff, stateOff, done, i;

            ramReset();
            fillDmem();
            bookOff = ramAlloc(bookBytes, 16);
            for (i = 0; i < bookBytes; i += 2) {
                putS16(bookOff + i, (int16_t)((book[8 + i] << 8) | book[9 + i]));
            }
            dataOff = ramAlloc(len + 16, 16);
            for (i = 0; i < len; i++) {
                putByte(dataOff + i, data[i]);
            }
            stateOff = ramAlloc(32, 16);
            byteHi = 0;
            for (done = 0; done < len / 9 && bad == 0;) {
                uint32_t n = len / 9 - done < 24 ? len / 9 - done : 24;
                uint32_t addr = dataOff + done * 9, align = addr & 7, load = n * 9 + align;

                listLen = 0;
                emitAddr(0x0B000000 | bookBytes, bookOff);
                emit(0x08000000, load + 8 - (load & 7));
                emitAddr(0x04000000, addr - align);
                emit(0x08000000 | align, (0x140 << 16) | (n * 32));
                emitAddr(0x01000000 | ((done == 0 ? A_INIT : 0) << 16), stateOff);
                /* Bytes of earlier pieces stay in DMEM past this piece's. */
                byteLo = DMEM_BUFFERS;
                if (byteHi < DMEM_BUFFERS + load + 8) {
                    byteHi = DMEM_BUFFERS + load + 8;
                }
                runBoth();
                if (dmemDiffs("wave") + ramDiffs("wave state", stateOff, 16) != 0) {
                    printf("      bank %d, wave %u, frame %u\n", bank + 1, w, done);
                    bad++;
                }
                done += n;
                frames += n;
            }
            waves++;
        }
    }
    byteLo = byteHi = 0;
    printf("      %d waves, %d frames\n", waves, frames);
    check(bad == 0, "A_ADPCM: every wave of the ROM's two sound banks decodes as with the microcode");
}

static uint8_t *readFile(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n);
    if (buf == NULL || fread(buf, 1, n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

int main(int argc, char **argv) {
    hram = calloc(1, HOST_RAM_SIZE);
    rdram = calloc(1, HOST_RAM_SIZE);
    if (hram == NULL || rdram == NULL) {
        return 2;
    }

    testAdpcmKnownFrames();
    testAdpcmReference();
    testEnvelopeRamp();

    if (argc > 1) {
        uint32_t size;
        uint8_t *rom = readFile(argv[1], &size);

        if (rom == NULL || size < RSP_AUDIO_ROM_DATA + RSP_AUDIO_DATA_SIZE || rom[0] != 0x80) {
            fprintf(stderr, "audiotest: cannot read the ROM %s (a .z64 file)\n", argv[1]);
            return 2;
        }
        check(rspAudioInit(rom, size) == 0, "the ROM's microcode data holds the filter table");
        rsp.rdram = rdram;
        rsp.rdramSize = HOST_RAM_SIZE;
        rspSimLoad(&rsp, rom + ROM_TEXT, ROM_TEXT_SIZE, rom + RSP_AUDIO_ROM_DATA, RSP_AUDIO_DATA_SIZE);
        testRandomAdpcm();
        testRandomResample();
        testRandomEnvmixer();
        testRandomPolef();
        testRandomSimple();
        testFrames();
        testRomWaves(rom);
        testResampleSine();
        free(rom);
    } else {
        printf("(give the ROM to compare the interpreter with the microcode)\n");
    }
    printf("%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
