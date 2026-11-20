/* The parts of the N64 operating system (libultra) that the game calls, rewritten for the PC.
   The port runs the game on one thread, one frame at a time (see frame.c). Thread and scheduler calls do
   nothing, and every DMA completes at once. */
PORT_C_BEGIN
#include "port.h"

/* ---- Message queues. A message is posted at once; nothing ever waits. */

void osCreateMesgQueue(OSMesgQueue *mq, MESG32 *msg, s32 count) {
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flags) {
    if (mq == NULL || mq->msg == NULL) {
        return -1;
    }
    if (mq->validCount >= mq->msgCount) {
        return -1;
    }
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    return 0;
}

s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flags) {
    if (mq == NULL || mq->validCount >= mq->msgCount) {
        return -1;
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    return 0;
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flags) {
    static s32 warned;

    if (mq == NULL || mq->validCount == 0) {
        if (flags == OS_MESG_BLOCK && !warned) {
            warned = 1;
            port_log("osRecvMesg: an empty queue would block forever");
        }
        return -1;
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    return 0;
}

void osSetEventMesg(OSEvent e, OSMesgQueue *mq, OSMesg m) {
}

void osViSetEvent(OSMesgQueue *mq, OSMesg m, u32 retraceCount) {
}

/* ---- Threads and interrupts: the port has one thread. */

void osInitialize(void) {
}

void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri) {
}

void osStartThread(OSThread *t) {
}

void osStopThread(OSThread *t) {
}

void osSetThreadPri(OSThread *t, OSPri pri) {
}

OSIntMask osSetIntMask(OSIntMask mask) {
    return 0;
}

int osSetTimer(OSTimer *t, OSTime countdown, OSTime interval, OSMesgQueue *mq, OSMesg msg) {
    return 0;
}

/* ---- Time. The CPU's count register runs at half the 93.75 MHz clock. */

u32 osGetCount(void) {
    return (u32)(port_time_us() * 46875 / 1000);
}

OSTime osGetTime(void) {
    return (OSTime)(port_time_us() * 46875 / 1000);
}

/* ---- Caches: the PC keeps them coherent. */

void osInvalDCache(void *p, s32 n) {
}

void osInvalICache(void *p, s32 n) {
}

void osWritebackDCache(void *p, s32 n) {
}

void osWritebackDCacheAll(void) {
}

/* ---- Cartridge reads. */

void port_dma(void *dst, u32 devAddr, u32 size) {
    u32 rom = devAddr & 0x0FFFFFFF;

    if (rom + size > port_rom_size) {
        port_log("DMA past the end of the ROM: %08X + %X", devAddr, size);
        if (rom >= port_rom_size) {
            return;
        }
        size = port_rom_size - rom;
    }
    memcpy(dst, port_rom + rom, size);
}

void osCreatePiManager(OSPri pri, OSMesgQueue *cmdQ, MESG32 *cmdBuf, s32 cmdMsgCnt) {
}

s32 osPiStartDma(OSIoMesg *mb, s32 priority, s32 direction, u32 devAddr, void *dramAddr, u32 size, OSMesgQueue *mq) {
    port_dma(dramAddr, devAddr, size);
    if (mb != NULL) {
        mb->hdr.retQueue = mq;
        mb->dramAddr = dramAddr;
        mb->devAddr = devAddr;
        mb->size = size;
    }
    osSendMesg(mq, (OSMesg)mb, OS_MESG_NOBLOCK);
    return 0;
}

s32 osPiRawReadIo(u32 devAddr, u32 *data) {
    u32 rom = devAddr & 0x0FFFFFFF;

    *data = rom + 4 <= port_rom_size ? port_be32(port_rom + rom) : 0;
    return 0;
}

/* ---- Video: the port draws each frame's display lists itself (frame.c). */

void osCreateViManager(OSPri pri) {
}

void osViSetMode(OSViMode *mode) {
}

void osViBlack(u8 active) {
}

void osViSetSpecialFeatures(u32 func) {
}

void osViSetYScale(f32 scale) {
}

void osViSwapBuffer(void *frameBufPtr) {
}

extern PTR32(u16) cfb_ptrs[];
extern u8 draw_buffer;

void *osViGetCurrentFramebuffer(void) {
    return cfb_ptrs[draw_buffer ^ 1];
}

void *osViGetNextFramebuffer(void) {
    return cfb_ptrs[draw_buffer];
}

/* ---- RSP tasks: the port's frame loop hands display lists to the host itself. */

void osSpTaskLoad(OSTask *task) {
}

void osSpTaskStartGo(OSTask *task) {
}

void osSpTaskYield(void) {
}

OSYieldResult osSpTaskYielded(OSTask *task) {
    return 0;
}

/* ---- Controllers. Controller 1 is the keyboard or a gamepad (see platform.c). */

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *status) {
    s32 i;

    *bitpattern = 1;
    for (i = 0; i < MAXCONTROLLERS; i++) {
        status[i].type = i == 0 ? CONT_TYPE_NORMAL : 0;
        status[i].status = 0;
        status[i].errno = i == 0 ? 0 : CONT_NO_RESPONSE_ERROR;
    }
    return 0;
}

s32 osContStartReadData(OSMesgQueue *mq) {
    osSendMesg(mq, (OSMesg)0, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetReadData(OSContPad *data) {
    s32 i;

    for (i = 0; i < MAXCONTROLLERS; i++) {
        data[i].button = 0;
        data[i].stick_x = 0;
        data[i].stick_y = 0;
        data[i].errno = CONT_NO_RESPONSE_ERROR;
    }
    port_controller_reads++;
    data[0].button = port_input_read(port_buttons);
    data[0].stick_x = port_stick_x;
    data[0].stick_y = port_stick_y;
    data[0].errno = 0;
}

/* ---- EEPROM: the game's 4 Kbit EEPROM (64 blocks of 8 bytes). The platform layer keeps it in a file. */

u8 port_eeprom[512];
s32 port_eeprom_dirty;

s32 osEepromProbe(OSMesgQueue *mq) {
    return EEPROM_TYPE_4K;
}

s32 osEepromLongRead(OSMesgQueue *mq, u8 address, u8 *buffer, int length) {
    if (address * 8 + length > (int)sizeof(port_eeprom)) {
        return -1;
    }
    memcpy(buffer, port_eeprom + address * 8, length);
    return 0;
}

s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int length) {
    if (address * 8 + length > (int)sizeof(port_eeprom)) {
        return -1;
    }
    memcpy(port_eeprom + address * 8, buffer, length);
    port_eeprom_dirty = 1;
    return 0;
}

s32 osEepromRead(OSMesgQueue *mq, u8 address, u8 *buffer) {
    return osEepromLongRead(mq, address, buffer, 8);
}

s32 osEepromWrite(OSMesgQueue *mq, u8 address, u8 *buffer) {
    return osEepromLongWrite(mq, address, buffer, 8);
}

/* ---- Calls the game makes that the PC needs nothing for. */

s32 osGetThreadPri(OSThread *t) {
    return 0;
}

void osYieldThread(void) {
}

PORT_C_END
