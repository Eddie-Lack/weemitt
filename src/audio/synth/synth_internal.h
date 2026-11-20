/*
 * synth_internal.h: shared declarations for the native reimplementation of
 * libaudio's synthesizer in Glover (USA).
 *
 * Every layout here was read from the ROM's machine code. libaudio occupies
 * VRAM 0x801C7EB0-0x801CC004 (gloveworld asm/C0800.s, lines 10045-14779).
 * docs/audio-synth-map.md explains each field, each function and each audio
 * command. The offsets in the comments are the ROM's offsets.
 *
 * The port builds for 32-bit x86 (gcc -m32) with 4-byte pointers. libmus
 * allocates these structures from musHeap, which is in port_ram, with the
 * ROM's sizes. So every structure here keeps the ROM's size and offsets. The
 * checks at the end of this file and in mus_internal.h make the build fail
 * when one does not.
 *
 * libmus (src/audio/mus) uses the same definitions: mus_internal.h
 * includes this header. There is one definition of each structure.
 *
 * Byte order (see the map, section "Data and byte order"):
 *  - Audio commands (Acmd) are two host-order words, w0 and w1. Build them
 *    with shifts. Never use C bit fields for them: their bit order is the
 *    compiler's, not the RSP's.
 *  - An address in a command is the word of a pointer into the game's
 *    memory or port_win (src/port/ptr32.h): PORT_P2W makes it and the
 *    interpreter reads it with PORT_W2P. A Ptr32 member's (u32) cast gives
 *    its word. The ROM masks addresses with 0x1FFFFFFF or calls
 *    osVirtualToPhysical. The port does neither.
 *  - ADPCM codebooks and loop states come from the pbank, which musBankInit
 *    converted to host-order s16. The low-pass coefficients are host-order
 *    s16 too. The interpreter reads all of them as host s16.
 *  - Wave data stays in ROM byte order. The DMA routine returns a pointer
 *    into port_rom.
 */
#ifndef SYNTH_INTERNAL_H
#define SYNTH_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <PR/ultratypes.h>
#include <PR/libaudio.h> /* ALSynth, ALVoice, ALPlayer, ALHeap; and through abi.h, Acmd and the states */

/* ------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/*
 * Parameter ids. A filter's setParam function takes one as paramID. An
 * ALParam (a timed change in an envelope mixer's list) holds one as type.
 * The two uses share one numbering. Glover's ROM never posts
 * SYN_FREE_VOICE or SYN_START_VOICE_PARAMS: alSynFreeVoice and
 * alSynStartVoiceParams are not in it. Id 10 is not used.
 */
enum SynParamId {
    SYN_FREE_VOICE = 0,          /* ALParam: move the PVoice to the lame list */
    SYN_SET_SOURCE = 1,          /* every filter: the filter it pulls from */
    SYN_ADD_SOURCE = 2,          /* buses: add a source */
    SYN_ADD_UPDATE = 3,          /* envelope mixer: append an ALParam to its list */
    SYN_RESET = 4,               /* envelope mixer, resampler, load filter: stop and rewind */
    SYN_SET_WAVETABLE = 5,       /* load filter: the ALWaveTable to play */
    SYN_SET_DRAM_OUT = 6,        /* save filter: the output buffer */
    SYN_SET_PITCH = 7,           /* resampler: the ratio (the f32's bits in the pointer) */
    SYN_SET_UNITY_PITCH = 8,     /* resampler: copy the samples, no resampling */
    SYN_START = 9,               /* envelope mixer, resampler: start playing */
    SYN_SET_VOLUME = 11,         /* ALParam: volume and ramp length */
    SYN_SET_PAN = 12,            /* ALParam: pan 0..127 */
    SYN_START_VOICE_PARAMS = 13, /* ALParam: start with pitch, volume, pan and fx mix (SynStartParams) */
    SYN_START_VOICE = 14,        /* ALParam: start the wave table in the value */
    SYN_STOP_VOICE = 15,         /* ALParam: stop (sends SYN_RESET down the voice) */
    SYN_SET_FX_MIX = 16          /* ALParam: reverb send 0..127 */
};

/* ALFilter.type. Written by the constructors and never read. 2 is not used. */
enum SynFilterType {
    SYN_FILTER_LOAD = 0,
    SYN_FILTER_RESAMPLE = 1,
    SYN_FILTER_SAVE = 3,
    SYN_FILTER_ENVMIX = 4,
    SYN_FILTER_FX = 5,
    SYN_FILTER_AUX_BUS = 6,
    SYN_FILTER_MAIN_BUS = 7
};

/*
 * alFxParamHdl's parameter ids: SYN_FX_PARAM_BASE + section * 8 + field.
 * Nothing in Glover's ROM calls alFxParamHdl.
 */
