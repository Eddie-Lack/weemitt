/*
 * mus_internal.h: shared declarations for the native reimplementation of
 * libmus, the music and sound effect driver of Glover (USA).
 *
 * Every layout here was read from the ROM's machine code (libmus occupies
 * VRAM 0x801BF800-0x801C4024). docs/audio-libmus-map.md explains each field,
 * each function and each data format. The offsets in the comments are the
 * ROM's offsets. The port builds for 32-bit x86 (gcc -m32) with 4-byte
 * pointers. Every structure here keeps the ROM's size and offsets.
 *
 * Byte order rules (see the map, section "Data formats"):
 *  - Song files and sound effect streams stay big-endian. Read them with
 *    musRead16 and musRead32. libmus never writes to them.
 *  - musBankInit converts a pbank (the "ptr" file) to host order in place,
 *    once. Its flags word at offset 0x10 stays big-endian. The "initialised"
 *    test reads it with musRead32 before and after the conversion.
 *  - The effect tables arrive in port_ram already converted by the port's
 *    loader: host pointers and host-order priorities.
 *  - Everything else (channels, messages, reverb presets) is host order.
 */
#ifndef MUS_INTERNAL_H
#define MUS_INTERNAL_H

#include <stddef.h>

#include <PR/ultratypes.h>
#include <PR/libaudio.h>

#include "port_win.h" /* the bytes reserved for the objects in port_win (gen/) */

/* The synthesizer's structures, constants and functions. libmus and the synthesizer share these definitions. */
#include "../synth/synth_internal.h"

/* ------------------------------------------------------------------------
 * Big-endian readers
 * ---------------------------------------------------------------------- */

static inline u16 musRead16(const u8 *p) {
    return (u16)((p[0] << 8) | p[1]);
}

static inline u32 musRead32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/*
 * Reads a variable length number from a command stream and returns the byte
 * after it. A byte below 0x80 is the value. Otherwise its low 7 bits and the
 * next byte form a 15-bit value, high byte first. Commands 0x81, 0x8B, 0x90
 * and 0xA7 and the note lengths use this form.
 */
static inline u8 *musReadVarLen(u8 *p, u16 *value) {
    u16 v = *p++;

    if (v & 0x80) {
        v = ((v & 0x7F) << 8) | *p++;
    }
    *value = v;
    return p;
}

/*
 * The ROM converts a float to an unsigned int with the sequence GCC emits for
 * (u32)f on MIPS. A plain C cast is undefined for negative values on x86, so
 * the loop start command (0x95) uses this helper.
 * Results: -1 < f < 2^31 truncates toward zero; f <= -1 and NaN give
 * 0xFFFFFFFF; 2^31 <= f < 2^32 gives (u32)(f - 2^31) | 0x80000000.
 */
static inline u32 musFloatToU32(f32 f) {
    if (!(f > -1.0f)) {
        return 0xFFFFFFFF;
    }
    if (f < 2147483648.0f) {
        return (u32)(s32)f;
    }
    f -= 2147483648.0f;
    if (f < 2147483648.0f) {
        return (u32)(s32)f | 0x80000000;
    }
    return 0xFFFFFFFF;
}

/* ------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/* Flags for MusStop, MusAsk and MusSetMasterVolume. */
#define MUSFLAG_EFFECTS 1 /* channels with fxNumber != 0 */
#define MUSFLAG_SONGS 2   /* channels with fxNumber == 0 */

/* MusChannel.flags */
#define MUS_CHANNEL_PAUSED 1 /* skipped by the player; set by MusStartSong until the unpause message runs */

/* Note stream values */
#define MUS_NOTE_REST 0x60          /* note number that only starts the release */
#define MUS_LENGTH_INFINITE 0x7FFF  /* note length that never ends */
#define MUS_LOOP_INFINITE 0xFF      /* loop count that repeats forever */
#define MUS_LOOP_DEPTH 4            /* nested loops per channel */
#define MUS_ENVELOPE_SIZE 7         /* bytes per envelope (command 0x84 operand) */
#define MUS_DRUM_ENTRY_SIZE 4       /* bytes per note in a drum map */
#define MUS_FIRST_COMMAND 0x80      /* bytes from here up are commands */
#define MUS_NUM_COMMANDS 43         /* commands 0x80..0xAA */

/* MusChannel.envelopePhase */
#define MUS_ENV_OFF 0
#define MUS_ENV_ATTACK 1
#define MUS_ENV_DECAY 2
#define MUS_ENV_SUSTAIN 3
#define MUS_ENV_RELEASE 4
#define MUS_ENV_DONE 5

/* MusMessage.type */
#define MUS_MSG_PAUSE 0
#define MUS_MSG_UNPAUSE 1
#define MUS_MSG_FX_TYPE 2

#define MUS_FIFO_MIN 64   /* MusConfig.fifo_length is clamped to this range */
#define MUS_FIFO_MAX 1024

/* Song file header offsets (big-endian u32 words, offsets from the song start). */
#define MUS_SONG_NUM_CHANNELS 0x00 /* s32 */
#define MUS_SONG_DATA_TABLE 0x04   /* offset of u32[numChannels]: command streams */
#define MUS_SONG_VOLUME_TABLE 0x08 /* offset of u32[numChannels]: volume streams */
#define MUS_SONG_BEND_TABLE 0x0C   /* offset of u32[numChannels]: pitch bend streams */
#define MUS_SONG_ENV_TABLE 0x10    /* offset of the envelope table (7 bytes each) */
#define MUS_SONG_DRUM_TABLE 0x14   /* offset of u32[]: offsets of drum maps */
#define MUS_SONG_UNRELOCATED_LIMIT 0x400 /* ROM test: word 0x04 below this means "offsets" */

