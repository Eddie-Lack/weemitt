/*
 * synth-voice.c: the voice API, the buses, the save filter and the resampler
 * of libaudio's synthesizer in Glover (USA).
 *
 * Translated from the ROM's machine code (gloveworld asm/C0800.s).
 * docs/audio-synth-map.md describes these functions in sections 9.1, 9.2, 11
 * and 13. The ROM address of each function is in the comment above it.
 *
 * libmus calls the alSyn* functions from its player handler. Each call queues
 * a timed change (an ALParam) on the voice's envelope mixer. The change's
 * time is ALSynth.paramSamples plus the PVoice's offset. The envelope mixer
 * applies it while it makes the samples of that time.
 *
 * The pull functions here append audio commands to the list that
 * alAudioFrame builds. Each command is two host-order words, built with
 * shifts. Addresses in commands are words (synth_internal.h), not masked
 * (see the map, section 4).
 */
PORT_C_BEGIN
#include "synth_internal.h"

/* ------------------------------------------------------------------------
 * Voices
 * ---------------------------------------------------------------------- */

/*
 * 801CBA00: gives voice a physical voice. Returns 1 when it got one.
 *
 * A stolen voice keeps playing for SYN_STEAL_DELAY samples. Its volume falls
 * to 0 over the first SYN_STEAL_DELAY - SYN_STEAL_FADE_MARGIN of them, then it
 * stops. Every change for its new owner is that many samples later. Glover
 * has a physical voice for each voice, so no voice is ever stolen.
 */
s32 alSynAllocVoice(ALSynth *s, ALVoice *voice, ALVoiceConfig *vc) {
    PVoice *pvoice = NULL;
    ALFilter *f;
    ALParam *update;
    s32 stolen;

    voice->priority = vc->priority;
    voice->table = NULL;
    voice->unityPitch = vc->unityPitch;
    voice->state = 0;
    voice->pvoice = NULL;
    voice->fxBus = vc->fxBus;

    stolen = _allocatePVoice(s, &pvoice, vc->priority);
    if (pvoice != NULL) {
        f = pvoice->channelKnob;
        if (stolen) {
            pvoice->offset = SYN_STEAL_DELAY;
            pvoice->vvoice->pvoice = NULL;

            /* The ROM does not check this change for NULL. */
            update = __allocParam();
            update->time = s->paramSamples;
            update->type = SYN_SET_VOLUME;
            update->value.i = 0;
            update->rampSamples = pvoice->offset - SYN_STEAL_FADE_MARGIN;
            f->setParam(f, SYN_ADD_UPDATE, update);

            update = __allocParam();
            if (update != NULL) {
                update->time = s->paramSamples + pvoice->offset;
                update->type = SYN_STOP_VOICE;
                update->next = NULL;
                f->setParam(f, SYN_ADD_UPDATE, update);
            }
        } else {
            pvoice->offset = 0;
        }
        pvoice->vvoice = voice;
        voice->pvoice = pvoice;
    }
    return pvoice != NULL;
}

/*
 * 801CBB28: finds a physical voice for a voice of the given priority. Puts it
 * in *pvoice and returns 1 when it stole one.
 *
 * A voice of the lame list comes first, then a free voice. Either moves to
 * the allocated list. Else it steals an allocated voice whose owner's
 * priority is at most the lowest found so far, and that is not being stolen
 * already. The last such voice in the list wins. A stolen voice stays in the
 * allocated list. When nothing qualifies, *pvoice is not changed.
 */
s32 _allocatePVoice(ALSynth *s, PVoice **pvoice, s16 priority) {
    ALLink *dl;
    PVoice *pv;
    s32 stolen = 0;

    if ((dl = s->pLameList.next) != NULL || (dl = s->pFreeList.next) != NULL) {
        *pvoice = (PVoice *)dl;
        alUnlink(dl);
        alLink(dl, &s->pAllocList);
        return stolen;
    }

    for (pv = (PVoice *)s->pAllocList.next; pv != NULL; pv = (PVoice *)pv->node.next) {
        if (pv->vvoice->priority <= priority && pv->offset == 0) {
            *pvoice = pv;
            priority = pv->vvoice->priority;
            stolen = 1;
        }
    }
    return stolen;
}

/*
 * The change API. Each function does nothing when the voice has no physical
 * voice or the pool of changes is empty. Else it queues one change on the
 * physical voice's envelope mixer.
 */

