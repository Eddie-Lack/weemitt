/* The input recorder and the reader of its files. A recording holds what reaches the game's logic: the length of
   every update, the controller values of every update, the settings that change the logic, and a few checks. The
   headless build replays it (--replay) and gives the same run. The file holds no data from the ROM. Plain C. */
#ifndef PORT_RECORD_H
#define PORT_RECORD_H

#ifdef __cplusplus
extern "C" {
#endif

#define REC_VERSION 1
#define REC_BUILD_ID_SIZE 96

/* What a run needs before it starts. */
typedef struct RecInfo {
    char buildId[REC_BUILD_ID_SIZE]; /* the program that wrote the file: version, system, sound player */
    int spaceDemo;
    int fixedRate;                   /* informational: the updates carry their own lengths */
    long level;                      /* --level (-1: the normal start) */
    int easyBall;                    /* the settings at the start */
    float cameraFollow;
    unsigned romHash;                /* of the ROM as the game sees it (a hash only; the file holds none of it) */
    unsigned eepromHash;
    unsigned char eeprom[512];       /* the save at the start (a save holds no ROM data) */
} RecInfo;

/* The id of this program for RecInfo.buildId. */
const char *rec_build_id(void);
/* A hash (FNV-1a) of some bytes. */
unsigned rec_hash_bytes(const void *data, unsigned long size);

/* Writing. rec_default_path puts weemitt-YYYYMMDD-HHMMSS.rec in the save folder into out; rec_folder gives the
   folder. rec_start returns 0 when the file is open. Every call flushes. */
void rec_default_path(char *out, int size);
void rec_folder(char *out, int size);
int rec_start(const char *path, const RecInfo *info);
int rec_active(void);
const char *rec_path(void);
/* Call before each port_game_update, with the settings and the controller values the update will see. */
void rec_update(float dt, int easyBall, float cameraFollow, unsigned short buttons, signed char x, signed char y);
/* Call after the update, when the game read the controller in it: the word the game received. */
void rec_read(unsigned short received);
/* A check value of the game's state (every REC_CHECK_EVERY updates, see platform.c). */
void rec_check(unsigned hash);
/* Ends the file with the update count and a hash of the game's memory. */
void rec_end(unsigned ramHash);
unsigned long rec_updates(void);

/* Reading. rec_open returns 0 when the file is valid (a message in err otherwise). */
enum { REC_EV_END, REC_EV_UPDATE, REC_EV_SETTINGS, REC_EV_READ, REC_EV_CHECK, REC_EV_FINISH };
typedef struct RecEvent {
    int type;
    float dt;                        /* UPDATE */
    unsigned short buttons;          /* UPDATE: port_buttons; READ: the word received */
    signed char x, y;                /* UPDATE */
    int easyBall;                    /* SETTINGS */
    float cameraFollow;
    unsigned hash;                   /* CHECK, FINISH (the memory hash) */
    unsigned long updates;           /* FINISH */
} RecEvent;

int rec_open(const char *path, RecInfo *info, char *err, int errSize);
/* The next event. REC_EV_END means the file ended with no FINISH record (the run was cut short). */
void rec_next(RecEvent *ev);
void rec_close(void);

#ifdef __cplusplus
}
#endif

#endif
