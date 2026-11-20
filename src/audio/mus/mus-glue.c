/*
 * mus-glue.c: the platform glue of libmus, replaced for the PC.
 *
 * The ROM's glue (801C2C10-801C3708) drives the N64's hardware: an audio
 * thread that is a client of the NN scheduler, RSP audio tasks, the audio
 * interface (AI) and cartridge DMA. The port does not translate it. This
 * file replaces it with the native model of docs/audio-libmus-map.md,
 * section 12.2.
 *
 * What the ROM's functions did, and what replaces them:
 *
 * amCreateAudioMgr (801C2C10). Opened the cartridge and 64DD PI handles. Set
 *   the synthesizer's DMA routine (amDmaNew) and its output rate (the rate
 *   osAiSetFrequency gives). Took DMA buffers, I/O messages, two command lists
 *   and four output blocks from the libmus heap. Called CustomInit. Created
 *   and started the audio thread. Replaced by amCreateAudioMgr below.
 *
 * amAudioThread (801C3040). Registered with the NN scheduler (nnScAddClient)
 *   and looped. It waited for a message, then dropped one more if one was
 *   waiting. A retrace message (type 1) ran amHandleFrame with the next of
 *   four output blocks (frame counter & 3) and the block sent last time. When
 *   that sent a task, the thread waited for the task's done message, called
 *   amHandleTaskDone and kept the block as lastInfo. A pre-NMI message
 *   (type 3) did nothing. Type 10 ended the loop, and the thread called
 *   alClose. Replaced by the host's audio thread, which calls musAudioRender.
 *
 * amHandleFrame (801C3198). Called amClearDma. Gave the last frame's samples
 *   to the AI (osAiSetNextBuffer). Chose this frame's length:
 *   frameSize - (samples still in the AI) + extra + 16, rounded down to 16
 *   and at least frameSize - 16. Called alAudioFrame with one of two command
 *   lists. Unless that made no commands, it built an RSP task (rspboot,
 *   aspMain and the list), sent it to the scheduler and switched lists.
 *   Replaced by musRenderFrame.
 *
 * amHandleTaskDone (801C3364). Read osAiGetLength and wrote 0 to a flag that
 *   was already 0. It had no effect. Nothing replaces it.
 *
 * amDmaCallback (801C33A4). The synthesizer's DMA routine. It returned the
 *   address of wave data in one of the DMA buffers, and started a DMA of a
 *   whole buffer when no buffer held the bytes. Translated as musDmaRead. The
 *   DMA becomes a copy from the ROM image.
 *
 * amDmaNew (801C35B8). Set up the DMA buffer lists on its first call and
 *   returned amDmaCallback. Translated as amDmaNew below.
 *
 * amClearDma (801C35F0). Took the DMA messages of the last frame, gave
 *   buffers unused for more than one frame back to the free list and counted
 *   the frame. Translated as musDmaClear, without the messages.
 *
 * Only a few of these things touch libmus's own state: the DMA routine and
 * the output rate in the synthesizer's configuration, the blocks taken from
 * the libmus heap, and the CustomInit call. The port keeps all of them. In
 * one audio frame the ROM ran amClearDma, then alAudioFrame, which first
 * calls the player handler for each period that starts in the frame and then
 * builds the frame. The port keeps this order. The DMA buffers matter to the
 * command lists: the low bits of a buffer address decide how the decoder
 * aligns the data it loads. With the ROM's buffers in the libmus heap, the
 * port's lists hold the same offsets as the ROM's, and the command lists in
 * port_ram hold no address outside port_ram. The rest of the per-frame work
 * (AI buffers, the I/O messages) only served the hardware.
 *
 * The native model:
 *
 * - With a host audio thread, the platform's audio callback calls
 *   musAudioRender. The native synthesizer calls musPlayerHandler through
 *   musPlayer at the times of map section 4.
 * - Without one (the headless mode, or no audio device), nothing renders on
 *   its own. The game's frame driver calls musAudioAdvance with the game
 *   time of each update, and MusAsk and MusHandleAsk call musAudioPoll.
 * - Wave data is read from the ROM image in memory (port_rom).
 * - musLock and musUnlock are one recursive lock around libmus. The Mus* API
 *   and musAudioRender take it.
 *
 * What the platform layer provides (src/port/platform.c). mus_internal.h
 * declares these. s32 is long and s16 is short. platform.c can define them
 * with plain C types:
 *
 * - port_audio_open(rate, render) opens a stereo stream of 16-bit samples at
 *   rate Hz. The stream converts to the device's rate itself. The platform's
 *   audio thread then calls render(out, frames) for every block of frames it
 *   needs. It returns 1 when the stream plays. It returns 0 when no audio
 *   thread will call render: in the headless mode, or with no audio device.
 *   The callback may start before port_audio_open returns. MusInitialize
 *   still holds the lock then. The first callback waits for it.
 * - port_audio_lock and port_audio_unlock are a recursive mutex. libmus
 *   calls them before port_audio_open too.
 * - port_audio_yield gives the processor away for a moment.
 */
