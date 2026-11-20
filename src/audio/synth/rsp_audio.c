/*
 * rsp_audio.c: the native interpreter of the audio command lists.
 *
 * On the N64, alAudioFrame builds a list of audio commands and the RSP runs
 * it with the audio microcode (aspMain). This file does the microcode's work
 * on the PC. See rsp_audio.h for the interface and docs/audio-synth-map.md,
 * section 15, for the commands.
 *
 * Source. Everything here was read from the microcode in the ROM: its code
 * (aspMainTextStart, ROM 0xBF9E0, 0xE20 bytes, run from IMEM 0x1080) and its
 * data (aspMainDataStart, ROM 0xF4E00). The comments give the IMEM address of
 * each command's handler. tools/audiotest checks this file against the
 * microcode itself, run on a model of the RSP.
 *
 * DMEM. The interpreter keeps a 4 KB data memory with the microcode's layout:
 *
 *   0x000-0x2BF  the microcode's data, copied in at the start of every task
 *                (0x0C0: the resampler's filter table)
 *   0x360-0x37F  the values that A_SETBUFF, A_SETVOL and A_SETLOOP set
 *   0x4C0-0x5BF  the coefficients that A_LOADADPCM loads
 *   0x5C0-       the sample buffers: DMEM offset 0 in a command is 0x5C0
 *   0xF90-0xFDF  where the resampler, the envelope mixer and the pole filter
 *                keep a voice's state while they run
 *
 * The microcode also keeps a segment table at 0x320 and copies the command
 * list to 0x380. The interpreter needs neither.
 *
 * Byte order. The RSP's memories are big-endian. Here every 16-bit value in
 * DMEM is in host order, as sample data in host memory is. The transfers
 * between DMEM and host memory copy bytes as they are. Compressed ADPCM data
 * is a byte stream and is read a byte at a time. The interpreter assumes that
 * every 16-bit value in DMEM is at an even address. Every list that the
 * synthesizer builds keeps to that.
 *
 * Fidelity. Each command makes the same changes to DMEM, to its state block
 * and to host memory as the microcode, with the microcode's rounding and
 * saturation. Where the microcode works on blocks of 8 or 16 samples, so does
 * the interpreter, and where it reads and writes the same buffer, it does so
 * in the same order. The few things that are not modelled are listed at the
 * commands concerned. None of them occurs in the lists that Glover's
 * synthesizer builds.
 */
PORT_C_BEGIN
#include <string.h>

#include "synth_internal.h"
#include "rsp_audio.h"

/* The port's ROM image and log (src/port/port.h). */
extern u8 *port_rom;
extern u32 port_rom_size;
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define DMEM_MASK (RSP_AUDIO_DMEM_SIZE - 1)

/* The microcode's DMEM layout. */
#define DMEM_FILTER_TABLE 0x0C0  /* 64 phases of 4 taps, s16 */
#define DMEM_PARAMS 0x360        /* see the PARAM_ offsets */
#define DMEM_CODEBOOK 0x4C0      /* A_LOADADPCM's coefficients: 16 s16 per predictor */
#define DMEM_BUFFERS 0x5C0       /* DMEM offset 0 of the commands */
#define DMEM_STAGE 0xF90         /* a voice's state while a command runs */
#define DMEM_WET_SCRATCH 0xFE0   /* the envelope mixer's wet output without A_AUX */

/* Offsets of the values in DMEM_PARAMS. */
#define PARAM_IN 0x00            /* A_SETBUFF: input buffer */
#define PARAM_OUT 0x02           /* A_SETBUFF: output buffer */
#define PARAM_COUNT 0x04         /* A_SETBUFF: byte count */
#define PARAM_VOL_LEFT 0x06      /* A_SETVOL A_VOL|A_LEFT */
#define PARAM_VOL_RIGHT 0x08     /* A_SETVOL A_VOL|A_RIGHT */
#define PARAM_DRY_RIGHT 0x0A     /* A_SETBUFF A_AUX: the envelope mixer's dry right output */
#define PARAM_WET_LEFT 0x0C      /* A_SETBUFF A_AUX */
#define PARAM_WET_RIGHT 0x0E     /* A_SETBUFF A_AUX */
#define PARAM_ENVELOPE 0x10      /* A_SETVOL A_RATE and A_AUX: the 8 values of EnvParam */
#define PARAM_LOOP 0x10          /* A_SETLOOP: the loop state's address, high half then low half */

/* The envelope mixer's 8 parameters, at PARAM_ENVELOPE and at 0x40 in its state. */
enum EnvParam {
    ENV_TARGET_LEFT,
    ENV_RATE_LEFT,               /* integer part of the ratio per 8 samples */
    ENV_FRAC_LEFT,               /* its fraction (u16) */
    ENV_TARGET_RIGHT,
    ENV_RATE_RIGHT,
    ENV_FRAC_RIGHT,
    ENV_DRY,
    ENV_WET,
    ENV_PARAM_COUNT
};

/* Byte offsets in the envelope mixer's state (80 bytes). */
#define ENV_STATE_LEFT 0x00      /* 8 s16: integer part of each lane's left volume */
#define ENV_STATE_LEFT_FRAC 0x10 /* 8 u16: fraction */
#define ENV_STATE_RIGHT 0x20
#define ENV_STATE_RIGHT_FRAC 0x30
#define ENV_STATE_PARAMS 0x40    /* the 8 EnvParam values */
#define ENV_STATE_SIZE 0x50

