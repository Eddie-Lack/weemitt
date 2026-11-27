"""Checks the window program's menu and settings.ini in a real window (it needs a display: DISPLAY, or Xvfb).

- Where the menu's windows open: with a few windows open, no window lies over another, and every window lies inside
  the game window. With every window open in a small game window, every window still starts inside it.
- The command line's options last for one run: a run with --skip-intro, --max-fps and others writes none of them
  to settings.ini.
- settings.ini puts each value in its section: the keys under [keys], the volumes under [audio].
- The studio (build/studio/weemitt-studio, when it is built): its windows and its bar lie apart, and inside the game
  window.

Usage: python3 tools/menutest.py [--rom ROM] [--exe PATH]
Each run gets its own HOME and XDG_DATA_HOME in a temporary folder.
"""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import regress  # noqa: E402

failures = 0


def report(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures += 1


def run(exe, rom, home, env_extra, args=()):
    env = dict(os.environ, HOME=str(home), XDG_DATA_HOME=str(home / "data"), **env_extra)
    return subprocess.run([exe, "--rom", rom, "--no-sound", *args], env=env, capture_output=True, text=True,
                          errors="replace", timeout=300, cwd=home)


def rects(path):
    lines = path.read_text().splitlines()
    dw, dh = (float(v) for v in lines[0].split()[1:3])
    out = {}
    for line in lines[1:]:
        name, nums = line.split("|")
        out[name] = [float(v) for v in nums.split()]
    return dw, dh, out


def overlap(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def check_layout(exe, rom, work, size, windows, apart, args=()):
    home = work / f"layout-{size}-{windows.replace(',', '-')}"
    home.mkdir()
    shot = home / "rects.txt"
    r = run(exe, rom, home, {"WEEMITT_MENU_TEST": "200", "WEEMITT_MENU_WINDOWS": windows,
                             "WEEMITT_MENU_SHOT": str(home / "menu.ppm"), "WEEMITT_MENU_RECTS": str(shot),
                             "WEEMITT_WINDOW_SIZE": size}, args)
    if r.returncode != 0 or not shot.exists():
        report(False, f"layout {size}: the run failed (exit {r.returncode})")
        return
    dw, dh, rs = rects(shot)
    inside = [n for n, (x, y, w, h) in rs.items() if x < 0 or y < 0 or x + min(w, dw) > dw + 0.5 or y > dh - 20]
    report(not inside, f"layout {size}, {windows}: {len(rs)} windows inside the game window"
           + (f" (outside: {', '.join(inside)})" if inside else ""))
    if apart:
        pairs = [(a, b) for a in rs for b in rs if a < b and overlap(rs[a], rs[b])]
        report(not pairs, f"layout {size}, {windows}: no window over another"
               + (f" ({pairs})" if pairs else ""))


def check_settings(exe, rom, work):
    home = work / "settings"
    home.mkdir()
    r = run(exe, rom, home, {"WEEMITT_QUIT_FRAMES": "60"},
            ["--skip-intro", "--max-fps", "25", "--no-vsync", "--easy-ball", "--infinite-lives", "--smooth-physics",
             "--camera-follow", "2"])
    ini = home / "data" / "weemitt" / "Glover" / "settings.ini"
    if r.returncode != 0 or not ini.exists():
        report(False, f"settings: the run failed (exit {r.returncode}) or wrote no settings.ini")
        return
    values = {}
    section = None
    sections = {}
    for line in ini.read_text().splitlines():
        if line.startswith("["):
            section = line.strip("[]")
        elif "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            values[k] = v
            sections[k] = section
    kept = {"skip_intro": "0", "max_fps": "0", "vsync": "1", "easy_ball": "0", "infinite_lives": "0",
            "physics_mode": "1", "camera_follow": "1.00"}
    wrong = [f"{k}={values.get(k)}" for k, v in kept.items() if values.get(k) != v]
    report(not wrong, "settings: the command line's options are not saved" + (f" ({', '.join(wrong)})" if wrong else ""))
    report(int(values.get("window_w", "0")) > 0, "settings: the window's size is saved")
    misplaced = [k for k in values if (k.startswith("key.") and sections[k] != "keys")
                 or (k.endswith("_volume") and sections[k] != "audio")]
    report(not misplaced, "settings: the keys are under [keys] and the volumes under [audio]"
           + (f" ({', '.join(misplaced)})" if misplaced else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=regress.default_rom())
    ap.add_argument("--exe", default=str(ROOT / "build" / "linux" / "weemitt"))
    ap.add_argument("--studio", default=str(ROOT / "build" / "studio" / "weemitt-studio"),
                    help="the studio build; its case is skipped when it is missing")
    args = ap.parse_args()
    regress.require_rom(args.rom, "menutest.py")
    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        sys.exit("menutest.py: no display. Set DISPLAY (for example to an Xvfb display).")
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        check_layout(args.exe, args.rom, work, "960x720", "graphics,audio,enhancements,cheats", True)
        check_layout(args.exe, args.rom, work, "960x720", "graphics,audio,log", True)
        check_layout(args.exe, args.rom, work, "640x480", "graphics,audio,controls,enhancements,cheats,log", False)
        check_settings(args.exe, args.rom, work)
        if os.path.exists(args.studio):
            # The studio's windows with the menu open: Points and search, Input, Start a level, States and the bar.
            check_layout(args.studio, args.rom, work, "1200x850", "level,states", True, ["--level", "13"])
        else:
            print(f"skip studio: no {args.studio}")
    print(f"menutest: {failures} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
