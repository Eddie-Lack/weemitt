/*
 * mus-reverb.c: the synthesizer set up and the custom reverb of libmus.
 *
 * Translated from the ROM's machine code: CustomInit to ChangeCustomEffect
 * (801C3710-801C4020). docs/audio-libmus-map.md describes them (section 10).
 *
 * libmus does not use libaudio's alInit, alSynNew and alFxNew. CustomSynNew
 * builds the synthesizer with one aux bus. CustomFxNew sizes the reverb's
 * buffers for the largest preset. ChangeCustomEffect switches presets while
 * the game runs, with no new allocation.
 *
 * The filter graph that CustomSynNew builds ("a <- b": b is a source of a):
 *   each voice:  envelope mixer <- resampler <- decoder (load filter)
 *   aux bus 0    <- every voice's envelope mixer
 *   reverb (fx)  <- aux bus 0
 *   main bus     <- reverb, or aux bus 0 itself when fxType is 0
 *   save filter  <- main bus (the last filter, s->outputFilter)
 *
 * The constructors called here (alSaveNew, alAuxBusNew, alLoadNew and the
 * others) belong to the native synthesizer (src/audio/synth). The structures
 * they fill are defined in synth_internal.h.
 *
 * Every block comes from the heap in the ALSynConfig (musHeap, in port_ram),
 * in the ROM's order and with the ROM's sizes. The layout checks in
 * mus_internal.h make the build fail if a structure loses its ROM size.
 *
 * None of this is platform glue. It runs inside MusInitialize (CustomInit)
 * and inside the player (ChangeCustomEffect), which hold musLock.
 */
PORT_C_BEGIN
#include "mus_internal.h"

/* ------------------------------------------------------------------------
 * Data
 * ---------------------------------------------------------------------- */

/* ROM gReverbFx (802AD370). */
MusReverbState musReverb;

s32 musCustomFxCount = MUS_NUM_CUSTOM_FX; /* 801F4380 */

/* Index n is libaudio's AL_FX_* number n (801F4384). The presets are the ROM's copies in port_ram (mus_internal.h). */
s32 *musCustomFxPresets[MUS_NUM_CUSTOM_FX + 1] = {
    musFxNone,      /* AL_FX_NONE */
    musFxSmallRoom, /* AL_FX_SMALLROOM */
    musFxBigRoom,   /* AL_FX_BIGROOM */
    musFxChorus,    /* AL_FX_CHORUS */
    musFxFlange,    /* AL_FX_FLANGE */
    musFxEcho,      /* AL_FX_ECHO */
    NULL,
};

/* ------------------------------------------------------------------------
 * Synthesizer set up
 * ---------------------------------------------------------------------- */

/*
 * 801C3710: sets up the synthesizer, once. alGlobals stays NULL until then.
 * The preset in c becomes the big room preset (CustomSetDefaultFx).
 */
void CustomInit(ALGlobals *g, ALSynConfig *c) {
    if (alGlobals == NULL) {
        alGlobals = g;
        CustomSetDefaultFx(c);
        CustomSynNew(&alGlobals->drvr, c);
    }
}

/*
 * 801C3758: libmus's version of alSynNew. It makes one aux bus, one main bus,
 * c->maxPVoices physical voices and c->maxUpdates parameter updates.
 */
