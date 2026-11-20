/*
 * mus-init.c: libmus set up, pbank conversion, the byte fill and copy
 * helpers, and the libmus globals.
 *
 * Translated from the ROM's machine code: MusInitialize (801C0300),
 * MusBankInitialize (801C0EC0), musBankInit (801C249C), musMemSet (801C2A18)
 * and musMemCopy (801C2A4C). docs/audio-libmus-map.md describes them
 * (sections 5.5, 7 and 11.1).
 *
 * In the ROM, amCreateAudioMgr creates the audio thread, the DMA buffers and
 * the RSP task buffers. The port's version of that function (mus-glue.c)
 * opens a host audio stream instead. MusInitialize calls it with the ROM's
 * arguments.
 *
 * MusInitialize and MusBankInitialize take musLock for their whole body.
 * MusInitialize holds it while amCreateAudioMgr starts the audio stream. The
 * first audio callback then waits until the player and the voices exist.
 *
 * The ROM has one more helper here, at 801C2A84. It adds a base address to
 * every nonzero word of an array of offsets. MusStartSong uses it on the song
 * header and track tables, and musBankInit on the bank header and wave table.
 * The port has no version of it. Songs stay as they are and are read with
 * musRead32. musBankInit converts the bank's offsets itself (musBankPointer).
 */
PORT_C_BEGIN
#include <string.h>

#include "mus_internal.h"

/* ------------------------------------------------------------------------
 * Globals (map section 7)
 *
 * The ROM keeps these in bss at 802AB010-802AB314. gen/ram.S already defines
 * the ROM's names inside port_ram, so these are new globals outside it. Like
 * bss, they start at zero. mus-player.c defines musCommands. mus-reverb.c
 * defines musReverb and the reverb presets. gen/ram.S defines
 * musGlobals, musPlayer and musHeap in port_win (mus_internal.h).
 * ---------------------------------------------------------------------- */

s32 musNumChannels;
ALVoice *musVoices;
MusChannel *musChannels;
PTR32(u8) *musFxStreams;
s32 *musFxPriorities;
s32 musFrameRate;
s32 musFrameMicros;
u16 musFxMasterVolume;
u16 musSongMasterVolume;
s32 musNextHandle;
s32 musRandomSeed;
MusBank *musCurrentBank;
MusBank *musDefaultBank;
s32 musSongFxChange;
s32 musFxType;
u32 musControlFlag;
s32 musFifoRead;
s32 musFifoWrite;
s32 musFifoSize;
MusMessage *musFifo;

/* ------------------------------------------------------------------------
 * Byte helpers
 * ---------------------------------------------------------------------- */

/* 801C2A18: fills count bytes at dst with the low byte of value. */
void musMemSet(void *dst, s32 value, s32 count) {
    u8 *d = (u8 *)dst;

    while (count-- != 0) {
        *d++ = (u8)value;
    }
}

/* 801C2A4C: copies count bytes from src to dst, first byte first. Note the order of the arguments. */
void musMemCopy(void *src, void *dst, s32 count) {
    u8 *s = (u8 *)src;
    u8 *d = (u8 *)dst;

    while (count-- != 0) {
        *d++ = *s++;
    }
}

/* ------------------------------------------------------------------------
 * pbank conversion
 * ---------------------------------------------------------------------- */

/*
 * Reads a big-endian offset from a field of a bank and returns the host
 * pointer it stands for. Offsets count from the bank start. Zero gives NULL:
 * the ROM relocates only nonzero words.
 */
static void *musBankPointer(MusBank *bank, void *field) {
    u32 offset = musRead32((u8 *)field);

    if (offset == 0) {
        return NULL;
    }
    return (u8 *)bank + offset;
}

/*
 * Returns 1 when a wave before index already points at p as its loop or as
 * its ADPCM codebook. That structure was converted with that wave. Glover's
 * banks share no loop and no codebook, but a shared one must be converted
 * only once.
 */
static s32 musBankIsConverted(MusBank *bank, s32 index, void *p) {
    ALWaveTable *wave;
    s32 j;

    for (j = 0; j < index; j++) {
        wave = bank->waves[j];
        if (wave == NULL) {
            continue;
        }
        if ((void *)wave->waveInfo.rawWave.loop == p) {
            return 1;
        }
        if (wave->type == AL_ADPCM_WAVE && (void *)wave->waveInfo.adpcmWave.book == p) {
            return 1;
        }
    }
    return 0;
}

/*
 * Converts a wave's loop to host order. Both loop types begin with start,
 * end and count. An ADPCM loop then holds the decoder state.
 */
