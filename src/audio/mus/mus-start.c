/*
 * mus-start.c: starting songs and sound effects, and choosing and resetting
 * their channels.
 *
 * Translated from the ROM's machine code (MusStartSong to MusStartEffect2 at
 * 801C0694-801C0A28, MusBankStartSong to MusBankStartEffect2 at
 * 801C0EE0-801C0FD4, and 801C2768-801C2BAC). docs/audio-libmus-map.md
 * describes the channel reset values (section 6.2) and the allocation rules
 * (section 9).
 *
 * Songs are big-endian files that libmus never modifies. The ROM's
 * MusStartSong relocates a song in place the first time it starts it: it
 * adds the song address to every nonzero offset in the header and in the
 * three track tables. It skips this when the header word 0x04 is 0x400 or
 * more, which means "already relocated". The port leaves the song as it is.
 * It reads each offset with musRead32 and adds the song address itself. A
 * zero offset gives NULL, as the ROM's zero entries stay zero.
 *
 * The public functions take musLock for their whole body. The lock is
 * recursive: the player calls MusStartEffect2 with the lock held (command
 * 0xA7), and MusStartSong calls MusHandleUnPause.
 */
PORT_C_BEGIN
#include <string.h>

#include "mus_internal.h"

/*
 * Returns the stream of one track from a song table, or NULL when the entry
 * is 0. tableField is the header word that holds the table offset
 * (MUS_SONG_DATA_TABLE, MUS_SONG_VOLUME_TABLE or MUS_SONG_BEND_TABLE).
 */
static u8 *musSongTrackStream(u8 *song, s32 tableField, s32 track) {
    u8 *table = song + musRead32(song + tableField);
    u32 offset = musRead32(table + track * 4);

    return offset != 0 ? song + offset : NULL;
}

/*
 * 801C2768: clears a channel and sets the values a new song track or effect
 * starts with (map section 6.2). voiceOn survives: the channel's voice may
 * still be playing, and the player stops it before it starts a new wave.
 */
void musChannelReset(MusChannel *ch) {
    u8 voiceOn = ch->voiceOn;

    memset(ch, 0, sizeof(MusChannel));

    ch->lastVolume = 0xFFFF;
    ch->lastReverb = 0xFF;
    ch->lastPan = 0xFF;
    ch->lastPitch = MUS_FIRST_PITCH;

    /* 120 beats per minute at normal speed. */
    ch->songTempoStep = ch->tempoStep = (u16)(MUS_TEMPO_BASE / musFrameRate);
    ch->length = 1;

    ch->fixedVelocity = 0x7F;
    ch->volume = 0x7F;
    ch->pan = 0x40;
    ch->volumeCount = 1;
    ch->bendCount = 1;
    ch->stopCount = -1;
    ch->handleVolume = 0x80;
    ch->handlePan = 0x80;
    ch->tempoScale = MUS_TEMPO_SCALE_ONE;

    /*
     * The default envelope does not match its own levels. With an
     * envelopeScale of 0 the volume never leaves envelopeInitial (0), so a
     * track that sets no envelope stays silent. This is the ROM's behaviour.
     */
    ch->envelopeSpeed = 1;
    ch->attackSteps = 1;
    ch->envelopePeak = 0x7F;
    ch->decaySteps = 0xFF;
    ch->bendRange = 1.0f / 32.0f;
    ch->attackStep = 1.0f;
    ch->decayStep = 1.0f / 255.0f;
    ch->envelopeSustain = 0x7F;
    ch->releaseSteps = 0xF;
    ch->releaseStep = 1.0f / 15.0f;

    ch->voiceOn = voiceOn;
    ch->bank = musCurrentBank;
}

/*
 * 801C28A0: chooses the channel for track songChannel of a song. Songs take
 * effect channels without a priority test. The order of preference:
 *  1. the first free channel;
 *  2. the effect channel with the lowest priority (the last one on a tie);
 *  3. channel 0, if it plays a track of another song (see below);
 *  4. the channel that already plays this track of this song;
 *  5. channel songChannel modulo the number of channels.
 */
