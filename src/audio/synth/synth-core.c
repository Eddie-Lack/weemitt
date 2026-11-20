/*
 * synth-core.c: the core of libaudio's synthesizer in Glover (USA).
 *
 * Translated from the ROM's machine code (gloveworld asm/C0800.s): the set up
 * (alInit, alClose, alSynNew, alFxNew), the heap and list helpers, the pool of
 * parameter changes (ALParam), the player clock and alAudioFrame.
 * docs/audio-synth-map.md describes them in sections 7, 8 and 14.6. The ROM
 * address of each function is in the comment above it.
 *
 * Glover's libmus builds the synthesizer with its own CustomInit and
 * CustomSynNew (mus-reverb.c). So alInit, alSynNew and alFxNew never run in
 * Glover. They are here for completeness.
 *
 * The native alAudioFrame builds the same command list as the ROM's. The ROM
 * then sent that list to the RSP. The port runs it at once with
 * synExecuteCommands. The samples are in outBuf when alAudioFrame returns.
 */
PORT_C_BEGIN
#include "synth_internal.h"

#include <PR/os.h> /* osSetIntMask */

/* Each heap block starts on a multiple of 16 bytes and has a multiple of 16 bytes. */
#define SYN_HEAP_ALIGN 16

/* A change made in a player's handler takes effect at its call time rounded down to this many samples. */
#define SYN_PARAM_ALIGN 16

/* synNextPlayer's start value: no player is further away than this. */
#define SYN_MAX_DELTA 0x7FFFFFFF

/*
 * alFxNew's presets (ROM 801F4420-801F45AF). Each is a section count, the
 * delay line length in samples, then 8 words per section: input tap, output
 * tap, feedback coefficient, feed-forward coefficient, gain, chorus rate,
 * chorus depth and low-pass coefficient. The port's loader puts the ROM's
 * copy in port_ram and swaps it to host order (gen/ramtab.c). These symbols
 * are that copy. The names follow alFxNew's switch on ALSynConfig.fxType and
 * the AL_FX_* values of libaudio.h.
 */
extern s32 D_801F4420[];
extern s32 D_801F4488[];
extern s32 D_801F4510[];
extern s32 D_801F4538[];
extern s32 D_801F4560[];
extern s32 D_801F4588[];
#define synFxSmallRoom D_801F4420 /* AL_FX_SMALLROOM */
#define synFxBigRoom D_801F4488   /* AL_FX_BIGROOM */
#define synFxEcho D_801F4510      /* AL_FX_ECHO */
#define synFxChorus D_801F4538    /* AL_FX_CHORUS */
#define synFxFlange D_801F4560    /* AL_FX_FLANGE */
#define synFxNone D_801F4588      /* any other type: no sections */

/* ------------------------------------------------------------------------
 * Set up and shut down
 * ---------------------------------------------------------------------- */

/* 801CB190: sets up the synthesizer once. Glover uses libmus's CustomInit instead. */
void alInit(ALGlobals *g, ALSynConfig *c) {
    if (alGlobals == NULL) {
        alGlobals = g;
        alSynNew(&g->drvr, c);
    }
}

/* 801CB1BC: removes every player. The ROM's audio thread calls it when it ends. */
void alClose(ALGlobals *g) {
    if (alGlobals != NULL) {
        alSynDelete(&g->drvr);
        alGlobals = NULL;
    }
}

/* 801CB9A0: removes every player. The voices, filters and the heap stay as they are. */
void alSynDelete(ALSynth *s) {
    s->head = NULL;
}

/* 801CA040: the part of set up that every filter shares. */
void alFilterNew(ALFilter *f, ALFilterHandler handler, ALSetParam setParam, s32 type) {
    f->source = NULL;
    f->handler = handler;
    f->setParam = setParam;
    f->inp = 0;
    f->outp = 0;
    f->type = type;
}

/*
 * 801CB310: builds the filter graph from c. Glover uses libmus's
 * CustomSynNew, which does the same with its own reverb.
 *   save filter <- main bus <- reverb, or the aux bus when fxType is 0
 *   reverb <- aux bus <- each voice's envelope mixer
 *   envelope mixer <- resampler <- load filter
 */
