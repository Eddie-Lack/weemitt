/*
 * mus-voice.c: libmus per-call voice arithmetic.
 *
 * Translated from the ROM's machine code (801C1A90-801C249C). musPlayerHandler
 * calls these for each playing channel. They run the envelope, the pan sweep,
 * the trill, the vibrato and the volume and bend streams. They send volume,
 * pan and pitch to the channel's voice. docs/audio-libmus-map.md describes
 * them (sections 8.3 to 8.7).
 *
 * The arithmetic follows the ROM step by step. Products the ROM makes with
 * multu are 32-bit unsigned. Every f32 and f64 step of the ROM is kept.
 * Channel clocks count 1/256 ticks, so 0x100 is one tick and ">> 8" turns a
 * clock difference into ticks. The volume and bend streams are read a byte at
 * a time, so their byte order needs no handling.
 */
PORT_C_BEGIN
#include <math.h>

#include "mus_internal.h"

/* One tick in channel clock units (8.8 fixed point). */
#define MUS_TICK 0x100

/* Release time of a note that never ends (MUS_LENGTH_INFINITE). */
#define MUS_RELEASE_NEVER 0x7FFFFFFF

/* The volume products are clamped to the largest positive s16. */
#define MUS_VOLUME_LIMIT 0x8000
#define MUS_VOLUME_MAX 0x7FFF

/* The pan sweep counts in 1/64 pan units. */
#define MUS_PAN_SWEEP_ONE 0x40
#define MUS_PAN_MAX 0x7F

/* Stream bend values are centred on this value. */
#define MUS_BEND_CENTRE 64.0

/* 1/12 as the ROM stores it (D_8010C180). It turns semitones into octaves. */
#define MUS_OCTAVES_PER_SEMITONE 0.08333333333333333

/*
 * Coefficients of the series for 2^x: (ln 2)^k / k! for k = 1 to 6, as the
 * ROM stores them (D_8010C188 to D_8010C1B0, again at D_8010C1B8 to
 * D_8010C1E0). Each decimal value gives the ROM's f64 exactly.
 */
#define MUS_POW2_C1 0.693147180559945
#define MUS_POW2_C2 0.240226506959101
#define MUS_POW2_C3 0.0555041086648216
#define MUS_POW2_C4 0.00961812910762848
#define MUS_POW2_C5 0.00133335581464284
#define MUS_POW2_C6 0.000154035303933816

/*
 * Port: the volume of songs and of sound effects, in percent (Settings, Audio). The values live outside the game's
 * memory. The window thread sets them. A change reaches a playing note at the next call, because the volume is sent
 * again when the percent it was sent with differs from the current one.
 */
#define MUS_GAIN_CHANNELS 64
static volatile s32 musSongGain = 100;
static volatile s32 musEffectGain = 100;
static s32 musSentGainOffset[MUS_GAIN_CHANNELS]; /* percent sent to each voice, minus 100 */

void musSetGroupGain(s32 song, s32 effect) {
    musSongGain = song;
    musEffectGain = effect;
}

/*
 * 801C1A90: sends the channel's volume and pan to its voice when they change.
 */
