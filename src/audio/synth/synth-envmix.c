/*
 * synth-envmix.c: the envelope mixer of libaudio's synthesizer in Glover
 * (USA).
 *
 * Translated from the ROM's machine code (gloveworld asm/C0800.s, lines
 * 10411-10458, 11486-12119 and 12206-12425). docs/audio-synth-map.md
 * describes these functions in sections 7, 9.3, 9.4 and 10. The ROM address
 * of each function is in the comment above it.
 *
 * The envelope mixer is the last filter of a voice. It holds the voice's list
 * of timed changes (ALParams) and applies each one at its sample time. It
 * mixes the voice into the dry buffers (main L and R) and into the reverb
 * send (aux L and R). Each side has a volume ramp. The pan and the reverb
 * send take their gains from the equal-power table.
 *
 * Each command is two host-order words, built with shifts. The address of
 * the ENVMIX_STATE is its word (synth_internal.h; see the map, section 4).
 * The ROM converted it with osVirtualToPhysical.
 */
PORT_C_BEGIN
#include "synth_internal.h"

/*
 * The table of base-2 logarithms that synGetRate uses (ROM 8010C4F0). Entry i
 * is log2((17 + 2i) / 32), the middle of the interval [(8 + i) / 16,
 * (9 + i) / 16), rounded to six decimals. These literals give the ROM's
 * doubles exactly.
 */
static const f64 synLog2Table[8] = {
    -0.912537, -0.752072, -0.607683, -0.476438, -0.356144, -0.245112, -0.142019, -0.045804,
};

/* ln 2 (ROM 8010C560, 0x3FE62E42FEFA39EF). */
#define SYN_LN2 0.6931471805599453

/* synGetRate takes the 2^30-th root of the volume ratio, then raises it to the power 2^30 / count. */
#define SYN_ROOT_POWER 1073741824.0

/* ------------------------------------------------------------------------
 * The envelope mixer
 * ---------------------------------------------------------------------- */

/* 801C83EC */
void alEnvmixerNew(ALEnvMixer *e, ALHeap *hp) {
    alFilterNew(&e->filter, alEnvmixerPull, alEnvmixerParam, SYN_FILTER_ENVMIX);
    e->state = (ENVMIX_STATE *)alHeapDBAlloc(NULL, 0, hp, 1, sizeof(ENVMIX_STATE));
    e->first = 1;
    e->playing = 0;
    e->volume = 1;
    e->targetLeft = 1;
    e->targetRight = 1;
    e->volLeft = 1;
    e->volRight = 1;
    e->dryAmount = 0;
    e->wetAmount = 0;
    /*
     * The ROM sets the left rate twice and never sets the right rate.
     * rateRight and rateRightFrac keep the heap's zeros. The first sub-frame
     * computes all four before they are used.
     */
    e->rateLeft = 1;
    e->rateLeftFrac = 0;
    e->rateLeft = 1;
    e->rateLeftFrac = 0;
    e->rampPos = 0;
    e->rampLength = 0;
    e->pan = 0;
    e->updates = NULL;
    e->lastUpdate = NULL;
    e->unused44 = 0;
}

/*
 * 801C9300: mixes outCount samples of the voice at sample time sampleOffset.
 *
 * It applies each due change at its time. Before most changes it mixes the
 * samples up to the change (a sub-frame). A start has no sub-frame before
 * it. Then it mixes the rest of the chunk. outp is not used: the sub-frames
 * write at their own DMEM offsets.
 */