#define SYN_FX_PARAM_BASE 2
#define SYN_FX_PARAMS_PER_SECTION 8
enum SynFxField {
    SYN_FX_INPUT = 0,
    SYN_FX_OUTPUT = 1,
    SYN_FX_FBCOEF = 2,
    SYN_FX_FFCOEF = 3,
    SYN_FX_GAIN = 4,
    SYN_FX_CHORUS_RATE = 5,
    SYN_FX_CHORUS_DEPTH = 6,
    SYN_FX_LOWPASS = 7
};

/*
 * Sample buffers in the RSP's data memory, as byte offsets in the commands.
 * Every buffer holds SYN_MAX_OUT_SAMPLES 16-bit samples (0x140 bytes). Some
 * offsets have two uses at different times in a frame.
 */
#define SYN_DMEM_DECODER_IN 0x000    /* compressed wave data (load filter) */
#define SYN_DMEM_RESAMPLER_OUT 0x000 /* the envelope mixer's input (its inp starts here) */
#define SYN_DMEM_DECODER_OUT 0x140   /* decoded samples (the resampler's inp starts here) */
#define SYN_DMEM_MAIN_L 0x440        /* dry mix, left */
#define SYN_DMEM_MAIN_R 0x580        /* dry mix, right */
#define SYN_DMEM_AUX_L 0x6C0         /* reverb send, left; the reverb's mono input and its output */
#define SYN_DMEM_AUX_R 0x800         /* reverb send, right; the reverb's output while it runs */
#define SYN_DMEM_FX_TEMP0 0x140      /* reverb: first output tap buffer (buff1 at the start) */
#define SYN_DMEM_FX_TEMP1 0x000      /* reverb: first input tap buffer (buff2 at the start) */
#define SYN_DMEM_FX_CHORUS 0x280     /* reverb: chorus input before resampling */
#define SYN_DMEM_SAVE_OUT 0x000      /* interleaved stereo output (4 bytes per sample) */
#define SYN_DMEM_END 0x940           /* end of the highest buffer */

/* Samples per chunk of a frame (ALSynth.maxOutSamples, AL_MAX_RSP_SAMPLES). */
#define SYN_MAX_OUT_SAMPLES 160

/* ADPCM: 16 samples per frame (ADPCMFSIZE in abi.h) in 9 bytes. */
#define SYN_ADPCM_FRAME_BYTES 9
#define SYN_ADPCM_FRAME_SHIFT 4 /* log2 of ADPCMFSIZE */

/* Bytes of the low-pass coefficients that A_LOADADPCM loads for A_POLEF. */
#define SYN_LOWPASS_COEF_BYTES 32
#define SYN_LOWPASS_COEFS 16

/* The equal-power table: 128 entries for pan and fx mix 0..127. A pan or fx mix p uses entries p and 127 - p. */
#define SYN_EQPOWER_SIZE 128
#define SYN_EQPOWER_MAX (SYN_EQPOWER_SIZE - 1)

/* A_RESAMPLE's pitch is the ratio in units of 1/32768, in 16 bits. The resamplers keep their ratio on that grid. */
#define SYN_PITCH_ONE 32768.0f

/* The RSP ramps a volume once per 8 samples by a 16.16 ratio. */
#define SYN_RAMP_BLOCK 8
#define SYN_RAMP_BLOCK_SHIFT 3

/* Microseconds per second, for alSynSetVol's ramp time and the player's period. */
#define SYN_US_PER_SECOND 1000000.0

/* A stolen voice keeps playing for this many samples; its volume falls to 0 over the first offset - 64. */
#define SYN_STEAL_DELAY 512
#define SYN_STEAL_FADE_MARGIN 64

/*
 * Gains of the reverb's mono mix. A_MIXER adds in * gain / 32768 to out, with
 * gain signed. Aux L is mixed into itself with (s16)0xDA83 (-9597), so it
 * becomes aux L * (32768 - 9597) / 32768. Then aux R is added with 0x5A82.
 * Both shares are about 0.7071.
 */
#define SYN_FX_MONO_GAIN_L 0xDA83
#define SYN_FX_MONO_GAIN_R 0x5A82
/* Gain of the main bus when it adds a source's aux buffers to the dry mix. */
#define SYN_MAIN_BUS_GAIN 0x7FFF

/* Chorus depth divisor (ROM 8010C468 and 8010C5E0; libmus's CustomFxSet has its own copy at 8010C200). */
#define SYN_CHORUS_DEPTH_DIVISOR 173123.404906676

/* ------------------------------------------------------------------------
 * Filters
 * ---------------------------------------------------------------------- */

/*
 * A filter's pull function. It appends the commands that make outCount
 * samples of its output at sample time sampleOffset and returns the new end
 * of the list. outp points at a DMEM offset (s16). The load filter moves
 * *outp to where its valid samples start; the other filters read it or pass
 * it on.
 */
typedef Acmd *ALFilterHandlerFn(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p);
typedef ALFilterHandlerFn *ALFilterHandler;

/* A filter's parameter function. Every one in the ROM returns 0 (alLoadParam returns no value). */
typedef s32 ALSetParamFn(void *filter, s32 paramID, void *param);
typedef ALSetParamFn *ALSetParam;