/* pbank header */
#define MUS_BANK_INITIALISED 0x80000000 /* bit of the big-endian flags word */
#define MUS_WAVE_RELOCATED 1            /* ALWaveTable.flags after musBankInit */
#define MUS_WAVE_DISK_TAG 0xFF000000    /* wave base in the 64DD drive ROM; unused by Glover */
#define MUS_WAVE_DISK_OFFSET 0x140000   /* added to a 64DD base by the ROM's DMA callback */

/* Timing */
#define MUS_US_PER_SECOND 1000000
#define MUS_FPS_PAL 50
#define MUS_FPS_NTSC 60
#define MUS_TEMPO_BASE 0x6000   /* tempo step at 120 bpm is MUS_TEMPO_BASE / fps */
#define MUS_TEMPO_SCALE_ONE 128 /* MusChannel.tempoScale for normal speed */

/* Pitch */
#define MUS_FIRST_PITCH 99.9f   /* MusChannel.lastPitch after reset: forces the first alSynSetPitch */
#define MUS_PITCH_MAX_RATIO 2.0 /* larger ratios are clamped and silence the note */

/* Random numbers (commands 0xA3-0xA5) */
#define MUS_RANDOM_SEED 0x12345678
#define MUS_RANDOM_TAPS 0x48000000

/* Reverb */
#define MUS_DEFAULT_FX_TYPE 2   /* preset chosen by MusInitialize (big room) */
#define MUS_NUM_CUSTOM_FX 6
#define MUS_FX_SECTION_WORDS 8  /* s32 words per delay section in a preset */

/* ------------------------------------------------------------------------
 * Structures
 * ---------------------------------------------------------------------- */

/*
 * The structure the game passes to MusInitialize (Glover: musicPlayer).
 * Names follow how libmus uses each word. The game's own typedef in
 * src/game/dma.c has the same layout but names the words 0x30-0x40 one
 * place off; the map lists both.
 */
typedef struct MusConfig {
    /* 0x00 */ u32 control_flag;    /* bit 0: wave data is addressable in memory (no DMA) */
    /* 0x04 */ s32 channels;        /* number of channels and voices */
    /* 0x08 */ PTR32(void) sched;   /* scheduler; only the ROM's audio thread used it */
    /* 0x0C */ s32 thread_priority; /* audio thread priority; also every voice's priority */
    /* 0x10 */ PTR32(u8) heap;      /* memory for libmus and the synthesizer */
    /* 0x14 */ s32 heap_length;
    /* 0x18 */ PTR32(u8) ptr;       /* default pbank, or NULL */
    /* 0x1C */ u32 wbk;             /* wave data of the default pbank (a cartridge address) */
    /* 0x20 */ PTR32(PTR32(u8)) default_fxbank; /* pointers to the sound effect streams */
    /* 0x24 */ PTR32(s32) priority; /* host-order priority of each sound effect */
    /* 0x28 */ s32 fifo_length;     /* message queue entries (clamped to 64..1024) */
    /* 0x2C */ s32 unused2C;        /* not read by libmus */
    /* 0x30 */ s32 syn_updates;     /* ALSynConfig.maxUpdates */
    /* 0x34 */ s32 syn_output_rate; /* requested output rate in Hz */
    /* 0x38 */ s32 syn_rsp_cmds;    /* ROM only: audio command list length */
    /* 0x3C */ s32 syn_retraceCount; /* ROM only: video frames per audio frame */
    /* 0x40 */ s32 syn_num_dma_bufs; /* ROM only: DMA buffers */
    /* 0x44 */ s32 syn_dma_buf_size; /* ROM only: bytes per DMA buffer */
} MusConfig; /* size = 0x48 */

/*
 * The third structure argument of amCreateAudioMgr (built on MusInitialize's
 * stack at sp+0x60).
 */
typedef struct MusAudioMgrConfig {
    /* 0x00 */ u32 outputRate;     /* MusConfig.syn_output_rate */
    /* 0x04 */ u32 framesPerField; /* MusConfig.syn_retraceCount */
    /* 0x08 */ u32 maxACMDSize;    /* MusConfig.syn_rsp_cmds */
} MusAudioMgrConfig; /* size = 0x0C */

/*
 * A pbank ("ptr" file, header "N64 PtrTablesV2"). Offsets in the file are
 * relative to the bank start. musBankInit turns them into host pointers and
 * byte-swaps every multi-byte field, except flags.
 */
typedef struct MusBank {
    /* 0x00 */ char id[16];        /* "N64 PtrTablesV2" */
    /* 0x10 */ u8 flags[4];        /* big-endian word, always; MUS_BANK_INITIALISED */
    /* 0x14 */ char wbankName[12]; /* wave file name ("SFX.WBK"); not read by libmus */
    /* 0x20 */ s32 count;          /* number of waves */
    /* 0x24 */ PTR32(u8) baseNotes; /* u8 per wave: base note, 0x30 = no shift */
    /* 0x28 */ PTR32(f32) pitch;   /* per wave; file: s8 detune in the first byte of each word */
    /* 0x2C */ PTR32(PTR32(ALWaveTable)) waves; /* per wave */
} MusBank; /* size = 0x30 */

/* One entry of the message queue between the game and the player. */
typedef struct MusMessage {
    /* 0x00 */ u8 type;  /* MUS_MSG_* */
    /* 0x04 */ s32 value; /* handle, or reverb preset for MUS_MSG_FX_TYPE */
} MusMessage; /* size = 0x08 */

/*
 * One sequencer channel. Channel i always drives voice i (musVoices[i]).
 * Times are in 1/256 ticks (8.8 fixed point). A tick is 1/48 beat.
 */
