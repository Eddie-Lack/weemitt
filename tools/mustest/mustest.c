/* Links the native libmus (src/audio/mus) with the native synthesizer (src/audio/synth) and runs MusInitialize
   with the configuration that the game's InitMusicDriver passes. It checks the heap layout against the N64's.

   Usage: tools/mustest/build.sh [DIR]     builds DIR/mustest (default build/mustest) and runs it

   The libmus buffers and the configuration sit at their N64 addresses in a small port_ram, as in the game. The
   pbanks are zeroed buffers, and no wave is converted. The expected values come from a snapshot of the N64's
   memory during the attract mode (tools/n64ref/emu.py): musicMemoryUsed and the addresses that libmus keeps in
   its globals. The program prints each value and fails if one differs. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <PR/os.h>

#include "mus_internal.h"

#define RAM_BASE 0x80000000
#define MUSIC_PLAYER 0x801FA940 /* musicPlayer, the game's MusConfig */
#define MUSIC_HEAP 0x8002DC90   /* audio_memory */
#define MUSIC_HEAP_SIZE 0x493E0
#define MUSIC_PRIORITY 0x8C     /* musicPriority */
#define SCHED 0x801FFE60        /* sched, the scheduler; libmus does not use it on the PC */
#define WBANK1 0xB04E3EE0       /* _wbank1SegmentRomStart */
#define WBANK2 0xB0607280       /* _wbank2SegmentRomStart */
#define SFX_COUNT 420

/* What the port provides (src/port, gen/ram.S). */
u8 port_ram[0x400000] __attribute__((aligned(16)));
u8 *port_rom;
u32 port_rom_size;
s32 osTvType = MUS_TV_NTSC;
ALGlobals *alGlobals;

/* The host objects that gen/ram.S puts in port_win in the game. */
ALGlobals musGlobals;
ALPlayer musPlayer;
ALHeap musHeap;
MusDmaState musDmaState;
s16 musBuffer[MUS_BUFFER_FRAMES * 2];
s16 musDropBuffer[MUS_BUFFER_FRAMES * 2];
u8 musDmaZeros[MUS_DMA_ZEROS_SIZE];

/* libmus's reverb presets, which the game's loader puts in port_ram. Their sizes decide the reverb's blocks in the
   heap. */
s32 D_801F4358[10] = { /* musFxNone */
    1, 800,
    0, 200, 0, 0, 0, 0, 0, 0,
};
s32 D_801F4190[26] = { /* musFxSmallRoom */
    3, 4000,
    0, 2160, 9830, -9830, 0, 0, 0, 0,
    760, 1520, 3276, -3276, 16383, 0, 0, 0,
    0, 2400, 5000, 0, 0, 0, 0, 20480,
};
s32 D_801F41F8[34] = { /* musFxBigRoom */
    4, 4000,
    0, 2640, 9830, -9830, 0, 0, 0, 0,
    880, 2160, 3276, -3276, 16383, 0, 0, 0,
    2640, 3640, 3276, -3276, 16383, 0, 0, 0,
    0, 3760, 8000, 0, 0, 0, 0, 20480,
};
s32 D_801F42A8[34] = { /* musFxChorus */
    1, 800,
    0, 200, 16384, 0, 32767, 7600, 700, 0,
};
s32 D_801F4330[10] = { /* musFxFlange */
    1, 800,
    0, 200, 0, 24575, 32767, 380, 500, 0,
};
s32 D_801F4280[10] = { /* musFxEcho */
    1, 8000,
    0, 7160, 12000, 0, 32767, 0, 0, 0,
};

/* Tables that the synthesizer reads from port_ram in the game (gen/ram.S). The test plays no note, so zeros do. */
s16 D_801F45B0[SYN_EQPOWER_SIZE];
s32 D_801F4420[26], D_801F4488[34], D_801F4510[10], D_801F4538[10], D_801F4560[10], D_801F4588[2];

static s32 lockDepth;
static int failures;