typedef struct ALFilter_s {
    /* 0x00 */ PTR32(struct ALFilter_s) source; /* the filter this one pulls from */
    /* 0x04 */ FN32(ALFilterHandlerFn) handler;
    /* 0x08 */ FN32(ALSetParamFn) setParam;
    /* 0x0C */ s16 inp;                   /* set to 0 by alFilterNew; never used */
    /* 0x0E */ s16 outp;                  /* set to 0 by alFilterNew; never used */
    /* 0x10 */ s32 type;                  /* SynFilterType */
} ALFilter; /* size = 0x14 */

/*
 * The load filter: reads a wave's data with the DMA routine and decodes it
 * (alAdpcmPull for AL_ADPCM_WAVE, alRaw16Pull for AL_RAW16_WAVE).
 */
typedef struct ALLoadFilter {
    /* 0x00 */ ALFilter filter;
    /* 0x14 */ PTR32(ADPCM_STATE) state;     /* decoder state; A_ADPCM reads and writes it */
    /* 0x18 */ PTR32(ADPCM_STATE) loopState; /* copy of the wave's loop state (alCopy), for A_SETLOOP */
    /* 0x1C */ ALRawLoop loop;         /* start and end in samples; count, -1 = forever */
    /* 0x28 */ PTR32(ALWaveTable) table;
    /* 0x2C */ s32 bookSize;           /* codebook bytes for A_LOADADPCM: order * npredictors * 16 */
    /* 0x30 */ FN32(ALDMAprocFn) dma;
    /* 0x34 */ PTR32(void) dmaState;   /* filled by the ALDMANew */
    /* 0x38 */ s32 sample;             /* sample number of the next output sample */
    /* 0x3C */ s32 framePos;           /* samples of the current 16-sample frame already output (0-15) */
    /* 0x40 */ s32 first;              /* 1: the next A_ADPCM has A_INIT */
    /* 0x44 */ s32 dataAddr;           /* wave address of the next frame to read (a cartridge address) */
} ALLoadFilter; /* size = 0x48 */

typedef struct ALResampler {
    /* 0x00 */ ALFilter filter;
    /* 0x14 */ PTR32(RESAMPLE_STATE) state; /* A_RESAMPLE reads and writes it */
    /* 0x18 */ f32 ratio;              /* pitch: input samples per output sample */
    /* 0x1C */ s32 unityPitch;         /* 1: copy the samples (A_DMEMMOVE), no resampling */
    /* 0x20 */ f32 delta;              /* fraction of an input sample carried to the next pull */
    /* 0x24 */ s32 first;              /* 1: the next A_RESAMPLE has A_INIT */
    /* 0x28 */ s32 unused28;           /* set to 0 by alResampleNew; never read */
    /* 0x2C */ s32 unused2C;           /* set to 0 by alResampleNew; never read */
    /* 0x30 */ s32 playing;            /* 1 after SYN_START, 0 after SYN_RESET; never read */
} ALResampler; /* size = 0x34 */

/*
 * The envelope mixer: applies volume, pan and the reverb send, and mixes the
 * voice into the dry and aux buffers. It holds the voice's list of timed
 * parameter changes.
 */
typedef struct ALEnvMixer {
    /* 0x00 */ ALFilter filter;
    /* 0x14 */ PTR32(ENVMIX_STATE) state; /* A_ENVMIXER reads and writes it */
    /* 0x18 */ s16 pan;                /* 0 = left, 127 = right */
    /* 0x1A */ s16 volume;             /* (v * v) >> 15 of the last SYN_SET_VOLUME; 1 after SYN_RESET */
    /* 0x1C */ s16 volLeft;            /* left volume at the start of the ramp (A_SETVOL A_VOL|A_LEFT) */
    /* 0x1E */ s16 volRight;
    /* 0x20 */ s16 dryAmount;          /* eqpower[fxMix] */
    /* 0x22 */ s16 wetAmount;          /* eqpower[127 - fxMix] */
    /* 0x24 */ u16 rateLeftFrac;       /* left ramp ratio per 8 samples, 16.16: fraction (written as trunc(f * 65535)) */
    /* 0x26 */ s16 rateLeft;           /* left ramp ratio: integer part */
    /* 0x28 */ s16 targetLeft;         /* (volume * eqpower[pan]) >> 15 */
    /* 0x2A */ u16 rateRightFrac;
    /* 0x2C */ s16 rateRight;
    /* 0x2E */ s16 targetRight;        /* (volume * eqpower[127 - pan]) >> 15 */
    /* 0x30 */ s32 rampPos;            /* samples mixed since the ramp started */
    /* 0x34 */ s32 rampLength;         /* samples of the ramp */
    /* 0x38 */ s32 first;              /* 1: send the volumes again and use A_INIT */
    /* 0x3C */ PTR32(struct ALParam_s) updates;    /* pending changes, oldest first */
    /* 0x40 */ PTR32(struct ALParam_s) lastUpdate; /* the last of them */
    /* 0x44 */ s32 unused44;           /* set to 0 by alEnvmixerNew; never read */
    /* 0x48 */ s32 playing;            /* 1 after SYN_START, 0 after SYN_RESET */
} ALEnvMixer; /* size = 0x4C */