typedef struct MusChannel {
    /* 0x000 */ u32 flags;              /* MUS_CHANNEL_PAUSED */
    /* 0x004 */ PTR32(u8) pdata;        /* next command stream byte; NULL = channel free */
    /* 0x008 */ PTR32(ALWaveTable) pendingWave; /* wave to start next frame, after the volume ramp */
    /* 0x00C */ u32 time;               /* channel clock */
    /* 0x010 */ s32 stopCount;          /* frames left in the fade out; -1 = not stopping */
    /* 0x014 */ u32 volumeTime;         /* clock of the volume stream */
    /* 0x018 */ u32 bendTime;           /* clock of the pitch bend stream */
    /* 0x01C */ s32 stopLength;         /* fade out length in frames (divisor) */
    /* 0x020 */ f32 vibratoDepth;       /* semitones */
    /* 0x024 */ f32 bend;               /* semitones: bendValue * bendRange */
    /* 0x028 */ f32 lastPitch;          /* last pitch sent to the synthesizer, semitones */
    /* 0x02C */ f32 notePitch;          /* pitch of the current note, semitones */
    /* 0x030 */ f32 pitchOffset;        /* fineTune + MusHandleSetFreqOffset, semitones */
    /* 0x034 */ PTR32(u8) bendStream;   /* NULL = none */
    /* 0x038 */ PTR32(u8) volumeStream; /* NULL = none */
    /* 0x03C */ u32 noteEnd;            /* clock of the next event */
    /* 0x040 */ u32 noteStart;          /* clock when the current note started */
    /* 0x044 */ s32 handle;             /* 0 = none */
    /* 0x048 */ s32 priority;           /* sound effects only; 0 for songs */
    /* 0x04C */ f32 portamentoFrom;     /* pitch at the start of the slide */
    /* 0x050 */ f32 portamentoPitch;    /* pitch reached by the slide */
    /* 0x054 */ u32 releaseTime;        /* clock when the envelope release starts */
    /* 0x058 */ f32 attackStep;         /* (peak - initial) / attack */
    /* 0x05C */ f32 decayStep;          /* (sustain - peak) / decay */
    /* 0x060 */ f32 releaseStep;        /* 1 / release */
    /* 0x064 */ s32 envelopeScale;      /* 1024 / envelopeSpeed */
    /* 0x068 */ f32 vibratoValue;       /* last vibrato offset, semitones */
    /* 0x06C */ f32 bendRange;          /* semitones per bend unit (default 1/32) */
    /* 0x070 */ f32 bendValue;          /* -64..63 */
    /* 0x074 */ PTR32(u8) song;         /* song file; NULL for sound effects */
    /* 0x078 */ PTR32(MusBank) bank;    /* pbank for waves and pitch */
    /* 0x07C */ PTR32(u8) dataStart;    /* start of the command stream (command 0xA1 base) */
    /* 0x080 */ PTR32(u8) drumMap;      /* NULL = off */
    /* 0x084 */ PTR32(u8) bendStreamStart; /* command 0xA1 base */
    /* 0x088 */ PTR32(u8) volumeStreamStart; /* command 0xA1 base */
    /* 0x08C */ f32 fineTune;           /* semitones (command 0x8F) */
    /* 0x090 */ u32 panSweepTime;       /* clock of the pan sweep */
    /* 0x094 */ s16 tempoScale;         /* MusHandleSetTempo, 1..256, 128 = normal */
    /* 0x096 */ u16 length;             /* note length in ticks; MUS_LENGTH_INFINITE */
    /* 0x098 */ u16 tempoStep;          /* added to time every frame */
    /* 0x09A */ s16 handleVolume;       /* MusHandleSetVolume, 128 = normal */
    /* 0x09C */ u16 lastVolume;         /* last volume sent to the synthesizer */
    /* 0x09E */ u16 volumeCount;        /* ticks until the next volume stream event */
    /* 0x0A0 */ u16 bendCount;          /* ticks until the next bend stream event */
    /* 0x0A2 */ u16 fxNumber;           /* sound effect number; 0 for songs */
    /* 0x0A4 */ u16 songTempoStep;      /* tempo step before tempoScale */
    /* 0x0A6 */ u16 noteTicks;          /* ticks since the note started (end of last frame) */
    /* 0x0A8 */ u16 fixedLength;        /* 0 = each note has a length */
    /* 0x0AA */ u16 wave;               /* wave index in the bank */
    /* 0x0AC */ s16 handlePan;          /* MusHandleSetPan, 128 = normal */
    /* 0x0AE */ u16 releaseAfterStart;  /* ticks; 0 = use releaseBeforeEnd */
    /* 0x0B0 */ u16 releaseBeforeEnd;   /* ticks */
    /* 0x0B2 */ u8 vibratoDelay;        /* ticks */
    /* 0x0B3 */ u8 readNextLength;      /* next note reads a length despite fixedLength */
    /* 0x0B4 */ u8 portamentoTicks;     /* 0 = off */
    /* 0x0B5 */ u8 transpose;           /* semitones, signed in use */
    /* 0x0B6 */ u8 ignoreTranspose;
    /* 0x0B7 */ u8 velocity;            /* 0..127 */
    /* 0x0B8 */ u8 volume;              /* 0..127 */
    /* 0x0B9 */ u8 pan;                 /* 0..127, 64 = centre */
    /* 0x0BA */ u8 lastPan;             /* last pan sent to the synthesizer */
    /* 0x0BB */ u8 envelopeSpeed;       /* frames per envelope step */
    /* 0x0BC */ u8 envelopeInitial;
    /* 0x0BD */ u8 envelopePeak;
    /* 0x0BE */ u8 envelopeSustain;
    /* 0x0BF */ u8 envelopePhase;       /* MUS_ENV_* */
    /* 0x0C0 */ u8 envelopeVolume;
    /* 0x0C1 */ u8 envelopeCounter;     /* frames until the next envelope step */
    /* 0x0C2 */ u8 attackSteps;
    /* 0x0C3 */ u8 decaySteps;
    /* 0x0C4 */ u8 releaseSteps;
    /* 0x0C5 */ u8 voiceOn;             /* voice is playing; survives musChannelReset */
    /* 0x0C6 */ u8 reverb;              /* song reverb (command 0xA2) */
    /* 0x0C7 */ u8 handleReverb;        /* MusHandleSetReverb */
    /* 0x0C8 */ u8 lastReverb;          /* reverb value last applied; 0xFF forces an update */
    /* 0x0C9 */ u8 releaseVolume;       /* envelope volume when the release started */
    /* 0x0CA */ u8 trillOnFrames;       /* 0 = trill off */
    /* 0x0CB */ u8 trillOffFrames;
    /* 0x0CC */ u8 trillCounter;
    /* 0x0CD */ s8 trillOffset;         /* 0 or trillDepth */
    /* 0x0CE */ u8 velocityOn;          /* each note has a velocity byte */
    /* 0x0CF */ u8 fixedVelocity;
    /* 0x0D0 */ u8 panSweepSpeed;       /* 0 = off */
    /* 0x0D1 */ u8 vibratoPeriod;       /* ticks; 0 = off */
    /* 0x0D2 */ u8 keepEnvelope;        /* new notes do not restart the envelope */
    /* 0x0D3 */ u8 tie;                 /* new notes do not restart the voice */
    /* 0x0D4 */ s8 trillDepth;          /* semitones */
    /* 0x0D5 */ u8 panSweepFraction;    /* 1/64 pan units */
    /* 0x0D6 */ u8 panSweepDown;
    /* 0x0D7 */ u8 loopDepth;
    /* 0x0D8 */ f32 vibratoStep;        /* radians per tick: 6.2831852 / vibratoPeriod */
    /* 0x0DC */ PTR32(u8) loopStart[MUS_LOOP_DEPTH];
    /* 0x0EC */ PTR32(u8) loopVolumeStream[MUS_LOOP_DEPTH];
    /* 0x0FC */ PTR32(u8) loopBendStream[MUS_LOOP_DEPTH];
    /* 0x10C */ u16 loopVolumeCount[MUS_LOOP_DEPTH];
    /* 0x114 */ u16 loopBendCount[MUS_LOOP_DEPTH];
    /* 0x11C */ u8 loopCount[MUS_LOOP_DEPTH];
    /* 0x120 */ u8 loopVolume[MUS_LOOP_DEPTH];
    /* 0x124 */ u8 loopBendValue[MUS_LOOP_DEPTH];
    /* 0x128 */ u8 unused128[0xC];      /* never read or written except by musChannelReset */
} MusChannel; /* size = 0x134 */