/* Byte offsets in the resampler's state (32 bytes). */
#define RES_STATE_HISTORY 0x00   /* 4 s16: the input samples before the next output */
#define RES_STATE_FRAC 0x08      /* u16: position of the next output between input samples */
#define RES_STATE_LOOP_SKIP 0x0A /* s16: used with A_LOOP */
#define RES_STATE_LOOP_DATA 0x10 /* 16 bytes: used with A_LOOP */
#define RES_STATE_SIZE 0x20

#define ADPCM_STATE_SIZE 0x20    /* 16 s16: the last 16 decoded samples */
#define POLEF_STATE_SIZE 0x08    /* 4 s16: the last 4 output samples */

/* ADPCM: 16 samples in 9 bytes, 2 history samples and 8 coefficients per order. */
#define ADPCM_FRAME_BYTES 9
#define ADPCM_COEF_ONE 2048      /* 1.0 in the codebook's fixed point (11 fraction bits) */
#define ADPCM_SHIFT_MAX 12       /* larger scale shifts in a frame header act as 12 */

static union {
    u8 b[RSP_AUDIO_DMEM_SIZE];
    s16 h[RSP_AUDIO_DMEM_SIZE / 2];
} dmem;

/* The microcode's data in host order, or zeros until rspAudioInit succeeds. */
static s16 ucodeData[RSP_AUDIO_DATA_SIZE / 2];

static s32 initDone;

/* ------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* An address in a command: the word of a pointer into the game's memory or port_win (src/port/ptr32.h). */
static void *hostAddress(u32 w) {
    return PORT_W2P(void, w);
}

/* A 16-bit value at an even DMEM byte address. */
static s16 load16(u32 addr) {
    return dmem.h[(addr & DMEM_MASK) >> 1];
}

static void store16(u32 addr, s16 value) {
    dmem.h[(addr & DMEM_MASK) >> 1] = value;
}

static u32 param(u32 offset) {
    return (u16)dmem.h[(DMEM_PARAMS + offset) >> 1];
}

static void setParam(u32 offset, u32 value) {
    dmem.h[(DMEM_PARAMS + offset) >> 1] = (s16)value;
}

static s16 clamp16(s32 x) {
    if (x > 0x7FFF) {
        return 0x7FFF;
    }
    if (x < -0x8000) {
        return -0x8000;
    }
    return (s16)x;
}

/*
 * The vector unit's accumulator holds 48 bits per lane. These two read a
 * result from it as the multiply instructions do: bits 47..16 saturated to
 * a signed 16-bit value, or bits 15..0, saturated to 0 or 0xFFFF when bits
 * 47..16 do not fit in 16 signed bits.
 */
static s64 wrap48(s64 acc) {
    return (s64)((u64)acc << 16) >> 16;
}

static s16 accHigh(s64 acc) {
    s64 x = wrap48(acc) >> 16;

    if (x > 0x7FFF) {
        return 0x7FFF;
    }
    if (x < -0x8000) {
        return -0x8000;
    }
    return (s16)x;
}

static u16 accLow(s64 acc) {
    s64 x;

    acc = wrap48(acc);
    x = acc >> 16;
    if (x > 0x7FFF || x < -0x8000) {
        return acc < 0 ? 0 : 0xFFFF;
    }
    return (u16)acc;
}

/*
 * The fractional multiply of the vector unit (VMULF), which every mixing
 * step starts with: a * b * 2 in 16.16, rounded, saturated.
 */
static s16 mulFrac(s16 a, s16 b) {
    return accHigh((s64)a * b * 2 + 0x8000);
}

/* out * 0x7FFF + in * gain, both as 1.15 fractions, rounded and saturated: the microcode's mixing step. */
static s16 mixSample(s16 out, s16 in, s16 gain) {
    return accHigh((s64)out * 0x7FFF * 2 + 0x8000 + (s64)in * gain * 2);
}

/*
 * The RSP's DMA. It moves whole 8-byte units: it ignores the low 3 bits of
 * the DMEM address and rounds the length up to a multiple of 8. A length over
 * 4096 bytes is split into rows, as the microcode's length register value
 * (count - 1) asks. The RSP would also ignore the low 3 bits of the RDRAM
 * address. Every address that the synthesizer gives is a multiple of 8. The
 * interpreter uses the host address as it is: a host buffer need not be
 * aligned.
 */
static void dmaTransfer(u32 dmemAddr, u8 *host, u32 count, s32 toHost) {
    u32 lengthReg = count - 1;
    u32 length = ((lengthReg & 0xFFF) | 7) + 1;
    u32 rows = ((lengthReg >> 12) & 0xFF) + 1;
    u32 skip = (lengthReg >> 20) & 0xFF8;
    u32 mem = dmemAddr & DMEM_MASK & ~7u;
    u32 row, i;

    for (row = 0; row < rows; row++) {
        for (i = 0; i < length; i++) {
            if (toHost) {
                host[i] = dmem.b[(mem + i) & DMEM_MASK];
            } else {
                dmem.b[(mem + i) & DMEM_MASK] = host[i];
            }
        }
        mem += length;
        host += length + skip;
    }
}

static void dmaRead(u32 dmemAddr, const void *src, u32 count) {
    dmaTransfer(dmemAddr, (u8 *)src, count, 0);
}

static void dmaWrite(void *dst, u32 dmemAddr, u32 count) {
    dmaTransfer(dmemAddr, (u8 *)dst, count, 1);
}

/* Copies bytes within DMEM in the order given. */
static void copyBytes(u32 dst, u32 src, u32 count) {
    u32 i;

    for (i = 0; i < count; i++) {
        dmem.b[(dst + i) & DMEM_MASK] = dmem.b[(src + i) & DMEM_MASK];
    }
}

/* ------------------------------------------------------------------------
 * Buffers and transfers
 * ---------------------------------------------------------------------- */

