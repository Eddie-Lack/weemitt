/*
 * synth-load.c: the load filter of libaudio's synthesizer in Glover (USA).
 *
 * Translated from the ROM's machine code (gloveworld asm/C0800.s, lines
 * 10462-10507 and 10630-11398). docs/audio-synth-map.md describes these
 * functions in sections 7 and 12. The ROM address of each function is in the
 * comment above it.
 *
 * The load filter is the first filter of a voice. It reads a wave's data with
 * the DMA routine and makes the commands that decode it into DMEM. Every wave
 * in Glover is ADPCM: frames of 9 bytes that each decode to 16 samples.
 *
 * The DMA routine is libmus's (mus-glue.c). It takes a wave address (a
 * cartridge address in Glover) and returns the word of the data's address in
 * a DMA buffer, as an s32. The wave data stays in the ROM's byte order.
 *
 * Each command is two host-order words, built with shifts. Addresses in
 * commands are words (synth_internal.h), not masked (see the map, section 4). The ROM
 * masked them with 0x1FFFFFFF. alAudioFrame sets segment 0 to base 0 and no
 * other segment is used, so the interpreter reads each address as it is.
 */
PORT_C_BEGIN
#include "synth_internal.h"

/* Bytes of DMEM per 16-bit sample. */
#define SYN_SAMPLE_BYTES 2

/* DMEM transfers are in blocks of 8 bytes; addresses in RDRAM are 8-byte aligned. */
#define SYN_DMA_ALIGN 8

/* A loop pass decodes at a DMEM offset aligned to 32 bytes. */
#define SYN_LOOP_PASS_ALIGN 0x20

/*
 * The byte count of an A_SETBUFF before an A_LOADBUFF of nbytes. It is
 * rounded up to 8 bytes, and 8 more bytes are added when nbytes is already a
 * multiple of 8.
 */
static s32 synLoadSize(s32 nbytes) {
    return nbytes + SYN_DMA_ALIGN - (nbytes & (SYN_DMA_ALIGN - 1));
}

/* ------------------------------------------------------------------------
 * Set up and parameters
 * ---------------------------------------------------------------------- */

/*
 * 801C84A4: sets up a load filter. It takes its decoder state and a block for
 * the loop state from the heap. dmaNew returns the DMA routine and fills
 * dmaState. The filter has no wave until SYN_SET_WAVETABLE.
 */
void alLoadNew(ALLoadFilter *f, ALDMANew dmaNew, ALHeap *hp) {
    alFilterNew(&f->filter, alAdpcmPull, alLoadParam, SYN_FILTER_LOAD);
    f->state = (ADPCM_STATE *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(ADPCM_STATE));
    f->loopState = (ADPCM_STATE *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(ADPCM_STATE));
    f->dma = dmaNew(&f->dmaState);
    f->framePos = 0;
    f->first = 1;
    f->dataAddr = 0;
}

/*
 * 801C8EC0: handles SYN_SET_WAVETABLE and SYN_RESET and ignores every other
 * id. The ROM's function returns no value; this one returns 0.
 *
 * SYN_SET_WAVETABLE does not change framePos or first. The SYN_RESET that
 * comes before each start has set them.
 */
