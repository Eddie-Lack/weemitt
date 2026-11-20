# libmus in Glover: a map for the native port

This document describes libmus, the music and sound effect driver in Glover (USA, NGVE), and its native
reimplementation in `src/audio/mus/`. The shared declarations are in
[`src/audio/mus/mus_internal.h`](../src/audio/mus/mus_internal.h). The names used below are the names in that
header.

## Contents

1. [Sources and method](#1-sources-and-method)
2. [Overview](#2-overview)
3. [Where libmus is in the ROM](#3-where-libmus-is-in-the-rom)
4. [Timing](#4-timing)
5. [Data formats](#5-data-formats)
6. [Structures](#6-structures)
7. [Globals](#7-globals)
8. [How the player works](#8-how-the-player-works)
9. [Handles, priorities and channel allocation](#9-handles-priorities-and-channel-allocation)
10. [Reverb set up](#10-reverb-set-up)
11. [Function reference](#11-function-reference)
12. [Platform glue and its native replacement](#12-platform-glue-and-its-native-replacement)
13. [Callers in the game](#13-callers-in-the-game)
14. [Rules for changing this code](#14-rules-for-changing-this-code)
15. [Notes for each file](#15-notes-for-each-file)
16. [Reimplementation status](#16-reimplementation-status)

## 1. Sources and method

Everything here comes from the ROM's machine code, as split by splat into `gloveworld/asm/C0800.s`, lines 1 to
5519. The data comes from `gloveworld/asm/data/` and from the ROM image itself. The port's own header `PR/libaudio.h`
gives the libaudio types. No libmus or SDK source code was used.

A draft in C of any function can be made with m2c. Extract the libmus part of the file once, then name the
function:

```sh
sed -n 1,5519p ../gloveworld/asm/C0800.s > libmus.s
python3 ../gloveworld/tools/m2c/m2c.py -t mips-gcc-c --globals none libmus.s \
    ../gloveworld/asm/data/D064.rodata.s ../gloveworld/asm/data/F50C0.data.s -f func_801C15B8
```

m2c gets some argument lists and types wrong (for example it misses the arguments that `MusBankInitialize`
passes through). Check its output against the assembly and against this map.

The analysis was checked against the game data: a parser built from the command table in
[section 5.2](#52-command-streams) reads all 60 songs and all 420 sound effects of the ROM to their ends with no
unknown byte.

## 2. Overview

libmus plays songs and sound effects. Both are byte streams of commands and notes. libmus has a fixed array of
channels (24 in Glover). Each channel plays one stream and owns one synthesizer voice. A song uses one channel
per track. A sound effect uses one channel.

- The game calls the `Mus*` API. Starting a song or an effect fills channels and returns a handle. The handle
  identifies all channels of one song or effect.
- The synthesizer calls `musPlayerHandler` (ROM `func_801C1338`) once per video frame of audio time. The handler
  advances every channel: it reads commands and notes, runs envelopes and effects, and sends pitch, volume, pan
  and reverb changes to the voices with the `alSyn*` API.
- Wave data comes from a pbank (a table of `ALWaveTable` entries, the "ptr" file) and a wbank (the raw ADPCM
  samples). Glover has two: a music bank and a sound effect bank.
- Reverb uses a custom version of libaudio's effect unit (`CustomInit` to `ChangeCustomEffect`), with six presets.

On the N64, libmus also runs an audio thread. That thread asks the synthesizer for a frame of commands, sends an
RSP task to the scheduler and feeds the audio interface. Wave data is copied from the cartridge with DMA. All of
this is platform glue (section 12). The port replaces it.

## 3. Where libmus is in the ROM

ROM offset = VRAM - 0x800FF000 for this segment.

| Part | VRAM | ROM | Notes |
| --- | --- | --- | --- |
| Code | 801BF800-801C4024 | C0800-C5024 | 109 functions, `C0800.s` lines 1-5519 |
| Command table | 801F40C0-801F416C | F50C0 | 43 function pointers, then a zero word |
| Glue data | 801F4170-801F418F | F5170 | frame counters and flags (section 7) |
| Reverb presets | 801F4190-801F439F | F5190 | six presets and a table (section 10) |
| Constants | 8010C170-8010C207 | D170 | doubles and floats, listed below |
| Variables | 802AB010-802AD3A0 | bss | section 7 |

Constants (rodata):

| Address | Type | Value | Used by |
| --- | --- | --- | --- |
| 8010C170, 8010C178 | f64 | 6.2831852 (not exactly 2 pi) | `musCmdVibratoUp`, `musCmdVibratoDown` |
| 8010C180 | f64 | 1/12 = 0.08333333333333333 | `musVoiceUpdatePitch` |
| 8010C188, 8010C1B8 | f64 | 0.693147180559945 | `musPow2` |
| 8010C190, 8010C1C0 | f64 | 0.240226506959101 | `musPow2` |
| 8010C198, 8010C1C8 | f64 | 0.0555041086648216 | `musPow2` |
| 8010C1A0, 8010C1D0 | f64 | 0.00961812910762848 | `musPow2` |
| 8010C1A8, 8010C1D8 | f64 | 0.00133335581464284 | `musPow2` |
| 8010C1B0, 8010C1E0 | f64 | 0.000154035303933816 | `musPow2` |
| 8010C1E8 | f32 | 99.9 (0x42C7CCCD) | `musChannelReset` (lastPitch) |
| 8010C1EC | f32 | 1/255 (0x3B808081) | `musChannelReset` (decayStep) |
| 8010C1F0 | f32 | 1/15 (0x3D888889) | `musChannelReset` (releaseStep) |
| 8010C200 | f64 | 173123.404906676 (0x4105221B3D3FB61B) | `CustomFxSet` |

Immediate constants in the code: 1.0, 2.0, 50.0, 64.0, 100.0, 1/64 (0x3F90000000000000), 1/32 (0x3D000000),
65536.0f, 1000.0f, 4294967296.0f (unsigned conversions).

## 4. Timing

**What drives the sequencer.** `MusInitialize` registers an `ALPlayer` (`musPlayer`) with `alSynAddPlayer`. Its
handler is `musPlayerHandler`. The synthesizer calls the handler when its sample clock reaches the player's call
time. The handler returns `musFrameMicros`, the delay in microseconds until the next call.

- `musFrameRate` is 50 when `osTvType == 0` (PAL) and 60 otherwise. The USA ROM on an NTSC console uses 60.
- `musFrameMicros = 1000000 / musFrameRate` (integer division): 16666 on NTSC.

So the sequencer runs once per video frame of *audio* time. It is not tied to the game's frame rate or to the
video retrace.

**How the ROM's synthesizer turns microseconds into samples** (for the native synthesizer step). In the ROM's
`alAudioFrame`, after each handler call the synthesizer adds
`(s32)(f32)((f64)((f32)micros * (f32)outputRate) / 1000000.0 + 0.5)` samples to the player's call time
(`func_801CB864`). With 16666 us and the NTSC output rate of 22047 Hz this is 367 samples, so the handler runs
about 60.07 times a second. Before the call, the synthesizer sets `paramSamples` to the call time rounded down to
a multiple of 16. Every `alSynSet*` change made inside the handler takes effect at that sample. The output rate
is what `osAiSetFrequency(22050)` returns: `48681812 / (s32)(48681812.0f / 22050.0f + 0.5f)` = 22047.

**Ticks.** Song time is counted in ticks, 48 ticks to the beat (quarter note). Channel clocks hold 1/256 ticks
(8.8 fixed point, `u32`).

- Each handler call adds `tempoStep` (`u16`) to `time` of every active, unpaused channel.
- Tempo command 0x85 with `bpm` sets `step = ((bpm * 0x6000) / 120) / musFrameRate` (two signed integer
  divisions). At 60 fps and 120 bpm the step is 409 (1.598 ticks per frame).
- A new channel gets `tempoStep = songTempoStep = (0x6000 / musFrameRate) & 0xFFFF`, which is 120 bpm.
- `MusHandleSetTempo(handle, t)` sets `tempoScale = t` (clamped to 1..256) and
  `tempoStep = (songTempoStep * t) >> 7`. So 128 is normal speed.
- Note lengths are in ticks. When a note is read, `noteStart = noteEnd` and `noteEnd += length << 8`. Events run
  while `noteEnd < time`. Several events can run in one frame.
- `noteTicks = (time - noteStart) >> 8` is updated at the end of each handler call. Portamento and vibrato use it.

**Frames.** Some counters count handler calls, not ticks: the envelope step counter (`envelopeSpeed` frames per
step), the trill on and off times, and the fade out of `MusStop` and `MusHandleStop`.

**Parameter ramps.** Every `alSynSetVol` call that libmus makes uses `musFrameMicros` as the ramp time.

## 5. Data formats

Byte order. All data below is big-endian in the ROM. The port's decision for each kind is in the last column of
this table.

| Data | Where it comes from | Port handling |
| --- | --- | --- |
| Song file | loaded from ROM by the game (`PrepareSong`) | stays big-endian; read with `musRead16/32`; never written |
| Sound effect streams | game data in `port_ram` (VRAM 801E7760-801E9CB4) | byte streams in ROM order; the port's loader must not swap them (section 12.3) |
| Effect list and priorities | game data in `port_ram` (VRAM 801E9CB4 and 801EA344) | converted once when the port loads the game's data: host pointers and host `s32` (section 12.3) |
| pbank ("ptr" file) | loaded from ROM by the game | converted to host order in place by `musBankInit`, except the flags word |
| wbank (samples) | ROM | not converted; the synthesizer decodes ADPCM bytes in ROM order |
| Reverb presets | libmus data | written as host-order C arrays (values in section 10) |

### 5.1 Song file

A song is loaded into RAM by the game. All offsets are relative to the start of the song. In the ROM,
`MusStartSong` relocates the header and tables in place when word 0x04 is below 0x400. The port does not modify
the song: it reads each word with `musRead32` and adds the song address itself. A zero offset means "none".

| Offset | Type | Meaning |
| --- | --- | --- |
| 0x00 | s32 | number of channels (24 in every Glover song) |
| 0x04 | u32 | offset of the command stream table: `u32[numChannels]` |
| 0x08 | u32 | offset of the volume stream table: `u32[numChannels]` |
| 0x0C | u32 | offset of the pitch bend stream table: `u32[numChannels]` |
| 0x10 | u32 | offset of the envelope table (7 bytes per entry) |
| 0x14 | u32 | offset of the drum table: `u32[]`, each an offset of a drum map |

- A zero entry in the command stream table means the track is unused. Glover songs use 8 to 11 of 24 tracks.
- A zero entry in the volume or bend table means the track has no such stream.
- The drum table entries are not relocated by the ROM. Command 0x9E adds the song address to the entry it reads.
  In all 60 Glover songs the drum table offset equals the envelope table offset, and command 0x9E never occurs.
- Envelope entry (7 bytes): speed, initial volume, attack steps, peak volume, decay steps, sustain volume, release
  steps. The same layout is the operand of command 0x84.
- Drum map: 4 bytes per note number: wave index (u8), envelope index (u8), pan (u8, halved when used), note
  (u8, replaces the note number).

The 60 songs of Glover are in ROM from 0x6F8720 (`atlantisTrackRomStart`) to 0x77BBE0
(`recordKeyFilesRomStart`). `PrepareSong` in `src/game/sound.c` lists them.

### 5.2 Command streams

A song track and a sound effect use the same stream format. Each byte is read with an unsigned byte load.

- A byte of 0x80 or more is a command. Its handler is `musCommands[byte - 0x80]`. Only 0x80-0xAA exist. A larger
  byte would read past the table (the word after it is zero) and crash.
- A byte below 0x80 is a note number. Note 0x60 is a rest.
- After the note: one velocity byte when `velocityOn` is set. Then the length, a variable length number, unless
  `fixedLength` is nonzero and `readNextLength` is clear.
- **Variable length number** ("varlen"): one byte `b`. If `b < 0x80` the value is `b`. Otherwise a second byte
  `c` follows and the value is `((b & 0x7F) << 8) + c` (15 bits). Commands 0x81, 0x8B, 0x90, 0xA7 and note
  lengths use it.
- Length 0x7FFF means "infinite": while the current note has it, the player stops reading events for the
  channel. Sound effects use it for looping sounds until the game stops them.

Command table (operands in stream order):

| Byte | Handler | ROM | Operands | Effect |
| --- | --- | --- | --- | --- |
| 0x80 | musCmdEnd | 801BF800 | none | Clears volumeStream, bendStream, song, fxNumber, handle, pendingWave. Returns NULL: the channel ends. |
| 0x81 | musCmdSetWave | 801BF824 | varlen | wave = value |
| 0x82 | musCmdPortamentoOn | 801BF85C | u8 t | portamentoTicks = t; if t != 0, portamentoPitch = notePitch |
| 0x83 | musCmdPortamentoOff | 801BF880 | none | portamentoTicks = 0 |
| 0x84 | musCmdSetEnvelope | 801BF88C | 7 bytes | envelope, see 8.4 |
| 0x85 | musCmdSetTempo | 801BF9E4 | u8 bpm | tempo, see 8.8 |
| 0x86 | musCmdReleaseAfterStart | 801BFAC4 | s16 (big-endian) | releaseAfterStart = value; releaseBeforeEnd = 0 |
| 0x87 | musCmdReleaseBeforeEnd | 801BFAB0 | u8 | releaseAfterStart = 0; releaseBeforeEnd = value |
| 0x88 | musCmdVibratoUp | 801BFAF4 | u8 delay, u8 period, u8 depth | vibrato, see 8.6 |
| 0x89 | musCmdVibratoDown | 801BFB88 | u8 delay, u8 period, u8 depth | same with a negative depth |
| 0x8A | musCmdVibratoOff | 801BFC20 | none | vibratoPeriod = 0; vibratoValue = 0.0f |
| 0x8B | musCmdFixedLength | 801BFC34 | varlen | fixedLength = value (0 = notes carry lengths) |
| 0x8C | musCmdReadNextLength | 801BFC78 | none | readNextLength = 1 |
| 0x8D | musCmdTranspose | 801BFC88 | u8 | transpose = value (used as signed) |
| 0x8E | musCmdIgnoreTranspose | 801BFC98 | none | ignoreTranspose = 1 (nothing clears it but a reset) |
| 0x8F | musCmdFineTune | 801BFCA8 | s8 cents | see 8.5 |
| 0x90 | musCmdEnvelopeIndex | 801BFD0C | varlen n | musCmdSetEnvelope(ch, song envelope table + n * 7); returns the stream after the varlen |
| 0x91 | musCmdKeepEnvelopeOn | 801BFD6C | none | keepEnvelope = 1 |
| 0x92 | musCmdKeepEnvelopeOff | 801BFD7C | none | keepEnvelope = 0 |
| 0x93 | musCmdTieOn | 801BFD88 | none | tie = 1 |
| 0x94 | musCmdTieOff | 801BFD98 | none | tie = 0 |
| 0x95 | musCmdLoopStart | 801BFDA4 | u8 count | see 8.9 |
| 0x96 | musCmdLoopEnd | 801BFE90 | none | see 8.9 |
| 0x97 | musCmdTrillOn | 801BFF40 | s8 depth, u8 on, u8 off | trillDepth, trillOnFrames, trillOffFrames |
| 0x98 | musCmdTrillOff | 801BFF60 | none | trillOnFrames = 0 |
| 0x99 | musCmdVelocityOn | 801BFF6C | none | velocityOn = 1 |
| 0x9A | musCmdVelocityOff | 801BFF7C | none | velocityOn = 0 |
| 0x9B | musCmdFixedVelocity | 801BFF88 | u8 | velocityOn = 0; fixedVelocity = value |
| 0x9C | musCmdSetPan | 801BFF9C | u8 | pan = value / 2 (0..127) |
| 0x9D | musCmdSkip2 | 801BFFBC | 2 bytes | ignored |
| 0x9E | musCmdDrumMapOn | 801BFFC8 | u8 n | drumMap = song + drumTable[n] (see 5.1) |
| 0x9F | musCmdDrumMapOff | 801BFFF0 | none | drumMap = NULL |
| 0xA0 | musCmdSkip1 | 801BFFFC | 1 byte | ignored |
| 0xA1 | musCmdGoto | 801C0008 | u16 data, u16 vol, u16 bend (big-endian) | volumeStream = volumeStreamStart + vol; volumeCount = 1; bendStream = bendStreamStart + bend; bendCount = 1; returns dataStart + data |
| 0xA2 | musCmdSetReverb | 801C006C | u8 | reverb = value |
| 0xA3 | musCmdRandomTranspose | 801C007C | u8 range, u8 base | transpose = (u8)(musRandom(range) + base) |
| 0xA4 | musCmdRandomVolume | 801C00CC | u8 range, u8 base | volume = (u8)(musRandom(range) + base) |
| 0xA5 | musCmdRandomPan | 801C011C | u8 range, u8 base | pan = (u8)(musRandom(range) + base) |
| 0xA6 | musCmdSetVolume | 801C016C | u8 | volume = value |
| 0xA7 | musCmdStartEffect | 801C017C | varlen number | starts a linked effect, see 9.4 |
| 0xA8 | musCmdBendRange | 801C025C | u8 | bend = bendValue * bendValue (sic); then bendRange = (f32)((f64)(f32)value * (1/64)) |
| 0xA9 | musCmdPanSweep | 801C02AC | u8 | panSweepSpeed = value |
| 0xAA | musCmdChangeEffect | 801C02BC | u8 | if musSongFxChange == 1: ChangeCustomEffect(value) |

Notes on the table:

- Every handler takes `(MusChannel *ch, u8 *p)` with `p` just after the command byte, and returns the next byte.
- 0x86 reads its operand as `(s16)((p[0] << 8) | p[1])` and stores it in a `u16` field. Envelope code reads it
  back unsigned.
- 0x9C and the drum map pan use a signed division by 2 of a value loaded unsigned. The result equals
  `value >> 1`.
- 0xA8 sets `bend` from the square of `bendValue`, not from the new range. This looks like a bug in the original
  code. Keep it: the next bend stream event overwrites `bend` anyway.
- 0xA1 reads 6 operand bytes but returns a new position, so the operand size does not matter to the parser.
- Sound effects have `song == NULL`. Commands 0x90 and 0x9E read the song header, so they must not occur in
  effects (they would read address 0x10 or 0x14 on the N64). They do not occur in Glover's effects.
- Commands used by Glover's data: 0x80-0x82, 0x84-0x88, 0x8A, 0x8B, 0x8D, 0x8F, 0x90, 0x95-0x97, 0x99-0x9C,
  0xA2, 0xA3, 0xA5-0xA8. The others are never used but must still be translated.

### 5.3 Volume and pitch bend streams

Each song track can have one volume stream and one pitch bend stream. They have no end marker. They are read as
time passes, one event per countdown (see 8.7).

- A byte `b < 0x80`: value `b`, lasting 1 tick.
- A byte `b >= 0x80`: value `b & 0x7F`, then a varlen `n`; the value lasts `n + 2` ticks.

For the volume stream the value goes to `volume`. For the bend stream `bendValue = (f32)((f64)(f32)v - 64.0)` and
`bend = bendValue * bendRange`.

### 5.4 Sound effects

- **Effect list** (`libmus_sfx_list`, VRAM 801E9CB4, ROM EACB4): 420 big-endian RAM addresses, one per effect
  number. Each points at a command stream in the game's data (VRAM 801E7760-801E9CB4, ROM E8760-EACB4). Effect
  0 is a stream that holds only 0x80.
- **Priority list** (`libmus_priority_list`, VRAM 801EA344, ROM EB344): 420 big-endian `s32`. Effect 0 has
  priority 0, every other effect has 100. The words after entry 419 are unrelated text.
- `MusInitialize` stores `config->default_fxbank` in `musFxStreams` and `config->priority` in
  `musFxPriorities`. libmus reads `musFxStreams[n]` as a pointer and `musFxPriorities[n]` as an `s32`. In the
  port both tables must hold host values. Section 12.3 says how the port's loader converts them.
- An effect stream is played like one song track with no volume or bend stream.

### 5.5 pbank (the "ptr" file)

Glover has two pbanks. The game loads both into `pbankBuffer` and passes them to libmus.

| Bank | ROM | Size | Waves | wbank (ROM) | Use |
| --- | --- | --- | --- | --- | --- |
| pbank1 | 4D58A0-4DA9A0 | 0x5100 | 111 | wbank1 at 4E3EE0 ("nosfx.wbk") | music, passed to `MusBankInitialize` and `MusBankStartSong` |
| pbank2 | 4DA9A0-4E3EE0 | 0x9540 | 210 | wbank2 at 607280 ("SFX.WBK") | sound effects, the default bank (`MusConfig.ptr`) |

Header (offsets relative to the bank start; `MusBank` in the header file):

| Offset | Type | Meaning |
| --- | --- | --- |
| 0x00 | char[16] | "N64 PtrTablesV2" and a zero |
| 0x10 | u32 | flags. Bit 31 is set by `musBankInit` and means "already initialised". Zero in the ROM. |
| 0x14 | char[12] | wbank file name. libmus does not read it. |
| 0x20 | s32 | number of waves |
| 0x24 | u32 | offset of `u8 baseNote[count]` |
| 0x28 | u32 | offset of the pitch table: one 4-byte word per wave |
| 0x2C | u32 | offset of the wave table: `u32[count]`, offsets of `ALWaveTable` entries |

Pitch table. In the file only the first byte of each word is used (the other three are zero in Glover). It is a
signed detune in hundredths of a semitone. `musBankInit` replaces the word with an `f32`:

```
pitch[i] = (f32)((f64)(f32)(s8)fileByte / 100.0);
pitch[i] = pitch[i] + (f32)(s8)(u8)(baseNote[i] - 0x30);
```

The ROM converts the signed values with `-(0x100 - v)` for bytes with bit 7 set. The result is the pitch of the
wave relative to note 0, in semitones: a note `n` plays at `pitch[wave] + n` semitones.

Wave entry (`ALWaveTable` from `PR/libaudio.h`, 0x14 bytes):

| Offset | Field | File content | After `musBankInit` |
| --- | --- | --- | --- |
| 0x00 | base | offset in the wbank | `wbank + offset`; left alone when the top byte is 0xFF (64DD data) |
| 0x04 | len | byte count | host order |
| 0x08 | type | 0 = ADPCM, 1 = raw 16-bit | unchanged |
| 0x09 | flags | 0 | 1 (`MUS_WAVE_RELOCATED`); a wave with flags != 0 is skipped |
| 0x0C | loop | offset in the bank, or 0 | host pointer to `ALADPCMloop` or `ALRawLoop`, host order |
| 0x10 | book | offset in the bank (ADPCM only) | host pointer to `ALADPCMBook`, host order |

The ROM relocates `book` only for ADPCM waves, and relocates `loop` only when it is nonzero. `ALADPCMloop` has
`u32 start, end, count` and `s16 state[16]`. `ALRawLoop` has `u32 start, end, count`. `ALADPCMBook` has
`s32 order, npredictors` and `s16 book[order * npredictors * 8]`.

Glover's banks hold only ADPCM waves (`order` 2, `npredictors` 4). No wave, loop or book is shared between two
entries, and no base has the 0xFF tag. Loops use `count` 0xFFFFFFFF (forever).

**Port decision: convert in place, once.** The ROM already modifies the bank in place (relocation and the pitch
floats). With 4-byte pointers on the host every field keeps its offset. `musBankInit` does:

1. Return if `musRead32(bank->flags) & MUS_BANK_INITIALISED`. Otherwise set bit 31 of the big-endian word
   (`bank->flags[0] |= 0x80`). The flags word stays big-endian forever. The test reads it the same way before and
   after the conversion.
2. Convert `count` and the three table offsets to host order, and turn the offsets into host pointers (zero stays
   NULL).
3. For each wave: compute the pitch float (reading the byte before writing the word); turn the wave table entry
   into a host pointer; if the wave's flags byte is 0, convert and relocate the wave entry, its loop and its book,
   then set flags to 1. Convert each loop and book once (guard with a set of converted addresses if a future bank
   shares them).

`base` keeps the ROM's arithmetic: `base = wbank + offset`. The game passes `wbank` as a cartridge address, the
value of `_wbank1SegmentRomStart` (0xB04E3EE0) or `_wbank2SegmentRomStart` (0xB0607280); the port's ROM symbols
hold these values. So `base` is a cartridge address too. The synthesizer turns it into a host pointer through the
DMA routine, which returns `port_rom + (addr & 0x0FFFFFFF)` (section 12.2).

### 5.6 wbank

The wbank is raw sample data in the ROM: ADPCM frames as the libaudio decoder expects them. libmus never reads
it. The synthesizer reads `len` bytes from `base`. The ROM's DMA routine always copies a whole DMA buffer (0x600
bytes) from `base` rounded down to an even address, so it may read past the end of a wave.

### 5.7 Messages

The game thread sends some requests through a ring buffer that the player reads at the start of each call. Each
entry is a `MusMessage` (8 bytes: `u8 type` at 0, `s32 value` at 4).

| Type | Sent by | Action in `musFifoExecute` |
| --- | --- | --- |
| 0 `MUS_MSG_PAUSE` | `MusHandlePause` | `musHandleSetFlags(value, ~1, 1)`: set `MUS_CHANNEL_PAUSED` |
| 1 `MUS_MSG_UNPAUSE` | `MusHandleUnPause` (also from `MusStartSong`) | `musHandleSetFlags(value, ~1, 0)`: clear it |
| 2 `MUS_MSG_FX_TYPE` | `MusSetFxType` | `ChangeCustomEffect(value)` |

Other types are ignored. The ring has `musFifoSize` entries: `MusConfig.fifo_length` clamped to 64..1024. Glover
leaves the field at 0, so the ring has 64 entries. `musFifoPost` refuses a message when the ring is full (it
returns 0).

## 6. Structures

All structures are in `mus_internal.h` with their ROM offsets, and a compile test with `gcc -m32` confirms every
offset and size listed there.

### 6.1 MusConfig (0x48 bytes)

The game fills `musicPlayer` (`src/game/dma.c`) and passes it to `MusInitialize`.

| Offset | Name in the header | Game's name in dma.c | Glover value | Use in libmus |
| --- | --- | --- | --- | --- |
| 0x00 | control_flag | control_flag | 0 | `musControlFlag`; bit 0 means wave data is in RAM |
| 0x04 | channels | channels | 24 | `musNumChannels` |
| 0x08 | sched | sched | `&sched` | glue only (scheduler client) |
| 0x0C | thread_priority | thread_priority | 0x8C | audio thread priority, and the priority of every voice |
| 0x10 | heap | heap | `audio_memory` | zeroed, then used as `musHeap` |
| 0x14 | heap_length | heap_length | 0x493E0 | |
| 0x18 | ptr | ptr | pbank2 | default pbank, initialised with `wbk` |
| 0x1C | wbk | wbk | wbank2 | |
| 0x20 | default_fxbank | default_fxbank | `libmus_sfx_list` | `musFxStreams` |
| 0x24 | priority | priority | `libmus_priority_list` | `musFxPriorities` |
| 0x28 | fifo_length | unk28[0..3] | 0 | `musFifoInit` (clamped to 64..1024) |
| 0x2C | unused2C | unk28[4..7] | 0 | not read |
| 0x30 | syn_updates | fifo_length | 0x400 | `ALSynConfig.maxUpdates` |
| 0x34 | syn_output_rate | syn_output_rate | 22050 | requested output rate |
| 0x38 | syn_rsp_cmds | syn_updates | 0x2000 | glue: command list length |
| 0x3C | syn_retraceCount | syn_rsp_cmds | 3 | glue: frame size multiplier |
| 0x40 | syn_num_dma_bufs | syn_retraceCount | 40 | glue: DMA buffer count |
| 0x44 | syn_dma_buf_size | syn_dma_buf_size | 0x600 | glue: DMA buffer size |

The game's names for 0x30-0x40 are one place off from how libmus uses the words. The values the game writes are
what matters; the layout is the same.

### 6.2 MusChannel (0x134 bytes)

One per channel, in `musChannels`. Channel `i` always uses voice `musVoices[i]`. The table gives each field's type,
its meaning and its value after `musChannelReset`.

| Offset | Field | Type | Meaning | Reset value |
| --- | --- | --- | --- | --- |
| 0x000 | flags | u32 | bit 0: paused (`MUS_CHANNEL_PAUSED`) | 0 |
| 0x004 | pdata | u8 * | next stream byte; NULL = free channel | NULL |
| 0x008 | pendingWave | ALWaveTable * | wave to start at the next call (after a volume ramp to 0) | NULL |
| 0x00C | time | u32 | channel clock (1/256 ticks) | 0 |
| 0x010 | stopCount | s32 | frames left in the fade out, -1 = not stopping | -1 |
| 0x014 | volumeTime | u32 | volume stream clock | 0 |
| 0x018 | bendTime | u32 | bend stream clock | 0 |
| 0x01C | stopLength | s32 | fade out length; divisor of the fade | 0 |
| 0x020 | vibratoDepth | f32 | semitones | 0 |
| 0x024 | bend | f32 | semitones (bendValue * bendRange) | 0 |
| 0x028 | lastPitch | f32 | last pitch sent, semitones | 99.9 |
| 0x02C | notePitch | f32 | current note pitch, semitones | 0 |
| 0x030 | pitchOffset | f32 | fineTune plus the handle's frequency offset | 0 |
| 0x034 | bendStream | u8 * | | NULL |
| 0x038 | volumeStream | u8 * | | NULL |
| 0x03C | noteEnd | u32 | clock of the next event | 0 |
| 0x040 | noteStart | u32 | clock when the current note began | 0 |
| 0x044 | handle | s32 | 0 = none | 0 |
| 0x048 | priority | s32 | effect priority | 0 |
| 0x04C | portamentoFrom | f32 | start of the slide | 0 |
| 0x050 | portamentoPitch | f32 | pitch reached by the slide | 0 |
| 0x054 | releaseTime | u32 | clock when the release starts; 0x7FFFFFFF for infinite notes | 0 |
| 0x058 | attackStep | f32 | | 1.0 |
| 0x05C | decayStep | f32 | | 1/255 |
| 0x060 | releaseStep | f32 | | 1/15 |
| 0x064 | envelopeScale | s32 | 1024 / envelopeSpeed | 0 |
| 0x068 | vibratoValue | f32 | last vibrato offset | 0 |
| 0x06C | bendRange | f32 | semitones per bend unit | 1/32 |
| 0x070 | bendValue | f32 | -64..63 | 0 |
| 0x074 | song | u8 * | song file; NULL for effects | NULL |
| 0x078 | bank | MusBank * | bank for waves | `musCurrentBank` |
| 0x07C | dataStart | u8 * | start of the stream | NULL |
| 0x080 | drumMap | u8 * | NULL = off | NULL |
| 0x084 | bendStreamStart | u8 * | | NULL |
| 0x088 | volumeStreamStart | u8 * | | NULL |
| 0x08C | fineTune | f32 | semitones | 0 |
| 0x090 | panSweepTime | u32 | pan sweep clock | 0 |
| 0x094 | tempoScale | s16 | 128 = normal | 0x80 |
| 0x096 | length | u16 | current note length in ticks | 1 |
| 0x098 | tempoStep | u16 | added to time each call | 0x6000 / fps |
| 0x09A | handleVolume | s16 | 128 = normal | 0x80 |
| 0x09C | lastVolume | u16 | last volume sent | 0xFFFF |
| 0x09E | volumeCount | u16 | ticks to the next volume event | 1 |
| 0x0A0 | bendCount | u16 | ticks to the next bend event | 1 |
| 0x0A2 | fxNumber | u16 | effect number, 0 for song tracks | 0 |
| 0x0A4 | songTempoStep | u16 | tempo step before tempoScale | 0x6000 / fps |
| 0x0A6 | noteTicks | u16 | ticks since note start | 0 |
| 0x0A8 | fixedLength | u16 | | 0 |
| 0x0AA | wave | u16 | wave index | 0 |
| 0x0AC | handlePan | s16 | 128 = normal | 0x80 |
| 0x0AE | releaseAfterStart | u16 | | 0 |
| 0x0B0 | releaseBeforeEnd | u16 | | 0 |
| 0x0B2 | vibratoDelay | u8 | ticks | 0 |
| 0x0B3 | readNextLength | u8 | | 0 |
| 0x0B4 | portamentoTicks | u8 | | 0 |
| 0x0B5 | transpose | u8 | signed in use | 0 |
| 0x0B6 | ignoreTranspose | u8 | | 0 |
| 0x0B7 | velocity | u8 | | 0 |
| 0x0B8 | volume | u8 | | 0x7F |
| 0x0B9 | pan | u8 | | 0x40 |
| 0x0BA | lastPan | u8 | | 0xFF |
| 0x0BB | envelopeSpeed | u8 | | 1 |
| 0x0BC | envelopeInitial | u8 | | 0 |
| 0x0BD | envelopePeak | u8 | | 0x7F |
| 0x0BE | envelopeSustain | u8 | | 0x7F |
| 0x0BF | envelopePhase | u8 | `MUS_ENV_*` | 0 |
| 0x0C0 | envelopeVolume | u8 | | 0 |
| 0x0C1 | envelopeCounter | u8 | | 0 |
| 0x0C2 | attackSteps | u8 | | 1 |
| 0x0C3 | decaySteps | u8 | | 0xFF |
| 0x0C4 | releaseSteps | u8 | | 0xF |
| 0x0C5 | voiceOn | u8 | voice is playing | kept |
| 0x0C6 | reverb | u8 | song reverb | 0 |
| 0x0C7 | handleReverb | u8 | | 0 |
| 0x0C8 | lastReverb | u8 | 0xFF forces an update | 0xFF |
| 0x0C9 | releaseVolume | u8 | | 0 |
| 0x0CA | trillOnFrames | u8 | 0 = off | 0 |
| 0x0CB | trillOffFrames | u8 | | 0 |
| 0x0CC | trillCounter | u8 | | 0 |
| 0x0CD | trillOffset | s8 | | 0 |
| 0x0CE | velocityOn | u8 | | 0 |
| 0x0CF | fixedVelocity | u8 | | 0x7F |
| 0x0D0 | panSweepSpeed | u8 | 0 = off | 0 |
| 0x0D1 | vibratoPeriod | u8 | 0 = off | 0 |
| 0x0D2 | keepEnvelope | u8 | | 0 |
| 0x0D3 | tie | u8 | | 0 |
| 0x0D4 | trillDepth | s8 | | 0 |
| 0x0D5 | panSweepFraction | u8 | | 0 |
| 0x0D6 | panSweepDown | u8 | | 0 |
| 0x0D7 | loopDepth | u8 | | 0 |
| 0x0D8 | vibratoStep | f32 | radians per tick | 0 |
| 0x0DC | loopStart[4] | u8 * | | NULL |
| 0x0EC | loopVolumeStream[4] | u8 * | | NULL |
| 0x0FC | loopBendStream[4] | u8 * | | NULL |
| 0x10C | loopVolumeCount[4] | u16 | | 0 |
| 0x114 | loopBendCount[4] | u16 | | 0 |
| 0x11C | loopCount[4] | u8 | | 0 |
| 0x120 | loopVolume[4] | u8 | | 0 |
| 0x124 | loopBendValue[4] | u8 | | 0 |
| 0x128 | unused128 | u8[12] | not used | 0 |

The default envelope is not consistent (`envelopeScale` 0 and `attackStep` 1.0 do not match the speed and levels).
With `envelopeScale` 0 the envelope never leaves its first level, so a track that never sets an envelope stays
silent. Keep these values as they are.

### 6.3 Other structures

- `MusBank` (0x30): the pbank header, section 5.5.
- `MusMessage` (8): section 5.7.
- `MusAudioMgrConfig` (0xC): built on `MusInitialize`'s stack and passed to `amCreateAudioMgr`:
  `outputRate` = config 0x34, `framesPerField` = config 0x3C, `maxACMDSize` = config 0x38.
- `ALSynConfig` (libaudio.h) built on `MusInitialize`'s stack: `maxVVoices = maxPVoices = channels`,
  `maxUpdates = syn_updates`, `maxFXbusses` not set (stack garbage; never read), `dmaproc = 0` (set by
  `amCreateAudioMgr`), `heap = &musHeap`, `outputRate = 0` (set by `amCreateAudioMgr`), `fxType = 2`, `params`
  not set. `CustomSetDefaultFx` overwrites `fxType` with 6 (`AL_FX_CUSTOM`) and `params` with `musFxBigRoom`.
- `ALVoiceConfig` for each voice: `priority = (s16)config->thread_priority`, `fxBus = 0`, `unityPitch = 0`.
- `MusReverbState` (0x30, `musReverb`, ROM `gReverbFx`): section 10.
- Synthesizer internals touched by `CustomSynNew` and the reverb code (`ALFilter`, `ALFx`, `ALDelay`,
  `ALAuxBus`, `ALMainBus`, `ALSave`, `ALResampler`, `ALLowPass`, `PVoice`, `ALParam`): in
  `src/audio/synth/synth_internal.h`, which `mus_internal.h` includes. Their layouts come from the ROM's libaudio
  (audio-synth-map.md, section 5).

## 7. Globals

libmus variables (bss, all zero at boot):

| Address | ROM name | Header name | Type | Meaning |
| --- | --- | --- | --- | --- |
| 802AB010 | D_802AB010 | musPlayer | ALPlayer (0x14, 0x18 reserved) | the player registered with the synthesizer |
| 802AB028 | D_802AB028 | musHeap | ALHeap (0x10) | libmus heap; `MusInitialize` returns `cur - base` (D_802AB02C is `cur`) |
| 802AB038 | D_802AB038 | musNumChannels | s32 | |
| 802AB03C | D_802AB03C | musVoices | ALVoice * | `channels` voices, 0x1C each |
| 802AB040 | D_802AB040 | musChannels | MusChannel * | `channels` channels |
| 802AB044 | D_802AB044 | musFxStreams | u8 ** | effect streams |
| 802AB048 | D_802AB048 | musFxPriorities | s32 * | effect priorities |
| 802AB04C | D_802AB04C | musFrameRate | s32 | 50 or 60 |
| 802AB050 | D_802AB050 | musFrameMicros | s32 | handler interval and volume ramp time |
| 802AB054 | D_802AB054 | musFxMasterVolume | u16 | `MusSetMasterVolume` bit 0; used for channels with fxNumber != 0 |
| 802AB056 | D_802AB056 | musSongMasterVolume | u16 | bit 1; channels with fxNumber == 0 |
| 802AB058 | D_802AB058 | musNextHandle | s32 | starts at 1 |
| 802AB05C | D_802AB05C | musRandomSeed | s32 | starts at 0x12345678 |
| 802AB060 | D_802AB060 | musCurrentBank | MusBank * | bank given to new channels |
| 802AB064 | D_802AB064 | musDefaultBank | MusBank * | `config->ptr` |
| 802AB068 | D_802AB068 | musSongFxChange | s32 | command 0xAA works only when 1 |
| 802AB06C | D_802AB06C | musFxType | s32 | reverb preset index, 2 after `MusInitialize` |
| 802AB070 | __muscontrol_flag | musControlFlag | u32 | `config->control_flag` |
| 802AB074 | D_802AB074 | musFifoRead | s32 | |
| 802AB078 | D_802AB078 | musFifoWrite | s32 | |
| 802AB07C | D_802AB07C | musFifoSize | s32 | |
| 802AB080 | D_802AB080 | musFifo | MusMessage * | (0x10 reserved, 4 used) |
| 802AB2C8 | D_802AB2C8 | musGlobals | ALGlobals (0x4C) | the synthesizer; `alGlobals` points here |
| 802AD370 | gReverbFx | musReverb | MusReverbState (0x30) | reverb buffers |

Glue variables (the port does not need them):

| Address | ROM name | Meaning |
| --- | --- | --- |
| 802AB090 | __am (0x18) | audio manager: `Acmd *cmdList[2]` at 0, `AudioInfo *info[4]` at 8 (D_802AB098, D_802AB0A0) |
| 802AB0A8 | D_802AB0A8 | `OSThread` of the audio thread |
| 802AB258, 802AB270 | | message queue for retrace messages, and its 8 slots |
| 802AB290, 802AB2A8 | | message queue for "task done" replies, and its 8 slots |
| 802AB318-802AD318 | (inside D_802AB2C8) | audio thread stack (0x2000 bytes; the top is passed to `osCreateThread`) |
| 802AD318 | D_802AD318 (0xC) | DMA state: `u8 initialised`, used list at 4, free list at 8 |
| 802AD324 | D_802AD324 | smallest frame (frame size - 16 samples) |
| 802AD328 | D_802AD328 | frame size in samples |
| 802AD32C | D_802AD32C | largest frame (extra + frame size + 16) |
| 802AD330 | D_802AD330 | command list length |
| 802AD338 | D_802AD338 | message queue for DMA completion |
| 802AD350 | D_802AD350 | scheduler |
| 802AD354 | D_802AD354 | DMA buffers (0x14 each: link, start address, last frame used, data pointer) |
| 802AD358 | D_802AD358 | `OSIoMesg` array, 2 per DMA buffer |
| 802AD35C | D_802AD35C | `OSMesg` array for the DMA queue |
| 802AD360 | D_802AD360 | DMA buffer size |
| 802AD364 | D_802AD364 | extra samples per frame (0x50 below 22050 Hz, else 0xF0) |
| 802AD368 | D_802AD368 | cartridge PI handle |
| 802AD36C | D_802AD36C | 64DD PI handle |
| 801F4170 | D_801F4170 | audio frame counter |
| 801F4174 | D_801F4174 | DMA requests this frame |
| 801F4178 | D_801F4178 | command list in use (0 or 1) |
| 801F417C | D_801F417C | thread created flag |
| 801F4180 | lastInfo | last `AudioInfo` sent to the audio interface |
| 801F4184 | D_801F4184 | flag written only by `amHandleTaskDone`; no effect |

libaudio's `alGlobals` (801F46D0) is set to `&musGlobals` by `CustomInit` when it is NULL.

## 8. How the player works

### 8.1 musPlayerHandler (801C1338)

Called by the synthesizer. Argument: the `ALPlayer`. Returns `musFrameMicros`.

```
musFifoProcess();
for (i = 0; i < musNumChannels; i++) {
    ch = &musChannels[i];
    if (ch->pdata == NULL || (ch->flags & MUS_CHANNEL_PAUSED)) continue;
    if (ch->pendingWave != NULL) musVoiceStart(ch, i);
    ch->time += ch->tempoStep;
    if (ch->length != MUS_LENGTH_INFINITE) {
        while (ch->noteEnd < ch->time && ch->pdata != NULL)      /* unsigned compare */
            musChannelNextEvent(ch, i);
        if (ch->pdata == NULL) continue;                         /* skips the rest, even noteTicks */
    }
    if (ch->volumeStream != NULL && ch->volumeTime < ch->time) musVolumeStreamUpdate(ch);
    if (ch->bendStream != NULL && ch->bendTime < ch->time) musBendStreamUpdate(ch);
    if (ch->stopCount != -1) {
        ch->stopCount--;
        if (ch->stopCount == -1) {
            ch->pdata = musCmdEnd(ch, NULL);                     /* NULL */
            if (ch->voiceOn) { ch->voiceOn = 0; alSynStopVoice(&musGlobals.drvr, &musVoices[i]); }
        }
    }
    if (ch->voiceOn) {
        if (ch->envelopePhase != 0) musEnvelopeUpdate(ch);
        if (ch->panSweepSpeed != 0 && ch->panSweepTime < ch->time) musPanSweepUpdate(ch);
        offset = ch->pitchOffset;
        if (ch->vibratoPeriod != 0) offset += musVibratoUpdate(ch);
        if (ch->trillOnFrames != 0) offset += musTrillUpdate(ch);
        if (ch->pendingWave == NULL) {
            musVoiceUpdatePitch(ch, i, offset);
            musVoiceUpdateVolumePan(ch, i);
        }
    }
    ch->noteTicks = (u16)((ch->time - ch->noteStart) >> 8);
}
return musFrameMicros;
```

The ROM reloads `musNumChannels` after each channel; the loop bound can change only if a handler changes it,
which none does.

### 8.2 musChannelNextEvent (801C15B8): commands, then one note

Arguments: channel, channel index. No return value.

1. `p = ch->pdata`. While `p != NULL` and `*p >= 0x80`: `p = musCommands[*p & 0x7F](ch, p + 1)`.
2. `ch->pdata = p`. If `p == NULL`: if `voiceOn`, clear it, `alSynSetVol(voice, 0, musFrameMicros)` and
   `alSynStopVoice(voice)`. Return.
3. `portamentoFrom = portamentoPitch`. `note = *p++`. If `velocityOn`, `velocity = *p++`; else
   `velocity = fixedVelocity`.
4. Length: if `fixedLength != 0` and `readNextLength == 0`, `length = fixedLength`. Otherwise clear
   `readNextLength` (only on the fixed-length path) and read a varlen into `length`.
5. `noteStart = noteEnd; noteEnd += length << 8; noteTicks = 0; trillOffset = 0; trillCounter = trillOffFrames`.
6. If `note == 0x60` (rest): if `envelopePhase < 4`, start the release: `envelopePhase = 4`,
   `envelopeCounter = 1`, `releaseTime = time`, `releaseVolume = envelopeVolume`. Return.
7. `bank = ch->bank`. If `drumMap != NULL`, with `d = drumMap + note * 4`: `wave = d[0]`, `pan = d[2] / 2`,
   `musCmdSetEnvelope(ch, song envelope table + d[1] * 7)`, `note = d[3]`.
8. If `keepEnvelope == 0`: `musEnvelopeStart(ch)`.
9. If `panSweepSpeed != 0`: `musPanSweepStart(ch)`.
10. If `tie == 0`: `pendingWave = bank->waves[wave]`. Then if `voiceOn && lastVolume != 0`:
    `lastVolume = 0; alSynSetVol(voice, 0, musFrameMicros)` (the wave starts at the next call). Else
    `musVoiceStart(ch, index)` now.
11. Pitch: `t = (u8)(transpose * (1 - ignoreTranspose))`, then `t -= 256` when `t & 0x80`.
    `notePitch = bank->pitch[wave] + (f32)note; notePitch = notePitch + (f32)t`. (The ROM tests bit 7 as
    `(f32)(t & 0x80) != 0.0f`.)
12. Reverb: if `lastReverb != reverb`: `lastReverb = reverb` and
    `alSynSetFXMix(voice, (u8)(handleReverb + (((0x80 - handleReverb) * reverb) >> 7)))`.

`musVoiceStart` (801C19FC): if `voiceOn`, `alSynStopVoice(voice)`. Then `voiceOn = 1`,
`alSynStartVoice(&musGlobals.drvr, voice, pendingWave)`, `pendingWave = NULL`. It does not set pitch, volume or
pan; the next update calls do that only when the value differs from the last value sent. The native synthesizer
must keep a voice's pitch, volume, pan and reverb mix across a stop and a start, like libaudio.

### 8.3 Volume and pan (musVoiceUpdateVolumePan, 801C1A90)

All products are unsigned 32-bit (`multu`), shifts are logical:

```
vol = volume * envelopeVolume;          /* u8 * u8 */
vol = vol * velocity;
vol = vol * (u32)(s32)handleVolume;
vol >>= 13;
if (vol >= 0x8000) vol = 0x7FFF;
vol = vol * (fxNumber == 0 ? musSongMasterVolume : musFxMasterVolume);
vol >>= 15;
if (stopCount != -1) vol = (u32)(stopCount * vol) / (u32)stopLength;
if (vol != lastVolume) { lastVolume = vol; alSynSetVol(voice, (s16)vol, musFrameMicros); }
if (lastPan != pan) {
    lastPan = (u8)((((s32)pan * handlePan) >> 7) & 0x7F);   /* arithmetic shift */
    alSynSetPan(voice, lastPan);
}
```

The pan test compares `lastPan` (the scaled value) with `pan` (the raw value). When `handlePan` is not 128 they
differ, so the pan is sent again every frame. Keep this. The `& 0x7F` wraps values above 127: a handle pan above
128 can move a sound to the other side.

### 8.4 Envelope

`musCmdSetEnvelope` (801BF88C), operand bytes `speed, initial, attack, peak, decay, sustain, release`:

```
if (speed == 0) speed = 1;
envelopeSpeed = speed;  envelopeScale = 0x400 / speed;
envelopeInitial = initial;  attackSteps = attack;  envelopePeak = peak;
attackStep  = (f32)((1.0 / (f64)(f32)attack)  * (f64)(f32)(s32)(peak - initial));
decaySteps = decay;  envelopeSustain = sustain;
decayStep   = (f32)((1.0 / (f64)(f32)decay)   * (f64)(f32)(s32)(sustain - peak));
releaseSteps = release;
releaseStep = (f32)(1.0 / (f64)(f32)release);
return p + 7;
```

`musEnvelopeStart` (801C1D48), at each note unless `keepEnvelope`:

```
if (length != 0x7FFF)
    releaseTime = releaseAfterStart != 0 ? noteStart + (releaseAfterStart << 8)
                                         : noteEnd - (releaseBeforeEnd << 8);
else
    releaseTime = 0x7FFFFFFF;
envelopePhase = 1;  envelopeVolume = envelopeInitial;  envelopeCounter = envelopeSpeed;
```

`musEnvelopeUpdate` (801C1DB4), every call while the voice plays and `envelopePhase != 0`:

```
if (time >= releaseTime && envelopePhase < 4) {           /* unsigned compare */
    envelopePhase = 4; envelopeCounter = 1; releaseVolume = envelopeVolume;
}
if (--envelopeCounter != 0) return;                         /* u8 */
envelopeCounter = envelopeSpeed;
switch (envelopePhase) {
case 1: s = (((time - noteStart) >> 8) * envelopeScale) >> 10;            /* u32 */
        if ((s32)s < attackSteps) envelopeVolume = envelopeInitial + (s32)(attackStep * (f32)(s32)s);
        else { envelopePhase = 2; envelopeVolume = envelopePeak; }
        break;
case 2: s = ((((time - noteStart) >> 8) - attackSteps) * envelopeScale) >> 10;
        if ((s32)s < decaySteps) envelopeVolume = envelopePeak + (s32)(decayStep * (f32)(s32)s);
        else { envelopePhase = 3; envelopeVolume = envelopeSustain; }
        break;
case 3: break;
case 4: s = (((time - releaseTime) >> 8) * envelopeScale) >> 10;
        if ((s32)s < releaseSteps)
            envelopeVolume = releaseVolume - (s32)((releaseStep * (f32)(s32)s) * (f32)releaseVolume);
        else { envelopePhase = 5; envelopeVolume = 0; }
        break;
}
```

`envelopeVolume` is a `u8`: the sums are truncated to 8 bits when stored. The float to int conversions truncate
toward zero.

### 8.5 Pitch (musVoiceUpdatePitch, 801C1C1C)

Arguments: channel, voice index, `offset` (pitchOffset plus vibrato plus trill, summed in `f32` in that order).

```
f = notePitch;
if (portamentoTicks != 0) {
    if (!(portamentoTicks < noteTicks))
        f = portamentoFrom + ((notePitch - portamentoFrom) / (f32)portamentoTicks) * (f32)noteTicks;
    portamentoPitch = f;
}
f = f + (offset + bend);
if (f != lastPitch) {
    lastPitch = f;
    ratio = musPow2((f32)((f64)f * 0.08333333333333333));
    if ((f64)ratio > 2.0) { ratio = 2.0f; velocity = 0; }   /* the note goes silent */
    alSynSetPitch(voice, ratio);
}
```

`musPow2` (801C2314) approximates `2^x`. Products of `x` are single precision, sums are double, in this order:

```
if (x == 0.0f) return 1.0f;
if (x > 0.0f) a = x; else a = -x;            /* NaN takes the negative branch */
x2 = a * a;  x4 = x2 * x2;                    /* f32 */
r = (f64)a * 0.693147180559945 + 1.0;
r = r + (f64)x2 * 0.240226506959101;
r = r + (f64)(x2 * a) * 0.0555041086648216;
r = r + (f64)x4 * 0.00961812910762848;
r = r + (f64)(x4 * a) * 0.00133335581464284;
r = r + (f64)(x4 * x2) * 0.000154035303933816;
return x > 0.0f ? (f32)r : (f32)(1.0 / r);
```

Fine tune (command 0x8F): `pitchOffset -= fineTune; fineTune = (f32)((f64)(f32)(s8)v / 100.0);
pitchOffset += fineTune`. `MusHandleSetFreqOffset` sets `pitchOffset = fineTune + offset`.

### 8.6 Vibrato, trill and pan sweep

Vibrato (commands 0x88 and 0x89; `musVibratoUpdate`, 801C20D8):

```
/* command */ vibratoDelay = p[0]; vibratoPeriod = p[1];
              vibratoDepth = (f32)((f64)(f32)p[2] / 50.0);     /* 0x89: (f32)((f64)(-(f32)p[2]) / 50.0) */
              vibratoStep  = (f32)(6.2831852 / (f64)(f32)vibratoPeriod);
/* update  */ d = (s32)noteTicks - vibratoDelay;
              vibratoValue = d > 0 ? __sinf((f32)d * vibratoStep) * vibratoDepth : 0.0f;
              return vibratoValue;
```

Trill (command 0x97; `musTrillUpdate`, 801C2088), once per call, returns an `f32`:

```
if (--trillCounter == 0) {
    if (trillOffset == 0) { trillOffset = trillDepth; trillCounter = trillOnFrames; }
    else                  { trillOffset = 0;          trillCounter = trillOffFrames; }
}
return (f32)trillOffset;      /* signed byte */
```

Pan sweep (command 0xA9). `musPanSweepStart` (801C1FBC): `panSweepFraction = 0;
panSweepDown = pan & 0x40; panSweepTime = noteStart`. `musPanSweepUpdate` (801C1FD8):

```
do {
    panSweepTime += 0x100;
    v = panSweepFraction + panSweepSpeed;        /* int */
    if (v < 0x40) panSweepFraction = v;
    else {
        panSweepFraction = v & 0x3F;  step = v >> 6;
        if (!panSweepDown) { pan += step; if (pan >= 0x80) { pan = 0x7F; panSweepDown = 1; } }
        else               { pan -= step; if (pan >= 0x80 || pan == 0) { pan = 0; panSweepDown = 0; } }
    }                                            /* pan is a u8; the tests use the stored byte */
} while (panSweepTime < time);
```

### 8.7 Volume and bend streams (801C2144, 801C21FC)

```
do {
    volumeTime += 0x100;
    if (--volumeCount == 0) {                     /* u16 */
        b = *volumeStream++;
        if (b >= 0x80) {
            volume = b & 0x7F;
            c = *volumeStream++;
            if (c >= 0x80) volumeCount = ((c & 0x7F) << 8) + *volumeStream++ + 2;
            else           volumeCount = c + 2;
        } else { volume = b; volumeCount = 1; }
    }
} while (volumeTime < time);
```

The bend stream is the same with `bendTime`, `bendCount`, `bendStream`, and instead of `volume = v`:
`bendValue = (f32)((f64)(f32)v - 64.0); bend = bendValue * bendRange`.

### 8.8 Tempo command (0x85)

```
step = ((bpm * 0x6000) / 120) / musFrameRate;
if (fxNumber != 0) { tempoStep = step; }              /* effects: no scale, songTempoStep unchanged */
else for each channel c with c->song == ch->song {
    c->songTempoStep = step;
    c->tempoStep = (u16)(((s32)ch->tempoScale * step) >> 7);   /* the caller's tempoScale */
}
```

### 8.9 Loops (0x95, 0x96)

`musCmdLoopStart`, with `i = loopDepth`:

```
loopCount[i] = p[0]; loopStart[i] = p + 1;
loopVolumeStream[i] = volumeStream; loopBendStream[i] = bendStream;
loopVolume[i] = volume; loopBendValue[i] = (u8)musFloatToU32(bendValue);
loopVolumeCount[i] = volumeCount; loopBendCount[i] = bendCount;
loopDepth++;
return p + 1;
```

The float conversion uses the MIPS unsigned conversion: a negative `bendValue` of -1 or less is saved as 0xFF.
When the loop repeats, `bendValue` comes back as 255.0. Keep this (use `musFloatToU32`).

`musCmdLoopEnd`, with `i = (s32)loopDepth - 1`:

```
if (loopCount[i] != 0xFF) {                  /* 0xFF repeats forever */
    if (--loopCount[i] == 0) { loopDepth = i; i = -1; }
}
if (i < 0) return p;                         /* continue after the loop */
volumeStream = loopVolumeStream[i]; bendStream = loopBendStream[i];
volume = loopVolume[i]; bendValue = (f32)loopBendValue[i];
volumeCount = loopVolumeCount[i]; bendCount = loopBendCount[i];
bend = bendValue * bendRange;
return loopStart[i];
```

A count of `n` plays the body `n` times. A count of 0 wraps to 0xFF and repeats forever. With `loopDepth == 0`
the ROM reads and may write byte 0x11B (the last byte of `loopBendCount[3]`); well-formed data never does this.

### 8.10 Random numbers (musRandom, 801C2668)

Argument `range`. Runs 8 steps of a shift register, then scales:

```
for (k = 0; k < 8; k++) {
    taps = musRandomSeed & 0x48000000;
    musRandomSeed <<= 1;
    if (taps == 0x48000000 || taps == 0x08000000) musRandomSeed |= 1;   /* bit 27 only */
}
return (s32)((f32)range * (((f32)musRandomSeed / 65536.0f) / 65536.0f));   /* seed as s32 */
```

The result can be negative. Callers add a base and store the low byte.

## 9. Handles, priorities and channel allocation

### 9.1 Handles

`musNextHandle` starts at 1. Each song start and each effect start takes the next value and increments it. All
channels of one song share the handle. A handle of 0 means "none": every `MusHandle*` function returns 0 at once
for handle 0. When a channel ends (`musCmdEnd`) its handle becomes 0, so `MusHandleAsk` counts only live
channels.

### 9.2 Starting a song (MusStartSong, 801C0694)

```
n = numChannels from the header;
handle = musNextHandle++;
for (i = 0; i < n; i++) {
    data = data table entry i; if (data == 0) continue;
    ch = &musChannels[musSongChannelFind(song, i)];
    musChannelReset(ch);
    ch->song = song;  ch->flags |= MUS_CHANNEL_PAUSED;
    ch->volumeStreamStart = ch->volumeStream = volume table entry i (or NULL);
    ch->bendStreamStart = ch->bendStream = bend table entry i (or NULL);
    ch->handle = handle;
    ch->dataStart = ch->pdata = data;
}
MusHandleUnPause(handle);
return handle;
```

The channels start paused and the unpause goes through the message ring. So all tracks start at the same player
call. `musChannelReset` gives each channel `bank = musCurrentBank`; `MusBankStartSong` sets that to the music bank
during the call.

`musSongChannelFind(song, i)` (801C28A0) picks a channel:

1. The first free channel (`pdata == NULL`).
2. Else the effect channel (`fxNumber != 0`) with the lowest `priority`. On a tie the last such channel wins
   (`<=` test).
3. Else, if channel 0 is a song channel (`fxNumber == 0`) of another song, channel 0. The ROM's loop here never
   advances its channel pointer, so it only ever looks at channel 0 (it returns index 0 or nothing). Keep this.
4. Else the first channel with `song == song` and `dataStart` equal to track `i`'s stream (the same track of the
   same song, restarting).
5. Else `i % musNumChannels`.

### 9.3 Starting an effect

`MusStartEffect2(number, volume, pan, restartflag, priority)` (801C08E4):

1. If `priority == -1`, `priority = musFxPriorities[number]`.
2. If `restartflag != 0`: the first channel with `fxNumber == number` (playing or not) is restarted with
   `musEffectStart` and its new handle is returned. No priority test.
3. The first free channel gets the effect.
4. Else the effect channel with the lowest priority is found (strictly lower than the best so far, starting from
   `priority + 1`, so the first of equal ones wins). It is replaced only if its priority is strictly below the new
   priority.
5. Else return 0.

Song channels are never taken by effects. In Glover every effect but 0 has priority 100 and the game always
passes -1, so a new effect never replaces a playing one: when all channels are busy it does not play.

`MusStartEffect(number)` (801C0808) is the same as `MusStartEffect2(number, 0x80, 0x80, 0, -1)`, written out.

`musEffectStart(ch, number, volume, pan, priority)` (801C2B24): `musChannelReset(ch)`; `fxNumber = number`;
`handleVolume = volume`; `handlePan = pan`; `handle = musNextHandle++`; `priority = priority`;
`dataStart = pdata = musFxStreams[number]`; returns the handle. The effect starts unpaused, at the next player
call.

### 9.4 Linked effects (command 0xA7)

`musCmdStartEffect`: reads a varlen effect number. Raises the channel's own `priority` by 1 for the duration of
the call (so it cannot be taken), and calls
`MusStartEffect2(number, handleVolume, handlePan, 0, original priority)`. If that returns a handle, every channel
with that handle gets the caller's `handle` and `bank`. So the child effect belongs to the parent's handle and
stops, fades and changes volume with it.

### 9.5 Stopping and pausing

`MusHandleStop(handle, speed)` (801C0B70) and `MusStop(flags, speed)` (801C0A2C) act on each matching channel
whose `stopCount == -1` (`MusStop` also needs `pdata != NULL`):

- If the channel is still paused (its song has not started): `stopLength = 1; stopCount = 0;` clear the paused
  flag. It stops at the next call.
- Else `stopLength = speed ? speed : 1; stopCount = speed`.

Each call then lowers `stopCount` by one and scales the volume by `stopCount / stopLength`, a linear fade over
`speed` frames. When `stopCount` reaches -1 the channel ends and its voice stops. With speed 0 the channel ends at
the next call. `MusStop` matches channels by `flags`: bit 0 effects, bit 1 songs. `MusHandleStop` returns the
number of channels it changed; `MusStop` returns nothing.

`MusHandlePause` and `MusHandleUnPause` post messages (section 5.7). A paused channel is skipped entirely: its
clock stops and its voice keeps its last pitch and volume.

### 9.6 Voices and priorities in the synthesizer

`MusInitialize` allocates one voice per channel with `alSynAllocVoice`, all with priority `thread_priority`
(0x8C) and fx bus 0. Voices are never freed or reallocated. libmus never calls `alSynFreeVoice`,
`alSynSetPriority` or `alSynStartVoiceParams`. The only voice calls are `alSynStartVoice`, `alSynStopVoice`,
`alSynSetVol`, `alSynSetPitch`, `alSynSetPan` and `alSynSetFXMix`.

## 10. Reverb set up

libmus replaces libaudio's `alSynNew` and `alFxNew` with `CustomSynNew` and `CustomFxNew`. The difference is that
the delay line and section buffers are sized for the largest preset. The preset can then change while the game
runs (`ChangeCustomEffect`).

**Presets.** Each preset is an `s32` array in libaudio's custom effect format: section count, delay line length
in samples, then per section 8 words: input tap, output tap, feedback coefficient, feed-forward coefficient, gain,
chorus rate, chorus depth, low-pass coefficient. `musCustomFxPresets` (801F4384) lists them, NULL terminated;
`musCustomFxCount` (801F4380) is 6. Index `n` matches libaudio's `AL_FX_*` number `n`: presets 1-5 hold exactly
the values that the ROM's `alFxNew` uses for `AL_FX_SMALLROOM` to `AL_FX_ECHO`.

| Index | Name | ROM | Count, length | Sections (input, output, fbcoef, ffcoef, gain, rate, depth, lowpass) |
| --- | --- | --- | --- | --- |
| 0 | musFxNone | 801F4358 | 1, 800 | (0, 200, 0, 0, 0, 0, 0, 0) |
| 1 | musFxSmallRoom | 801F4190 | 3, 4000 | (0, 2160, 9830, -9830, 0, 0, 0, 0), (760, 1520, 3276, -3276, 16383, 0, 0, 0), (0, 2400, 5000, 0, 0, 0, 0, 20480) |
| 2 | musFxBigRoom | 801F41F8 | 4, 4000 | (0, 2640, 9830, -9830, 0, 0, 0, 0), (880, 2160, 3276, -3276, 16383, 0, 0, 0), (2640, 3640, 3276, -3276, 16383, 0, 0, 0), (0, 3760, 8000, 0, 0, 0, 0, 20480) |
| 3 | musFxChorus | 801F42A8 | 1, 800 | (0, 200, 16384, 0, 32767, 7600, 700, 0), then 24 zero words |
| 4 | musFxFlange | 801F4330 | 1, 800 | (0, 200, 0, 24575, 32767, 380, 500, 0) |
| 5 | musFxEcho | 801F4280 | 1, 8000 | (0, 7160, 12000, 0, 32767, 0, 0, 0) |

Array sizes in words: 10, 26, 34, 34, 10, 10 (the chorus array has 24 trailing zeros).

The port reads the ROM's copies of the six presets in `port_ram`, which the loader swaps to host order:
`mus_internal.h` defines `musFxNone` to `musFxEcho` as `D_801F4358` and the others. `ALSynConfig.params` holds a
pointer to one of them, and a pointer in the sound player's memory must point into `port_ram` or `port_win`
([port64.md](port64.md)). `musCustomFxPresets` and `musCustomFxCount` are host globals in mus-reverb.

**Order of set up.** `MusInitialize` builds an `ALSynConfig` with `fxType` 2 and sets `musFxType = 2`.
`amCreateAudioMgr` calls `CustomInit(&musGlobals, config)`:

- `CustomInit` (801C3710): if `alGlobals == NULL`: `alGlobals = g`; `CustomSetDefaultFx(c)` (fxType = 6,
  params = `musFxBigRoom`); `CustomSynNew(&g->drvr, c)`.
- `CustomSynNew` (801C3758) builds the synthesizer like libaudio's `alSynNew` but with one aux bus:
  `head = 0, curSamples = 0, paramSamples = 0, numPVoices = maxPVoices, maxOutSamples = 160,
  outputRate = c->outputRate, dma = c->dmaproc`. Allocates and creates a save filter (`outputFilter`), one
  `ALAuxBus` (0x4C, `maxAuxBusses = 1`) with `maxPVoices` source slots, and an `ALMainBus` (0x20) with
  `maxPVoices` slots. If `c->fxType != 0`: `CustomAllocFX(s, 0, c, heap)`; else the aux bus is added to the main
  bus directly. Clears the three voice lists. Allocates `maxPVoices` `PVoice` (0xDC each); for each: links it into
  the free list, `vvoice = NULL`, creates the decoder (`alLoadNew` with `dma`, then `alLoadParam(…, 1, 0)`), the
  resampler (source = decoder), the envelope mixer (source = resampler), adds the mixer to the aux bus, and sets
  `channelKnob = &envmixer`. Then `alSaveParam(save, 1, mainBus)`. Allocates `maxUpdates` `ALParam` (0x1C) and
  pushes each on the free list `paramList` (so the last one allocated is first). `heap = heap`.
- `CustomAllocFX(s, bus, c, heap)` (801C3A58): `musReverb.synth = s`; `fx = &s->auxBus[bus].fx[0]`;
  `CustomFxNew(fx, c, heap)`; `alFxParam(fx, 1, &s->auxBus[bus])` (set source); `alMainBusParam(mainBus, 2, fx)`
  (add source). Returns `fx`.
- `CustomFxNew(fx, c, heap)` (801C3AFC): `alFilterNew(fx, 0, alFxParam, 5)`; `fx->filter.handler = alFxPull`;
  `fx->paramHandler = alFxParamHdl`. Allocates into `musReverb`: `delays` = `CustomMaxSections()` x 0x28,
  `delayLine` = `CustomMaxLength()` x 2 bytes, one `ALResampler` (0x34), one `RESAMPLE_STATE` (0x20), one
  `ALLowPass` (0x30), one `POLEF_STATE` (8). Then `CustomFxSet(c->params)`.
- `CustomMaxSections` (801C3F30) and `CustomMaxLength` (801C3F78) walk `musCustomFxPresets` to the NULL and
  return the largest word 0 and word 1 (4 and 8000 for Glover).

**CustomFxSet(params)** (801C3C34), also used for every preset change:

```
fx = &musReverb.synth->auxBus->fx[0];
fx->sectionCount = (u8)params[0];
fx->length = params[1];
fx->sections = musReverb.delays;
fx->delayLine = fx->inputPos = musReverb.delayLine;
for (j = 0; j < fx->length; j++) fx->delayLine[j] = 0;      /* j is u16 */
k = 2;
for (i = 0; i < fx->sectionCount; i++) {                     /* i is u16 */
    d = &fx->sections[i];
    d->input = params[k++]; d->output = params[k++];
    d->fbcoef = params[k++]; d->ffcoef = params[k++]; d->gain = params[k++];
    rate = params[k++];
    if (rate != 0) {
        depth = params[k++];
        d->chorusStep  = (f32)((2.0 * (f64)((f32)rate / 1000.0f)) / (f64)musReverb.synth->outputRate);
        d->chorusValue = 1.0f;  d->chorusDelta = 0;
        d->chorusDepth = (f32)(((f64)(f32)depth / 173123.404906676) * (f64)(u32)(d->output - d->input));
        d->resampler = musReverb.resampler;
        d->resampler->state = musReverb.resampleState;
        d->resampler->delta = 0.0f;  d->resampler->first = 1;
    } else { d->resampler = NULL; k++; }
    lp = params[k++];
    if (lp != 0) {
        d->lowpass = musReverb.lowpass;
        d->lowpass->fstate = musReverb.lowpassState;
        d->lowpass->fc = (s16)lp;
        init_lpfilter(d->lowpass);
    } else d->lowpass = NULL;
}
```

All sections with chorus share one resampler, and all sections with a low-pass share one filter. No Glover
preset has more than one of each. The delay line is cleared at every preset change.

**ChangeCustomEffect(type)** (801C3FC0): returns 1 if `type >= musCustomFxCount`. Else
`CustomFxSet(musCustomFxPresets[type])`, `alFxParam(fx, 1, auxBus)` and return 0. It is called from the message
ring (`MusSetFxType`) and from command 0xAA when `musSongFxChange == 1`. Glover never calls `MusSetFxType` or
`MusSetSongFxChange`, so its reverb stays on the big room preset. The game sets each sound's reverb amount with
`MusHandleSetReverb`.

`MusSetSongFxChange(onoff)`: if `onoff == 0`, posts `MusSetFxType(musFxType)` (back to the default preset) and
uses its result; else the result is 1. If the result is nonzero, `musSongFxChange = onoff`. Returns the result.

## 11. Function reference

All 109 functions: 43 commands, 23 API functions, 27 internal functions, 9 reverb functions and 7 glue
functions. "Callers" lists direct calls; `musCommands` and the synthesizer call some functions through pointers.
Unless stated otherwise, a `Mus*` function that takes a handle returns the number of channels it changed, and
returns 0 for handle 0.

### 11.1 Public API

| ROM | Name | Arguments | Returns | Role | Callers | Callees |
| --- | --- | --- | --- | --- | --- | --- |
| 801C0300 | MusInitialize | MusConfig * | heap bytes used | set up, see below | game `InitMusicDriver` | musMemSet, alHeapInit, alHeapDBAlloc, musFifoInit, musBankInit, amCreateAudioMgr, MusSetMasterVolume, alSynAddPlayer, musChannelReset, alSynAllocVoice |
| 801C066C | MusSetMasterVolume | s32 flags, s32 volume | none | bit 0: `musFxMasterVolume = volume`; bit 1: `musSongMasterVolume = volume` (stored as u16) | MusInitialize, game | none |
| 801C0694 | MusStartSong | u8 *song | handle | section 9.2 | MusBankStartSong | musSongChannelFind, musChannelReset, MusHandleUnPause (ROM: also the relocation helper) |
| 801C0808 | MusStartEffect | s32 number | handle or 0 | section 9.3 | MusBankStartEffect | musEffectStart |
| 801C08E4 | MusStartEffect2 | number, volume, pan, restartflag, priority | handle or 0 | section 9.3 | musCmdStartEffect, MusBankStartEffect2, game | musEffectStart |
| 801C0A2C | MusStop | s32 flags, s32 speed | none | section 9.5 | game | none |
| 801C0B00 | MusAsk | s32 flags | count | channels with `pdata != NULL` matching flags (bit 0 effects, bit 1 songs) | game | none |
| 801C0B70 | MusHandleStop | handle, speed | count | section 9.5 | game | none |
| 801C0C1C | MusHandleAsk | handle | count | channels with this handle | game | none |
| 801C0C70 | MusHandleSetVolume | handle, s32 volume | count | `handleVolume = (s16)volume` | game | none |
| 801C0CD4 | MusHandleSetPan | handle, s32 pan | count | `handlePan = (s16)pan; lastPan = 0xFF` | game | none |
| 801C0D40 | MusHandleSetFreqOffset | handle, f32 offset | count | `pitchOffset = fineTune + offset` (semitones) | game | none |
| 801C0DAC | MusHandleSetTempo | handle, s32 tempo | count | clamp to 1..256; `tempoScale = tempo; tempoStep = (songTempoStep * tempo) >> 7` | game | none |
| 801C0E3C | MusHandleSetReverb | handle, s32 reverb | count | clamp to 0..127; `handleReverb = reverb; lastReverb = 0xFF` (applies at the next note) | game | none |
| 801C0EC0 | MusBankInitialize | u8 *pbank, u8 *wbank | none | `musBankInit(pbank, wbank)` | game | musBankInit |
| 801C0EE0 | MusBankStartSong | u8 *pbank, u8 *song | handle | if pbank != NULL and initialised, `musCurrentBank = pbank`; `MusStartSong(song)`; then `musCurrentBank = musDefaultBank` | game | MusStartSong |
| 801C0F28 | MusBankStartEffect | u8 *pbank, s32 number | handle | same pattern with MusStartEffect | none | MusStartEffect |
| 801C0F70 | MusBankStartEffect2 | pbank, number, volume, pan, restartflag, priority | handle | same pattern with MusStartEffect2 | none | MusStartEffect2 |
| 801C0FD8 | MusHandleGetPtrAddr | handle | MusBank * | `bank` of the first channel with the handle, else NULL | none | none |
| 801C102C | MusHandlePause | handle | 1, or 0 if the ring is full | posts `MUS_MSG_PAUSE` | none | musFifoPost |
| 801C1054 | MusHandleUnPause | handle | same | posts `MUS_MSG_UNPAUSE` | MusStartSong | musFifoPost |
| 801C1080 | MusSetFxType | s32 type | same | `musFxType = type`; posts `MUS_MSG_FX_TYPE` | MusSetSongFxChange | musFifoPost |
| 801C10B4 | MusSetSongFxChange | s32 onoff | result | section 10 | none | MusSetFxType |

`MusInitialize` in order:

1. `musControlFlag = control_flag; musNumChannels = channels; musFxStreams = default_fxbank;
   musFxPriorities = priority`.
2. `musFrameRate = osTvType == 0 ? 50 : 60; musFrameMicros = 1000000 / musFrameRate`.
3. `musMemSet(heap, 0, heap_length); alHeapInit(&musHeap, heap, heap_length)`.
4. `musVoices = alHeapDBAlloc(0, 0, &musHeap, 1, channels * 0x1C)`;
   `musChannels = alHeapDBAlloc(0, 0, &musHeap, 1, channels * 0x134)`; `musFifoInit(fifo_length)`; zero both
   arrays.
5. If `ptr != NULL`: `musBankInit(ptr, wbk); musCurrentBank = musDefaultBank = ptr`. Else `musDefaultBank = NULL`
   (`musCurrentBank` stays as it was).
6. Build `ALSynConfig` and `MusAudioMgrConfig` (section 6.3); `musFxType = 2`;
   `amCreateAudioMgr(sched, &synConfig, thread_priority, &amConfig, syn_num_dma_bufs, syn_dma_buf_size,
   musFrameRate)`.
7. `MusSetMasterVolume(3, 0x7FFF); musNextHandle = 1; musRandomSeed = 0x12345678`.
8. `musPlayer.next = NULL; musPlayer.handler = musPlayerHandler; musPlayer.clientData = &musPlayer;
   alSynAddPlayer(&musGlobals.drvr, &musPlayer)`.
9. For each channel: `voiceOn = 0; musChannelReset(ch)`; `alSynAllocVoice(&musGlobals.drvr, &musVoices[i], &vc)`
   with `vc = { (s16)thread_priority, 0, 0 }`.
10. Return `musHeap.cur - musHeap.base`. The game stores it and never reads it.

### 11.2 Sequencer commands

The 43 command handlers are listed in [section 5.2](#52-command-streams) with their ROM addresses and effects.
Each takes `(MusChannel *ch, u8 *p)` and returns the next stream byte. Only the player calls them, through
`musCommands`. Besides that, `musCmdSetEnvelope` is called directly by `musCmdEnvelopeIndex` and by
`musChannelNextEvent` (drum maps); `musCmdEnd` by `musPlayerHandler` (with `p = NULL`). Calls they make:
`musCmdRandomTranspose`, `musCmdRandomVolume`, `musCmdRandomPan` call `musRandom`; `musCmdStartEffect` calls
`MusStartEffect2`; `musCmdChangeEffect` calls `ChangeCustomEffect`.

### 11.3 Internal functions

| ROM | Name | Arguments | Returns | Role | Callers | Callees |
| --- | --- | --- | --- | --- | --- | --- |
| 801C1100 | musFifoInit | s32 length | none | clamp to 64..1024; `musFifo = alHeapDBAlloc(0, 0, &musHeap, 1, length * 8)`; size = length; read = write = 0 | MusInitialize | alHeapDBAlloc |
| 801C1184 | musFifoProcess | none | none | while read != write: execute `musFifo[read]`, advance read (wrap to 0 at size) | musPlayerHandler | musFifoExecute |
| 801C1228 | musFifoExecute | MusMessage * | none | section 5.7 | musFifoProcess | musHandleSetFlags, ChangeCustomEffect |
| 801C12A0 | musFifoPost | MusMessage * | 1, or 0 if full | `next = (write + 1) % size`; full when `next == read`; copy 8 bytes to `musFifo[write]`; `write = next` | MusHandlePause, MusHandleUnPause, MusSetFxType | musMemCopy |
| 801C1338 | musPlayerHandler | void *player | `musFrameMicros` | section 8.1 | synthesizer (ALPlayer) | musFifoProcess, musVoiceStart, musChannelNextEvent, musVolumeStreamUpdate, musBendStreamUpdate, musCmdEnd, alSynStopVoice, musEnvelopeUpdate, musPanSweepUpdate, musVibratoUpdate, musTrillUpdate, musVoiceUpdatePitch, musVoiceUpdateVolumePan |
| 801C15B8 | musChannelNextEvent | ch, s32 index | none | section 8.2 | musPlayerHandler | commands, musCmdSetEnvelope, musEnvelopeStart, musPanSweepStart, alSynSetVol, musVoiceStart, alSynSetFXMix, alSynStopVoice |
| 801C19FC | musVoiceStart | ch, s32 index | none | section 8.2 | musPlayerHandler, musChannelNextEvent | alSynStopVoice, alSynStartVoice |
| 801C1A90 | musVoiceUpdateVolumePan | ch, s32 index | none | section 8.3 | musPlayerHandler | alSynSetVol, alSynSetPan |
| 801C1C1C | musVoiceUpdatePitch | ch, s32 index, f32 offset | none | section 8.5 | musPlayerHandler | musPow2, alSynSetPitch |
| 801C1D48 | musEnvelopeStart | ch | none | section 8.4 | musChannelNextEvent | none |
| 801C1DB4 | musEnvelopeUpdate | ch | none | section 8.4 | musPlayerHandler | none |
| 801C1FBC | musPanSweepStart | ch | none | section 8.6 | musChannelNextEvent | none |
| 801C1FD8 | musPanSweepUpdate | ch | none | section 8.6 | musPlayerHandler | none |
| 801C2088 | musTrillUpdate | ch | f32 | section 8.6 | musPlayerHandler | none |
| 801C20D8 | musVibratoUpdate | ch | f32 | section 8.6 | musPlayerHandler | __sinf |
| 801C2144 | musVolumeStreamUpdate | ch | none | section 8.7 | musPlayerHandler | none |
| 801C21FC | musBendStreamUpdate | ch | none | section 8.7 | musPlayerHandler | none |
| 801C2314 | musPow2 | f32 x | f32 | section 8.5 | musVoiceUpdatePitch | none |
| 801C249C | musBankInit | MusBank *, u8 *wbank | none | section 5.5 (ROM: relocation, pitch floats, then `osWritebackDCacheAll`) | MusInitialize, MusBankInitialize | (ROM: relocation helper, osWritebackDCacheAll) |
| 801C2668 | musRandom | s32 range | s32 | section 8.10 | musCmdRandomTranspose, musCmdRandomVolume, musCmdRandomPan | none |
| 801C2768 | musChannelReset | ch | none | `pdata = NULL`; zero all 0x134 bytes but keep `voiceOn`; set the reset values of section 6.2; `bank = musCurrentBank` | MusInitialize, MusStartSong, musEffectStart | none |
| 801C28A0 | musSongChannelFind | u8 *song, s32 track | channel index | section 9.2 | MusStartSong | none |
| 801C2A18 | musMemSet | dst, s32 value, s32 count | none | byte fill | MusInitialize | none |
| 801C2A4C | musMemCopy | src, dst, s32 count | none | byte copy, source first | musFifoPost | none |
| 801C2A84 | (relocate offsets) | u32 *array, base, s32 count | none | adds `base` to each nonzero word. The port replaces it with big-endian reads (songs) and `musBankInit` (banks). | MusStartSong, musBankInit | none |
| 801C2B24 | musEffectStart | ch, number, volume, pan, priority | handle | section 9.3 | MusStartEffect, MusStartEffect2 | musChannelReset |
| 801C2BB0 | musHandleSetFlags | handle, u32 keep, u32 set | none | for each channel with the handle: `flags = (flags & keep) | set` | musFifoExecute | none |

### 11.4 Synthesizer set up and reverb

| ROM | Name | Arguments | Returns | Callers | Callees |
| --- | --- | --- | --- | --- | --- |
| 801C3710 | CustomInit | ALGlobals *, ALSynConfig * | none | amCreateAudioMgr | CustomSetDefaultFx, CustomSynNew |
| 801C3758 | CustomSynNew | ALSynth *, ALSynConfig * | none | CustomInit | alHeapDBAlloc, alSaveNew, alAuxBusNew, alMainBusNew, CustomAllocFX, alMainBusParam, alLink, alLoadNew, alLoadParam, alResampleNew, alResampleParam, alEnvmixerNew, alEnvmixerParam, alAuxBusParam, alSaveParam |
| 801C3A58 | CustomAllocFX | ALSynth *, s16 bus, ALSynConfig *, ALHeap * | ALFx * | CustomSynNew | CustomFxNew, alFxParam, alMainBusParam |
| 801C3AFC | CustomFxNew | ALFx *, ALSynConfig *, ALHeap * | none | CustomAllocFX | alFilterNew, CustomMaxSections, alHeapDBAlloc, CustomMaxLength, CustomFxSet |
| 801C3C34 | CustomFxSet | s32 *params | none | CustomFxNew, ChangeCustomEffect | init_lpfilter |
| 801C3F18 | CustomSetDefaultFx | ALSynConfig * | none | CustomInit | none |
| 801C3F30 | CustomMaxSections | none | s32 | CustomFxNew | none |
| 801C3F78 | CustomMaxLength | none | s32 | CustomFxNew | none |
| 801C3FC0 | ChangeCustomEffect | s32 type | 0, or 1 for a bad index | musFifoExecute, musCmdChangeEffect | CustomFxSet, alFxParam |

The behaviour is in section 10.

### 11.5 Platform glue

| ROM | Name | Role |
| --- | --- | --- |
| 801C2C10 | amCreateAudioMgr | section 12 |
| 801C3040 | amAudioThread | section 12 |
| 801C3198 | amHandleFrame | section 12 |
| 801C3364 | amHandleTaskDone | section 12 |
| 801C33A4 | amDmaCallback | section 12 |
| 801C35B8 | amDmaNew | section 12 |
| 801C35F0 | amClearDma | section 12 |

### 11.6 Names and evidence

- `MusInitialize` to `MusSetSongFxChange`, `amCreateAudioMgr`, `CustomInit` to `CustomFxSet`,
  `ChangeCustomEffect`, `__am`, `lastInfo`: ROM symbol names, kept.
- `musControlFlag` and `musReverb` are the ROM's `__muscontrol_flag` and `gReverbFx`. `gen/ram.S` already defines
  the ROM names as symbols in `port_ram`. The native globals therefore have other names.
- Command handlers: named from what each one writes (section 5.2) and from the data (operand sizes checked on all
  songs and effects).
- `musPlayerHandler`: stored in `ALPlayer.handler` and passed to `alSynAddPlayer`; returns the call interval.
- `musChannelNextEvent`: dispatches commands until a note byte, then starts the note.
- `musVoice*`: the only callers of `alSynSetVol/SetPan/SetPitch/StartVoice` with the channel's voice.
- `musEnvelope*`: phases 1-5 with initial, peak and sustain levels from command 0x84; volume feeds the voice
  volume.
- `musPow2`: the coefficients are `(ln 2)^k / k!`, the series of `2^x`; the caller divides semitones by 12.
- `musRandom`: shift register with a fixed seed, scaled to its argument; used only by the "random" commands.
- `musFifo*`: an 8-byte message ring with read and write indices, filled by the API and drained by the player.
- `musBankInit`: relocates a "N64 PtrTablesV2" file; `musSongChannelFind`, `musEffectStart`: their only callers
  are `MusStartSong` and `MusStartEffect*`.
- `CustomSetDefaultFx`, `CustomMaxSections`, `CustomMaxLength`: named from what they compute; the `Custom` prefix
  follows their callers.
- `musFxNone` to `musFxEcho`: preset index equals the `AL_FX_*` number, and presets 1-5 equal the ROM's `alFxNew`
  presets for the same number.
- `am*` glue names describe what each function does; they are only used in this document and the header comment.

## 12. Platform glue and its native replacement

### 12.1 What the ROM's glue does

- **amCreateAudioMgr(sched, synConfig, threadPriority, amConfig, numDmaBuffers, dmaBufferSize, frameRate)**
  (801C2C10): opens the cartridge and 64DD PI handles; sets `synConfig->dmaproc = amDmaNew` and
  `synConfig->outputRate = osAiSetFrequency(amConfig->outputRate)`; allocates DMA buffers, I/O messages and
  queues from the libmus heap; computes the frame size `ceil(framesPerField * outputRate / frameRate)` rounded up
  to 16 samples (1104 for Glover), its minimum (size - 16) and maximum (extra + size + 16); calls
  `CustomInit(&musGlobals, synConfig)`; allocates two command lists of `maxACMDSize` commands and four
  `AudioInfo` (0x68 bytes: output buffer, sample count, an `NNScTask`, and a message of type 2 that points back to
  the info); creates the queues; creates and starts the audio thread (`osCreateThread` id 3, stack top 802AD318).
- **amAudioThread** (801C3040): registers with the NN scheduler (`nnScAddClient`). Loops: waits for a message,
  drops one extra pending message. On a retrace message (type 1) it calls `amHandleFrame(info[frame & 3],
  lastInfo)`; if that sent a task, it waits for the "task done" reply and records `lastInfo`. Type 3 (pre-NMI)
  does nothing. Type 10 ends the loop and calls `alClose`.
- **amHandleFrame(info, last)** (801C3198): `amClearDma()`; queues the previous buffer to the audio interface;
  computes the sample count `((frameSize - osAiGetLength() / 4) + extra + 16) & 0xFFF0` (at least the minimum);
  calls `alAudioFrame`; builds an RSP audio task (rspboot, aspMain, the command list) and sends it to the
  scheduler; flips the command list. Returns 0 if `alAudioFrame` produced no commands.
- **amHandleTaskDone** (801C3364): tests `osAiGetLength` and a flag, then writes 0 to a flag that is already 0. No
  effect.
- **amDmaNew(state)** (801C35B8): initialises the DMA state once and returns `amDmaCallback`.
- **amDmaCallback(addr, len, state)** (801C33A4): a 64DD address (top byte 0xFF) becomes
  `(addr & 0xFFFFFF) + 0x140000` on the drive handle. If `__muscontrol_flag & 1`, returns
  `osVirtualToPhysical(addr)` (wave data already in RAM). Else looks for a used DMA buffer that already holds
  `[addr, addr + len]` and returns a pointer into it; else takes a free buffer, starts a DMA of a whole buffer
  from the even address at or below `addr`, and returns a pointer into it. If no buffer is free it returns the
  address of the list head (a bug, never hit in practice).
- **amClearDma** (801C35F0): drains the DMA queue, returns buffers unused for more than one frame to the free
  list, and advances the frame counter.

### 12.2 Native model for the port

| ROM piece | Port replacement |
| --- | --- |
| audio thread, scheduler client, RSP task, AI buffers | the host audio callback calls `musAudioRender(out, frames)`. It locks, runs whole frames of the native synthesizer and keeps the samples left over for the next call. The synthesizer calls `musPlayerHandler` through `musPlayer` at the times of section 4. |
| `amCreateAudioMgr` | a native function with the same signature: `c->dmaproc = amDmaNew; c->outputRate = musAiFrequency(amc->outputRate)` (22047); the ROM's blocks from the libmus heap, in the ROM's order and sizes; the ROM's frame size (1104); `CustomInit(&musGlobals, c)`; then `port_audio_open` opens the host's stream at 22047 Hz. It ignores `sched` and the thread priority. `MusInitialize` still uses the thread priority for the voices. |
| `amHandleFrame` | `musRenderFrame`: `musDmaClear`, then `alAudioFrame` for 1104 samples. The native `alAudioFrame` also runs the command list, so the samples are ready when it returns. |
| `amDmaNew`, `amDmaCallback` and `amClearDma` | `amDmaNew`, `musDmaRead` and `musDmaClear` keep the ROM's DMA buffers: the records and the data are in the libmus heap, and a "DMA" copies a whole buffer from `port_rom`. With `musControlFlag & 1` the routine returns `addr` unchanged. A 64DD address (top byte 0xFF) does not occur in Glover. The port logs the first one and reads zeros for it. |
| `osAiSetFrequency` | `musAiFrequency(r) = 48681812 / (s32)(48681812.0f / (f32)r + 0.5f)` (NTSC video clock). |
| cache writeback in `musBankInit` | nothing |
| `alClose` on thread exit | nothing (or close the stream at shutdown) |

The native synthesizer calls the DMA routine for the bytes it needs, as libaudio's load filter does. The routine
returns a host pointer (an `s32` on the 32-bit host).

**Locking.** On the N64 the game thread calls the API while the higher-priority audio thread can interrupt it at
any instruction; there is no lock. On the host the audio callback runs on another thread. The port needs a
lock. Use one recursive mutex (`musLock`, `musUnlock`):

- `musAudioRender` holds it while the synthesizer runs (and so while `musPlayerHandler` runs).
- Every public `Mus*` function takes it for its whole body. It must be recursive: `musCmdStartEffect` calls
  `MusStartEffect2` from inside the player, and some API functions call others.

**Busy waits in the game.** `PrepareSong`, `FreeScreen` and `FreeLevel` wait in loops such as
`while (MusHandleAsk(audioCtrl.handle)) {}` until the player has stopped a song. Stopping takes two player calls.
These loops end only if the player runs while the game thread spins. `MusAsk` and `MusHandleAsk` call
`musAudioPoll()` before they count. With an audio thread it yields the processor. Without one (the headless mode,
or no audio device) it runs one frame of the synthesizer itself, which calls the player three or four times.

### 12.3 What the port's loader and build must provide

The port keeps the game's data at its N64 offsets in `port_ram` and converts it when it loads the main segment
(`docs/architecture.md`). libmus depends on three things there:

1. **Effect streams stay bytes.** `D_801E7760` to the end of `D_801E9C9C` (VRAM 801E7760-801E9CB4) are byte
   streams. The generator currently treats them as 32-bit words: it swaps them and turns some of them into
   pointers, which corrupts the streams. Declare them as one open byte array in `tools/gen/extra_types.c`
   (`extern u8 D_801E7760[];`) and regenerate.
2. **The effect list holds host pointers.** Type `libmus_sfx_list` as `u8 *libmus_sfx_list[420]` (its words then
   become `port_ram` pointers to the streams).
3. **The priority list holds host integers.** Type `libmus_priority_list` as `s32 libmus_priority_list[420]`.

`src/game/dma.c` declares both lists as `u8[]`; change those declarations to the types above, because the
generator takes the types from the game's C. Check after loading: `musFxStreams[0]` points at
`port_ram + 0x1E7760`, `musFxStreams[0][0]` is 0x80, `musFxStreams[1][0]` is 0x81, and `musFxPriorities[1]` is
100.

Other data needs nothing: the game loads the pbanks and songs at run time with `DMAMemory` (raw ROM bytes), and
the wave data stays in `port_rom`.

**Build.** The Makefile compiles the audio sources as C++ with C linkage, with the game's flags (`GAME_CXXFLAGS`):
`-x c++ -m32 -msse2 -mfpmath=sse -ffp-contract=off -fwrapv -funsigned-char` and `-include src/port/port_pre.h`
([port64.md](port64.md)). `port_pre.h` maps `__sinf` to `port_sinf`, the bit-exact libultra sine in
`src/port/libultra.c`. Each file starts with `PORT_C_BEGIN` and ends with `PORT_C_END`. Write code that compiles as
C++ and as gnu89 C (no declarations in a `for` statement, a cast on each `void *` that is stored in another pointer
type). `tools/mustest` also compiles every file as C.

**Tests.** The regression test (`tools/regress.py`) hashes all of `port_ram`. Its baseline was made with the
stand-in in `src/port/audio_stub.c`, which writes nothing, and the test still uses the stand-in. The real libmus
writes to `audio_memory` (`port_ram` 0x2DC90-0x77070: channels, voices and the synthesizer) and to `alGlobals`. In the
headless mode it runs on the game's clock (`musAudioAdvance` and `musAudioPoll`), so two runs give the same memory.

## 13. Callers in the game

| Function | Callers (port `src/game/`) |
| --- | --- |
| MusInitialize | `InitMusicDriver` (dma.c) |
| MusBankInitialize | `InitMusicDriver`: pbank1 with wbank1 |
| MusBankStartSong | `PrepareSong` (sound.c), always with pbank1 |
| MusStartEffect2 | `PlaySample`, `PlaySampleRadius`, `AddAmbientSfx`, `UpdateContinuousSample`, `ProcessSFXScript` (sound.c), `MainOptionsWait` (options.c); always `restartflag` 0 and priority -1 |
| MusHandleStop | `PrepareSong`, `ClearAmbientSfx`, `StopContinuousSample`, `UpdateContinuousSample`, `ProcessSFXScript` (sound.c), `nnScEventHandler` (sched.c), `CheckForCheats` (cheat.c) |
| MusHandleAsk | `PrepareSong` |
| MusHandleSetVolume | sound.c (several), game.c, `CheckCameraForWater` (water.c) |
| MusHandleSetPan | `UpdateContinuousSample` |
| MusHandleSetFreqOffset | sound.c (several), `MaintainBulletBomb2` (bullet.c); offset is `(pitch - 128) / 10` |
| MusHandleSetTempo | sound.c (several); for effects the game passes the same pitch byte; `PrepareSong` passes 106 for song 26 on PAL only |
| MusHandleSetReverb | sound.c (30, 40 or 60 for effects; 0 for music), `CheckCameraForWater` (60 under water) |
| MusSetMasterVolume | `RunFadeN` (frontend.c), `FreeLevel2`, `LevelSelectScreen`, `LoadLevel`, `SelectLanguage`, `UpdatePause` (game.c), `CheckJalloc` (jalloc.c); flag 1 effects, flag 2 music |
| MusStop, MusAsk | `FreeScreen`, `FreeLevel` (game.c), with busy waits |

Not used by the game: `MusStartSong` (internal), `MusStartEffect`, `MusBankStartEffect`, `MusBankStartEffect2`,
`MusHandleGetPtrAddr`, `MusHandlePause`, `MusHandleUnPause` (internal), `MusSetFxType`, `MusSetSongFxChange`.

The game sets the music volume with `MusHandleSetVolume(handle, 80)` and effect volumes with the `volume`
argument of `MusStartEffect2` (0..255). Both scale `handleVolume`, where 128 is unity.

## 14. Rules for changing this code

These rules hold for every change to `src/audio/mus/`.

1. **Floating point must match the ROM bit for bit.** Build the audio code with `-msse2 -mfpmath=sse
   -ffp-contract=off` and without `-ffast-math` (the Makefile's `GAME_CXXFLAGS` has these). With these flags `f32`
   and `f64` operations round like the MIPS FPU. Keep every `f32`/`f64` conversion that this map shows (they are
   in the ROM). The ROM runs with flush-to-zero on
   (`FPCSR` 0x01000800, set by `osInitialize`); the FTZ bit of MXCSR must be set on the audio thread to match edge
   cases.
   The same `FPCSR` value enables the invalid-operation trap, so data that would produce a NaN crashes the N64
   (for example an envelope with attack 0 and peak equal to initial). Glover's data never does this.
2. **Integer widths.** Use the field types of the header. Reproduce `u8` and `u16` truncation where the ROM
   stores to a byte or halfword (envelope volume, pan, counters). Products noted as unsigned (`multu`) in section
   8 are 32-bit unsigned.
3. **Float to integer.** `(s32)f` truncation matches `trunc.w.s`. The one unsigned conversion (command 0x95) uses
   `musFloatToU32`.
4. **Unsigned to float.** Where the ROM converts a `u8` or `u32` to float it adds 2^32 to negative results; in C
   this is `(f32)(u32)x`.
5. **`__sinf`.** The vibrato needs libultra's `sinf` as the ROM computes it (the ROM's `__sinf` at 801C6000), not
   the C library's. `port_pre.h` maps `__sinf` to `port_sinf` (`src/port/libultra.c`), which is bit-exact.
6. **Endianness.** Section 5 gives the readers: `musRead16/32` for songs, byte reads for streams, `musBankInit` for pbanks.
7. **Keep the quirks.** This map lists behaviours that look like bugs. The code keeps them as they are: the 0xA8 bend
   square (5.2), the pan resend (8.3), the 0x95 negative bend save (8.9), the tempo command on effects (8.8), the
   channel 0 check in `musSongChannelFind` (9.2), the default envelope (6.2), the shared chorus and low-pass state
   (10).
8. **Edge cases with bad data.** Command bytes above 0xAA, `loopDepth == 0` at command 0x96, and commands 0x90 and
   0x9E in effects are undefined in the ROM. The code may add an assertion there, but the behaviour for good data
   must not change.
9. **Comments.** Short plain sentences. Comment only arithmetic that is not obvious.
10. **Locks.** Public `Mus*` functions take `musLock` (section 12.2).

## 15. Notes for each file

Section 16.1 lists the functions of each file.

### Where the globals are defined

`musCommands` is in mus-player.c; `musReverb`, `musCustomFxCount` and `musCustomFxPresets` are in mus-reverb.c;
`musLock`, `musUnlock`, `musAudioPoll`, `musAudioRender`, `musAiFrequency`, `amDmaNew` and the native
`amCreateAudioMgr` are in mus-glue.c; every other global of section 7 is in mus-init.c. All are
ordinary C globals outside `port_ram`, except the objects that the sound player's memory points to. `gen/ram.S`
defines those in `port_win`, after the game's memory: `musGlobals`, `musPlayer`, `musHeap`, the DMA state
`musDmaState`, the output buffers `musBuffer` and `musDropBuffer`, and `musDmaZeros` ([port64.md](port64.md)).

### What each file must keep

- **mus-cmd-a.c, mus-cmd-b.c** (sections 5.2 and 8): `musCmdSetEnvelope` is also called with a pointer into a
  song's envelope table, so it must not assume that the stream continues after its 7 bytes.
- **mus-player.c** (sections 8.1 and 8.2): voice `i` is `&musVoices[i]`; the synthesizer is `&musGlobals.drvr`.
- **mus-voice.c** (sections 8.3 to 8.7): the order of the float operations in `musPow2` and in the pitch sum is
  the ROM's.
- **mus-start.c** (section 9): songs are read with `musRead32` and never changed. Zero offsets give NULL pointers.
- **mus-handle.c** (sections 9.5, 5.7 and 10 for `MusSetSongFxChange`): `MusHandleSetTempo` and
  `MusHandleSetReverb` clamp their value before the handle loop, after the test for handle 0. `MusAsk` and
  `MusHandleAsk` call `musAudioPoll()` first (section 12.2).
- **mus-init.c** (sections 11.1 and 5.5): `musBankInit` converts the pbank in place, where the ROM relocates it.
- **mus-reverb.c** (section 10): `CustomSynNew` builds the native synthesizer's filter graph with the
  synthesizer's own types (`synth_internal.h`).
- **mus-glue.c** (section 12): the native model of section 12.2, not a translation of the ROM's glue.

## 16. Reimplementation status

All 109 functions of the ROM's libmus are accounted for. 101 are translated: the 43 commands, the 23 API
functions, 26 of the 27 internal functions and the 9 reverb functions. The relocation helper (801C2A84) has no
port version. The 7 glue functions are replaced by the native model of section 12.2. With the native synthesizer
(`src/audio/synth`, [audio-synth-map.md](audio-synth-map.md)) and the platform's sound device, `make AUDIO=mus`
builds a port that plays the game's music and sound effects. The status of the whole sound player is in section 20
of the synthesizer's map.

### 16.1 Files and functions

Every file is in `src/audio/mus/` and includes `mus_internal.h`.

| File | Functions (ROM address) | Port additions |
| --- | --- | --- |
| mus-cmd-a.c | commands 0x80-0x90: musCmdEnd (801BF800) to musCmdEnvelopeIndex (801BFD0C) | none |
| mus-cmd-b.c | commands 0x91-0xAA: musCmdKeepEnvelopeOn (801BFD6C) to musCmdChangeEffect (801C02BC); musRandom (801C2668) | static musRandomOperand, shared by 0xA3-0xA5 |
| mus-player.c | musCommands (801F40C0), musPlayerHandler (801C1338), musChannelNextEvent (801C15B8), musVoiceStart (801C19FC) | none |
| mus-voice.c | musVoiceUpdateVolumePan (801C1A90), musVoiceUpdatePitch (801C1C1C), musEnvelopeStart (801C1D48), musEnvelopeUpdate (801C1DB4), musPanSweepStart (801C1FBC), musPanSweepUpdate (801C1FD8), musTrillUpdate (801C2088), musVibratoUpdate (801C20D8), musVolumeStreamUpdate (801C2144), musBendStreamUpdate (801C21FC), musPow2 (801C2314) | static musStreamReadEvent, shared by the two stream functions |
| mus-start.c | MusStartSong (801C0694), MusStartEffect (801C0808), MusStartEffect2 (801C08E4), MusBankStartSong (801C0EE0), MusBankStartEffect (801C0F28), MusBankStartEffect2 (801C0F70), musChannelReset (801C2768), musSongChannelFind (801C28A0), musEffectStart (801C2B24) | static musSongTrackStream, musEffectAllocate (the ROM has a copy in each start function), musSelectBank |
| mus-handle.c | MusSetMasterVolume (801C066C), MusStop (801C0A2C), MusAsk (801C0B00), MusHandleStop (801C0B70), MusHandleAsk (801C0C1C), MusHandleSetVolume (801C0C70), MusHandleSetPan (801C0CD4), MusHandleSetFreqOffset (801C0D40), MusHandleSetTempo (801C0DAC), MusHandleSetReverb (801C0E3C), MusHandleGetPtrAddr (801C0FD8), MusHandlePause (801C102C), MusHandleUnPause (801C1054), MusSetFxType (801C1080), MusSetSongFxChange (801C10B4), musFifoInit (801C1100), musFifoProcess (801C1184), musFifoExecute (801C1228), musFifoPost (801C12A0), musHandleSetFlags (801C2BB0) | static musChannelMatchesFlags, musChannelStop, musPostMessage |
| mus-init.c | MusInitialize (801C0300), MusBankInitialize (801C0EC0), musBankInit (801C249C), musMemSet (801C2A18), musMemCopy (801C2A4C); the globals of section 7 | static musBankPointer, musBankIsConverted, musLoopToHost, musBookToHost (the byte order conversion of a pbank) |
| mus-reverb.c | CustomInit (801C3710), CustomSynNew (801C3758), CustomAllocFX (801C3A58), CustomFxNew (801C3AFC), CustomFxSet (801C3C34), CustomSetDefaultFx (801C3F18), CustomMaxSections (801C3F30), CustomMaxLength (801C3F78), ChangeCustomEffect (801C3FC0); musReverb and the table of the six presets | none |
| mus-glue.c | native amCreateAudioMgr (for 801C2C10) and amDmaNew (for 801C35B8). amAudioThread (801C3040), amHandleFrame (801C3198), amDmaCallback (801C33A4) and amClearDma (801C35F0) are replaced by musAudioRender, musRenderFrame, musDmaRead and musDmaClear. amHandleTaskDone (801C3364) has no replacement. | musLock, musUnlock, musAiFrequency, musAudioRender, musAudioPoll, musAudioAdvance; static musDmaRead, musDmaClear, musRenderFrame, musCheckEffectTables and helpers |

`mus_internal.h` holds the shared declarations, the big-endian readers (`musRead16`, `musRead32`,
`musReadVarLen`), the declarations of what the port provides (`port_audio_*`, `port_rom`, `port_log`, `osTvType`)
and layout checks. The checks stop the build when a structure that libmus keeps in `musHeap` or reads from a pbank
loses its ROM size or offsets. `synth_internal.h`, which it includes, checks the synthesizer's types.

### 16.2 How it was checked

- Each file compiles with no warnings with `gcc -m32 -std=gnu99 -O2 -Wall` and with the Makefile's game flags
  plus `-Wall`, with gcc for Linux and with i686-w64-mingw32-gcc for Windows.
- `tools/mustest/build.sh` links every file with the native synthesizer and runs `MusInitialize` with the
  configuration of `InitMusicDriver`. The heap use is 281168 bytes (0x44A50). The offsets of the voices, the channels, the message
  ring, the save filter, both buses and every reverb buffer from the start of `audio_memory` are as on the N64. The
  N64's values come from a snapshot of its memory during the attract mode (`tools/n64ref/emu.py`):
  `musicMemoryUsed` and the pointers that libmus keeps in its globals. The output rate is 22047 Hz on both.
- With the game's objects, the synthesizer and the platform layer, `make AUDIO=mus` links: no symbol is defined
  twice and none is missing.

Not checked yet: the player's state over time against the N64. That needs a trace of the channels per player
call from an emulator, compared with the port's.

### 16.3 What libmus needs from the synthesizer

These are the libaudio functions that libmus calls, with the signatures of `PR/libaudio.h` and
`synth_internal.h`. `src/audio/synth` provides each.

| Function | How libmus uses it |
| --- | --- |
| `alHeapInit(ALHeap *, u8 *base, s32 len)` | once, on `audio_memory` |
| `alHeapDBAlloc(u8 *file, s32 line, ALHeap *, s32 num, s32 size)` | every block, with file NULL and line 0. It must work like the ROM's (801CB280): `base` rounded up to 16 bytes by `alHeapInit`, each block `num * size` rounded up to 16, NULL when it does not fit. `MusInitialize` returns `cur - base`. |
| `alLink(ALLink *, ALLink *after)` | puts each physical voice on `pFreeList` |
| `alSynAddPlayer(ALSynth *, ALPlayer *)` | registers `musPlayer`: `handler` = `musPlayerHandler`, `clientData` = `&musPlayer`, `next` = NULL |
| `alSynAllocVoice(ALSynth *, ALVoice *, ALVoiceConfig *)` | once per channel at start-up, config `{ priority = thread_priority (140), fxBus = 0, unityPitch = 0 }`. libmus never frees a voice. |
| `alSynStartVoice(ALSynth *, ALVoice *, ALWaveTable *)` | starts a note's wave. libmus sends no pitch, volume or pan with it. |
| `alSynStopVoice(ALSynth *, ALVoice *)` | before a new wave, at the end of a channel |
| `alSynSetVol(ALSynth *, ALVoice *, s16 vol, ALMicroTime delta)` | vol 0..0x7FFF, delta always `musFrameMicros` (16666) |
| `alSynSetPitch(ALSynth *, ALVoice *, f32 ratio)` | ratio above 0, at most 2.0 |
| `alSynSetPan(ALSynth *, ALVoice *, ALPan pan)` | 0..127 |
| `alSynSetFXMix(ALSynth *, ALVoice *, u8 fxmix)` | at a note, when the reverb changed |
| `alAudioFrame(Acmd *cmdList, s32 *cmdLen, s16 *outBuf, s32 outLen)` | called by mus-glue only (section 16.4) |
| `alFilterNew(ALFilter *, ALFilterHandler, ALSetParam, s32 type)` | the reverb: handler NULL, `alFxParam`, type 5 |
| `alSaveNew`, `alSaveParam` | the output filter; source (param 1) = main bus |
| `alAuxBusNew(ALAuxBus *, void *sources, s32 max)`, `alAuxBusParam` | one aux bus with 24 source slots; each voice's envelope mixer is added (param 2) |
| `alMainBusNew(ALMainBus *, void *sources, s32 max)`, `alMainBusParam` | 24 source slots; the reverb is added (param 2), or the aux bus when fxType is 0 |
| `alLoadNew(ALLoadFilter *, ALDMANew, ALHeap *)`, `alLoadParam` | one per voice; source (param 1) NULL |
| `alResampleNew(ALResampler *, ALHeap *)`, `alResampleParam` | one per voice; source = the load filter |
| `alEnvmixerNew(ALEnvMixer *, ALHeap *)`, `alEnvmixerParam` | one per voice; source = the resampler |
| `alFxParam(void *, s32, void *)` | source (param 1) = aux bus 0, at start-up and after each preset change |
| `alFxParamHdl`, `alFxPull` | only their addresses are stored, in `ALFx.paramHandler` and `ALFx.filter.handler` |
| `init_lpfilter(ALLowPass *)` | for each preset section with a low-pass coefficient; libmus has set `fc` and `fstate` |

libmus also sets `alGlobals` (in `port_ram`, from `gen/ram.S`) to `&musGlobals` in `CustomInit`. It does not
call `alSynNew`, `alInit`, `alClose`, `alSynFreeVoice`, `alSynSetPriority` or `alSynStartVoiceParams`.

Rules the synthesizer must follow:

- **Heap.** The ROM's constructors take state blocks from the heap that libmus passes: `alLoadNew` two blocks of
  0x20 bytes, `alResampleNew` one of 0x20, `alEnvmixerNew` one of 0x50, in that order. With these, the heap use
  equals the N64's. `alLoadNew` calls the `ALDMANew` it gets (`amDmaNew`) once, with the address of its DMA
  state word, and keeps the `ALDMAproc` it returns.
- **Wave data.** The DMA routine returns a host pointer to the bytes, which keep the ROM's (big-endian) order.
  ADPCM frames are bytes. Raw 16-bit samples (none in Glover) would need swapping. `musBankInit` has already
  converted the wave tables, loops and codebooks to host order.
- **Voices keep their state.** A voice keeps its pitch, volume, pan and reverb mix across a stop and a start.
  libmus sends a value only when it differs from the last one it sent.
- **Timing.** Section 4: the handler runs when the sample clock reaches its call time. Its return value
  (microseconds) becomes samples with the formula of `func_801CB864`. Changes made inside the handler take effect
  at the call time rounded down to 16 samples.
- **Structures.** libmus writes these fields, and the synthesizer must read them where the ROM's libaudio does:
  `ALSynth` (all fields, in `CustomSynNew`), `ALSynConfig` (all), `ALPlayer` (`next`, `handler`, `clientData`),
  `ALVoiceConfig` (all), `ALHeap` (`base`, `cur`), `ALWaveTable`, `ALADPCMloop`, `ALRawLoop` and `ALADPCMBook` (all,
  in host order), `ALFilter` (`handler` of the reverb), `ALFx` (`filter.handler`, `delayLine`, `inputPos`,
  `length`, `sections`, `sectionCount`, `paramHandler`), `ALDelay` (all), `ALResampler` (`state`, `delta`,
  `first`), `ALLowPass` (`fc`, `fstate`), `PVoice` (`node`, `vvoice`, `channelKnob`, and the three filters it
  builds in place), `ALParam` (`next`). The layouts are in `synth_internal.h`, with the ROM's sizes.

### 16.4 What the port provides

- **The platform.** With `make AUDIO=mus`, `src/port/platform.c` is compiled with `-DPORT_AUDIO_MUS` and defines
  `port_audio_open`, `port_audio_lock`, `port_audio_unlock` and `port_audio_yield` (declared in `mus_internal.h`,
  described in `mus-glue.c`). The lock is a recursive SDL mutex that works before `port_audio_open`: `MusInitialize`
  holds it while `amCreateAudioMgr` opens the stream. After the stream starts, the platform must not call
  `SDL_LockAudioDevice`, `SDL_PauseAudioDevice` or `SDL_CloseAudioDevice` while it holds the lock. The SDL callback
  holds the device lock while it waits for this one. Such a call would deadlock. `port_audio_open` opens no device
  in the headless mode or with `--no-sound`.
- **The frame driver.** `src/port/frame.c` calls `musAudioAdvance(dt * retraces)` once per update, before
  `GameStep`. With a sound device it does nothing. `src/port/audio_stub.c` has an empty `musAudioAdvance`.
- **`alAudioFrame` in the native synthesizer.** mus-glue calls it with the first command list (in the libmus heap).
  The player handlers whose call times fall in the frame run first. Then `outLen` stereo frames of host-order 16-bit
  samples (left, right) are in `outBuf`. `*cmdLen` is 0 only when no player is registered.
- **The effect tables** (section 12.3). The generated tables load the effect streams as bytes, the list as host
  pointers and the priorities as host integers. In an `AUDIO=mus` build, `musCheckEffectTables` compares them with
  the ROM at start-up and logs any difference. It logs none.
- **The regression test.** With `AUDIO=mus`, libmus writes `audio_memory` and `alGlobals` in `port_ram`, which the
  memory hash covers. The baseline was made with the stand-in, and the test uses the stand-in. `alGlobals` and the
  load filters' DMA state words hold pointers to objects in `port_win`, after the game's memory. They do not change
  between runs of one program.

### 16.5 Open questions and known differences

Bad data. The ROM leaves these cases undefined. The parser of section 1 found no command byte above 0xAA in
Glover's data.

- A command byte above 0xAA. The ROM reads past its table and crashes. The port logs the byte and ends the
  channel (`musChannelNextEvent`). This is the only deliberate change of behaviour.
- Command 0x96 with `loopDepth` 0. The ROM uses the byte before `loopCount`. On the host that byte is the high
  byte of `loopBendCount[3]`, not the low byte. The effect on that count differs.
- Commands 0x90 and 0x9E in an effect (no song). The ROM reads low N64 addresses. The port dereferences NULL.
- Command 0xA1 with a missing volume or bend stream gives a bogus pointer, as in the ROM.
- An envelope with 0 attack or 0 decay steps and equal levels gives NaN on the host. The N64 traps (its FPCSR
  enables the invalid operation trap). A fade with `stopLength` 0 and `0x6000 / musFrameRate` before
  `MusInitialize` divide by zero on both.
- `ChangeCustomEffect` does not check for a negative type, as in the ROM. With fxType 0, `musReverb.synth` stays
  NULL and a preset change crashes, as in the ROM. Glover always uses fxType 6.
- `musEffectAllocate` with priority 0x7FFFFFFF: the ROM starts the effect on an unset pointer. The port does not
  start it.

Differences that good data cannot show:

- Songs are never relocated in place. The ROM relocates a song only when its header word 0x04 is below 0x400.
  Every song in Glover has the offsets 0x18 to 0x138. Both read the same streams.
- Commands 0xA3-0xA5, `musRandom`, `musHandleSetFlags`, `musChannelReset` and the stream functions write some
  fields once where the ROM writes them twice or in another order. The lock hides the difference.
- `musPostMessage` writes zeros into the message's padding. The ROM copies stack bytes.
- Every `alAudioFrame` call makes 1104 samples. The N64 made 1104, and now and then 16 more or less, to follow
  the audio interface. The handler's call times do not depend on this.
- `musAudioAdvance` counts 60 retraces a second. The NTSC video rate is about 59.94 Hz. In headless runs the
  port therefore makes about 0.1% fewer samples per retrace than the N64: one player call fewer about every 17
  seconds.

Build and runtime conditions:

- The float arithmetic matches the ROM only with the game's flags (`-msse2 -mfpmath=sse -ffp-contract=off`). The
  plain flags of the link test give x87 code, which is not exact.
- `musLock` turns on flush-to-zero only when the code is compiled for SSE arithmetic. It does not turn on
  denormals-are-zero. The ROM never computes with a denormal operand: the R4300 traps on one.
- `MusHandleAsk` polls only for a nonzero handle, and `MusAsk` always polls. Both poll before they take the lock.
  In headless runs, this decides how many player calls a busy wait in the game runs.
- The game declares `MusHandleStop`, `MusHandleSetVolume`, `MusHandleSetPan`, `MusHandleSetFreqOffset`,
  `MusHandleSetTempo` and `MusHandleSetReverb` as void. libmus returns a count. On x86 this is harmless: the
  caller ignores the return register.
