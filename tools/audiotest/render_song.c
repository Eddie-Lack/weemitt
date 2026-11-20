/* Renders Glover's songs and sound effects with the port's sound player and writes them to WAV files, with a log
   of the notes. tools/audiotest/analyze_audio.py then checks the files.

   Usage: tools/audiotest/render.sh ROM OUTDIR      builds the game's objects with AUDIO=mus, links this program
                                                    with them, runs it and runs the checks

          render_song ROM OUTDIR [SECONDS]          SECONDS of each song (default 20)

   The program starts the sound player as the game does. port_load_ram fills the game's memory from the ROM, and
   InitMusicDriver (src/game/dma.c) loads both pbanks and calls MusInitialize and MusBankInitialize. Songs start
   with the game's PrepareSong (src/game/sound.c), which loads the song and calls MusBankStartSong. Sound effects
   start with MusStartEffect2, as sound.c does. The samples come from musAudioRender, which the platform's audio
   callback calls in the game. No sound device is opened.

   The files hold the game's audio. Write them outside the repository.

   Files in OUTDIR:
     song_NN.wav, song_NN.log       the first SECONDS of song NN, and every voice call libmus made
     song_NN_vVV.wav, ...log        the same song with only voice VV audible (the others get volume 0) and
                                    no reverb (every voice gets fx mix 0)
     fx_NNN_left.wav, fx_NNN_right.wav   effect NNN with pan 0 and pan 255
     fx_NNN_dry.wav, fx_NNN_wet.wav      effect NNN in the centre without reverb and with MusHandleSetReverb 60

   A log starts with "# start SAMPLE", the synthesizer's time of the file's first sample. Every other line is
   "sample voice event value note wave drum". sample is the synthesizer's time of the change
   (ALSynth.paramSamples). event is start, stop, vol, pan, fxmix or pitch. For pitch, value is the ratio and note
   is the channel's note number with its transpose (notePitch minus the wave's pitch offset). wave is the wave
   index in the bank. drum is 1 when the channel uses a drum map. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mus_internal.h"

#define OUTPUT_RATE_REQUEST 22050
#define SAMPLE_BYTES 4
#define CHUNK_FRAMES 512 /* frames per musAudioRender call, about what an SDL callback asks for */
#define SYNTH_FRAME 1104 /* samples per synthesizer frame (mus-glue.c); each file is a whole number of them */
#define MAX_VOICES 24
#define SOLO_VOICES 2    /* voices of each song rendered alone */
#define FX_VOLUME 200
#define FX_PAN_LEFT 0
#define FX_PAN_RIGHT 255
#define FX_PAN_CENTRE 128
#define FX_REVERB 60
#define FX_SECONDS 3
#define FLUSH_SECONDS 2 /* silence rendered and dropped before each effect, for the reverb to die out */

/* The game's functions and data (src/game, src/port). */
void port_load_ram(void);
void port_set_rom_symbols(void);
void InitMusicDriver(void);
void PrepareSong(u8 song);
extern s32 osTvType;
extern u8 *port_rom;
extern u32 port_rom_size;

/* The wrapped synthesizer functions (the link uses --wrap for each). */
void __real_alSynStartVoice(ALSynth *s, ALVoice *voice, ALWaveTable *table);
void __real_alSynStopVoice(ALSynth *s, ALVoice *voice);
void __real_alSynSetVol(ALSynth *s, ALVoice *voice, s16 vol, ALMicroTime t);
void __real_alSynSetPan(ALSynth *s, ALVoice *voice, ALPan pan);
void __real_alSynSetFXMix(ALSynth *s, ALVoice *voice, u8 fxmix);
void __real_alSynSetPitch(ALSynth *s, ALVoice *voice, f32 ratio);

static const u8 songs[] = { 25, 1, 5, 9, 13, 17, 21, 28 };
static const s16 effects[] = { 131, 3, 20, 45, 60, 90 };

static FILE *logFile;
static s32 soloVoice = -1;
static s32 starts[MAX_VOICES];
static s32 drumStarts[MAX_VOICES];

/* ---- What the platform layer provides in the game (src/port/platform.c). */

u64 port_time_us(void) {
    return 0;
}

/* One thread: the lock only counts. */
static s32 lockDepth;

void port_audio_lock(void) {
    lockDepth++;
}

void port_audio_unlock(void) {
    lockDepth--;
}

void port_audio_yield(void) {
}

/* No device: libmus runs only when this program asks for samples, and in the game's busy waits. */
s32 port_audio_open(s32 rate, void (*render)(s16 *out, s32 frames)) {
    return 0;
}

/* ---- The note log. */

static s32 voiceIndex(ALVoice *voice) {
    s32 i = (s32)(voice - musVoices);

    return i >= 0 && i < musNumChannels ? i : -1;
}

