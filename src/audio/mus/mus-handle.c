/*
 * mus-handle.c: the libmus calls that act on handles or on all channels, and
 * the message ring between the game and the player.
 *
 * Translated from the ROM's machine code: MusSetMasterVolume (801C066C),
 * MusStop to MusHandleSetReverb (801C0A2C-801C0EBC), MusHandleGetPtrAddr to
 * MusSetSongFxChange (801C0FD8-801C10FC), the ring (801C1100-801C1334) and
 * musHandleSetFlags (801C2BB0). docs/audio-libmus-map.md describes them
 * (sections 5.7, 9.5, 10 and 11).
 *
 * A handle names every channel of one song or sound effect. Handle 0 means
 * "none": the MusHandle* functions that act on channels return 0 for it at
 * once. The others return the number of channels they changed.
 *
 * Pause, unpause and reverb preset changes do not act at once. They go into a
 * ring of MusMessage entries, which the player empties at the start of each
 * call (musFifoProcess). So a song's tracks all start in the same call.
 *
 * The public functions take musLock for their whole body. The lock is
 * recursive: MusStartSong calls MusHandleUnPause and MusSetSongFxChange calls
 * MusSetFxType with the lock held. The ring functions and musHandleSetFlags
 * run inside a public function or inside the player, so they take no lock.
 */
PORT_C_BEGIN
#include <string.h>

#include "mus_internal.h"

/*
 * MusStop and MusAsk select channels by kind. MUSFLAG_EFFECTS matches the
 * channels that play a sound effect (fxNumber != 0). MUSFLAG_SONGS matches
 * song tracks (fxNumber == 0).
 */
static s32 musChannelMatchesFlags(MusChannel *ch, s32 flags) {
    if (ch->fxNumber != 0) {
        return flags & MUSFLAG_EFFECTS;
    }
    return flags & MUSFLAG_SONGS;
}

/*
 * Starts the fade out of a channel. The player lowers stopCount once per call
 * and scales the volume by stopCount / stopLength. The channel ends when
 * stopCount reaches -1. A channel that is still paused (its song has not
 * started) is unpaused and ends at the next call.
 */
static void musChannelStop(MusChannel *ch, s32 speed) {
    if (ch->flags & MUS_CHANNEL_PAUSED) {
        ch->stopLength = 1;
        ch->stopCount = 0;
        ch->flags &= ~MUS_CHANNEL_PAUSED;
    } else {
        ch->stopLength = (speed != 0) ? speed : 1;
        ch->stopCount = speed;
    }
}

/* Posts one message to the ring. Returns 1, or 0 when the ring is full. */
static s32 musPostMessage(u8 type, s32 value) {
    MusMessage msg;

    /* The ring is in musHeap, inside port_ram, which the regression test
       hashes. The ROM copies three padding bytes of stack garbage into it.
       Here they are zero. */
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.value = value;
    return musFifoPost(&msg);
}

/* ------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * 801C066C: sets the master volume of effects (MUSFLAG_EFFECTS), of songs
 * (MUSFLAG_SONGS) or of both. 0x7FFF is full volume.
 */
void MusSetMasterVolume(s32 flags, s32 volume) {
    musLock();
    if (flags & MUSFLAG_EFFECTS) {
        musFxMasterVolume = (u16)volume;
    }
    if (flags & MUSFLAG_SONGS) {
        musSongMasterVolume = (u16)volume;
    }
    musUnlock();
}

/*
 * 801C0A2C: fades out every playing channel of the kinds in flags over speed
 * player calls. Channels that are already stopping keep their fade.
 */
void MusStop(s32 flags, s32 speed) {
    MusChannel *ch;
    s32 i;

    musLock();
    for (i = 0; i < musNumChannels; i++) {
        ch = &musChannels[i];
        if (musChannelMatchesFlags(ch, flags) && ch->pdata != NULL && ch->stopCount == -1) {
            musChannelStop(ch, speed);
        }
    }
    musUnlock();
}

/* 801C0B00: returns the number of playing channels of the kinds in flags. */
s32 MusAsk(s32 flags) {
    MusChannel *ch;
    s32 count;
    s32 i;

    /* The game calls this in a loop until the player has stopped the channels. */
    musAudioPoll();

    musLock();
    count = 0;
    for (i = 0; i < musNumChannels; i++) {
        ch = &musChannels[i];
        if (ch->pdata != NULL && musChannelMatchesFlags(ch, flags)) {
            count++;
        }
    }
    musUnlock();
    return count;
}

/*
 * 801C0B70: fades out the channels of a handle over speed player calls.
 * Unlike MusStop it does not test pdata. Channels that are already stopping
 * keep their fade and are not counted.
 */
