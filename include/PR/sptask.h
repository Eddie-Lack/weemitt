/* Written for weemitt: the parts of the N64 system interface that the port's code uses. */
#ifndef WEEMITT_PR_SPTASK_H
#define WEEMITT_PR_SPTASK_H

#include <PR/ultratypes.h>

/* A task for the RSP: the microcode to run and the buffers it works with. The game keeps tasks in its own
 * memory, so the pointers are PTR32 fields. */
typedef struct {
    u32 type;
    u32 flags;
    PTR32(u64) ucode_boot;
    u32 ucode_boot_size;
    PTR32(u64) ucode;
    u32 ucode_size;
    PTR32(u64) ucode_data;
    u32 ucode_data_size;
    PTR32(u64) dram_stack;
    u32 dram_stack_size;
    PTR32(u64) output_buff;
    PTR32(u64) output_buff_size;
    PTR32(u64) data_ptr;
    u32 data_size;
    PTR32(u64) yield_data_ptr;
    u32 yield_data_size;
} OSTask_t;

/* The second member gives the task the alignment of a 64-bit integer. */
typedef union {
    OSTask_t t;
    long long int force_structure_alignment;
} OSTask;

typedef u32 OSYieldResult;

/* Bytes in the buffer that keeps a graphics task's state while it is paused. */
#ifdef F3DEX_GBI
#define OS_YIELD_DATA_SIZE 0xC00
#else
#define OS_YIELD_DATA_SIZE 0x900
#endif

void osSpTaskLoad(OSTask *task);
void osSpTaskStartGo(OSTask *task);
void osSpTaskYield(void);
OSYieldResult osSpTaskYielded(OSTask *task);

/* Loads a task and starts it. */
#define osSpTaskStart(task) { osSpTaskLoad(task); osSpTaskStartGo(task); }

#endif