/* 801CBBE0: stops the voice. The envelope mixer sends SYN_RESET down the voice. */
void alSynStopVoice(ALSynth *s, ALVoice *voice) {
    ALParam *update;
    ALFilter *f;

    if (voice->pvoice != NULL) {
        update = __allocParam();
        if (update != NULL) {
            update->time = s->paramSamples + voice->pvoice->offset;
            update->type = SYN_STOP_VOICE;
            update->next = NULL;
            f = voice->pvoice->channelKnob;
            f->setParam(f, SYN_ADD_UPDATE, update);
        }
    }
}

/* 801CBC60: starts the voice on table. voice->table is not set. */
void alSynStartVoice(ALSynth *s, ALVoice *voice, ALWaveTable *table) {
    ALParam *update;
    ALFilter *f;

    if (voice->pvoice != NULL) {
        update = __allocParam();
        if (update != NULL) {
            update->time = s->paramSamples + voice->pvoice->offset;
            update->type = SYN_START_VOICE;
            update->value.p = table;
            update->next = NULL;
            update->unityPitch = voice->unityPitch;
            f = voice->pvoice->channelKnob;
            f->setParam(f, SYN_ADD_UPDATE, update);
        }
    }
}

/* 801CBD00: sets the pitch: input samples per output sample. */
void alSynSetPitch(ALSynth *s, ALVoice *voice, f32 ratio) {
    ALParam *update;
    ALFilter *f;

    if (voice->pvoice != NULL) {
        update = __allocParam();
        if (update != NULL) {
            update->time = s->paramSamples + voice->pvoice->offset;
            update->type = SYN_SET_PITCH;
            update->value.f = ratio;
            update->next = NULL;
            f = voice->pvoice->channelKnob;
            f->setParam(f, SYN_ADD_UPDATE, update);
        }
    }
}

/*
 * 801CBD90: ramps the volume to vol over t microseconds. The ramp length is
 * in samples, rounded down to 16: 352 samples for libmus's 16666.
 */
void alSynSetVol(ALSynth *s, ALVoice *voice, s16 vol, ALMicroTime t) {
    ALParam *update;
    ALFilter *f;

    if (voice->pvoice != NULL) {
        update = __allocParam();
        if (update != NULL) {
            update->time = s->paramSamples + voice->pvoice->offset;
            update->type = SYN_SET_VOLUME;
            update->value.i = vol;
            update->rampSamples = _timeToSamples(s, t);
            update->next = NULL;
            f = voice->pvoice->channelKnob;
            f->setParam(f, SYN_ADD_UPDATE, update);
        }
    }
}

/* 801CBE50: sets the reverb send, 0 to 127. */
void alSynSetFXMix(ALSynth *s, ALVoice *voice, u8 fxmix) {
    ALParam *update;
    ALFilter *f;

    if (voice->pvoice != NULL) {
        update = __allocParam();
        if (update != NULL) {
            update->time = s->paramSamples + voice->pvoice->offset;
            update->type = SYN_SET_FX_MIX;
            update->value.i = fxmix;
            update->next = NULL;
            f = voice->pvoice->channelKnob;
            f->setParam(f, SYN_ADD_UPDATE, update);
        }
    }
}

/* 801CBEE0: sets the pan, 0 (left) to 127 (right). */
void alSynSetPan(ALSynth *s, ALVoice *voice, ALPan pan) {
    ALParam *update;
    ALFilter *f;

    if (voice->pvoice != NULL) {
        update = __allocParam();
        if (update != NULL) {
            update->time = s->paramSamples + voice->pvoice->offset;
            update->type = SYN_SET_PAN;
            update->value.i = pan;
            update->next = NULL;
            f = voice->pvoice->channelKnob;
            f->setParam(f, SYN_ADD_UPDATE, update);
        }
    }
}

/*
 * 801CBF70: sets up the reverb of aux bus number bus, between that bus and
 * the main bus. Returns the reverb (an ALFx). Only alSynNew calls it. Glover
 * uses libmus's CustomAllocFX instead.
 */
ALFxRef *alSynAllocFX(ALSynth *s, s16 bus, ALSynConfig *c, ALHeap *hp) {
    ALFx *fx = &s->auxBus[bus].fx[0];

    alFxNew(fx, c, hp);
    alFxParam(fx, SYN_SET_SOURCE, &s->auxBus[bus]);
    alMainBusParam(s->mainBus, SYN_ADD_SOURCE, fx);
    return (ALFxRef *)fx;
}

/* ------------------------------------------------------------------------
 * The aux bus: the reverb send of every voice
 * ---------------------------------------------------------------------- */