static void musLoopToHost(ALRawLoop *loop, u8 type) {
    ALADPCMloop *adpcmLoop;
    s32 k;

    loop->start = musRead32((u8 *)&loop->start);
    loop->end = musRead32((u8 *)&loop->end);
    loop->count = musRead32((u8 *)&loop->count);
    if (type == AL_ADPCM_WAVE) {
        adpcmLoop = (ALADPCMloop *)loop;
        for (k = 0; k < ADPCMFSIZE; k++) {
            adpcmLoop->state[k] = (s16)musRead16((u8 *)&adpcmLoop->state[k]);
        }
    }
}

/*
 * Converts an ADPCM codebook to host order: the order, the number of
 * predictors, then 8 coefficients for each order and predictor.
 */
static void musBookToHost(ALADPCMBook *book) {
    s16 *coef = book->book;
    s32 count;
    s32 k;

    book->order = (s32)musRead32((u8 *)&book->order);
    book->npredictors = (s32)musRead32((u8 *)&book->npredictors);
    count = book->order * book->npredictors * ADPCMVSIZE;
    for (k = 0; k < count; k++) {
        coef[k] = (s16)musRead16((u8 *)&coef[k]);
    }
}

/*
 * 801C249C: prepares a pbank for use. It runs once per bank: it sets the
 * MUS_BANK_INITIALISED bit and returns at once when the bit is already set.
 *
 * The ROM relocates the bank in place. It adds the bank address to the
 * offsets in the header, in the wave table and in each wave's loop and book
 * fields. It adds the wbank address to each wave's base, and it replaces
 * each word of the pitch table with a float. The port does the same and also
 * converts every multi-byte field it touches to host order. The flags word
 * stays big-endian: the test reads it the same way before and after.
 *
 * wbank is the address of the bank's wave data as the game passes it. In
 * Glover it is a cartridge address (0xB0000000 + ROM offset), so each base
 * becomes a cartridge address too. The DMA routine of the native synthesizer
 * turns it into a pointer into port_rom. A base whose top byte is 0xFF is in
 * the 64DD's ROM. It keeps its value.
 */
void musBankInit(MusBank *bank, u32 wbank) {
    ALWaveTable *wave;
    u8 *pitchBytes;
    ALRawLoop *loop;
    u32 base;
    u32 bookOffset;
    s32 i;

    if (musRead32(bank->flags) & MUS_BANK_INITIALISED) {
        return;
    }
    bank->flags[0] |= MUS_BANK_INITIALISED >> 24;

    bank->count = (s32)musRead32((u8 *)&bank->count);
    bank->baseNotes = (u8 *)musBankPointer(bank, &bank->baseNotes);
    bank->pitch = (f32 *)musBankPointer(bank, &bank->pitch);
    bank->waves = (PTR32(ALWaveTable) *)musBankPointer(bank, &bank->waves);
    for (i = 0; i < bank->count; i++) {
        bank->waves[i] = (ALWaveTable *)musBankPointer(bank, &bank->waves[i]);
    }

    pitchBytes = (u8 *)bank->pitch;
    for (i = 0; i < bank->count; i++) {
        /*
         * The first byte of each pitch word is a signed detune in hundredths
         * of a semitone. The base note counts from 0x30, which means no
         * shift. The float that replaces the word is the sum in semitones.
         * The player adds the note number to it.
         */
        bank->pitch[i] = (f32)((f64)(f32)(s8)pitchBytes[i * 4] / 100.0);
        bank->pitch[i] = bank->pitch[i] + (f32)(s8)(bank->baseNotes[i] - 0x30);

        wave = bank->waves[i];

        /*
         * A wave whose flags are set was converted already: the wave table
         * lists it twice. A zero entry would make the ROM read address 9
         * and crash. Glover's banks have neither.
         */
        if (wave == NULL || wave->flags != 0) {
            continue;
        }

        base = musRead32((u8 *)&wave->base);
        if ((base & MUS_WAVE_DISK_TAG) != MUS_WAVE_DISK_TAG) {
            base += wbank;
        }
        wave->base = base;
        wave->len = (s32)musRead32((u8 *)&wave->len);
        wave->flags = MUS_WAVE_RELOCATED;

        /* The loop is the first word of both members of waveInfo. */
        loop = (ALRawLoop *)musBankPointer(bank, &wave->waveInfo.rawWave.loop);
        wave->waveInfo.rawWave.loop = loop;
        if (loop != NULL && !musBankIsConverted(bank, i, loop)) {
            musLoopToHost(loop, wave->type);
        }

        /*
         * The ROM relocates the book of every ADPCM wave, even when its
         * offset is 0. That gives the bank address, which is no codebook,
         * so the port does not convert anything there.
         */
        if (wave->type == AL_ADPCM_WAVE) {
            bookOffset = musRead32((u8 *)&wave->waveInfo.adpcmWave.book);
            wave->waveInfo.adpcmWave.book = (ALADPCMBook *)((u8 *)bank + bookOffset);
            if (bookOffset != 0 && !musBankIsConverted(bank, i, wave->waveInfo.adpcmWave.book)) {
                musBookToHost(wave->waveInfo.adpcmWave.book);
            }
        }
    }

    /* The ROM writes the data cache back here (osWritebackDCacheAll). The host needs nothing. */
}