void alSynNew(ALSynth *s, ALSynConfig *c) {
    ALHeap *hp = c->heap;
    ALSave *save;
    PTR32(ALFilter) *sources;
    PVoice *pvoices;
    PVoice *pv;
    ALParam *params;
    s32 i;

    s->head = NULL;
    s->curSamples = 0;
    s->paramSamples = 0;
    s->numPVoices = c->maxPVoices;
    s->maxOutSamples = SYN_MAX_OUT_SAMPLES;
    s->outputRate = c->outputRate;
    s->dma = (ALDMANew)c->dmaproc;

    save = (ALSave *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(ALSave));
    alSaveNew(save);
    s->outputFilter = &save->filter;

    /* One aux bus. It and the main bus each get a source slot per voice. */
    s->auxBus = (ALAuxBus *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(ALAuxBus));
    s->maxAuxBusses = 1;
    sources = (PTR32(ALFilter) *)alHeapDBAlloc(NULL, 0, hp, c->maxPVoices, sizeof(PTR32(ALFilter)));
    alAuxBusNew(s->auxBus, sources, c->maxPVoices);

    s->mainBus = (ALMainBus *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(ALMainBus));
    sources = (PTR32(ALFilter) *)alHeapDBAlloc(NULL, 0, hp, c->maxPVoices, sizeof(PTR32(ALFilter)));
    alMainBusNew(s->mainBus, sources, c->maxPVoices);

    if (c->fxType != AL_FX_NONE) {
        alSynAllocFX(s, 0, c, hp);
    } else {
        alMainBusParam(s->mainBus, SYN_ADD_SOURCE, s->auxBus);
    }

    s->pFreeList.next = NULL;
    s->pFreeList.prev = NULL;
    s->pLameList.next = NULL;
    s->pLameList.prev = NULL;
    s->pAllocList.next = NULL;
    s->pAllocList.prev = NULL;

    pvoices = (PVoice *)alHeapDBAlloc(NULL, 0, hp, c->maxPVoices, sizeof(PVoice));
    for (i = 0; i < c->maxPVoices; i++) {
        pv = &pvoices[i];
        alLink(&pv->node, &s->pFreeList);
        pv->vvoice = NULL;

        alLoadNew(&pv->decoder, s->dma, hp);
        alLoadParam(&pv->decoder, SYN_SET_SOURCE, NULL);
        alResampleNew(&pv->resampler, hp);
        alResampleParam(&pv->resampler, SYN_SET_SOURCE, &pv->decoder);
        alEnvmixerNew(&pv->envmixer, hp);
        alEnvmixerParam(&pv->envmixer, SYN_SET_SOURCE, &pv->resampler);
        alAuxBusParam(s->auxBus, SYN_ADD_SOURCE, &pv->envmixer);
        pv->channelKnob = &pv->envmixer.filter;
    }

    alSaveParam(save, SYN_SET_SOURCE, s->mainBus);

    /* Each change goes on the front of the free list. The list starts with the last one. */
    params = (ALParam *)alHeapDBAlloc(NULL, 0, hp, c->maxUpdates, sizeof(ALParam));
    s->paramList = NULL;
    for (i = 0; i < c->maxUpdates; i++) {
        params[i].next = s->paramList;
        s->paramList = &params[i];
    }

    s->heap = hp;
}

/*
 * 801C7F54: libaudio's own reverb set up. alSynAllocFX calls it, and only
 * alSynNew calls that. Glover uses libmus's CustomFxNew instead. The preset
 * comes from c->fxType. Each section with a chorus gets its own resampler, and
 * each section with a low-pass coefficient gets its own filter.
 */
void alFxNew(ALFx *r, ALSynConfig *c, ALHeap *hp) {
    s32 *param;
    ALDelay *d;
    u16 i; /* 16 bits in the ROM, for the samples and the sections */
    u16 j; /* index of the next word in param; 16 bits in the ROM */
    f32 rateDiv1000;

    alFilterNew(&r->filter, NULL, alFxParam, SYN_FILTER_FX);
    r->filter.handler = alFxPull;
    r->paramHandler = alFxParamHdl;

    switch (c->fxType) {
        case AL_FX_SMALLROOM:
            param = synFxSmallRoom;
            break;
        case AL_FX_BIGROOM:
            param = synFxBigRoom;
            break;
        case AL_FX_CHORUS:
            param = synFxChorus;
            break;
        case AL_FX_FLANGE:
            param = synFxFlange;
            break;
        case AL_FX_ECHO:
            param = synFxEcho;
            break;
        case AL_FX_CUSTOM:
            param = c->params;
            break;
        default:
            param = synFxNone;
            break;
    }

    j = 0;
    r->sectionCount = (u8)param[j++];
    r->length = param[j++];
    r->sections = (ALDelay *)alHeapDBAlloc(NULL, 0, hp, r->sectionCount, sizeof(ALDelay));
    r->delayLine = (s16 *)alHeapDBAlloc(NULL, 0, hp, r->length, sizeof(s16));
    r->inputPos = r->delayLine;

    for (i = 0; i < r->length; i++) {
        r->delayLine[i] = 0;
    }

    for (i = 0; i < r->sectionCount; i++) {
        d = &r->sections[i];
        d->input = param[j++];
        d->output = param[j++];
        d->fbcoef = (s16)param[j++];
        d->ffcoef = (s16)param[j++];
        d->gain = (s16)param[j++];

        if (param[j] != 0) {
            /* The ROM divides the rate in single precision and does the rest in
               double precision. The tap distance is unsigned. */
            rateDiv1000 = (f32)param[j++] / 1000.0f;
            d->chorusStep = (f32)(2.0 * (f64)rateDiv1000 / (f64)c->outputRate);
            d->chorusDepth =
                (f32)((f64)(f32)param[j++] / SYN_CHORUS_DEPTH_DIVISOR * (f64)(u32)(d->output - d->input));
            d->chorusDelta = 0;
            d->chorusValue = 1.0f;
            d->resampler = (ALResampler *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(ALResampler));
            d->resampler->state = (RESAMPLE_STATE *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(RESAMPLE_STATE));
            d->resampler->delta = 0.0f;
            d->resampler->first = 1;
        } else {
            d->resampler = NULL;
            j += 2; /* the rate and depth words */
        }

        if (param[j] != 0) {
            d->lowpass = (ALLowPass *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(ALLowPass));
            d->lowpass->fstate = (POLEF_STATE *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(POLEF_STATE));
            d->lowpass->fc = (s16)param[j++];
            /* The ROM has init_lpfilter's code here, inline, with the same arithmetic. */
            init_lpfilter(d->lowpass);
        } else {
            d->lowpass = NULL;
            j++;
        }
    }
}

