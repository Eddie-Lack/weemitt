/*
 * rspsim.h: a small model of the N64's RSP (the signal processor) that runs
 * the ROM's own audio microcode. tools/audiotest uses it to check the native
 * interpreter (src/audio/synth/rsp_audio.c) against the microcode.
 *
 * It models the scalar unit, the vector unit (only the instructions that the
 * audio microcode uses), the 4 KB data memory (DMEM), the 4 KB instruction
 * memory (IMEM) and the DMA engine. RDRAM is a byte array that the caller
 * owns. All memories are big-endian, as on the N64.
 *
 * The microcode comes from the player's ROM at run time. Nothing of it is in
 * this source.
 */
#ifndef RSPSIM_H
#define RSPSIM_H

#include <stdint.h>

typedef struct RspSim {
    uint32_t r[32];         /* scalar registers */
    uint8_t v[32][16];      /* vector registers, big-endian bytes: lane i is bytes 2i and 2i + 1 */
    int64_t acc[8];         /* accumulator, 48 bits, kept sign-extended */
    uint16_t vcoCarry;      /* VCO low byte: one bit per lane */
    uint16_t vcoNe;         /* VCO high byte */
    uint16_t vccLo, vccHi;
    uint16_t vce;
    uint8_t dmem[4096];
    uint8_t imem[4096];
    uint8_t data[4096];     /* the microcode's data, which the boot code copies to DMEM 0 for every task */
    uint32_t dataSize;
    uint32_t c0[16];        /* the coprocessor 0 registers that the microcode writes */
    uint8_t *rdram;
    uint32_t rdramSize;
    uint64_t steps;
    int error;              /* set when the model meets an instruction it does not know */
} RspSim;

/* Loads the microcode: text at IMEM 0x080; the data goes to DMEM 0 now and at the start of every task. */
void rspSimLoad(RspSim *rsp, const uint8_t *text, uint32_t textSize, const uint8_t *data, uint32_t dataSize);

/*
 * Runs one audio task: the command list at RDRAM address listAddr, listBytes
 * long. Returns 0 when the microcode reached its end (break), -1 on an error.
 */
int rspSimRunTask(RspSim *rsp, uint32_t listAddr, uint32_t listBytes);

#endif
