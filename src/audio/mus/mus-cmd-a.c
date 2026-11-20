/*
 * mus-cmd-a.c: libmus sequencer commands 0x80 to 0x90.
 *
 * Translated from the ROM's machine code (801BF800-801BFD6C). The player calls
 * each command through musCommands with p just after the command byte. Each
 * command reads its operands and returns the next stream byte, or NULL to end
 * the channel. docs/audio-libmus-map.md describes them (sections 5.2 and 8).
 *
 * Streams and songs stay big-endian. Operands are read a byte at a time or
 * with musRead16, and song header words with musRead32.
 */
PORT_C_BEGIN
#include "mus_internal.h"

/* Beats per minute that MUS_TEMPO_BASE stands for. */
#define MUS_TEMPO_BASE_BPM 120

/* The ROM's value of 2 pi (D_8010C170 and D_8010C178), a little below the exact one. */
#define MUS_VIBRATO_CYCLE 6.2831852

/* 0x80 (801BF800): ends the channel. The player also calls it with p = NULL. */
u8 *musCmdEnd(MusChannel *ch, u8 *p) {
    ch->volumeStream = NULL;
    ch->bendStream = NULL;
    ch->song = NULL;
    ch->fxNumber = 0;
    ch->handle = 0;
    ch->pendingWave = NULL;
    return NULL;
}

/* 0x81 (801BF824): varlen wave index in the channel's bank. */
u8 *musCmdSetWave(MusChannel *ch, u8 *p) {
    return musReadVarLen(p, &ch->wave);
}

/* 0x82 (801BF85C): u8 portamento length in ticks. */
u8 *musCmdPortamentoOn(MusChannel *ch, u8 *p) {
    ch->portamentoTicks = *p++;
    if (ch->portamentoTicks != 0) {
        /* The next note slides from the pitch of the current one. */
        ch->portamentoPitch = ch->notePitch;
    }
    return p;
}

/* 0x83 (801BF880) */
u8 *musCmdPortamentoOff(MusChannel *ch, u8 *p) {
    ch->portamentoTicks = 0;
    return p;
}

/*
 * 0x84 (801BF88C): seven bytes: speed, initial, attack, peak, decay, sustain
 * and release. It reads exactly these seven bytes. musCmdEnvelopeIndex and the
 * drum maps also call it with a pointer into a song's envelope table.
 */
u8 *musCmdSetEnvelope(MusChannel *ch, u8 *p) {
    u8 speed;
    u8 attack;
    u8 peak;
    u8 decay;
    u8 release;

    speed = p[0];
    if (speed == 0) {
        speed = 1;
    }
    ch->envelopeSpeed = speed;
    ch->envelopeScale = 0x400 / speed;
    ch->envelopeInitial = p[1];

    /*
     * Each step is the level change divided by the number of steps. The ROM
     * converts both to f32, then to f64, divides 1.0 by the step count and
     * multiplies in f64, and rounds the product to f32.
     */
    attack = p[2];
    ch->attackSteps = attack;
    peak = p[3];
    ch->envelopePeak = peak;
    ch->attackStep = (f32)((1.0 / (f64)(f32)attack) * (f64)(f32)((s32)peak - (s32)ch->envelopeInitial));

    decay = p[4];
    ch->decaySteps = decay;
    ch->envelopeSustain = p[5];
    ch->decayStep = (f32)((1.0 / (f64)(f32)decay) * (f64)(f32)((s32)ch->envelopeSustain - (s32)peak));

    /* The release step is a fraction of the volume at the start of the release. */
    release = p[6];
    ch->releaseSteps = release;
    ch->releaseStep = (f32)(1.0 / (f64)(f32)release);
    return p + 7;
}

/*
 * 0x85 (801BF9E4): u8 tempo in beats per minute. The clock runs in 1/256 ticks
 * and a beat is 48 ticks, so 120 bpm is MUS_TEMPO_BASE clock units a second.
 */
u8 *musCmdSetTempo(MusChannel *ch, u8 *p) {
    s32 step;
    s32 scaledStep;
    s32 i;
    MusChannel *other;

    step = (((s32)*p++ * MUS_TEMPO_BASE) / MUS_TEMPO_BASE_BPM) / musFrameRate;
    /* Every channel of the song gets the tempoScale of the channel that ran the command. */
    scaledStep = (ch->tempoScale * step) >> 7;

    if (ch->fxNumber != 0) {
        /* A sound effect takes the step unscaled and keeps its songTempoStep. */
        ch->tempoStep = (u16)step;
        return p;
    }

    other = musChannels;
    for (i = 0; i < musNumChannels; i++, other++) {
        if (other->song == ch->song) {
            other->songTempoStep = (u16)step;
            other->tempoStep = (u16)scaledStep;
        }
    }
    return p;
}