PORT_C_BEGIN
#include <string.h>

#include "mus_internal.h"

/* The N64's video clocks in Hz. The audio interface divides one of them by a whole number. */
#define MUS_VI_CLOCK_NTSC 48681812
#define MUS_VI_CLOCK_PAL 49656530
#define MUS_VI_CLOCK_MPAL 48628316
#define MUS_AI_MIN_DIVISOR 132 /* osAiSetFrequency fails below this */

/* MusConfig.control_flag bit 0: the wave data is in memory. */
#define MUS_CONTROL_WAVES_IN_RAM 1

/* Every frame of the synthesizer is a whole number of these blocks of samples. */
#define MUS_FRAME_ALIGN 16

/* Extra samples the ROM allowed in a frame for refilling the AI: 0x50 below 22050 Hz, else 0xF0. */
#define MUS_EXTRA_RATE 22050
#define MUS_EXTRA_LOW 0x50
#define MUS_EXTRA_HIGH 0xF0

/* Sizes of the blocks that the ROM's glue took from the libmus heap. */
#define MUS_ROM_DMA_BUFFER_SIZE 0x14 /* link, start address, frame last used, data pointer */
#define MUS_ROM_IO_MESG_SIZE 0x18    /* OSIoMesg: two per DMA buffer */
#define MUS_ROM_MESG_SIZE 4          /* OSMesg: two per DMA buffer */
#define MUS_ROM_ACMD_SIZE 8          /* one audio command */
#define MUS_ROM_AUDIO_INFOS 4
#define MUS_ROM_AUDIO_INFO_SIZE 0x68 /* output buffer, sample count, NNScTask, message */

/* One stereo frame: two 16-bit samples, left then right. */
#define MUS_SAMPLE_BYTES 4

/* MXCSR bit FZ: results too small for a normal float become zero. */
#define MUS_MXCSR_FLUSH_TO_ZERO 0x8000

/* Glover's sound effect streams start here (D_801E7760) and end where the effect list starts. */
#define MUS_RAM_FX_STREAMS 0x801E7760
#define MUS_RAM_BASE 0x80000000

MUS_CHECK_SIZE(MusDmaBuffer, MUS_ROM_DMA_BUFFER_SIZE);

/* musDmaState, musBuffer, musDropBuffer and musDmaZeros are in port_win (mus_internal.h). */
static MusDmaBuffer *musDmaBuffers; /* ROM D_802AD354: the records, in the libmus heap */
static s32 musDmaBufferSize;        /* ROM D_802AD360: MusConfig.syn_dma_buf_size */
static u32 musDmaFrame;             /* ROM D_801F4170: frames made; amClearDma counted them */
static s32 musDmaBadRequests;       /* requests the port could not serve; the first one is logged */

/* Set by amCreateAudioMgr. */
static s32 musOutputRate; /* samples per second: 22047 */
static s32 musFrameSize;  /* samples in one alAudioFrame call: the ROM's frame, 1104 */
static Acmd *musCmdList;  /* the command list passed to alAudioFrame */
static s32 musStarted;    /* the synthesizer exists */
static s32 musStreaming;  /* a host audio thread calls musAudioRender */

/* The samples in musBuffer that the host has not taken yet. */
static s32 musBufferPos;
static s32 musBufferCount;

/* The audio clock without an audio thread. */
static f64 musClockRetraces; /* game time given to musAudioAdvance, in video retraces */
static s64 musClockSamples;  /* samples made since amCreateAudioMgr */

/* The lock's nesting depth and the FPU mode of the thread that holds it. */
static s32 musLockDepth;
static u32 musLockSavedCsr;

/* ------------------------------------------------------------------------
 * The lock and the FPU mode
 * ---------------------------------------------------------------------- */