void port_log(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void port_fatal(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

s32 port_audio_open(s32 rate, void (*render)(s16 *out, s32 frames)) {
    (void)rate;
    (void)render;
    return 0;
}

void port_audio_lock(void) {
    lockDepth++;
}

void port_audio_unlock(void) {
    lockDepth--;
}

void port_audio_yield(void) {
}

/* The port's osSetIntMask (src/port/os.c) does nothing either. */
OSIntMask osSetIntMask(OSIntMask mask) {
    return mask;
}

/* The game build maps __sinf to port_sinf (src/port/port_pre.h). The test does not run the player. */
f32 __sinf(f32 x) {
    return sinf(x);
}

static u8 *ram(u32 address) {
    return port_ram + (address - RAM_BASE);
}

static void check(const char *what, long value, long expected) {
    printf("%-34s %8ld (0x%05lX)", what, value, (unsigned long)value);
    if (value != expected) {
        printf("   expected %ld (0x%05lX)", expected, (unsigned long)expected);
        failures++;
    }
    printf("\n");
}

/* An offset from the start of the music heap, as the N64 address minus audio_memory. */
static long heapOffset(void *p) {
    return (long)((u8 *)p - ram(MUSIC_HEAP));
}

int main(void) {
    static u8 pbank1[0x30];
    static u8 pbank2[0x30];
    static u8 *fxStreams[SFX_COUNT];
    static s32 fxPriorities[SFX_COUNT];
    MusConfig *config = (MusConfig *)ram(MUSIC_PLAYER);
    ALSynth *synth;
    s32 used;

    /* InitMusicDriver. The game's MusConfig names the words from 0x30 one place off (see the map), and it never
       sets the word at 0x28, which stays 0 in bss. */
    config->control_flag = 0;
    config->channels = 24;
    config->sched = ram(SCHED);
    config->thread_priority = MUSIC_PRIORITY;
    config->heap = ram(MUSIC_HEAP);
    config->heap_length = MUSIC_HEAP_SIZE;
    config->ptr = pbank2;
    config->wbk = WBANK2;
    config->default_fxbank = fxStreams;
    config->priority = fxPriorities;
    config->syn_updates = 0x400;     /* the game's fifo_length */
    config->syn_output_rate = 22050; /* syn_output_rate */
    config->syn_rsp_cmds = 0x2000;   /* the game's syn_updates */
    config->syn_retraceCount = 3;    /* the game's syn_rsp_cmds */
    config->syn_num_dma_bufs = 40;   /* the game's syn_retraceCount */
    config->syn_dma_buf_size = 0x600;

    used = MusInitialize(config);
    MusBankInitialize(pbank1, (u8 *)WBANK1);

    synth = &musGlobals.drvr;
    check("MusInitialize result", used, 0x44A50);
    check("musFrameRate", musFrameRate, 60);
    check("musFrameMicros", musFrameMicros, 16666);
    check("musFifoSize", musFifoSize, 64);
    check("musVoices", heapOffset(musVoices), 0x0);
    check("musChannels", heapOffset(musChannels), 0x2A0);
    check("musFifo", heapOffset(musFifo), 0x1F80);
    check("outputFilter (ALSave)", heapOffset(synth->outputFilter), 0x2D60);
    check("auxBus", heapOffset(synth->auxBus), 0x2D80);
    check("mainBus", heapOffset(synth->mainBus), 0x2E30);
    check("musReverb.delays", heapOffset(musReverb.delays), 0x2EB0);
    check("musReverb.delayLine", heapOffset(musReverb.delayLine), 0x2F50);
    check("musReverb.resampler", heapOffset(musReverb.resampler), 0x6DD0);
    check("musReverb.resampleState", heapOffset(musReverb.resampleState), 0x6E10);
    check("musReverb.lowpass", heapOffset(musReverb.lowpass), 0x6E30);
    check("musReverb.lowpassState", heapOffset(musReverb.lowpassState), 0x6E60);
    check("outputRate", synth->outputRate, 22047);
    check("numPVoices", synth->numPVoices, 24);
    check("maxAuxBusses", synth->maxAuxBusses, 1);
    check("maxOutSamples", synth->maxOutSamples, 160);
    check("alGlobals is &musGlobals", alGlobals == &musGlobals, 1);
    check("musReverb.synth is the synthesizer", musReverb.synth == synth, 1);
    check("musCurrentBank is pbank2", musCurrentBank == (MusBank *)pbank2, 1);

    /* A few calls the game makes. With no channel playing they change nothing. */
    MusSetMasterVolume(MUSFLAG_EFFECTS, 0x4000);
    check("musFxMasterVolume", musFxMasterVolume, 0x4000);
    check("MusHandleSetVolume(0, 80)", MusHandleSetVolume(0, 80), 0);
    check("MusAsk(3)", MusAsk(MUSFLAG_EFFECTS | MUSFLAG_SONGS), 0);
    MusStop(MUSFLAG_EFFECTS | MUSFLAG_SONGS, 0);
    musAudioAdvance(3.0f);

    check("lock depth after the calls", lockDepth, 0);

    if (failures != 0) {
        printf("%d checks failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
