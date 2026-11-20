/* Written for weemitt: the parts of the N64 system interface that the port's code uses. */
#ifndef WEEMITT_PR_OS_H
#define WEEMITT_PR_OS_H

#include <PR/ultratypes.h>

/* PTR32 and MESG32 come from the port's ptr32.h. They mark the members that hold a 4-byte pointer or message in the
   game's memory. */

typedef s32 OSId;
typedef s32 OSPri;
typedef u32 OSEvent;
typedef u32 OSIntMask;
typedef u64 OSTime;
typedef void *OSMesg;

/* Threads. */

typedef union {
    struct {
        f32 f_odd;
        f32 f_even;
    } f;
    f64 d;
} __OSfp;

/* The registers that a thread keeps while it does not run. */
typedef struct {
    u64 at;
    u64 v0, v1;
    u64 a0, a1, a2, a3;
    u64 t0, t1, t2, t3, t4, t5, t6, t7;
    u64 s0, s1, s2, s3, s4, s5, s6, s7;
    u64 t8, t9;
    u64 gp, sp, s8, ra;
    u64 lo, hi;
    u32 sr;
    u32 pc;
    u32 cause;
    u32 badvaddr;
    u32 rcp;
    u32 fpcsr;
    __OSfp fp0, fp2, fp4, fp6, fp8, fp10, fp12, fp14;
    __OSfp fp16, fp18, fp20, fp22, fp24, fp26, fp28, fp30;
} __OSThreadContext;

typedef struct OSThread_s {
    PTR32(struct OSThread_s) next;
    OSPri priority;
    PTR32(PTR32(struct OSThread_s)) queue;
    PTR32(struct OSThread_s) tlnext;
    u16 state;
    u16 flags;
    OSId id;
    int fp;
    __OSThreadContext context;
} OSThread;

#define OS_PRIORITY_PIMGR 150
#define OS_PRIORITY_VIMGR 254

void osCreateThread(OSThread *thread, OSId id, void (*entry)(void *), void *arg, void *stackTop, OSPri pri);
void osStartThread(OSThread *thread);
void osStopThread(OSThread *thread);
void osYieldThread(void);
OSPri osGetThreadPri(OSThread *thread);
void osSetThreadPri(OSThread *thread, OSPri pri);

/* Message queues. The last argument of the send and receive calls is one of the two OS_MESG flags. */

typedef struct OSMesgQueue_s {
    PTR32(OSThread) mtqueue;
    PTR32(OSThread) fullqueue;
    s32 validCount;
    s32 first;
    s32 msgCount;
    PTR32(MESG32) msg;
} OSMesgQueue;

#define OS_MESG_NOBLOCK 0
#define OS_MESG_BLOCK 1

void osCreateMesgQueue(OSMesgQueue *mq, MESG32 *buf, s32 count);
s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag);
s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag);
s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag);

/* Events and the interrupt mask. */

#define OS_EVENT_SP 4
#define OS_EVENT_SI 5
#define OS_EVENT_DP 9
#define OS_EVENT_PRENMI 14

#define OS_IM_NONE 0x00000001

/* tools/gen/gen_types.py reads the types of the game's data from the extern declarations in this file. It is the
   only user of this one, of the two device handle pointers and of the three single video modes below. */
extern OSIntMask __OSGlobalIntMask;

void osSetEventMesg(OSEvent event, OSMesgQueue *mq, OSMesg msg);
OSIntMask osSetIntMask(OSIntMask mask);

/* Timers and the clock. */

typedef struct OSTimer_s {
    PTR32(struct OSTimer_s) next;
    PTR32(struct OSTimer_s) prev;
    OSTime interval;
    OSTime value;
    PTR32(OSMesgQueue) mq;
    MESG32 msg;
} OSTimer;

extern u64 osClockRate;

u32 osGetCount(void);
OSTime osGetTime(void);
int osSetTimer(OSTimer *timer, OSTime start, OSTime interval, OSMesgQueue *mq, OSMesg msg);

/* Reading from the cartridge. The game only stores pointers to a device handle, so the handle has no members here. */

typedef struct OSPiHandle_s OSPiHandle;

typedef struct {
    u16 type;
    u8 pri;
    u8 status;
    PTR32(OSMesgQueue) retQueue;
} OSIoMesgHdr;

