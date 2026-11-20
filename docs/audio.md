# Sound

The game plays its music and sound effects with two N64 libraries: libmus, Software Creations' music driver, and
libaudio, libultra's synthesizer. On the N64 the synthesizer builds command lists that the RSP runs with its audio
microcode. The port has all three parts, written from the ROM's machine code:

| Part | Where | Map |
| --- | --- | --- |
| libmus: songs, effects, voices, handles, reverb set-up | `src/audio/mus/` | [audio-libmus-map.md](audio-libmus-map.md) |
| libaudio's synthesizer: voices, timing, the filter chain, the command lists | `src/audio/synth/synth-*.c` | [audio-synth-map.md](audio-synth-map.md) |
| the audio microcode's commands, run on the PC | `src/audio/synth/rsp_audio.c` | [audio-synth-map.md](audio-synth-map.md) |

## How sound reaches the speakers

1. The game calls the `Mus*` functions, as on the N64.
2. SDL's audio callback asks the glue (`src/audio/mus/mus-glue.c`) for samples. The glue makes frames of 1104
   samples at 22047 Hz, the N64's frame size and output rate.
3. For each frame, the synthesizer calls libmus's player once for every sixtieth of a second of sound, applies the voice
   changes at the N64's sample times, and builds the frame's command list.
4. The interpreter runs the list: ADPCM decoding, resampling, the envelope mixer, the reverb, and the mix to
   16-bit stereo.

Two settings scale the sound outside the game's logic. The master volume scales the samples in the audio callback
(`platform.c`). The music and effects volumes scale each voice's volume where libmus sends it to the synthesizer
(`musVoiceUpdateVolumePan`). A channel that has an effect number plays an effect, and the others play a song. See
[menu.md](menu.md).

The sound runs on the audio device's clock, not on the game's frame rate. One recursive lock protects libmus: the
game's calls and the audio callback never run at the same time.

Without a sound device (the headless mode, or `--no-sound`) the player runs on the game's clock instead.

## How it was checked

- `tools/mustest` checks libmus's memory layout against the N64's.
- `tools/audiotest/build.sh` runs the ROM's own audio microcode in a model of the RSP and compares it with the
  interpreter, bit for bit.
- `tools/audiotest/render.sh` renders songs and effects to WAV files and checks pitch, stereo, reverb and timing.
- Against the real game. gloveworld's emulator tools (`tools/emu`) run the ROM with the SDL audio plugin and
  `SDL_AUDIODRIVER=disk`, which writes the sound to a file. To hear one effect on its own, a script patches a small
  routine into RunGame (as `tools/n64ref/emu.py` does) that calls `MusStartEffect2`, or the game's own function,
  at a chosen frame. A second run without the call gives the same music. The difference of the two files is the
  effect. The port renders the same effect with `render_song` or with the same call at the same game frame. The
  effect's loudness and shape can then be compared, as long as both sides use one scale: the emulator's output is
  about 0.4 to 0.8 times the port's, and the same factor applies to the music. This agrees for the ball
  transform sound (effect 176 starting effect 400): in the main menu the two waveforms correlate at 0.96, and in
  the level 10 demo (`HandTransformBall` called at frame 200) the effect and the music have the same ratio and the
  same shape in both. The sound is quiet by design: effect 400 peaks near 5100 at volume 146, against 13000 to
  17000 for the spell voices (effects 265 to 268). Runs of the emulator are not exactly repeatable after the
  first few seconds, so compare a run with its own twin, and look for the shift between the two files first.

What is still approximate is listed in the status sections of the two maps.