s32 musSongChannelFind(u8 *song, s32 songChannel) {
    s32 numChannels = musNumChannels;
    MusChannel *ch;
    s32 lowestPriority;
    s32 lowest;
    u8 *data;
    s32 i;

    ch = musChannels;
    for (i = 0; i < numChannels; i++, ch++) {
        if (ch->pdata == NULL) {
            return i;
        }
    }

    lowestPriority = 0x7FFFFFFF;
    lowest = -1;
    ch = musChannels;
    for (i = 0; i < numChannels; i++, ch++) {
        if (ch->fxNumber != 0 && ch->priority <= lowestPriority) {
            lowestPriority = ch->priority;
            lowest = i;
        }
    }
    if (lowest != -1) {
        return lowest;
    }

    /*
     * The ROM loops over the channels here to find a song channel of another
     * song, but its loop never advances the channel pointer. It only ever
     * tests channel 0, and returns 0 or goes on.
     */
    if (numChannels > 0 && musChannels[0].fxNumber == 0 && musChannels[0].song != song) {
        return 0;
    }

    data = musSongTrackStream(song, MUS_SONG_DATA_TABLE, songChannel);
    ch = musChannels;
    for (i = 0; i < numChannels; i++, ch++) {
        if (ch->song == song && ch->dataStart == data) {
            return i;
        }
    }

    return songChannel % numChannels;
}

/*
 * 801C2B24: starts sound effect number on a channel and gives it a new
 * handle. The effect starts unpaused, at the next player call.
 */
s32 musEffectStart(MusChannel *ch, s32 number, s32 volume, s32 pan, s32 priority) {
    musChannelReset(ch);
    ch->fxNumber = (u16)number;
    ch->handleVolume = (s16)volume;
    ch->handlePan = (s16)pan;
    ch->handle = musNextHandle++;
    ch->priority = priority;
    ch->dataStart = ch->pdata = musFxStreams[number];
    return ch->handle;
}

/*
 * The channel search that MusStartEffect and MusStartEffect2 share (the ROM
 * has a copy in each). The first free channel gets the effect. Without one,
 * the effect channel with the lowest priority is replaced, but only when
 * that priority is below the new effect's. Among equal lowest priorities
 * the first channel is chosen. Song channels are never taken.
 * Returns the new handle, or 0 when the effect does not start.
 */
static s32 musEffectAllocate(s32 number, s32 volume, s32 pan, s32 priority) {
    s32 numChannels = musNumChannels;
    MusChannel *ch;
    MusChannel *lowest = NULL;
    s32 lowestPriority;
    s32 i;

    /*
     * The search starts one above the new priority. A channel of equal
     * priority can be found, but the test after the loop rejects it.
     */
    lowestPriority = priority + 1;
    ch = musChannels;
    for (i = 0; i < numChannels; i++, ch++) {
        if (ch->pdata == NULL) {
            return musEffectStart(ch, number, volume, pan, priority);
        }
        if (ch->fxNumber != 0 && ch->priority < lowestPriority) {
            lowestPriority = ch->priority;
            lowest = ch;
        }
    }

    /*
     * lowest is set whenever lowestPriority < priority, except for a priority
     * of 0x7FFFFFFF: then priority + 1 wraps and the ROM starts the effect on
     * an unset pointer. The port does not start it.
     */
    if (lowestPriority < priority && lowest != NULL) {
        return musEffectStart(lowest, number, volume, pan, priority);
    }
    return 0;
}

/*
 * 801C0694: starts every track of a song on its own channel. All tracks share
 * one handle. They start paused, and the unpause goes through the message
 * queue: the player starts them all in the same call.
 * Returns the handle.
 */
