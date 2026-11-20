#!/usr/bin/env python3
"""Runs the real ROM in an emulator with one input per controller read and records memory words at each read.

    from emurun import emu_run
    records, vis = emu_run(rom, inputs, watch, reads, writes=[])

The emulator is BizHawk's mupen64plus core (EMULATOR "bizhawk", the default), built by tools/sync/bizhawk/setup.sh,
or the system's mupen64plus ("mupen64plus"). $GLOVER_EMULATOR chooses. docs/sync.md, "The emulators", compares them.

inputs: a list of (buttons, x, y) in the N64's controller word (engine.h's BTN_ bits). Read k gets inputs[k], or no
input past the end. watch: a list of N64 addresses of 32-bit words, or of (pointer address, offset) pairs: the word
at the pointer's target plus the offset. writes: (read index, N64 address, size, value) to write just before that
read. reads: how many controller reads to run. Returns, for each read, the watched words just before it and the video
frame it came in.

The system's mupen64plus needs gloveworld's tools/emu/setup.sh, a C compiler and the mupen64plus headers (emu_run
builds build/sync/pollinput.so and vicount.so), and a display (Xvfb :99). The code follows gloveworld's
tools/emu/emu.py.
"""
import ctypes
import os
import shutil
import subprocess
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORE = os.environ.get('M64_CORE', '/usr/lib/x86_64-linux-gnu/libmupen64plus.so.2')
PLUGINS = os.environ.get('M64_PLUGINS', '/usr/lib/x86_64-linux-gnu/mupen64plus')
GLOVEWORLD = os.environ.get('GLOVEWORLD', os.path.join(ROOT, '..', '..', 'gloveworld'))
GW_EMU = os.path.join(GLOVEWORLD, 'tools', 'emu', 'build')
POLLINPUT = os.path.join(ROOT, 'build', 'sync', 'pollinput.so')
VICOUNT = os.path.join(ROOT, 'build', 'sync', 'vicount.so')
# BizHawk's Mupen64Plus core runs two video frames before a movie's first frame (N64.cs in BizHawk's source).
BIZHAWK_FIRST_VI = 2
EMULATOR = os.environ.get('GLOVER_EMULATOR', 'bizhawk')
# The EEPROM's bytes at power-on: BizHawk's core fills it with zeros (init_saveram), the system's mupen64plus with
# 0xFF. Events scripts carry the value ("#eeprom-fill", src/run.h).
EEPROM_FILL = 0x00 if EMULATOR == 'bizhawk' else 0xFF
BIZHAWK = os.path.join(ROOT, 'build', 'bizhawk')

CMD_ROM_OPEN, CMD_ROM_CLOSE, CMD_EXECUTE, CMD_STOP = 1, 2, 5, 6
CMD_SET_FRAME_CALLBACK, CMD_STATE_SET = 15, 17
# BizHawk's core: one video frame at a time, and a callback at each vertical interrupt.
CMD_ADVANCE_FRAME, CMD_SET_VI_CALLBACK = 20, 21
CORE_SPEED_LIMITER = 5
M64TYPE_INT = 1
M64TYPE_STRING = 4
PLUGIN_RSP, PLUGIN_GFX, PLUGIN_AUDIO, PLUGIN_INPUT = 1, 2, 3, 4

DEBUGCB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p)
STATECB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_int)
FRAMECB = ctypes.CFUNCTYPE(None, ctypes.c_uint)
VOIDCB = ctypes.CFUNCTYPE(None)


def m64p_buttons(buttons, x, y):
    """The N64's controller word in mupen64plus's BUTTONS layout: the two bytes of the buttons swapped, then the
    stick."""
    swapped = ((buttons >> 8) | (buttons << 8)) & 0xFFFF
    return swapped | (x & 0xFF) << 16 | (y & 0xFF) << 24


def build_plugins():
    """Builds the two plugins when they are missing or older than their source."""
    for lib in (POLLINPUT, VICOUNT):
        src = os.path.join(ROOT, 'tools', 'sync', os.path.basename(lib)[:-3] + '.c')
        if not os.path.exists(lib) or os.path.getmtime(lib) < os.path.getmtime(src):
            os.makedirs(os.path.dirname(lib), exist_ok=True)
            subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-I/usr/include/mupen64plus', '-o', lib, src, '-ldl'],
                           check=True)