#if defined(__SSE_MATH__)
static u32 musFpuFlushToZero(void) {
    u32 csr = __builtin_ia32_stmxcsr();

    __builtin_ia32_ldmxcsr(csr | MUS_MXCSR_FLUSH_TO_ZERO);
    return csr;
}

static void musFpuRestore(u32 csr) {
    __builtin_ia32_ldmxcsr(csr);
}
#elif defined(__aarch64__)
/* FPCR bit FZ. Unlike the MXCSR's bit, it makes inputs too small for a normal float zero as well. */
#define MUS_FPCR_FLUSH_TO_ZERO (1u << 24)

static u32 musFpuFlushToZero(void) {
    unsigned long fpcr;

    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr | MUS_FPCR_FLUSH_TO_ZERO));
    return (u32)fpcr;
}

static void musFpuRestore(u32 csr) {
    unsigned long fpcr = csr;

    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
}
#else
/* x87 arithmetic has no flush-to-zero mode. */
static u32 musFpuFlushToZero(void) {
    return 0;
}

static void musFpuRestore(u32 csr) {
}
#endif

/*
 * The N64's threads run with flush-to-zero on (FPCSR 0x01000800, set by
 * osInitialize). libmus and the synthesizer run inside this lock. The lock
 * sets that mode on the thread that holds it. It restores the thread's
 * own mode when the lock is released. The game's other code keeps the host's
 * default mode. Only the thread that holds the lock touches the depth and
 * the saved mode.
 */
void musLock(void) {
    port_audio_lock();
    if (musLockDepth++ == 0) {
        musLockSavedCsr = musFpuFlushToZero();
    }
}

void musUnlock(void) {
    if (--musLockDepth == 0) {
        musFpuRestore(musLockSavedCsr);
    }
    port_audio_unlock();
}

/* ------------------------------------------------------------------------
 * Output rate
 * ---------------------------------------------------------------------- */

/* The video clock for the TV type (osViClock, set by osInitialize). */
static s32 musViClock(void) {
    if (osTvType == MUS_TV_PAL) {
        return MUS_VI_CLOCK_PAL;
    }
    if (osTvType == MUS_TV_MPAL) {
        return MUS_VI_CLOCK_MPAL;
    }
    return MUS_VI_CLOCK_NTSC;
}

/*
 * The output rate that osAiSetFrequency (801CE300) returns for a requested
 * rate. The AI divides the video clock by a whole number. 22050 Hz becomes
 * 22047 Hz on NTSC. Like osAiSetFrequency, it returns -1 when the divisor is
 * too small.
 */
s32 musAiFrequency(s32 requested) {
    s32 clock = musViClock();
    u32 divisor = musFloatToU32((f32)clock / (f32)(u32)requested + 0.5f);

    if (divisor < MUS_AI_MIN_DIVISOR) {
        return -1;
    }
    return clock / (s32)divisor;
}

/* ------------------------------------------------------------------------
 * Wave data
 * ---------------------------------------------------------------------- */

/*
 * A request for wave data that the port cannot serve. Glover never makes
 * one. The ROM read a 64DD address from the disk drive.
 */
static s32 musDmaBad(const char *what, s32 addr, s32 len) {
    if (musDmaBadRequests++ == 0) {
        port_log("libmus: wave data at %08X (%d bytes) is %s; it plays as zeros", (u32)addr, len, what);
    }
    if (len > (s32)sizeof(musDmaZeros)) {
        port_fatal("libmus: a wave data request of %d bytes", len);
    }
    return (s32)PORT_P2W(musDmaZeros);
}

/*
 * The DMA itself (the ROM's osEPiStartDma): copies size bytes of the ROM
 * image from the cartridge address addr. Bytes past the end of the image
 * read as zeros. They are never played: a buffer is read whole, but only the
 * requested bytes are used.
 */
static void musDmaCopy(u8 *dest, u32 addr, s32 size) {
    u32 offset = addr & MUS_PI_ROM_MASK;
    s32 inside = 0;

    if (offset < port_rom_size) {
        inside = port_rom_size - offset < (u32)size ? (s32)(port_rom_size - offset) : size;
        memcpy(dest, port_rom + offset, inside);
    }
    memset(dest + inside, 0, size - inside);
}