void musVoiceUpdateVolumePan(MusChannel *ch, s32 index) {
    u32 vol;
    s32 gain;
    s32 gainChanged;

    /* Unity is about 127 * 127 * 127 * 128 >> 13 = 0x7D08. */
    vol = (u32)ch->volume * ch->envelopeVolume;
    vol *= ch->velocity;
    vol *= (u32)(s32)ch->handleVolume;
    vol >>= 13;
    if (vol >= MUS_VOLUME_LIMIT) {
        vol = MUS_VOLUME_MAX;
    }
    if (ch->fxNumber == 0) {
        vol *= musSongMasterVolume;
    } else {
        vol *= musFxMasterVolume;
    }
    vol >>= 15;
    if (ch->stopCount != -1) {
        /* Fade out: the volume falls linearly while stopCount counts down to 0. */
        vol = ((u32)ch->stopCount * vol) / (u32)ch->stopLength;
    }
    gain = ch->fxNumber == 0 ? musSongGain : musEffectGain;
    gainChanged = index < MUS_GAIN_CHANNELS && musSentGainOffset[index] != gain - 100;
    if (vol != ch->lastVolume || gainChanged) {
        ch->lastVolume = (u16)vol;
        if (index < MUS_GAIN_CHANNELS) {
            musSentGainOffset[index] = gain - 100;
        }
        /* lastVolume keeps the game's value. Only the synthesizer gets the scaled one. The envelope mixer squares the
           volume it is given, so the square root of the share gives that share of the amplitude. */
        if (gain != 100) {
            vol = (u32)((f64)vol * sqrt((f64)gain / 100.0));
        }
        alSynSetVol(&musGlobals.drvr, &musVoices[index], (s16)vol, musFrameMicros);
    }

    /*
     * lastPan holds the scaled pan but is compared with the raw pan. With a
     * handle pan other than 128 the pan is sent again on every call. The mask
     * wraps a scaled pan above 127.
     */
    if (ch->lastPan != ch->pan) {
        ch->lastPan = (u8)((((s32)ch->pan * ch->handlePan) >> 7) & 0x7F);
        alSynSetPan(&musGlobals.drvr, &musVoices[index], ch->lastPan);
    }
}

/*
 * 801C1C1C: works out the pitch in semitones and sends it to the voice as a
 * ratio when it changes. offset is pitchOffset plus the vibrato and trill
 * offsets.
 */
void musVoiceUpdatePitch(MusChannel *ch, s32 index, f32 offset) {
    f32 pitch;
    f32 ratio;

    pitch = ch->notePitch;
    if (ch->portamentoTicks != 0) {
        if (ch->portamentoTicks >= ch->noteTicks) {
            /* Slide linearly from portamentoFrom to the note over portamentoTicks ticks. */
            pitch = ch->portamentoFrom
                  + ((ch->notePitch - ch->portamentoFrom) / (f32)ch->portamentoTicks) * (f32)ch->noteTicks;
        }
        ch->portamentoPitch = pitch;
    }
    pitch = pitch + (offset + ch->bend);
    if (pitch != ch->lastPitch) {
        ch->lastPitch = pitch;
        ratio = musPow2((f32)((f64)pitch * MUS_OCTAVES_PER_SEMITONE));
        if ((f64)ratio > MUS_PITCH_MAX_RATIO) {
            /* More than an octave up is clamped to one octave, and the note goes silent. */
            ratio = 2.0f;
            ch->velocity = 0;
        }
        alSynSetPitch(&musGlobals.drvr, &musVoices[index], ratio);
    }
}

/*
 * 801C1D48: restarts the envelope at the start of a note. The release starts
 * releaseAfterStart ticks after the note starts, or else releaseBeforeEnd
 * ticks before it ends.
 */
void musEnvelopeStart(MusChannel *ch) {
    if (ch->length != MUS_LENGTH_INFINITE) {
        if (ch->releaseAfterStart != 0) {
            ch->releaseTime = ch->noteStart + ((u32)ch->releaseAfterStart << 8);
        } else {
            ch->releaseTime = ch->noteEnd - ((u32)ch->releaseBeforeEnd << 8);
        }
    } else {
        ch->releaseTime = MUS_RELEASE_NEVER;
    }
    ch->envelopePhase = MUS_ENV_ATTACK;
    ch->envelopeVolume = ch->envelopeInitial;
    ch->envelopeCounter = ch->envelopeSpeed;
}

/*
 * 801C1DB4: advances the envelope. The level changes once every envelopeSpeed
 * calls. Each phase measures its progress in steps: ticks * envelopeScale /
 * 1024, where envelopeScale is 1024 / envelopeSpeed. envelopeVolume is a byte,
 * so each sum is truncated to 8 bits.
 */