typedef struct ALSave {
    /* 0x00 */ ALFilter filter;
    /* 0x14 */ s32 dramOut;            /* output buffer of A_SAVEBUFF (a word, ptr32.h) */
    /* 0x18 */ s32 first;              /* set to 1 by alSaveNew; never read */
} ALSave; /* size = 0x1C */

typedef struct ALMainBus_s {
    /* 0x00 */ ALFilter filter;
    /* 0x14 */ s32 sourceCount;
    /* 0x18 */ s32 maxSources;         /* not checked */
    /* 0x1C */ PTR32(PTR32(ALFilter)) sources;
} ALMainBus; /* size = 0x20 */

/* The low-pass filter of a reverb section. */
typedef struct ALLowPass {
    /* 0x00 */ s16 fc;                 /* coefficient from the preset */
    /* 0x02 */ s16 gain;               /* 0x4000 - fc / 2: the gain of A_POLEF */
    /* 0x04 */ s32 unused04;           /* never written or read */
    /* 0x08 */ s16 coef[SYN_LOWPASS_COEFS]; /* A_LOADADPCM data: 8 zeros, fc / 2, then its powers */
    /* 0x28 */ PTR32(POLEF_STATE) fstate; /* A_POLEF reads and writes it */
    /* 0x2C */ s32 first;              /* 1: the next A_POLEF has A_INIT */
} ALLowPass; /* size = 0x30 */

/* One delay section of the reverb. */
typedef struct ALDelay {
    /* 0x00 */ u32 input;              /* input tap, samples behind the write position */
    /* 0x04 */ u32 output;             /* output tap, samples behind the write position */
    /* 0x08 */ s16 ffcoef;             /* feed-forward: output += input * ffcoef */
    /* 0x0A */ s16 fbcoef;             /* feedback: input += output * fbcoef */
    /* 0x0C */ s16 gain;               /* output's share of the reverb's result */
    /* 0x0E */ s16 pad0E;
    /* 0x10 */ f32 chorusStep;         /* 2 * (rate / 1000) / outputRate */
    /* 0x14 */ f32 chorusValue;        /* triangle wave phase, -2..2; 1.0 at set up */
    /* 0x18 */ s32 chorusDelta;        /* offset of the chorused output tap, in samples */
    /* 0x1C */ f32 chorusDepth;        /* depth / 173123.404906676 * (output - input) */
    /* 0x20 */ PTR32(ALLowPass) lowpass;     /* NULL: none */
    /* 0x24 */ PTR32(ALResampler) resampler; /* NULL: no chorus */
} ALDelay; /* size = 0x28 */

typedef struct ALFx {
    /* 0x00 */ ALFilter filter;
    /* 0x14 */ PTR32(s16) delayLine;   /* base of the delay line */
    /* 0x18 */ PTR32(s16) inputPos;    /* write position; in delayLine[0..length] */
    /* 0x1C */ u32 length;             /* delay line length in samples */
    /* 0x20 */ PTR32(ALDelay) sections;
    /* 0x24 */ u8 sectionCount;
    /* 0x28 */ FN32(ALSetParamFn) paramHandler; /* alFxParamHdl */
} ALFx; /* size = 0x2C */

typedef struct ALAuxBus_s {
    /* 0x00 */ ALFilter filter;
    /* 0x14 */ s32 sourceCount;
    /* 0x18 */ s32 maxSources;         /* not checked */
    /* 0x1C */ PTR32(PTR32(ALFilter)) sources;
    /* 0x20 */ ALFx fx[1];
} ALAuxBus; /* size = 0x4C */

/* A physical voice: a load filter, a resampler and an envelope mixer in a chain. */
typedef struct PVoice_s {
    /* 0x00 */ ALLink node;            /* in ALSynth.pFreeList, pAllocList or pLameList */
    /* 0x08 */ PTR32(ALVoice) vvoice;  /* the ALVoice that owns it, or NULL */
    /* 0x0C */ PTR32(ALFilter) channelKnob; /* the envelope mixer: every change goes there */
    /* 0x10 */ ALLoadFilter decoder;
    /* 0x58 */ ALResampler resampler;
    /* 0x8C */ ALEnvMixer envmixer;
    /* 0xD8 */ s32 offset;             /* samples added to each change's time: SYN_STEAL_DELAY after a steal, else 0 */
} PVoice; /* size = 0xDC */

/* ------------------------------------------------------------------------
 * Parameter changes
 * ---------------------------------------------------------------------- */

/* One word of a change. Pitch travels as the bits of an f32. */
typedef union SynValue {
    s32 i;
    f32 f;
    PTR32(void) p;
} SynValue;

/*
 * A value that is not a pointer travels in the void * of a filter's
 * setParam: SYN_SET_PITCH sends the bits of an f32. These give the bits as
 * they are. A PTR32 member (SynValue.p) would encode them as a pointer.
 */