/* A sequencer command: reads its operands at p and returns the next byte, or NULL to end the channel. */
typedef u8 *(*MusCommand)(MusChannel *ch, u8 *p);

/*
 * A DMA buffer's record (the ROM's are 0x14 bytes each, in the libmus heap;
 * mus-glue.c). A buffer holds a copy of the ROM from startAddr on.
 */
typedef struct MusDmaBuffer {
    /* 0x00 */ ALLink node;   /* in the used list, in the order of startAddr, or in the free list */
    /* 0x08 */ u32 startAddr; /* cartridge address of the first byte (even) */
    /* 0x0C */ u32 lastFrame; /* the frame that used it last (musDmaFrame) */
    /* 0x10 */ PTR32(u8) ptr; /* the data: musDmaBufferSize bytes from the libmus heap */
} MusDmaBuffer; /* size = 0x14 */

/* The DMA routine's state (musDmaState; ROM D_802AD318). The load filters keep a pointer to it, and never use it. */
typedef struct MusDmaState {
    u8 initialized;
    PTR32(MusDmaBuffer) firstUsed;
    PTR32(MusDmaBuffer) firstFree;
} MusDmaState;

/* The output buffer of the port's frames, in stereo frames (mus-glue.c). */
#define MUS_BUFFER_FRAMES 2048
/* The bytes that a request for wave data that the port cannot serve reads. */
#define MUS_DMA_ZEROS_SIZE 0x1000

/* The reverb state (musReverb; ROM gReverbFx, 0x802AD370). One set of buffers serves every preset. */
typedef struct MusReverbState {
    /* 0x00 */ PTR32(ALSynth) synth;
    /* 0x04 */ u8 unused04[0xC];
    /* 0x10 */ PTR32(ALDelay) delays;       /* CustomMaxSections() entries */
    /* 0x14 */ PTR32(s16) delayLine;        /* CustomMaxLength() samples */
    /* 0x18 */ PTR32(ALResampler) resampler; /* shared by every chorus section */
    /* 0x1C */ PTR32(RESAMPLE_STATE) resampleState;
    /* 0x20 */ PTR32(ALLowPass) lowpass;    /* shared by every low-pass section */
    /* 0x24 */ PTR32(POLEF_STATE) lowpassState;
    /* 0x28 */ u8 unused28[8];
} MusReverbState; /* size = 0x30 */

/* ------------------------------------------------------------------------
 * Layout checks
 *
 * libmus takes these structures from musHeap, which is in port_ram, or reads
 * them from the game's data. Each must keep its ROM size and offsets on the
 * host. synth_internal.h checks the rest of the synthesizer's types.
 * ---------------------------------------------------------------------- */