void musEnvelopeUpdate(MusChannel *ch) {
    u32 steps;

    if (ch->time >= ch->releaseTime && ch->envelopePhase < MUS_ENV_RELEASE) {
        ch->envelopePhase = MUS_ENV_RELEASE;
        ch->envelopeCounter = 1;
        ch->releaseVolume = ch->envelopeVolume;
    }
    if (--ch->envelopeCounter != 0) {
        return;
    }
    ch->envelopeCounter = ch->envelopeSpeed;

    switch (ch->envelopePhase) {
        case MUS_ENV_ATTACK:
            steps = (((ch->time - ch->noteStart) >> 8) * (u32)ch->envelopeScale) >> 10;
            if ((s32)steps < ch->attackSteps) {
                ch->envelopeVolume = ch->envelopeInitial + (s32)(ch->attackStep * (f32)(s32)steps);
            } else {
                ch->envelopePhase = MUS_ENV_DECAY;
                ch->envelopeVolume = ch->envelopePeak;
            }
            break;

        case MUS_ENV_DECAY:
            /* The decay counts from the end of the attack. */
            steps = ((((ch->time - ch->noteStart) >> 8) - ch->attackSteps) * (u32)ch->envelopeScale) >> 10;
            if ((s32)steps < ch->decaySteps) {
                ch->envelopeVolume = ch->envelopePeak + (s32)(ch->decayStep * (f32)(s32)steps);
            } else {
                ch->envelopePhase = MUS_ENV_SUSTAIN;
                ch->envelopeVolume = ch->envelopeSustain;
            }
            break;

        case MUS_ENV_SUSTAIN:
            break;

        case MUS_ENV_RELEASE:
            /* The release falls from releaseVolume to 0 over releaseSteps steps. */
            steps = (((ch->time - ch->releaseTime) >> 8) * (u32)ch->envelopeScale) >> 10;
            if ((s32)steps < ch->releaseSteps) {
                ch->envelopeVolume =
                    ch->releaseVolume - (s32)((ch->releaseStep * (f32)(s32)steps) * (f32)ch->releaseVolume);
            } else {
                ch->envelopePhase = MUS_ENV_DONE;
                ch->envelopeVolume = 0;
            }
            break;
    }
}

/*
 * 801C1FBC: restarts the pan sweep at the start of a note. A pan in the right
 * half (64 and above) sweeps left first.
 */
void musPanSweepStart(MusChannel *ch) {
    ch->panSweepFraction = 0;
    ch->panSweepDown = ch->pan & 0x40;
    ch->panSweepTime = ch->noteStart;
}

/*
 * 801C1FD8: moves the pan by panSweepSpeed / 64 per tick, for each tick up to
 * the channel clock. The pan bounces between 0 and 127. pan is a byte: the
 * tests read the stored byte, so a pan below 0 wraps to 0x80 or above.
 */
void musPanSweepUpdate(MusChannel *ch) {
    u32 sum;
    u32 step;

    do {
        ch->panSweepTime += MUS_TICK;
        sum = ch->panSweepFraction + ch->panSweepSpeed;
        if (sum < MUS_PAN_SWEEP_ONE) {
            ch->panSweepFraction = sum;
        } else {
            ch->panSweepFraction = sum & (MUS_PAN_SWEEP_ONE - 1);
            step = sum >> 6;
            if (!ch->panSweepDown) {
                ch->pan += step;
                if (ch->pan > MUS_PAN_MAX) {
                    ch->pan = MUS_PAN_MAX;
                    ch->panSweepDown = 1;
                }
            } else {
                ch->pan -= step;
                if (ch->pan > MUS_PAN_MAX || ch->pan == 0) {
                    ch->pan = 0;
                    ch->panSweepDown = 0;
                }
            }
        }
    } while (ch->panSweepTime < ch->time);
}

/*
 * 801C2088: counts the trill's frames and returns its pitch offset in
 * semitones. The offset switches between trillDepth (for trillOnFrames calls)
 * and 0 (for trillOffFrames calls).
 */