/* A_SETBUFF (handler 0x12EC). With A_AUX it sets the envelope mixer's other three outputs. */
static void cmdSetBuffer(u32 w0, u32 w1) {
    if ((w0 >> 16) & A_AUX) {
        setParam(PARAM_DRY_RIGHT, w0 + DMEM_BUFFERS);
        setParam(PARAM_WET_LEFT, (w1 >> 16) + DMEM_BUFFERS);
        setParam(PARAM_WET_RIGHT, w1 + DMEM_BUFFERS);
    } else {
        setParam(PARAM_IN, w0 + DMEM_BUFFERS);
        setParam(PARAM_OUT, (w1 >> 16) + DMEM_BUFFERS);
        setParam(PARAM_COUNT, w1);
    }
}

/* A_CLEARBUFF (0x11DC): clears count bytes, rounded up to a multiple of 16. */
static void cmdClearBuffer(u32 w0, u32 w1) {
    u32 addr = (w0 & 0xFFFF) + DMEM_BUFFERS;
    s32 count = w1 & 0xFFFF;
    u32 i;

    while (count > 0) {
        for (i = 0; i < 16; i++) {
            dmem.b[(addr + i) & DMEM_MASK] = 0;
        }
        addr += 16;
        count -= 16;
    }
}

/* A_LOADBUFF (0x1214): host memory to the input buffer, A_SETBUFF's count bytes. */
static void cmdLoadBuffer(u32 w1) {
    u32 count = param(PARAM_COUNT);

    if (count != 0) {
        dmaRead(param(PARAM_IN), hostAddress(w1), count);
    }
}

/* A_SAVEBUFF (0x1254): the output buffer to host memory. */
static void cmdSaveBuffer(u32 w1) {
    u32 count = param(PARAM_COUNT);

    if (count != 0) {
        dmaWrite(hostAddress(w1), param(PARAM_OUT), count);
    }
}

/*
 * A_LOADADPCM (0x1294): count bytes of coefficients to DMEM_CODEBOOK. The
 * microcode does not check for a count of 0, which would start a transfer of
 * about 1 MB. The interpreter does nothing then.
 */
static void cmdLoadAdpcm(u32 w0, u32 w1) {
    u32 count = w0 & 0xFFFF;

    if (count != 0) {
        dmaRead(DMEM_CODEBOOK, hostAddress(w1), count);
    }
}

/* A_DMEMMOVE (0x140C): copies count bytes, rounded up to 16, a block of 16 bytes at a time. */
static void cmdDmemMove(u32 w0, u32 w1) {
    u32 src = (w0 & 0xFFFF) + DMEM_BUFFERS;
    u32 dst = (w1 >> 16) + DMEM_BUFFERS;
    s32 count = w1 & 0xFFFF;
    u8 block[16];
    u32 i;

    while (count > 0) {
        for (i = 0; i < 16; i++) {
            block[i] = dmem.b[(src + i) & DMEM_MASK];
        }
        for (i = 0; i < 16; i++) {
            dmem.b[(dst + i) & DMEM_MASK] = block[i];
        }
        src += 16;
        dst += 16;
        count -= 16;
    }
}

/*
 * A_INTERLEAVE (0x138C): the left and right buffers to the output buffer as
 * stereo pairs, 8 pairs at a time. The count is A_SETBUFF's, in bytes of one
 * input buffer.
 */
static void cmdInterleave(u32 w1) {
    u32 left = (w1 >> 16) + DMEM_BUFFERS;
    u32 right = (w1 & 0xFFFF) + DMEM_BUFFERS;
    u32 out = param(PARAM_OUT);
    s32 count = param(PARAM_COUNT);
    s16 l[8], r[8];
    s32 i;

    while (count > 0) {
        for (i = 0; i < 8; i++) {
            l[i] = load16(left + i * 2);
            r[i] = load16(right + i * 2);
        }
        for (i = 0; i < 8; i++) {
            store16(out + i * 4, l[i]);
            store16(out + i * 4 + 2, r[i]);
        }
        left += 16;
        right += 16;
        out += 32;
        count -= 16;
    }
}

/*
 * A_MIXER (0x1E24): out = out * 0x7FFF / 0x8000 + in * gain / 0x8000, 16
 * samples at a time, rounded and saturated. The gain is signed. The microcode
 * reads the next block's input between the two halves of its stores. The
 * interpreter keeps that order.
 */
static void cmdMixer(u32 w0, u32 w1) {
    s16 gain = (s16)(w0 & 0xFFFF);
    u32 in = (w1 >> 16) + DMEM_BUFFERS;
    u32 out = (w1 & 0xFFFF) + DMEM_BUFFERS;
    s32 count = param(PARAM_COUNT);
    s16 x[16], y[16];
    s32 i;

    if (count == 0) {
        return;
    }
    for (i = 0; i < 16; i++) {
        y[i] = load16(out + i * 2);
        x[i] = load16(in + i * 2);
    }
    for (;;) {
        for (i = 0; i < 8; i++) {
            store16(out + i * 2, mixSample(y[i], x[i], gain));
        }
        count -= 32;
        in += 32;
        for (i = 8; i < 16; i++) {
            y[i] = mixSample(y[i], x[i], gain);
        }
        for (i = 0; i < 16; i++) {
            x[i] = load16(in + i * 2);
        }
        for (i = 8; i < 16; i++) {
            store16(out + i * 2, y[i]);
        }
        out += 32;
        for (i = 0; i < 16; i++) {
            y[i] = load16(out + i * 2);
        }
        if (count <= 0) {
            break;
        }
    }
}

