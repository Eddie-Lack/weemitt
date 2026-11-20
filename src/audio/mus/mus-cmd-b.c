/*
 * mus-cmd-b.c: sequencer commands 0x91 to 0xAA and the random number
 * generator of libmus.
 *
 * Translated from the ROM's machine code (801BFD6C-801C02FC and 801C2668).
 * docs/audio-libmus-map.md describes each command (section 5.2), loops
 * (8.9), random numbers (8.10) and linked effects (9.4).
 *
 * Every command takes the channel and the stream position just after the
 * command byte, and returns the position of the next byte. Command streams
 * are big-endian bytes from the ROM. Multi-byte operands are read with
 * musRead16 and musRead32.
 */
PORT_C_BEGIN
#include "mus_internal.h"

/* 0x91: new notes keep the running envelope. */
u8 *musCmdKeepEnvelopeOn(MusChannel *ch, u8 *p) {
    ch->keepEnvelope = 1;
    return p;
}

/* 0x92 */
u8 *musCmdKeepEnvelopeOff(MusChannel *ch, u8 *p) {
    ch->keepEnvelope = 0;
    return p;
}

/* 0x93: new notes do not restart the voice. */
u8 *musCmdTieOn(MusChannel *ch, u8 *p) {
    ch->tie = 1;
    return p;
}

/* 0x94 */
u8 *musCmdTieOff(MusChannel *ch, u8 *p) {
    ch->tie = 0;
    return p;
}

/* 0x95, operand u8 count: saves the stream positions and the levels at the start of a loop. */
u8 *musCmdLoopStart(MusChannel *ch, u8 *p) {
    s32 i = ch->loopDepth;

    ch->loopCount[i] = p[0];
    ch->loopStart[i] = p + 1;
    ch->loopVolumeStream[i] = ch->volumeStream;
    ch->loopBendStream[i] = ch->bendStream;
    ch->loopVolume[i] = ch->volume;
    /* The ROM converts the bend value to an unsigned int. A value of -1 or less becomes 0xFF. */
    ch->loopBendValue[i] = (u8)musFloatToU32(ch->bendValue);
    ch->loopVolumeCount[i] = ch->volumeCount;
    ch->loopBendCount[i] = ch->bendCount;
    ch->loopDepth++;
    return p + 1;
}

/*
 * 0x96: goes back to the start of the innermost loop, or leaves it.
 * A count of n plays the loop body n times. A count of 0 wraps to 0xFF at
 * the first loop end and then repeats forever.
 * With loopDepth 0 the ROM reads, and may write, byte 0x11B of the channel,
 * just before loopCount. loopCount[-1] is the same byte here. It belongs to
 * loopBendCount[3], which is little-endian on the host, so the effect on that
 * count differs. Well-formed data never ends a loop it did not start.
 */
u8 *musCmdLoopEnd(MusChannel *ch, u8 *p) {
    s32 i = ch->loopDepth - 1;

    if (ch->loopCount[i] != MUS_LOOP_INFINITE) {
        if (--ch->loopCount[i] == 0) {
            ch->loopDepth = i;
            i = -1;
        }
    }
    if (i < 0) {
        return p;
    }
    ch->volumeStream = ch->loopVolumeStream[i];
    ch->bendStream = ch->loopBendStream[i];
    ch->volume = ch->loopVolume[i];
    ch->bendValue = (f32)ch->loopBendValue[i];
    ch->volumeCount = ch->loopVolumeCount[i];
    ch->bendCount = ch->loopBendCount[i];
    ch->bend = ch->bendValue * ch->bendRange;
    return ch->loopStart[i];
}

/* 0x97, operands s8 depth, u8 frames on, u8 frames off. */
u8 *musCmdTrillOn(MusChannel *ch, u8 *p) {
    ch->trillDepth = (s8)p[0];
    ch->trillOnFrames = p[1];
    ch->trillOffFrames = p[2];
    return p + 3;
}

/* 0x98 */
u8 *musCmdTrillOff(MusChannel *ch, u8 *p) {
    ch->trillOnFrames = 0;
    return p;
}

/* 0x99: each note carries a velocity byte. */
u8 *musCmdVelocityOn(MusChannel *ch, u8 *p) {
    ch->velocityOn = 1;
    return p;
}

/* 0x9A */
u8 *musCmdVelocityOff(MusChannel *ch, u8 *p) {
    ch->velocityOn = 0;
    return p;
}

/* 0x9B, operand u8 velocity used by every note. */
u8 *musCmdFixedVelocity(MusChannel *ch, u8 *p) {
    ch->velocityOn = 0;
    ch->fixedVelocity = p[0];
    return p + 1;
}

/* 0x9C, operand u8 pan in 0..255. */
u8 *musCmdSetPan(MusChannel *ch, u8 *p) {
    ch->pan = p[0] / 2;
    return p + 1;
}

/* 0x9D: two operand bytes that libmus ignores. */
u8 *musCmdSkip2(MusChannel *ch, u8 *p) {
    return p + 2;
}

/*
 * 0x9E, operand u8 n: drumMap = song + drumTable[n].
 * The drum table entries are offsets from the song start. Sound effects have
 * no song: the ROM would read the N64's address 0x14 here. Glover's effects
 * never use this command.
 */