static inline void *synParamFromBits(s32 bits) {
    return (void *)(uintptr_t)(u32)bits;
}

static inline s32 synParamBits(void *param) {
    return (s32)(u32)(uintptr_t)param;
}

/* A timed change for a voice, queued in its envelope mixer. */
typedef struct ALParam_s {
    /* 0x00 */ PTR32(struct ALParam_s) next;
    /* 0x04 */ s32 time;               /* sample time: paramSamples + PVoice.offset */
    /* 0x08 */ s16 type;               /* SynParamId */
    /* 0x0A */ s16 unityPitch;         /* SYN_START_VOICE: ALVoice.unityPitch */
    /* 0x0C */ SynValue value;         /* volume (s16 value), pan, fx mix, pitch, ALWaveTable * or PVoice * */
    /* 0x10 */ s32 rampSamples;        /* SYN_SET_VOLUME: ramp length in samples (multiple of 16) */
    /* 0x14 */ s32 unused14;
    /* 0x18 */ s32 unused18;
} ALParam; /* size = 0x1C */

/* The layout of an ALParam of type SYN_START_VOICE_PARAMS. */
typedef struct SynStartParams {
    /* 0x00 */ PTR32(struct ALParam_s) next;
    /* 0x04 */ s32 time;
    /* 0x08 */ s16 type;
    /* 0x0A */ s16 unityPitch;
    /* 0x0C */ f32 pitch;
    /* 0x10 */ s16 volume;
    /* 0x12 */ u8 pan;
    /* 0x13 */ u8 fxMix;
    /* 0x14 */ s32 rampSamples;        /* 0: start at the target volume */
    /* 0x18 */ PTR32(ALWaveTable) wave;
} SynStartParams; /* size = 0x1C */

/* ------------------------------------------------------------------------
 * Tables
 * ---------------------------------------------------------------------- */

/*
 * The equal-power table (ROM D_801F45B0, 128 s16): entry i is
 * round(32767 * cos(i * pi / 254)). The port's loader puts the ROM's copy in
 * port_ram and swaps it to host order (gen/ramtab.c), so the synthesizer
 * reads that copy. libmus keeps pan and fx mix in 0..127, so no index goes
 * past the table.
 */
extern s16 D_801F45B0[];
#define synEqPower D_801F45B0

/* ------------------------------------------------------------------------
 * Audio commands. Each builder writes one command at p and returns p + 1.
 * The masks follow the RSP's command format, except that addresses keep all
 * 32 bits.
 * ---------------------------------------------------------------------- */

/* A_ADPCM: decodes with the flags (A_INIT, A_LOOP) and the ADPCM_STATE at state. */
static inline Acmd *synCmdADPCM(Acmd *p, u32 flags, u32 state) {
    p->words.w0 = ((u32)A_ADPCM << 24) | ((flags & 0xFF) << 16);
    p->words.w1 = state;
    return p + 1;
}

/* A_CLEARBUFF: clears count bytes of DMEM at dmem. */
static inline Acmd *synCmdClearBuffer(Acmd *p, s32 dmem, s32 count) {
    p->words.w0 = ((u32)A_CLEARBUFF << 24) | ((u32)dmem & 0xFFFFFF);
    p->words.w1 = (u32)count;
    return p + 1;
}

/* A_ENVMIXER: mixes with the flags (A_INIT, A_AUX) and the ENVMIX_STATE at state. */
static inline Acmd *synCmdEnvMixer(Acmd *p, u32 flags, u32 state) {
    p->words.w0 = ((u32)A_ENVMIXER << 24) | ((flags & 0xFF) << 16);
    p->words.w1 = state;
    return p + 1;
}

/* A_LOADBUFF: copies the last A_SETBUFF's count of bytes from addr to its input offset. */
static inline Acmd *synCmdLoadBuffer(Acmd *p, u32 addr) {
    p->words.w0 = (u32)A_LOADBUFF << 24;
    p->words.w1 = addr;
    return p + 1;
}

/* A_SAVEBUFF: copies the last A_SETBUFF's count of bytes from its output offset to addr. */
static inline Acmd *synCmdSaveBuffer(Acmd *p, u32 addr) {
    p->words.w0 = (u32)A_SAVEBUFF << 24;
    p->words.w1 = addr;
    return p + 1;
}

/*
 * A_SETBUFF with no flags: the input, output and byte count of the commands
 * that follow. A_LOADBUFF writes to in; A_SAVEBUFF reads from out.
 */
static inline Acmd *synCmdSetBuffer(Acmd *p, s32 in, s32 out, s32 count) {
    p->words.w0 = ((u32)A_SETBUFF << 24) | ((u32)in & 0xFFFF);
    p->words.w1 = ((u32)out << 16) | ((u32)count & 0xFFFF);
    return p + 1;
}