/* ------------------------------------------------------------------------
 * ADPCM
 * ---------------------------------------------------------------------- */

/* One compressed frame, as the decoder has read it. */
typedef struct AdpcmFrame {
    s16 residual[16];   /* the 16 nibbles, scaled by the header's shift */
    s16 coef0[8];       /* the predictor's coefficients for the sample two back */
    s16 coef1[8];       /* for the sample one back; also the impulse response for the residuals */
} AdpcmFrame;

/* A 4-bit two's complement value: -8 to 7. */
static s32 signedNibble(u32 nibble) {
    return (s32)(nibble ^ 8) - 8;
}

/*
 * Reads the frame at DMEM addr: a header byte (scale shift in the high
 * nibble, predictor in the low nibble), then 16 signed nibbles, high nibble
 * first. The predictor's 16 coefficients come from DMEM_CODEBOOK. Nothing
 * checks the predictor against the codebook's size. Predictors 8 to 15 would
 * take their coefficients from the sample buffers after the codebook. Valid
 * data never names them.
 */
static void adpcmReadFrame(AdpcmFrame *frame, u32 addr) {
    u32 header = dmem.b[addr & DMEM_MASK];
    u32 shift = header >> 4;
    u32 book = DMEM_CODEBOOK + (header & 0xF) * 32;
    s32 i;

    if (shift > ADPCM_SHIFT_MAX) {
        shift = ADPCM_SHIFT_MAX;
    }
    for (i = 0; i < 8; i++) {
        u32 byte = dmem.b[(addr + 1 + i) & DMEM_MASK];

        frame->residual[i * 2] = (s16)(signedNibble(byte >> 4) * (1 << shift));
        frame->residual[i * 2 + 1] = (s16)(signedNibble(byte & 0xF) * (1 << shift));
    }
    for (i = 0; i < 8; i++) {
        frame->coef0[i] = load16(book + i * 2);
        frame->coef1[i] = load16(book + 16 + i * 2);
    }
}

/*
 * Decodes 8 samples from the two samples before them (prev2, prev1) and 8
 * residuals. The microcode computes all 8 at once: each sample's prediction
 * uses only the two samples before the block, and the coefficients for the
 * sample one back double as the response to the earlier residuals of the
 * block. The sum is 32 bits (it wraps), then >> 11 and saturated.
 */
static void adpcmDecodeHalf(const AdpcmFrame *frame, const s16 *residual, s16 prev2, s16 prev1, s16 *out) {
    s32 i, k;

    for (i = 0; i < 8; i++) {
        u32 sum = (u32)((s32)frame->coef0[i] * prev2) + (u32)((s32)frame->coef1[i] * prev1) +
                  (u32)((s32)residual[i] * ADPCM_COEF_ONE);

        for (k = 0; k < i; k++) {
            sum += (u32)((s32)frame->coef1[i - 1 - k] * residual[k]);
        }
        out[i] = clamp16((s32)sum >> 11);
    }
}

/*
 * A_ADPCM (0x1470). Decodes A_SETBUFF's count bytes of samples (rounded up to
 * frames of 16) from the input buffer to the output buffer. The output starts
 * with the 16 samples before the first frame: zeros with A_INIT, else the
 * loop state with A_LOOP, else the voice's state. Then come the new frames.
 * At the end the last 16 samples of the output go to the voice's state.
 * The output buffer must be 16-byte aligned. It always is.
 */
static void cmdAdpcm(u32 w0, u32 w1) {
    u32 flags = (w0 >> 16) & 0xFF;
    void *state = hostAddress(w1);
    u32 in = param(PARAM_IN);
    u32 out = param(PARAM_OUT);
    s32 count = param(PARAM_COUNT);
    AdpcmFrame frame, next;
    s16 samples[16];
    s16 prev2, prev1;
    s32 i;

    for (i = 0; i < ADPCM_STATE_SIZE; i += 2) {
        store16(out + i, 0);
    }
    if (!(flags & A_INIT)) {
        if (flags & A_LOOP) {
            dmaRead(out, hostAddress((param(PARAM_LOOP) << 16) | param(PARAM_LOOP + 2)), ADPCM_STATE_SIZE);
        } else {
            dmaRead(out, state, ADPCM_STATE_SIZE);
        }
    }
    prev2 = load16(out + 28);
    prev1 = load16(out + 30);
    out += ADPCM_STATE_SIZE;

    if (count != 0) {
        adpcmReadFrame(&frame, in);
        do {
            /* The microcode reads the next frame before it stores this one. */
            in += ADPCM_FRAME_BYTES;
            adpcmReadFrame(&next, in);
            adpcmDecodeHalf(&frame, frame.residual, prev2, prev1, samples);
            adpcmDecodeHalf(&frame, frame.residual + 8, samples[6], samples[7], samples + 8);
            for (i = 0; i < 16; i++) {
                store16(out + i * 2, samples[i]);
            }
            prev2 = samples[14];
            prev1 = samples[15];
            out += 32;
            count -= 32;
            frame = next;
        } while (count > 0);
    }
    dmaWrite(state, out - ADPCM_STATE_SIZE, ADPCM_STATE_SIZE);
}

/* A_SETLOOP (0x144C): the address of the loop state that A_ADPCM reads with A_LOOP. */
static void cmdSetLoop(u32 w1) {
    /* The microcode keeps the address in the same DMEM word as the left target and rate of A_SETVOL. */
    setParam(PARAM_LOOP, w1 >> 16);
    setParam(PARAM_LOOP + 2, w1);
}

/* ------------------------------------------------------------------------
 * Resampler
 * ---------------------------------------------------------------------- */

