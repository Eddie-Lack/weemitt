#!/usr/bin/env python3
"""Checks the WAV files and logs that render_song writes (tools/audiotest/render.sh).

Usage: analyze_audio.py OUTDIR

Checks:
  level    every file has sound (RMS above SILENT_RMS)
  clipping no run of CLIP_RUN or more samples at full scale
  stereo   a song whose voices have different pans has different channels; an effect with pan 0 is in the left
           channel only, one whose voices all have pan 120 or more is in the right channel only (after the
           first volume ramp)
  reverb   an effect with MusHandleSetReverb 60 has a longer tail than the same effect without reverb, and the
           tail dies away
  pitch    in the songs rendered with one voice (and no reverb), the fundamental of each held note against its
           pitch ratio and note number. Each note is measured over the same stretch of its wave, so two notes of
           one wave differ only by the frequency scale of their ratios. For each wave, the interval between a
           note's spectrum and the longest note's spectrum must be the interval of their ratios. For a wave
           played at two or more note numbers, an unbent note's fundamental (or, for a bell, its strongest
           partial) must lie on the equal-tempered grid of A = 440 Hz

Prints one line per check and exits with 1 when one fails.
"""
import glob
import os
import sys
import wave

import numpy as np

SILENT_RMS = 30.0
CLIP_LEVEL = 32767
CLIP_RUN = 4
PAN_LEAK = 0.05          # largest energy share of the silent side of a hard-panned effect
HARD_RIGHT = 120         # pans from here up count as hard right
FX_RAMP_SAMPLES = 384    # the first volume ramp of an effect (one player period, rounded up)
REVERB_GAIN = 2.0        # the wet tail must have this many times the energy of the dry one
TAIL_SECONDS = 0.5
REVERB_DECAY = 100.0     # the end of the file must be this many times weaker than the start of the tail
NOTE_MIN_SECONDS = 0.15  # held notes shorter than this are not measured
NOTE_SKIP_SECONDS = 0.04 # the start of a note (attack) is skipped
NOTE_WINDOW_SAMPLES = 1500  # shortest stretch of a wave (at ratio 1) that every note of the wave must play
RATIO_TOLERANCE = 5.0    # cents between the measured and the commanded interval of two notes of one wave
MAX_SHIFT = 600          # the interval search covers this many cents around the commanded interval
SPECTRUM_LOW = 20.0
SPECTRUM_HIGH = 6000.0
GRID_TOLERANCE = 35.0    # cents from the nearest equal-tempered note

failures = []


def report(ok, text):
    print(("ok    " if ok else "FAIL  ") + text)
    if not ok:
        failures.append(text)


def read_wav(path):
    with wave.open(path) as w:
        rate = w.getframerate()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, 2)
    return rate, data.astype(np.float64)


def read_log(path):
    start = 0
    events = []
    with open(path) as f:
        for line in f:
            if line.startswith("# start"):
                start = int(line.split()[2])
                continue
            t, voice, event, value, note, wave_index, drum = line.split()
            events.append((int(t) - start, int(voice), event, float(value), float(note), int(wave_index), int(drum)))
    return events


def longest_clip_run(x):
    clipped = np.abs(x) >= CLIP_LEVEL
    best = run = 0
    for c in clipped:
        run = run + 1 if c else 0
        best = max(best, run)
    return best


def check_levels(path, rate, data):
    rms = np.sqrt((data ** 2).mean())
    report(rms > SILENT_RMS, "level    %-22s RMS %.0f" % (os.path.basename(path), rms))
    runs = max(longest_clip_run(data[:, 0]), longest_clip_run(data[:, 1]))
    full = int((np.abs(data) >= CLIP_LEVEL).sum())
    report(runs < CLIP_RUN, "clipping %-22s longest run %d, %d samples at full scale" %
           (os.path.basename(path), runs, full))


def last_pans(events):
    pans = {}
    for t, voice, event, value, note, wave_index, drum in events:
        if event == "pan":
            pans.setdefault(voice, set()).add(int(value))
    return pans


