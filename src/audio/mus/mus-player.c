/*
 * mus-player.c: the libmus player and its command table.
 *
 * Translated from the ROM's machine code (801C1338-801C1A90) and data
 * (801F40C0). docs/audio-libmus-map.md describes the player (sections 4,
 * 8.1 and 8.2).
 *
 * The native synthesizer calls musPlayerHandler through musPlayer, the way
 * libaudio calls an ALPlayer. musAudioRender holds musLock while the
 * synthesizer runs. The handler takes no lock itself.
 *
 * Channel i always drives voice musVoices[i] of the synthesizer
 * musGlobals.drvr. Command streams are big-endian bytes from the ROM and are
 * read a byte at a time. Song header words are read with musRead32. The
 * pbank tables (waves, pitch) are in host order after musBankInit.
 */
PORT_C_BEGIN
#include "mus_internal.h"

/* The command handlers, indexed by command byte - MUS_FIRST_COMMAND (ROM jumptable). */
MusCommand musCommands[MUS_NUM_COMMANDS] = {
    musCmdEnd,               /* 0x80 */
    musCmdSetWave,           /* 0x81 */
    musCmdPortamentoOn,      /* 0x82 */
    musCmdPortamentoOff,     /* 0x83 */
    musCmdSetEnvelope,       /* 0x84 */
    musCmdSetTempo,          /* 0x85 */
    musCmdReleaseAfterStart, /* 0x86 */
    musCmdReleaseBeforeEnd,  /* 0x87 */
    musCmdVibratoUp,         /* 0x88 */
    musCmdVibratoDown,       /* 0x89 */
    musCmdVibratoOff,        /* 0x8A */
    musCmdFixedLength,       /* 0x8B */
    musCmdReadNextLength,    /* 0x8C */
    musCmdTranspose,         /* 0x8D */
    musCmdIgnoreTranspose,   /* 0x8E */
    musCmdFineTune,          /* 0x8F */
    musCmdEnvelopeIndex,     /* 0x90 */
    musCmdKeepEnvelopeOn,    /* 0x91 */
    musCmdKeepEnvelopeOff,   /* 0x92 */
    musCmdTieOn,             /* 0x93 */
    musCmdTieOff,            /* 0x94 */
    musCmdLoopStart,         /* 0x95 */
    musCmdLoopEnd,           /* 0x96 */
    musCmdTrillOn,           /* 0x97 */
    musCmdTrillOff,          /* 0x98 */
    musCmdVelocityOn,        /* 0x99 */
    musCmdVelocityOff,       /* 0x9A */
    musCmdFixedVelocity,     /* 0x9B */
    musCmdSetPan,            /* 0x9C */
    musCmdSkip2,             /* 0x9D */
    musCmdDrumMapOn,         /* 0x9E */
    musCmdDrumMapOff,        /* 0x9F */
    musCmdSkip1,             /* 0xA0 */
    musCmdGoto,              /* 0xA1 */
    musCmdSetReverb,         /* 0xA2 */
    musCmdRandomTranspose,   /* 0xA3 */
    musCmdRandomVolume,      /* 0xA4 */
    musCmdRandomPan,         /* 0xA5 */
    musCmdSetVolume,         /* 0xA6 */
    musCmdStartEffect,       /* 0xA7 */
    musCmdBendRange,         /* 0xA8 */
    musCmdPanSweep,          /* 0xA9 */
    musCmdChangeEffect,      /* 0xAA */
};

/*
 * 801C1338: the ALPlayer handler. The synthesizer calls it once per player
 * period. It returns the time to the next call in microseconds.
 */