/*
 * One output sample of the resampler: 4 input samples from DMEM addr through
 * the 4 taps of a phase of the filter table. Each product is rounded and
 * saturated, then the pairs are added with saturation.
 */
static s16 resampleTap(u32 addr, u32 phase) {
    u32 taps = DMEM_FILTER_TABLE + phase * 8;
    s16 p0 = mulFrac(load16(addr), load16(taps));
    s16 p1 = mulFrac(load16(addr + 2), load16(taps + 2));
    s16 p2 = mulFrac(load16(addr + 4), load16(taps + 4));
    s16 p3 = mulFrac(load16(addr + 6), load16(taps + 6));

    return clamp16(clamp16(p0 + p1) + clamp16(p2 + p3));
}

/*
 * A_RESAMPLE (0x187C). Makes A_SETBUFF's count bytes of output (rounded up to
 * blocks of 8 samples, at least one block) from the input buffer at a pitch
 * of (w0 & 0xFFFF) / 0x8000 input samples per output sample.
 *
 * The position is 16.16 fixed point and starts at the state's fraction. The 4
 * samples of the state go just before the input. Output sample n uses the 4
 * input samples from 4 before the input, plus the integer part of the
 * position, and the filter phase from the top 6 bits of the fraction.
 * Positions advance by 2 * pitch per output sample. They are exact. At the
 * end the state gets the 4 input samples and the fraction for the next
 * output.
 *
 * A_LOOP is not in Glover's lists. With it, the 16 bytes of the state's loop
 * data go just before the input, and the history goes the state's loop skip
 * bytes before that. The interpreter does the same.
 *
 * The microcode also writes the next block's input and table addresses to
 * DMEM 0xFB0-0xFCF and the state's address to 0xFD0. Nothing reads them after
 * the command, and the interpreter does not write them.
 */
static void cmdResample(u32 w0, u32 w1) {
    u32 flags = (w0 >> 16) & 0xFF;
    u32 step = (w0 & 0xFFFF) * 2;
    u8 *state = (u8 *)hostAddress(w1);
    s32 in = (s16)param(PARAM_IN);
    u32 out = param(PARAM_OUT);
    s32 count = (s16)param(PARAM_COUNT);
    s32 base, next;
    u32 frac, pos, skip;
    s16 block[8];
    s32 i;

    if (flags & A_INIT) {
        store16(DMEM_STAGE + RES_STATE_FRAC, 0);
        for (i = 0; i < 8; i += 2) {
            store16(DMEM_STAGE + RES_STATE_HISTORY + i, 0);
        }
    } else {
        dmaRead(DMEM_STAGE, state, RES_STATE_SIZE);
    }
    base = in;
    if (flags & A_LOOP) {
        copyBytes(in - 16, DMEM_STAGE + RES_STATE_LOOP_DATA, 16);
        base = in - load16(DMEM_STAGE + RES_STATE_LOOP_SKIP);
    }
    base -= 8;
    frac = (u16)load16(DMEM_STAGE + RES_STATE_FRAC);
    copyBytes(base, DMEM_STAGE + RES_STATE_HISTORY, 8);

    pos = frac;
    for (;;) {
        for (i = 0; i < 8; i++) {
            block[i] = resampleTap(base + (pos >> 16) * 2, (pos & 0xFFFF) >> 10);
            pos += step;
        }
        for (i = 0; i < 8; i++) {
            store16(out + i * 2, block[i]);
        }
        out += 16;
        count -= 16;
        if (count <= 0) {
            break;
        }
    }

    /* The state for the next command. */
    next = (s16)(base + (pos >> 16) * 2);
    store16(DMEM_STAGE + RES_STATE_FRAC, (s16)pos);
    copyBytes(DMEM_STAGE + RES_STATE_HISTORY, next, 8);
    next += 8;
    skip = (next - in) & 0xF;
    next -= skip;
    store16(DMEM_STAGE + RES_STATE_LOOP_SKIP, skip != 0 ? 16 - skip : 0);
    copyBytes(DMEM_STAGE + RES_STATE_LOOP_DATA, next, 16);
    dmaWrite(state, DMEM_STAGE, RES_STATE_SIZE);
}

/* ------------------------------------------------------------------------
 * Envelope mixer
 * ---------------------------------------------------------------------- */

/*
 * The volume of one side for a block of 8 samples: one 16.16 value per
 * sample. The integer parts are saturated to s16 and to the target; the
 * fractions are not changed by the target.
 */
typedef struct EnvRamp {
    s16 vol[8];
    u16 frac[8];
} EnvRamp;

/*
 * The first block after A_INIT. The volume runs from start towards
 * start * rate over the block, in equal steps: sample i gets start plus
 * (i + 1) / 8 of the change, the last 0xFFFF / 0x10000 of it.
 */
static void envStart(EnvRamp *ramp, s16 start, s16 rate, u16 rateFrac) {
    s64 acc = (s64)start * rateFrac + (s64)start * rate * 65536;
    s16 deltaVol = clamp16(accHigh(acc) - start);
    u16 deltaFrac = accLow(acc);
    s32 i;

    for (i = 0; i < 8; i++) {
        u32 weight = i < 7 ? 0x2000 * (i + 1) : 0xFFFF;

        acc = (((u32)deltaFrac * weight) >> 16) + (s64)weight * deltaVol + (s64)start * 65536;
        ramp->vol[i] = accHigh(acc);
        ramp->frac[i] = accLow(acc);
    }
}

