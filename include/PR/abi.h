/* Written for weemitt: the parts of the N64 system interface that the port's code uses. */
#ifndef WEEMITT_PR_ABI_H
#define WEEMITT_PR_ABI_H

/*
 * The audio command list. The synthesizer writes it and the port's
 * interpreter (src/audio/synth/rsp_audio.c) runs it in place of the RSP.
 */

/* Command codes: the top byte of a command's first word. */
#define A_SPNOOP     0
#define A_ADPCM      1
#define A_CLEARBUFF  2
#define A_ENVMIXER   3
#define A_LOADBUFF   4
#define A_RESAMPLE   5
#define A_SAVEBUFF   6
#define A_SEGMENT    7
#define A_SETBUFF    8
#define A_SETVOL     9
#define A_DMEMMOVE   10
#define A_LOADADPCM  11
#define A_MIXER      12
#define A_INTERLEAVE 13
#define A_POLEF      14
#define A_SETLOOP    15

/* Flags: the second byte of a command's first word. Some bits mean different things in different commands. */
#define A_INIT  0x01
#define A_LOOP  0x02
#define A_LEFT  0x02
#define A_RIGHT 0x00
#define A_VOL   0x04
#define A_RATE  0x00
#define A_AUX   0x08

/* Samples in one ADPCM frame, and codebook entries for each order of each predictor. */
#define ADPCMFSIZE 16
#define ADPCMVSIZE 8

/* The highest pitch ratio the resampler takes: just under one octave up. */
#define MAX_RATIO 1.99996

#if defined(_LANGUAGE_C)

/* One command: two words. The long long member sets the union's alignment. */
typedef union {
    struct {
        unsigned int w0;
        unsigned int w1;
    } words;
    long long int align;
} Acmd;

/* The state a command keeps from one frame to the next, in main memory. */
typedef short ADPCM_STATE[ADPCMFSIZE];
typedef short RESAMPLE_STATE[16];
typedef short POLEF_STATE[4];
typedef short ENVMIX_STATE[40];

#endif

#endif