ALMicroTime musPlayerHandler(void *player) {
    MusChannel *ch;
    s32 i;
    f32 offset;

    (void)player;
    musFifoProcess();

    ch = musChannels;
    for (i = 0; i < musNumChannels; i++, ch++) {
        if (ch->pdata == NULL || (ch->flags & MUS_CHANNEL_PAUSED)) {
            continue;
        }
        if (ch->pendingWave != NULL) {
            musVoiceStart(ch, i);
        }

        ch->time += ch->tempoStep;

        /* While a note of infinite length plays, the channel reads no events. */
        if (ch->length != MUS_LENGTH_INFINITE) {
            while (ch->noteEnd < ch->time && ch->pdata != NULL) {
                musChannelNextEvent(ch, i);
            }
            if (ch->pdata == NULL) {
                /* The channel ended. noteTicks is not updated either. */
                continue;
            }
        }

        if (ch->volumeStream != NULL && ch->volumeTime < ch->time) {
            musVolumeStreamUpdate(ch);
        }
        if (ch->bendStream != NULL && ch->bendTime < ch->time) {
            musBendStreamUpdate(ch);
        }

        /* A fade out lowers stopCount once per period. At -1 the channel ends. */
        if (ch->stopCount != -1) {
            ch->stopCount--;
            if (ch->stopCount == -1) {
                ch->pdata = musCmdEnd(ch, NULL);
                if (ch->voiceOn) {
                    ch->voiceOn = 0;
                    alSynStopVoice(&musGlobals.drvr, &musVoices[i]);
                }
            }
        }

        if (ch->voiceOn) {
            if (ch->envelopePhase != MUS_ENV_OFF) {
                musEnvelopeUpdate(ch);
            }
            if (ch->panSweepSpeed != 0 && ch->panSweepTime < ch->time) {
                musPanSweepUpdate(ch);
            }

            /* An f32 sum in this order: pitch offset, vibrato, trill. */
            offset = ch->pitchOffset;
            if (ch->vibratoPeriod != 0) {
                offset += musVibratoUpdate(ch);
            }
            if (ch->trillOnFrames != 0) {
                offset += musTrillUpdate(ch);
            }

            /*
             * A wave that waits for the next period means the voice is fading
             * out. Its pitch, volume and pan stay as they are until then.
             */
            if (ch->pendingWave == NULL) {
                musVoiceUpdatePitch(ch, i, offset);
                musVoiceUpdateVolumePan(ch, i);
            }
        }

        ch->noteTicks = (u16)((ch->time - ch->noteStart) >> 8);
    }
    return musFrameMicros;
}

/*
 * 801C15B8: runs the commands up to the next note, then starts that note.
 * The player calls it while the channel clock is past noteEnd.
 */