/* The next block: every sample's volume times the ratio rate.rateFrac, as the microcode's four multiplies give it. */
static void envStep(EnvRamp *ramp, s16 rate, u16 rateFrac) {
    s32 i;

    for (i = 0; i < 8; i++) {
        s64 acc = (((u32)ramp->frac[i] * rateFrac) >> 16) + (s64)ramp->vol[i] * rateFrac +
                  (s64)ramp->frac[i] * rate + (s64)ramp->vol[i] * rate * 65536;

        ramp->vol[i] = accHigh(acc);
        ramp->frac[i] = accLow(acc);
    }
}

/*
 * Stops the ramp at the target. With a ratio of 1 or more the microcode keeps
 * the smaller value, compared as unsigned (VCL); else the larger, compared as
 * signed (VGE). Both instructions also read carry flags that earlier vector
 * instructions leave. Those flags are clear here: in Glover's lists a
 * resampler command, which clears them, runs before every envelope mixer.
 */
static void envClamp(EnvRamp *ramp, s16 target, s16 rate) {
    s32 i;

    for (i = 0; i < 8; i++) {
        if (rate > 0) {
            if ((u16)ramp->vol[i] >= (u16)target) {
                ramp->vol[i] = target;
            }
        } else if (ramp->vol[i] < target) {
            ramp->vol[i] = target;
        }
    }
}

static void envSave(const EnvRamp *ramp, u32 addr) {
    s32 i;

    for (i = 0; i < 8; i++) {
        store16(addr + i * 2, ramp->vol[i]);
        store16(addr + 16 + i * 2, (s16)ramp->frac[i]);
    }
}

static void envLoad(EnvRamp *ramp, u32 addr) {
    s32 i;

    for (i = 0; i < 8; i++) {
        ramp->vol[i] = load16(addr + i * 2);
        ramp->frac[i] = (u16)load16(addr + 16 + i * 2);
    }
}

static void load8(s16 *dst, u32 addr) {
    s32 i;

    for (i = 0; i < 8; i++) {
        dst[i] = load16(addr + i * 2);
    }
}

static void store8(u32 addr, const s16 *src) {
    s32 i;

    for (i = 0; i < 8; i++) {
        store16(addr + i * 2, src[i]);
    }
}

/* Mixes 8 input samples into a dry and a wet buffer at one side's volume. */
static void envMix(s16 *dry, s16 *wet, const s16 *in, const EnvRamp *ramp, const s16 *params) {
    s32 i;

    for (i = 0; i < 8; i++) {
        dry[i] = mixSample(dry[i], in[i], mulFrac(ramp->vol[i], params[ENV_DRY]));
        wet[i] = mixSample(wet[i], in[i], mulFrac(ramp->vol[i], params[ENV_WET]));
    }
}

/*
 * A_ENVMIXER (0x1B38). Mixes A_SETBUFF's input into the dry outputs (main
 * left and right) and the wet outputs (aux left and right), 8 samples at a
 * time, with a volume ramp for each side.
 *
 * With A_INIT the ramps start from A_SETVOL's volumes and the parameters
 * come from A_SETVOL; the first block is made apart from the others, and at
 * least 2 blocks are made. Without it, the ramps and the parameters come from
 * the state, and at least 1 block is made. Each block's volume is the last
 * block's times the ratio, stopped at the target.
 *
 * Without A_AUX both wet outputs go to one scratch area of DMEM. Nothing
 * reads it.
 */
static void cmdEnvmixer(u32 w0, u32 w1) {
    u32 flags = (w0 >> 16) & 0xFF;
    u8 *state = (u8 *)hostAddress(w1);
    s16 params[ENV_PARAM_COUNT];
    EnvRamp left, right;
    s16 in[8], mainL[8], wetL[8], mainR[8], wetR[8];
    u32 inAddr = param(PARAM_IN);
    u32 mainLAddr = param(PARAM_OUT);
    u32 mainRAddr = param(PARAM_DRY_RIGHT);
    u32 wetLAddr = param(PARAM_WET_LEFT);
    u32 wetRAddr = param(PARAM_WET_RIGHT);
    u32 wetStep = 16;
    s32 count = (s16)param(PARAM_COUNT);
    s32 i;

    if (!(flags & A_AUX)) {
        wetLAddr = wetRAddr = DMEM_WET_SCRATCH;
        wetStep = 0;
    }
    if (flags & A_INIT) {
        for (i = 0; i < ENV_PARAM_COUNT; i++) {
            params[i] = (s16)param(PARAM_ENVELOPE + i * 2);
        }
        envStart(&left, (s16)param(PARAM_VOL_LEFT), params[ENV_RATE_LEFT], params[ENV_FRAC_LEFT]);
        envClamp(&left, params[ENV_TARGET_LEFT], params[ENV_RATE_LEFT]);
        load8(in, inAddr);
        load8(mainL, mainLAddr);
        load8(wetL, wetLAddr);
        envMix(mainL, wetL, in, &left, params);
        store8(mainLAddr, mainL);
        store8(wetLAddr, wetL);
        envStart(&right, (s16)param(PARAM_VOL_RIGHT), params[ENV_RATE_RIGHT], params[ENV_FRAC_RIGHT]);
        envClamp(&right, params[ENV_TARGET_RIGHT], params[ENV_RATE_RIGHT]);
        load8(mainR, mainRAddr);
        load8(wetR, wetRAddr);
        envMix(mainR, wetR, in, &right, params);
        store8(mainRAddr, mainR);
        store8(wetRAddr, wetR);
        count -= 16;
        inAddr += 16;
        mainLAddr += 16;
        mainRAddr += 16;
        wetLAddr += wetStep;
        wetRAddr += wetStep;
    } else {
        dmaRead(DMEM_STAGE, state, ENV_STATE_SIZE);
        envLoad(&left, DMEM_STAGE + ENV_STATE_LEFT);
        envLoad(&right, DMEM_STAGE + ENV_STATE_RIGHT);
        load8(params, DMEM_STAGE + ENV_STATE_PARAMS);
    }

    envStep(&left, params[ENV_RATE_LEFT], params[ENV_FRAC_LEFT]);
    for (;;) {
        envClamp(&left, params[ENV_TARGET_LEFT], params[ENV_RATE_LEFT]);
        envStep(&right, params[ENV_RATE_RIGHT], params[ENV_FRAC_RIGHT]);
        load8(in, inAddr);
        load8(mainL, mainLAddr);
        load8(wetL, wetLAddr);
        envSave(&left, DMEM_STAGE + ENV_STATE_LEFT);
        envMix(mainL, wetL, in, &left, params);
        load8(mainR, mainRAddr);
        load8(wetR, wetRAddr);
        store8(mainLAddr, mainL);
        store8(wetLAddr, wetL);
        envClamp(&right, params[ENV_TARGET_RIGHT], params[ENV_RATE_RIGHT]);
        /* The microcode computes the left volume of the next block here, also after the last block. */
        envStep(&left, params[ENV_RATE_LEFT], params[ENV_FRAC_LEFT]);
        envMix(mainR, wetR, in, &right, params);
        store8(mainRAddr, mainR);
        store8(wetRAddr, wetR);
        count -= 16;
        inAddr += 16;
        mainLAddr += 16;
        mainRAddr += 16;
        wetLAddr += wetStep;
        wetRAddr += wetStep;
        if (count <= 0) {
            break;
        }
    }
    envSave(&right, DMEM_STAGE + ENV_STATE_RIGHT);
    store8(DMEM_STAGE + ENV_STATE_PARAMS, params);
    dmaWrite(state, DMEM_STAGE, ENV_STATE_SIZE);
}