typedef struct {
    OSIoMesgHdr hdr;
    PTR32(void) dramAddr;
    u32 devAddr;
    u32 size;
    PTR32(OSPiHandle) piHandle;
} OSIoMesg;

extern OSPiHandle *__osPiTable;
extern OSPiHandle *__osDiskHandle;

void osCreatePiManager(OSPri pri, OSMesgQueue *cmdQueue, MESG32 *cmdBuf, s32 cmdCount);
s32 osPiStartDma(OSIoMesg *mb, s32 pri, s32 direction, u32 devAddr, void *dramAddr, u32 size, OSMesgQueue *mq);
s32 osPiRawReadIo(u32 devAddr, u32 *data);

/* Video. */

typedef struct {
    u32 ctrl;
    u32 width;
    u32 burst;
    u32 vSync;
    u32 hSync;
    u32 leap;
    u32 hStart;
    u32 xScale;
    u32 vCurrent;
} OSViCommonRegs;

typedef struct {
    u32 origin;
    u32 yScale;
    u32 vStart;
    u32 vBurst;
    u32 vIntr;
} OSViFieldRegs;

typedef struct {
    u8 type;
    OSViCommonRegs comRegs;
    OSViFieldRegs fldRegs[2];
} OSViMode;

/* A value of osTvType. */
#define OS_TV_NTSC 1

extern s32 osTvType;
extern OSViMode osViModeTable[];
extern OSViMode osViModeNtscLan1;
extern OSViMode osViModePalLan1;
extern OSViMode osViModeMpalLan1;

void osCreateViManager(OSPri pri);
void osViSetMode(OSViMode *mode);
void osViSetEvent(OSMesgQueue *mq, OSMesg msg, u32 retraceCount);
void osViSetYScale(f32 scale);
void osViSetSpecialFeatures(u32 features);
void osViSwapBuffer(void *frameBuffer);
void osViBlack(u8 active);
void *osViGetCurrentFramebuffer(void);
void *osViGetNextFramebuffer(void);

/* Controllers. */

#define MAXCONTROLLERS 4

#define CONT_TYPE_NORMAL 0x0005
#define CONT_NO_RESPONSE_ERROR 0x8

/* The bits of OSContPad.button. */
#define A_BUTTON 0x8000
#define B_BUTTON 0x4000
#define Z_TRIG 0x2000
#define START_BUTTON 0x1000
#define L_JPAD 0x0200
#define R_JPAD 0x0100
#define L_TRIG 0x0020
#define R_TRIG 0x0010
#define L_CBUTTONS 0x0002
#define R_CBUTTONS 0x0001

typedef struct {
    u16 type;
    u8 status;
    u8 errno;
} OSContStatus;

typedef struct {
    u16 button;
    s8 stick_x;
    s8 stick_y;
    u8 errno;
} OSContPad;

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *status);
s32 osContStartReadData(OSMesgQueue *mq);
void osContGetReadData(OSContPad *pad);

/* The save memory (EEPROM). */

#define EEPROM_TYPE_4K 0x01

s32 osEepromProbe(OSMesgQueue *mq);
s32 osEepromRead(OSMesgQueue *mq, u8 address, u8 *buffer);
s32 osEepromWrite(OSMesgQueue *mq, u8 address, u8 *buffer);
s32 osEepromLongRead(OSMesgQueue *mq, u8 address, u8 *buffer, int length);
s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int length);

/* The CPU's caches. */

void osInvalDCache(void *addr, s32 size);
void osInvalICache(void *addr, s32 size);
void osWritebackDCache(void *addr, s32 size);
void osWritebackDCacheAll(void);

/* The address that a display list command carries for a pointer into the game's memory. */
#ifdef PORT
#define OS_K0_TO_PHYSICAL(x) ((u32)PORT_P2W(x) - 0x80000000)
#else
#define OS_K0_TO_PHYSICAL(x) ((u32)((char *)(x) - 0x80000000))
#endif

/* Memory and string helpers. The counts are int. */

void bcopy(const void *src, void *dst, int count);
int bcmp(const void *a, const void *b, int count);
void bzero(void *dst, int count);
int sprintf(char *dst, const char *fmt, ...);

#endif
