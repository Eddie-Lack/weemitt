/*
 * synth-fx.c: the reverb of libaudio's synthesizer in Glover (USA).
 *
 * Translated from the ROM's machine code (gloveworld asm/C0800.s, lines
 * 10045-10089 and 12751-13577). docs/audio-synth-map.md describes these
 * functions in section 14. The ROM address of each function is in the
 * comment above it.
 *
 * The reverb is the filter between the aux bus and the main bus. It mixes the
 * voices' reverb send (aux L and R) into one mono signal and writes it into a
 * delay line. Each section of the reverb reads two taps of the delay line, an
 * input tap and an output tap. It mixes them into each other (feed-forward
 * and feedback), can low-pass filter the output tap and can move it with a
 * chorus. Each section adds its output tap to aux R with its gain. At the end
 * aux R is copied to aux L, and the main bus adds both to the dry mix.
 *
 * libmus sets the reverb up (CustomFxNew and CustomFxSet in mus-reverb.c).
 * Glover uses the big room preset: 4 sections, and only the last one has a
 * low-pass filter. No section has a chorus.
 *
 * Each command is two host-order words, built with shifts. Addresses in
 * commands are words (synth_internal.h; see the map, section 4). The ROM
 * converted them with osVirtualToPhysical. The delay line positions are
 * computed on the words too.
 *
 * Delay line positions. A tap lies up to the delay line's length behind the
 * write position, so its address can be below delayLine before it is
 * wrapped. Pointer arithmetic outside an array is undefined in C. So this
 * file computes every position as a 32-bit address (u32) and compares
 * addresses unsigned, as the ROM does.
 */
PORT_C_BEGIN
#include "synth_internal.h"

/* 1.0 in the low-pass filter's fixed point. A_POLEF's coefficients and gain have 14 fraction bits. */
#define SYN_LOWPASS_ONE 16384.0
#define SYN_LOWPASS_GAIN_ONE 0x4000

/* ------------------------------------------------------------------------
 * The low-pass filter
 * ---------------------------------------------------------------------- */

/*
 * 801C7EB0: sets up a low-pass filter from its coefficient fc. Let a be
 * fc / 2 with 14 fraction bits. The filter is y[n] = (1 - a) * x[n] +
 * a * y[n-1], and gain holds 1 - a. A_POLEF takes its coefficients as an
 * ADPCM predictor. The first row is 0. The second row holds a and its powers
 * a^2 to a^8. A_POLEF works on 8 samples at once.
 */
void init_lpfilter(ALLowPass *lp) {
    s32 i;
    s32 temp = lp->fc >> 1;
    f64 ffc;
    f64 fcoef;

    lp->first = 1;
    lp->gain = SYN_LOWPASS_GAIN_ONE - temp;
    for (i = 0; i < 8; i++) {
        lp->coef[i] = 0;
    }
    lp->coef[8] = temp;

    ffc = (f64)(s16)temp / SYN_LOWPASS_ONE;
    fcoef = ffc;
    for (i = 9; i < SYN_LOWPASS_COEFS; i++) {
        fcoef *= ffc;
        lp->coef[i] = (s16)(s32)(fcoef * SYN_LOWPASS_ONE);
    }
}

/*
 * 801CAF2C: runs the low-pass filter on count samples of DMEM buff, in place.
 * A_LOADADPCM loads the coefficients over the decoder's codebook. Every
 * voice's load filter loads its own codebook again before it decodes.
 */
Acmd *_filterBuffer(ALLowPass *lp, s32 buff, s32 count, Acmd *p) {
    p = synCmdSetBuffer(p, buff, buff, count << 1);
    p = synCmdLoadADPCM(p, SYN_LOWPASS_COEF_BYTES, PORT_P2W(lp->coef));
    /* The flag is the low byte of first, as the ROM loads it. */
    p = synCmdPoleFilter(p, (u8)lp->first, lp->gain, (u32)lp->fstate);
    lp->first = 0;
    return p;
}

/* ------------------------------------------------------------------------
 * The delay line
 * ---------------------------------------------------------------------- */

/*
 * 801CAC4C: loads count samples of the delay line from curr_ptr into DMEM
 * buff. A position below the delay line is wrapped once by its length. A
 * transfer that crosses the end of the delay line is split in two: the part
 * before the end, and the rest from the start of the delay line. The last
 * A_SETBUFF sets the count of the commands that follow.
 */