/* A_SETVOL (0x1328): the values of the next A_ENVMIXER with A_INIT. */
static void cmdSetVolume(u32 w0, u32 w1) {
    u32 flags = (w0 >> 16) & 0xFF;

    if (flags & A_AUX) {
        setParam(PARAM_ENVELOPE + ENV_DRY * 2, w0);
        setParam(PARAM_ENVELOPE + ENV_WET * 2, w1);
    } else if (flags & A_VOL) {
        setParam((flags & A_LEFT) ? PARAM_VOL_LEFT : PARAM_VOL_RIGHT, w0);
    } else if (flags & A_LEFT) {
        setParam(PARAM_ENVELOPE + ENV_TARGET_LEFT * 2, w0);
        setParam(PARAM_ENVELOPE + ENV_RATE_LEFT * 2, w1 >> 16);
        setParam(PARAM_ENVELOPE + ENV_FRAC_LEFT * 2, w1);
    } else {
        setParam(PARAM_ENVELOPE + ENV_TARGET_RIGHT * 2, w0);
        setParam(PARAM_ENVELOPE + ENV_RATE_RIGHT * 2, w1 >> 16);
        setParam(PARAM_ENVELOPE + ENV_FRAC_RIGHT * 2, w1);
    }
}

/* ------------------------------------------------------------------------
 * Pole filter
 * ---------------------------------------------------------------------- */

/*
 * A_POLEF (0x170C): a recursive filter of order 2 over A_SETBUFF's count
 * bytes (rounded up to blocks of 8), from the input buffer to the output
 * buffer. The reverb runs it in place as its low-pass filter.
 *
 * Its coefficients are the 16 values at DMEM_CODEBOOK that A_LOADADPCM
 * loaded: c0 for the output two back, c1 for the output one back. The
 * microcode scales c1 by gain / 0x4000 (with 16 bits of gain * 4) and writes
 * the result over c1 in DMEM. The scaled values are the response to the
 * earlier inputs of a block, like ADPCM's coefficients. Each output is
 * (c0 * y2 + c1 * y1 + those + gain * x) >> 14, saturated.
 *
 * The state is the last 4 outputs. With A_INIT the microcode clears only the
 * first 2 of the 4 history values in its staging area, and the 2 it uses are
 * whatever the last command left there. The interpreter keeps the same
 * staging area and gets the same values.
 */
static void cmdPoleFilter(u32 w0, u32 w1) {
    u32 flags = (w0 >> 16) & 0xFF;
    s16 gain = (s16)(w0 & 0xFFFF);
    u16 gain4 = (u16)((w0 & 0xFFFF) << 2);
    u8 *state = (u8 *)hostAddress(w1);
    u32 in = param(PARAM_IN);
    u32 out = param(PARAM_OUT);
    s32 count = param(PARAM_COUNT);
    s16 c0[8], c1[8], scaled[8], x[8], next[8], y[8];
    s16 prev2, prev1;
    s32 i, k;

    if (count == 0) {
        return;
    }
    store16(DMEM_STAGE, 0);
    store16(DMEM_STAGE + 2, 0);
    if (!(flags & A_INIT)) {
        dmaRead(DMEM_STAGE, state, POLEF_STATE_SIZE);
    }
    load8(c1, DMEM_CODEBOOK + 16);
    for (i = 0; i < 8; i++) {
        scaled[i] = accHigh((s64)c1[i] * gain4);
    }
    store8(DMEM_CODEBOOK + 16, scaled);
    load8(c0, DMEM_CODEBOOK);
    prev2 = load16(DMEM_STAGE + 4);
    prev1 = load16(DMEM_STAGE + 6);
    load8(x, in);

    for (;;) {
        for (i = 0; i < 8; i++) {
            u32 sum = (u32)((s32)c0[i] * prev2) + (u32)((s32)c1[i] * prev1) + (u32)((s32)x[i] * gain);

            for (k = 0; k < i; k++) {
                sum += (u32)((s32)scaled[i - 1 - k] * x[k]);
            }
            y[i] = clamp16((s32)sum >> 14);
        }
        in += 16;
        count -= 16;
        /* The microcode reads the next block before it stores this one. */
        load8(next, in);
        store8(out, y);
        out += 16;
        prev2 = y[6];
        prev1 = y[7];
        if (count <= 0) {
            break;
        }
        memcpy(x, next, sizeof(x));
    }
    dmaWrite(state, out - POLEF_STATE_SIZE, POLEF_STATE_SIZE);
}