/*
 * 0x86 (801BFAC4): 16-bit big-endian release time in ticks after the note
 * start. The ROM reads it as an s16; the halfword it stores is the same.
 */
u8 *musCmdReleaseAfterStart(MusChannel *ch, u8 *p) {
    ch->releaseAfterStart = musRead16(p);
    ch->releaseBeforeEnd = 0;
    return p + 2;
}

/* 0x87 (801BFAB0): u8 release time in ticks before the note end. */
u8 *musCmdReleaseBeforeEnd(MusChannel *ch, u8 *p) {
    ch->releaseAfterStart = 0;
    ch->releaseBeforeEnd = *p;
    return p + 1;
}

/*
 * 0x88 (801BFAF4): u8 delay and u8 period in ticks, u8 depth in 1/50
 * semitones. One vibrato cycle lasts vibratoPeriod ticks.
 */
u8 *musCmdVibratoUp(MusChannel *ch, u8 *p) {
    ch->vibratoDelay = p[0];
    ch->vibratoPeriod = p[1];
    ch->vibratoDepth = (f32)((f64)(f32)p[2] / 50.0);
    /* A period of 0 gives an infinite step; the player then skips the vibrato. */
    ch->vibratoStep = (f32)(MUS_VIBRATO_CYCLE / (f64)(f32)ch->vibratoPeriod);
    return p + 3;
}

/* 0x89 (801BFB88): as 0x88, with the depth negated as an f32 before the division. */
u8 *musCmdVibratoDown(MusChannel *ch, u8 *p) {
    ch->vibratoDelay = p[0];
    ch->vibratoPeriod = p[1];
    ch->vibratoDepth = (f32)((f64)(-(f32)p[2]) / 50.0);
    ch->vibratoStep = (f32)(MUS_VIBRATO_CYCLE / (f64)(f32)ch->vibratoPeriod);
    return p + 3;
}

/* 0x8A (801BFC20) */
u8 *musCmdVibratoOff(MusChannel *ch, u8 *p) {
    ch->vibratoPeriod = 0;
    ch->vibratoValue = 0.0f;
    return p;
}

/* 0x8B (801BFC34): varlen length in ticks for every following note; 0 means each note has its own. */
u8 *musCmdFixedLength(MusChannel *ch, u8 *p) {
    return musReadVarLen(p, &ch->fixedLength);
}

/* 0x8C (801BFC78): the next note has a length despite fixedLength. */
u8 *musCmdReadNextLength(MusChannel *ch, u8 *p) {
    ch->readNextLength = 1;
    return p;
}

/* 0x8D (801BFC88): u8 transpose in semitones; the player treats it as signed. */
u8 *musCmdTranspose(MusChannel *ch, u8 *p) {
    ch->transpose = *p;
    return p + 1;
}

/* 0x8E (801BFC98): only musChannelReset clears this flag. */
u8 *musCmdIgnoreTranspose(MusChannel *ch, u8 *p) {
    ch->ignoreTranspose = 1;
    return p;
}

/* 0x8F (801BFCA8): s8 fine tune in cents. */
u8 *musCmdFineTune(MusChannel *ch, u8 *p) {
    /* pitchOffset holds the fine tune plus the handle's offset. Swap the old fine tune for the new one, in f32. */
    ch->pitchOffset -= ch->fineTune;
    ch->fineTune = (f32)((f64)(f32)(s8)p[0] / 100.0);
    ch->pitchOffset += ch->fineTune;
    return p + 1;
}

/*
 * 0x90 (801BFD0C): varlen index into the song's envelope table. Only song
 * tracks use it: a sound effect has no song.
 */
u8 *musCmdEnvelopeIndex(MusChannel *ch, u8 *p) {
    u16 index;
    u8 *envelopes;

    p = musReadVarLen(p, &index);
    envelopes = ch->song + musRead32(ch->song + MUS_SONG_ENV_TABLE);
    musCmdSetEnvelope(ch, envelopes + index * MUS_ENVELOPE_SIZE);
    return p;
}
PORT_C_END