Acmd *_loadBuffer(ALFx *r, s16 *curr_ptr, s32 buff, s32 count, Acmd *p) {
    u32 curr = PORT_P2W(curr_ptr);
    u32 lineStart = (u32)r->delayLine;
    u32 lineBytes = r->length << 1;
    u32 lineEnd = lineStart + lineBytes;
    u32 updated;
    s32 beforeEnd;
    s32 afterEnd;

    if (curr < lineStart) {
        curr += lineBytes;
    }
    updated = curr + ((u32)count << 1);

    if (lineEnd < updated) {
        /* Samples before and after the end; the ROM shifts the byte differences arithmetically. */
        beforeEnd = (s32)(lineEnd - curr) >> 1;
        afterEnd = (s32)(updated - lineEnd) >> 1;
        p = synCmdSetBuffer(p, buff, 0, beforeEnd << 1);
        p = synCmdLoadBuffer(p, curr);
        p = synCmdSetBuffer(p, buff + (beforeEnd << 1), 0, afterEnd << 1);
        p = synCmdLoadBuffer(p, lineStart);
    } else {
        p = synCmdSetBuffer(p, buff, 0, count << 1);
        p = synCmdLoadBuffer(p, curr);
    }
    p = synCmdSetBuffer(p, 0, 0, count << 1);
    return p;
}

/*
 * 801CADBC: saves count samples of DMEM buff into the delay line at
 * curr_ptr. It wraps and splits as _loadBuffer does. Unlike _loadBuffer, it
 * ends with an A_SETBUFF of count only when it splits.
 */
Acmd *_saveBuffer(ALFx *r, s16 *curr_ptr, s32 buff, s32 count, Acmd *p) {
    u32 curr = PORT_P2W(curr_ptr);
    u32 lineStart = (u32)r->delayLine;
    u32 lineBytes = r->length << 1;
    u32 lineEnd = lineStart + lineBytes;
    u32 updated;
    s32 beforeEnd;
    s32 afterEnd;

    if (curr < lineStart) {
        curr += lineBytes;
    }
    updated = curr + ((u32)count << 1);

    if (lineEnd < updated) {
        beforeEnd = (s32)(lineEnd - curr) >> 1;
        afterEnd = (s32)(updated - lineEnd) >> 1;
        p = synCmdSetBuffer(p, 0, buff, beforeEnd << 1);
        p = synCmdSaveBuffer(p, curr);
        p = synCmdSetBuffer(p, 0, buff + (beforeEnd << 1), afterEnd << 1);
        p = synCmdSaveBuffer(p, lineStart);
        p = synCmdSetBuffer(p, 0, 0, count << 1);
    } else {
        p = synCmdSetBuffer(p, 0, buff, count << 1);
        p = synCmdSaveBuffer(p, curr);
    }
    return p;
}

/* ------------------------------------------------------------------------
 * The chorus
 * ---------------------------------------------------------------------- */

/*
 * 801CAFC8: advances the chorus oscillator by count samples and returns the
 * chorus offset in samples. chorusValue is a phase from -2 to 2, and
 * |chorusValue| - 1 is a triangle wave from -1 to 1. The comparison and the
 * two subtractions are in double precision.
 */
f32 _doModFunc(ALDelay *d, s32 count) {
    f32 val;

    d->chorusValue += d->chorusStep * (f32)count;
    val = d->chorusValue;
    if ((f64)val > 2.0) {
        val = (f32)((f64)val - 4.0);
    }
    d->chorusValue = val;
    if (val < 0.0f) {
        val = -val;
    }
    val = (f32)((f64)val - 1.0);
    return d->chorusDepth * val;
}

/*
 * 801CAA34: loads incount samples of a section's output tap into DMEM buff.
 *
 * Without a chorus the tap is d->output samples behind the write position.
 * With one, the tap moves by the oscillator. The resampler reads count
 * samples from the moving tap and makes incount samples at the pitch fratio.
 * It keeps the fraction of a sample it did not use in delta. The load starts
 * at the 8-byte boundary at or below the tap, and A_SETBUFF's input skips the
 * ramalign samples before the tap.
 */