/*
 * 801C33A4 (amDmaCallback): the synthesizer's DMA routine (an ALDMAproc). It
 * returns the address of len bytes of wave data at addr, a cartridge address.
 * The address is a word, as in the audio commands (src/port/ptr32.h). The
 * bytes keep the ROM's byte order. Like the ROM's routine, it ignores state.
 *
 * When a used buffer holds all the bytes, it returns their address in that
 * buffer. Otherwise it takes a free buffer, fills it from the even address at
 * or below addr, and puts it in the used list, which stays in the order of
 * the buffers' start addresses. The ROM's DMA moved 2-byte units.
 */
static s32 musDmaRead(s32 addr, s32 len, void *state) {
    MusDmaBuffer *firstUsed = musDmaState.firstUsed;
    MusDmaBuffer *last = NULL;
    MusDmaBuffer *dma;
    u32 start = (u32)addr;
    u32 odd;

    if ((start & MUS_WAVE_DISK_TAG) == MUS_WAVE_DISK_TAG) {
        /* The ROM read these from the 64DD at (addr & 0xFFFFFF) + 0x140000. */
        return musDmaBad("a 64DD address", addr, len);
    }
    /* The ROM returned the physical address. In the port the address is already a word in memory. */
    if (musControlFlag & MUS_CONTROL_WAVES_IN_RAM) {
        return addr;
    }

    for (dma = firstUsed; dma != NULL; dma = (MusDmaBuffer *)dma->node.next) {
        if (start < dma->startAddr) {
            break;
        }
        last = dma;
        /* The ROM compares the two end addresses signed. Cartridge addresses (0xB...) are all negative. */
        if ((s32)(dma->startAddr + musDmaBufferSize) >= (s32)(start + len)) {
            dma->lastFrame = musDmaFrame;
            return (s32)PORT_P2W(dma->ptr + (start - dma->startAddr));
        }
    }

    dma = musDmaState.firstFree;
    if (dma == NULL) {
        /* The ROM returns the address of the first used record, which holds no wave data. Glover never gets here. */
        if (musDmaBadRequests++ == 0) {
            port_log("libmus: no free DMA buffer for wave data at %08X", start);
        }
        return (s32)PORT_P2W(firstUsed);
    }
    musDmaState.firstFree = (MusDmaBuffer *)dma->node.next;
    alUnlink(&dma->node);
    if (last != NULL) {
        alLink(&dma->node, &last->node);
    } else if (musDmaState.firstUsed != NULL) {
        dma->node.next = &musDmaState.firstUsed->node;
        dma->node.prev = NULL;
        musDmaState.firstUsed->node.prev = &dma->node;
        musDmaState.firstUsed = dma;
    } else {
        musDmaState.firstUsed = dma;
        dma->node.next = NULL;
        dma->node.prev = NULL;
    }

    odd = start & 1;
    start -= odd;
    dma->startAddr = start;
    dma->lastFrame = musDmaFrame;
    musDmaCopy(dma->ptr, start, musDmaBufferSize);
    return (s32)PORT_P2W(dma->ptr + odd);
}

/*
 * 801C35F0 (amClearDma): runs at the start of every frame. A buffer that
 * neither the last frame nor this one used goes back to the free list, after
 * the first free buffer. Then the frame is counted. The ROM first took the
 * messages of the last frame's DMAs; the port's copies need none.
 */
static void musDmaClear(void) {
    MusDmaBuffer *dma = musDmaState.firstUsed;
    MusDmaBuffer *next;

    while (dma != NULL) {
        next = (MusDmaBuffer *)dma->node.next;
        if (dma->lastFrame + 1 < musDmaFrame) {
            if (musDmaState.firstUsed == dma) {
                musDmaState.firstUsed = next;
            }
            alUnlink(&dma->node);
            if (musDmaState.firstFree != NULL) {
                alLink(&dma->node, &musDmaState.firstFree->node);
            } else {
                musDmaState.firstFree = dma;
                dma->node.next = NULL;
                dma->node.prev = NULL;
            }
        }
        dma = next;
    }
    musDmaFrame++;
}

/*
 * 801C35B8: the ALDMANew of the synthesizer's load filters. On its first call
 * every buffer is free. It stores a pointer to the DMA state in *state, the
 * load filter's dmaState word. amCreateAudioMgr links the buffers after this
 * first call, before any DMA.
 */
ALDMAproc amDmaNew(void *state) {
    if (!musDmaState.initialized) {
        musDmaState.firstUsed = NULL;
        musDmaState.firstFree = musDmaBuffers;
        musDmaState.initialized = 1;
    }
    *(PTR32(void) *)state = &musDmaState;
    return musDmaRead;
}

