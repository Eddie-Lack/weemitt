#ifndef NNSCHED_H
#define NNSCHED_H

#include "common.h"

#define NN_SC_MAX_MESGS 8

#define NN_SC_RETRACE_MSG 1
#define NN_SC_PRE_NMI_MSG 3

#define VIDEO_MSG 666
#define RSP_DONE_MSG 667
#define RDP_DONE_MSG 668
#define PRE_NMI_MSG 669

#define NN_SC_SWAPBUFFER 0x40

typedef s16 NNScMsg;

typedef struct NNScClient {
    /* 0x00 */ PTR32(struct NNScClient) next;
    /* 0x04 */ PTR32(OSMesgQueue) msgQ;
} NNScClient;

typedef struct NNScTask {
    /* 0x00 */ PTR32(struct NNScTask) next;
    /* 0x04 */ u32 state;
    /* 0x08 */ u32 flags;
    /* 0x0C */ PTR32(void) framebuffer;
    /* 0x10 */ OSTask list;
    /* 0x50 */ PTR32(OSMesgQueue) msgQ;
    /* 0x54 */ MESG32 msg;
} NNScTask;

typedef struct NNSched {
    /* 0x000 */ NNScMsg retraceMsg;
    /* 0x002 */ NNScMsg prenmiMsg;
    /* 0x004 */ OSMesgQueue audioRequestMQ;
    /* 0x01C */ MESG32 audioRequestBuf[NN_SC_MAX_MESGS];
    /* 0x03C */ OSMesgQueue graphicsRequestMQ;
    /* 0x054 */ MESG32 graphicsRequestBuf[NN_SC_MAX_MESGS];
    /* 0x074 */ OSMesgQueue retraceMQ;
    /* 0x08C */ MESG32 retraceMsgBuf[NN_SC_MAX_MESGS];
    /* 0x0AC */ OSMesgQueue rspMQ;
    /* 0x0C4 */ MESG32 rspMsgBuf[NN_SC_MAX_MESGS];
    /* 0x0E4 */ OSMesgQueue rdpMQ;
    /* 0x0FC */ MESG32 rdpMsgBuf[NN_SC_MAX_MESGS];
    /* 0x11C */ OSMesgQueue waitMQ;
    /* 0x134 */ MESG32 waitMsgBuf[NN_SC_MAX_MESGS];
    /* 0x158 */ OSThread schedulerThread;
    /* 0x308 */ OSThread audioThread;
    /* 0x4B8 */ OSThread graphicsThread;
    /* 0x668 */ PTR32(NNScClient) clientList;
    /* 0x66C */ PTR32(NNScTask) curGraphicsTask;
    /* 0x670 */ PTR32(NNScTask) curAudioTask;
    /* 0x674 */ PTR32(NNScTask) graphicsTaskSuspended;
    /* 0x678 */ u32 firstTime;
} NNSched;

extern NNSched sched;

void nnScCreateScheduler(NNSched *sc, u8 videoMode, u8 numFields);
void nnScEventHandler(NNSched *sc);
void nnScAddClient(NNSched *sc, NNScClient *client, OSMesgQueue *msgQ);
void nnScRemoveClient(NNSched *sc, NNScClient *c);
void nnScEventBroadcast(NNSched *sc, NNScMsg *msg);
void nnScExecuteAudio(NNSched *sc);
void nnScExecuteGraphics(NNSched *sc);
void nnScWaitTaskReady(NNSched *sc, NNScTask *task);
OSMesgQueue *nnScGetAudioMQ(NNSched *sc);
OSMesgQueue *nnScGetGfxMQ(NNSched *sc);

#endif