void CustomSynNew(ALSynth *s, ALSynConfig *c) {
    ALHeap *heap = c->heap;
    ALSave *save;
    PTR32(ALFilter) *sources;
    PVoice *pvoices;
    PVoice *pv;
    ALParam *updates;
    s32 i;

    s->head = NULL;
    s->curSamples = 0;
    s->paramSamples = 0;
    s->numPVoices = c->maxPVoices;
    s->maxOutSamples = SYN_MAX_OUT_SAMPLES;
    s->outputRate = c->outputRate;
    s->dma = (ALDMANew)c->dmaproc;

    save = (ALSave *)alHeapDBAlloc(NULL, 0, heap, 1, sizeof(ALSave));
    alSaveNew(save);
    s->outputFilter = &save->filter;

    /* One aux bus. It and the main bus each get a source slot per voice. */
    s->auxBus = (ALAuxBus *)alHeapDBAlloc(NULL, 0, heap, 1, sizeof(ALAuxBus));
    s->maxAuxBusses = 1;
    sources = (PTR32(ALFilter) *)alHeapDBAlloc(NULL, 0, heap, c->maxPVoices, sizeof(PTR32(ALFilter)));
    alAuxBusNew(s->auxBus, sources, c->maxPVoices);

    s->mainBus = (ALMainBus *)alHeapDBAlloc(NULL, 0, heap, 1, sizeof(ALMainBus));
    sources = (PTR32(ALFilter) *)alHeapDBAlloc(NULL, 0, heap, c->maxPVoices, sizeof(PTR32(ALFilter)));
    alMainBusNew(s->mainBus, sources, c->maxPVoices);

    if (c->fxType != AL_FX_NONE) {
        CustomAllocFX(s, 0, c, heap);
    } else {
        alMainBusParam(s->mainBus, SYN_ADD_SOURCE, s->auxBus);
    }

    s->pFreeList.next = NULL;
    s->pFreeList.prev = NULL;
    s->pAllocList.next = NULL;
    s->pAllocList.prev = NULL;
    s->pLameList.next = NULL;
    s->pLameList.prev = NULL;

    pvoices = (PVoice *)alHeapDBAlloc(NULL, 0, heap, c->maxPVoices, sizeof(PVoice));
    for (i = 0; i < c->maxPVoices; i++) {
        pv = &pvoices[i];
        alLink(&pv->node, &s->pFreeList);
        pv->vvoice = NULL;

        alLoadNew(&pv->decoder, s->dma, heap);
        alLoadParam(&pv->decoder, SYN_SET_SOURCE, NULL);
        alResampleNew(&pv->resampler, heap);
        alResampleParam(&pv->resampler, SYN_SET_SOURCE, &pv->decoder);
        alEnvmixerNew(&pv->envmixer, heap);
        alEnvmixerParam(&pv->envmixer, SYN_SET_SOURCE, &pv->resampler);
        alAuxBusParam(s->auxBus, SYN_ADD_SOURCE, &pv->envmixer);
        pv->channelKnob = &pv->envmixer.filter;
    }

    alSaveParam(save, SYN_SET_SOURCE, s->mainBus);

    /* Each update goes on the front of the free list. The list starts with the last one. */
    updates = (ALParam *)alHeapDBAlloc(NULL, 0, heap, c->maxUpdates, sizeof(ALParam));
    s->paramList = NULL;
    for (i = 0; i < c->maxUpdates; i++) {
        updates[i].next = s->paramList;
        s->paramList = &updates[i];
    }

    s->heap = heap;
}

/*
 * 801C3A58: creates the reverb on aux bus `bus` (always 0) and puts it
 * between that bus and the main bus. Returns the reverb.
 */
ALFx *CustomAllocFX(ALSynth *s, s16 bus, ALSynConfig *c, ALHeap *hp) {
    ALFx *fx;

    musReverb.synth = s;
    fx = &s->auxBus[bus].fx[0];
    CustomFxNew(fx, c, hp);
    alFxParam(fx, SYN_SET_SOURCE, &s->auxBus[bus]);
    alMainBusParam(s->mainBus, SYN_ADD_SOURCE, fx);
    return fx;
}

/*
 * 801C3AFC: libmus's version of alFxNew. The delay sections and the delay
 * line are big enough for every preset. One resampler and one low-pass filter
 * serve every section. Then it loads the preset c->params.
 */
void CustomFxNew(ALFx *fx, ALSynConfig *c, ALHeap *hp) {
    alFilterNew(&fx->filter, NULL, alFxParam, SYN_FILTER_FX);
    fx->filter.handler = alFxPull;
    fx->paramHandler = alFxParamHdl;

    musReverb.delays = (ALDelay *)alHeapDBAlloc(NULL, 0, hp, CustomMaxSections(), sizeof(ALDelay));
    musReverb.delayLine = (s16 *)alHeapDBAlloc(NULL, 0, hp, CustomMaxLength(), sizeof(s16));
    musReverb.resampler = (ALResampler *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(ALResampler));
    musReverb.resampleState = (RESAMPLE_STATE *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(RESAMPLE_STATE));
    musReverb.lowpass = (ALLowPass *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(ALLowPass));
    musReverb.lowpassState = (POLEF_STATE *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(POLEF_STATE));

    CustomFxSet(c->params);
}