void musChannelNextEvent(MusChannel *ch, s32 index) {
    MusBank *bank;
    u8 *p;
    u8 *drum;
    u8 *envelopes;
    u32 command;
    s32 note;
    s32 transpose;
    s32 mix;
    u16 wave;

    /*
     * ch->pdata keeps its old value while the commands run. A command
     * returns NULL when the channel ends.
     */
    p = ch->pdata;
    while (p != NULL && *p >= MUS_FIRST_COMMAND) {
        command = *p - MUS_FIRST_COMMAND;
        if (command >= MUS_NUM_COMMANDS) {
            /* The ROM reads past its table and crashes. Glover's data has no such byte. */
            port_log("libmus: unknown command %02X; the channel ends", *p);
            p = musCmdEnd(ch, p + 1);
        } else {
            p = musCommands[command](ch, p + 1);
        }
    }
    ch->pdata = p;
    if (p == NULL) {
        if (ch->voiceOn) {
            ch->voiceOn = 0;
            alSynSetVol(&musGlobals.drvr, &musVoices[index], 0, musFrameMicros);
            alSynStopVoice(&musGlobals.drvr, &musVoices[index]);
        }
        return;
    }

    /* A slide starts from the pitch that the last slide reached. */
    ch->portamentoFrom = ch->portamentoPitch;

    note = *p++;
    if (ch->velocityOn) {
        ch->velocity = *p++;
    } else {
        ch->velocity = ch->fixedVelocity;
    }

    if (ch->fixedLength == 0) {
        p = musReadVarLen(p, &ch->length);
    } else if (ch->readNextLength == 0) {
        ch->length = ch->fixedLength;
    } else {
        ch->readNextLength = 0;
        p = musReadVarLen(p, &ch->length);
    }
    ch->pdata = p;

    ch->noteStart = ch->noteEnd;
    ch->noteEnd += ch->length << 8;
    ch->noteTicks = 0;
    ch->trillOffset = 0;
    ch->trillCounter = ch->trillOffFrames;

    if (note == MUS_NOTE_REST) {
        /* A rest only starts the release of the note that sounds. */
        if (ch->envelopePhase < MUS_ENV_RELEASE) {
            ch->envelopePhase = MUS_ENV_RELEASE;
            ch->envelopeCounter = 1;
            ch->releaseTime = ch->time;
            ch->releaseVolume = ch->envelopeVolume;
        }
        return;
    }

    bank = ch->bank;
    if (ch->drumMap != NULL) {
        /* A drum map gives each note number its own wave, envelope, pan and note. */
        drum = ch->drumMap + note * MUS_DRUM_ENTRY_SIZE;
        ch->wave = drum[0];
        ch->pan = drum[2] / 2;
        envelopes = ch->song + musRead32(ch->song + MUS_SONG_ENV_TABLE);
        musCmdSetEnvelope(ch, envelopes + drum[1] * MUS_ENVELOPE_SIZE);
        note = drum[3];
    }
    if (ch->keepEnvelope == 0) {
        musEnvelopeStart(ch);
    }
    if (ch->panSweepSpeed != 0) {
        musPanSweepStart(ch);
    }

    wave = ch->wave;
    if (ch->tie == 0) {
        ch->pendingWave = bank->waves[wave];
        if (ch->voiceOn && ch->lastVolume != 0) {
            /* The voice fades to 0 over one period. The player starts the new wave at its next call. */
            ch->lastVolume = 0;
            alSynSetVol(&musGlobals.drvr, &musVoices[index], 0, musFrameMicros);
        } else {
            musVoiceStart(ch, index);
        }
    }

    /*
     * transpose is a signed byte, and ignoreTranspose (0 or 1) turns it off.
     * The ROM tests bit 7 with a float compare against 0. The integer test
     * gives the same result.
     */
    transpose = (u8)(ch->transpose * (1 - ch->ignoreTranspose));
    if (transpose & 0x80) {
        transpose -= 0x100;
    }
    ch->notePitch = bank->pitch[wave] + (f32)note;
    ch->notePitch = ch->notePitch + (f32)transpose;

    if (ch->lastReverb != ch->reverb) {
        ch->lastReverb = ch->reverb;
        /*
         * The song reverb scales the part of the mix above the handle's
         * reverb: handleReverb + (128 - handleReverb) * reverb / 128.
         */
        mix = ch->handleReverb + (((0x80 - ch->handleReverb) * ch->reverb) >> 7);
        alSynSetFXMix(&musGlobals.drvr, &musVoices[index], (u8)mix);
    }
}

/*
 * 801C19FC: starts pendingWave on the channel's voice. It sets no pitch,
 * volume or pan. The update functions send a value only when it differs
 * from the last one sent. The synthesizer keeps a voice's pitch, pan and
 * reverb mix across a stop and a start. It does not keep the volume: a stop
 * sets it to 1 (docs/audio-synth-map.md, section 9.4).
 */
void musVoiceStart(MusChannel *ch, s32 index) {
    if (ch->voiceOn) {
        alSynStopVoice(&musGlobals.drvr, &musVoices[index]);
    }
    ch->voiceOn = 1;
    alSynStartVoice(&musGlobals.drvr, &musVoices[index], ch->pendingWave);
    ch->pendingWave = NULL;
}
PORT_C_END