Acmd *alEnvmixerPull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p) {
    ALEnvMixer *e = (ALEnvMixer *)filter;
    ALFilter *source;
    ALParam *u;
    SynStartParams *start;
    PVoice *pvoice;
    SynValue pitch;
    s16 inp = SYN_DMEM_RESAMPLER_OUT; /* where the source writes the next sub-frame */
    s16 mixOffset = 0;                /* where the next sub-frame goes in the dry and aux buffers */
    s32 lastOffset = sampleOffset;
    s32 thisOffset;
    s32 samples;

    while (e->updates != NULL) {
        thisOffset = e->updates->time;
        samples = thisOffset - lastOffset;
        lastOffset = thisOffset;
        if (samples > outCount) {
            /* The change belongs to a later chunk. */
            break;
        }
        /* The ROM calls __assert(samples >= 0) and __assert(samples <= 160) here. Its __assert does nothing. */

        u = e->updates;
        switch (u->type) {
            case SYN_START_VOICE_PARAMS:
                /* Nothing in Glover posts this change. Like SYN_START_VOICE, it has no sub-frame before it. */
                start = (SynStartParams *)u;
                if (start->unityPitch) {
                    e->filter.setParam(e, SYN_SET_UNITY_PITCH, NULL);
                }
                e->filter.setParam(e, SYN_SET_WAVETABLE, start->wave);
                e->filter.setParam(e, SYN_START, NULL);
                e->first = 1;
                e->rampPos = 0;
                e->rampLength = start->rampSamples;
                e->volume = (start->volume * start->volume) >> 15;
                e->pan = start->pan;
                e->dryAmount = synEqPower[start->fxMix];
                e->wetAmount = synEqPower[SYN_EQPOWER_MAX - start->fxMix];
                if (start->rampSamples != 0) {
                    e->volLeft = 1;
                    e->volRight = 1;
                } else {
                    e->volLeft = (e->volume * synEqPower[e->pan]) >> 15;
                    e->volRight = (e->volume * synEqPower[SYN_EQPOWER_MAX - e->pan]) >> 15;
                }
                source = e->filter.source;
                if (source != NULL) {
                    /* The pointer holds the bits of an f32. */
                    pitch.f = start->pitch;
                    source->setParam(source, SYN_SET_PITCH, synParamFromBits(pitch.i));
                }
                break;

            case SYN_START_VOICE:
                /*
                 * There is no sub-frame before a start. If the voice was
                 * playing, it makes no sound between the previous change and
                 * the start. libmus always stops a playing voice first, at
                 * the same time.
                 */
                if (u->unityPitch) {
                    e->filter.setParam(e, SYN_SET_UNITY_PITCH, NULL);
                }
                e->filter.setParam(e, SYN_SET_WAVETABLE, u->value.p);
                e->filter.setParam(e, SYN_START, NULL);
                break;

            case SYN_STOP_VOICE:
                p = synPullSubFrame(e, &inp, &mixOffset, samples, sampleOffset, p);
                e->filter.setParam(e, SYN_RESET, NULL);
                break;

            case SYN_FREE_VOICE:
                /* Nothing in Glover posts this change. */
                pvoice = (PVoice *)u->value.p;
                pvoice->offset = 0;
                _freePVoice(&alGlobals->drvr, pvoice);
                break;

            case SYN_SET_VOLUME:
            case SYN_SET_PAN:
            case SYN_SET_FX_MIX:
                p = synPullSubFrame(e, &inp, &mixOffset, samples, sampleOffset, p);
                if (e->rampPos >= e->rampLength) {
                    /* The ramp has ended: the volumes are at the targets. */
                    e->targetLeft = (e->volume * synEqPower[e->pan]) >> 15;
                    e->targetRight = (e->volume * synEqPower[SYN_EQPOWER_MAX - e->pan]) >> 15;
                    e->rampPos = e->rampLength;
                    e->volLeft = e->targetLeft;
                    e->volRight = e->targetRight;
                } else {
                    /* The ramp is still running: estimate where it has got to. */
                    e->volLeft = (s16)(s32)synGetVol((f32)e->volLeft, e->rampPos, e->rateLeft, e->rateLeftFrac);
                    e->volRight = (s16)(s32)synGetVol((f32)e->volRight, e->rampPos, e->rateRight, e->rateRightFrac);
                }
                /* A ramp multiplies the volume by its rate. A ramp from 0 would stay at 0. */
                if (e->volLeft == 0) {
                    e->volLeft = 1;
                }
                if (e->volRight == 0) {
                    e->volRight = 1;
                }
                if (u->type == SYN_SET_PAN) {
                    e->pan = u->value.i;
                }
                if (u->type == SYN_SET_VOLUME) {
                    e->rampPos = 0;
                    e->volume = (u->value.i * u->value.i) >> 15;
                    e->rampLength = u->rampSamples;
                }
                if (u->type == SYN_SET_FX_MIX) {
                    e->dryAmount = synEqPower[u->value.i];
                    e->wetAmount = synEqPower[SYN_EQPOWER_MAX - u->value.i];
                }
                /* The next sub-frame starts a new ramp, over the full rampLength. */
                e->first = 1;
                break;

            default:
                /* SYN_SET_PITCH in Glover, with the bits of an f32. It goes down the voice to the resampler. */
                p = synPullSubFrame(e, &inp, &mixOffset, samples, sampleOffset, p);
                e->filter.setParam(e, u->type, synParamFromBits(u->value.i));
                break;
        }

        outCount -= samples;
        mixOffset += samples << 1;
        e->updates = u->next;
        if (e->updates == NULL) {
            e->lastUpdate = NULL;
        }
        __freeParam(u);
    }

    p = synPullSubFrame(e, &inp, &mixOffset, outCount, sampleOffset, p);
    if (e->rampPos > e->rampLength) {
        e->rampPos = e->rampLength;
    }
    return p;
}