/* ------------------------------------------------------------------------
 * Presets
 * ---------------------------------------------------------------------- */

/*
 * 801C3C34: loads a preset into the reverb of aux bus 0 and clears the delay
 * line. Every section with a chorus gets the same resampler, and every
 * section with a low-pass filter gets the same filter.
 */
void CustomFxSet(s32 *params) {
    ALFx *fx = &musReverb.synth->auxBus->fx[0];
    ALDelay *d;
    u16 i; /* 16 bits in the ROM, for the samples and the sections */
    u16 k; /* index of the next word in params */
    s32 rate;
    s32 depth;
    s32 lowpassCoef;
    f32 rateDiv1000;

    fx->sectionCount = (u8)params[0];
    fx->length = params[1];
    fx->sections = musReverb.delays;
    fx->delayLine = musReverb.delayLine;
    fx->inputPos = musReverb.delayLine;

    for (i = 0; i < fx->length; i++) {
        fx->delayLine[i] = 0;
    }

    k = 2;
    for (i = 0; i < fx->sectionCount; i++) {
        d = &fx->sections[i];
        d->input = params[k++];
        d->output = params[k++];
        d->fbcoef = (s16)params[k++];
        d->ffcoef = (s16)params[k++];
        d->gain = (s16)params[k++];

        rate = params[k++];
        if (rate != 0) {
            depth = params[k++];
            /* The ROM divides the rate in single precision and does the rest in
               double precision. The tap distance is unsigned. */
            rateDiv1000 = (f32)rate / 1000.0f;
            d->chorusStep = (f32)(2.0 * (f64)rateDiv1000 / (f64)musReverb.synth->outputRate);
            d->chorusValue = 1.0f;
            d->chorusDelta = 0;
            d->chorusDepth =
                (f32)((f64)(f32)depth / SYN_CHORUS_DEPTH_DIVISOR * (f64)(u32)(d->output - d->input));
            d->resampler = musReverb.resampler;
            d->resampler->state = musReverb.resampleState;
            d->resampler->delta = 0.0f;
            d->resampler->first = 1;
        } else {
            d->resampler = NULL;
            k++; /* the depth word */
        }

        lowpassCoef = params[k++];
        if (lowpassCoef != 0) {
            d->lowpass = musReverb.lowpass;
            d->lowpass->fstate = musReverb.lowpassState;
            d->lowpass->fc = (s16)lowpassCoef;
            init_lpfilter(d->lowpass);
        } else {
            d->lowpass = NULL;
        }
    }
}

/* 801C3F18: selects the custom effect type with the big room preset. */
void CustomSetDefaultFx(ALSynConfig *c) {
    c->fxType = AL_FX_CUSTOM;
    c->params = musFxBigRoom;
}

/* 801C3F30: the largest section count of all presets (4 in Glover). */
s32 CustomMaxSections(void) {
    s32 **preset;
    s32 max = 0;

    for (preset = musCustomFxPresets; *preset != NULL; preset++) {
        if (max < (*preset)[0]) {
            max = (*preset)[0];
        }
    }
    return max;
}

/* 801C3F78: the longest delay line of all presets in samples (8000 in Glover). */
s32 CustomMaxLength(void) {
    s32 **preset;
    s32 max = 0;

    for (preset = musCustomFxPresets; *preset != NULL; preset++) {
        if (max < (*preset)[1]) {
            max = (*preset)[1];
        }
    }
    return max;
}

/*
 * 801C3FC0: switches the reverb to preset `type` (an AL_FX_* number).
 * Returns 1 if there is no such preset, else 0. A negative type is not
 * checked, as in the ROM; no caller passes one.
 */
s32 ChangeCustomEffect(s32 type) {
    if (type >= musCustomFxCount) {
        return 1;
    }
    CustomFxSet(musCustomFxPresets[type]);
    /* The source is already aux bus 0. alFxParam stores it again. */
    alFxParam(&musReverb.synth->auxBus->fx[0], SYN_SET_SOURCE, musReverb.synth->auxBus);
    return 0;
}
PORT_C_END
