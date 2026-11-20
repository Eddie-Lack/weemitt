# libaudio's synthesizer in Glover: a map for the native port

This document describes the part of libaudio that Glover's ROM contains: the synthesizer that turns libmus's voice
changes into audio command lists for the RSP. The native reimplementation is in `src/audio/synth/`, and section 20
gives its status. The shared declarations are in
[`src/audio/synth/synth_internal.h`](../src/audio/synth/synth_internal.h). The names used below are the names in that
header. Read [audio-libmus-map.md](audio-libmus-map.md) first, especially sections 4, 9.6, 10 and 12. libmus drives
this synthesizer.

## Contents

1. [Sources and method](#1-sources-and-method)
2. [Overview](#2-overview)
3. [Where libaudio is in the ROM](#3-where-libaudio-is-in-the-rom)
4. [Data and byte order](#4-data-and-byte-order)
5. [Structures](#5-structures)
6. [Parameter ids and filter types](#6-parameter-ids-and-filter-types)
7. [Set up](#7-set-up)
8. [The frame: alAudioFrame](#8-the-frame-alaudioframe)
9. [Voices and parameter changes](#9-voices-and-parameter-changes)
10. [Envelope mixer: volume, pan and the reverb send](#10-envelope-mixer-volume-pan-and-the-reverb-send)
11. [Resampler: pitch](#11-resampler-pitch)
12. [Load filter: ADPCM and raw waves](#12-load-filter-adpcm-and-raw-waves)
13. [Buses and the save filter](#13-buses-and-the-save-filter)
14. [Reverb](#14-reverb)
15. [Audio commands](#15-audio-commands)
16. [Function reference](#16-function-reference)
17. [libmus and the synthesizer together](#17-libmus-and-the-synthesizer-together)
18. [Rules for changing this code](#18-rules-for-changing-this-code)
19. [Files and functions](#19-files-and-functions)
20. [Status](#20-status)

## 1. Sources and method

Everything here comes from the ROM's machine code, as split by splat into `gloveworld/asm/C0800.s`, lines 10045 to
14779. The data comes from `gloveworld/asm/data/D064.rodata.s` and `gloveworld/asm/data/F50C0.data.s`. The public
headers `PR/libaudio.h` and `PR/abi.h` give the public types and the command numbers. No libaudio source code was
used, and no emulator's audio code.

A draft in C of any function can be made with m2c. Extract the libaudio part of the file once, then name the
function:

```sh
sed -n 10045,14779p ../gloveworld/asm/C0800.s > libaudio.s
python3 ../gloveworld/tools/m2c/m2c.py -t mips-gcc-c --globals none libaudio.s \
    ../gloveworld/asm/data/D064.rodata.s ../gloveworld/asm/data/F50C0.data.s -f alEnvmixerPull
```

m2c misreads some argument lists. For example, it does not see that `synGetRate` takes two doubles in `$f12` and
`$f14` and two more arguments on the stack. Check its output against the assembly and this map.

Facts checked against the game data with small scripts (not kept): every wave of both pbanks is ADPCM, with an
order 2 codebook of 4 predictors (128 bytes); 95 waves loop forever (loop count -1) and the rest do not loop; 48
loops start at a sample that is not a multiple of 16; every reverb amount in the songs and effects (command 0xA2)
is 0 to 127.

## 2. Overview

libaudio's synthesizer is a graph of filters. Each filter has a pull function that appends audio commands to a list,
and a parameter function. To make a frame, `alAudioFrame` pulls the last filter. Each filter first pulls its source
and then adds its own commands. The RSP runs the list later. The port runs it at once (section 15.4).

The graph that libmus builds (`CustomSynNew`, map section 10) is:

```
save filter <- main bus <- reverb (ALFx) <- aux bus <- 24 voices
voice: envelope mixer <- resampler <- load filter (ADPCM decoder) <- wave data in the ROM
```

- A **physical voice** (`PVoice`) is one load filter, one resampler and one envelope mixer. libmus's `ALVoice`s
  (virtual voices) each own one physical voice.
- The **load filter** reads ADPCM frames with the DMA routine and decodes them.
- The **resampler** changes the pitch.
- The **envelope mixer** applies volume and pan, and sends the voice to the dry mix (main buffers) and to the
  reverb (aux buffers). It also holds the voice's list of timed parameter changes (`ALParam`).
- The **aux bus** clears the aux buffers and pulls every voice.
- The **reverb** mixes the aux buffers into mono, runs it through a delay line with several sections, and writes
  the result back to both aux buffers.
- The **main bus** clears the main buffers, pulls the reverb, and adds the reverb's output to the main buffers.
- The **save filter** interleaves the main buffers into stereo and writes them to the output buffer.

**Timing.** `alAudioFrame` first calls each player's handler (libmus's `musPlayerHandler`) for every call time that
falls in the frame. The handler calls the `alSyn*` API. Each call becomes an `ALParam` with a sample time, queued in
the voice's envelope mixer. The pull functions then apply each change at its sample time.

**Code in the ROM that Glover never runs.** `alInit`, `alSynNew`, `alSynAllocFX`, `alFxNew` and `alFxParamHdl` are
in the ROM, but libmus uses its own `CustomInit`, `CustomSynNew` and `CustomFxNew`, and nothing calls
`alFxParamHdl`. `alRaw16Pull` never runs: every Glover wave is ADPCM. `_frexpf` and `_ldexpf` are never called
(`synGetRate` has the same code inline). The envelope mixer handles two change types that nothing posts:
`SYN_FREE_VOICE` and `SYN_START_VOICE_PARAMS` (`alSynFreeVoice` and `alSynStartVoiceParams` are not in the ROM).
The translation includes all of them except `_frexpf`, `_ldexpf` and `__assert` (section 19).

## 3. Where libaudio is in the ROM

ROM offset = VRAM - 0x800FF000 for this segment.

| Part | VRAM | ROM | Notes |
| --- | --- | --- | --- |
| Code | 801C7EB0-801CC004 | C8EB0-CD004 | 64 functions, `C0800.s` lines 10045-14779 |
| Constants | 8010C440-8010C653 | D440-D653 | doubles, jump tables and assert strings, listed below |
| alFxNew presets | 801F4420-801F45AF | F5420-F55AF | small room, big room, echo, chorus, flange, none |
| Equal-power table | 801F45B0-801F46AF | F55B0-F56AF | 128 `s16` (section 10.1) |
| `alGlobals` | 801F46D0 | F56D0 | `ALGlobals *`, NULL until set up |
| Audio microcode data | 801F3E00-801F40BF | F4E00-F50BF | `aspMainDataStart`, 0x2C0 bytes (section 15.4) |
| Audio microcode code | 801BE9E0-801BF7FF | BF9E0-C07FF | `aspMainTextStart`, 0xE20 bytes |

Constants (rodata) used by the synthesizer:

| Address | Type | Value | Used by |
| --- | --- | --- | --- |
| 8010C440, 8010C460 | f64 | 16384.0 | `init_lpfilter`, `alFxNew` |
| 8010C468, 8010C5E0 | f64 | 173123.404906676 | `alFxNew`, `alFxParamHdl` (chorus depth) |
| 8010C470, 8010C5E8 | f64 | 4294967296.0 | unsigned to double conversions |
| 8010C480 | string | "EX" | the expression text of every assert |
| 8010C484, 8010C5B4, 8010C614, 8010C644 | string | "env.c", "reverb.c", "save.c", "synthesizer.c" | assert file names |
| 8010C4D8-8010C4E8 | f64 | 1.0, 0.5, 0.5 | `_frexpf` |
| 8010C4F0 | f64[8] | -0.912537, -0.752072, -0.607683, -0.476438, -0.356144, -0.245112, -0.142019, -0.045804 | `synGetRate` (log2 table) |
| 8010C530 | f64 | 1.0 | `synGetRate` (1 / count) |
| 8010C538, 8010C568 | f64 | 1073741824.0 (2^30) | `synGetRate` |
| 8010C540-8010C550 | f64 | 1.0, 0.5, 0.5 | `synGetRate` (inline frexp) |
| 8010C558 | f64 | 16.0 | `synGetRate` (table index) |
| 8010C560 | f64 | 0.6931471805599453 (ln 2) | `synGetRate` |
| 8010C570 | f64 | 1.0 | `synGetRate` |
| 8010C578 | f64 | 65535.0 | `synGetRate` (fraction) |
| 8010C580 | f64 | 1.99996 (`MAX_RATIO` in abi.h) | `alResamplePull` |
| 8010C5F0, 8010C608 | f64 | 1.0 | `_loadOutputBuffer`, `_doModFunc` |
| 8010C5F8, 8010C600 | f64 | 2.0, 4.0 | `_doModFunc` |
| 8010C620, 8010C630 | f64 | 1000000.0 | `synTimeToSamplesExact`, `_timeToSamples` |
| 8010C628, 8010C638 | f64 | 0.5 | the same |

Immediate constants in the code: 1000.0f (0x447A0000), 32768.0f (0x47000000), 65536.0f (0x47800000), 1.0f, and
1.99996f (0x3FFFFEB0, the clamped ratio).

The log2 table holds log2((17 + 2i) / 32) for i = 0 to 7: the base-2 logarithm of the middle of the interval
[(8 + i) / 16, (9 + i) / 16), rounded to six decimals. The six-decimal literals above give exactly the ROM's doubles. The port's copy of the
rodata in `port_ram` is no use for doubles: `gen/ramtab.c` swaps that area as 32-bit words.

## 4. Data and byte order

| Data | Where | Port handling |
| --- | --- | --- |
| Synthesizer structures, ALParams, state blocks | `musHeap`, in `port_ram` | host order; the ROM's sizes (the header checks them) |
| Audio commands (`Acmd`) | the command list, in `musHeap` | two host-order `u32` words, built with shifts |
| Addresses in commands | | the 32-bit words of the pointers (`src/port/ptr32.h`); no mask (see below) |
| ADPCM codebook, ADPCM loop state | the pbank, in `port_ram` | host-order `s16`, converted by `musBankInit` |
| Low-pass coefficients | `ALLowPass.coef`, in `musHeap` | host-order `s16`, written by `init_lpfilter` |
| Wave data (ADPCM bytes) | the ROM | not converted; the DMA routine copies them from `port_rom` into a DMA buffer in `musHeap` |
| Equal-power table | ROM data at 801F45B0 | read from `port_ram` (`D_801F45B0`), already swapped to host `s16` |
| Output samples | the buffer passed to `alAudioFrame` | host-order `s16`, left then right |

**Commands.** abi.h defines each command as a structure of bit fields. On x86 GCC orders bit fields from the low bit,
so those structures do not give the RSP's layout. Build every command from the words `w0` and `w1` as section 15
lists. The interpreter reads them the same way.

**Addresses.** The ROM converts each RDRAM address in a command with `osVirtualToPhysical` or with
`& 0x1FFFFFFF`. On the N64 every such address is below 8 MB, so its top byte is 0. The port stores the word of the
pointer, as the game's pointers in `port_ram` are stored ([port64.md](port64.md)): `PORT_P2W` of a plain pointer, or
the `(u32)` cast of a `PTR32` member. The interpreter reads each address with `PORT_W2P`. On the normal builds the
word is the host pointer. Do not mask it. Every address points into `port_ram` (the libmus heap and the pbanks) or
`port_win` (the output buffers and `musDmaZeros`).

**Segments.** `alAudioFrame` starts every 160-sample chunk with `A_SEGMENT` for segment 0 and base 0. No other
`A_SEGMENT` occurs. Every address the RSP sees therefore uses segment 0, which is 0. The interpreter can treat each
address word as the word of a pointer and ignore `A_SEGMENT`.

**DMA.** The load filter gets wave bytes through `ALLoadFilter.dma`, the routine that libmus's `amDmaNew` returns
(`mus-glue.c`). It returns the word of a pointer as an `s32`. Like the ROM's routine, it copies a whole buffer of the ROM
from the even address at or below the request into one of the ROM's DMA buffers. The buffers are in `musHeap` at the
ROM's offsets, and `port_ram` is aligned to 4096 bytes. So the low bits of each returned address are the ROM's, and
so are `dramAlign` and the commands that depend on it. `A_LOADBUFF` loads from up to 7 bytes before the request and
up to 15 bytes after it. These bytes are inside the DMA buffers or the heap, as on the N64.

**State blocks.** The interpreter alone reads and writes `ADPCM_STATE`, `RESAMPLE_STATE`, `ENVMIX_STATE` and
`POLEF_STATE` blocks. It keeps the microcode's layout of each (section 15.4) with host-order `s16`. `A_SETLOOP`
points at a copy of the pbank's loop state, which is host-order `s16` too.

## 5. Structures

All structures are in `synth_internal.h` with their ROM offsets. `_Static_assert`s there check every size and the
main offsets with `gcc -m32`. The header compiles together with `mus_internal.h` and every libmus file.

### 5.1 Public types (libaudio.h)

These come from `PR/libaudio.h` unchanged. The ROM uses them with these layouts.

**ALSynth** (0x4C, `musGlobals.drvr`, in `port_win` after the game's memory):

| Offset | Field | Use |
| --- | --- | --- |
| 0x00 | head | first `ALPlayer`; NULL means `alAudioFrame` makes nothing |
| 0x04 | pFreeList | free physical voices (`ALLink`) |
| 0x0C | pAllocList | physical voices in use |
| 0x14 | pLameList | voices freed by `SYN_FREE_VOICE`; moved to the free list after each frame |
| 0x1C | paramSamples | sample time given to changes made now: the current handler's call time rounded down to 16 |
| 0x20 | curSamples | sample time of the start of the next chunk |
| 0x24 | dma | `ALDMANew`, from `ALSynConfig.dmaproc` |
| 0x28 | heap | `ALHeap *` |
| 0x2C | paramList | free `ALParam`s |
| 0x30 | mainBus | `ALMainBus *` |
| 0x34 | auxBus | `ALAuxBus *`, an array of `maxAuxBusses` |
| 0x38 | outputFilter | the save filter |
| 0x3C | numPVoices | `ALSynConfig.maxPVoices` (24) |
| 0x40 | maxAuxBusses | 1 |
| 0x44 | outputRate | 22047 in Glover (map section 4) |
| 0x48 | maxOutSamples | 160: samples per chunk |

**ALPlayer** (0x14): next (0x00), clientData (0x04), handler (0x08, returns the delay to the next call in
microseconds), callTime (0x0C, never used by the synthesizer), samplesLeft (0x10, the sample time of the next call,
not a count).

**ALVoice** (0x1C, libmus's `musVoices`): node (0x00), pvoice (0x08), table (0x0C, cleared by `alSynAllocVoice`,
never set), clientPrivate (0x10, never used), state (0x14, set to 0 by `alSynAllocVoice`, never used again), priority
(0x16), fxBus (0x18, stored, never used), unityPitch (0x1A).

**ALVoiceConfig** (6 bytes): priority (0x00, `s16`), fxBus (0x02), unityPitch (0x04, `u8`). libmus passes 0x8C, 0
and 0.

**ALSynConfig** (0x24): maxVVoices (0x00, not read), maxPVoices (0x04), maxUpdates (0x08), maxFXbusses (0x0C, not
read), dmaproc (0x10), heap (0x14), outputRate (0x18), fxType (0x1C, `u8`), params (0x20). Glover: 24, 24, 0x400,
-, `amDmaNew`, `&musHeap`, 22047, `AL_FX_CUSTOM`, `musFxBigRoom`.

**ALHeap** (0x10): base, cur, len, count (0x0C, set to 0 and never changed).

### 5.2 ALFilter (0x14)

The first member of every filter.

| Offset | Field | Use |
| --- | --- | --- |
| 0x00 | source | the filter this one pulls from |
| 0x04 | handler | pull function (`ALFilterHandler`) |
| 0x08 | setParam | parameter function (`ALSetParam`) |
| 0x0C | inp | `s16`, set to 0, never used |
| 0x0E | outp | `s16`, set to 0, never used |
| 0x10 | type | `SynFilterType`, never read |

`ALFilterHandler` is `Acmd *(*)(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p)`. It appends the
commands that make `outCount` output samples starting at sample time `sampleOffset`, and returns the new end of the
list. `outp` points at an `s16` DMEM offset. The resampler passes a pointer to its input offset to the load filter,
and the load filter moves it to the first valid sample. The other filters ignore `outp` or pass it on.

`ALSetParam` is `s32 (*)(void *filter, s32 paramID, void *param)`. Every one returns 0, except `alLoadParam`, which
returns nothing; no caller uses the result.

### 5.3 ALLoadFilter (0x48)

| Offset | Field | Use |
| --- | --- | --- |
| 0x00 | filter | handler `alAdpcmPull` or `alRaw16Pull`, setParam `alLoadParam`, type 0 |
| 0x14 | state | `ADPCM_STATE *` (0x20 bytes from the heap) |
| 0x18 | loopState | `ADPCM_STATE *` (0x20 bytes): copy of the wave's loop state |
| 0x1C | loop | `ALRawLoop`: start, end (samples), count (`u32`, 0xFFFFFFFF forever) |
| 0x28 | table | the `ALWaveTable` |
| 0x2C | bookSize | `order * npredictors * 16`: bytes of codebook (128 for Glover) |
| 0x30 | dma | `ALDMAproc` |
| 0x34 | dmaState | filled by the `ALDMANew` |
| 0x38 | sample | sample number of the next output sample |
| 0x3C | framePos | samples of the last decoded 16-sample frame already output, 0 to 15 |
| 0x40 | first | 1: the next `A_ADPCM` has `A_INIT` |
| 0x44 | dataAddr | wave address of the next frame to read; in Glover a cartridge address (0xB0000000 + ROM offset) |

### 5.4 ALResampler (0x34)

| Offset | Field | Use |
| --- | --- | --- |
| 0x00 | filter | handler `alResamplePull`, setParam `alResampleParam`, type 1 |
| 0x14 | state | `RESAMPLE_STATE *` (0x20 bytes) |
| 0x18 | ratio | `f32`, 1.0 at first |
| 0x1C | unityPitch | 1: copy, do not resample |
| 0x20 | delta | `f32`, fraction of an input sample carried over |
| 0x24 | first | 1: the next `A_RESAMPLE` has `A_INIT` (its low byte is the command's flags) |
| 0x28, 0x2C | unused28, unused2C | set to 0, never read |
| 0x30 | playing | set by `SYN_START`, cleared by `SYN_RESET`, never read |

The reverb's chorus also uses an `ALResampler` (section 14.4). There only `state`, `delta` and `first` are used, and
nothing calls `alResampleNew` on it.

### 5.5 ALEnvMixer (0x4C)

| Offset | Field | Use |
| --- | --- | --- |
| 0x00 | filter | handler `alEnvmixerPull`, setParam `alEnvmixerParam`, type 4 |
| 0x14 | state | `ENVMIX_STATE *` (0x50 bytes) |
| 0x18 | pan | 0 to 127; 0 is left |
| 0x1A | volume | `(v * v) >> 15` of the last volume change; 1 after `SYN_RESET` |
| 0x1C | volLeft | left volume at the start of the current ramp (`s16`) |
| 0x1E | volRight | right volume at the start of the current ramp |
| 0x20 | dryAmount | `synEqPower[fxMix]` |
| 0x22 | wetAmount | `synEqPower[127 - fxMix]` |
| 0x24 | rateLeftFrac | `u16`: fraction of the left ramp ratio (16.16) |
| 0x26 | rateLeft | `s16`: integer part |
| 0x28 | targetLeft | `(volume * synEqPower[pan]) >> 15` |
| 0x2A | rateRightFrac | |
| 0x2C | rateRight | |
| 0x2E | targetRight | `(volume * synEqPower[127 - pan]) >> 15` |
| 0x30 | rampPos | samples mixed since the ramp started |
| 0x34 | rampLength | samples of the ramp |
| 0x38 | first | 1: the next sub-frame sends the volumes again with `A_INIT` |
| 0x3C | updates | pending `ALParam`s, oldest first |
| 0x40 | lastUpdate | the last of them |
| 0x44 | unused44 | set to 0, never read |
| 0x48 | playing | 1 after `SYN_START`, 0 after `SYN_RESET` |

### 5.6 Buses and the save filter

**ALMainBus** (0x20): filter (handler `alMainBusPull`, setParam `alMainBusParam`, type 7), sourceCount (0x14),
maxSources (0x18, not checked), sources (0x1C, `ALFilter **`).

**ALAuxBus** (0x4C): the same first four fields (handler `alAuxBusPull`, setParam `alAuxBusParam`, type 6), then
`fx[1]` (0x20, one `ALFx`).

**ALSave** (0x1C): filter (handler `alSavePull`, setParam `alSaveParam`, type 3), dramOut (0x14, the output buffer),
first (0x18, set to 1, never read).

### 5.7 Reverb

**ALFx** (0x2C): filter (handler `alFxPull`, setParam `alFxParam`, type 5), delayLine (0x14), inputPos (0x18, the
write position), length (0x1C, samples), sections (0x20), sectionCount (0x24, `u8`), paramHandler (0x28,
`alFxParamHdl`).

**ALDelay** (0x28), one section:

| Offset | Field | Use |
| --- | --- | --- |
| 0x00 | input | `u32`: input tap, samples behind `inputPos` |
| 0x04 | output | `u32`: output tap, samples behind `inputPos` |
| 0x08 | ffcoef | `s16`: feed-forward gain, input tap into output tap |
| 0x0A | fbcoef | `s16`: feedback gain, output tap into input tap |
| 0x0C | gain | `s16`: the output tap's share of the reverb's result |
| 0x10 | chorusStep | `f32`: phase step per sample |
| 0x14 | chorusValue | `f32`: triangle phase, -2 to 2; 1.0 at set up |
| 0x18 | chorusDelta | `s32`: extra delay of the chorused output tap, in samples |
| 0x1C | chorusDepth | `f32`: depth in samples |
| 0x20 | lowpass | `ALLowPass *`, or NULL |
| 0x24 | resampler | `ALResampler *` for the chorus, or NULL |

A preset word list gives a section as input, output, fbcoef, ffcoef, gain, chorus rate, chorus depth, low-pass
coefficient (`alFxNew` and libmus's `CustomFxSet` both read this order).

**ALLowPass** (0x30): fc (0x00, `s16`), gain (0x02, `s16`, `0x4000 - fc / 2`), unused04 (0x04), coef (0x08, 16
`s16`), fstate (0x28, `POLEF_STATE *`, 8 bytes), first (0x2C).

### 5.8 PVoice (0xDC)

| Offset | Field | Use |
| --- | --- | --- |
| 0x00 | node | `ALLink` in one of the three lists of `ALSynth` |
| 0x08 | vvoice | the owning `ALVoice`, or NULL |
| 0x0C | channelKnob | the envelope mixer: every `ALParam` goes to its setParam |
| 0x10 | decoder | `ALLoadFilter` |
| 0x58 | resampler | `ALResampler` |
| 0x8C | envmixer | `ALEnvMixer` |
| 0xD8 | offset | samples added to the time of each change: 512 after the voice was stolen, else 0 |

### 5.9 ALParam (0x1C)

| Offset | Field | Use |
| --- | --- | --- |
| 0x00 | next | |
| 0x04 | time | sample time of the change: `paramSamples + PVoice.offset` |
| 0x08 | type | `SynParamId` (`s16`) |
| 0x0A | unityPitch | `SYN_START_VOICE`: the voice's `unityPitch` |
| 0x0C | value | the volume (`s16` sign-extended), pan, fx mix, pitch (`f32` bits), `ALWaveTable *` or `PVoice *` |
| 0x10 | rampSamples | `SYN_SET_VOLUME`: ramp length in samples |
| 0x14, 0x18 | unused | |

A change of type `SYN_START_VOICE_PARAMS` has another layout (`SynStartParams`): pitch (0x0C, `f32`), volume (0x10,
`s16`), pan (0x12, `u8`), fxMix (0x13, `u8`), rampSamples (0x14), wave (0x18). Nothing in Glover posts one.

**The pool.** `alSynNew` and `CustomSynNew` allocate `maxUpdates` ALParams (1024 in Glover) and push each on
`paramList`. `__allocParam` pops one and sets its `next` to NULL, or returns NULL when the list is empty.
`__freeParam` pushes one back. Every `alSyn*` call takes one; the envelope mixer frees it when it applies it.

## 6. Parameter ids and filter types

One numbering serves as `paramID` of the setParam functions and as `ALParam.type`.

| Id | Name | Sent to | Effect |
| --- | --- | --- | --- |
| 0 | SYN_FREE_VOICE | ALParam | the PVoice in `value` gets offset 0 and goes to the lame list (never posted) |
| 1 | SYN_SET_SOURCE | every filter | `filter.source = param`; `alLoadParam` ignores it |
| 2 | SYN_ADD_SOURCE | buses | `sources[sourceCount++] = param` |
| 3 | SYN_ADD_UPDATE | envelope mixer | appends the ALParam to the list |
| 4 | SYN_RESET | envelope mixer, resampler, load filter | stops the voice and rewinds (section 9.4) |
| 5 | SYN_SET_WAVETABLE | load filter | the wave to play (section 12.1) |
| 6 | SYN_SET_DRAM_OUT | save filter | `dramOut = param` |
| 7 | SYN_SET_PITCH | resampler | `ratio` = the `f32` whose bits are in `param` |
| 8 | SYN_SET_UNITY_PITCH | resampler | `unityPitch = 1` |
| 9 | SYN_START | envelope mixer, resampler | `playing = 1`; the load filter ignores it |
| 10 | | | not used |
| 11 | SYN_SET_VOLUME | ALParam | volume and ramp |
| 12 | SYN_SET_PAN | ALParam | pan |
| 13 | SYN_START_VOICE_PARAMS | ALParam | start with pitch, volume, pan and fx mix (never posted) |
| 14 | SYN_START_VOICE | ALParam | start the wave in `value` |
| 15 | SYN_STOP_VOICE | ALParam | stop |
| 16 | SYN_SET_FX_MIX | ALParam | reverb send |

The envelope mixer and the resampler pass every id they do not handle on to their source. So a `SYN_SET_PITCH`
change sent to the envelope mixer reaches the resampler, and `SYN_SET_WAVETABLE` reaches the load filter.

Filter types (`ALFilter.type`, never read): 0 load, 1 resampler, 3 save, 4 envelope mixer, 5 reverb, 6 aux bus, 7
main bus.

`alFxParamHdl`'s ids: `2 + 8 * section + field`, field 0 input, 1 output, 2 fbcoef, 3 ffcoef, 4 gain, 5 chorus
rate, 6 chorus depth, 7 low-pass coefficient (section 14.7).

## 7. Set up

libmus builds the synthesizer with `CustomInit` and `CustomSynNew` (map section 10, `mus-reverb.c`). Its order is
the order of `alSynNew` in the ROM, with the reverb made by `CustomAllocFX` instead of `alSynAllocFX`. The
constructors it calls are the synthesizer's:

- **alFilterNew(f, handler, setParam, type)** (801CA040): `source = NULL`, `handler`, `setParam`, `inp = outp =
  0`, `type`.
- **alSaveNew(f)** (801C8698): `alFilterNew(f, alSavePull, alSaveParam, 3)`; `dramOut = 0`; `first = 1`.
- **alAuxBusNew(m, sources, max)** (801C85E0): `alFilterNew(m, alAuxBusPull, alAuxBusParam, 6)`; `sourceCount = 0`;
  `maxSources = max`; `sources = sources`.
- **alMainBusNew(m, sources, max)** (801C863C): the same with `alMainBusPull`, `alMainBusParam`, type 7.
- **alLoadNew(f, dmaNew, hp)** (801C84A4): `alFilterNew(f, alAdpcmPull, alLoadParam, 0)`; `state` = 0x20 bytes from
  the heap; `loopState` = 0x20 bytes; `dma = dmaNew(&f->dmaState)`; `framePos = 0`; `first = 1`; `dataAddr = 0`.
- **alResampleNew(r, hp)** (801C8554): `alFilterNew(r, alResamplePull, alResampleParam, 1)`; `state` = 0x20 bytes;
  `delta = 0.0`; `first = 1`; `playing = 0`; `unityPitch = 0`; `unused28 = unused2C = 0`; `ratio = 1.0f`.
- **alEnvmixerNew(e, hp)** (801C83EC): `alFilterNew(e, alEnvmixerPull, alEnvmixerParam, 4)`; `state` = 0x50 bytes;
  `first = 1`; `playing = 0`; `volume = 1`; `targetLeft = targetRight = 1`; `volLeft = volRight = 1`;
  `dryAmount = wetAmount = 0`; `rateLeft = 1`; `rateLeftFrac = 0`; `rampPos = rampLength = 0`; `pan = 0`;
  `updates = lastUpdate = NULL`; `unused44 = 0`. The ROM writes `rateLeft` and `rateLeftFrac` twice and never
  `rateRight` or `rateRightFrac`, which keep the heap's zeros. The first sub-frame computes all four rates, so
  this has no effect.

The heap is zeroed by `MusInitialize` before any of this. Each allocation is rounded up to 16 bytes.

**alHeapInit(hp, base, len)** (801CB240): `base` rounded up to 16 (if `base & 0xF` is not 0, `base + 16 - (base &
0xF)`); `cur = base`; `len = len`, not reduced by the rounding; `count = 0`.

**alHeapDBAlloc(file, line, hp, num, size)** (801CB280): `bytes = (num * size + 15) & ~15`. If
`hp->base + hp->len < hp->cur + bytes` (an unsigned compare), returns NULL. Else returns `cur` and adds `bytes` to
it. `file` and `line` are not used.

**alSynNew(s, c)** (801CB310, unused): `head = NULL`; `curSamples = paramSamples = 0`; `numPVoices =
c->maxPVoices`; `maxOutSamples = 160`; `outputRate = c->outputRate`; `dma = c->dmaproc`. Then, from `c->heap`:
the save filter, one aux bus (`maxAuxBusses = 1`) with `maxPVoices` source slots, the main bus with `maxPVoices`
slots. If `c->fxType != 0`: `alSynAllocFX(s, 0, c, heap)`; else the aux bus becomes the main bus's source. The three
voice lists are cleared. `maxPVoices` PVoices: each is linked into the free list, gets `vvoice = NULL`, a load
filter (`alLoadNew(..., s->dma, heap)`, then `alLoadParam(..., SYN_SET_SOURCE, NULL)`), a resampler (source: the load
filter), an envelope mixer (source: the resampler), is added to the aux bus, and `channelKnob = &envmixer`. Then
`alSaveParam(save, SYN_SET_SOURCE, mainBus)`, `maxUpdates` ALParams pushed on `paramList`, and `heap = heap`.
`CustomSynNew` is the same.

**alSynAllocFX(s, bus, c, hp)** (801CBF70, unused): `fx = &s->auxBus[bus].fx[0]`; `alFxNew(fx, c, hp)`;
`alFxParam(fx, SYN_SET_SOURCE, &s->auxBus[bus])`; `alMainBusParam(s->mainBus, SYN_ADD_SOURCE, fx)`; returns `fx`.

**alInit(g, c)** (801CB190, unused): if `alGlobals == NULL`: `alGlobals = g`; `alSynNew(&g->drvr, c)`.

**alClose(g)** (801CB1BC): if `alGlobals != NULL`: `alSynDelete(&g->drvr)`; `alGlobals = NULL`. The ROM's audio
thread calls it when it ends.

**alSynDelete(s)** (801CB9A0): `s->head = NULL`. Nothing else.

**alSynAddPlayer(s, client)** (801CB9B0): with interrupts off (`osSetIntMask(OS_IM_NONE)`): `client->samplesLeft =
s->curSamples`; `client->next = s->head`; `s->head = client`. In the port the caller holds `musLock`.

**alFxNew(r, c, hp)** (801C7F54, unused) is section 14.6.

## 8. The frame: alAudioFrame

**alAudioFrame(cmdList, cmdLen, outBuf, outLen)** (801CB5DC) makes `outLen` stereo samples.

```c
ALSynth *s = &alGlobals->drvr;
Acmd *cmdlEnd = cmdList;
ALPlayer *client;
s16 outp = 0; /* given to the save filter; no filter uses it */
s32 n;

if (s->head == NULL) {
    *cmdLen = 0;
    return cmdList;
}

/* Run every player whose call time falls before the end of this frame. */
s->paramSamples = synNextPlayer(s, &client);
while (s->paramSamples - s->curSamples < outLen) {
    s->paramSamples &= ~0xF;
    client->samplesLeft += synTimeToSamplesExact(s, client->handler(client));
    s->paramSamples = synNextPlayer(s, &client);
}
s->paramSamples &= ~0xF;

/* Make the frame in chunks of at most maxOutSamples. */
for (; outLen > 0; outLen -= n) {
    n = outLen < s->maxOutSamples ? outLen : s->maxOutSamples;
    cmdlEnd->words.w0 = A_SEGMENT << 24; /* aSegment(0, 0) */
    cmdlEnd->words.w1 = 0;
    s->outputFilter->setParam(s->outputFilter, SYN_SET_DRAM_OUT, outBuf);
    cmdlEnd = s->outputFilter->handler(s->outputFilter, &outp, n, s->curSamples, cmdlEnd + 1);
    outBuf += n << 1;
    s->curSamples += n;
}
*cmdLen = cmdlEnd - cmdList; /* in commands */
_collectPVoices(s);
return cmdlEnd;
```

The port's `alAudioFrame` then runs the list (section 15.4).

**synNextPlayer(s, &client)** (801CB8F8): asserts that `s->head` is not NULL (`__assert`, "synthesizer.c" line
307; `__assert` does nothing in this ROM). Sets `*client = NULL` and `delta = 0x7FFFFFFF`. For each player in the
list, from the head: if `cl->samplesLeft - s->curSamples < delta`, then `*client = cl` and `delta = cl->samplesLeft
- s->curSamples`. Returns `(*client)->samplesLeft`. Ties go to the first player in the list. Glover has one player.

**Time to samples.** Both functions compute

`samples = (s32)(f32)((f64)((f32)micros * (f32)s->outputRate) / 1000000.0 + 0.5)`

with each conversion as written: the product in single precision, the division and the addition in double
precision, then single precision, then truncation.

- **synTimeToSamplesExact(s, micros)** (801CB864) returns `samples`. It sets the player's next call time.
- **_timeToSamples(s, micros)** (801CB8AC) returns `samples & ~0xF`. It gives the ramp length of `alSynSetVol`.

For libmus's period of 16666 microseconds at 22047 Hz: `16666.0f * 22047.0f` = 367435296 (rounded to single
precision), `/ 1000000.0 + 0.5` = 367.935296, so 367 samples between player calls and ramps of 352 samples.

**The player clock.** `samplesLeft` starts at `curSamples` when `alSynAddPlayer` runs (0 in Glover). Each call adds
367. The call times are 0, 367, 734 and so on. The changes made by a call get `paramSamples` = the call time rounded
down to a multiple of 16: 0, 352, 720 and so on. All handler calls of a frame happen before any command of that frame
is made. A frame of 1104 samples calls the handler three or four times.

**Frame sizes.** The ROM's glue always passes a multiple of 16. `mus-glue.c` always passes 1104, the ROM's usual
frame. Then `curSamples` is a
multiple of 16 at the start of each frame and `paramSamples` never falls before it. With other sizes a change could
get a time before its chunk, and the envelope mixer would pull a negative sample count. The chunk boundaries are
`curSamples` at the frame start plus multiples of 160. They change the command list and, through the resampler's
`f32` arithmetic, can change the samples slightly. A fixed frame size gives repeatable output.

**The pull chain of one chunk** (Glover's graph):

```
A_SEGMENT
alSavePull
  alMainBusPull: A_CLEARBUFF main L, main R
    alFxPull (the main bus's only source)
      alAuxBusPull: A_CLEARBUFF aux L, aux R
        alEnvmixerPull, for each of the 24 voices in order
          apply each due change; before most, synPullSubFrame for the samples up to it
          synPullSubFrame for the rest of the chunk
            alResamplePull
              alAdpcmPull (synDecodeChunk)
            A_SETBUFF, A_SETBUFF (aux), [5 x A_SETVOL], A_ENVMIXER
      the reverb's commands (section 14)
    A_SETBUFF, A_MIXER aux L into main L, A_MIXER aux R into main R
  A_SETBUFF, A_INTERLEAVE, A_SETBUFF, A_SAVEBUFF to outBuf
```

**_collectPVoices(s)** (801CB7D0): moves every PVoice of `pLameList` to `pFreeList` (`alUnlink`, then `alLink`
after the list head). In Glover the lame list is always empty.

**alLink(element, after)** (801CB1EC): `element->next = after->next`; `element->prev = after`; if `after->next`
is not NULL, `after->next->prev = element`; `after->next = element`. **alUnlink(element)** (801CB20C): if
`element->next`, `element->next->prev = element->prev`; if `element->prev`, `element->prev->next =
element->next`. The element's own links are not cleared. **alCopy(src, dest, len)** (801CB2D0): copies `len` bytes,
first byte first.

## 9. Voices and parameter changes

### 9.1 Allocating a voice

**alSynAllocVoice(s, voice, vc)** (801CBA00). libmus calls it once per channel in `MusInitialize`.

```c
PVoice *pvoice = NULL;
ALFilter *f;
ALParam *update;
s32 stolen;

voice->priority = vc->priority;
voice->table = NULL;
voice->unityPitch = vc->unityPitch;
voice->state = 0;
voice->pvoice = NULL;
voice->fxBus = vc->fxBus;
stolen = _allocatePVoice(s, &pvoice, vc->priority);
if (pvoice != NULL) {
    f = pvoice->channelKnob;
    if (stolen) {
        pvoice->offset = SYN_STEAL_DELAY;          /* 512 */
        pvoice->vvoice->pvoice = NULL;
        update = __allocParam();                   /* not checked for NULL */
        update->time = s->paramSamples;
        update->type = SYN_SET_VOLUME;
        update->value.i = 0;
        update->rampSamples = pvoice->offset - SYN_STEAL_FADE_MARGIN; /* 448 */
        f->setParam(f, SYN_ADD_UPDATE, update);
        update = __allocParam();
        if (update != NULL) {
            update->time = s->paramSamples + pvoice->offset;
            update->type = SYN_STOP_VOICE;
            update->next = NULL;
            f->setParam(f, SYN_ADD_UPDATE, update);
        }
    } else {
        pvoice->offset = 0;
    }
    pvoice->vvoice = voice;
    voice->pvoice = pvoice;
}
return pvoice != NULL;
```

**_allocatePVoice(s, &pvoice, priority)** (801CBB28) returns 1 when it stole a voice:

1. If `pLameList.next` is not NULL, take it.
2. Else if `pFreeList.next` is not NULL, take it.
3. In both cases: `*pvoice` = it; `alUnlink` it; `alLink` it after `pAllocList`; return 0.
4. Else walk `pAllocList`. For each voice with `vvoice->priority <= priority` and `offset == 0`: `*pvoice` = it,
   `priority = vvoice->priority`, stolen = 1. The last voice that passes wins; the priority test uses the lowest
   priority found so far. The stolen voice stays in the allocated list. Return stolen.

Glover has 24 physical voices for 24 voices, and never frees one, so no voice is ever stolen and every `offset` is 0.

### 9.2 The change API

Each function does nothing when `voice->pvoice` is NULL or when `__allocParam` returns NULL. Otherwise it fills an
ALParam with `time = s->paramSamples + voice->pvoice->offset` and `next = NULL`, and passes it to
`channelKnob->setParam(channelKnob, SYN_ADD_UPDATE, update)`.

| Function | ROM | type | value | Other fields |
| --- | --- | --- | --- | --- |
| alSynStartVoice(s, voice, table) | 801CBC60 | SYN_START_VOICE | `table` | `unityPitch = voice->unityPitch` |
| alSynStopVoice(s, voice) | 801CBBE0 | SYN_STOP_VOICE | not set | |
| alSynSetPitch(s, voice, ratio) | 801CBD00 | SYN_SET_PITCH | `value.f = ratio` | |
| alSynSetVol(s, voice, vol, t) | 801CBD90 | SYN_SET_VOLUME | `value.i = (s16)vol` | `rampSamples = _timeToSamples(s, t)` |
| alSynSetPan(s, voice, pan) | 801CBEE0 | SYN_SET_PAN | `value.i = (u8)pan` | |
| alSynSetFXMix(s, voice, fxmix) | 801CBE50 | SYN_SET_FX_MIX | `value.i = (u8)fxmix` | |

`alSynStartVoice` does not set `voice->table`. libmus's ramps are always `musFrameMicros` (16666), so every
`SYN_SET_VOLUME` has `rampSamples` 352.

### 9.3 How the envelope mixer applies changes

**alEnvmixerParam(filter, id, param)** (801C984C):

- `SYN_ADD_UPDATE`: if `lastUpdate` is not NULL, `lastUpdate->next = param`; else `updates = param`. Then
  `lastUpdate = param`. The list stays in the order of posting. Its times are in order too, because
  `paramSamples` never decreases and every offset is 0.
- `SYN_RESET`: `first = 1`; `playing = 0`; `volume = 1`; passes `SYN_RESET` to the source.
- `SYN_START`: `playing = 1`; passes `SYN_START` to the source.
- `SYN_SET_SOURCE`: `filter.source = param`.
- any other id: passed to the source unchanged, if there is one.

**alEnvmixerPull(filter, outp, outCount, sampleOffset, p)** (801C9300). `outp` is not used. Two local DMEM offsets
start at 0: `inp` (where the resampler writes, advanced by `synPullSubFrame`) and `outp` (where the mix goes,
advanced here).

```c
s16 inp = 0, outp = 0;
s32 lastOffset = sampleOffset, thisOffset, samples;
ALParam *u;

while (e->updates != NULL) {
    thisOffset = e->updates->time;
    samples = thisOffset - lastOffset;
    lastOffset = thisOffset;
    if (samples > outCount) {
        break;                       /* the change belongs to a later chunk */
    }
    /* ROM: __assert(samples >= 0) and __assert(samples <= 160), env.c lines 104 and 105 */
    u = e->updates;
    switch (u->type) {
    case SYN_START_VOICE_PARAMS:     /* never posted; see below */
        ...
        break;
    case SYN_START_VOICE:            /* no pull before it */
        if (u->unityPitch) {
            e->filter.setParam(e, SYN_SET_UNITY_PITCH, NULL);
        }
        e->filter.setParam(e, SYN_SET_WAVETABLE, u->value.p);
        e->filter.setParam(e, SYN_START, NULL);
        break;
    case SYN_STOP_VOICE:
        p = synPullSubFrame(e, &inp, &outp, samples, sampleOffset, p);
        e->filter.setParam(e, SYN_RESET, NULL);
        break;
    case SYN_FREE_VOICE:             /* never posted */
        ((PVoice *)u->value.p)->offset = 0;
        _freePVoice(&alGlobals->drvr, u->value.p);
        break;
    case SYN_SET_VOLUME:
    case SYN_SET_PAN:
    case SYN_SET_FX_MIX:
        p = synPullSubFrame(e, &inp, &outp, samples, sampleOffset, p);
        if (e->rampPos >= e->rampLength) {
            /* The ramp has ended: the volumes are the targets. */
            e->targetLeft = (e->volume * synEqPower[e->pan]) >> 15;
            e->targetRight = (e->volume * synEqPower[127 - e->pan]) >> 15;
            e->rampPos = e->rampLength;
            e->volLeft = e->targetLeft;
            e->volRight = e->targetRight;
        } else {
            /* The ramp is still running: estimate where it is. */
            e->volLeft = (s16)synGetVol((f32)e->volLeft, e->rampPos, e->rateLeft, e->rateLeftFrac);
            e->volRight = (s16)synGetVol((f32)e->volRight, e->rampPos, e->rateRight, e->rateRightFrac);
        }
        if (e->volLeft == 0) e->volLeft = 1;
        if (e->volRight == 0) e->volRight = 1;
        if (u->type == SYN_SET_PAN) {
            e->pan = (s16)u->value.i;
        }
        if (u->type == SYN_SET_VOLUME) {
            e->rampPos = 0;
            e->volume = (u->value.i * u->value.i) >> 15;
            e->rampLength = u->rampSamples;
        }
        if (u->type == SYN_SET_FX_MIX) {
            e->dryAmount = synEqPower[u->value.i];
            e->wetAmount = synEqPower[127 - u->value.i];
        }
        e->first = 1;
        break;
    default:                         /* SYN_SET_PITCH in Glover; any id 1 to 10 or above 16 */
        p = synPullSubFrame(e, &inp, &outp, samples, sampleOffset, p);
        e->filter.setParam(e, u->type, u->value.p);
        break;
    }
    outCount -= samples;
    outp += samples << 1;
    e->updates = u->next;
    if (e->updates == NULL) {
        e->lastUpdate = NULL;
    }
    __freeParam(u);
}
p = synPullSubFrame(e, &inp, &outp, outCount, sampleOffset, p);
if (e->rampPos > e->rampLength) {
    e->rampPos = e->rampLength;
}
return p;
```

`sampleOffset` passed to `synPullSubFrame` is always the chunk's start; it is only passed on and never used.

`SYN_START_VOICE_PARAMS` (for completeness): no pull; `unityPitch` and `SYN_SET_WAVETABLE` (with `wave`) and
`SYN_START` as above; `first = 1`; `rampPos = 0`; `rampLength = rampSamples`; `volume = (volume * volume) >> 15`;
`pan = pan`; `dryAmount = synEqPower[fxMix]`; `wetAmount = synEqPower[127 - fxMix]`; if `rampSamples` is not 0,
`volLeft = volRight = 1`; else `volLeft = (volume * synEqPower[pan]) >> 15` and `volRight = (volume *
synEqPower[127 - pan]) >> 15`; then, if the source is not NULL, it gets `SYN_SET_PITCH` with `pitch`.

### 9.4 What a stop and a start keep

`alSynStopVoice` queues `SYN_STOP_VOICE`. When it is due, the mixer pulls the samples up to it and sends `SYN_RESET`
down the voice:

| Filter | Reset by `SYN_RESET` | Kept |
| --- | --- | --- |
| envelope mixer | `first = 1`, `playing = 0`, `volume = 1` | pan, dry and wet amounts, `volLeft`, `volRight`, targets, rates, ramp position and length |
| resampler | `delta = 0`, `first = 1`, `playing = 0`, `unityPitch = 0` | `ratio` |
| load filter | `framePos = 0`, `first = 1`, `sample = 0`; if a wave is set: `dataAddr = base`, and `loop.count` from the wave's loop | `table`, `loop.start`, `loop.end`, the loop state |

While `playing` is 0, `synPullSubFrame` makes no commands, so the voice is silent and costs nothing.

`alSynStartVoice` queues `SYN_START_VOICE`. The mixer does not pull the samples before it: if the voice was still
playing, its samples between the previous change and the start are lost. libmus always stops a playing voice first
(`musVoiceStart`), at the same time, so this never happens.

Note on `volume`: after a stop, `volume` is 1, so the targets of the next ramp are 0 until a `SYN_SET_VOLUME`
arrives. libmus usually sends one: before it starts a new wave on a sounding voice, it fades the voice to 0 and
sets its record of the last volume sent to 0 (`musChannelNextEvent`).

### 9.5 _freePVoice

**_freePVoice(s, pvoice)** (801CB824): `alUnlink(pvoice)`; `alLink(pvoice, &s->pLameList)`. Only the
`SYN_FREE_VOICE` change calls it.

## 10. Envelope mixer: volume, pan and the reverb send

### 10.1 The equal-power table

`synEqPower` (ROM `D_801F45B0`) has 128 `s16`. Entry `i` is `round(32767 * cos(i * pi / 254))`: 32767 at 0, 0 at
127. The formula gives all 128 entries of the ROM's table (each value is at least 0.002 from a rounding boundary).
The port reads the ROM's own copy in `port_ram` (`gen/ramtab.c` swaps it to host order). The 16 halfwords after
the table in the ROM (801F46B0) are unrelated data.

- Pan `p` (0 = left): left gain `synEqPower[p]`, right gain `synEqPower[127 - p]`.
- Reverb send `m`: dry gain `synEqPower[m]`, wet gain `synEqPower[127 - m]`.

libmus masks the pan to 0..127 and its reverb mix `handleReverb + (((128 - handleReverb) * reverb) >> 7)` stays in
0..127, because `handleReverb` is clamped to 0..127 and every song and effect uses reverb values 0..127. So no index
goes past the table in Glover. The ROM does not check.

### 10.2 Volume

libmus sends `vol` in 0..0x7FFF. The mixer squares it: `volume = (vol * vol) >> 15`. The targets of a ramp are

- `targetLeft = (volume * synEqPower[pan]) >> 15`
- `targetRight = (volume * synEqPower[127 - pan]) >> 15`

All products are 32-bit signed; the results are stored as `s16`.

### 10.3 One sub-frame: synPullSubFrame

**synPullSubFrame(e, &inp, &outp, outCount, sampleOffset, p)** (801C9920):

```c
ALFilter *source = e->filter.source;

if (e->playing != 1 || outCount == 0) {
    return p;
}
/* ROM: __assert(source), env.c line 367 */
p = source->handler(source, inp, outCount, sampleOffset, p); /* the resampler writes at *inp */

aSetBuffer: w0 = 0x08000000 | (u16)*inp
            w1 = ((*outp + 0x440) << 16) | ((outCount << 1) & 0xFFFF)          /* in, main L, count */
aSetBuffer: w0 = 0x08080000 | ((*outp + 0x580) & 0xFFFF)                      /* A_AUX: main R */
            w1 = ((*outp + 0x6C0) << 16) | ((*outp + 0x800) & 0xFFFF)         /* aux L, aux R */
if (e->first) {
    e->first = 0;
    e->targetLeft = (e->volume * synEqPower[e->pan]) >> 15;
    e->rateLeft = synGetRate((f64)e->volLeft, (f64)e->targetLeft, e->rampLength, &e->rateLeftFrac);
    e->targetRight = (e->volume * synEqPower[127 - e->pan]) >> 15;
    e->rateRight = synGetRate((f64)e->volRight, (f64)e->targetRight, e->rampLength, &e->rateRightFrac);
    aSetVolume A_VOL|A_LEFT:   w0 = 0x09060000 | (u16)e->volLeft,     w1 = 0
    aSetVolume A_VOL|A_RIGHT:  w0 = 0x09040000 | (u16)e->volRight,    w1 = 0
    aSetVolume A_RATE|A_LEFT:  w0 = 0x09020000 | (u16)e->targetLeft,  w1 = ((u16)e->rateLeft << 16) | e->rateLeftFrac
    aSetVolume A_RATE|A_RIGHT: w0 = 0x09000000 | (u16)e->targetRight, w1 = ((u16)e->rateRight << 16) | e->rateRightFrac
    aSetVolume A_AUX:          w0 = 0x09080000 | (u16)e->dryAmount,   w1 = (u16)e->wetAmount
    aEnvMixer A_INIT|A_AUX:    w0 = 0x03090000, w1 = e->state
} else {
    aEnvMixer A_AUX:           w0 = 0x03080000, w1 = e->state
}
*inp += outCount << 1;
e->rampPos += outCount;
return p;
```

The ramp always runs over `rampLength` samples from the current volumes, even when `first` was set by a pan or fx
change in the middle of a ramp. `rampPos` is not reset by those changes.

### 10.4 Ramp rates: synGetRate

**synGetRate(vol, tgt, count, &ratel)** (801C9D24) computes the ratio by which the RSP multiplies the volume every 8
samples: over `count` samples it takes the volume from `vol` to `tgt`. It returns the integer part and stores the
fraction. Doubles throughout, in this order:

```c
f64 invn = 1.0 / (f64)count;   /* computed first, even when count is 0 */
f64 a, f, r, res;
s32 b, exp = 0;
s16 s;

if (count == 0) {
    if (tgt >= vol) { *ratel = 0xFFFF; return 0x7FFF; }
    *ratel = 0;
    return 0;
}
if (tgt < 1.0) tgt = 1.0;
if (vol <= 0.0) vol = 1.0;
a = tgt / vol;
b = (s32)(invn * 1073741824.0);         /* 2^30 / count */
if (a != 0.0) {                         /* frexp, inline: a = f * 2^exp, 0.5 <= f < 1 */
    f = a > 0.0 ? a : -a;
    if (f >= 1.0) do { f *= 0.5; exp++; } while (f >= 1.0);
    if (f < 0.5) do { f += f; exp--; } while (f < 0.5);
    a = a > 0.0 ? f : -f;
}
r = log2Table[(s32)(a * 16.0) - 8] + (f64)exp;   /* about log2(tgt / vol) */
r = r * 0.6931471805599453;                      /* times ln 2 */
r = r / 1073741824.0;
r = r + 1.0;                                     /* about (tgt / vol)^(1 / 2^30) */
res = 1.0;
if (b != 0) do { if (b & 1) res *= r; b >>= 1; r *= r; } while (b != 0);  /* r^b; b is shifted arithmetically */
res = res * res;
res = res * res;
res = res * res;                                 /* ratio per 8 samples */
s = (s16)(s32)res;
*ratel = (u16)(s32)((res - (f64)(f32)(s32)s) * 65535.0);
return s;
```

`log2Table` is the eight doubles of section 3. `a` is never 0 or negative here (`tgt >= 1`, `vol > 0`). Note the
integer part goes through `f32` before it is subtracted. Example: from 1 to 32767 over 352 samples gives 1 and 17408,
a ratio of 1.2656 per 8 samples against the exact 1.2666.

### 10.5 Where a running ramp is: synGetVol

**synGetVol(ivol, samples, ratem, ratel)** (801C9FA4), single precision:

```c
f32 r, a = 1.0f;
s32 i;

samples >>= 3;                  /* arithmetic shift: 8-sample steps */
if (samples == 0) return ivol;
r = ((f32)(ratem << 16) + (f32)(s32)ratel) / 65536.0f;
for (i = 0; i < 32; i++) {
    if (samples & 1) a *= r;
    samples >>= 1;
    if (samples == 0) return ivol * a;
    r *= r;
}
return ivol * a;
```

`ratem << 16` is done on the sign-extended `s16`, as an `s32`. The sum is rounded to `f32` before the division.

### 10.6 The RSP's part

The RSP starts the ramp at `volLeft` and `volRight` (A_VOL), multiplies by the rate every 8 samples, and stops at the
target (A_RATE). It mixes `in * left` into main L and `in * right` into main R, scaled by the dry amount, and into aux
L and aux R scaled by the wet amount (A_AUX). `A_INIT` starts a new envelope from the A_SETVOL values; without it the
envelope continues from `ENVMIX_STATE`. Section 15.4 gives the microcode's arithmetic. Each block of 8 samples
multiplies the volume by the ratio, the 8-sample geometric ramp that `synGetVol` models.

## 11. Resampler: pitch

**alResampleParam(filter, id, param)** (801CA3BC): `SYN_SET_SOURCE`: `source = param`. `SYN_RESET`: `delta = 0.0f`,
`first = 1`, `playing = 0`, `unityPitch = 0`, then `SYN_RESET` with NULL to the source. `SYN_START`: `playing = 1`,
then `SYN_START` with NULL to the source. `SYN_SET_PITCH`: `ratio` = the bits of `param` as an `f32`.
`SYN_SET_UNITY_PITCH`: `unityPitch = 1`. Any other id goes to the source unchanged.

In C, move the pitch through the union: `SynValue v; v.p = param; r->ratio = v.f;`. The envelope mixer passes
`u->value.p`, so the bits arrive unchanged.

**alResamplePull(filter, outp, outCount, sampleOffset, p)** (801CA1D0):

```c
s16 inp = SYN_DMEM_DECODER_OUT;    /* 0x140; the load filter moves it */
s32 incount;
f32 finCount;

if (outCount == 0) return p;
if (r->unityPitch) {
    p = source->handler(source, &inp, outCount, sampleOffset, p);
    aDMEMMove: w0 = 0x0A000000 | (inp & 0xFFFFFF), w1 = (*outp << 16) | ((outCount << 1) & 0xFFFF)
} else {
    if ((f64)r->ratio > 1.99996) r->ratio = 1.99996f;          /* 0x3FFFFEB0 */
    r->ratio = (f32)(s32)(r->ratio * 32768.0f) / 32768.0f;     /* steps of 1/32768, truncated */
    finCount = r->delta + r->ratio * (f32)outCount;
    incount = (s32)finCount;
    r->delta = finCount - (f32)incount;
    p = source->handler(source, &inp, incount, sampleOffset, p);
    aSetBuffer: w0 = 0x08000000 | (u16)inp, w1 = (*outp << 16) | ((outCount << 1) & 0xFFFF)
    aResample:  w0 = 0x05000000 | ((u8)r->first << 16) | ((s32)(r->ratio * 32768.0f) & 0xFFFF), w1 = r->state
    r->first = 0;
}
return p;
```

libmus clamps its ratio to 2.0 (map section 8.5). The resampler then clamps 2.0 to 1.99996f and quantizes it to
65534 / 32768. The pitch field of `A_RESAMPLE` is `ratio * 32768`, so at most 0xFFFE. Glover's voices never have
unity pitch (libmus passes `unityPitch` 0).

## 12. Load filter: ADPCM and raw waves

### 12.1 alLoadParam

**alLoadParam(filter, id, param)** (801C8EC0) handles two ids and ignores the rest:

- `SYN_SET_WAVETABLE`: `table = param`; `dataAddr = (s32)table->base`; `sample = 0`. For `AL_ADPCM_WAVE`:
  `handler = alAdpcmPull`; `table->len = 9 * (table->len / 9)` (it writes to the bank; signed division);
  `bookSize = book->order * book->npredictors * 16`; if the wave has a loop: `loop.start`, `loop.end`,
  `loop.count` from it and `alCopy(adpcmLoop->state, loopState, 32)`; else all three are 0. For
  `AL_RAW16_WAVE`: `handler = alRaw16Pull`; the loop as above, without a state. Other types: nothing more.
  `framePos` and `first` are not changed: the reset before each start set them.
- `SYN_RESET`: `framePos = 0`; `first = 1`; `sample = 0`; if `table` is not NULL: `dataAddr = (s32)table->base`, and
  for either wave type with a loop, `loop.count = loop->count`.

### 12.2 alAdpcmPull

An ADPCM frame is 9 bytes for 16 samples. **alAdpcmPull(filter, outp, outCount, sampleOffset, p)** (801C86E0)
makes `outCount` decoded samples at DMEM `*outp`, and moves `*outp` to the first of them.

```c
s32 nSam, nLeft, tsam, nframes, nbytes, nover, overFlow, startZero, op, dramAlign, decoded = 0, looped;

if (outCount == 0) return p;

aLoadADPCM: w0 = 0x0B000000 | (f->bookSize & 0xFFFFFF), w1 = f->table->waveInfo.adpcmWave.book->book

looped = (u32)(outCount + f->sample) > f->loop.end && f->loop.count != 0;
nSam = looped ? f->loop.end - f->sample : outCount;
nLeft = f->framePos ? 16 - f->framePos : 0;     /* samples still waiting in the last frame */
tsam = nSam - nLeft; if (tsam < 0) tsam = 0;
nframes = (tsam + 15) >> 4;
nbytes = nframes * 9;

if (looped) {
    p = synDecodeChunk(p, f, tsam, nbytes, *outp, 0, f->first);
    *outp += f->framePos ? f->framePos << 1 : 32;
    f->framePos = f->loop.start & 0xF;
    f->sample = f->loop.start;
    f->dataAddr = (s32)f->table->base + 9 * ((f->loop.start >> 4) + 1);
    op = *outp;
    while (nSam < outCount) {
        dramAlign = (op + ((nframes + 1) << 5)) & ~0x1F;   /* past the last decode, 32-byte aligned */
        op += nSam << 1;
        if (f->loop.count != (u32)-1 && f->loop.count != 0) f->loop.count--;
        outCount -= nSam;
        nSam = (u32)outCount < f->loop.end - f->loop.start ? outCount : f->loop.end - f->loop.start;
        tsam = nSam - 16 + f->framePos; if (tsam < 0) tsam = 0;
        nframes = (tsam + 15) >> 4;
        nbytes = nframes * 9;
        p = synDecodeChunk(p, f, tsam, nbytes, (s16)dramAlign, 0, f->first | A_LOOP);
        aDMEMMove: w0 = 0x0A000000 | ((dramAlign + (f->framePos << 1)) & 0xFFFFFF)
                   w1 = (op << 16) | ((nSam << 1) & 0xFFFF)
    }
    f->sample += outCount;
    f->framePos = (outCount + f->framePos) & 0xF;
    f->dataAddr += 9 * nframes;
    return p;
}

nover = f->dataAddr + nbytes - ((s32)f->table->base + f->table->len);  /* bytes past the wave's end */
if (nover < 0) nover = 0;
overFlow = (nover / 9) << 4;                                           /* in samples */
if (overFlow > (nframes << 4) + nLeft) overFlow = (nframes << 4) + nLeft;
nbytes -= nover;
if (overFlow - (overFlow & 0xF) < outCount) {
    decoded = 1;
    p = synDecodeChunk(p, f, (nframes << 4) - overFlow, nbytes, *outp, 0, f->first);
    *outp += f->framePos ? f->framePos << 1 : 32;
    f->sample += outCount;
    f->framePos = (outCount + f->framePos) & 0xF;
    f->dataAddr += 9 * nframes;
} else {
    f->framePos = 0;
    f->dataAddr += 9 * nframes;
}
if (overFlow != 0) {
    f->framePos = 0;
    startZero = decoded ? (nLeft + (nframes << 4) - overFlow) << 1 : 0;
    aClearBuffer: w0 = 0x02000000 | ((startZero + *outp) & 0xFFFFFF), w1 = overFlow << 1
}
return p;
```

What the arithmetic assumes of `A_ADPCM`: the output at `outp` starts with the 16 samples of the last frame of the
previous decode (from the state), then the new frames. That is why `*outp` moves by `framePos * 2` (the samples of
that frame already used) or by 32 when the whole frame was used.

**The loop.** When the wave reaches `loop.end` inside this pull, the first decode stops there. Each pass of the loop
then decodes from the frame after the one that holds `loop.start`, with `A_LOOP`: the decoder starts from the loop
state (`A_SETLOOP`) instead of its own state. The loop state stands for the frame that holds `loop.start`, so the
first 16 samples of the pass's output are that frame. The pass writes past the last decode at a 32-byte boundary.
`A_DMEMMOVE` then copies its samples from `loop.start & 15` samples into that first frame, which is `loop.start`
itself, to the end of the samples so far. `dataAddr` stays at the loop's frame during the passes and advances once at
the end. A loop count of -1 never counts down. In Glover every loop has count -1, and 48 loops start inside a frame.

**The end of a wave.** Without a loop, the frames past `table->base + table->len` are not read. Their samples become
zeros (`A_CLEARBUFF`), and `framePos` returns to 0. The voice keeps playing silence until libmus stops it.

### 12.3 synDecodeChunk

**synDecodeChunk(p, f, tsam, nbytes, outp, inp, flags)** (801C906C), the only helper of `alAdpcmPull`. `outp` and
`inp` are DMEM offsets (`s16`; the ROM reads `outp` as a `u16`).

```c
s32 dramLoc, dramAlign;

if (nbytes > 0) {
    dramLoc = f->dma(f->dataAddr, nbytes, f->dmaState);
    dramAlign = dramLoc & 7;
    nbytes += dramAlign;
    aSetBuffer: w0 = 0x08000000 | (inp & 0xFFFF), w1 = (nbytes + 8 - (nbytes & 7)) & 0xFFFF
    aLoadBuffer: w0 = 0x04000000, w1 = dramLoc - dramAlign
} else {
    dramAlign = 0;
}
if (flags & A_LOOP) {
    aSetLoop: w0 = 0x0F000000, w1 = f->loopState
}
aSetBuffer: w0 = 0x08000000 | ((inp + dramAlign) & 0xFFFF), w1 = ((u16)outp << 16) | ((tsam << 1) & 0xFFFF)
aADPCMdec:  w0 = 0x01000000 | ((flags & 0xFF) << 16), w1 = f->state
f->first = 0;
return p;
```

The load size `nbytes + 8 - (nbytes & 7)` adds 8 even when `nbytes` is a multiple of 8. `inp` is always 0
(`SYN_DMEM_DECODER_IN`). When `nbytes` is 0 the chunk still decodes with count `tsam * 2`, which can be 0.

### 12.4 alRaw16Pull (not used by Glover)

**alRaw16Pull** (801C8B24) loads 16-bit samples directly. The same loop logic, with 2 bytes per sample:

```c
s32 nSam, nbytes, dramLoc, dramAlign, dmemAlign, overFlow, startZero, op;

if (outCount == 0) return p;
if ((u32)(f->sample + outCount) > f->loop.end && f->loop.count != 0) {
    nSam = f->loop.end - f->sample;
    nbytes = nSam << 1;
    if (nSam > 0) {
        dramLoc = f->dma(f->dataAddr, nbytes, f->dmaState);
        dramAlign = dramLoc & 7;
        nbytes += dramAlign;
        aSetBuffer(0, *outp, 0, nbytes + 8 - (nbytes & 7)); aLoadBuffer(dramLoc - dramAlign);
    } else {
        dramAlign = 0;
    }
    *outp += dramAlign;
    f->sample = f->loop.start;
    f->dataAddr = (s32)f->table->base + (f->loop.start << 1);
    op = *outp;
    while (nSam < outCount) {
        op += nSam << 1;
        if (f->loop.count != (u32)-1 && f->loop.count != 0) f->loop.count--;
        outCount -= nSam;
        nSam = MIN((u32)outCount, f->loop.end - f->loop.start);
        nbytes = nSam << 1;
        dramLoc = f->dma(f->dataAddr, nbytes, f->dmaState);
        dramAlign = dramLoc & 7;
        nbytes += dramAlign;
        dmemAlign = (op & 7) ? 8 - (op & 7) : 0;
        aSetBuffer(0, op + dmemAlign, 0, nbytes + 8 - (nbytes & 7)); aLoadBuffer(dramLoc - dramAlign);
        if (dramAlign || dmemAlign) aDMEMMove(op + dramAlign + dmemAlign, op, nSam << 1);
    }
    f->sample += outCount;
    f->dataAddr += outCount << 1;
    return p;
}
nbytes = outCount << 1;
overFlow = f->dataAddr + nbytes - ((s32)f->table->base + f->table->len);
if (overFlow < 0) overFlow = 0;
if (overFlow > nbytes) overFlow = nbytes;
if (overFlow < nbytes) {
    nbytes -= overFlow;
    if (outCount > 0) {
        dramLoc = f->dma(f->dataAddr, nbytes, f->dmaState);
        dramAlign = dramLoc & 7;
        nbytes += dramAlign;
        aSetBuffer(0, *outp, 0, nbytes + 8 - (nbytes & 7)); aLoadBuffer(dramLoc - dramAlign);
    } else {
        dramAlign = 0;
    }
    *outp += dramAlign;
    f->sample += outCount;
    f->dataAddr += outCount << 1;
} else {
    f->dataAddr += outCount << 1;
}
if (overFlow != 0) {
    startZero = (outCount << 1) - overFlow; if (startZero < 0) startZero = 0;
    aClearBuffer(startZero + *outp, overFlow);
}
return p;
```

The `aSetBuffer` forms here have `in` in `w0` and `count` in `w1` (out 0), as in section 15.

## 13. Buses and the save filter

**alAuxBusParam** (801C92CC) and **alMainBusParam** (801CA1A0): for `SYN_ADD_SOURCE`, `sources[sourceCount++] =
param`; other ids are ignored. Neither checks `maxSources`.

**alAuxBusPull** (801C91F0):

```c
aClearBuffer: w0 = 0x020006C0, w1 = outCount << 1     /* aux L */
aClearBuffer: w0 = 0x02000800, w1 = outCount << 1     /* aux R */
for (i = 0; i < m->sourceCount; i++)
    p = m->sources[i]->handler(m->sources[i], outp, outCount, sampleOffset, p);
```

**alMainBusPull** (801CA060):

```c
aClearBuffer: w0 = 0x02000440, w1 = outCount << 1     /* main L */
aClearBuffer: w0 = 0x02000580, w1 = outCount << 1     /* main R */
for (i = 0; i < m->sourceCount; i++) {
    p = m->sources[i]->handler(m->sources[i], outp, outCount, sampleOffset, p);
    aSetBuffer: w0 = 0x08000000, w1 = (outCount << 1) & 0xFFFF
    aMix:       w0 = 0x0C007FFF, w1 = 0x06C00440        /* aux L into main L, gain 0x7FFF */
    aMix:       w0 = 0x0C007FFF, w1 = 0x08000580        /* aux R into main R */
}
```

In Glover the main bus has one source, the reverb, whose output is in both aux buffers.

**alSaveParam** (801CB160): `SYN_SET_SOURCE`: `source = param`; `SYN_SET_DRAM_OUT`: `dramOut = (s32)param`.

**alSavePull** (801CB060):

```c
/* ROM: __assert(source), save.c line 34 */
p = source->handler(source, outp, outCount, sampleOffset, p);
aSetBuffer:   w0 = 0x08000000, w1 = (outCount << 1) & 0xFFFF
aInterleave:  w0 = 0x0D000000, w1 = 0x04400580               /* main L, main R */
aSetBuffer:   w0 = 0x08000000, w1 = (outCount << 2) & 0xFFFF
aSaveBuffer:  w0 = 0x06000000, w1 = f->dramOut                /* not converted, unlike the other addresses */
```

## 14. Reverb

libmus loads the presets with its own `CustomFxSet` (map section 10). Glover uses the big room preset (4 sections,
delay line 4000 samples in a buffer of 8000) and never changes it. Only section 4 has a low-pass filter; no section
has a chorus.

### 14.1 alFxPull

**alFxPull(filter, outp, outCount, sampleOffset, p)** (801CA480):

```c
s16 i, buff1 = SYN_DMEM_FX_TEMP0, buff2 = SYN_DMEM_FX_TEMP1, t;   /* 0x140 and 0 */
s16 *in_ptr, *out_ptr, *prev_out_ptr = NULL;
ALDelay *d;

/* ROM: __assert(source), reverb.c line 75 */
p = source->handler(source, outp, outCount, sampleOffset, p);   /* the aux bus: every voice's wet mix */

/* Mono input: aux L = 0.7071 * (aux L + aux R). */
aSetBuffer: w0 = 0x08000000, w1 = (outCount << 1) & 0xFFFF
aMix:       w0 = 0x0C00DA83, w1 = 0x06C006C0     /* aux L += aux L * -9597 / 32768 */
aMix:       w0 = 0x0C005A82, w1 = 0x080006C0     /* aux L += aux R * 23170 / 32768 */
p = _saveBuffer(r, r->inputPos, SYN_DMEM_AUX_L, outCount, p);    /* into the delay line */
aClearBuffer: w0 = 0x02000800, w1 = outCount << 1                /* aux R collects the output */

for (i = 0; i < r->sectionCount; i++) {
    d = &r->sections[i];
    in_ptr = &r->inputPos[-d->input];
    out_ptr = &r->inputPos[-d->output];
    if (in_ptr == prev_out_ptr) {
        t = buff2; buff2 = buff1; buff1 = t;     /* reuse the last output as this input */
    } else {
        p = _loadBuffer(r, in_ptr, buff2, outCount, p);
    }
    p = _loadOutputBuffer(r, d, buff1, outCount, p);
    if (d->ffcoef) {
        aMix: w0 = 0x0C000000 | (u16)d->ffcoef, w1 = (buff2 << 16) | (u16)buff1
        if (!d->resampler && !d->lowpass) p = _saveBuffer(r, out_ptr, buff1, outCount, p);
    }
    if (d->fbcoef) {
        aMix: w0 = 0x0C000000 | (u16)d->fbcoef, w1 = (buff1 << 16) | (u16)buff2
        p = _saveBuffer(r, in_ptr, buff2, outCount, p);
    }
    if (d->lowpass) p = _filterBuffer(d->lowpass, buff1, outCount, p);
    if (!d->resampler) p = _saveBuffer(r, out_ptr, buff1, outCount, p);
    if (d->gain) {
        aMix: w0 = 0x0C000000 | (u16)d->gain, w1 = (buff1 << 16) | 0x800
    }
    prev_out_ptr = &r->inputPos[d->output];      /* + output, not - output: see below */
}
r->inputPos += outCount;
if ((u32)r->inputPos > (u32)&r->delayLine[r->length]) r->inputPos -= r->length;
aDMEMMove: w0 = 0x0A000800, w1 = 0x06C00000 | ((outCount << 1) & 0xFFFF)   /* aux R to aux L */
return p;
```

After the reverb both aux buffers hold its output (the move copies aux R to aux L), and the main bus adds them to
the dry mix.

Quirks to keep:

- `prev_out_ptr` is `inputPos + output`, but `in_ptr` is `inputPos - input`. They are equal only when both taps are
  0, so the buffer swap never happens in Glover. Keep the test and the swap.
- With `ffcoef`, no chorus and no low-pass, the output tap is saved twice with the same data.
- The wrap test is `>`, so `inputPos` can equal the end of the delay line. The next `_saveBuffer` then splits with
  0 samples before the end (section 14.2). This happens whenever the write position lands exactly on the end.
- The tap positions can lie before `delayLine` (as far as `length` before it). Compute them as 32-bit addresses
  (`u32`) and compare unsigned, as the ROM does (`sltu`). Pointer arithmetic outside an array is undefined in C.

### 14.2 The delay line: _loadBuffer and _saveBuffer

Both wrap a position below `delayLine` by adding `length` once, then split a transfer that crosses the end.

**_loadBuffer(r, curr_ptr, buff, count, p)** (801CAC4C), delay line to DMEM `buff`:

```c
s16 *delay_end = &r->delayLine[r->length];
s16 *updated_ptr;
s32 before_end, after_end;

if ((u32)curr_ptr < (u32)r->delayLine) curr_ptr += r->length;
updated_ptr = curr_ptr + count;
if ((u32)delay_end < (u32)updated_ptr) {
    before_end = delay_end - curr_ptr;         /* samples */
    after_end = updated_ptr - delay_end;
    aSetBuffer:  w0 = 0x08000000 | (u16)buff, w1 = (before_end << 1) & 0xFFFF
    aLoadBuffer: w0 = 0x04000000, w1 = curr_ptr
    aSetBuffer:  w0 = 0x08000000 | ((buff + (before_end << 1)) & 0xFFFF), w1 = (after_end << 1) & 0xFFFF
    aLoadBuffer: w0 = 0x04000000, w1 = r->delayLine
} else {
    aSetBuffer:  w0 = 0x08000000 | (u16)buff, w1 = (count << 1) & 0xFFFF
    aLoadBuffer: w0 = 0x04000000, w1 = curr_ptr
}
aSetBuffer: w0 = 0x08000000, w1 = (count << 1) & 0xFFFF
```

**_saveBuffer(r, curr_ptr, buff, count, p)** (801CADBC), DMEM `buff` to the delay line. The same positions; the
buffer is the `out` field of `A_SETBUFF`:

```c
if (wrap) {
    aSetBuffer:  w0 = 0x08000000, w1 = (buff << 16) | ((before_end << 1) & 0xFFFF)
    aSaveBuffer: w0 = 0x06000000, w1 = curr_ptr
    aSetBuffer:  w0 = 0x08000000, w1 = ((buff + (before_end << 1)) << 16) | ((after_end << 1) & 0xFFFF)
    aSaveBuffer: w0 = 0x06000000, w1 = r->delayLine
    aSetBuffer:  w0 = 0x08000000, w1 = (count << 1) & 0xFFFF
} else {
    aSetBuffer:  w0 = 0x08000000, w1 = (buff << 16) | ((count << 1) & 0xFFFF)
    aSaveBuffer: w0 = 0x06000000, w1 = curr_ptr
}
```

Note the asymmetry: `_saveBuffer` ends with `A_SETBUFF` only when it wraps; `_loadBuffer` always does.

### 14.3 The output tap: _loadOutputBuffer

**_loadOutputBuffer(r, d, buff, incount, p)** (801CAA34). Without a chorus it is `_loadBuffer(r,
&r->inputPos[-d->output], buff, incount, p)`. With one:

```c
s32 length = d->output - d->input;
s32 count, ramalign;
f32 delta, fratio, fincount;
s16 *out_ptr;

delta = _doModFunc(d, incount);
delta /= (f32)length;
delta = (f32)(s32)(delta * 32768.0f) / 32768.0f;
fratio = (f32)(1.0 - (f64)delta);
fincount = d->resampler->delta + fratio * (f32)incount;
count = (s32)fincount;
d->resampler->delta = fincount - (f32)count;
out_ptr = &r->inputPos[d->chorusDelta - d->output];
ramalign = ((u32)out_ptr & 7) >> 1;                    /* samples to the 8-byte boundary below */
p = _loadBuffer(r, out_ptr - ramalign, SYN_DMEM_FX_CHORUS, count + ramalign, p);
aSetBuffer: w0 = 0x08000000 | (0x280 | (ramalign << 1)), w1 = (buff << 16) | ((incount << 1) & 0xFFFF)
aResample:  w0 = 0x05000000 | ((u8)d->resampler->first << 16) | ((s32)(fratio * 32768.0f) & 0xFFFF)
            w1 = d->resampler->state
d->resampler->first = 0;
d->chorusDelta += count - incount;
```

The ROM combines `0x280` and `ramalign << 1` with an OR; it equals the sum. The delay line comes from the 16-byte
aligned heap, so `ramalign` is the same as on the N64.

### 14.4 The chorus oscillator: _doModFunc

**_doModFunc(d, count)** (801CAFC8) returns the chorus offset in samples:

```c
f32 val;

d->chorusValue += d->chorusStep * (f32)count;
val = d->chorusValue;
if ((f64)val > 2.0) val = (f32)((f64)val - 4.0);
d->chorusValue = val;
if (val < 0.0f) val = -val;
val = (f32)((f64)val - 1.0);
return d->chorusDepth * val;
```

`chorusValue` runs from -2 to 2; `|value| - 1` is a triangle wave from -1 to 1. All sections with a chorus share
one `ALResampler` in libmus's set up (map section 10).

### 14.5 The low-pass filter: init_lpfilter and _filterBuffer

**init_lpfilter(lp)** (801C7EB0):

```c
s32 i, temp = lp->fc >> 1;          /* fc is s16 */
f64 ffc, fcoef;

lp->first = 1;
lp->gain = 0x4000 - temp;
for (i = 0; i < 8; i++) lp->coef[i] = 0;
lp->coef[8] = temp;
fcoef = ffc = (f64)(s16)temp / 16384.0;
for (i = 9; i < 16; i++) {
    fcoef *= ffc;
    lp->coef[i] = (s16)(s32)(fcoef * 16384.0);
}
```

For Glover's `fc` 20480: gain 6144; coefficients 0 (8 times), 10240, 6400, 4000, 2500, 1562, 976, 610, 381.

**_filterBuffer(lp, buff, count, p)** (801CAF2C):

```c
aSetBuffer:  w0 = 0x08000000 | (u16)buff, w1 = (buff << 16) | ((count << 1) & 0xFFFF)
aLoadADPCM:  w0 = 0x0B000020, w1 = lp->coef                           /* 32 bytes */
aPoleFilter: w0 = 0x0E000000 | ((u8)lp->first << 16) | (u16)lp->gain, w1 = lp->fstate
lp->first = 0;
```

The filter runs in place on `buff`. It reuses the decoder's codebook area: `A_LOADADPCM` overwrites the codebook,
and every voice's `alAdpcmPull` loads its own again.

### 14.6 alFxNew (not used by Glover)

**alFxNew(r, c, hp)** (801C7F54) is libaudio's own set up. It differs from libmus's `CustomFxNew` and `CustomFxSet`:

- The preset comes from `c->fxType`: 1 small room (801F4420), 2 big room (801F4488), 3 chorus (801F4538), 4 flange
  (801F4560), 5 echo (801F4510), 6 `c->params`, any other value the empty preset at 801F4588 (0 sections, length
  0). The values are libmus's presets 1 to 5 (map section 10).
- `sections` (`sectionCount` x 0x28) and the delay line (`length` x 2) are sized for this preset. Each section with
  a chorus gets its own `ALResampler` (0x34, not set up by `alResampleNew`) and `RESAMPLE_STATE`; each with a
  low-pass gets its own `ALLowPass` and `POLEF_STATE`. The low-pass set up is `init_lpfilter`, inlined.
- The chorus values use `c->outputRate`: `chorusStep = (f32)(2.0 * (f64)((f32)rate / 1000.0f) / (f64)c->outputRate)`
  and `chorusDepth = (f32)((f64)(f32)depth / 173123.404906676 * (f64)(u32)(output - input))`; `chorusValue = 1.0f`;
  `chorusDelta = 0`; the resampler's `delta = 0.0f` and `first = 1`.
- The loop counters are `u16`, as in `CustomFxSet`.

### 14.7 alFxParam and alFxParamHdl

**alFxParam** (801CA830): `SYN_SET_SOURCE` sets `source`; other ids are ignored.

**alFxParamHdl(filter, id, param)** (801CA844, never called): `param` points at an `s32` value. `s = (id - 2) / 8`
and `field = (id - 2) % 8`, with C's signed division. Then on `sections[s]`: input or output = `value & ~7`;
fbcoef, ffcoef or gain = `(s16)value`; chorus rate: `chorusStep = (f32)(2.0 * (f64)((f32)value / 1000.0f) /
(f64)alGlobals->drvr.outputRate)`; chorus depth: `chorusDepth = (f32)((f64)(f32)value / 173123.404906676 *
(f64)(u32)(output - input))`; low-pass: if `lowpass` is not NULL, `fc = (s16)value` and `init_lpfilter`.

## 15. Audio commands

### 15.1 Encoding

Every command is two 32-bit words. The command number is the top byte of `w0`. The table lists every form the ROM
emits. DMEM offsets are bytes; counts are bytes. "addr" is the word of a pointer in the port (section 4).

| Cmd | Name | w0 | w1 | Emitted by |
| --- | --- | --- | --- | --- |
| 0x01 | A_ADPCM | `0x01 << 24 \| flags << 16`; flags `A_INIT` (1) on the first decode, `A_LOOP` (2) with the loop state | addr of the `ADPCM_STATE` (32 bytes) | synDecodeChunk |
| 0x02 | A_CLEARBUFF | `0x02 << 24 \| dmem & 0xFFFFFF` | count | alAdpcmPull, alRaw16Pull, the buses, alFxPull |
| 0x03 | A_ENVMIXER | `0x03 << 24 \| flags << 16`; flags 0x09 (`A_INIT \| A_AUX`) or 0x08 (`A_AUX`) | addr of the `ENVMIX_STATE` (80 bytes) | synPullSubFrame |
| 0x04 | A_LOADBUFF | `0x04 << 24` | addr of the source bytes | synDecodeChunk, alRaw16Pull, _loadBuffer |
| 0x05 | A_RESAMPLE | `0x05 << 24 \| flags << 16 \| pitch & 0xFFFF`; flags `A_INIT` or 0; pitch `ratio * 32768` | addr of the `RESAMPLE_STATE` (32 bytes) | alResamplePull, _loadOutputBuffer |
| 0x06 | A_SAVEBUFF | `0x06 << 24` | addr of the destination | _saveBuffer, alSavePull |
| 0x07 | A_SEGMENT | `0x07 << 24` | 0 (segment 0, base 0) | alAudioFrame, once per chunk |
| 0x08 | A_SETBUFF | `0x08 << 24 \| flags << 16 \| in & 0xFFFF`; flags 0 | `out << 16 \| count & 0xFFFF` | many |
| 0x08 | A_SETBUFF with A_AUX | `0x08 << 24 \| 0x08 << 16 \| dryRight & 0xFFFF` | `wetLeft << 16 \| wetRight & 0xFFFF` | synPullSubFrame |
| 0x09 | A_SETVOL | `0x09 << 24 \| flags << 16 \| value & 0xFFFF` | depends on flags, see below | synPullSubFrame |
| 0x0A | A_DMEMMOVE | `0x0A << 24 \| in & 0xFFFFFF` | `out << 16 \| count & 0xFFFF` | alAdpcmPull, alRaw16Pull, alResamplePull (unity), alFxPull |
| 0x0B | A_LOADADPCM | `0x0B << 24 \| count & 0xFFFFFF` | addr of `count` bytes of `s16` coefficients | alAdpcmPull (codebook, 128 bytes), _filterBuffer (32 bytes) |
| 0x0C | A_MIXER | `0x0C << 24 \| gain & 0xFFFF`; flags 0 | `in << 16 \| out & 0xFFFF` | alMainBusPull, alFxPull |
| 0x0D | A_INTERLEAVE | `0x0D << 24` | `left << 16 \| right` | alSavePull |
| 0x0E | A_POLEF | `0x0E << 24 \| flags << 16 \| gain & 0xFFFF`; flags `A_INIT` or 0 | addr of the `POLEF_STATE` (8 bytes) | _filterBuffer |
| 0x0F | A_SETLOOP | `0x0F << 24` | addr of the loop state (`ADPCM_STATE`) | synDecodeChunk |

The five forms of `A_SETVOL`, in the order `synPullSubFrame` emits them:

| Flags | Meaning | value (low 16 bits of w0) | w1 |
| --- | --- | --- | --- |
| 6 (`A_VOL \| A_LEFT`) | left volume at the start of the ramp | `volLeft` | 0 |
| 4 (`A_VOL \| A_RIGHT`) | right volume at the start of the ramp | `volRight` | 0 |
| 2 (`A_RATE \| A_LEFT`) | left ramp | `targetLeft` | `rateLeft << 16 \| rateLeftFrac` (16.16 per 8 samples) |
| 0 (`A_RATE \| A_RIGHT`) | right ramp | `targetRight` | `rateRight << 16 \| rateRightFrac` |
| 8 (`A_AUX`) | dry and wet amounts | `dryAmount` | `wetAmount` (low 16 bits) |

`A_SPNOOP` (0) is never emitted. The flag values come from abi.h: `A_INIT` 1, `A_LOOP` 2, `A_LEFT` 2, `A_VOL` 4,
`A_AUX` 8.

### 15.2 DMEM layout

| Offset | Size | Use |
| --- | --- | --- |
| 0x000 | 0x140 | compressed ADPCM bytes (decoder input); resampler output and envelope mixer input; reverb temp 1 (`buff2`); the interleaved output (0x280 bytes, after everything else) |
| 0x140 | 0x140 and more | decoded samples (decoder output, resampler input); reverb temp 0 (`buff1`) |
| 0x280 | | reverb chorus input |
| 0x440 | 0x140 | main L |
| 0x580 | 0x140 | main R |
| 0x6C0 | 0x140 | aux L |
| 0x800 | 0x140 | aux R |

The decoder output runs past 0x280: a pull of 160 samples at a ratio near 2 decodes up to 320 samples, after the 16
samples of the previous frame. A loop pass decodes after that at a 32-byte boundary, so in rare cases the data can
come close to 0x440 or past it. These offsets are relative to DMEM 0x5C0 (section 15.4). The interpreter models the
whole 4 KB DMEM with the microcode's layout, so any overlap behaves as on the RSP.

### 15.3 What one frame contains

For a chunk with all 24 voices playing and no change due: 1 `A_SEGMENT`; 2 clears (main); 2 clears (aux); per
voice: `A_LOADADPCM`, `A_SETBUFF`, `A_LOADBUFF`, `A_SETBUFF`, `A_ADPCM` (plus `A_SETLOOP`, `A_SETBUFF`, `A_ADPCM`,
`A_DMEMMOVE` per loop pass, and `A_CLEARBUFF` at a wave's end), `A_SETBUFF`, `A_RESAMPLE`, 2 x `A_SETBUFF`,
`A_ENVMIXER` (plus 5 x `A_SETVOL` after a change): about 10; the reverb: 6 + about 12 per section (4 sections, one
`A_POLEF` group); the main mix: 3; the save: 4. About 300 commands per chunk, about 2200 for a 1104-sample frame. Each due
change adds a sub-frame (about 10 more). libmus's glue gives `alAudioFrame` a list of 0x2000 commands, and the ROM
never checks the length. Neither does the port.

### 15.4 The microcode and the interpreter

[`src/audio/synth/rsp_audio.c`](../src/audio/synth/rsp_audio.c) runs the command lists on the PC. Its arithmetic
comes from the ROM's microcode: the code at `aspMainTextStart` (ROM 0xBF9E0, 0xE20 bytes, run from IMEM 0x1080) and
the data at `aspMainDataStart` (ROM 0xF4E00, 0x2C0 bytes). The RSP loads the data at DMEM 0 for every task. The
interpreter reads it from `port_rom` when it first runs. `tools/audiotest` runs the microcode itself on a model of the
RSP and checks that the interpreter gives the same results ([testing.md](testing.md)). A frame of 1104 samples with
24 voices takes about 1.2 ms.

**The data.** Offset 0x10 holds the IMEM addresses of the handlers of commands 0 to 15 (0x1118, 0x1470, 0x11DC,
0x1B38, 0x1214, 0x187C, 0x1254, 0x12D0, 0x12EC, 0x1328, 0x140C, 0x1294, 0x1E24, 0x138C, 0x170C, 0x144C). The
dispatcher takes the index from bits 24 to 30 of `w0`. Offsets 0xC0 to 0x2BF (ROM 0xF4EC0) are the resampler's filter
table: 64 phases of 4 taps (`s16`). Phase 63 is phase 0 backwards, and so on, and the taps of each phase add up to
32768 ± 16 (a gain of 1). The interpreter checks both facts when it reads the data. The rest is constants: 0, 1, 2,
0xFFFF, 0x20, 0x800, 0x7FFF and 0x4000 at offset 0, nibble masks, lane selectors and the envelope weights at 0xB0.

**DMEM.** The microcode's layout. The interpreter keeps the same layout in a 4 KB array.

| DMEM | Use |
| --- | --- |
| 0x000-0x2BF | the data |
| 0x320-0x35F | the segment table |
| 0x360-0x37F | the values that `A_SETBUFF`, `A_SETVOL` and `A_SETLOOP` set (below) |
| 0x380-0x4BF | the command list, copied in 0x140 bytes at a time |
| 0x4C0-0x5BF | `A_LOADADPCM`'s coefficients: 8 predictors of 16 `s16` |
| 0x5C0-0xEFF | the sample buffers. A command's DMEM offset 0 is DMEM 0x5C0, so main L (0x440) is at 0xA00 |
| 0xF90-0xFDF | a voice's state while `A_RESAMPLE`, `A_ENVMIXER` or `A_POLEF` runs |
| 0xFE0-0xFEF | `A_ENVMIXER`'s wet output without `A_AUX` |

The values at 0x360: +0 input, +2 output, +4 count (`A_SETBUFF`); +6 and +8 the left and right start volumes
(`A_SETVOL A_VOL`); +0xA, +0xC and +0xE dry right, wet left and wet right (`A_SETBUFF A_AUX`); +0x10 to +0x1E target,
rate and fraction left, the same right, then dry and wet (`A_SETVOL`). `A_SETLOOP` writes its address over +0x10 and
+0x12. Buffer offsets are stored with 0x5C0 added.

**Transfers.** The RSP's DMA moves whole 8-byte units. It ignores the low 3 bits of both addresses and rounds the
length up to a multiple of 8. `A_LOADBUFF` and `A_SAVEBUFF` use `A_SETBUFF`'s count and do nothing when it is 0.
`A_LOADADPCM` does not check for 0. Every address in the synthesizer's lists is 8-byte aligned on the N64. The
interpreter uses host addresses unchanged and keeps the rounding.

**The commands.** Counts are in bytes. A "block" is the unit that the microcode works in; it always finishes a block.

- `A_CLEARBUFF` and `A_DMEMMOVE` work in blocks of 16 bytes, `A_INTERLEAVE` in blocks of 8 pairs.
- `A_MIXER` works in blocks of 16 samples: `out = (out * 0x7FFF * 2 + 0x8000 + in * gain * 2) >> 16`, saturated. The
  gain is signed.
- `A_ADPCM` first writes the 16 samples before the first frame to the output: zeros with `A_INIT`, else the loop state
  with `A_LOOP`, else the voice's state. Then it decodes `count / 32` frames, rounded up. In each half of 8 samples,
  sample `i` is `c0[i] * y2 + c1[i] * y1 + c1[i-1] * r[0] + ... + c1[0] * r[i-1] + 2048 * r[i]`, kept in 32 bits,
  `>> 11` and saturated. `y2` and `y1` are the two samples before the half, `c0` and `c1` the predictor's two rows, and
  `r` the nibbles shifted left by the header's scale. A scale above 12 acts as 12. The state gets the last 16 samples
  of the output. With a count of 0 that is the 16 samples written first.
- `A_RESAMPLE` writes the state's 4 history samples just before the input. The position is 16.16 fixed point. It
  starts at the state's fraction and advances by `2 * pitch` per output sample. It is exact. Output `n` takes the 4
  input samples from 4 before the input plus the integer part, and the taps of phase `fraction >> 10`. Each product is
  rounded and saturated (VMULF), then added in pairs with saturation. At pitch 1.0 the output is the input 3 samples
  late. Blocks of 8 samples, at least one. The state gets the 4 samples at the next position and its fraction.
- `A_ENVMIXER` works in blocks of 8 samples. Each sample has its own 16.16 volume per side. With `A_INIT` the first
  block goes from the start volume towards `start * ratio` in 8 equal steps (weights 1/8 to 7/8, then 0xFFFF/0x10000).
  Each later block multiplies every sample's volume by the ratio. Then the integer part stops at the target: the
  smaller value with a ratio of 1 or more (unsigned compare), else the larger. The fraction is not changed. Each output
  is mixed as in `A_MIXER` with gain `VMULF(volume, dry)` or `VMULF(volume, wet)`. With `A_INIT` at least 2 blocks
  are made, else at least 1. The state holds the last block's volumes and the 8 parameters.
- `A_POLEF` scales `c1`, the second row of coefficients, by `gain * 4 / 65536` (`gain * 4` kept in 16 bits) and
  writes it back over DMEM 0x4D0. Each output is `c0[i] * y2 + c1[i] * y1 + scaled c1 terms on the earlier inputs +
  gain * x[i]`, `>> 14` and saturated, in blocks of 8. The state is the last 4 outputs.

**Quirks.** The interpreter keeps them all.

- With `A_INIT`, `A_POLEF` clears only the first 2 of its 4 history values. It uses the other 2, which hold what the
  last command left at DMEM 0xF94. In Glover that happens once, in the first frame's low-pass. The 2 values are then
  2 volumes from the last envelope mixer of that frame, or older values when no voice played.
- `A_SETLOOP` and the left target and rate of `A_SETVOL` share a DMEM word. This has no effect: the synthesizer
  always sends the volumes after the loop address.
- The microcode reads the next ADPCM frame, `A_POLEF`'s next block and `A_MIXER`'s next input before it stores the
  current block. This matters only when the input and output overlap.

**Not modelled.** None of these occurs in the synthesizer's lists.

- 16-bit values at odd DMEM addresses. The interpreter keeps 16-bit values in host order, and the RSP's big-endian
  bytes would mix two samples.
- ADPCM predictors 8 to 15, whose coefficients would come from the sample buffers.
- `A_SEGMENT`. Glover sets only segment 0, to 0. Every address is absolute.
- The low 3 bits of host addresses in transfers, an `A_LOADADPCM` count of 0, commands 16 and above.
- The flags that `A_ENVMIXER`'s compares read are assumed clear. A resampler command, which clears them, runs before
  every envelope mixer in Glover's lists.
- The resampler's scratch values at DMEM 0xFB0-0xFD3. Nothing reads them.

## 16. Function reference

All 64 functions of the range. "Callers" lists direct calls; the synthesizer calls the pull and parameter functions
through pointers.

### 16.1 Synthesizer core

| ROM | Name | Arguments | Returns | Role | Callers |
| --- | --- | --- | --- | --- | --- |
| 801CB190 | alInit | ALGlobals *g, ALSynConfig *c | none | set up once (unused) | none |
| 801CB1BC | alClose | ALGlobals *g | none | `alSynDelete`, `alGlobals = NULL` | ROM audio thread |
| 801CB1EC | alLink | ALLink *element, ALLink *after | none | insert after | many |
| 801CB20C | alUnlink | ALLink *element | none | remove | voice lists |
| 801CB240 | alHeapInit | ALHeap *hp, u8 *base, s32 len | none | section 7 | MusInitialize |
| 801CB280 | alHeapDBAlloc | u8 *file, s32 line, ALHeap *hp, s32 num, s32 size | void * | section 7 | everywhere |
| 801CB2D0 | alCopy | void *src, void *dest, s32 len | none | byte copy | alLoadParam |
| 801CB310 | alSynNew | ALSynth *s, ALSynConfig *c | none | build the graph (unused) | alInit |
| 801CB5DC | alAudioFrame | Acmd *cmdList, s32 *cmdLen, s16 *outBuf, s32 outLen | Acmd * | section 8 | libmus glue |
| 801CB78C | __allocParam | none | ALParam * | pop the pool | alSyn* |
| 801CB7B8 | __freeParam | ALParam *param | none | push the pool | alEnvmixerPull |
| 801CB7D0 | _collectPVoices | ALSynth *s | none | lame list to free list | alAudioFrame |
| 801CB824 | _freePVoice | ALSynth *s, PVoice *pvoice | none | to the lame list | alEnvmixerPull |
| 801CB864 | synTimeToSamplesExact | ALSynth *s, s32 micros | s32 | section 8 | alAudioFrame |
| 801CB8AC | _timeToSamples | ALSynth *s, s32 micros | s32 | section 8, rounded to 16 | alSynSetVol |
| 801CB8F8 | synNextPlayer | ALSynth *s, ALPlayer **client | s32 | section 8 | alAudioFrame |
| 801CB9A0 | alSynDelete | ALSynth *s | none | `head = NULL` | alClose |
| 801CB9B0 | alSynAddPlayer | ALSynth *s, ALPlayer *client | none | section 7 | MusInitialize |
| 801CA040 | alFilterNew | ALFilter *f, ALFilterHandler h, ALSetParam p, s32 type | none | section 7 | constructors |
| 801CA030 | __assert | expression, file, line | none | does nothing | 6 asserts |

### 16.2 Voice API

| ROM | Name | Arguments | Returns | Callers |
| --- | --- | --- | --- | --- |
| 801CBA00 | alSynAllocVoice | ALSynth *s, ALVoice *voice, ALVoiceConfig *vc | s32 (1 if it got a PVoice) | MusInitialize |
| 801CBB28 | _allocatePVoice | ALSynth *s, PVoice **pvoice, s16 priority | s32 (1 if stolen) | alSynAllocVoice |
| 801CBBE0 | alSynStopVoice | ALSynth *s, ALVoice *voice | none | libmus |
| 801CBC60 | alSynStartVoice | ALSynth *s, ALVoice *voice, ALWaveTable *table | none | libmus |
| 801CBD00 | alSynSetPitch | ALSynth *s, ALVoice *voice, f32 ratio | none | libmus |
| 801CBD90 | alSynSetVol | ALSynth *s, ALVoice *voice, s16 vol, ALMicroTime t | none | libmus |
| 801CBE50 | alSynSetFXMix | ALSynth *s, ALVoice *voice, u8 fxmix | none | libmus |
| 801CBEE0 | alSynSetPan | ALSynth *s, ALVoice *voice, ALPan pan | none | libmus |
| 801CBF70 | alSynAllocFX | ALSynth *s, s16 bus, ALSynConfig *c, ALHeap *hp | the ALFx | alSynNew (unused) |

### 16.3 Filters

| ROM | Name | Arguments | Returns | Section |
| --- | --- | --- | --- | --- |
| 801C84A4 | alLoadNew | ALLoadFilter *f, ALDMANew dmaNew, ALHeap *hp | none | 7 |
| 801C86E0 | alAdpcmPull | pull | Acmd * | 12.2 |
| 801C8B24 | alRaw16Pull | pull | Acmd * | 12.4 |
| 801C8EC0 | alLoadParam | param | none (the port returns 0) | 12.1 |
| 801C906C | synDecodeChunk | Acmd *p, ALLoadFilter *f, s32 tsam, s32 nbytes, s16 outp, s16 inp, u32 flags | Acmd * | 12.3 |
| 801C8554 | alResampleNew | ALResampler *r, ALHeap *hp | none | 7 |
| 801CA1D0 | alResamplePull | pull | Acmd * | 11 |
| 801CA3BC | alResampleParam | param | 0 | 11 |
| 801C83EC | alEnvmixerNew | ALEnvMixer *e, ALHeap *hp | none | 7 |
| 801C9300 | alEnvmixerPull | pull | Acmd * | 9.3 |
| 801C984C | alEnvmixerParam | param | 0 | 9.3 |
| 801C9920 | synPullSubFrame | ALEnvMixer *e, s16 *inp, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p | Acmd * | 10.3 |
| 801C9D24 | synGetRate | f64 vol, f64 tgt, s32 count, u16 *ratel | s16 | 10.4 |
| 801C9FA4 | synGetVol | f32 ivol, s32 samples, s16 ratem, u16 ratel | f32 | 10.5 |
| 801C9C20 | _frexpf | f64 value, s32 *exp (third argument slot) | f64 | unused |
| 801C9D00 | _ldexpf | f64 x, s32 e | f64 | unused: `x * (1 << e)` when `e` is not 0 |
| 801C85E0 | alAuxBusNew | ALAuxBus *m, void *sources, s32 maxSources | none | 7 |
| 801C91F0 | alAuxBusPull | pull | Acmd * | 13 |
| 801C92CC | alAuxBusParam | param | 0 | 13 |
| 801C863C | alMainBusNew | ALMainBus *m, void *sources, s32 maxSources | none | 7 |
| 801CA060 | alMainBusPull | pull | Acmd * | 13 |
| 801CA1A0 | alMainBusParam | param | 0 | 13 |
| 801C8698 | alSaveNew | ALSave *f | none | 7 |
| 801CB060 | alSavePull | pull | Acmd * | 13 |
| 801CB160 | alSaveParam | param | 0 | 13 |
| 801C7EB0 | init_lpfilter | ALLowPass *lp | none | 14.5 |
| 801C7F54 | alFxNew | ALFx *r, ALSynConfig *c, ALHeap *hp | none | 14.6 |
| 801CA480 | alFxPull | pull | Acmd * | 14.1 |
| 801CA830 | alFxParam | param | 0 | 14.7 |
| 801CA844 | alFxParamHdl | param | 0 | 14.7 |
| 801CAA34 | _loadOutputBuffer | ALFx *r, ALDelay *d, s32 buff, s32 incount, Acmd *p | Acmd * | 14.3 |
| 801CAC4C | _loadBuffer | ALFx *r, s16 *curr_ptr, s32 buff, s32 count, Acmd *p | Acmd * | 14.2 |
| 801CADBC | _saveBuffer | ALFx *r, s16 *curr_ptr, s32 buff, s32 count, Acmd *p | Acmd * | 14.2 |
| 801CAF2C | _filterBuffer | ALLowPass *lp, s32 buff, s32 count, Acmd *p | Acmd * | 14.5 |
| 801CAFC8 | _doModFunc | ALDelay *d, s32 count | f32 | 14.4 |

"pull" is `(void *filter, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p)`; "param" is `(void *filter, s32
paramID, void *param)`. The ROM passes the `buff` arguments of the reverb helpers as `s16` values (sign-extended).

### 16.4 Names and evidence

- The names that are not `func_` addresses are gloveworld's symbol names (`glabel` in `C0800.s`); they are kept.
- **synDecodeChunk** (801C906C): its only caller is `alAdpcmPull` (three calls). It reads `nbytes` of wave data
  through the DMA routine and emits the commands that load and decode them (`A_SETBUFF`, `A_LOADBUFF`, `A_SETLOOP`,
  `A_SETBUFF`, `A_ADPCM`).
- **synPullSubFrame** (801C9920): `alEnvmixerPull` calls it with the sample count up to the next change and once for
  the rest of the chunk. It pulls the resampler and emits the envelope mixer's commands. Its assert names "env.c".
- **synGetRate** (801C9D24): `synPullSubFrame` calls it for each side. It returns the integer part and stores the
  fraction of `(tgt / vol)^(8 / count)`; both go to `A_SETVOL` with `A_RATE`.
- **synGetVol** (801C9FA4): `alEnvmixerPull` calls it when a change arrives during a ramp. It returns `ivol *
  rate^(samples / 8)`, where the ramp has got to.
- **synTimeToSamplesExact** (801CB864): the arithmetic of the ROM's `_timeToSamples` without the rounding down to
  16. Its only caller adds the result to the player's next call time.
- **synNextPlayer** (801CB8F8): returns the earliest call time of the player list and that player. Its assert names
  "synthesizer.c".
- The field names in section 5 describe what the code does with each field. The names `mus_internal.h` already gave
  (`ALDelay`, `ALFx`, `ALAuxBus`, `ALMainBus`, `ALSave`, `PVoice`, and `ratio`, `delta`, `first`, `state`, `fc`,
  `fstate` elsewhere) are kept.
- The `syn` prefix marks names that the ROM does not give; the `SYN_` constants are the port's.

## 17. libmus and the synthesizer together

**One set of definitions.** `synth_internal.h` defines every structure, constant and function of the synthesizer,
and the builders of the audio commands (`synCmdSetBuffer` and the others, one per command). `mus_internal.h`
includes it, and libmus uses the same names: `SYN_SET_SOURCE`, `SYN_ADD_SOURCE`, `SYN_FILTER_FX`,
`SYN_MAX_OUT_SAMPLES` and `SYN_CHORUS_DEPTH_DIVISOR`. The layout checks of both headers cover every structure that
is kept in `musHeap`.

**The frame.** `musRenderFrame` (`mus-glue.c`) does what the ROM's `amHandleFrame` did for libmus:

1. `musDmaClear` frees the DMA buffers that the last two frames did not use and counts the frame (the ROM's
   `amClearDma`).
2. `alAudioFrame(musCmdList, &cmdLen, buf, 1104)` calls `musPlayerHandler` for each call time in the frame, then
   builds the commands in chunks of 160 samples (six of 160 and one of 144). The command list is the first of the
   two lists that the ROM takes from `musHeap` (0x2000 commands). Neither the ROM nor the port checks its length.
3. At its end, `alAudioFrame` calls `synExecuteCommands` (`rsp_audio.c`, section 15.4). The 1104 stereo samples are
   then in `buf`. With no player, `cmdLen` is 0 and the glue clears `buf`.

Every frame has 1104 samples. The ROM's glue added or removed a block of 16 samples now and then to follow the
audio interface. The handler's call times and the time of each change do not depend on the frame size (section 8).

**Who asks for frames.**

- With a sound device, SDL's audio thread calls `musAudioRender` for each block of samples it needs. It takes
  `musLock`, makes whole frames and keeps the samples left over for the next call.
- Without one (the headless mode, `--no-sound`, or no device), the frame driver calls `musAudioAdvance` once per
  game update with the update's length in video retraces. It makes the frames that fit in the game's time and
  drops their samples. `MusAsk` and `MusHandleAsk` call `musAudioPoll`, which makes one frame. The game waits in
  loops on these two functions. The frames then depend only on the game's updates, so a headless run gives the same
  memory every time.

**The platform.** `make AUDIO=mus` compiles `src/audio/*.c`, `src/audio/mus/*.c` and `src/audio/synth/*.c` with the
game's flags, and the platform layer with `-DPORT_AUDIO_MUS`. With that define `src/port/platform.c` provides
`port_audio_open`, which opens an SDL device for 16-bit stereo at 22047 Hz with a buffer of 1024 samples, and lets
SDL convert to the device's rate. Its lock (`port_audio_lock`) is a recursive SDL mutex. The default build
(`AUDIO=stub`) keeps the stand-in player in `src/port/audio_stub.c` and opens no device.

**Memory.** With `AUDIO=mus` the synthesizer writes its structures, the `ALParam`s, the command list and the state
blocks in `musHeap`, which is in `port_ram`. The regression test's baseline was made with the stand-in, which writes
nothing there, so the test uses the stand-in.

**Set up.** `CustomSynNew` calls `alLoadParam(&pv->decoder, SYN_SET_SOURCE, NULL)`, which does nothing (the ROM's
does the same).

## 18. Rules for changing this code

These rules hold for every change to `src/audio/synth/`.

1. **Floating point must match the ROM bit for bit.** The code is built with the game's flags (`-msse2 -mfpmath=sse
   -ffp-contract=off`). Every conversion this map shows must stay: the `f32` product then `f64` division in the time
   functions, the double arithmetic of `synGetRate` in its order (`1.0 / count` first, then `* 2^30`), the `f32`
   arithmetic of `synGetVol`, the resampler and the chorus, and the `f64` steps inside them (`(f64)ratio >
   1.99996`, `1.0 - (f64)delta`, `(f64)val > 2.0`, `(f64)val - 4.0`, `(f64)val - 1.0`). `libmus` sets flush-to-zero
   while it holds `musLock`; the synthesizer runs inside that lock.
2. **Float to integer** is truncation (`trunc.w.s`, `trunc.w.d`): a C cast. Values here are in range.
3. **Unsigned compares.** The loop end test, `nSam`'s minimum, the heap test and every delay line position test are
   unsigned in the ROM (`sltu`), and they are `u32` in the code.
4. **Integer widths.** DMEM offsets are `s16` locals; the counts in `A_SETBUFF` are masked to 16 bits; the 24-bit
   fields of `A_CLEARBUFF`, `A_DMEMMOVE` and `A_LOADADPCM` are masked with 0xFFFFFF. `rateLeftFrac` and
   `rateRightFrac` are `u16`; the targets, volumes and rates are `s16`. Products such as `volume * synEqPower[pan]`
   are 32-bit before the shift.
5. **Commands.** `w0` and `w1` are built with shifts and masks as section 15 shows. abi.h's bit fields and its
   macros' `_SHIFTL` must not be used on addresses: they mask to the field width.
6. **Addresses.** Commands hold the words of the pointers (section 4), with no `0x1FFFFFFF` mask and no
   `osVirtualToPhysical`.
7. **The quirks stay:** the reverb's `prev_out_ptr` sign (14.1), the double save (14.1), the `>` wrap test (14.1),
   the `_saveBuffer`/`_loadBuffer` asymmetry (14.2), `volume = 1` on reset (9.4), the start without a pull (9.3),
   the ramp over the full `rampLength` after a pan or fx change (10.3), the unchecked first `__allocParam` in
   `alSynAllocVoice` (9.1), the `+ 8` in the load size (12.3), the double writes in `alEnvmixerNew` (7).
8. **Asserts.** The ROM's `__assert` does nothing. A debug build may check `samples >= 0` and `samples <= 160` in
   `alEnvmixerPull`, but the behaviour for good data must not change.
9. **The pitch through `void *`.** It passes through the `SynValue` union.
10. **Comments.** Short plain sentences. Comment only arithmetic that is not obvious.

## 19. Files and functions

Every file is in `src/audio/synth/` and includes `synth_internal.h` (and not `mus_internal.h`).

| File | Functions |
| --- | --- |
| synth-core.c | alInit, alClose, alLink, alUnlink, alHeapInit, alHeapDBAlloc, alCopy, alSynNew, alAudioFrame, __allocParam, __freeParam, _collectPVoices, _freePVoice, synTimeToSamplesExact, _timeToSamples, synNextPlayer, alSynDelete, alSynAddPlayer, alFilterNew, alFxNew |
| synth-voice.c | alSynAllocVoice, _allocatePVoice, alSynStopVoice, alSynStartVoice, alSynSetPitch, alSynSetVol, alSynSetFXMix, alSynSetPan, alSynAllocFX, alAuxBusNew, alAuxBusParam, alAuxBusPull, alMainBusNew, alMainBusParam, alMainBusPull, alSaveNew, alSaveParam, alSavePull, alResampleNew, alResampleParam, alResamplePull |
| synth-load.c | alLoadNew, alLoadParam, alAdpcmPull, alRaw16Pull, synDecodeChunk |
| synth-envmix.c | alEnvmixerNew, alEnvmixerParam, alEnvmixerPull, synPullSubFrame, synGetRate, synGetVol; the log2 table |
| synth-fx.c | init_lpfilter, alFxPull, alFxParam, alFxParamHdl, _loadOutputBuffer, _loadBuffer, _saveBuffer, _filterBuffer, _doModFunc |

Not translated: `_frexpf` and `_ldexpf` (never called; `synGetRate` has the same steps), `__assert` (empty).
`synExecuteCommands` is in `rsp_audio.c`, the interpreter (section 15.4).

Notes for each file:

- **synth-core.c** (sections 7 and 8): `alAudioFrame` ends with `synExecuteCommands` (section 15.4). `alFxNew`
  (14.6) is here because it is set-up code. It calls `init_lpfilter` from synth-fx.c, where the ROM has that code
  inline. `alFxNew`'s presets are the same numbers as libmus's (`musFxSmallRoom` and the others in
  `mus-reverb.c`). The code reads the ROM's copies at 801F4420-801F45AF from `port_ram`, where they are swapped as
  `s32`.
- **synth-voice.c** (sections 9.1, 9.2, 11 and 13): the bus pulls and the save pull are short; the resampler is
  section 11.
- **synth-load.c** (section 12): `alRaw16Pull` never runs in Glover. It follows the assembly and section 12.4.
- **synth-envmix.c** (sections 9.3, 9.4 and 10): the log2 table is a `static const f64` array with the six-decimal
  literals of section 3. Both pan and fx mix use `synEqPower` (the `port_ram` table).
- **synth-fx.c** (section 14): delay line positions are `u32` addresses (section 14.1). libmus's set up shares the
  chorus resampler and the low-pass, so one state block serves every section that uses them.

## 20. Status

The port plays Glover's music and sound effects with libmus, this synthesizer and the interpreter of section 15.4.
`make AUDIO=mus` builds it. The default build still uses the stand-in player.

### 20.1 What is done

- **Translation.** 61 of the 64 functions of section 3 are in `src/audio/synth/`. `_frexpf` and `_ldexpf` are never
  called, and `__assert` is empty. The files and their functions are in section 19.
- **Headers.** libmus and the synthesizer share one definition of each structure (section 17).
- **Frames.** The glue runs `alAudioFrame` with the ROM's frame of 1104 samples, then the interpreter (section 17).
- **Wave data.** The DMA routine uses the ROM's DMA buffers in `musHeap` (section 4).
- **Sound device.** In a build with `AUDIO=mus`, the platform layer opens an SDL device and its callback calls
  `musAudioRender` under the lock. `--no-sound` and the headless mode open no device.
- **Headless runs.** The player runs on the game's clock (section 17). Nothing is heard.
- **Builds.** `make AUDIO=mus RENDERER=null`, `make AUDIO=mus` and `make TARGET=windows AUDIO=mus` build and link.
  Every file of `src/audio/` compiles with the game's flags and `-Wall -Wextra -Wno-unused-parameter` with no warning.

### 20.2 What was checked

- **The interpreter against the microcode.** `tools/audiotest/build.sh DIR ROM` passes its 17 checks. It runs the
  ROM's own audio microcode on a model of the RSP. Both give the same DMEM, states and output, bit for bit, for
  2000 to 3000 random cases of each command that the synthesizer uses, 40 lists like the synthesizer's and all
  242085 ADPCM frames of the game's 321 waves.
- **The heap.** `tools/mustest/build.sh` runs `MusInitialize` with the real synthesizer. The heap use is 281168
  bytes, as on the N64. The voices, the channels, the message ring, the save filter, both buses and every reverb
  buffer are at their N64 offsets in `audio_memory`.
- **Songs and effects.** `tools/audiotest/render.sh ROM OUTDIR` starts the player as the game does
  (`InitMusicDriver`, with both pbanks from the ROM) and starts songs with the game's `PrepareSong`. It renders 20
  seconds of songs 25, 1, 5, 9, 13, 17, 21 and 28. It renders each song again with only one voice heard, for the two
  voices that play the most notes. It renders effects 131, 3, 20, 45, 60 and 90 with `MusStartEffect2`, panned left,
  right and centre, and with `MusHandleSetReverb` 60. That makes 48 WAV files. `analyze_audio.py` then checks them,
  and all its checks pass:
  - Level: no file is silent (RMS 157 to 5123).
  - Clipping: no sample in any file is at full scale.
  - Stereo: in songs whose voices have different pans, left and right differ in 94% to 97% of the samples. An
    effect panned hard left or right leaves at most 0.06% of its energy in the other channel.
  - Reverb: every effect that ends within the file has a tail with reverb. Its energy is 931 to 2.4e10 times that
    of the dry effect. The tail dies away: the last 0.5 seconds are 81 to 105 dB weaker.
  - Pitch: 478 held notes of 29 waves. Between two notes of one wave, the interval measured from the spectra is
    within 2.1 cents of the interval of the pitch ratios that libmus sent. For the 17 waves played at two or more
    note numbers, the fundamental or the strongest partial lies within 34 cents of an equal-tempered note (A = 440
    Hz), 15 of them within 18 cents. The tuning of the recorded waves accounts for these offsets.
  - Timing: the 44811 voice changes logged for the songs fall on multiples of 16 samples. Successive change times are
    352 or 368 samples apart, or a sum of these. That is one player period (367 samples) rounded down to 16.
- **Repeatable output.** Two separate builds and runs of `render.sh` gave byte-identical WAV files. Two headless
  runs of an `AUDIO=mus` build (the attract mode for 900 frames, level 5 for 600) gave the same memory hash every
  100 frames. The run logged no wave data request that the DMA routine could not serve, and no problem with the
  effect tables.
- **The sound device.** An `AUDIO=mus RENDERER=null` build ran for 25 seconds with SDL's disk audio driver. It wrote
  24.66 seconds of audio at 22047 Hz. The output is silent for the first 0.11 seconds, until the first sound. After
  that, no run of silent samples is longer than 2 samples. The title music and the attract mode's sounds are there.
  An `AUDIO=mus` build with the OpenGL renderer wrote 14.8 seconds of audio in a run of 15 seconds.
- **The logic.** `tools/regress.py check`: 0 of 48 runs differ from the baseline.
- **Wave lengths.** `alLoadParam` rounds a wave's length down to whole 9-byte ADPCM frames, in the bank in
  `port_ram`. Every wave of both banks is already a whole number of frames, so this writes the same value.

### 20.3 What is approximate

- **Frame size.** The ROM's frames were usually 1104 samples, sometimes 16 more or less. The port's are always 1104.
  The handler's call times and the time of each change are the same (section 8). The chunk boundaries can differ.
  They decide where the command list splits the work, and the resampler's `f32` position is rounded at each chunk.
  So the samples can differ slightly from the N64's at the same time.
- **The audio clock.** With a sound device, the device's clock drives the player, as the audio interface did on
  the N64. When a command from the game reaches the player depends on the threads' timing on both. Headless runs
  count 60 retraces a second; the NTSC rate is about 59.94 Hz (audio-libmus-map.md, section 16.5).
- **The output.** SDL converts 22047 Hz to the device's rate with its own resampler. The N64's audio interface and
  its analogue filtering are not modelled.
- **Latency.** SDL's buffer holds 1024 samples (46 ms), and the player makes a whole frame of 1104 samples (50 ms)
  when it needs one.
- **Floating point.** `musLock` turns on flush-to-zero while libmus and the synthesizer run, as the N64's FPCSR
  did. It does not turn on denormals-are-zero. A float to integer conversion out of range gives 0x80000000 on x86;
  the N64 traps. The tests met neither case.
- **The interpreter.** Section 15.4 lists what it does not model. None of it occurs in Glover's lists.

### 20.4 Open questions

- **A whole frame against the N64.** No test compares a whole command list or a stretch of output with the N64's
  for the same state. The interpreter matches the microcode. A comparison
  could take a memory snapshot of the N64 between two audio frames, load libmus's and the synthesizer's state into
  the port, run `alAudioFrame` and compare the next command list with the one in the N64's memory.
- **The regression test.** It covers the logic with the stand-in player. A baseline made with `AUDIO=mus` would
  also cover the player's state in `port_ram` in the headless mode.