u8 *musCmdDrumMapOn(MusChannel *ch, u8 *p) {
    u8 *drumTable = ch->song + musRead32(ch->song + MUS_SONG_DRUM_TABLE);

    ch->drumMap = ch->song + musRead32(drumTable + p[0] * 4);
    return p + 1;
}

/* 0x9F */
u8 *musCmdDrumMapOff(MusChannel *ch, u8 *p) {
    ch->drumMap = NULL;
    return p;
}

/* 0xA0: one operand byte that libmus ignores. */
u8 *musCmdSkip1(MusChannel *ch, u8 *p) {
    return p + 1;
}

/* 0xA1, operands u16 data, u16 volume, u16 bend: offsets from the start of each stream. */
u8 *musCmdGoto(MusChannel *ch, u8 *p) {
    ch->volumeStream = ch->volumeStreamStart + musRead16(p + 2);
    ch->volumeCount = 1;
    ch->bendStream = ch->bendStreamStart + musRead16(p + 4);
    ch->bendCount = 1;
    return ch->dataStart + musRead16(p);
}

/* 0xA2, operand u8 reverb. */
u8 *musCmdSetReverb(MusChannel *ch, u8 *p) {
    ch->reverb = p[0];
    return p + 1;
}

/*
 * Operands u8 range, u8 base of commands 0xA3 to 0xA5. musRandom can return
 * a negative number. Only the low byte of the sum is kept.
 */
static u8 musRandomOperand(const u8 *p) {
    s32 value = musRandom(p[0]);

    return (u8)(value + p[1]);
}

/* 0xA3 */
u8 *musCmdRandomTranspose(MusChannel *ch, u8 *p) {
    ch->transpose = musRandomOperand(p);
    return p + 2;
}

/* 0xA4 */
u8 *musCmdRandomVolume(MusChannel *ch, u8 *p) {
    ch->volume = musRandomOperand(p);
    return p + 2;
}

/* 0xA5 */
u8 *musCmdRandomPan(MusChannel *ch, u8 *p) {
    ch->pan = musRandomOperand(p);
    return p + 2;
}

/* 0xA6, operand u8 volume. */
u8 *musCmdSetVolume(MusChannel *ch, u8 *p) {
    ch->volume = p[0];
    return p + 1;
}

/*
 * 0xA7, operand varlen effect number: starts a linked effect.
 * The channel's priority is one higher during the call, so the new effect
 * cannot take this channel. The new effect's channels then join this
 * channel's handle and bank.
 */
u8 *musCmdStartEffect(MusChannel *ch, u8 *p) {
    u16 number;
    s32 priority;
    s32 handle;
    s32 i;
    MusChannel *other;

    p = musReadVarLen(p, &number);

    priority = ch->priority;
    ch->priority = priority + 1;
    handle = MusStartEffect2(number, ch->handleVolume, ch->handlePan, 0, priority);
    ch->priority--;

    if (handle != 0) {
        other = musChannels;
        for (i = 0; i < musNumChannels; i++, other++) {
            if (other->handle == handle) {
                other->handle = ch->handle;
                other->bank = ch->bank;
            }
        }
    }
    return p;
}

/*
 * 0xA8, operand u8 range in 1/64 semitones.
 * The ROM sets bend to the square of bendValue, not to bendValue times the
 * new range. The next bend stream event sets bend again.
 */
u8 *musCmdBendRange(MusChannel *ch, u8 *p) {
    ch->bend = ch->bendValue * ch->bendValue;
    ch->bendRange = (f32)((f64)(f32)p[0] * (1.0 / 64.0));
    return p + 1;
}

/* 0xA9, operand u8 speed in 1/64 pan units per tick; 0 = off. */
u8 *musCmdPanSweep(MusChannel *ch, u8 *p) {
    ch->panSweepSpeed = p[0];
    return p + 1;
}

/* 0xAA, operand u8 reverb preset. Songs may change the preset only after MusSetSongFxChange(1). */
u8 *musCmdChangeEffect(MusChannel *ch, u8 *p) {
    if (musSongFxChange == 1) {
        ChangeCustomEffect(p[0]);
    }
    return p + 1;
}

/*
 * Returns a pseudo-random number between -range / 2 and range / 2,
 * truncated toward zero (801C2668).
 * The seed is a 32-bit shift register advanced by 8 steps per call. Each step
 * shifts it left and sets bit 0 when bit 27 was set. The test compares the
 * two tap bits with 0x48000000 and 0x08000000, so bit 30 has no effect.
 * The scaled value is the seed as a signed number divided by 2^32.
 */
s32 musRandom(s32 range) {
    u32 seed = (u32)musRandomSeed;
    u32 taps;
    s32 k;

    for (k = 0; k < 8; k++) {
        taps = seed & MUS_RANDOM_TAPS;
        seed <<= 1;
        if (taps == MUS_RANDOM_TAPS || taps == 0x08000000) {
            seed |= 1;
        }
    }
    musRandomSeed = (s32)seed;
    return (s32)((f32)range * (((f32)(s32)seed / 65536.0f) / 65536.0f));
}
PORT_C_END