s32 MusHandleStop(s32 handle, s32 speed) {
    MusChannel *ch;
    s32 count;
    s32 i;

    if (handle == 0) {
        return 0;
    }

    musLock();
    count = 0;
    for (i = 0; i < musNumChannels; i++) {
        ch = &musChannels[i];
        if (ch->handle == handle && ch->stopCount == -1) {
            count++;
            musChannelStop(ch, speed);
        }
    }
    musUnlock();
    return count;
}

/*
 * 801C0C1C: returns the number of channels of a handle. A channel loses its
 * handle when it ends, so this counts the channels still playing.
 */
s32 MusHandleAsk(s32 handle) {
    s32 count;
    s32 i;

    if (handle == 0) {
        return 0;
    }

    /* The game calls this in a loop until a song has started or stopped. */
    musAudioPoll();

    musLock();
    count = 0;
    for (i = 0; i < musNumChannels; i++) {
        if (musChannels[i].handle == handle) {
            count++;
        }
    }
    musUnlock();
    return count;
}

/* 801C0C70: volume scale of a handle; 128 is unity. */
s32 MusHandleSetVolume(s32 handle, s32 volume) {
    MusChannel *ch;
    s32 count;
    s32 i;

    if (handle == 0) {
        return 0;
    }

    musLock();
    count = 0;
    for (i = 0; i < musNumChannels; i++) {
        ch = &musChannels[i];
        if (ch->handle == handle) {
            ch->handleVolume = (s16)volume;
            count++;
        }
    }
    musUnlock();
    return count;
}

/* 801C0CD4: pan scale of a handle; 128 is unity. */
s32 MusHandleSetPan(s32 handle, s32 pan) {
    MusChannel *ch;
    s32 count;
    s32 i;

    if (handle == 0) {
        return 0;
    }

    musLock();
    count = 0;
    for (i = 0; i < musNumChannels; i++) {
        ch = &musChannels[i];
        if (ch->handle == handle) {
            ch->handlePan = (s16)pan;
            /* The player sends the pan again unless the channel's pan is 0xFF. */
            ch->lastPan = 0xFF;
            count++;
        }
    }
    musUnlock();
    return count;
}

/*
 * 801C0D40: pitch offset of a handle in semitones. It replaces the previous
 * offset: pitchOffset is the channel's fine tune plus this value.
 */
s32 MusHandleSetFreqOffset(s32 handle, f32 offset) {
    MusChannel *ch;
    s32 count;
    s32 i;

    if (handle == 0) {
        return 0;
    }

    musLock();
    count = 0;
    for (i = 0; i < musNumChannels; i++) {
        ch = &musChannels[i];
        if (ch->handle == handle) {
            ch->pitchOffset = ch->fineTune + offset;
            count++;
        }
    }
    musUnlock();
    return count;
}

/*
 * 801C0DAC: tempo scale of a handle, clamped to 1..256; 128 is normal speed.
 * Each channel's step becomes songTempoStep * tempo / 128.
 */
s32 MusHandleSetTempo(s32 handle, s32 tempo) {
    MusChannel *ch;
    s32 count;
    s32 i;

    if (handle == 0) {
        return 0;
    }

    if (tempo <= 0) {
        tempo = 1;
    } else if (tempo > 0x100) {
        tempo = 0x100;
    }

    musLock();
    count = 0;
    for (i = 0; i < musNumChannels; i++) {
        ch = &musChannels[i];
        if (ch->handle == handle) {
            ch->tempoScale = (s16)tempo;
            ch->tempoStep = (u16)((ch->songTempoStep * tempo) >> 7);
            count++;
        }
    }
    musUnlock();
    return count;
}

/*
 * 801C0E3C: reverb amount of a handle, clamped to 0..127. The player applies
 * it at the next note of each channel.
 */
s32 MusHandleSetReverb(s32 handle, s32 reverb) {
    MusChannel *ch;
    s32 count;
    s32 i;

    if (handle == 0) {
        return 0;
    }

    if (reverb < 0) {
        reverb = 0;
    } else if (reverb > 0x7F) {
        reverb = 0x7F;
    }

    musLock();
    count = 0;
    for (i = 0; i < musNumChannels; i++) {
        ch = &musChannels[i];
        if (ch->handle == handle) {
            ch->handleReverb = (u8)reverb;
            /* The next note sends the reverb mix again unless the channel's reverb is 0xFF. */
            ch->lastReverb = 0xFF;
            count++;
        }
    }
    musUnlock();
    return count;
}