/* A_SETBUFF with A_AUX: the dry right, wet left and wet right buffers of A_ENVMIXER. */
static inline Acmd *synCmdSetBufferAux(Acmd *p, s32 dryRight, s32 wetLeft, s32 wetRight) {
    p->words.w0 = ((u32)A_SETBUFF << 24) | ((u32)A_AUX << 16) | ((u32)dryRight & 0xFFFF);
    p->words.w1 = ((u32)wetLeft << 16) | ((u32)wetRight & 0xFFFF);
    return p + 1;
}

/* A_SETVOL: the flags, a value in w0's low 16 bits, and the two halves of w1. */
static inline Acmd *synCmdSetVolume(Acmd *p, u32 flags, s32 value, s32 high, s32 low) {
    p->words.w0 = ((u32)A_SETVOL << 24) | ((flags & 0xFF) << 16) | ((u32)value & 0xFFFF);
    p->words.w1 = ((u32)high << 16) | ((u32)low & 0xFFFF);
    return p + 1;
}

/* A_SETLOOP: the ADPCM_STATE that an A_ADPCM with A_LOOP starts from. */
static inline Acmd *synCmdSetLoop(Acmd *p, u32 state) {
    p->words.w0 = (u32)A_SETLOOP << 24;
    p->words.w1 = state;
    return p + 1;
}

/* A_DMEMMOVE: copies count bytes of DMEM from in to out. */
static inline Acmd *synCmdDMEMMove(Acmd *p, s32 in, s32 out, s32 count) {
    p->words.w0 = ((u32)A_DMEMMOVE << 24) | ((u32)in & 0xFFFFFF);
    p->words.w1 = ((u32)out << 16) | ((u32)count & 0xFFFF);
    return p + 1;
}

/* A_LOADADPCM: loads count bytes of s16 coefficients (host order) from addr. */
static inline Acmd *synCmdLoadADPCM(Acmd *p, s32 count, u32 addr) {
    p->words.w0 = ((u32)A_LOADADPCM << 24) | ((u32)count & 0xFFFFFF);
    p->words.w1 = addr;
    return p + 1;
}

/* A_MIXER with no flags: mixes DMEM in into DMEM out with gain. The byte count is the last A_SETBUFF's. */
static inline Acmd *synCmdMixer(Acmd *p, s32 gain, s32 in, s32 out) {
    p->words.w0 = ((u32)A_MIXER << 24) | ((u32)gain & 0xFFFF);
    p->words.w1 = ((u32)in << 16) | ((u32)out & 0xFFFF);
    return p + 1;
}

/* A_INTERLEAVE: interleaves the left and right buffers. */
static inline Acmd *synCmdInterleave(Acmd *p, s32 left, s32 right) {
    p->words.w0 = (u32)A_INTERLEAVE << 24;
    p->words.w1 = ((u32)left << 16) | ((u32)right & 0xFFFF);
    return p + 1;
}

/* A_RESAMPLE: the flags, the pitch in units of 1/32768 and the address of the RESAMPLE_STATE. */
static inline Acmd *synCmdResample(Acmd *p, s32 flags, s32 pitch, u32 state) {
    p->words.w0 = ((u32)A_RESAMPLE << 24) | (((u32)flags & 0xFF) << 16) | ((u32)pitch & 0xFFFF);
    p->words.w1 = state;
    return p + 1;
}

/* A_POLEF: the flags, the gain and the address of the POLEF_STATE. */
static inline Acmd *synCmdPoleFilter(Acmd *p, s32 flags, s32 gain, u32 state) {
    p->words.w0 = ((u32)A_POLEF << 24) | (((u32)flags & 0xFF) << 16) | ((u32)gain & 0xFFFF);
    p->words.w1 = state;
    return p + 1;
}

/* ------------------------------------------------------------------------
 * Functions. ROM address in the comment; new names are explained in the map.
 * ---------------------------------------------------------------------- */

/* synth-core.c: set up, the frame, the parameter pool and helpers */
void alInit(ALGlobals *g, ALSynConfig *c);                                    /* 801CB190; unused by Glover */
void alClose(ALGlobals *g);                                                   /* 801CB1BC */
void alLink(ALLink *element, ALLink *after);                                  /* 801CB1EC */
void alUnlink(ALLink *element);                                               /* 801CB20C */
void alHeapInit(ALHeap *hp, u8 *base, s32 len);                               /* 801CB240 */
void *alHeapDBAlloc(u8 *file, s32 line, ALHeap *hp, s32 num, s32 size);       /* 801CB280 */
void alCopy(void *src, void *dest, s32 len);                                  /* 801CB2D0 */
void alSynNew(ALSynth *s, ALSynConfig *c);                                    /* 801CB310; unused by Glover */
Acmd *alAudioFrame(Acmd *cmdList, s32 *cmdLen, s16 *outBuf, s32 outLen);      /* 801CB5DC */
ALParam *__allocParam(void);                                                  /* 801CB78C */
void __freeParam(ALParam *param);                                             /* 801CB7B8 */
void _collectPVoices(ALSynth *s);                                             /* 801CB7D0 */
void _freePVoice(ALSynth *s, PVoice *pvoice);                                 /* 801CB824 */
s32 synTimeToSamplesExact(ALSynth *s, s32 micros);                            /* 801CB864 */
s32 _timeToSamples(ALSynth *s, s32 micros);                                   /* 801CB8AC */
s32 synNextPlayer(ALSynth *s, ALPlayer **client);                             /* 801CB8F8 */
void alSynDelete(ALSynth *s);                                                 /* 801CB9A0 */
void alSynAddPlayer(ALSynth *s, ALPlayer *client);                            /* 801CB9B0 */
void alFilterNew(ALFilter *f, ALFilterHandler handler, ALSetParam setParam, s32 type); /* 801CA040 */
void alFxNew(ALFx *r, ALSynConfig *c, ALHeap *hp);                            /* 801C7F54; unused by Glover */

