/*
 * rsp_audio.h: the native interpreter of the audio command lists.
 *
 * On the N64 the synthesizer builds a list of audio commands (Acmd, see
 * PR/abi.h) and the RSP runs it with the audio microcode (aspMain). The port
 * has no RSP. rsp_audio.c does the microcode's work on the PC: it runs each
 * command in order on a simulated data memory (DMEM) and reads and writes
 * host memory where the microcode used DMA.
 *
 * The arithmetic of every command was read from the microcode in the ROM
 * (aspMainTextStart, ROM 0xBF9E0). docs/audio-synth-map.md, section 15,
 * describes the commands and what the interpreter does.
 *
 * Commands are two host-order words. Addresses in them are 32-bit host
 * pointers. Sample data in host memory and in the simulated DMEM is
 * host-order s16. Compressed ADPCM data is a byte stream in ROM order.
 */
#ifndef RSP_AUDIO_H
#define RSP_AUDIO_H

#include <PR/ultratypes.h>
#include <PR/libaudio.h> /* Acmd, through abi.h */

/* The RSP's data memory. Byte addresses in the interpreter wrap at this size. */
#define RSP_AUDIO_DMEM_SIZE 0x1000

/*
 * The microcode's data (aspMainDataStart, ROM 0xF4E00, 0x2C0 bytes). The RSP
 * loads it at DMEM 0 at the start of every task. It holds the resampler's
 * filter table. The interpreter copies it from the ROM image at start-up.
 */
#define RSP_AUDIO_ROM_DATA 0xF4E00
#define RSP_AUDIO_DATA_SIZE 0x2C0

/*
 * Reads the microcode's data from the ROM image (big-endian, .z64 order) and
 * checks that it holds the filter table. Returns 0 on success and -1 when the
 * ROM is too small or the data is not what the interpreter expects. Until it
 * succeeds the filter table is all zeros, and every resampled voice is silent.
 */
s32 rspAudioInit(const u8 *rom, u32 romSize);

/* Runs one task: cmdCount commands from cmdList. */
void rspAudioRunTask(const Acmd *cmdList, s32 cmdCount);

/* The simulated DMEM, for tests. 16-bit values in it are in host order. */
u8 *rspAudioDmem(void);

#endif /* RSP_AUDIO_H */