def check_song_stereo(path, data, events):
    pans = set()
    for values in last_pans(events).values():
        pans |= values
    side = np.sqrt(((data[:, 0] - data[:, 1]) ** 2).mean())
    mid = np.sqrt(((data[:, 0] + data[:, 1]) ** 2).mean())
    differ = np.count_nonzero(data[:, 0] != data[:, 1]) / len(data)
    if len(pans) > 1:
        report(differ > 0.5, "stereo   %-22s pans %s: left and right differ in %.0f%% of the samples, side/mid %.3f" %
               (os.path.basename(path), sorted(pans), 100 * differ, side / mid))


def check_fx_stereo(path, data, events, side):
    """A voice keeps its last volumes and pan across a stop and a start, as in the ROM (audio-synth-map.md, 9.4).
    So a note can start where the voice's previous note was and move to its own pan over one ramp. The check
    skips that ramp."""
    starts = [t for t, voice, event, value, note, wave_index, drum in events if event == "start"]
    first = (starts[0] if starts else 0) + FX_RAMP_SAMPLES
    energy = (data[first:] ** 2).sum(axis=0) + 1.0
    pans = set()
    for values in last_pans(events).values():
        pans |= values
    name = os.path.basename(path)
    if side == "left":
        share = energy[1] / energy.sum()
        report(pans <= {0} and share < PAN_LEAK, "stereo   %-22s pans %s, right share %.4f" %
               (name, sorted(pans), share))
    elif min(pans) >= HARD_RIGHT:
        share = energy[0] / energy.sum()
        report(share < PAN_LEAK, "stereo   %-22s pans %s, left share %.4f" % (name, sorted(pans), share))
    else:
        # The ROM keeps (pan * handlePan) >> 7 & 0x7F: a voice panned right of centre wraps around.
        print("info  stereo   %-22s pans %s (not all hard right), energy L %.3g R %.3g" %
              (name, sorted(pans), energy[0], energy[1]))


def envelope_db(data, rate, block):
    mono = data.mean(axis=1)
    n = len(mono) // block
    rms = np.sqrt((mono[:n * block].reshape(n, block) ** 2).mean(axis=1)) + 1e-9
    return 20 * np.log10(rms / 32768.0)


def check_reverb(dry_path, wet_path):
    rate, dry = read_wav(dry_path)
    _, wet = read_wav(wet_path)
    block = rate // 100
    env = envelope_db(dry, rate, block)
    peak = env.max()
    # The dry sound ends at the last 10 ms block within 40 dB of its peak.
    loud = np.nonzero(env > peak - 40)[0]
    end = (loud[-1] + 1) * block
    tail = int(TAIL_SECONDS * rate)
    if end + tail > len(dry):
        print("info  reverb   %-22s the sound lasts the whole file; not checked" % os.path.basename(dry_path))
        return
    dry_tail = (dry[end:end + tail] ** 2).sum() + 1.0
    wet_tail = (wet[end:end + tail] ** 2).sum() + 1.0
    # The tail must die away: the reverb's feedback is below 1.
    late = (wet[-tail:] ** 2).sum() + 1.0
    report(wet_tail > REVERB_GAIN * dry_tail and late < wet_tail / REVERB_DECAY,
           "reverb   %-22s tail after %.2f s: wet/dry energy %.3g; last %.1f s of the file %.1f dB below the tail" %
           (os.path.basename(wet_path), end / rate, wet_tail / dry_tail, TAIL_SECONDS,
            10 * np.log10(wet_tail / late)))


def fundamental(segment, rate):
    """The fundamental of a held note: the harmonic product spectrum's peak, refined on the spectrum."""
    x = segment * np.hanning(len(segment))
    size = 1 << 17
    spectrum = np.abs(np.fft.rfft(x, size))
    freqs = np.fft.rfftfreq(size, 1.0 / rate)
    lo = int(SPECTRUM_LOW * size / rate)
    hi = int(2000.0 * size / rate)
    hps = np.log(spectrum[:hi] + 1e-9)
    for k in (2, 3):
        hps = hps + np.log(spectrum[::k][:hi] + 1e-9)
    i = lo + int(np.argmax(hps[lo:hi]))
    # Refine on the plain spectrum near the HPS peak, with a parabola through the top three bins.
    w = max(2, int(i * 0.01))
    j = i - w + int(np.argmax(spectrum[i - w:i + w + 1]))
    a, b, c = np.log(spectrum[j - 1:j + 2] + 1e-9)
    offset = 0.5 * (a - c) / (a - 2 * b + c) if a - 2 * b + c != 0 else 0.0
    return (j + offset) * rate / size, spectrum[j] / (spectrum[lo:hi].max() + 1e-9)