s32 MusStartSong(u8 *song) {
    s32 numTracks;
    s32 handle;
    MusChannel *ch;
    u8 *data;
    s32 i;

    musLock();

    numTracks = (s32)musRead32(song + MUS_SONG_NUM_CHANNELS);
    handle = musNextHandle++;

    for (i = 0; i < numTracks; i++) {
        data = musSongTrackStream(song, MUS_SONG_DATA_TABLE, i);
        if (data == NULL) {
            continue;
        }
        ch = &musChannels[musSongChannelFind(song, i)];
        musChannelReset(ch);
        ch->song = song;
        ch->flags |= MUS_CHANNEL_PAUSED;
        ch->volumeStreamStart = ch->volumeStream = musSongTrackStream(song, MUS_SONG_VOLUME_TABLE, i);
        ch->bendStreamStart = ch->bendStream = musSongTrackStream(song, MUS_SONG_BEND_TABLE, i);
        ch->handle = handle;
        ch->dataStart = ch->pdata = data;
    }

    MusHandleUnPause(handle);

    musUnlock();
    return handle;
}

/*
 * 801C0808: starts sound effect number at normal volume (0x80) and centre
 * pan (0x80), with its priority from the table. The same as
 * MusStartEffect2(number, 0x80, 0x80, 0, -1).
 * Returns the handle, or 0 when no channel is available.
 */
s32 MusStartEffect(s32 number) {
    s32 handle;

    musLock();
    handle = musEffectAllocate(number, 0x80, 0x80, musFxPriorities[number]);
    musUnlock();
    return handle;
}

/*
 * 801C08E4: starts sound effect number. A priority of -1 takes the priority
 * from the table. With restartflag set, the first channel whose fxNumber is
 * number restarts the effect, whether it still plays or not, and whatever
 * its priority. Otherwise the effect gets a free channel or replaces one of
 * lower priority.
 * Returns the handle, or 0 when no channel is available.
 */
s32 MusStartEffect2(s32 number, s32 volume, s32 pan, s32 restartflag, s32 priority) {
    MusChannel *restart = NULL;
    MusChannel *ch;
    s32 handle;
    s32 i;

    musLock();

    if (priority == -1) {
        priority = musFxPriorities[number];
    }

    if (restartflag != 0) {
        ch = musChannels;
        for (i = 0; i < musNumChannels; i++, ch++) {
            if (ch->fxNumber == number) {
                restart = ch;
                break;
            }
        }
    }

    if (restart != NULL) {
        handle = musEffectStart(restart, number, volume, pan, priority);
    } else {
        handle = musEffectAllocate(number, volume, pan, priority);
    }

    musUnlock();
    return handle;
}

/*
 * Makes pbank the bank of the channels that start next. A NULL pbank, or one
 * that musBankInit has not converted, is ignored.
 */
static void musSelectBank(u8 *pbank) {
    MusBank *bank = (MusBank *)pbank;

    if (bank != NULL && (musRead32(bank->flags) & MUS_BANK_INITIALISED)) {
        musCurrentBank = bank;
    }
}

/*
 * 801C0EE0: starts a song with the waves of pbank instead of the default
 * bank. A NULL or uninitialised pbank leaves the current bank. The default
 * bank is current again afterwards, in every case.
 */
s32 MusBankStartSong(u8 *pbank, u8 *song) {
    s32 handle;

    musLock();
    musSelectBank(pbank);
    handle = MusStartSong(song);
    musCurrentBank = musDefaultBank;
    musUnlock();
    return handle;
}

/* 801C0F28: MusStartEffect with the waves of pbank, as MusBankStartSong. */
s32 MusBankStartEffect(u8 *pbank, s32 number) {
    s32 handle;

    musLock();
    musSelectBank(pbank);
    handle = MusStartEffect(number);
    musCurrentBank = musDefaultBank;
    musUnlock();
    return handle;
}

/* 801C0F70: MusStartEffect2 with the waves of pbank, as MusBankStartSong. */
s32 MusBankStartEffect2(u8 *pbank, s32 number, s32 volume, s32 pan, s32 restartflag, s32 priority) {
    s32 handle;

    musLock();
    musSelectBank(pbank);
    handle = MusStartEffect2(number, volume, pan, restartflag, priority);
    musCurrentBank = musDefaultBank;
    musUnlock();
    return handle;
}
PORT_C_END