static void logEvent(ALVoice *voice, const char *event, f64 value) {
    s32 i = voiceIndex(voice);
    MusChannel *ch;
    f32 note = 0.0f;

    if (logFile == NULL || i < 0) {
        return;
    }
    ch = &musChannels[i];
    if (ch->bank != NULL && ch->wave < ch->bank->count) {
        note = ch->notePitch - ch->bank->pitch[ch->wave];
    }
    fprintf(logFile, "%d %d %s %.9g %.4f %u %d\n", musGlobals.drvr.paramSamples, i, event, value, note, ch->wave,
            ch->drumMap != NULL);
}

void __wrap_alSynStartVoice(ALSynth *s, ALVoice *voice, ALWaveTable *table) {
    s32 i = voiceIndex(voice);

    if (i >= 0) {
        starts[i]++;
        if (musChannels[i].drumMap != NULL) {
            drumStarts[i]++;
        }
    }
    logEvent(voice, "start", 0.0);
    __real_alSynStartVoice(s, voice, table);
}

void __wrap_alSynStopVoice(ALSynth *s, ALVoice *voice) {
    logEvent(voice, "stop", 0.0);
    __real_alSynStopVoice(s, voice);
}

void __wrap_alSynSetVol(ALSynth *s, ALVoice *voice, s16 vol, ALMicroTime t) {
    logEvent(voice, "vol", vol);
    if (soloVoice >= 0 && voiceIndex(voice) != soloVoice) {
        vol = 0;
    }
    __real_alSynSetVol(s, voice, vol, t);
}

void __wrap_alSynSetPan(ALSynth *s, ALVoice *voice, ALPan pan) {
    logEvent(voice, "pan", pan);
    __real_alSynSetPan(s, voice, pan);
}

void __wrap_alSynSetFXMix(ALSynth *s, ALVoice *voice, u8 fxmix) {
    logEvent(voice, "fxmix", fxmix);
    if (soloVoice >= 0) {
        fxmix = 0;
    }
    __real_alSynSetFXMix(s, voice, fxmix);
}

void __wrap_alSynSetPitch(ALSynth *s, ALVoice *voice, f32 ratio) {
    logEvent(voice, "pitch", ratio);
    __real_alSynSetPitch(s, voice, ratio);
}

/* ---- WAV files. */

static void put16(FILE *f, u32 v) {
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
}

static void put32(FILE *f, u32 v) {
    put16(f, v & 0xFFFF);
    put16(f, v >> 16);
}

static FILE *openFile(const char *dir, const char *name, const char *mode) {
    char path[1024];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, mode);
    if (f == NULL) {
        fprintf(stderr, "render_song: cannot write %s: %s\n", path, strerror(errno));
        exit(1);
    }
    return f;
}

/*
 * Renders about seconds of stereo frames with musAudioRender into a 16-bit stereo WAV file at the output rate.
 * The file holds whole synthesizer frames. musAudioRender then has no samples left over for the next file, and the
 * file's first sample is the synthesizer's time when this starts.
 */
static void renderWav(const char *dir, const char *name, s32 seconds) {
    static s16 chunk[CHUNK_FRAMES * 2];
    FILE *f = openFile(dir, name, "wb");
    s32 rate = musGlobals.drvr.outputRate;
    s32 total = (seconds * rate + SYNTH_FRAME - 1) / SYNTH_FRAME * SYNTH_FRAME;
    s32 frames = total;
    s32 start = musGlobals.drvr.curSamples;
    s32 n;
    s32 i;

    if (logFile != NULL) {
        fprintf(logFile, "# start %d\n", start);
    }

    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + frames * SAMPLE_BYTES);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1); /* PCM */
    put16(f, 2);
    put32(f, rate);
    put32(f, rate * SAMPLE_BYTES);
    put16(f, SAMPLE_BYTES);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, frames * SAMPLE_BYTES);
    while (frames > 0) {
        n = frames < CHUNK_FRAMES ? frames : CHUNK_FRAMES;
        musAudioRender(chunk, n);
        for (i = 0; i < n * 2; i++) {
            put16(f, (u16)chunk[i]);
        }
        frames -= n;
    }
    fclose(f);
    if (musGlobals.drvr.curSamples - start != total) {
        fprintf(stderr, "render_song: %s: the synthesizer made %d samples for %d\n", name,
                musGlobals.drvr.curSamples - start, total);
        exit(1);
    }
}

/* Renders and drops some seconds, in whole synthesizer frames. */
static void renderSilent(s32 seconds) {
    static s16 chunk[CHUNK_FRAMES * 2];
    s32 frames = (seconds * musGlobals.drvr.outputRate + SYNTH_FRAME - 1) / SYNTH_FRAME * SYNTH_FRAME;
    s32 n;

    while (frames > 0) {
        n = frames < CHUNK_FRAMES ? frames : CHUNK_FRAMES;
        musAudioRender(chunk, n);
        frames -= n;
    }
}

/* ---- The tests. */