def note_segments(events, voice, rate, length):
    """The start of each note of a voice: from its start event until the next pitch change, start or stop.
    Returns (start, end, ratio, note, wave). A pitch change sent with the start counts as the note's pitch."""
    out = []
    ratio = None
    current = None
    for t, v, event, value, n, w, drum in events:
        if v != voice:
            continue
        if event == "pitch" and current is not None and t == current[0]:
            ratio = value
            current = (t, n, w)
            continue
        if current is not None and event in ("pitch", "start", "stop") and ratio is not None and t > current[0]:
            out.append((current[0], t, ratio, current[1], current[2]))
        if event in ("pitch", "start", "stop"):
            current = None
        if event == "pitch":
            ratio = value
        elif event == "start":
            current = (t, n, w)
    if current is not None and ratio is not None:
        out.append((current[0], length, ratio, current[1], current[2]))
    return out


def check_pitch(path, events):
    """The held notes of the solo voice: (song, wave, note, ratio, start, end, samples)."""
    rate, data = read_wav(path)
    voice = int(os.path.basename(path).split("_v")[1].split(".")[0])
    song = os.path.basename(path).split("_v")[0]
    mono = data.mean(axis=1)
    notes = []
    for start, end, ratio, note, wave_index in note_segments(events, voice, rate, len(mono)):
        if end - start >= int(NOTE_MIN_SECONDS * rate):
            notes.append((song, wave_index, note, ratio, start, end, mono))
    return notes


def source_window(notes, rate):
    """The stretch of the wave that every note plays, in samples of the wave at ratio 1. A note at ratio r plays
    wave sample r * k at its output sample k. Measuring each note over the same stretch of the wave compares the
    same sound, scaled in frequency by the ratio. Notes too short for a common stretch are left out."""
    first = NOTE_SKIP_SECONDS * rate
    notes = sorted(notes, key=lambda n: -(n[5] - n[4]) * n[3])
    while notes:
        last = min((end - start) * ratio for song, w, note, ratio, start, end, mono in notes)
        if last - first >= NOTE_WINDOW_SAMPLES:
            return first, min(last, first + 4 * NOTE_WINDOW_SAMPLES), notes
        notes = notes[:-1]
    return None, None, []


def peak(segment, rate):
    """The frequency of the strongest partial, refined with a parabola through the top three bins."""
    x = segment * np.hanning(len(segment))
    size = 1 << 17
    spectrum = np.abs(np.fft.rfft(x, size))
    lo = int(SPECTRUM_LOW * size / rate)
    hi = int(SPECTRUM_HIGH * size / rate)
    j = lo + int(np.argmax(spectrum[lo:hi]))
    a, b, c = np.log(spectrum[j - 1:j + 2] + 1e-9)
    offset = 0.5 * (a - c) / (a - 2 * b + c) if a - 2 * b + c != 0 else 0.0
    return (j + offset) * rate / size


def log_spectrum(segment, rate):
    """The log magnitude spectrum of a segment on a grid of 1 cent from SPECTRUM_LOW to SPECTRUM_HIGH Hz,
    normalised to mean 0 and deviation 1."""
    x = segment * np.hanning(len(segment))
    size = 1 << 17
    spectrum = np.log(np.abs(np.fft.rfft(x, size)) + 1e-3)
    freqs = np.fft.rfftfreq(size, 1.0 / rate)
    cents = np.arange(0, int(1200 * np.log2(SPECTRUM_HIGH / SPECTRUM_LOW)))
    grid = np.interp(SPECTRUM_LOW * 2.0 ** (cents / 1200.0), freqs, spectrum)
    return (grid - grid.mean()) / (grid.std() + 1e-12)