/* ------------------------------------------------------------------------
 * Heap and lists
 * ---------------------------------------------------------------------- */

/* 801CB240: the heap starts at base rounded up to 16 bytes. len is not reduced by the rounding. */
void alHeapInit(ALHeap *hp, u8 *base, s32 len) {
    s32 extra = SYN_HEAP_ALIGN - (PORT_P2W(base) & (SYN_HEAP_ALIGN - 1));

    if (extra != SYN_HEAP_ALIGN) {
        hp->base = base + extra;
    } else {
        hp->base = base;
    }
    hp->len = len;
    hp->count = 0;
    hp->cur = hp->base;
}

/*
 * 801CB280: takes num * size bytes, rounded up to 16, from the heap. Returns
 * NULL when they do not fit. file and line are not used. The ROM compares the
 * 32-bit addresses unsigned.
 */
void *alHeapDBAlloc(u8 *file, s32 line, ALHeap *hp, s32 num, s32 size) {
    s32 bytes = (num * size + SYN_HEAP_ALIGN - 1) & ~(SYN_HEAP_ALIGN - 1);
    u32 limit = (u32)hp->base + hp->len;
    u32 next = (u32)hp->cur + bytes;
    u8 *block = NULL;

    (void)file;
    (void)line;
    if (!(limit < next)) {
        block = hp->cur;
        hp->cur += bytes;
    }
    return block;
}

/* 801CB1EC: puts element after the node after. */
void alLink(ALLink *element, ALLink *after) {
    element->next = after->next;
    element->prev = after;
    if (after->next != NULL) {
        after->next->prev = element;
    }
    after->next = element;
}

/* 801CB20C: takes element out of its list. Its own links keep their values. */
void alUnlink(ALLink *element) {
    if (element->next != NULL) {
        element->next->prev = element->prev;
    }
    if (element->prev != NULL) {
        element->prev->next = element->next;
    }
}

/* 801CB2D0: copies len bytes, the first byte first. */
void alCopy(void *src, void *dest, s32 len) {
    u8 *from = (u8 *)src;
    u8 *to = (u8 *)dest;
    s32 i;

    for (i = 0; i < len; i++) {
        *to++ = *from++;
    }
}

/* ------------------------------------------------------------------------
 * Parameter changes and physical voices
 * ---------------------------------------------------------------------- */

/* 801CB78C: takes a change from the pool. Returns NULL when the pool is empty. */
ALParam *__allocParam(void) {
    ALSynth *s = &alGlobals->drvr;
    ALParam *update = NULL;

    if (s->paramList != NULL) {
        update = s->paramList;
        s->paramList = update->next;
        update->next = NULL;
    }
    return update;
}

/* 801CB7B8: gives a change back to the pool. */
void __freeParam(ALParam *param) {
    ALSynth *s = &alGlobals->drvr;

    param->next = s->paramList;
    s->paramList = param;
}

/* 801CB7D0: moves every voice of the lame list to the free list. Glover's lame list is always empty. */
void _collectPVoices(ALSynth *s) {
    ALLink *dl;

    while ((dl = s->pLameList.next) != NULL) {
        alUnlink(dl);
        alLink(dl, &s->pFreeList);
    }
}

/* 801CB824: moves a voice to the lame list. _collectPVoices frees it after the frame. */
void _freePVoice(ALSynth *s, PVoice *pvoice) {
    alUnlink(&pvoice->node);
    alLink(&pvoice->node, &s->pLameList);
}