/*
 * The functions that the sound player's structures point to (FN32 members).
 * On an encoded build such a member holds a handle into this table
 * (src/port/ptr32.h).
 */
const PortFn port_audio_fn_table[] = {
    (PortFn)amDmaNew,         /* ALSynConfig.dmaproc, ALSynth.dma */
    (PortFn)musDmaRead,       /* ALLoadFilter.dma */
    (PortFn)musPlayerHandler, /* ALPlayer.handler */
    (PortFn)alAdpcmPull,      /* ALFilter.handler and setParam */
    (PortFn)alRaw16Pull,
    (PortFn)alLoadParam,
    (PortFn)alResamplePull,
    (PortFn)alResampleParam,
    (PortFn)alEnvmixerPull,
    (PortFn)alEnvmixerParam,
    (PortFn)alSavePull,
    (PortFn)alSaveParam,
    (PortFn)alAuxBusPull,
    (PortFn)alAuxBusParam,
    (PortFn)alMainBusPull,
    (PortFn)alMainBusParam,
    (PortFn)alFxPull,
    (PortFn)alFxParam,
    (PortFn)alFxParamHdl,     /* ALFx.paramHandler */
};
const unsigned int port_audio_fn_count = sizeof(port_audio_fn_table) / sizeof(port_audio_fn_table[0]);

/* ------------------------------------------------------------------------
 * Rendering
 * ---------------------------------------------------------------------- */

/*
 * One frame of the synthesizer: the part of the ROM's amHandleFrame that
 * matters to libmus. alAudioFrame first calls the player handler for each
 * period that starts in the frame, then makes the frame. The native
 * alAudioFrame also does the RSP's work: the samples are in buf when it
 * returns.
 *
 * Every frame has musFrameSize samples, the ROM's usual frame (1104 for
 * Glover). The ROM's amHandleFrame added or removed a block of 16 samples now
 * and then to follow the AI. The handler's call times and the time of each
 * change do not depend on the frame size: they are counted in samples from
 * the start. The frame size only decides where the synthesizer splits its
 * work. That can change the rounding of a resampler's position.
 */
static void musRenderFrame(s16 *buf) {
    s32 cmdLen;

    musDmaClear();
    alAudioFrame(musCmdList, &cmdLen, buf, musFrameSize);
    if (cmdLen == 0) {
        /* No player yet. The ROM sent no task, and the AI got no new samples. */
        memset(buf, 0, musFrameSize * MUS_SAMPLE_BYTES);
    }
    musClockSamples += musFrameSize;
}

/*
 * The body of the host's audio callback. It writes frames stereo frames
 * (16-bit, left then right, host byte order) to out. The ROM's audio thread
 * made one frame of about 1104 samples every three video retraces. Here the
 * host asks for samples when it needs them. The synthesizer makes whole
 * frames, and samples left over from one call start the next.
 */
void musAudioRender(s16 *out, s32 frames) {
    s32 n;

    musLock();
    if (!musStarted) {
        memset(out, 0, frames * MUS_SAMPLE_BYTES);
        musUnlock();
        return;
    }
    while (frames > 0) {
        if (musBufferCount == 0) {
            musRenderFrame(musBuffer);
            musBufferPos = 0;
            musBufferCount = musFrameSize;
        }
        n = frames < musBufferCount ? frames : musBufferCount;
        memcpy(out, &musBuffer[musBufferPos * 2], n * MUS_SAMPLE_BYTES);
        out += n * 2;
        frames -= n;
        musBufferPos += n;
        musBufferCount -= n;
    }
    musUnlock();
}

/* Without an audio thread, the frames that musAudioPoll and musAudioAdvance make go here when it is set (the
   library, src/lib/audio.c). They do not change the game's memory. */
void (*port_audio_sink)(const s16 *frames, s32 count);

/*
 * MusAsk and MusHandleAsk call this before they count. The game waits in
 * loops on them until the player has stopped or started a song (map section
 * 12.2). On the N64 the audio thread interrupts such a loop. With a host
 * audio thread this only gives that thread the processor. Without one it runs
 * one frame of the synthesizer and drops its samples, as the audio thread
 * would have made one. A frame calls the player three or four times.
 */