static void loadRom(const char *path) {
    FILE *f = fopen(path, "rb");
    long size;

    if (f == NULL) {
        fprintf(stderr, "render_song: cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    port_rom = malloc(size);
    if (port_rom == NULL || fread(port_rom, 1, size, f) != (size_t)size) {
        fprintf(stderr, "render_song: cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    if (size < 0x1000 || port_rom[0] != 0x80 || port_rom[1] != 0x37) {
        fprintf(stderr, "render_song: %s is not a big-endian (.z64) ROM\n", path);
        exit(1);
    }
    port_rom_size = (u32)size;
}

/* Starts a song as the game does and renders it. With solo >= 0 only that voice is heard. */
static void renderSong(const char *dir, u8 song, s32 seconds, s32 solo) {
    char name[64];

    PrepareSong(0); /* stops the last song and waits until it has stopped */
    soloVoice = solo;
    if (solo < 0) {
        memset(starts, 0, sizeof(starts));
        memset(drumStarts, 0, sizeof(drumStarts));
        snprintf(name, sizeof(name), "song_%02u.log", song);
    } else {
        snprintf(name, sizeof(name), "song_%02u_v%02d.log", song, solo);
    }
    logFile = openFile(dir, name, "w");
    PrepareSong(song);
    if (solo < 0) {
        snprintf(name, sizeof(name), "song_%02u.wav", song);
    } else {
        snprintf(name, sizeof(name), "song_%02u_v%02d.wav", song, solo);
    }
    renderWav(dir, name, seconds);
    fclose(logFile);
    logFile = NULL;
    soloVoice = -1;
}

/* The voices that started the most notes without a drum map, best first. */
static void pickSoloVoices(s32 *picked, s32 count) {
    s32 used[MAX_VOICES] = { 0 };
    s32 k;
    s32 i;

    for (k = 0; k < count; k++) {
        s32 best = -1;

        for (i = 0; i < musNumChannels && i < MAX_VOICES; i++) {
            if (!used[i] && drumStarts[i] == 0 && starts[i] >= 4 && (best < 0 || starts[i] > starts[best])) {
                best = i;
            }
        }
        picked[k] = best;
        if (best >= 0) {
            used[best] = 1;
        }
    }
}

/* Renders an effect after a flush of silence. Returns its handle. */
static void renderEffect(const char *dir, s16 number, s32 pan, s32 reverb, const char *what) {
    char name[64];
    s32 handle;

    MusStop(MUSFLAG_EFFECTS | MUSFLAG_SONGS, 0);
    renderSilent(FLUSH_SECONDS);
    snprintf(name, sizeof(name), "fx_%03d_%s.log", number, what);
    logFile = openFile(dir, name, "w");
    handle = MusStartEffect2(number, FX_VOLUME, pan, 0, -1);
    MusHandleSetReverb(handle, reverb);
    snprintf(name, sizeof(name), "fx_%03d_%s.wav", number, what);
    renderWav(dir, name, FX_SECONDS);
    fclose(logFile);
    logFile = NULL;
}

int main(int argc, char **argv) {
    const char *dir;
    s32 seconds = 20;
    s32 solo[SOLO_VOICES];
    u32 i;
    s32 k;

    if (argc < 3) {
        fprintf(stderr, "usage: render_song ROM OUTDIR [SECONDS]\n");
        return 2;
    }
    loadRom(argv[1]);
    dir = argv[2];
    if (argc > 3) {
        seconds = atoi(argv[3]);
    }

    /* port_game_init's first steps, then the game's own start-up of the sound player (MainProc calls it). */
    port_load_ram();
    port_set_rom_symbols();
    osTvType = MUS_TV_NTSC;
    InitMusicDriver();
    printf("output rate %d Hz, heap use %ld bytes\n", musGlobals.drvr.outputRate,
           (long)(musHeap.cur - musHeap.base));

    for (i = 0; i < sizeof(songs); i++) {
        renderSong(dir, songs[i], seconds, -1);
        pickSoloVoices(solo, SOLO_VOICES);
        printf("song %u:", songs[i]);
        for (k = 0; k < MAX_VOICES; k++) {
            if (starts[k] != 0) {
                printf(" v%d=%d%s", k, starts[k], drumStarts[k] ? "d" : "");
            }
        }
        printf("\n");
        for (k = 0; k < SOLO_VOICES; k++) {
            if (solo[k] >= 0) {
                renderSong(dir, songs[i], seconds, solo[k]);
            }
        }
    }
    PrepareSong(0);

    for (i = 0; i < sizeof(effects) / sizeof(effects[0]); i++) {
        renderEffect(dir, effects[i], FX_PAN_LEFT, 0, "left");
        renderEffect(dir, effects[i], FX_PAN_RIGHT, 0, "right");
        renderEffect(dir, effects[i], FX_PAN_CENTRE, 0, "dry");
        renderEffect(dir, effects[i], FX_PAN_CENTRE, FX_REVERB, "wet");
    }

    if (lockDepth != 0) {
        fprintf(stderr, "render_song: the lock depth is %d at the end\n", lockDepth);
        return 1;
    }
    return 0;
}