/* ------------------------------------------------------------------------
 * The player clock
 * ---------------------------------------------------------------------- */

/*
 * 801CB864: microseconds to samples at the output rate, rounded to the
 * nearest. The product is single precision. The division and the addition
 * are double precision, then the result goes back to single precision before
 * the truncation.
 */
s32 synTimeToSamplesExact(ALSynth *s, s32 micros) {
    f32 product = (f32)micros * (f32)s->outputRate;

    return (s32)(f32)((f64)product / SYN_US_PER_SECOND + 0.5);
}

/* 801CB8AC: the same as synTimeToSamplesExact, rounded down to a multiple of 16. */
s32 _timeToSamples(ALSynth *s, s32 micros) {
    f32 product = (f32)micros * (f32)s->outputRate;
    s32 samples = (s32)(f32)((f64)product / SYN_US_PER_SECOND + 0.5);

    return samples & ~(SYN_PARAM_ALIGN - 1);
}

/*
 * 801CB8F8: finds the player with the earliest call time. Puts it in *client
 * and returns its call time. A tie goes to the player nearer the head of the
 * list. The list must not be empty.
 */
s32 synNextPlayer(ALSynth *s, ALPlayer **client) {
    ALPlayer *cl;
    s32 delta = SYN_MAX_DELTA;

    /* ROM: __assert(s->head != NULL), "synthesizer.c" line 307. The ROM's __assert does nothing. */
    *client = NULL;
    for (cl = s->head; cl != NULL; cl = cl->next) {
        if (cl->samplesLeft - s->curSamples < delta) {
            *client = cl;
            delta = cl->samplesLeft - s->curSamples;
        }
    }
    return (*client)->samplesLeft;
}

/*
 * 801CB9B0: adds a player at the head of the list. Its first call is at the
 * current sample time. The ROM turns interrupts off around the change. In the
 * port the caller holds musLock, and osSetIntMask does nothing.
 */
void alSynAddPlayer(ALSynth *s, ALPlayer *client) {
    OSIntMask mask = osSetIntMask(OS_IM_NONE);

    client->samplesLeft = s->curSamples;
    client->next = s->head;
    s->head = client;
    osSetIntMask(mask);
}

/* ------------------------------------------------------------------------
 * The frame
 * ---------------------------------------------------------------------- */

/*
 * 801CB5DC: makes outLen stereo samples in outBuf. Returns the end of the
 * command list and puts the number of commands in *cmdLen.
 *
 * First it calls each player whose call time falls before the end of the
 * frame. Changes made by a call take effect at its call time rounded down to
 * 16 samples. Then it makes the frame in chunks of at most maxOutSamples. Each
 * chunk starts with A_SEGMENT for segment 0 at base 0, and pulls the save
 * filter.
 *
 * With no player it makes no commands, and *cmdLen is 0. Otherwise the port
 * runs the list before it returns.
 */
Acmd *alAudioFrame(Acmd *cmdList, s32 *cmdLen, s16 *outBuf, s32 outLen) {
    ALSynth *s = &alGlobals->drvr;
    Acmd *cmdlEnd = cmdList;
    ALPlayer *client;
    ALFilter *output;
    ALMicroTime delay;
    s32 samples;
    s32 count;
    s16 outp = 0; /* the save filter's outp; no filter reads it */

    if (s->head == NULL) {
        *cmdLen = 0;
        return cmdList;
    }

    /* Run every player whose call time falls before the end of this frame. */
    s->paramSamples = synNextPlayer(s, &client);
    while (s->paramSamples - s->curSamples < outLen) {
        s->paramSamples &= ~(SYN_PARAM_ALIGN - 1);
        delay = client->handler(client);
        samples = synTimeToSamplesExact(s, delay);
        client->samplesLeft += samples;
        s->paramSamples = synNextPlayer(s, &client);
    }
    s->paramSamples &= ~(SYN_PARAM_ALIGN - 1);

    while (outLen > 0) {
        count = outLen < s->maxOutSamples ? outLen : s->maxOutSamples;

        /* The segment command: segment 0, base 0 */
        cmdlEnd->words.w0 = (u32)A_SEGMENT << 24;
        cmdlEnd->words.w1 = 0;
        cmdlEnd++;
        outLen -= count;

        output = s->outputFilter;
        output->setParam(output, SYN_SET_DRAM_OUT, outBuf);
        cmdlEnd = output->handler(output, &outp, count, s->curSamples, cmdlEnd);

        outBuf += count << 1; /* two samples, left and right, per frame */
        s->curSamples += count;
    }

    *cmdLen = cmdlEnd - cmdList;
    _collectPVoices(s);

    /* The ROM's RSP ran the list after this. The port runs it now. */
    synExecuteCommands(cmdList, *cmdLen);
    return cmdlEnd;
}
PORT_C_END