#define MUS_CHECK_SIZE(type, size) SYN_STATIC_ASSERT(sizeof(type) == (size), #type " must keep the ROM's size")
#define MUS_CHECK_OFFSET(type, field, offset) \
    SYN_STATIC_ASSERT(offsetof(type, field) == (offset), #type "." #field " must keep the ROM's offset")

MUS_CHECK_SIZE(PTR32(void), 4);
MUS_CHECK_SIZE(MusConfig, 0x48);
MUS_CHECK_SIZE(MusAudioMgrConfig, 0x0C);
MUS_CHECK_SIZE(MusBank, 0x30);
MUS_CHECK_SIZE(MusMessage, 0x08);
MUS_CHECK_SIZE(MusChannel, 0x134);
MUS_CHECK_OFFSET(MusChannel, vibratoStep, 0xD8);
MUS_CHECK_OFFSET(MusChannel, loopBendValue, 0x124);
MUS_CHECK_SIZE(MusReverbState, 0x30);

/* libaudio types in musHeap or in a pbank */
MUS_CHECK_SIZE(ALVoice, 0x1C);
MUS_CHECK_OFFSET(ALWaveTable, type, 0x08);
MUS_CHECK_OFFSET(ALWaveTable, flags, 0x09);
MUS_CHECK_OFFSET(ALWaveTable, waveInfo, 0x0C);
MUS_CHECK_SIZE(ALRawLoop, 0x0C);
MUS_CHECK_SIZE(ALADPCMloop, 0x2C);
MUS_CHECK_OFFSET(ALADPCMBook, book, 0x08);

/* Synthesizer blocks that CustomSynNew and CustomFxNew allocate */
MUS_CHECK_SIZE(ALSave, 0x1C);
MUS_CHECK_SIZE(ALAuxBus, 0x4C);
MUS_CHECK_SIZE(ALMainBus, 0x20);
MUS_CHECK_SIZE(ALFx, 0x2C);
MUS_CHECK_SIZE(PVoice, 0xDC);
MUS_CHECK_SIZE(ALParam, 0x1C);
MUS_CHECK_SIZE(ALDelay, 0x28);
MUS_CHECK_OFFSET(ALDelay, chorusStep, 0x10);
MUS_CHECK_SIZE(ALResampler, 0x34);
MUS_CHECK_SIZE(RESAMPLE_STATE, 0x20);
MUS_CHECK_SIZE(ALLowPass, 0x30);
MUS_CHECK_SIZE(POLEF_STATE, 0x08);

/* ------------------------------------------------------------------------
 * Globals (ROM address and original name in the comment)
 *
 * These are new globals outside port_ram. gen/ram.S already defines the ROM's
 * libmus symbols at their N64 offsets (D_802AB010, __muscontrol_flag,
 * gReverbFx, ...); the native code does not use those copies, and must not
 * define C globals with those names. alGlobals and the reverb presets are the
 * exceptions: the native code uses their port_ram copies.
 * ---------------------------------------------------------------------- */

extern MusCommand musCommands[MUS_NUM_COMMANDS]; /* 801F40C0 jumptable */

extern s32 musNumChannels;           /* 802AB038 D_802AB038 */
extern ALVoice *musVoices;           /* 802AB03C D_802AB03C */
extern MusChannel *musChannels;      /* 802AB040 D_802AB040 */
extern PTR32(u8) *musFxStreams;       /* 802AB044 D_802AB044 */
extern s32 *musFxPriorities;         /* 802AB048 D_802AB048 */
extern s32 musFrameRate;             /* 802AB04C D_802AB04C: 50 or 60 */
extern s32 musFrameMicros;           /* 802AB050 D_802AB050: 1000000 / musFrameRate */
extern u16 musFxMasterVolume;        /* 802AB054 D_802AB054 */
extern u16 musSongMasterVolume;      /* 802AB056 D_802AB056 */
extern s32 musNextHandle;            /* 802AB058 D_802AB058 */
extern s32 musRandomSeed;            /* 802AB05C D_802AB05C */
extern MusBank *musCurrentBank;      /* 802AB060 D_802AB060 */
extern MusBank *musDefaultBank;      /* 802AB064 D_802AB064 */
extern s32 musSongFxChange;          /* 802AB068 D_802AB068: command 0xAA allowed when 1 */
extern s32 musFxType;                /* 802AB06C D_802AB06C: preset restored by MusSetSongFxChange(0) */
extern u32 musControlFlag;           /* 802AB070 __muscontrol_flag */
extern s32 musFifoRead;              /* 802AB074 D_802AB074 */
extern s32 musFifoWrite;             /* 802AB078 D_802AB078 */
extern s32 musFifoSize;              /* 802AB07C D_802AB07C */
extern MusMessage *musFifo;          /* 802AB080 D_802AB080 */
extern MusReverbState musReverb;     /* 802AD370 gReverbFx */

/*
 * The host objects that the sound player's memory points to. gen/ram.S
 * defines them in port_win, after the game's memory, where a pointer in
 * port_ram can point to them (docs/port64.md). Each must fit the bytes that
 * port_win.h reserves for it.
 */
extern ALGlobals musGlobals;         /* 802AB2C8 D_802AB2C8: the synthesizer instance */
extern ALPlayer musPlayer;           /* 802AB010 D_802AB010 */
extern ALHeap musHeap;               /* 802AB028 D_802AB028 (+0x04 is D_802AB02C, heap.cur) */
extern MusDmaState musDmaState;      /* 802AD318 D_802AD318 */
extern s16 musBuffer[MUS_BUFFER_FRAMES * 2]; /* samples made by the synthesizer for the host (mus-glue.c) */
extern s16 musDropBuffer[MUS_BUFFER_FRAMES * 2]; /* where the frames that nobody hears go */
extern u8 musDmaZeros[MUS_DMA_ZEROS_SIZE];

SYN_STATIC_ASSERT(sizeof(musGlobals) <= PORT_WIN_SIZE_musGlobals, "musGlobals must fit its place in port_win");
SYN_STATIC_ASSERT(sizeof(musPlayer) <= PORT_WIN_SIZE_musPlayer, "musPlayer must fit its place in port_win");
SYN_STATIC_ASSERT(sizeof(musHeap) <= PORT_WIN_SIZE_musHeap, "musHeap must fit its place in port_win");
SYN_STATIC_ASSERT(sizeof(musDmaState) <= PORT_WIN_SIZE_musDmaState, "musDmaState must fit its place in port_win");
SYN_STATIC_ASSERT(sizeof(musBuffer) <= PORT_WIN_SIZE_musBuffer, "musBuffer must fit its place in port_win");
SYN_STATIC_ASSERT(sizeof(musDropBuffer) <= PORT_WIN_SIZE_musDropBuffer, "musDropBuffer must fit its place in port_win");
SYN_STATIC_ASSERT(sizeof(musDmaZeros) <= PORT_WIN_SIZE_musDmaZeros, "musDmaZeros must fit its place in port_win");

/*
 * Reverb presets (ROM 801F4190-801F4380). The values are in the map. The
 * port's loader puts the ROM's copy in port_ram and swaps it to host order
 * (gen/ramtab.c), and libmus reads that copy: ALSynConfig.params holds a
 * pointer to one of them, and a pointer in the sound player's memory must
 * point into port_ram or port_win.
 */
extern s32 D_801F4358[10];
extern s32 D_801F4190[26];
extern s32 D_801F41F8[34];
extern s32 D_801F42A8[34]; /* the last 24 words are zeros, which no code reads */
extern s32 D_801F4330[10];
extern s32 D_801F4280[10];
#define musFxNone D_801F4358
#define musFxSmallRoom D_801F4190
#define musFxBigRoom D_801F41F8
#define musFxChorus D_801F42A8
#define musFxFlange D_801F4330
#define musFxEcho D_801F4280
extern s32 musCustomFxCount;         /* 801F4380 D_801F4380: 6 */
extern s32 *musCustomFxPresets[MUS_NUM_CUSTOM_FX + 1]; /* 801F4384 D_801F4384: NULL terminated */

/* ------------------------------------------------------------------------
 * Public API (names from the ROM's symbols)
 * ---------------------------------------------------------------------- */

s32 MusInitialize(MusConfig *config);
void MusSetMasterVolume(s32 flags, s32 volume);
s32 MusStartSong(u8 *song);
s32 MusStartEffect(s32 number);
s32 MusStartEffect2(s32 number, s32 volume, s32 pan, s32 restartflag, s32 priority);
void MusStop(s32 flags, s32 speed);
s32 MusAsk(s32 flags);
s32 MusHandleStop(s32 handle, s32 speed);
s32 MusHandleAsk(s32 handle);
s32 MusHandleSetVolume(s32 handle, s32 volume);
s32 MusHandleSetPan(s32 handle, s32 pan);
s32 MusHandleSetFreqOffset(s32 handle, f32 offset);
s32 MusHandleSetTempo(s32 handle, s32 tempo);
s32 MusHandleSetReverb(s32 handle, s32 reverb);
void MusBankInitialize(u8 *pbank, u8 *wbank);
s32 MusBankStartSong(u8 *pbank, u8 *song);
s32 MusBankStartEffect(u8 *pbank, s32 number);
s32 MusBankStartEffect2(u8 *pbank, s32 number, s32 volume, s32 pan, s32 restartflag, s32 priority);
MusBank *MusHandleGetPtrAddr(s32 handle);
s32 MusHandlePause(s32 handle);
s32 MusHandleUnPause(s32 handle);
s32 MusSetFxType(s32 fxtype);
s32 MusSetSongFxChange(s32 onoff);

/* ------------------------------------------------------------------------
 * Sequencer commands (table musCommands, index = command byte - 0x80)
 * ---------------------------------------------------------------------- */

u8 *musCmdEnd(MusChannel *ch, u8 *p);               /* 0x80 801BF800 */
u8 *musCmdSetWave(MusChannel *ch, u8 *p);           /* 0x81 801BF824 */
u8 *musCmdPortamentoOn(MusChannel *ch, u8 *p);      /* 0x82 801BF85C */
u8 *musCmdPortamentoOff(MusChannel *ch, u8 *p);     /* 0x83 801BF880 */
u8 *musCmdSetEnvelope(MusChannel *ch, u8 *p);       /* 0x84 801BF88C */
u8 *musCmdSetTempo(MusChannel *ch, u8 *p);          /* 0x85 801BF9E4 */
u8 *musCmdReleaseAfterStart(MusChannel *ch, u8 *p); /* 0x86 801BFAC4 */
u8 *musCmdReleaseBeforeEnd(MusChannel *ch, u8 *p);  /* 0x87 801BFAB0 */
u8 *musCmdVibratoUp(MusChannel *ch, u8 *p);         /* 0x88 801BFAF4 */
u8 *musCmdVibratoDown(MusChannel *ch, u8 *p);       /* 0x89 801BFB88 */
u8 *musCmdVibratoOff(MusChannel *ch, u8 *p);        /* 0x8A 801BFC20 */
u8 *musCmdFixedLength(MusChannel *ch, u8 *p);       /* 0x8B 801BFC34 */
u8 *musCmdReadNextLength(MusChannel *ch, u8 *p);    /* 0x8C 801BFC78 */
u8 *musCmdTranspose(MusChannel *ch, u8 *p);         /* 0x8D 801BFC88 */
u8 *musCmdIgnoreTranspose(MusChannel *ch, u8 *p);   /* 0x8E 801BFC98 */
u8 *musCmdFineTune(MusChannel *ch, u8 *p);          /* 0x8F 801BFCA8 */
u8 *musCmdEnvelopeIndex(MusChannel *ch, u8 *p);     /* 0x90 801BFD0C */
u8 *musCmdKeepEnvelopeOn(MusChannel *ch, u8 *p);    /* 0x91 801BFD6C */
u8 *musCmdKeepEnvelopeOff(MusChannel *ch, u8 *p);   /* 0x92 801BFD7C */
u8 *musCmdTieOn(MusChannel *ch, u8 *p);             /* 0x93 801BFD88 */
u8 *musCmdTieOff(MusChannel *ch, u8 *p);            /* 0x94 801BFD98 */
u8 *musCmdLoopStart(MusChannel *ch, u8 *p);         /* 0x95 801BFDA4 */
u8 *musCmdLoopEnd(MusChannel *ch, u8 *p);           /* 0x96 801BFE90 */
u8 *musCmdTrillOn(MusChannel *ch, u8 *p);           /* 0x97 801BFF40 */
u8 *musCmdTrillOff(MusChannel *ch, u8 *p);          /* 0x98 801BFF60 */
u8 *musCmdVelocityOn(MusChannel *ch, u8 *p);        /* 0x99 801BFF6C */
u8 *musCmdVelocityOff(MusChannel *ch, u8 *p);       /* 0x9A 801BFF7C */
u8 *musCmdFixedVelocity(MusChannel *ch, u8 *p);     /* 0x9B 801BFF88 */
u8 *musCmdSetPan(MusChannel *ch, u8 *p);            /* 0x9C 801BFF9C */
u8 *musCmdSkip2(MusChannel *ch, u8 *p);             /* 0x9D 801BFFBC */
u8 *musCmdDrumMapOn(MusChannel *ch, u8 *p);         /* 0x9E 801BFFC8 */
u8 *musCmdDrumMapOff(MusChannel *ch, u8 *p);        /* 0x9F 801BFFF0 */
u8 *musCmdSkip1(MusChannel *ch, u8 *p);             /* 0xA0 801BFFFC */
u8 *musCmdGoto(MusChannel *ch, u8 *p);              /* 0xA1 801C0008 */
u8 *musCmdSetReverb(MusChannel *ch, u8 *p);         /* 0xA2 801C006C */
u8 *musCmdRandomTranspose(MusChannel *ch, u8 *p);   /* 0xA3 801C007C */
u8 *musCmdRandomVolume(MusChannel *ch, u8 *p);      /* 0xA4 801C00CC */
u8 *musCmdRandomPan(MusChannel *ch, u8 *p);         /* 0xA5 801C011C */
u8 *musCmdSetVolume(MusChannel *ch, u8 *p);         /* 0xA6 801C016C */
u8 *musCmdStartEffect(MusChannel *ch, u8 *p);       /* 0xA7 801C017C */
u8 *musCmdBendRange(MusChannel *ch, u8 *p);         /* 0xA8 801C025C */
u8 *musCmdPanSweep(MusChannel *ch, u8 *p);          /* 0xA9 801C02AC */
u8 *musCmdChangeEffect(MusChannel *ch, u8 *p);      /* 0xAA 801C02BC */

/* ------------------------------------------------------------------------
 * Internal functions (ROM address in the comment)
 * ---------------------------------------------------------------------- */

/* Message queue */
void musFifoInit(s32 length);                      /* 801C1100 */
void musFifoProcess(void);                         /* 801C1184 */
void musFifoExecute(MusMessage *msg);              /* 801C1228 */
s32 musFifoPost(MusMessage *msg);                  /* 801C12A0 */
void musHandleSetFlags(s32 handle, u32 keep, u32 set); /* 801C2BB0 */

/* Player */
ALMicroTime musPlayerHandler(void *player);        /* 801C1338 */
void musChannelNextEvent(MusChannel *ch, s32 index); /* 801C15B8 */
void musVoiceStart(MusChannel *ch, s32 index);     /* 801C19FC */
void musVoiceUpdateVolumePan(MusChannel *ch, s32 index); /* 801C1A90 */
void musSetGroupGain(s32 song, s32 effect); /* port: percent of the volume for songs and for sound effects */
void musVoiceUpdatePitch(MusChannel *ch, s32 index, f32 offset); /* 801C1C1C */
void musEnvelopeStart(MusChannel *ch);             /* 801C1D48 */
void musEnvelopeUpdate(MusChannel *ch);            /* 801C1DB4 */
void musPanSweepStart(MusChannel *ch);             /* 801C1FBC */
void musPanSweepUpdate(MusChannel *ch);            /* 801C1FD8 */
f32 musTrillUpdate(MusChannel *ch);                /* 801C2088 */
f32 musVibratoUpdate(MusChannel *ch);              /* 801C20D8 */
void musVolumeStreamUpdate(MusChannel *ch);        /* 801C2144 */
void musBendStreamUpdate(MusChannel *ch);          /* 801C21FC */
f32 musPow2(f32 x);                                /* 801C2314 */
s32 musRandom(s32 range);                          /* 801C2668 */

/* Channels, banks, songs */
void musBankInit(MusBank *bank, u32 wbank);        /* 801C249C */
void musChannelReset(MusChannel *ch);              /* 801C2768 */
s32 musSongChannelFind(u8 *song, s32 songChannel); /* 801C28A0 */
void musMemSet(void *dst, s32 value, s32 count);   /* 801C2A18 */
void musMemCopy(void *src, void *dst, s32 count);  /* 801C2A4C: note the order (source first) */
s32 musEffectStart(MusChannel *ch, s32 number, s32 volume, s32 pan, s32 priority); /* 801C2B24 */
/* 801C2A84 (relocate an array of offsets) has no port version: the port reads songs with
   musRead32 and musBankInit converts pbank tables itself. */

/* Synthesizer set up and reverb */
void CustomInit(ALGlobals *g, ALSynConfig *c);     /* 801C3710 */
void CustomSynNew(ALSynth *s, ALSynConfig *c);     /* 801C3758 */
ALFx *CustomAllocFX(ALSynth *s, s16 bus, ALSynConfig *c, ALHeap *hp); /* 801C3A58 */
void CustomFxNew(ALFx *fx, ALSynConfig *c, ALHeap *hp); /* 801C3AFC */
void CustomFxSet(s32 *params);                     /* 801C3C34 */
void CustomSetDefaultFx(ALSynConfig *c);           /* 801C3F18 */
s32 CustomMaxSections(void);                       /* 801C3F30 */
s32 CustomMaxLength(void);                         /* 801C3F78 */
s32 ChangeCustomEffect(s32 type);                  /* 801C3FC0 */

/* libultra sinf used by the vibrato; must be the port's bit-exact translation
   of the ROM's __sinf (801C6000), the same one the game code calls. */
f32 __sinf(f32 x);

/* ------------------------------------------------------------------------
 * Platform glue: native replacements (mus-glue.c)
 *
 * ROM functions replaced, not translated:
 *   amCreateAudioMgr 801C2C10  creates the audio thread, DMA buffers, task buffers
 *   amAudioThread    801C3040  thread: waits for retrace messages, builds frames
 *   amHandleFrame    801C3198  alAudioFrame, AI buffer, RSP task to the scheduler
 *   amHandleTaskDone 801C3364  empty check after a task (no effect)
 *   amDmaCallback    801C33A4  copies wave data from ROM into DMA buffers
 *   amDmaNew         801C35B8  returns amDmaCallback
 *   amClearDma       801C35F0  frees DMA buffers unused for two frames
 * ---------------------------------------------------------------------- */

/* Same signature as the ROM's function; MusInitialize calls it unchanged. Native version:
   sets c->dmaproc = amDmaNew, c->outputRate = musAiFrequency(amc->outputRate),
   calls CustomInit(&musGlobals, c) and starts the host audio stream. */
void amCreateAudioMgr(void *sched, ALSynConfig *c, s32 threadPriority, MusAudioMgrConfig *amc,
                      s32 numDmaBuffers, s32 dmaBufferSize, s32 frameRate);

/* ALDMANew for the native synthesizer. alLoadNew calls it with the address of
   the load filter's DMA state word, and amDmaNew stores a pointer to its own
   state there. The returned ALDMAproc gives the address of the bytes at a
   wave address, as a word (src/port/ptr32.h): a cartridge address
   (0xB0000000 + ROM offset, as the game passes the wbanks) is copied from
   port_rom into one of the ROM's DMA buffers in the libmus heap. With
   musControlFlag bit 0 set the address is already a word in memory. A
   request it cannot serve (a 64DD address) reads zeros (musDmaZeros). Only
   the first one is logged. */
ALDMAproc amDmaNew(void *state);

/* Output rate the ROM gets from osAiSetFrequency: 22047 for 22050 on NTSC. */
s32 musAiFrequency(s32 requested);

/* Host audio callback body: locks, renders frames with the native synthesizer
   (which calls musPlayerHandler on its own schedule), unlocks. */
void musAudioRender(s16 *out, s32 frames);

/* Called by MusAsk and MusHandleAsk before they count. The game waits in loops
   on these two functions. With an audio thread this only yields; without one
   (headless runs) it runs one frame of the synthesizer (three or four player
   calls). */
void musAudioPoll(void);

/* Without an audio thread (headless, or no audio device) the frame driver
   calls this once per update with the update's length in video retraces. It
   runs whole frames of the synthesizer while the audio clock stays at or
   behind the game's clock. With an audio thread it does nothing. */
void musAudioAdvance(f32 retraces);

/* Recursive lock shared by the Mus* API and musAudioRender. It also turns on
   flush-to-zero (MXCSR) while it is held, as the N64's FPCSR does. */
void musLock(void);
void musUnlock(void);

/* ------------------------------------------------------------------------
 * What the port provides to libmus
 * ---------------------------------------------------------------------- */

/* The platform layer (src/port/platform.c). s32 is long and s16 is short.
   platform.c can define these with plain C types. */

/* Opens a stereo stream of 16-bit samples at rate Hz; the stream converts to
   the device's rate itself. The platform's audio thread then calls
   render(out, frames) for each block it needs. Returns 1 when the stream
   plays, 0 when no audio thread will call render (headless, or no device). */
s32 port_audio_open(s32 rate, void (*render)(s16 *out, s32 frames));
/* A recursive mutex. libmus takes it before port_audio_open too. */
void port_audio_lock(void);
void port_audio_unlock(void);
/* Gives the processor away for a moment. */
void port_audio_yield(void);
/* Gets the frames that the sound player makes on the game's clock, when set (mus-glue.c). */
extern void (*port_audio_sink)(const s16 *frames, s32 count);

/* src/port/port.h */
extern u8 port_ram[];
extern u8 *port_rom;
extern u32 port_rom_size;
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* The TV type, a libultra global in port_ram (PR/os.h declares it the same
   way, but os.h clashes with the C library's string.h without port_pre.h). */
extern s32 osTvType;
#define MUS_TV_PAL 0
#define MUS_TV_NTSC 1
#define MUS_TV_MPAL 2

/* Where the game's effect tables come from (for checks; the port's loader
   converts them into port_ram, see the map). */
#define MUS_ROM_SFX_LIST 0xEACB4      /* 420 big-endian RAM pointers (libmus_sfx_list) */
#define MUS_ROM_PRIORITY_LIST 0xEB344 /* 420 big-endian s32 (libmus_priority_list) */
#define MUS_SFX_COUNT 420
#define MUS_ROM_DATA_RAM_DELTA 0x800FF000 /* RAM address - this = ROM offset (game data segment) */
#define MUS_PI_ROM_MASK 0x0FFFFFFF        /* cartridge address & this = ROM offset */

#endif /* MUS_INTERNAL_H */