/* synth-voice.c: the voice API, the buses, the save filter and the resampler */
s32 alSynAllocVoice(ALSynth *s, ALVoice *voice, ALVoiceConfig *vc);           /* 801CBA00 */
s32 _allocatePVoice(ALSynth *s, PVoice **pvoice, s16 priority);               /* 801CBB28 */
void alSynStopVoice(ALSynth *s, ALVoice *voice);                              /* 801CBBE0 */
void alSynStartVoice(ALSynth *s, ALVoice *voice, ALWaveTable *table);         /* 801CBC60 */
void alSynSetPitch(ALSynth *s, ALVoice *voice, f32 ratio);                    /* 801CBD00 */
void alSynSetVol(ALSynth *s, ALVoice *voice, s16 vol, ALMicroTime t);         /* 801CBD90 */
void alSynSetFXMix(ALSynth *s, ALVoice *voice, u8 fxmix);                     /* 801CBE50 */
void alSynSetPan(ALSynth *s, ALVoice *voice, ALPan pan);                      /* 801CBEE0 */
ALFxRef *alSynAllocFX(ALSynth *s, s16 bus, ALSynConfig *c, ALHeap *hp);       /* 801CBF70; returns the ALFx */
void alAuxBusNew(ALAuxBus *m, void *sources, s32 maxSources);                 /* 801C85E0 */
Acmd *alAuxBusPull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p); /* 801C91F0 */
s32 alAuxBusParam(void *filter, s32 paramID, void *param);                    /* 801C92CC */
void alMainBusNew(ALMainBus *m, void *sources, s32 maxSources);               /* 801C863C */
Acmd *alMainBusPull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p); /* 801CA060 */
s32 alMainBusParam(void *filter, s32 paramID, void *param);                   /* 801CA1A0 */
void alSaveNew(ALSave *f);                                                    /* 801C8698 */
Acmd *alSavePull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p); /* 801CB060 */
s32 alSaveParam(void *filter, s32 paramID, void *param);                      /* 801CB160 */
void alResampleNew(ALResampler *r, ALHeap *hp);                               /* 801C8554 */
Acmd *alResamplePull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p); /* 801CA1D0 */
s32 alResampleParam(void *filter, s32 paramID, void *param);                  /* 801CA3BC */

/* synth-load.c: the load filter */
void alLoadNew(ALLoadFilter *f, ALDMANew dmaNew, ALHeap *hp);                 /* 801C84A4 */
Acmd *alAdpcmPull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p); /* 801C86E0 */
Acmd *alRaw16Pull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p); /* 801C8B24 */
s32 alLoadParam(void *filter, s32 paramID, void *param);                      /* 801C8EC0 */
Acmd *synDecodeChunk(Acmd *p, ALLoadFilter *f, s32 tsam, s32 nbytes, s16 outp, s16 inp, u32 flags); /* 801C906C */

/* synth-envmix.c: the envelope mixer */
void alEnvmixerNew(ALEnvMixer *e, ALHeap *hp);                                /* 801C83EC */
Acmd *alEnvmixerPull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p); /* 801C9300 */
s32 alEnvmixerParam(void *filter, s32 paramID, void *param);                  /* 801C984C */
Acmd *synPullSubFrame(ALEnvMixer *e, s16 *inp, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p); /* 801C9920 */
s16 synGetRate(f64 vol, f64 tgt, s32 count, u16 *ratel);                      /* 801C9D24 */
f32 synGetVol(f32 ivol, s32 samples, s16 ratem, u16 ratel);                   /* 801C9FA4 */

/* synth-fx.c: the reverb */
void init_lpfilter(ALLowPass *lp);                                            /* 801C7EB0 */
Acmd *alFxPull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p); /* 801CA480 */
s32 alFxParam(void *filter, s32 paramID, void *param);                        /* 801CA830 */
s32 alFxParamHdl(void *filter, s32 paramID, void *param);                     /* 801CA844; unused by Glover */
Acmd *_loadOutputBuffer(ALFx *r, ALDelay *d, s32 buff, s32 incount, Acmd *p); /* 801CAA34 */
Acmd *_loadBuffer(ALFx *r, s16 *curr_ptr, s32 buff, s32 count, Acmd *p);      /* 801CAC4C */
Acmd *_saveBuffer(ALFx *r, s16 *curr_ptr, s32 buff, s32 count, Acmd *p);      /* 801CADBC */
Acmd *_filterBuffer(ALLowPass *lp, s32 buff, s32 count, Acmd *p);             /* 801CAF2C */
f32 _doModFunc(ALDelay *d, s32 count);                                        /* 801CAFC8 */