void musAudioPoll(void) {
    if (!musStarted) {
        return;
    }
    if (musStreaming) {
        port_audio_yield();
        return;
    }
    musLock();
    musRenderFrame(musDropBuffer);
    if (port_audio_sink != NULL) {
        port_audio_sink(musDropBuffer, musFrameSize);
    }
    musUnlock();
}

/*
 * Without an audio thread, the game's frame driver calls this once per update
 * with the update's length in video retraces. It runs whole frames of the
 * synthesizer while the audio clock stays at or behind the game's clock, and
 * drops their samples. musFrameRate retraces are one second of samples.
 * Frames that musAudioPoll ran count too. After a wait the audio clock is
 * ahead, and the next updates run fewer frames. The frames depend only on the
 * game's updates. A headless run gives the same memory every time. With an
 * audio thread this does nothing.
 */
void musAudioAdvance(f32 retraces) {
    s64 target;

    if (!musStarted || musStreaming) {
        return;
    }
    musLock();
    musClockRetraces += retraces;
    target = (s64)(musClockRetraces * (f64)musOutputRate / (f64)musFrameRate);
    while (musClockSamples + musFrameSize <= target) {
        musRenderFrame(musDropBuffer);
        if (port_audio_sink != NULL) {
            port_audio_sink(musDropBuffer, musFrameSize);
        }
    }
    musUnlock();
}

/* ------------------------------------------------------------------------
 * Start-up
 * ---------------------------------------------------------------------- */

/* A host pointer for an N64 RAM address. */
static u8 *musRamToHost(u32 address) {
    return port_ram + (address - MUS_RAM_BASE);
}

/*
 * Checks that the port's loader gave libmus the game's effect tables in the
 * form it needs (map section 12.3). The streams must be the ROM's bytes, the
 * list host pointers to them, and the priorities host integers. It compares
 * them with the ROM image and logs what differs. Tables that are not the
 * game's are not checked.
 */
static void musCheckEffectTables(void) {
    u32 listAddress = MUS_ROM_SFX_LIST + MUS_ROM_DATA_RAM_DELTA;
    u32 priorityAddress = MUS_ROM_PRIORITY_LIST + MUS_ROM_DATA_RAM_DELTA;
    s32 badPointers = 0;
    s32 badPriorities = 0;
    s32 i;

    if ((u8 *)musFxStreams != musRamToHost(listAddress) || (u8 *)musFxPriorities != musRamToHost(priorityAddress)
        || port_rom_size < MUS_ROM_PRIORITY_LIST + MUS_SFX_COUNT * 4) {
        return;
    }
    for (i = 0; i < MUS_SFX_COUNT; i++) {
        if (musFxStreams[i] != musRamToHost(musRead32(port_rom + MUS_ROM_SFX_LIST + i * 4))) {
            badPointers++;
        }
        if (musFxPriorities[i] != (s32)musRead32(port_rom + MUS_ROM_PRIORITY_LIST + i * 4)) {
            badPriorities++;
        }
    }
    if (badPointers != 0) {
        port_log("libmus: %d of %d entries of libmus_sfx_list are not host pointers to the streams", badPointers,
                 MUS_SFX_COUNT);
    }
    if (badPriorities != 0) {
        port_log("libmus: %d of %d entries of libmus_priority_list are not host integers", badPriorities,
                 MUS_SFX_COUNT);
    }
    if (memcmp(musRamToHost(MUS_RAM_FX_STREAMS), port_rom + (MUS_RAM_FX_STREAMS - MUS_ROM_DATA_RAM_DELTA),
               listAddress - MUS_RAM_FX_STREAMS) != 0) {
        port_log("libmus: the sound effect streams differ from the ROM's bytes; gen/ must load D_801E7760 as bytes");
    }
}

/*
 * The port's amCreateAudioMgr, with the ROM's arguments. MusInitialize calls
 * it with musLock held.
 *
 * It takes the ROM's blocks from the libmus heap, in the ROM's order and with
 * the ROM's sizes. It uses the DMA buffers and their records, and the first
 * command list. Every later block then has its ROM offset in the heap, and
 * MusInitialize returns the ROM's heap use (the game keeps it in
 * musicMemoryUsed). The ROM also wrote a message into each AudioInfo block.
 * The port leaves the blocks it does not use as they are. sched and
 * threadPriority are not used. MusInitialize still gives every voice the
 * thread priority.
 */
