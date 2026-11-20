/* Written for weemitt: the parts of the N64 system interface that the port's code uses. */
#ifndef WEEMITT_PR_UCODE_H
#define WEEMITT_PR_UCODE_H

/* Sizes in bytes that the game puts in a graphics task. */
#define SP_UCODE_SIZE       4096
#define SP_UCODE_DATA_SIZE  2048
#define SP_DRAM_STACK_SIZE8 1024

/* Where the microcode sits in the game's memory. The port uses these addresses only as names for the microcode. */
extern long long int rspbootTextStart[];
extern long long int rspbootTextEnd[];
extern long long int gspF3DEX_fifoTextStart[];
extern long long int gspF3DEX_fifoDataStart[];
extern long long int gspSprite2DTextStart[];
extern long long int gspSprite2DDataStart[];

#endif