Acmd *_loadOutputBuffer(ALFx *r, ALDelay *d, s32 buff, s32 incount, Acmd *p) {
    s32 length;
    s32 count;
    s32 ramalign;
    f32 delta;
    f32 fratio;
    f32 fincount;
    u32 outAddr;

    if (d->resampler == NULL) {
        outAddr = (u32)r->inputPos - (d->output << 1);
        return _loadBuffer(r, PORT_W2P(s16, outAddr), buff, incount, p);
    }

    length = (s32)(d->output - d->input);
    delta = _doModFunc(d, incount);
    delta /= (f32)length;
    /* The change of pitch keeps only whole units of A_RESAMPLE's pitch. */
    delta = (f32)(s32)(delta * SYN_PITCH_ONE) / SYN_PITCH_ONE;
    fratio = (f32)(1.0 - (f64)delta);
    fincount = d->resampler->delta + fratio * (f32)incount;
    count = (s32)fincount;
    d->resampler->delta = fincount - (f32)count;

    outAddr = (u32)r->inputPos + ((u32)(d->chorusDelta - d->output) << 1);
    ramalign = (s32)(outAddr & 7) >> 1;
    p = _loadBuffer(r, PORT_W2P(s16, outAddr - ((u32)ramalign << 1)), SYN_DMEM_FX_CHORUS, count + ramalign, p);

    /* The ROM combines the two parts of the input with an OR. It equals their sum. */
    p = synCmdSetBuffer(p, SYN_DMEM_FX_CHORUS | (ramalign << 1), buff, incount << 1);
    /* The flag is the low byte of first, as the ROM loads it. */
    p = synCmdResample(p, (u8)d->resampler->first, (s32)(fratio * SYN_PITCH_ONE), (u32)d->resampler->state);
    d->resampler->first = 0;
    d->chorusDelta += count - incount;
    return p;
}

/* ------------------------------------------------------------------------
 * The reverb filter
 * ---------------------------------------------------------------------- */

/*
 * 801CA480: the reverb's pull function. It pulls the aux bus, which leaves
 * every voice's reverb send in aux L and aux R. It writes their mono mix into
 * the delay line, runs each section, and leaves the result in both aux
 * buffers.
 *
 * outBuff and inBuff are the DMEM buffers of a section's output tap and input
 * tap. When a section's input tap is the previous section's output tap, the
 * two buffers swap and the input is not loaded again. The previous output
 * position is computed with + output, not - output, so in Glover it never
 * equals an input position and the swap never happens. The test is kept as
 * the ROM has it.
 */
Acmd *alFxPull(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p) {
    ALFx *r = (ALFx *)filter;
    ALFilter *source = r->filter.source;
    ALDelay *d;
    s16 i;
    s16 outBuff = SYN_DMEM_FX_TEMP0;
    s16 inBuff = SYN_DMEM_FX_TEMP1;
    s16 swap;
    u32 inAddr;
    u32 outAddr;
    u32 prevOutAddr = 0;
    u32 newPos;

    /* The ROM asserts that source is not NULL (reverb.c, line 75). Its __assert does nothing. */
    p = source->handler(source, outp, outCount, sampleOffset, p);

    /*
     * The mono input: aux L = aux L * 0.7071 + aux R * 0.7071. A_MIXER keeps
     * out and adds in * gain / 32768. The first gain is 0xDA83 (-9597), so
     * aux L keeps (32767 - 9597) / 32768 of itself.
     */
    p = synCmdSetBuffer(p, 0, 0, outCount << 1);
    p = synCmdMixer(p, SYN_FX_MONO_GAIN_L, SYN_DMEM_AUX_L, SYN_DMEM_AUX_L);
    p = synCmdMixer(p, SYN_FX_MONO_GAIN_R, SYN_DMEM_AUX_R, SYN_DMEM_AUX_L);
    p = _saveBuffer(r, r->inputPos, SYN_DMEM_AUX_L, outCount, p);

    /* Aux R collects the sections' outputs. */
    p = synCmdClearBuffer(p, SYN_DMEM_AUX_R, outCount << 1);

    for (i = 0; i < r->sectionCount; i++) {
        d = &r->sections[i];
        inAddr = (u32)r->inputPos - (d->input << 1);
        outAddr = (u32)r->inputPos - (d->output << 1);

        if (inAddr == prevOutAddr) {
            swap = inBuff;
            inBuff = outBuff;
            outBuff = swap;
        } else {
            p = _loadBuffer(r, PORT_W2P(s16, inAddr), inBuff, outCount, p);
        }
        p = _loadOutputBuffer(r, d, outBuff, outCount, p);

        if (d->ffcoef != 0) {
            p = synCmdMixer(p, d->ffcoef, inBuff, outBuff);
            /* The save below repeats this one with the same data. */
            if (d->resampler == NULL && d->lowpass == NULL) {
                p = _saveBuffer(r, PORT_W2P(s16, outAddr), outBuff, outCount, p);
            }
        }
        if (d->fbcoef != 0) {
            p = synCmdMixer(p, d->fbcoef, outBuff, inBuff);
            p = _saveBuffer(r, PORT_W2P(s16, inAddr), inBuff, outCount, p);
        }
        if (d->lowpass != NULL) {
            p = _filterBuffer(d->lowpass, outBuff, outCount, p);
        }
        if (d->resampler == NULL) {
            p = _saveBuffer(r, PORT_W2P(s16, outAddr), outBuff, outCount, p);
        }
        if (d->gain != 0) {
            p = synCmdMixer(p, d->gain, outBuff, SYN_DMEM_AUX_R);
        }
        prevOutAddr = (u32)r->inputPos + (d->output << 1);
    }

    /*
     * Move the write position. The test is >, so the position can equal the
     * end of the delay line. The next save then splits with 0 samples before
     * the end.
     */
    newPos = (u32)r->inputPos + ((u32)outCount << 1);
    r->inputPos = PORT_W2P(s16, newPos);
    if ((u32)r->delayLine + (r->length << 1) < newPos) {
        r->inputPos = PORT_W2P(s16, newPos - (r->length << 1));
    }

    p = synCmdDMEMMove(p, SYN_DMEM_AUX_R, SYN_DMEM_AUX_L, outCount << 1);
    return p;
}