void amCreateAudioMgr(void *sched, ALSynConfig *c, s32 threadPriority, MusAudioMgrConfig *amc,
                      s32 numDmaBuffers, s32 dmaBufferSize, s32 frameRate) {
    ALHeap *heap = c->heap;
    s32 extraSamples;
    f32 exactFrame;
    s32 frameSize;
    s32 largestFrame;
    s32 i;

    extraSamples = amc->outputRate < MUS_EXTRA_RATE ? MUS_EXTRA_LOW : MUS_EXTRA_HIGH;

    c->dmaproc = amDmaNew;
    c->outputRate = musAiFrequency((s32)amc->outputRate);
    if (c->outputRate <= 0) {
        port_fatal("libmus: the audio interface cannot play %u Hz", amc->outputRate);
    }
    musOutputRate = c->outputRate;
    if (numDmaBuffers < 1) {
        port_fatal("libmus: %d DMA buffers", numDmaBuffers);
    }
    musDmaBufferSize = dmaBufferSize;
    musDmaState.initialized = 0;

    /* ROM: the DMA buffers' records, their I/O messages and the DMA queue's slots. */
    musDmaBuffers = (MusDmaBuffer *)alHeapDBAlloc(NULL, 0, heap, 1, numDmaBuffers * MUS_ROM_DMA_BUFFER_SIZE);
    alHeapDBAlloc(NULL, 0, heap, 1, 2 * numDmaBuffers * MUS_ROM_IO_MESG_SIZE);
    alHeapDBAlloc(NULL, 0, heap, 1, 2 * numDmaBuffers * MUS_ROM_MESG_SIZE);

    /*
     * The ROM's frame: framesPerField video fields of samples, rounded up to
     * a whole sample, then up to a whole block (1104 for Glover). The largest
     * frame adds the extra samples and one block (1360).
     */
    exactFrame = (f32)amc->framesPerField * (f32)c->outputRate / (f32)frameRate;
    frameSize = (s32)exactFrame;
    if ((f32)(u32)frameSize < exactFrame) {
        frameSize++;
    }
    if (frameSize & (MUS_FRAME_ALIGN - 1)) {
        frameSize = (frameSize & ~(MUS_FRAME_ALIGN - 1)) + MUS_FRAME_ALIGN;
    }
    largestFrame = extraSamples + frameSize + MUS_FRAME_ALIGN;

    CustomInit(&musGlobals, c);

    /* ROM: the free list of the DMA buffers, in their order, and the data of each. */
    musDmaBuffers[0].node.prev = NULL;
    musDmaBuffers[0].node.next = NULL;
    for (i = 0; i < numDmaBuffers - 1; i++) {
        alLink(&musDmaBuffers[i + 1].node, &musDmaBuffers[i].node);
        musDmaBuffers[i].ptr = (u8 *)alHeapDBAlloc(NULL, 0, heap, 1, dmaBufferSize);
    }
    musDmaBuffers[i].ptr = (u8 *)alHeapDBAlloc(NULL, 0, heap, 1, dmaBufferSize);

    /* ROM: two command lists of maxACMDSize commands. alAudioFrame gets the first. */
    musCmdList = (Acmd *)alHeapDBAlloc(NULL, 0, heap, 1, amc->maxACMDSize * MUS_ROM_ACMD_SIZE);
    alHeapDBAlloc(NULL, 0, heap, 1, amc->maxACMDSize * MUS_ROM_ACMD_SIZE);

    /* ROM: four AudioInfo blocks, each followed by an output buffer for the largest frame. */
    for (i = 0; i < MUS_ROM_AUDIO_INFOS; i++) {
        alHeapDBAlloc(NULL, 0, heap, 1, MUS_ROM_AUDIO_INFO_SIZE);
        alHeapDBAlloc(NULL, 0, heap, 1, largestFrame * MUS_SAMPLE_BYTES);
    }

    /* The port makes every frame at the ROM's usual size. */
    musFrameSize = frameSize;
    if (musFrameSize < MUS_FRAME_ALIGN || musFrameSize > MUS_BUFFER_FRAMES) {
        port_fatal("libmus: a frame of %d samples does not fit the port's buffer", musFrameSize);
    }
    musBufferPos = 0;
    musBufferCount = 0;
    musClockRetraces = 0.0;
    musClockSamples = 0;
    musStarted = 1;

    musCheckEffectTables();

    /* ROM: osCreateThread and osStartThread. */
    if (!musStreaming) {
        musStreaming = port_audio_open(musOutputRate, musAudioRender) != 0;
    }
}
PORT_C_END