/*
 * 801C984C: handles SYN_SET_SOURCE, SYN_ADD_UPDATE, SYN_RESET and SYN_START.
 * SYN_RESET and SYN_START go on to the source too. Every other id goes to the
 * source unchanged.
 */
s32 alEnvmixerParam(void *filter, s32 paramID, void *param) {
    ALEnvMixer *e = (ALEnvMixer *)filter;
    ALFilter *source;

    switch (paramID) {
        case SYN_SET_SOURCE:
            e->filter.source = (ALFilter *)param;
            break;

        case SYN_ADD_UPDATE:
            /* Append the change to the list. The list stays in the order of posting. */
            if (e->lastUpdate != NULL) {
                e->lastUpdate->next = (ALParam *)param;
            } else {
                e->updates = (ALParam *)param;
            }
            e->lastUpdate = (ALParam *)param;
            break;

        case SYN_RESET:
            e->first = 1;
            e->playing = 0;
            /* The volume is not kept across a stop. Pan, fx mix and the ramp are kept. */
            e->volume = 1;
            source = e->filter.source;
            if (source != NULL) {
                source->setParam(source, SYN_RESET, param);
            }
            break;

        case SYN_START:
            e->playing = 1;
            source = e->filter.source;
            if (source != NULL) {
                source->setParam(source, SYN_START, param);
            }
            break;

        default:
            source = e->filter.source;
            if (source != NULL) {
                source->setParam(source, paramID, param);
            }
            break;
    }
    return 0;
}

/*
 * 801C9920: pulls outCount samples from the source into DMEM at *inp and
 * mixes them at offset *outp of the dry and aux buffers. Then it moves *inp
 * past them and advances the ramp. It makes no commands when the voice is not
 * playing or outCount is 0. sampleOffset only goes on to the source.
 *
 * After a change (first is set) it computes new targets and rates and sends
 * all the volumes again. The new ramp starts from the current volumes and
 * runs over the full rampLength, even after a pan or fx change in the middle
 * of a ramp. rampPos is not reset by those changes.
 */
Acmd *synPullSubFrame(ALEnvMixer *e, s16 *inp, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p) {
    ALFilter *source = e->filter.source;

    if (e->playing != 1 || outCount == 0) {
        return p;
    }

    /* The ROM calls __assert(source != NULL) here. Its __assert does nothing. */
    p = source->handler(source, inp, outCount, sampleOffset, p);

    p = synCmdSetBuffer(p, *inp, *outp + SYN_DMEM_MAIN_L, outCount << 1);
    p = synCmdSetBufferAux(p, *outp + SYN_DMEM_MAIN_R, *outp + SYN_DMEM_AUX_L, *outp + SYN_DMEM_AUX_R);

    if (e->first) {
        e->first = 0;
        e->targetLeft = (e->volume * synEqPower[e->pan]) >> 15;
        e->rateLeft = synGetRate((f64)e->volLeft, (f64)e->targetLeft, e->rampLength, &e->rateLeftFrac);
        e->targetRight = (e->volume * synEqPower[SYN_EQPOWER_MAX - e->pan]) >> 15;
        e->rateRight = synGetRate((f64)e->volRight, (f64)e->targetRight, e->rampLength, &e->rateRightFrac);

        p = synCmdSetVolume(p, A_VOL | A_LEFT, e->volLeft, 0, 0);
        p = synCmdSetVolume(p, A_VOL | A_RIGHT, e->volRight, 0, 0);
        p = synCmdSetVolume(p, A_RATE | A_LEFT, e->targetLeft, e->rateLeft, e->rateLeftFrac);
        p = synCmdSetVolume(p, A_RATE | A_RIGHT, e->targetRight, e->rateRight, e->rateRightFrac);
        p = synCmdSetVolume(p, A_AUX, e->dryAmount, 0, e->wetAmount);
        p = synCmdEnvMixer(p, A_INIT | A_AUX, (u32)e->state);
    } else {
        p = synCmdEnvMixer(p, A_AUX, (u32)e->state);
    }

    *inp += outCount << 1;
    e->rampPos += outCount;
    return p;
}