/* 801C0FD8: returns the bank of the first channel of a handle, or NULL. */
MusBank *MusHandleGetPtrAddr(s32 handle) {
    MusBank *bank;
    s32 i;

    if (handle == 0) {
        return NULL;
    }

    musLock();
    bank = NULL;
    for (i = 0; i < musNumChannels; i++) {
        if (musChannels[i].handle == handle) {
            bank = musChannels[i].bank;
            break;
        }
    }
    musUnlock();
    return bank;
}

/*
 * 801C102C: pauses the channels of a handle at the start of the next player
 * call. Returns 1, or 0 when the ring is full. Handle 0 is not refused.
 */
s32 MusHandlePause(s32 handle) {
    s32 result;

    musLock();
    result = musPostMessage(MUS_MSG_PAUSE, handle);
    musUnlock();
    return result;
}

/* 801C1054: the opposite of MusHandlePause. MusStartSong uses it to start a song. */
s32 MusHandleUnPause(s32 handle) {
    s32 result;

    musLock();
    result = musPostMessage(MUS_MSG_UNPAUSE, handle);
    musUnlock();
    return result;
}

/*
 * 801C1080: switches the reverb to preset fxtype at the start of the next
 * player call. It records fxtype as the default preset even when the ring is
 * full. Returns 1, or 0 when the ring is full.
 */
s32 MusSetFxType(s32 fxtype) {
    s32 result;

    musLock();
    musFxType = fxtype;
    result = musPostMessage(MUS_MSG_FX_TYPE, fxtype);
    musUnlock();
    return result;
}

/*
 * 801C10B4: allows (1) or forbids (0) the reverb changes of songs (command
 * 0xAA). Forbidding them also posts a change back to the default preset. When
 * that post fails, the setting stays as it was and the result is 0.
 */
s32 MusSetSongFxChange(s32 onoff) {
    s32 result;

    musLock();
    result = 1;
    if (onoff == 0) {
        result = MusSetFxType(musFxType);
    }
    if (result != 0) {
        musSongFxChange = onoff;
    }
    musUnlock();
    return result;
}

/* ------------------------------------------------------------------------
 * Message ring
 * ---------------------------------------------------------------------- */

/* 801C1100: allocates the ring from the libmus heap. Called by MusInitialize. */
void musFifoInit(s32 length) {
    if (length < MUS_FIFO_MIN) {
        length = MUS_FIFO_MIN;
    } else if (length > MUS_FIFO_MAX) {
        length = MUS_FIFO_MAX;
    }
    musFifo = (MusMessage *)alHeapDBAlloc(NULL, 0, &musHeap, 1, length * (s32)sizeof(MusMessage));
    musFifoSize = length;
    musFifoWrite = 0;
    musFifoRead = 0;
}

/* 801C1184: runs every waiting message, oldest first. Called by the player at the start of each call. */
void musFifoProcess(void) {
    while (musFifoRead != musFifoWrite) {
        musFifoExecute(&musFifo[musFifoRead]);
        musFifoRead++;
        if (musFifoRead == musFifoSize) {
            musFifoRead = 0;
        }
    }
}

/* 801C1228: runs one message. Unknown types do nothing. */
void musFifoExecute(MusMessage *msg) {
    switch (msg->type) {
        case MUS_MSG_PAUSE:
            musHandleSetFlags(msg->value, ~MUS_CHANNEL_PAUSED, MUS_CHANNEL_PAUSED);
            break;

        case MUS_MSG_UNPAUSE:
            musHandleSetFlags(msg->value, ~MUS_CHANNEL_PAUSED, 0);
            break;

        case MUS_MSG_FX_TYPE:
            ChangeCustomEffect(msg->value);
            break;
    }
}

/*
 * 801C12A0: copies a message into the ring. One entry always stays empty: the
 * ring is full when the write index would reach the read index. Returns 1, or
 * 0 when the ring is full.
 */
s32 musFifoPost(MusMessage *msg) {
    s32 next;

    next = (musFifoWrite + 1) % musFifoSize;
    if (next == musFifoRead) {
        return 0;
    }
    musMemCopy(msg, &musFifo[musFifoWrite], sizeof(MusMessage));
    musFifoWrite = next;
    return 1;
}

/*
 * 801C2BB0: for every channel of a handle, flags = (flags & keep) | set.
 * There is no test for handle 0: it changes every channel without a handle,
 * free ones included. musChannelReset clears the flags when a channel starts.
 */
void musHandleSetFlags(s32 handle, u32 keep, u32 set) {
    MusChannel *ch;
    s32 i;

    for (i = 0; i < musNumChannels; i++) {
        ch = &musChannels[i];
        if (ch->handle == handle) {
            ch->flags = (ch->flags & keep) | set;
        }
    }
}
PORT_C_END