def emu_run(rom, inputs, watch, reads, writes=(), gfx='rice', verbose=False, by_vi=False, max_vi=None):
    """by_vi: inputs holds one entry per video frame (a BizHawk movie, entry 0 after BIZHAWK_FIRST_VI video frames)
    instead of one per read. max_vi: stop after this many video frames too. The video frame of a read is the count
    of vertical interrupts before it."""
    if EMULATOR == 'bizhawk':
        return emu_run_bizhawk(rom, inputs, watch, reads, writes, verbose, by_vi, max_vi)
    build_plugins()
    os.environ.setdefault('DISPLAY', ':99')
    os.environ.setdefault('SDL_AUDIODRIVER', 'dummy')
    core = ctypes.CDLL(CORE)

    def debug(ctx, level, msg):
        if verbose or level <= 1:
            print('m64p:', msg.decode(errors='replace'))

    debugcb = DEBUGCB(debug)
    statecb = STATECB(lambda ctx, p, v: None)
    cfg = os.path.join(ROOT, 'build', 'sync', 'cfg')
    os.makedirs(cfg, exist_ok=True)
    if core.CoreStartup(0x020001, cfg.encode(), b'/usr/share/games/mupen64plus', None, debugcb, None, statecb):
        raise RuntimeError('CoreStartup failed')
    # The EEPROM starts blank (0xFF), as the engine's does: mupen64plus keeps save files in SaveSRAMPath, here a new
    # empty folder.
    section = ctypes.c_void_p()
    saves = tempfile.mkdtemp(prefix='glover-eeprom-')
    core.ConfigOpenSection(b'Core', ctypes.byref(section))
    core.ConfigSetParameter(section, b'SaveSRAMPath', M64TYPE_STRING, ctypes.c_char_p(saves.encode()))
    # BizHawk's movies use the pure interpreter (0). The interpreter (1) and the dynamic recompiler (2) give the same
    # timing here; $GLOVER_R4300 picks one.
    mode = ctypes.c_int(int(os.environ.get('GLOVER_R4300', '0')))
    core.ConfigSetParameter(section, b'R4300Emulator', M64TYPE_INT, ctypes.byref(mode))
    data = open(rom, 'rb').read()
    rombuf = ctypes.create_string_buffer(data, len(data))
    if core.CoreDoCommand(CMD_ROM_OPEN, len(data), rombuf):
        raise RuntimeError('cannot open the ROM')
    if gfx == 'angrylion':
        plugins = [(PLUGIN_GFX, os.path.join(GW_EMU, 'mupen64plus-video-angrylion-plus.so')),
                   (PLUGIN_RSP, os.path.join(PLUGINS, 'mupen64plus-rsp-z64.so'))]
    else:
        plugins = [(PLUGIN_GFX, os.path.join(PLUGINS, 'mupen64plus-video-rice.so')),
                   (PLUGIN_RSP, os.path.join(PLUGINS, 'mupen64plus-rsp-hle.so'))]
    # Without an audio plugin the game's main loop never runs (gloveworld's tools/emu notes).
    plugins.insert(1, (PLUGIN_AUDIO, os.path.join(PLUGINS, 'mupen64plus-audio-sdl.so')))
    plugins.insert(2, (PLUGIN_INPUT, POLLINPUT))
    # The video plugin goes through vicount, which counts video frames. pollinput finds vicount_value through the
    # global symbols.
    vic = ctypes.CDLL(VICOUNT, mode=ctypes.RTLD_GLOBAL)
    if not vic.vicount_load(plugins[0][1].encode()):
        raise RuntimeError('cannot load ' + plugins[0][1])
    plugins[0] = (PLUGIN_GFX, VICOUNT)
    libs = [vic]
    pin = None
    for kind, path in plugins:
        lib = vic if path == VICOUNT else ctypes.CDLL(path)
        lib.PluginStartup(ctypes.c_void_p(core._handle), None, debugcb)
        if core.CoreAttachPlugin(kind, ctypes.c_void_p(lib._handle)):
            raise RuntimeError('cannot attach ' + path)
        libs.append(lib)
        if kind == PLUGIN_INPUT:
            pin = lib
    table = (ctypes.c_uint32 * max(len(inputs), 1))(*[m64p_buttons(*i) for i in inputs])
    addrs = [w[0] if isinstance(w, tuple) else w for w in watch]
    offs = [w[1] if isinstance(w, tuple) else 0xFFFFFFFF for w in watch]
    wtab = (ctypes.c_uint32 * max(len(watch), 1))(*[a & 0x7FFFFF for a in addrs])
    otab = (ctypes.c_uint32 * max(len(watch), 1))(*offs)
    flat = [v & 0xFFFFFFFF for w in writes for v in w]
    wrtab = (ctypes.c_uint32 * max(len(flat), 1))(*flat)
    log = (ctypes.c_uint32 * (max(len(watch), 1) * reads))()
    vilog = (ctypes.c_uint32 * reads)()
    pin.pollinput_set(table, len(inputs), wtab, otab, len(watch), wrtab, len(writes), log, vilog, reads)
    pin.pollinput_by_vi(1 if by_vi else 0, BIZHAWK_FIRST_VI)

    def frame(index):
        if pin.pollinput_reads() > reads or (max_vi is not None and vic.vicount_value() > max_vi):
            core.CoreDoCommand(CMD_STOP, 0, None)

    # The SDL audio plugin's libraries turn on the SSE flags that flush denormals; BizHawk's emulator thread has
    # them off (pollinput.c). The emulator runs on this thread.
    pin.pollinput_fpu()
    framecb = FRAMECB(frame)
    core.CoreDoCommand(CMD_SET_FRAME_CALLBACK, 0, framecb)
    off = ctypes.c_int(0)
    core.CoreDoCommand(CMD_STATE_SET, CORE_SPEED_LIMITER, ctypes.byref(off))
    core.CoreDoCommand(CMD_EXECUTE, 0, None)
    core.CoreDoCommand(CMD_ROM_CLOSE, 0, None)
    for kind in (PLUGIN_RSP, PLUGIN_INPUT, PLUGIN_AUDIO, PLUGIN_GFX):
        core.CoreDetachPlugin(kind)
    resets = pin.pollinput_fpu()
    if resets:
        print('emurun: the SSE flush flags came back on at %d reads' % resets)
    core.CoreShutdown()
    shutil.rmtree(saves, ignore_errors=True)
    n = len(watch)
    done = min(pin.pollinput_reads(), reads)
    records = [list(log[k * n:(k + 1) * n]) for k in range(done)]
    return records, list(vilog)[:done]