/*
 * 801C9D24: the ratio by which the RSP multiplies a volume every 8 samples
 * to take it from vol to tgt in count samples: about (tgt / vol)^(8 / count).
 * Returns the integer part. Stores the fraction times 65535 at *ratel. With
 * count 0 the volume jumps: the ratio is the largest, or 0.
 *
 * Every step is in double precision, in the ROM's order. The ROM writes
 * frexp inline and does not call _frexpf.
 */
s16 synGetRate(f64 vol, f64 tgt, s32 count, u16 *ratel) {
    f64 invCount = 1.0 / (f64)count; /* the ROM divides before it tests count */
    f64 ratio;
    f64 mant;
    f64 root;
    f64 result;
    s32 power;
    s32 exponent = 0;
    s16 whole;

    if (count == 0) {
        if (vol <= tgt) {
            *ratel = 0xFFFF;
            return 0x7FFF;
        }
        *ratel = 0;
        return 0;
    }

    if (tgt < 1.0) {
        tgt = 1.0;
    }
    if (vol <= 0.0) {
        vol = 1.0;
    }

    ratio = tgt / vol;
    power = (s32)(invCount * SYN_ROOT_POWER);

    /* As frexp: ratio = mant * 2^exponent with 0.5 <= mant < 1. Then ratio keeps only the mantissa, with its sign. */
    if (ratio != 0.0) {
        mant = ratio > 0.0 ? ratio : -ratio;
        while (mant >= 1.0) {
            mant *= 0.5;
            exponent++;
        }
        while (mant < 0.5) {
            mant += mant;
            exponent--;
        }
        ratio = ratio > 0.0 ? mant : -mant;
    }

    /*
     * log2(tgt / vol) is about exponent plus the table's entry for the first
     * three bits of the mantissa after the leading 1. Then the 2^30-th root of
     * tgt / vol is about 1 + ln(tgt / vol) / 2^30.
     */
    root = synLog2Table[(s32)(ratio * 16.0) - 8] + (f64)exponent;
    root = root * SYN_LN2;
    root = root / SYN_ROOT_POWER;
    root = root + 1.0;

    /* root^power by squaring: about (tgt / vol)^(1 / count). */
    result = 1.0;
    while (power != 0) {
        if (power & 1) {
            result *= root;
        }
        power >>= 1;
        root *= root;
    }

    /* The eighth power: the ratio per 8 samples. */
    result = result * result;
    result = result * result;
    result = result * result;

    whole = (s16)(s32)result;
    /* The ROM subtracts the integer part after a conversion to f32. */
    *ratel = (u16)(s32)((result - (f64)(f32)whole) * 65535.0);
    return whole;
}

/*
 * 801C9FA4: where a ramp that started at ivol has got to after samples
 * samples: ivol * rate^(samples / 8), in single precision. rate is the 16.16
 * ratio per 8 samples from synGetRate: ratem is its integer part and ratel
 * its fraction.
 */
f32 synGetVol(f32 ivol, s32 samples, s16 ratem, u16 ratel) {
    f32 rate;
    f32 gain = 1.0f;
    s32 i;

    samples >>= SYN_RAMP_BLOCK_SHIFT;
    if (samples == 0) {
        return ivol;
    }

    /* The sum is rounded to f32 before the division. */
    rate = ((f32)(ratem << 16) + (f32)ratel) / 65536.0f;

    /* rate^samples by squaring. */
    for (i = 0; i < 32; i++) {
        if (samples & 1) {
            gain *= rate;
        }
        samples >>= 1;
        if (samples == 0) {
            return ivol * gain;
        }
        rate *= rate;
    }
    return ivol * gain;
}
PORT_C_END