s32 alLoadParam(void *filter, s32 paramID, void *param) {
    ALLoadFilter *f = (ALLoadFilter *)filter;
    ALWaveTable *table;
    ALADPCMBook *book;
    ALADPCMloop *adpcmLoop;
    ALRawLoop *rawLoop;

    switch (paramID) {
        case SYN_SET_WAVETABLE:
            table = (ALWaveTable *)param;
            f->table = table;
            f->dataAddr = (s32)table->base;
            f->sample = 0;
            switch (table->type) {
                case AL_ADPCM_WAVE:
                    f->filter.handler = alAdpcmPull;

                    /* Drop a partial frame at the end. This writes to the bank. */
                    table->len = SYN_ADPCM_FRAME_BYTES * (table->len / SYN_ADPCM_FRAME_BYTES);

                    /* Each predictor has order * 8 coefficients of 2 bytes. */
                    book = table->waveInfo.adpcmWave.book;
                    f->bookSize = book->order * book->npredictors * ADPCMVSIZE * (s32)sizeof(s16);

                    adpcmLoop = table->waveInfo.adpcmWave.loop;
                    if (adpcmLoop != NULL) {
                        f->loop.start = adpcmLoop->start;
                        f->loop.end = adpcmLoop->end;
                        f->loop.count = adpcmLoop->count;
                        alCopy(adpcmLoop->state, f->loopState, sizeof(ADPCM_STATE));
                    } else {
                        f->loop.count = 0;
                        f->loop.end = 0;
                        f->loop.start = 0;
                    }
                    break;

                case AL_RAW16_WAVE:
                    f->filter.handler = alRaw16Pull;
                    rawLoop = table->waveInfo.rawWave.loop;
                    if (rawLoop != NULL) {
                        f->loop.start = rawLoop->start;
                        f->loop.end = rawLoop->end;
                        f->loop.count = rawLoop->count;
                    } else {
                        f->loop.count = 0;
                        f->loop.end = 0;
                        f->loop.start = 0;
                    }
                    break;
            }
            break;

        case SYN_RESET:
            f->framePos = 0;
            f->first = 1;
            f->sample = 0;
            table = f->table;
            if (table != NULL) {
                f->dataAddr = (s32)table->base;
                /* Only the count is reset. The loop's start and end stay. */
                switch (table->type) {
                    case AL_ADPCM_WAVE:
                        if (table->waveInfo.adpcmWave.loop != NULL) {
                            f->loop.count = table->waveInfo.adpcmWave.loop->count;
                        }
                        break;

                    case AL_RAW16_WAVE:
                        if (table->waveInfo.rawWave.loop != NULL) {
                            f->loop.count = table->waveInfo.rawWave.loop->count;
                        }
                        break;
                }
            }
            break;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * ADPCM waves
 * ---------------------------------------------------------------------- */

/*
 * 801C906C: reads nbytes of ADPCM frames at f->dataAddr with the DMA routine
 * and decodes tsam samples to DMEM outp. inp is the DMEM offset of the
 * compressed bytes. flags are those of A_ADPCM: A_INIT starts from silence,
 * A_LOOP starts from the loop state. When nbytes is 0 the decode still runs,
 * from the bytes already in DMEM.
 *
 * The DMA routine returns any address. The load starts at the 8-byte aligned
 * address below it, and the decode skips the extra bytes.
 */
Acmd *synDecodeChunk(Acmd *p, ALLoadFilter *f, s32 tsam, s32 nbytes, s16 outp, s16 inp, u32 flags) {
    s32 dramLoc;
    s32 dramAlign;

    if (nbytes > 0) {
        dramLoc = f->dma(f->dataAddr, nbytes, f->dmaState);
        dramAlign = dramLoc & (SYN_DMA_ALIGN - 1);
        nbytes += dramAlign;
        p = synCmdSetBuffer(p, inp, 0, synLoadSize(nbytes));
        p = synCmdLoadBuffer(p, (u32)(dramLoc - dramAlign));
    } else {
        dramAlign = 0;
    }

    if (flags & A_LOOP) {
        p = synCmdSetLoop(p, (u32)f->loopState);
    }

    p = synCmdSetBuffer(p, inp + dramAlign, outp, tsam * SYN_SAMPLE_BYTES);
    p = synCmdADPCM(p, flags, (u32)f->state);
    f->first = 0;
    return p;
}

/*
 * 801C86E0: makes outCount decoded samples in DMEM and moves *outp to the
 * first of them.
 *
 * The arithmetic assumes that A_ADPCM writes the 16 samples of the last
 * frame it decoded before the new ones. framePos samples of that frame were
 * output by the previous pull (0 means all 16). So the first sample this pull
 * needs is framePos samples into the output, or 16 when framePos is 0.
 *
 * When the wave reaches its loop end in this pull, the samples up to the
 * loop end are decoded first. Then each loop pass decodes from the frame
 * after the one that holds loop.start, starting from the loop state. The
 * loop state stands for the frame that holds loop.start. A_DMEMMOVE then
 * copies the pass's samples, from loop.start on, to the end of the samples
 * so far.
 *
 * Past the end of a wave that does not loop, no data is read. Those samples
 * are cleared to 0 and framePos returns to 0.
 */
Acmd *alAdpcmPull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p) {
    ALLoadFilter *f = (ALLoadFilter *)filter;
    s32 nSam;      /* samples to make before the loop end, or outCount */
    s32 nLeft;     /* samples of the last decoded frame that were not output yet */
    s32 tsam;      /* new samples to decode */
    s32 nframes;   /* frames to read */
    s32 nbytes;    /* bytes to read */
    s32 nover;     /* bytes past the end of the wave */
    s32 overFlow;  /* samples past the end of the wave, cleared to 0 */
    s32 startZero; /* bytes from *outp to the first cleared sample */
    s32 op;        /* DMEM offset of the end of the samples so far */
    s32 passOut;   /* DMEM offset where a loop pass decodes */
    s32 decoded = 0;
    s32 looped;

    /* The load filter does not use the sample time. */
    (void)sampleOffset;

    if (outCount == 0) {
        return p;
    }

    p = synCmdLoadADPCM(p, f->bookSize, PORT_P2W(f->table->waveInfo.adpcmWave.book->book));

    looped = (u32)(outCount + f->sample) > f->loop.end && f->loop.count != 0;
    if (looped) {
        nSam = f->loop.end - f->sample;
    } else {
        nSam = outCount;
    }

    if (f->framePos != 0) {
        nLeft = ADPCMFSIZE - f->framePos;
    } else {
        nLeft = 0;
    }

    tsam = nSam - nLeft;
    if (tsam < 0) {
        tsam = 0;
    }
    nframes = (tsam + ADPCMFSIZE - 1) >> SYN_ADPCM_FRAME_SHIFT;
    nbytes = nframes * SYN_ADPCM_FRAME_BYTES;

    if (looped) {
        p = synDecodeChunk(p, f, tsam, nbytes, *outp, SYN_DMEM_DECODER_IN, f->first);
        if (f->framePos != 0) {
            *outp += f->framePos * SYN_SAMPLE_BYTES;
        } else {
            *outp += ADPCMFSIZE * SYN_SAMPLE_BYTES;
        }

        /* Go back to the loop start. dataAddr points at the frame after the one that holds it. */
        f->framePos = f->loop.start & (ADPCMFSIZE - 1);
        f->sample = f->loop.start;
        f->dataAddr = (s32)f->table->base + SYN_ADPCM_FRAME_BYTES * ((f->loop.start >> SYN_ADPCM_FRAME_SHIFT) + 1);

        op = *outp;
        while (nSam < outCount) {
            /*
             * op plus the size of the previous decode's output (the frame
             * before its new frames, then those frames), rounded down to 32
             * bytes.
             */
            passOut = (op + (nframes + 1) * ADPCMFSIZE * SYN_SAMPLE_BYTES) & ~(SYN_LOOP_PASS_ALIGN - 1);
            op += nSam * SYN_SAMPLE_BYTES;

            /* A count of -1 loops forever. */
            if (f->loop.count != (u32)-1 && f->loop.count != 0) {
                f->loop.count--;
            }

            outCount -= nSam;
            if ((u32)outCount < f->loop.end - f->loop.start) {
                nSam = outCount;
            } else {
                nSam = f->loop.end - f->loop.start;
            }

            /* The first 16 samples of the pass come from the loop state. */
            tsam = nSam - ADPCMFSIZE + f->framePos;
            if (tsam < 0) {
                tsam = 0;
            }
            nframes = (tsam + ADPCMFSIZE - 1) >> SYN_ADPCM_FRAME_SHIFT;
            nbytes = nframes * SYN_ADPCM_FRAME_BYTES;

            p = synDecodeChunk(p, f, tsam, nbytes, (s16)passOut, SYN_DMEM_DECODER_IN, f->first | A_LOOP);
            p = synCmdDMEMMove(p, passOut + f->framePos * SYN_SAMPLE_BYTES, op, nSam * SYN_SAMPLE_BYTES);
        }

        f->sample += outCount;
        f->framePos = (outCount + f->framePos) & (ADPCMFSIZE - 1);
        f->dataAddr += nframes * SYN_ADPCM_FRAME_BYTES;
        return p;
    }

    /* Frames past the end of the wave are not read. Their samples become zeros. */
    nover = f->dataAddr + nbytes - ((s32)f->table->base + f->table->len);
    if (nover < 0) {
        nover = 0;
    }
    overFlow = (nover / SYN_ADPCM_FRAME_BYTES) << SYN_ADPCM_FRAME_SHIFT;
    if (overFlow > (nframes << SYN_ADPCM_FRAME_SHIFT) + nLeft) {
        overFlow = (nframes << SYN_ADPCM_FRAME_SHIFT) + nLeft;
    }
    nbytes -= nover;

    if (overFlow - (overFlow & (ADPCMFSIZE - 1)) < outCount) {
        decoded = 1;
        p = synDecodeChunk(p, f, (nframes << SYN_ADPCM_FRAME_SHIFT) - overFlow, nbytes, *outp, SYN_DMEM_DECODER_IN,
                           f->first);
        if (f->framePos != 0) {
            *outp += f->framePos * SYN_SAMPLE_BYTES;
        } else {
            *outp += ADPCMFSIZE * SYN_SAMPLE_BYTES;
        }
        f->sample += outCount;
        f->framePos = (outCount + f->framePos) & (ADPCMFSIZE - 1);
        f->dataAddr += nframes * SYN_ADPCM_FRAME_BYTES;
    } else {
        /* All of it is past the end: nothing is decoded, and sample stays. */
        f->framePos = 0;
        f->dataAddr += nframes * SYN_ADPCM_FRAME_BYTES;
    }

    if (overFlow != 0) {
        f->framePos = 0;
        if (decoded) {
            startZero = (nLeft + (nframes << SYN_ADPCM_FRAME_SHIFT) - overFlow) * SYN_SAMPLE_BYTES;
        } else {
            startZero = 0;
        }
        p = synCmdClearBuffer(p, startZero + *outp, overFlow * SYN_SAMPLE_BYTES);
    }
    return p;
}

/* ------------------------------------------------------------------------
 * Raw 16-bit waves. No wave in Glover is of this type.
 * ---------------------------------------------------------------------- */

/*
 * 801C8B24: makes outCount samples of a wave of 16-bit samples in DMEM and
 * moves *outp to the first of them. The loop and the end of the wave work as
 * in alAdpcmPull, with 2 bytes per sample. Each loop pass loads at an 8-byte
 * aligned DMEM offset. When the pass's samples do not start there,
 * A_DMEMMOVE moves them to the end of the samples so far.
 *
 * The samples are loaded as they are in the ROM, in big-endian order.
 */
Acmd *alRaw16Pull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p) {
    ALLoadFilter *f = (ALLoadFilter *)filter;
    s32 nSam;      /* samples to make before the loop end */
    s32 nbytes;    /* bytes to read */
    s32 dramLoc;   /* address the DMA routine returned */
    s32 dramAlign; /* bytes from the aligned address below dramLoc to dramLoc */
    s32 dmemAlign; /* bytes from op up to the next 8-byte aligned DMEM offset */
    s32 overFlow;  /* bytes past the end of the wave, cleared to 0 */
    s32 startZero; /* bytes from *outp to the first cleared sample */
    s32 op;        /* DMEM offset of the end of the samples so far */

    /* The load filter does not use the sample time. */
    (void)sampleOffset;

    if (outCount == 0) {
        return p;
    }

    if ((u32)(outCount + f->sample) > f->loop.end && f->loop.count != 0) {
        nSam = f->loop.end - f->sample;
        nbytes = nSam * SYN_SAMPLE_BYTES;
        if (nSam > 0) {
            dramLoc = f->dma(f->dataAddr, nbytes, f->dmaState);
            dramAlign = dramLoc & (SYN_DMA_ALIGN - 1);
            nbytes += dramAlign;
            p = synCmdSetBuffer(p, *outp, 0, synLoadSize(nbytes));
            p = synCmdLoadBuffer(p, (u32)(dramLoc - dramAlign));
        } else {
            dramAlign = 0;
        }
        *outp += dramAlign;

        f->sample = f->loop.start;
        f->dataAddr = (s32)f->table->base + (f->loop.start * SYN_SAMPLE_BYTES);

        op = *outp;
        while (nSam < outCount) {
            op += nSam * SYN_SAMPLE_BYTES;

            /* A count of -1 loops forever. */
            if (f->loop.count != (u32)-1 && f->loop.count != 0) {
                f->loop.count--;
            }

            outCount -= nSam;
            if ((u32)outCount < f->loop.end - f->loop.start) {
                nSam = outCount;
            } else {
                nSam = f->loop.end - f->loop.start;
            }

            nbytes = nSam * SYN_SAMPLE_BYTES;
            dramLoc = f->dma(f->dataAddr, nbytes, f->dmaState);
            dramAlign = dramLoc & (SYN_DMA_ALIGN - 1);
            nbytes += dramAlign;
            if (op & (SYN_DMA_ALIGN - 1)) {
                dmemAlign = SYN_DMA_ALIGN - (op & (SYN_DMA_ALIGN - 1));
            } else {
                dmemAlign = 0;
            }

            p = synCmdSetBuffer(p, op + dmemAlign, 0, synLoadSize(nbytes));
            p = synCmdLoadBuffer(p, (u32)(dramLoc - dramAlign));
            if (dramAlign != 0 || dmemAlign != 0) {
                p = synCmdDMEMMove(p, op + dramAlign + dmemAlign, op, nSam * SYN_SAMPLE_BYTES);
            }
        }

        f->sample += outCount;
        f->dataAddr += outCount * SYN_SAMPLE_BYTES;
        return p;
    }

    /* Bytes past the end of the wave are not read. They become zeros. */
    nbytes = outCount * SYN_SAMPLE_BYTES;
    overFlow = f->dataAddr + nbytes - ((s32)f->table->base + f->table->len);
    if (overFlow < 0) {
        overFlow = 0;
    }
    if (overFlow > nbytes) {
        overFlow = nbytes;
    }

    if (overFlow < nbytes) {
        nbytes -= overFlow;
        if (outCount > 0) {
            dramLoc = f->dma(f->dataAddr, nbytes, f->dmaState);
            dramAlign = dramLoc & (SYN_DMA_ALIGN - 1);
            nbytes += dramAlign;
            p = synCmdSetBuffer(p, *outp, 0, synLoadSize(nbytes));
            p = synCmdLoadBuffer(p, (u32)(dramLoc - dramAlign));
        } else {
            dramAlign = 0;
        }
        *outp += dramAlign;
        f->sample += outCount;
        f->dataAddr += outCount * SYN_SAMPLE_BYTES;
    } else {
        /* All of it is past the end: nothing is read, and sample stays. */
        f->dataAddr += outCount * SYN_SAMPLE_BYTES;
    }

    if (overFlow != 0) {
        startZero = outCount * SYN_SAMPLE_BYTES - overFlow;
        if (startZero < 0) {
            startZero = 0;
        }
        p = synCmdClearBuffer(p, startZero + *outp, overFlow);
    }
    return p;
}
PORT_C_END