/*
 * The command interpreter (the HLE phase). The ROM's alAudioFrame only
 * builds the list; the RSP ran it later. libmus's glue (mus-glue.c) expects
 * the samples in outBuf when alAudioFrame returns. So the native
 * alAudioFrame runs the list it built with this function before it returns.
 */
void synExecuteCommands(Acmd *cmdList, s32 cmdCount);

/* ------------------------------------------------------------------------
 * Layout checks
 * ---------------------------------------------------------------------- */

#ifdef __cplusplus
#define SYN_STATIC_ASSERT static_assert
#else
#define SYN_STATIC_ASSERT _Static_assert
#endif
#define SYN_CHECK_SIZE(type, size) SYN_STATIC_ASSERT(sizeof(type) == (size), #type " must keep the ROM's size")
#define SYN_CHECK_OFFSET(type, field, offset) \
    SYN_STATIC_ASSERT(offsetof(type, field) == (offset), #type "." #field " must keep the ROM's offset")

SYN_CHECK_SIZE(PTR32(void), 4);
SYN_CHECK_SIZE(Acmd, 8);
SYN_CHECK_SIZE(ALSynth, 0x4C);
SYN_CHECK_OFFSET(ALSynth, paramSamples, 0x1C);
SYN_CHECK_OFFSET(ALSynth, maxOutSamples, 0x48);
SYN_CHECK_SIZE(ALPlayer, 0x14);
SYN_CHECK_SIZE(ALVoice, 0x1C);
SYN_CHECK_OFFSET(ALVoiceConfig, unityPitch, 0x04);
SYN_CHECK_SIZE(ALSynConfig, 0x24);
SYN_CHECK_OFFSET(ALSynConfig, fxType, 0x1C);
SYN_CHECK_SIZE(ALWaveTable, 0x14);
SYN_CHECK_SIZE(ALADPCMloop, 0x2C);
SYN_CHECK_SIZE(ADPCM_STATE, 0x20);
SYN_CHECK_SIZE(RESAMPLE_STATE, 0x20);
SYN_CHECK_SIZE(ENVMIX_STATE, 0x50);
SYN_CHECK_SIZE(POLEF_STATE, 0x08);

SYN_CHECK_SIZE(ALFilter, 0x14);
SYN_CHECK_SIZE(ALLoadFilter, 0x48);
SYN_CHECK_OFFSET(ALLoadFilter, loop, 0x1C);
SYN_CHECK_OFFSET(ALLoadFilter, dataAddr, 0x44);
SYN_CHECK_SIZE(ALResampler, 0x34);
SYN_CHECK_OFFSET(ALResampler, first, 0x24);
SYN_CHECK_SIZE(ALEnvMixer, 0x4C);
SYN_CHECK_OFFSET(ALEnvMixer, rateLeftFrac, 0x24);
SYN_CHECK_OFFSET(ALEnvMixer, playing, 0x48);
SYN_CHECK_SIZE(ALSave, 0x1C);
SYN_CHECK_SIZE(ALMainBus, 0x20);
SYN_CHECK_SIZE(ALLowPass, 0x30);
SYN_CHECK_OFFSET(ALLowPass, coef, 0x08);
SYN_CHECK_OFFSET(ALLowPass, fstate, 0x28);
SYN_CHECK_SIZE(ALDelay, 0x28);
SYN_CHECK_OFFSET(ALDelay, chorusStep, 0x10);
SYN_CHECK_OFFSET(ALDelay, resampler, 0x24);
SYN_CHECK_SIZE(ALFx, 0x2C);
SYN_CHECK_OFFSET(ALFx, sectionCount, 0x24);
SYN_CHECK_SIZE(ALAuxBus, 0x4C);
SYN_CHECK_OFFSET(ALAuxBus, fx, 0x20);
SYN_CHECK_SIZE(PVoice, 0xDC);
SYN_CHECK_OFFSET(PVoice, decoder, 0x10);
SYN_CHECK_OFFSET(PVoice, resampler, 0x58);
SYN_CHECK_OFFSET(PVoice, envmixer, 0x8C);
SYN_CHECK_OFFSET(PVoice, offset, 0xD8);
SYN_CHECK_SIZE(ALParam, 0x1C);
SYN_CHECK_OFFSET(ALParam, value, 0x0C);
SYN_CHECK_SIZE(SynStartParams, 0x1C);
SYN_CHECK_OFFSET(SynStartParams, volume, 0x10);
SYN_CHECK_OFFSET(SynStartParams, wave, 0x18);

#endif /* SYNTH_INTERNAL_H */
