#ifndef SPIKE_H
#define SPIKE_H

#include "types.h"
#include "platform.h"

typedef struct Spike {
    /* 0x00 */ PTR32(struct Spike) next;
    /* 0x04 */ PTR32(struct Spike) prev;
    /* 0x08 */ PTR32(Platform) platform;
} Spike;

typedef struct SpikeList {
    /* 0x00 */ s32 count;
    /* 0x04 */ Spike head;
} SpikeList;

extern SpikeList spikeList;

void UpdateSpikes(void);
void InitSpikeList(void);
void AddSpike(Spike *spike);

#endif