f32 musTrillUpdate(MusChannel *ch) {
    if (--ch->trillCounter == 0) {
        if (ch->trillOffset == 0) {
            ch->trillOffset = ch->trillDepth;
            ch->trillCounter = ch->trillOnFrames;
        } else {
            ch->trillOffset = 0;
            ch->trillCounter = ch->trillOffFrames;
        }
    }
    return (f32)ch->trillOffset;
}

/*
 * 801C20D8: returns the vibrato's pitch offset in semitones. The vibrato
 * starts vibratoDelay ticks into the note.
 */
f32 musVibratoUpdate(MusChannel *ch) {
    s32 ticks;

    ticks = (s32)ch->noteTicks - ch->vibratoDelay;
    if (ticks > 0) {
        ch->vibratoValue = __sinf((f32)ticks * ch->vibratoStep) * ch->vibratoDepth;
    } else {
        ch->vibratoValue = 0.0f;
    }
    return ch->vibratoValue;
}

/*
 * Reads one event of a volume or bend stream at p and returns the next byte.
 * A byte below 0x80 is a value that lasts one tick. A byte with bit 7 set
 * holds the value in its low 7 bits. A length follows it: one byte below
 * 0x80, or two bytes that form a 15-bit number, high byte first. The value
 * then lasts length + 2 ticks.
 */
static u8 *musStreamReadEvent(u8 *p, u8 *value, u16 *ticks) {
    u8 b;

    b = *p++;
    if (b < 0x80) {
        *value = b;
        *ticks = 1;
        return p;
    }
    *value = b & 0x7F;
    b = *p++;
    if (b < 0x80) {
        *ticks = b + 2;
    } else {
        *ticks = ((b & 0x7F) << 8) + *p++ + 2;
    }
    return p;
}

/*
 * 801C2144: plays the volume stream, one tick at a time, up to the channel
 * clock. The stream sets the channel's volume byte.
 */
void musVolumeStreamUpdate(MusChannel *ch) {
    do {
        ch->volumeTime += MUS_TICK;
        if (--ch->volumeCount == 0) {
            ch->volumeStream = musStreamReadEvent(ch->volumeStream, &ch->volume, &ch->volumeCount);
        }
    } while (ch->volumeTime < ch->time);
}

/*
 * 801C21FC: plays the pitch bend stream, one tick at a time, up to the channel
 * clock. Stream values 0 to 127 give bendValue -64 to 63.
 */
void musBendStreamUpdate(MusChannel *ch) {
    u8 value;

    do {
        ch->bendTime += MUS_TICK;
        if (--ch->bendCount == 0) {
            ch->bendStream = musStreamReadEvent(ch->bendStream, &value, &ch->bendCount);
            ch->bendValue = (f32)((f64)(f32)value - MUS_BEND_CENTRE);
            ch->bend = ch->bendValue * ch->bendRange;
        }
    } while (ch->bendTime < ch->time);
}

/*
 * 801C2314: approximates 2^x with the first terms of its power series. The
 * powers of |x| are f32 products and the sum is f64, in this order. A negative
 * x returns the reciprocal of the sum for -x. NaN takes that path too.
 */
f32 musPow2(f32 x) {
    f32 a;
    f32 a2;
    f32 a4;
    f64 sum;

    if (x == 0.0f) {
        return 1.0f;
    }
    if (x > 0.0f) {
        a = x;
    } else {
        a = -x;
    }
    a2 = a * a;
    a4 = a2 * a2;
    sum = (f64)a * MUS_POW2_C1 + 1.0;
    sum = sum + (f64)a2 * MUS_POW2_C2;
    sum = sum + (f64)(a2 * a) * MUS_POW2_C3;
    sum = sum + (f64)a4 * MUS_POW2_C4;
    sum = sum + (f64)(a4 * a) * MUS_POW2_C5;
    sum = sum + (f64)(a4 * a2) * MUS_POW2_C6;
    if (x > 0.0f) {
        return (f32)sum;
    }
    return (f32)(1.0 / sum);
}
PORT_C_END