/* 801C85E0: sources is an array of maxSources filters. maxSources is not checked. */
void alAuxBusNew(ALAuxBus *m, void *sources, s32 maxSources) {
    alFilterNew(&m->filter, alAuxBusPull, alAuxBusParam, SYN_FILTER_AUX_BUS);
    m->sourceCount = 0;
    m->maxSources = maxSources;
    m->sources = (PTR32(ALFilter) *)sources;
}

/*
 * 801C91F0: clears the aux buffers, then pulls each source in turn. Each
 * voice's envelope mixer adds its dry part to the main buffers and its wet
 * part to the aux buffers.
 */
Acmd *alAuxBusPull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p) {
    ALAuxBus *m = (ALAuxBus *)filter;
    PTR32(ALFilter) *sources = m->sources;
    s32 i;

    p = synCmdClearBuffer(p, SYN_DMEM_AUX_L, outCount << 1);
    p = synCmdClearBuffer(p, SYN_DMEM_AUX_R, outCount << 1);
    for (i = 0; i < m->sourceCount; i++) {
        p = sources[i]->handler(sources[i], outp, outCount, sampleOffset, p);
    }
    return p;
}

/* 801C92CC: SYN_ADD_SOURCE adds a source. Every other id is ignored. */
s32 alAuxBusParam(void *filter, s32 paramID, void *param) {
    ALAuxBus *m = (ALAuxBus *)filter;
    PTR32(ALFilter) *sources = m->sources;

    if (paramID == SYN_ADD_SOURCE) {
        sources[m->sourceCount++] = (ALFilter *)param;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * The main bus: the dry mix and the reverb's output
 * ---------------------------------------------------------------------- */

/* 801C863C: sources is an array of maxSources filters. maxSources is not checked. */
void alMainBusNew(ALMainBus *m, void *sources, s32 maxSources) {
    alFilterNew(&m->filter, alMainBusPull, alMainBusParam, SYN_FILTER_MAIN_BUS);
    m->sourceCount = 0;
    m->maxSources = maxSources;
    m->sources = (PTR32(ALFilter) *)sources;
}

/*
 * 801CA060: clears the main buffers, then pulls each source in turn. After
 * each source it adds the aux buffers to the main buffers. In Glover the only
 * source is the reverb, which leaves its output in both aux buffers.
 */
Acmd *alMainBusPull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p) {
    ALMainBus *m = (ALMainBus *)filter;
    PTR32(ALFilter) *sources = m->sources;
    s32 i;

    p = synCmdClearBuffer(p, SYN_DMEM_MAIN_L, outCount << 1);
    p = synCmdClearBuffer(p, SYN_DMEM_MAIN_R, outCount << 1);
    for (i = 0; i < m->sourceCount; i++) {
        p = sources[i]->handler(sources[i], outp, outCount, sampleOffset, p);
        /* This A_SETBUFF sets the byte count of the two A_MIXER commands. */
        p = synCmdSetBuffer(p, 0, 0, outCount << 1);
        p = synCmdMixer(p, SYN_MAIN_BUS_GAIN, SYN_DMEM_AUX_L, SYN_DMEM_MAIN_L);
        p = synCmdMixer(p, SYN_MAIN_BUS_GAIN, SYN_DMEM_AUX_R, SYN_DMEM_MAIN_R);
    }
    return p;
}

/* 801CA1A0: SYN_ADD_SOURCE adds a source. Every other id is ignored. */
s32 alMainBusParam(void *filter, s32 paramID, void *param) {
    ALMainBus *m = (ALMainBus *)filter;
    PTR32(ALFilter) *sources = m->sources;

    if (paramID == SYN_ADD_SOURCE) {
        sources[m->sourceCount++] = (ALFilter *)param;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * The save filter: the frame's output
 * ---------------------------------------------------------------------- */

/* 801C8698 */
void alSaveNew(ALSave *f) {
    alFilterNew(&f->filter, alSavePull, alSaveParam, SYN_FILTER_SAVE);
    f->dramOut = 0;
    f->first = 1;
}

/*
 * 801CB060: pulls the main bus, interleaves the main buffers into stereo
 * samples at DMEM 0, and copies them to dramOut. alAudioFrame sets dramOut
 * before each chunk.
 */
Acmd *alSavePull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p) {
    ALSave *f = (ALSave *)filter;
    ALFilter *source = f->filter.source;

    /* ROM: __assert(source != NULL), "save.c" line 34. The ROM's __assert does nothing. */
    p = source->handler(source, outp, outCount, sampleOffset, p);
    p = synCmdSetBuffer(p, 0, SYN_DMEM_SAVE_OUT, outCount << 1);
    p = synCmdInterleave(p, SYN_DMEM_MAIN_L, SYN_DMEM_MAIN_R);
    p = synCmdSetBuffer(p, 0, SYN_DMEM_SAVE_OUT, outCount << 2);
    p = synCmdSaveBuffer(p, (u32)f->dramOut);
    return p;
}

/* 801CB160: SYN_SET_SOURCE and SYN_SET_DRAM_OUT. Every other id is ignored. */
s32 alSaveParam(void *filter, s32 paramID, void *param) {
    ALSave *f = (ALSave *)filter;

    if (paramID == SYN_SET_SOURCE) {
        f->filter.source = (ALFilter *)param;
    } else if (paramID == SYN_SET_DRAM_OUT) {
        f->dramOut = (s32)PORT_P2W(param);
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * The resampler: pitch
 * ---------------------------------------------------------------------- */

/* 801C8554 */
void alResampleNew(ALResampler *r, ALHeap *hp) {
    alFilterNew(&r->filter, alResamplePull, alResampleParam, SYN_FILTER_RESAMPLE);
    r->state = (RESAMPLE_STATE *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(RESAMPLE_STATE));
    r->delta = 0.0f;
    r->first = 1;
    r->playing = 0;
    r->unityPitch = 0;
    r->unused28 = 0;
    r->unused2C = 0;
    r->ratio = 1.0f;
}

/*
 * 801CA1D0: makes outCount samples at DMEM *outp from the source's samples.
 *
 * The source decodes into DMEM from 0x140 and moves inp to its first valid
 * sample. At unity pitch the resampler copies outCount samples. Else it pulls
 * ratio * outCount input samples and carries the fraction to the next pull.
 * A count of 0 makes no commands.
 */
Acmd *alResamplePull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p) {
    ALResampler *r = (ALResampler *)filter;
    ALFilter *source = r->filter.source;
    s16 inp = SYN_DMEM_DECODER_OUT;
    f32 finCount;
    s32 incount;
    s32 pitch;

    if (outCount == 0) {
        return p;
    }

    if (r->unityPitch) {
        p = source->handler(source, &inp, outCount, sampleOffset, p);
        p = synCmdDMEMMove(p, inp, *outp, outCount << 1);
    } else {
        /*
         * The limit test is in double precision. The limit stored is the
         * nearest f32 to MAX_RATIO (1.99996): 0x3FFFFEB0. Then the ratio is
         * truncated to a multiple of 1/32768 in single precision.
         */
        if ((f64)r->ratio > MAX_RATIO) {
            r->ratio = MAX_RATIO;
        }
        r->ratio = (f32)(s32)(r->ratio * SYN_PITCH_ONE) / SYN_PITCH_ONE;

        finCount = r->delta + r->ratio * (f32)outCount;
        incount = (s32)finCount;
        r->delta = finCount - (f32)incount;

        p = source->handler(source, &inp, incount, sampleOffset, p);
        p = synCmdSetBuffer(p, inp, *outp, outCount << 1);
        pitch = (s32)(r->ratio * SYN_PITCH_ONE);
        p = synCmdResample(p, r->first, pitch, (u32)r->state);
        r->first = 0;
    }
    return p;
}

/*
 * 801CA3BC: handles SYN_SET_SOURCE, SYN_RESET, SYN_START, SYN_SET_PITCH and
 * SYN_SET_UNITY_PITCH. SYN_RESET and SYN_START go on to the source too. Every
 * other id goes to the source unchanged.
 */
s32 alResampleParam(void *filter, s32 paramID, void *param) {
    ALResampler *r = (ALResampler *)filter;
    ALFilter *source;
    SynValue pitch;

    switch (paramID) {
        case SYN_SET_SOURCE:
            r->filter.source = (ALFilter *)param;
            break;

        case SYN_RESET:
            r->delta = 0.0f;
            r->first = 1;
            r->playing = 0;
            r->unityPitch = 0;
            source = r->filter.source;
            if (source != NULL) {
                source->setParam(source, SYN_RESET, NULL);
            }
            break;

        case SYN_START:
            r->playing = 1;
            source = r->filter.source;
            if (source != NULL) {
                source->setParam(source, SYN_START, NULL);
            }
            break;

        case SYN_SET_PITCH:
            /* The pointer holds the bits of an f32. */
            pitch.i = synParamBits(param);
            r->ratio = pitch.f;
            break;

        case SYN_SET_UNITY_PITCH:
            r->unityPitch = 1;
            break;

        default:
            source = r->filter.source;
            if (source != NULL) {
                source->setParam(source, paramID, param);
            }
            break;
    }
    return 0;
}
PORT_C_END