/* ------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * 801C0300: sets up libmus and the synthesizer. config->heap holds
 * everything: the voices, the channels, the message ring and the
 * synthesizer. Returns the number of heap bytes used. Glover stores it and
 * never reads it.
 */
s32 MusInitialize(MusConfig *config) {
    ALSynConfig synConfig;
    MusAudioMgrConfig amConfig;
    ALVoiceConfig voiceConfig;
    s32 used;
    s32 i;

    musLock();

    musControlFlag = config->control_flag;
    musNumChannels = config->channels;
    musFxStreams = config->default_fxbank;
    musFxPriorities = config->priority;

    if (osTvType == MUS_TV_PAL) {
        musFrameRate = MUS_FPS_PAL;
    } else {
        musFrameRate = MUS_FPS_NTSC;
    }
    musFrameMicros = MUS_US_PER_SECOND / musFrameRate;

    musMemSet(config->heap, 0, config->heap_length);
    alHeapInit(&musHeap, config->heap, config->heap_length);

    musVoices = (ALVoice *)alHeapDBAlloc(NULL, 0, &musHeap, 1, musNumChannels * (s32)sizeof(ALVoice));
    musChannels = (MusChannel *)alHeapDBAlloc(NULL, 0, &musHeap, 1, musNumChannels * (s32)sizeof(MusChannel));
    musFifoInit(config->fifo_length);
    musMemSet(musVoices, 0, musNumChannels * (s32)sizeof(ALVoice));
    musMemSet(musChannels, 0, musNumChannels * (s32)sizeof(MusChannel));

    /* Without a default bank, musCurrentBank keeps its value. */
    if (config->ptr != NULL) {
        musBankInit((MusBank *)config->ptr, config->wbk);
        musCurrentBank = musDefaultBank = (MusBank *)config->ptr;
    } else {
        musDefaultBank = NULL;
    }

    /*
     * The ROM leaves maxFXbusses and params unset. Nothing reads
     * maxFXbusses, and CustomSetDefaultFx sets params. Here both are zero.
     * amCreateAudioMgr sets dmaproc and outputRate.
     */
    memset(&synConfig, 0, sizeof(synConfig));
    synConfig.maxVVoices = musNumChannels;
    synConfig.maxPVoices = musNumChannels;
    synConfig.maxUpdates = config->syn_updates;
    synConfig.dmaproc = NULL;
    synConfig.heap = &musHeap;
    synConfig.outputRate = 0;
    synConfig.fxType = MUS_DEFAULT_FX_TYPE;
    musFxType = MUS_DEFAULT_FX_TYPE;

    amConfig.outputRate = config->syn_output_rate;
    amConfig.framesPerField = config->syn_retraceCount;
    amConfig.maxACMDSize = config->syn_rsp_cmds;

    amCreateAudioMgr(config->sched, &synConfig, config->thread_priority, &amConfig,
                     config->syn_num_dma_bufs, config->syn_dma_buf_size, musFrameRate);

    MusSetMasterVolume(MUSFLAG_EFFECTS | MUSFLAG_SONGS, 0x7FFF);
    musNextHandle = 1;
    musRandomSeed = MUS_RANDOM_SEED;

    musPlayer.next = NULL;
    musPlayer.handler = musPlayerHandler;
    musPlayer.clientData = &musPlayer;
    alSynAddPlayer(&musGlobals.drvr, &musPlayer);

    /* Channel i always plays on voice i. Every voice has the audio thread's priority. */
    for (i = 0; i < musNumChannels; i++) {
        musChannels[i].voiceOn = 0;
        musChannelReset(&musChannels[i]);
        voiceConfig.priority = (s16)config->thread_priority;
        voiceConfig.fxBus = 0;
        voiceConfig.unityPitch = 0;
        alSynAllocVoice(&musGlobals.drvr, &musVoices[i], &voiceConfig);
    }

    used = (s32)(musHeap.cur - musHeap.base);
    musUnlock();
    return used;
}

/* 801C0EC0: prepares another pbank and its wave data. Glover passes the music bank. */
void MusBankInitialize(u8 *pbank, u8 *wbank) {
    musLock();
    musBankInit((MusBank *)pbank, PORT_ROM_ADDR(wbank));
    musUnlock();
}
PORT_C_END