def plugin_tables(inputs, watch, writes, reads):
    """The input plugin's tables as ctypes arrays: inputs, watched addresses, offsets, writes, log, video frames."""
    table = (ctypes.c_uint32 * max(len(inputs), 1))(*[m64p_buttons(*i) for i in inputs])
    addrs = [w[0] if isinstance(w, tuple) else w for w in watch]
    offs = [w[1] if isinstance(w, tuple) else 0xFFFFFFFF for w in watch]
    wtab = (ctypes.c_uint32 * max(len(watch), 1))(*[a & 0x7FFFFF for a in addrs])
    otab = (ctypes.c_uint32 * max(len(watch), 1))(*offs)
    flat = [v & 0xFFFFFFFF for w in writes for v in w]
    wrtab = (ctypes.c_uint32 * max(len(flat), 1))(*flat)
    log = (ctypes.c_uint32 * (max(len(watch), 1) * reads))()
    vilog = (ctypes.c_uint32 * reads)()
    return table, wtab, otab, wrtab, log, vilog


def emu_run_bizhawk(rom, inputs, watch, reads, writes, verbose, by_vi, max_vi):
    """emu_run on BizHawk's core, driven as BizHawk 2.11.1 drives it (N64.cs, mupen64plusCoreApi.cs): the pure
    interpreter, a zero-filled EEPROM, the core in a thread of its own, two video frames before the movie, then one
    video frame per movie frame. With by_vi, every read in movie frame f takes inputs[f]."""
    libs = {name: os.path.join(BIZHAWK, name + '.so')
            for name in ('libbhcore', 'libbhrsp', 'libbhaudio', 'nullvideo', 'pollinput')}
    for path in libs.values():
        if not os.path.exists(path):
            raise RuntimeError('%s is missing: run tools/sync/bizhawk/setup.sh' % path)
    # The two small plugins are rebuilt when their source changes, with the core's own API headers.
    api = os.path.join(BIZHAWK, 'src', 'libmupen64plus', 'mupen64plus-core', 'src', 'api')
    for name, src in (('pollinput', os.path.join(ROOT, 'tools', 'sync', 'pollinput.c')),
                      ('nullvideo', os.path.join(ROOT, 'tools', 'sync', 'bizhawk', 'nullvideo.c'))):
        if os.path.getmtime(libs[name]) < os.path.getmtime(src):
            subprocess.run(['gcc', '-O2', '-fPIC', '-ffp-contract=off', '-w', '-I' + api, '-shared', '-o',
                            libs[name], src, '-ldl'], check=True)
    os.environ.setdefault('SDL_AUDIODRIVER', 'dummy')
    core = ctypes.CDLL(libs['libbhcore'])
    core.CoreDoCommand.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]

    def debug(ctx, level, msg):
        if verbose or level <= 1:
            print('core:', msg.decode(errors='replace'))

    debugcb = DEBUGCB(debug)
    cfg = tempfile.mkdtemp(prefix='glover-bizhawk-')
    if core.CoreStartup(0x020001, cfg.encode(), b'', None, debugcb, None, None):
        raise RuntimeError('CoreStartup failed')
    section = ctypes.c_void_p()
    core.ConfigOpenSection(b'Core', ctypes.byref(section))
    pure = ctypes.c_int(0)
    core.ConfigSetParameter(section, b'R4300Emulator', M64TYPE_INT, ctypes.byref(pure))
    core.ConfigSetParameter(section, b'SaveSRAMPath', M64TYPE_STRING, ctypes.c_char_p(cfg.encode()))
    data = open(rom, 'rb').read()
    rombuf = ctypes.create_string_buffer(data, len(data))
    if core.CoreDoCommand(CMD_ROM_OPEN, len(data), rombuf):
        raise RuntimeError('cannot open the ROM')
    # BizHawk's own call: the EEPROM and the other save memories start as zeros.
    core.init_saveram()
    frame_end = threading.Event()
    vicb = VOIDCB(frame_end.set)
    core.CoreDoCommand(CMD_SET_VI_CALLBACK, 0, ctypes.cast(vicb, ctypes.c_void_p))
    # nullvideo gives vicount_value to pollinput through the global symbols.
    video = ctypes.CDLL(libs['nullvideo'], mode=ctypes.RTLD_GLOBAL)
    pin = ctypes.CDLL(libs['pollinput'])
    plugins = [(PLUGIN_GFX, video), (PLUGIN_AUDIO, ctypes.CDLL(libs['libbhaudio'])), (PLUGIN_INPUT, pin),
               (PLUGIN_RSP, ctypes.CDLL(libs['libbhrsp']))]
    for kind, lib in plugins:
        lib.PluginStartup(ctypes.c_void_p(core._handle), None, debugcb)
        if core.CoreAttachPlugin(kind, ctypes.c_void_p(lib._handle)):
            raise RuntimeError('cannot attach plugin %d' % kind)
    table, wtab, otab, wrtab, log, vilog = plugin_tables(inputs, watch, writes, reads)
    pin.pollinput_set(table, len(inputs), wtab, otab, len(watch), wrtab, len(writes), log, vilog, reads)
    pin.pollinput_by_vi(1 if by_vi else 0, BIZHAWK_FIRST_VI)
    pin.pollinput_frame(-1)

    started = threading.Event()
    startcb = VOIDCB(started.set)
    thread = threading.Thread(target=lambda: core.CoreDoCommand(CMD_EXECUTE, 0, ctypes.cast(startcb,
                                                                                            ctypes.c_void_p)),
                              daemon=True)
    thread.start()
    if not started.wait(60):
        raise RuntimeError('the core did not start')

    def advance():
        frame_end.clear()
        core.CoreDoCommand(CMD_ADVANCE_FRAME, 0, None)
        if not frame_end.wait(120):
            raise RuntimeError('a video frame did not end')

    advance()
    advance()
    f = 0
    # One read takes at least one video frame; a load can take hundreds without a read.
    limit = (len(inputs) if by_vi else 40 * reads + 2000)
    if max_vi is not None:
        limit = min(limit, max_vi - BIZHAWK_FIRST_VI)
    while f < limit and pin.pollinput_reads() <= reads:
        if by_vi:
            pin.pollinput_frame(f)
        advance()
        f += 1
    resets = pin.pollinput_fpu()
    if resets:
        print('emurun: the SSE flush flags came back on at %d reads' % resets)
    n = len(watch)
    done = min(pin.pollinput_reads(), reads)
    records = [list(log[k * n:(k + 1) * n]) for k in range(done)]
    vis = list(vilog)[:done]
    # The core waits inside a video frame. Stop it, let it reach the stop, and leave the thread: one run a process.
    core.CoreDoCommand(CMD_STOP, 0, None)
    core.CoreDoCommand(CMD_ADVANCE_FRAME, 0, None)
    thread.join(5)
    shutil.rmtree(cfg, ignore_errors=True)
    return records, vis