def interval(spectrum, reference, expected):
    """The shift in cents that best lines spectrum up with reference, searched within MAX_SHIFT of expected."""
    n = len(spectrum)
    size = 1 << int(np.ceil(np.log2(2 * n)))
    corr = np.fft.irfft(np.fft.rfft(spectrum, size) * np.conj(np.fft.rfft(reference, size)), size)
    lags = np.arange(size)
    lags[lags >= size // 2] -= size
    window = np.abs(lags - expected) <= MAX_SHIFT
    i = np.nonzero(window)[0][np.argmax(corr[window])]
    a, b, c = corr[(i - 1) % size], corr[i], corr[(i + 1) % size]
    offset = 0.5 * (a - c) / (a - 2 * b + c) if a - 2 * b + c != 0 else 0.0
    return lags[i] + offset


def check_waves(notes, rate):
    """Per wave: each note against the longest one. The interval between their spectra must be the interval of
    their pitch ratios. For a wave played at two or more note numbers, the longest note's fundamental must lie on
    the equal-tempered grid of A = 440 Hz, a whole number of semitones from its note number."""
    by_wave = {}
    for n in notes:
        by_wave.setdefault((n[0], n[1]), []).append(n)
    for (song, wave_index), group in sorted(by_wave.items()):
        first, last, group = source_window(group, rate)
        segments = []
        for song_, w, note, ratio, start, end, mono in group:
            segment = mono[start + int(first / ratio):start + int(last / ratio)]
            if np.sqrt((segment ** 2).mean()) >= SILENT_RMS:
                segments.append((note, ratio, segment))
        if len(segments) < 2:
            continue
        ref_note, ref_ratio, ref_segment = segments[0]
        reference = log_spectrum(ref_segment, rate)
        worst = 0.0
        for note, ratio, segment in segments[1:]:
            expected = 1200 * np.log2(ratio / ref_ratio)
            worst = max(worst, abs(interval(log_spectrum(segment, rate), reference, expected) - expected))
        distinct = len(set(note for note, ratio, segment in segments))
        text = "pitch    %s wave %3d: %2d notes at %d note numbers, worst %.1f cents off the commanded interval" % (
            song, wave_index, len(segments), distinct, worst)
        ok = worst < RATIO_TOLERANCE
        # The grid: a note played without a pitch bend has a ratio of 2 ^ ((note + offset) / 12), with the wave's
        # offset a whole number of semitones (baseNote - 48 in Glover's banks; no wave has a detune).
        unbent = [s for s in segments if abs(12 * np.log2(s[1]) - s[0] - np.round(12 * np.log2(s[1]) - s[0])) < 0.01]
        if distinct >= 2 and unbent:
            # A bell's strongest partial carries its pitch, but it has no harmonic series. Either the fundamental
            # or the strongest partial must lie on the grid.
            note, ratio, segment = unbent[0]
            best = None
            for kind, f in (("fundamental", fundamental(segment, rate)[0]), ("strongest partial", peak(segment, rate))):
                midi = 69 + 12 * np.log2(f / 440.0)
                shift = np.round(midi - note)
                grid = abs(midi - note - shift) * 100
                if best is None or grid < best[0]:
                    best = (grid, kind, f, shift)
            grid, kind, f, shift = best
            ok = ok and grid < GRID_TOLERANCE
            text += "; %s %.2f Hz at ratio 1, %.1f cents off the grid, MIDI note = note + %d" % (
                kind, f / ratio, grid, shift)
        report(ok, text)


def main():
    out = sys.argv[1]
    notes = []
    rate = 0
    for path in sorted(glob.glob(os.path.join(out, "*.wav"))):
        rate, data = read_wav(path)
        check_levels(path, rate, data)
        log = path[:-4] + ".log"
        events = read_log(log) if os.path.exists(log) else []
        name = os.path.basename(path)
        if name.startswith("song_") and "_v" not in name:
            check_song_stereo(path, data, events)
        elif name.endswith("_left.wav"):
            check_fx_stereo(path, data, events, "left")
        elif name.endswith("_right.wav"):
            check_fx_stereo(path, data, events, "right")
        elif "_v" in name:
            notes += check_pitch(path, events)
    for wet in sorted(glob.glob(os.path.join(out, "fx_*_wet.wav"))):
        check_reverb(wet.replace("_wet", "_dry"), wet)
    if notes:
        check_waves(notes, rate)
    print("%d checks failed" % len(failures) if failures else "all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
