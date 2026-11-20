/* Written for weemitt: the parts of the N64 system interface that the port's code uses. */
#ifndef WEEMITT_PR_LIBAUDIO_H
#define WEEMITT_PR_LIBAUDIO_H

/*
 * The audio library's structures that the port's sound player uses
 * (src/audio/mus and src/audio/synth). The functions are declared in
 * src/audio/synth/synth_internal.h.
 *
 * libmus allocates these structures in the game's memory, so each keeps the
 * N64 layout. A member that holds a pointer is a PTR32 or an FN32
 * (src/port/ptr32.h): 4 bytes on every host.
 */

#include <PR/ultratypes.h>
#include <PR/abi.h>

typedef s32 ALMicroTime;
typedef u8 ALPan;
typedef u8 ALFxId;
typedef void *ALFxRef;

/* Effect types for ALSynConfig.fxType. */
#define AL_FX_NONE      0
#define AL_FX_SMALLROOM 1
#define AL_FX_BIGROOM   2
#define AL_FX_CHORUS    3
#define AL_FX_FLANGE    4
#define AL_FX_ECHO      5
#define AL_FX_CUSTOM    6

/* A node of a doubly linked list. */
typedef struct ALLink_s {
    PTR32(struct ALLink_s) next;
    PTR32(struct ALLink_s) prev;
} ALLink;

typedef struct {
    PTR32(u8) base;
    PTR32(u8) cur;
    s32 len;
    s32 count;
} ALHeap;

/* A load filter's DMA routine, and the routine that makes one for each filter. */
typedef s32 ALDMAprocFn(s32 addr, s32 len, void *state);
typedef ALDMAprocFn *ALDMAproc;
typedef ALDMAproc ALDMANewFn(void *state);
typedef ALDMANewFn *ALDMANew;

/* Wave tables, as the sound banks hold them. */

enum {
    AL_ADPCM_WAVE = 0,
    AL_RAW16_WAVE
};

typedef struct {
    s32 order;
    s32 npredictors;
    s16 book[1]; /* order * npredictors * 8 entries */
} ALADPCMBook;

typedef struct {
    u32 start;
    u32 end;
    u32 count;
    ADPCM_STATE state;
} ALADPCMloop;

typedef struct {
    u32 start;
    u32 end;
    u32 count;
} ALRawLoop;

typedef struct {
    PTR32(ALADPCMloop) loop;
    PTR32(ALADPCMBook) book;
} ALADPCMWaveInfo;

typedef struct {
    PTR32(ALRawLoop) loop;
} ALRAWWaveInfo;

typedef struct ALWaveTable_s {
    u32 base; /* where the samples start: in Glover, an address in the cartridge */
    s32 len;
    u8 type;  /* AL_ADPCM_WAVE or AL_RAW16_WAVE */
    u8 flags;
    union {
        ALADPCMWaveInfo adpcmWave;
        ALRAWWaveInfo rawWave;
    } waveInfo;
} ALWaveTable;

/* The synthesizer. */

typedef ALMicroTime ALVoiceHandlerFn(void *);

typedef struct {
    s32 maxVVoices;
    s32 maxPVoices;
    s32 maxUpdates;
    s32 maxFXbusses;
    FN32(ALDMANewFn) dmaproc;
    PTR32(ALHeap) heap;
    s32 outputRate;
    ALFxId fxType;
    PTR32(s32) params;
} ALSynConfig;

/* A client of the synthesizer. Its handler runs at callTime and returns the time to the next call. */
typedef struct ALPlayer_s {
    PTR32(struct ALPlayer_s) next;
    PTR32(void) clientData;
    FN32(ALVoiceHandlerFn) handler;
    ALMicroTime callTime;
    s32 samplesLeft;
} ALPlayer;

typedef struct ALVoice_s {
    ALLink node;
    PTR32(struct PVoice_s) pvoice;
    PTR32(ALWaveTable) table;
    PTR32(void) clientPrivate;
    s16 state;
    s16 priority;
    s16 fxBus;
    s16 unityPitch;
} ALVoice;

typedef struct ALVoiceConfig_s {
    s16 priority;
    s16 fxBus;
    u8 unityPitch;
} ALVoiceConfig;

typedef struct {
    PTR32(ALPlayer) head;
    ALLink pFreeList;
    ALLink pAllocList;
    ALLink pLameList; /* voices to free at the next frame */
    s32 paramSamples;
    s32 curSamples;
    FN32(ALDMANewFn) dma;
    PTR32(ALHeap) heap;
    PTR32(struct ALParam_s) paramList;
    PTR32(struct ALMainBus_s) mainBus;
    PTR32(struct ALAuxBus_s) auxBus;
    PTR32(struct ALFilter_s) outputFilter;
    s32 numPVoices;
    s32 maxAuxBusses;
    s32 outputRate;
    s32 maxOutSamples;
} ALSynth;

typedef struct {
    ALSynth drvr;
} ALGlobals;

extern PTR32(ALGlobals) alGlobals;

#endif