/* ------------------------------------------------------------------------
 * Tasks
 * ---------------------------------------------------------------------- */

/*
 * Checks the data that rspAudioInit read. The resampler's table is 64 phases
 * of 4 taps; phase 63 is phase 0 backwards, and so on, and each phase's taps
 * add up to about 0x8000 (a gain of 1). The table at offset 0x10 holds the
 * 16 command handlers' addresses, which lie in the microcode's code (IMEM
 * 0x1080-0x1E9F).
 */
static s32 checkData(const s16 *data) {
    const s16 *table = data + DMEM_FILTER_TABLE / 2;
    s32 phase, tap, sum;

    for (phase = 0; phase < 64; phase++) {
        sum = 0;
        for (tap = 0; tap < 4; tap++) {
            if (table[phase * 4 + tap] != table[(63 - phase) * 4 + 3 - tap]) {
                return -1;
            }
            sum += table[phase * 4 + tap];
        }
        if (sum < 0x8000 - 64 || sum > 0x8000 + 64) {
            return -1;
        }
    }
    for (tap = 0; tap < 16; tap++) {
        u16 handler = (u16)data[0x10 / 2 + tap];

        if (handler < 0x1080 || handler >= 0x1EA0) {
            return -1;
        }
    }
    return 0;
}

s32 rspAudioInit(const u8 *rom, u32 romSize) {
    s16 data[RSP_AUDIO_DATA_SIZE / 2];
    u32 i;

    if (rom == NULL || romSize < RSP_AUDIO_ROM_DATA + RSP_AUDIO_DATA_SIZE) {
        return -1;
    }
    for (i = 0; i < RSP_AUDIO_DATA_SIZE / 2; i++) {
        data[i] = (s16)((rom[RSP_AUDIO_ROM_DATA + i * 2] << 8) | rom[RSP_AUDIO_ROM_DATA + i * 2 + 1]);
    }
    if (checkData(data) != 0) {
        return -1;
    }
    memcpy(ucodeData, data, sizeof(ucodeData));
    return 0;
}

u8 *rspAudioDmem(void) {
    return dmem.b;
}

void rspAudioRunTask(const Acmd *cmdList, s32 cmdCount) {
    static s32 reported;
    s32 n;

    /* The RSP loads the microcode's data at the start of each task. The rest of DMEM keeps what was there. */
    memcpy(dmem.b, ucodeData, sizeof(ucodeData));

    for (n = 0; n < cmdCount; n++) {
        u32 w0 = cmdList[n].words.w0;
        u32 w1 = cmdList[n].words.w1;

        /* The microcode ignores the top bit of the command byte. */
        switch ((w0 >> 24) & 0x7F) {
            case A_SPNOOP:
                break;
            case A_ADPCM:
                cmdAdpcm(w0, w1);
                break;
            case A_CLEARBUFF:
                cmdClearBuffer(w0, w1);
                break;
            case A_ENVMIXER:
                cmdEnvmixer(w0, w1);
                break;
            case A_LOADBUFF:
                cmdLoadBuffer(w1);
                break;
            case A_RESAMPLE:
                cmdResample(w0, w1);
                break;
            case A_SAVEBUFF:
                cmdSaveBuffer(w1);
                break;
            case A_SEGMENT:
                /* Glover's lists set only segment 0, to 0. Addresses are words, read with PORT_W2P. */
                break;
            case A_SETBUFF:
                cmdSetBuffer(w0, w1);
                break;
            case A_SETVOL:
                cmdSetVolume(w0, w1);
                break;
            case A_DMEMMOVE:
                cmdDmemMove(w0, w1);
                break;
            case A_LOADADPCM:
                cmdLoadAdpcm(w0, w1);
                break;
            case A_MIXER:
                cmdMixer(w0, w1);
                break;
            case A_INTERLEAVE:
                cmdInterleave(w1);
                break;
            case A_POLEF:
                cmdPoleFilter(w0, w1);
                break;
            case A_SETLOOP:
                cmdSetLoop(w1);
                break;
            default:
                /* The microcode would jump to an address read past its handler table. */
                if (!reported) {
                    reported = 1;
                    port_log("rsp_audio: unknown audio command %08lX %08lX", (unsigned long)w0,
                             (unsigned long)w1);
                }
                break;
        }
    }
}

/*
 * The synthesizer's entry point (synth_internal.h). The ROM's alAudioFrame
 * only built the list; the native one runs it with this function before it
 * returns. The first call reads the microcode's data from the ROM image.
 */
void synExecuteCommands(Acmd *cmdList, s32 cmdCount) {
    if (!initDone) {
        initDone = 1;
        if (rspAudioInit(port_rom, port_rom_size) != 0) {
            port_log("rsp_audio: the ROM has no audio microcode data at %X; all voices are silent",
                     RSP_AUDIO_ROM_DATA);
        }
    }
    rspAudioRunTask(cmdList, cmdCount);
}
PORT_C_END
