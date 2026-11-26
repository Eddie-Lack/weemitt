# Playing

This page is for players. It says how to start the game, which keys play it, and where your saves go.

## The ROM

weemitt needs your own copy of the Glover (USA) ROM. The game code is NGVE. Other regions do not work.

- The ROM can be in any of the three usual byte orders: `.z64`, `.v64` or `.n64`.
- Put the ROM next to the program, or in the folder you start it from. The program then finds it by itself. It
  first tries the names `glover.z64`, `Glover.z64`, `Glover (USA).z64`, `baserom.us.z64`, `glover.n64` and
  `glover.v64`. Then it tries every other file that ends in `.z64`, `.v64` or `.n64`, in any letter case, and
  takes the first one that is Glover (USA).
- Or name the ROM when you start the program: `weemitt /path/to/glover.z64`, or `weemitt --rom /path/to/glover.z64`.
  On Windows you can drop the ROM on `weemitt.exe`.

## Keys

These are the keys at the start. Settings, Controls in the menu changes them. Each input can have two keys.

| N64 | Key |
| --- | --- |
| Control stick | arrow keys, or W A S D |
| A | Space |
| B | X |
| Z | Z, or Left Shift |
| Start | Return |
| L | Q |
| R | E |
| C buttons (up, down, left, right) | I, K, J, L |
| D-pad (up, down, left, right) | T, G, F, H |

The game shows the N64's buttons in its own texts. Read them with this table.

Other keys:

| Key | What it does |
| --- | --- |
| Escape | opens and closes the menu |
| F11, or Alt+Enter | switches between a window and full screen |
| F9 | pauses the game, and resumes it (Developer, Frame step) |
| F10 | while paused: runs one game frame |

## The menu

Escape opens the menu at the top of the window. While it is open, the keyboard and the mouse work on the menu, not
on the game. A controller still works for the game. The game keeps running behind the menu, unless you turn on
Settings, Graphics, "Pause while the menu is open".

- Settings: Graphics (frame rate, full screen, the picture), Audio and Controls (keys and controller).
- Enhancements: easy ball controls, Ballworld, the camera's speed and skipping the intro.
- Cheats: the game's own cheats, and Go to level.
- Developer: tools for testing and for finding glitches, and the log.

The menu saves your choices in `settings.ini` when it closes. [menu.md](menu.md) describes every entry.

## A controller

A game controller works as soon as it is plugged in. The left stick moves the glove. The other inputs at the start:

| N64 | Controller |
| --- | --- |
| A | A |
| B | B, or X |
| Z | left trigger |
| Start | Start |
| L | left shoulder |
| R | right shoulder, or right trigger |
| C buttons | right stick (C-Up also Y) |
| D-pad | D-pad |

An N64 controller on a raphnet adapter works too. Settings, Controls, Controller changes the bindings.

## Saves and settings

The game saves as on the N64, in the save slots of its own menu. The program keeps the save in one file,
`eeprom.bin`, and the menu's choices in `settings.ini`. Ballworld mode has its own save, `eeprom-ballworld.bin`.
They are in this folder:

| System | Folder |
| --- | --- |
| Linux | `~/.local/share/weemitt/Glover/` (or `$XDG_DATA_HOME/weemitt/Glover/`) |
| Windows | `%APPDATA%\weemitt\Glover\` |

`--save FILE` uses another save file for one run. Developer, Open folder opens the folder.

## Options for one run

The program takes options on its command line. They last for that run only: they do not change `settings.ini`.
`weemitt --help` lists them all. The ones for players:

| Option | What it does |
| --- | --- |
| `--rom FILE` | the ROM to play |
| `--fullscreen` | starts in full screen |
| `--level N` | starts level N at once (0 to 47; Cheats, Go to level lists the names) |
| `--save FILE` | the save file to use |
| `--no-sound` | plays without sound |
| `--skip-intro` | goes straight to the title menu |
| `--fixed` | runs the game at the N64's rate, with no blending |