/* 801CA830: the reverb's parameter function. Only SYN_SET_SOURCE does something. */
s32 alFxParam(void *filter, s32 paramID, void *param) {
    ALFilter *f = (ALFilter *)filter;

    if (paramID == SYN_SET_SOURCE) {
        f->source = (ALFilter *)param;
    }
    return 0;
}

/*
 * 801CA844: changes one field of one section. paramID is SYN_FX_PARAM_BASE +
 * section * SYN_FX_PARAMS_PER_SECTION + field (SynFxField), with C's signed
 * division. param points at an s32. The ROM reads it before it checks the
 * field. Nothing in Glover's ROM calls this function.
 */
s32 alFxParamHdl(void *filter, s32 paramID, void *param) {
    ALFx *r = (ALFx *)filter;
    s32 index = paramID - SYN_FX_PARAM_BASE;
    s32 s = index / SYN_FX_PARAMS_PER_SECTION;
    s32 field = index % SYN_FX_PARAMS_PER_SECTION;
    s32 value = *(s32 *)param;
    ALDelay *d;
    ALLowPass *lp;

    switch (field) {
        case SYN_FX_INPUT:
            r->sections[s].input = value & ~7;
            break;
        case SYN_FX_OUTPUT:
            r->sections[s].output = value & ~7;
            break;
        case SYN_FX_FBCOEF:
            r->sections[s].fbcoef = (s16)value;
            break;
        case SYN_FX_FFCOEF:
            r->sections[s].ffcoef = (s16)value;
            break;
        case SYN_FX_GAIN:
            r->sections[s].gain = (s16)value;
            break;
        case SYN_FX_CHORUS_RATE:
            /* The rate is divided in single precision, the rest is in double precision. */
            r->sections[s].chorusStep =
                (f32)(2.0 * (f64)((f32)value / 1000.0f) / (f64)alGlobals->drvr.outputRate);
            break;
        case SYN_FX_CHORUS_DEPTH:
            /* The tap distance is unsigned. */
            d = &r->sections[s];
            d->chorusDepth =
                (f32)((f64)(f32)value / SYN_CHORUS_DEPTH_DIVISOR * (f64)(u32)(d->output - d->input));
            break;
        case SYN_FX_LOWPASS:
            lp = r->sections[s].lowpass;
            if (lp != NULL) {
                lp->fc = (s16)value;
                init_lpfilter(lp);
            }
            break;
    }
    return 0;
}
PORT_C_END
