# The in-game menu

Press Escape in a window build to open or close the menu. It is made with Dear ImGui (1.90.1, MIT, in
`third_party/imgui`, see its README). The game keeps running while the menu is open, unless Settings, Graphics,
Pause while the menu is open is on. The menu takes the keyboard and the mouse: the game gets no keyboard buttons, but
a game controller still works. F11 and Alt+Enter still switch full screen. Alt+Enter's Return never reaches the game.
[playing.md](playing.md) is the guide for players.

A window opens at its own place when that is free. Otherwise it opens at the first free place that holds it, with
no other window over it, below the menu bar (`place_window` in `menu.cpp`). When no free place is left, it opens at
its own place. Every window is kept inside the game window, also when the game window gets smaller. Enhancements and
Cheats are items of the menu bar itself: one click opens the window. A short notice at the top of the window
(`menu_notice`) shows for six seconds, also while the menu is closed. The program uses it when the save or the
settings cannot be written, and when an input recording cannot start.

## Files

| File | Contents |
| --- | --- |
| `src/port/menu.cpp` | the menu bar and its windows, written in C++ |
| `src/port/menu.h` | the plain C interface that `platform.c` calls: `menu_init`, `menu_handle_event`, `menu_draw`, `menu_is_open`, `menu_shutdown` |
| `src/port/menu_null.c` | the same interface with no effect, for the headless build |
| `src/port/settings.c`, `settings.h` | the `Settings` structure, its defaults, and reading and writing `settings.ini` |
| `src/port/record.c`, `record.h` | the input recorder and the reader of its files (plain C, in every build; see [testing.md](testing.md#recording-and-replay)) |
| `src/port/log.c`, `log.h` | the log: file, ring of recent lines, verbose level and crash handler (plain C, in every build) |
| `src/port/devview.c` | what the Developer overlays read from the game: the collision lines and the actor names (game code, read only) |
| `src/port/debugpad.c` | the debug pad `pad[4]`, the debug actions, the timing bar scale and the recording files (game code; see "Game debug") |
| `src/gfx/gfx_debug.c` | the scene camera, the projection of a world point, and the line drawing of the collision overlay |

`platform.c` keeps one `Settings` value in `main`. It gives the menu a pointer. The menu changes the value, and the
window loop reads it: the frame-rate limit each frame, the key mapping in `read_input`. `menu_draw` runs after
`gfx_draw_frame` and before the buffer swap.

## What works

- Settings, Graphics: Physics, VSync on or off, and a frame-rate limit from 20 to 240 frames a second, or Unlimited. A
  change takes effect at once. Physics has two choices. Smooth (frame-rate independent) runs the game's logic once per
  drawn frame, at any of these rates (see [delta-time.md](delta-time.md)). Original (interpolated) runs the logic as on
  the N64, 20 whole updates a second in a level and 30 in a menu, and blends the previous and the latest update when it
  draws (see [interpolation.md](interpolation.md)). Glitches that need whole frames, such as wall clips, work in the
  Original mode. Original (no blending) runs the same logic, and each drawn frame shows the latest game frame, as the
  N64 does: no delay is added. It is the same as `--fixed`, and a recording made with it says so. The limit and VSync
  apply to drawing in all three. The choice is saved as `physics_mode` (0, 1 or 2), and the default is Original. A
  change from one mode to another takes effect at once, in a level and in a menu.
- Settings, Graphics, Window:
  - Full screen: the same as F11 and Alt+Enter. The choice is saved as `fullscreen`. The window's size and place
    while it is not full screen are saved as `window_x`, `window_y`, `window_w` and `window_h`. The next start opens
    the window there, when that place is still on a display. `--fullscreen` and `WEEMITT_WINDOW_SIZE` act for one run.
  - Pause while the menu is open (`pause_menu`, off by default): the game's logic waits while the menu is open.
  - Pause when the window loses focus (`pause_unfocused`, off by default): the logic waits while another window is
    active.
  Both act as Developer, Pause game logic does, without the PAUSED text: in the Smooth mode the logic stops at the
  end of the original frame in progress. The sound player runs on real time, so the music goes on. No step runs
  while either holds the game.
- The mouse cursor hides over the game after 2 seconds without a move. It shows while the menu or the studio's bar
  is open. The window's icon (a glove on a ball) is drawn from a small map in `platform.c`: it holds nothing from the
  ROM.
- Settings, Graphics, Image options. All four are saved in `settings.ini` and take effect at once. The defaults give
  the look of the build without the options (a screenshot at the defaults is identical, pixel for pixel).
  - Resolution scale (`render_scale`, 50 to 400%, default 100): the renderer draws the picture into an offscreen frame
    buffer of this share of the window's size, then scales it to the window with a linear filter. Above 100% the
    picture is smoother when it is scaled down. The size is cut to the GPU's largest render buffer. When the GPU
    refuses a size, the anti-aliasing samples halve until it accepts.
  - Anti-aliasing (`msaa`, Off, 2x, 4x or 8x, default 4x): the frame buffer is multisampled and resolved before it
    is scaled, so it works with the resolution scale. The choices above the GPU's `GL_MAX_SAMPLES` are disabled. Only
    polygon edges are smoothed. Cut-out textures (leaves, fences) are drawn by discarding texels with an alpha below
    1/8, as the N64's coverage rule does (`COVERAGE_EDGE` in `gfx_render.c`; there is no alpha-to-coverage), so their
    edges look as before with any sample count. The environment variable `GLOVER_MSAA` still overrides the choice.
  - Texture filtering (`texture_filter`: 0 N64, 1 bilinear, 2 nearest, default N64): the renderer's own filter was
    already the N64's three-point filter, so that is the default. Bilinear is the four-point filter (the old
    `GLOVER_GFX_FILTER=bilinear`, which still overrides the choice). Nearest turns filtering off. Textures that the
    game draws with point sampling stay point sampled in every choice.
  - Widescreen (`widescreen`: 0 off, 1 stretch, 2 extend view, default off). Off keeps the 4:3 shape with bars at the
    sides. Stretch draws the 4:3 picture over the whole window. Extend view (Hor+) makes the frame as wide as the
    window and widens the view: every x coordinate that reaches the renderer is divided by k, the window's aspect
    divided by 4:3. The N64's screen becomes the middle of the frame. Triangles drawn with a perspective projection
    and a viewport over the whole screen width (the 3D world, 3D sprites) also draw what lies beyond the middle area:
    their scissor covers the whole frame and the clip codes of `G_CULLDL` are widened by k. The HUD, menus and
    other texture rectangles keep their 4:3 shape in the middle. A rectangle or Sprite2D backdrop that spans the whole
    screen width (a fade, a bar) is stretched over the whole frame. A window narrower than 4:3 gets bars above and
    below. The game's logic does not change: the view is widened only in the renderer. The record pass of the
    interpolation widens the view the same way as the draw, so Original physics blends correctly.
    Limits: the game builds its display lists from what its own code finds inside its 4:3 view. Objects outside it
    are missing at the sides and pop in at the edge of the middle area (a wooden walkway in level 20 ends at the
    4:3 edge). Some effects are polygons sized for 4:3: the title screen's sky is blue only in the middle, and the
    star wipe at the start and end of a level covers only the middle, so the world shows beside it.
- Settings, Controls has two tabs. Keyboard: every input that the keyboard drives (A, B, Z, Start, L, R, the C buttons,
  the D-pad and the four directions of the stick) has two keys. Click a key button, then press the new key. Escape
  cancels. A right-click clears the key. A key belongs to one input: binding it takes it from the input that had it,
  and a line at the top of the window says so for six seconds ("K was moved from C-Down to A."). Reset to defaults
  asks first, then restores the original mapping (arrow keys and WASD for the stick, Space for A, and so on;
  [playing.md](playing.md) lists them).
- Controller: the same inputs, two bindings each. Click a slot, then press a controller button or move a trigger or
  a stick past about 60% (axes bind per direction). Escape cancels, a right-click clears, and a binding belongs to one
  input. The defaults: A is A, B is B and X, Z is the left trigger, Start is Start, L is the left shoulder, R is the
  right shoulder and the right trigger, the C buttons are the right stick (C-Up also Y), the D-pad is the D-pad. The
  left stick is analog and always moves the glove. A binding on a Stick row adds a full-tilt direction.
  The menu still reads the controller while it is open, so the buttons work for the game behind it.
  The top of the tab names the connected device and says whether it is a "Game controller" or a "Joystick (no SDL
  mapping)". A game controller is a device that SDL knows a mapping for. A game controller is used when one is
  connected. Otherwise the first plain joystick is used. A raw joystick has its own bindings, which the tab shows in
  place of the controller ones. They are named "Button 3", "Axis 2+" and "Hat 0 Up". Moving an axis past about 60%
  binds it. The default layout is the usual order of the raphnet N64 adapter: buttons 0 to 5 are A, B, Z, Start, L
  and R, buttons 6 to 9 are C-Up, C-Down, C-Left and C-Right, buttons 10 to 13 are the D-pad, and hat 0 also drives
  the D-pad. Axes 0 and 1 are the analog stick, with the same dead zone and full-tilt settings. Reset to defaults
  restores both tables.
  To teach SDL a mapping, put a `gamecontrollerdb.txt` next to the program or in the save folder. The program reads
  both at start and logs how many mappings it added. The log also lists every device found or removed, with its
  name, GUID, and number of axes, buttons and hats, and says how the program opened it. Send that log when the
  default joystick layout is wrong.
- Controller, Dead zone and Full tilt at (defaults 12% and 90%): the left stick is read as one round control. Up to
  the dead zone the glove stands still. From the dead zone to the full-tilt point the stick length grows in a straight
  line from 6 to 80, the N64 stick's full range. The game drops up to 5 on each axis and clamps at 79, so the glove
  sees at most 74 at full tilt. The D-pad buttons make the game set the stick to 79 when the stick is centred, so
  full tilt is about 94% of D-pad speed. The old mapping used a 24% dead zone on each axis and reached 80 only at the
  very rim (29500 of 32767 gave 64 in the game, now 74; 16000 gave 21, now 36).
- Enhancements, Easy ball controls: the N64 game reverses the stick while the glove stands on the ball, except on
  Easy. With the option on, every difficulty steers the ball the way Easy does. It is off by default, which is the
  original behaviour. `ProcessInput` in `controller.c` tests `easyBallControls` (a global next to `spaceDemo`, set by
  `port_set_easy_ball`); the Space demo has the easy direction always. `--easy-ball` turns it on for a run, in the
  headless mode too. A scripted run shows the effect: level 35 with a random script that stands the glove on the ball
  (the ball position differs from the first ball-walk frame on).
- Enhancements, Ballworld: off by default (`ballworld`, `--ballworld`). In this mode the player plays every level as
  the ball alone. Glover, the glove, is hidden and rides the ball. The game logic is `src/game/ballworld.c` with
  `include/ballworld.h`, copied from the same author's Ballworld mod for the N64 game. The copy of
  `ballworld.c` has three local changes for the C++ build, listed in its first comment. An update replaces both files
  with newer copies and keeps those changes until the mod's own copy has them. The switch is the global
  `ballworldOn`. The game calls the mode from `MainGameLoop` (`main.c`), `HurtHand` (`hand.c`), the boss update
  (`nmeprocess.c`), the rotor potion in `ActivateCollectable` (`collect.c`), and the places where a hit makes Glover
  let go of the ball (`CheckActorCollisions`, `HurtBall`, `MaintainBulletBubble`, `NMEProcessSucker`); each call returns at
  once when `ballworldOn` is 0, and the logic is then bit for bit as before (`tools/regress.py check`).
  The menu and `settings.ini` hold a requested value. `StartNewLevel` (`game.c`) calls `port_ballworld_latch`
  (`frame.c`), which copies it into `ballworldOn`. So a change in the menu starts at the next level, never in the
  middle of one. `--ballworld` sets the requested value from the start, in the headless mode too.
  The mode has its own save file, `eeprom-ballworld.bin`, next to `eeprom.bin`. The program picks the file once, at
  start-up, from the requested value, and loads it. If a level start then latches a value that differs from the
  file's mode, the program writes no save until the next start, even if the mode is turned back later: the game's
  copy of the save then holds the other mode's progress. The menu says so. When the mode is turned off, Glover gets
  back the health that the mode held (`BallworldLeave`). The other file is never
  touched. `--save FILE` names the file directly, and the mode does not change that. The recorder does not store the
  mode.
- Enhancements, Camera follow speed: a slider from 1.0x (the original) to 3.0x, with a Reset button. The camera trails the
  glove: it turns behind it, closes the distance to it and follows its height with springs. The option makes those
  pulls stronger. It changes only the normal follow in `MoveCam` (`camera.c`) and `UpdateActorPositionDependencies` (`actor.c`): the pull
  towards the glove's distance, the turn behind a moving glove, the turn that lines the camera up behind the glove
  when the stick rests, the height spring, and the aim point's follow of the glove (a third of the gap a frame in
  height, and the speed limit across). Cut-scenes, the fixed and override cameras, orbiting and the camera turn
  buttons are not touched. The global `cameraFollowSpeed` (next to `easyBallControls`, set by `port_set_camera_follow`)
  holds the value. A factor k of the original per frame becomes 1 - (1 - k)^speed, which `CameraFollowLerp` and
  `CameraFollowDiv` in `camera.c` compute. The usual delta-time conversion comes after it, so the result is right at any
  frame rate. A per-frame acceleration or a speed limit is multiplied by the value. At 1.0x every path gives the
  original floats. `--camera-follow N` sets the value for a run, in the headless mode too. On level 35, a run
  of 70 frames in a straight line leaves the camera 8.4 units farther from the glove at 1.0x, 4.7 at 2.0x and 4.0 at
  3.0x (at 0.1 of a frame an update: 8.6, 5.5 and 4.6).
- Developer, Log: see the next section.
- Developer, Record input (from the next start): a checkbox, saved in `settings.ini` as `record_input`. When it is on,
  the next start writes every update's length and every controller value the game reads to
  `weemitt-YYYYMMDD-HHMMSS.rec` in the save folder, from the start of the program. The window shows the folder (or,
  while recording, the file), with an Open folder button. A red REC mark is drawn at the top right corner of the
  window and the title ends with `[REC]`. The player sends the file and a developer replays it with
  `--replay` (see [testing.md](testing.md#recording-and-replay)). The file holds the save and no ROM data. `--record FILE`
  records one run without the setting.
- Developer: Show FPS (an overlay) and the frame rate and frame time. When Unlimited is chosen, both show the cap
  (see the next section). With VSync on and a display slower than the cap, VSync sets the rate and no cap shows. The
  FPS, PAUSED and REC texts lie under the menu's windows.
- Developer, Frame step, Show collision and Show object names: see "The Developer tools" below.
- Developer, Game debug: the debug code that the retail game still holds. See "Game debug" below.

- Settings, Audio: when there is no sound, a line at the top of the window says why (no sound device, `--no-sound`,
  or a build without the sound player).
- Settings, Audio, Master volume: 0 to 100% of the game's own level, 20% by default. The game's own level is loud next
  to other programs, and the tooltip says so. The sound callback in
  `platform.c` scales every sample after the sound player has mixed them. The game's logic does not see it.
- Settings, Audio, Music volume and Effects volume: 0 to 100%, both 100% by default (saved as `music_volume` and
  `effects_volume`). They are shares of the master volume. libmus gives every channel one synthesizer voice, and a
  channel plays either a song (no effect number) or a sound effect. `musVoiceUpdateVolumePan` (`src/audio/mus/mus-voice.c`)
  scales the volume it sends to the voice by the share of the channel's group. The game's own value stays in the
  channel (`lastVolume`), so no game memory changes. A playing note follows a slider at the next player call. The
  envelope mixer squares the volume it gets, so the code sends the square root of the share and the slider gives that
  share of the amplitude. At 100% the code sends the game's value and the sound is bit for bit as before (checked by
  rendering a song and two effects with `tools/audiotest`'s `render_song` and comparing the samples). At 50% a song
  has half the RMS level, and at 0% an effect is silent. The headless build has no sound player, so the regression
  check does not see this.
- Settings, Audio, Mute when the window loses focus: off by default (`mute_unfocused`). The window loop sets the master
  volume to 0 each frame while SDL says the window has no input focus. The game keeps running.
- Enhancements, Skip intro videos: off by default (`skip_intro`, `--skip-intro`). A normal start shows the
  presentation scene (the logos, a recorded demo with the bowling ball) and then the fly-through with "press start".
  The option presses Start for the player, on every other original frame, in those two scenes: the game allows a button
  press to leave both (`ReadControllerFromData` ends a recorded demo on A, B or Start, and `DoFlythruScreen` on any
  button). The press stops when the scene begins to leave and when the title menu is reached, so the menu gets none.
  The presses go through the update's controller word (`port_intro_skip_buttons` in `frame.c`, added in `platform.c`),
  so a recording holds them. The first 140 frames (the start-up) and the scene's first 9 frames cannot be skipped
  (the game ignores buttons then). In the headless run the presentation ends at frame 186 instead of 566, the fly-through starts at 187, and the title menu comes at frame 233. Without the option the fly-through starts at frame 567 and waits for a press. The option does nothing in
  the Space demo (it boots into a level), with `--level`, and when the title's attract mode comes back later.
- Enhancements has no "Smoother camera" option. Camera follow speed and Graphics, Physics, Original (interpolated) already
  cover it.
- Cheats: four options that hold the game's own cheat bits (`cheatActivated`, `cheat.c`) on. They are off by default.
  The boxes show the bits as they are now, so a change in the pause menu's cheat list shows there. A click sets or
  clears the bits at once and the choice is saved. `port_set_cheat_hold` keeps the held bits on at the start of every
  update (`port_game_update`), so a new level, the game's start-up and the code entry do not undo them. The pause
  menu's list shows those cheats as `--` (locked). With no option on the mask is 0 and the update is as before.
  - Infinite lives (`infinite_lives`, `--infinite-lives`): cheat 3, INFINITE LIVES. `LoseLife` then keeps the count.
    In the Space demo the box is on and cannot be changed (the demo sets the bit itself).
  - Infinite health (`infinite_health`, `--infinite-health`): cheat 6, INFINITE ENERGY. `HurtHand` (`hand.c`) does
    not take hearts while the bit is set. A hit that kills at once (`damage < 0`), falls, drowning and crushing
    still kill, as with the game's cheat.
  - Unlock all levels (`unlock_all`, `--unlock-all`): cheats 24 and 25, OPEN PORTALS and OPEN LEVELS. The game
    reads them where it decides which portals and bonus levels are open. The save is not changed: this is a memory
    option and nothing is written unless the player saves. The cheats are not stored in a save slot.
  - Level select (`level_select`, `--level-select`): cheat 23, LEVEL SELECT. The title menu reads it (`title.c`) and
    allows its level select entry. The entry has no label: press Up from START, then A, to reach SELECT LEVEL.
  - Go to level: a list of the game's levels 0 to 41 (hubs, the cave, the assault course, the six worlds' levels,
    bosses and bonus levels) and a Go button. It starts the level at the next update the way the level select does
    (save slot 6, medium difficulty, `StartLevelNow` in `frame.c`), from a level or from the title menu. A running level
    is freed first, the pause state and the pause menu's cheat list are reset, and a recorded demo is stopped. The
    result is the same as `--level N` (the trace of level 20 from frame 0 matches the one of a level 5 run that
    went to level 20 after 200 frames, bit for bit). The button is disabled in the Space demo and in the screens
    between levels. It asks first. It does not use or change the player's save.
  - Headless: `--infinite-lives`, `--infinite-health`, `--unlock-all`, `--level-select`, `--skip-intro` and
    `--goto-level FRAME LEVEL` (start the level at original frame FRAME). They work in a window build too, for that
    run only: `settings.ini` keeps its own values. A recording does not hold these options (only easy ball and camera follow speed): replay one
    that was made with cheats on by giving the same options.

## The Developer tools

The Developer window has three tools. They only read the game's state. Nothing in them changes the game's memory,
so the logic is the same whether they are on or off (`tools/regress.py check` gives 0 differences).

### Frame step

- Pause game logic (a checkbox, or the pause key, F9 by default) stops the game's logic. Drawing, the menu and the
  overlays go on. The sound player keeps running on real time, so the music goes on.
- Step (a button, or the step key, F10 by default) runs exactly one original frame while paused: one call of
  `port_game_update(1.0f)`. It reads the controller as it is at that moment. A press of the step key while the logic
  runs pauses it. Holding the key repeats the step at the keyboard's repeat rate. The window shows PAUSED at the top
  with both key names. The two keys can be rebound in the window (click, press a key, right-click clears) and are saved
  as `pause_key` and `step_key` (SDL scancode numbers).
- Original physics: the pause starts at once. The picture is drawn at `t` = 1, the latest game frame, as the
  interpolation does at the end of a frame. Each step records the new game frame for the renderer, so the next drawn
  frame shows it. After the pause ends, the first update runs at once, and the picture goes on from the frame that
  was on show.
- Smooth physics: a pause that is asked for in the middle of an original frame lets the window loop run its normal
  updates until the frame ends (at most 50 ms in a level). The logic stops there. A step is then one update of length
  1, a whole frame that reads the controller and has `deltaTick` 1. Without this, a step would start in the middle of
  a frame and would not read the controller.
- A recording (`--record`) writes each step as an update, so a replay of it needs no pause.
- The window loop in `platform.c` does this. `Settings.pauseLogic` and `Settings.stepRequests` (not saved) carry the
  state between the menu and the loop. A step asked for while paused counts at most 4 ahead.

### Show collision

Draws the geometry that the game collides with, near the glove, as lines over the 3D view, with the collision
spheres. It reads the data that `CheckCollisions` (`collision.c`) tests:

- Landscape: the actors of type `LANDSCAPE_ACTOR` in `collisionList`, with their `faces` (`CollFace`, whose `edgeStart`
  holds the three corners in world space). A face counts when one of its corners is within the range of the glove.
- Platforms: each platform of `platformList` (the game skips those with flags 0x84). The window works the corners out
  from the platform's mesh, position, rotation and scale, as `RecalculatePlatformInfo` does, because the stored faces
  are brought up to date only when something is near.
- Spheres: the glove's, the ball's and those of the enemies within range, at the positions that `CalcCollisionSpheres`
  finds, drawn as three circles each.
- Colours: floor (normal y above 0.3) green, wall orange, ceiling (y below -0.3) magenta, platform blue, water (terrain
  5 and 12) cyan, harmful (terrain 6 and 9) red, glove yellow, ball white, enemy light red.
- Each of these nine kinds has its own box in the window, drawn in its colour, with All and None buttons. Only the
  checked kinds are drawn. The choice is saved as `collision_types`, a set of bits: floor 1, wall 2, ceiling 4,
  platform 8, water 16, hurts 32, glove 64, ball 128, enemy 256 (`PORT_COLL_` in `frame.h`). All are on by default.
- Hide lines behind surfaces: depth test against the picture's depth buffer. A line in a surface is pulled a little
  towards the camera. It then wins the depth test over its own surface. Off by default: the lines show through the scenery.
- The range around the glove is a slider from 100 to 2000 game units (500 by default), saved as `debug_range`.
  Lines per frame are limited to 60000. The overlay needs a level: it draws nothing in the menus.

How it draws: `platform.c` asks `port_debug_collision` for the lines before each `gfx_draw_frame` and hands them to
`gfx_debug_lines`. The renderer draws them into its own frame buffer after the game's display lists, with the camera
that it used for that frame. So the overlay shares the picture's resolution and multisampling, and the interpolated
camera of the Original mode applies to it. The camera is the matrix that `SetupViewing` (`main.c`) builds: the
projection with the camera's view multiplied in. `gfx_dl.c` keeps the last such matrix of a frame, and skips the one
`ClearViewing` builds (a view from the origin, which leaves the translation row as it was).

### Show object names

Writes a label at each actor within range of the glove that is on the picture. A label is a dot at the actor's position
with its name above it, drawn with ImGui's background draw list, so the menu windows are on top. The name is the enemy
type for an enemy (from the game's `ActorType` list), "Glover" and "Ball", and for platforms, pick-ups and the rest the
name of the object that the actor uses (the model's name in the level file, such as `gbl_bt` or `cball`). Landscape,
water, debris and bullets get none. Labels that would overlap move down a row. The position is projected with the
same camera as the collision lines.

### Limits

- The collision lines, the spheres and the labels show the game's latest logic state. The picture shows the
  state before the latest update, and in the Original mode it blends the two latest frames. A moving glove or ball is
  therefore drawn about one update's distance away from its sphere. The landscape does not move, so it always lines up.
- The depth test uses the depth buffer of the scene. Labels are not tested: a name shows for an actor behind a wall.
- A face is drawn once for each of its three edges, so edges that two faces share are drawn twice.

## Game debug

The retail game still holds the team's debug code (see `docs/game.md`, "Debug code", in
[gloveworld](https://github.com/Eddie-Lack/gloveworld)). It reads a fifth controller entry, `pad[4]`, that the retail
game never fills, and it writes recordings and timing bars for the development PC. The Developer window's Game debug
section (closed until opened) turns these on. Everything is off at every start. Nothing is saved in `settings.ini`.
With the debug controller Off and no debug button used, the game's memory is the same as before the feature
(`tools/regress.py check` gives 0 differences).

### The debug controller

The combo box chooses what fills `pad[4]`.

| Choice | What it does |
| --- | --- |
| Off | `pad[4]` stays zero, as in the retail game. |
| Second controller | The second SDL game controller or joystick drives it. It uses the same binding tables as controller 1 (the Controls window), including the stick, the dead zone and the range. The window names the device. With no second device it says so. A device that comes or goes is found at once. |
| Keyboard (debug keys) | The keys below drive it. They work with the menu closed, as the game's own keys do. The keys are fixed. |

The debug keys are 1 (A), 2 (B), 3 (Z), 4 (Start), 5 (L), 6 (R), 7 (C-up), 8 (C-down), 9 (C-left), 0 (C-right),
`[` (D-pad up), `]` (D-pad down), `-` (D-pad left), `=` (D-pad right), and Home, End, Delete and Page Down for the
stick (up, down, left, right). They avoid the game's keys, F9 to F11 and Escape. The window shows them in a table
with each debug control and what it does (taken from `docs/game.md`).

How `pad[4]` is filled: `ControllerStep` (`main.c`) reads controller 1 for the game. The debug ROM patch reads controller 2
right after it, in the same function. The port does the same through `PortDebugPadStep` (`debugpad.c`). It gives the
game's own `ReadController` the button word and stick of the debug pad. So `pad[4]` gets the buttons, the stick, the
presses, the hold times and the stick angle as `pad[0]` does, once per original frame. `ReadController` works on the
entry of controller 2, which the port never uses, so the call swaps that entry with `pad[4]` and back. The recorder of the
debug code (`autoPlayback` 1) does not see this pad. The window loop samples the debug keys or device once per drawn
frame. A press shorter than the time between two game reads is kept until the next read.

`pad[4]` lies past the end of the array that the headers declare. In `DeltaHoldInput` this means its presses are not
cleared between ticks. Every `pad[4]` press in the game's code is already tested under `deltaTick`, so each press acts
once, in both physics modes.

The debug pad is not part of an input recording (`.rec`): a replay does not repeat what it did.

### The action buttons

Each button puts the input of one debug control into `pad[4]` for the next game frames. The game's own code then does
the work. A button waits for the one before it to finish. They work in a level (game modes 4 and 5, where
`ProcessInputPersonal` runs) and are dimmed elsewhere. They also work with the debug controller Off. After the last
frame `pad[4]` goes back to zero.

| Button | Input on `pad[4]` | Frames | Effect |
| --- | --- | --- | --- |
| Previous platform, Next platform | C-left, C-right | 1 | Moves Glover (and the ball) to the previous or next platform. |
| Mark all levels done | L | 1 | Sets every level and garib bit of the current save slot. The game may save it. The button asks first. |
| Timing bars | Z | 1 | Cycles `timerMode` through 0, 1, 2. |
| Previous song, Next song | D-pad left, D-pad right | 1 | Plays the previous or next of the 61 songs. D-pad right also turns the ambient light a little, as on the debug ROM, because the game tests that button held. |
| Toggle inertia | R | 1 | Divides the hand's acceleration by 3, or multiplies it back. |
| Quit to menu | A, then A and B | 2 | The code needs A held when B goes down. The game leaves the level. In record mode it writes the recording first. |
| Hurt ball | Start | 1 | `HurtBall`. |
| Cycle (recording mode) | Z | 1 | Only on the title menu. See below. |

Every sequence ends with a frame with no buttons, so the next press is a new press. The `--debug-action` option of the
headless build runs the same sequences (see [testing.md](testing.md)).

### Timing bars

`PrintTimers` (`timer.c`) draws one row of bars for each `StartTimer` slot when `timerMode` is not 0: Main (0),
GRAPHICS (1), CONTROL (2) and NEAR (3) have work in the port. The slots for the sound player and the RSP (7 to 9) stay
empty. A bar starts where its timer started inside the frame and is as long as the time it ran. A pixel is 20000 ticks
of the N64's counter (0.43 ms). The long white lines mark one and two retraces (16.7 and 33.3 ms) at the scale. The
game also draws a line at 781350 pixels. The rectangle's 12-bit coordinates wrap it round, and it shows as a line left of the others.

`osGetCount` gives real time in the window build: the system's high resolution clock at 46.875 MHz, as the N64's counter
runs. The headless build keeps its simulated clock. The PC does a frame's work in a few milliseconds, so the bars are
short. The Bar scale slider (1x to 64x, 16x at the start, not saved) multiplies the time that the timers see
(`PortTimerCount` in `debugpad.c`, used by `StartTimer`, `EndTimer` and the frame start in `ControllerStep`). At scale 1
the values are `osGetCount`'s own. The bars draw through the renderer's fill rectangles. They show the work of the
latest original frame. In the Smooth physics mode a frame has several updates, and each timer keeps ten samples.

### Recordings

`autoPlayback` is 0 (off), 1 (record) or 2 (play back). On the title menu, Z on `pad[4]` cycles it (`MainTitleWait`).
The window shows the state. Its Cycle button sends that Z and works only while the title menu runs
(`PortDebugTitleWait` tells the window).

- Record: cycle to Record, then start a level, from the menu or with Go to level (a level started from the title
  menu keeps the mode). The game stores the button changes. Quit to menu (A held, then B) writes the recording: the game
  calls `PCcreat`, `PCwrite` and `PCclose`, which the port implements in `debugpad.c`. The file goes to the save
  folder as `LEVELNAME.moo`, named as the game names it (spaces become underscores), without the development PC's path.
  The log says `wrote FILE (N bytes)`. A recording is made big-endian, in the N64 format: a size word that counts itself,
  then pairs of a frame number and a button word, ended by -1. Only buttons are recorded, not the stick. The tools in
  gloveworld (`tools/patches/mooedit.html`) read these files. Leaving a level by any other way discards the recording.
  The intro level always plays back.
- Play back: the game plays the demo that the ROM holds for the level you start, as on the debug ROM (the controller
  picture appears). A level without a demo stops with "LEVEL UNPLAYABLE - NO MOO FILE". Go to level from the title
  menu does not clear the mode.

### Messages

`debugPrintf` (`main.c`) already sends the game's `PRINTF` messages to the log. Turn on Developer, Write a log file or
open the Log window.

## Unlimited frame rate

The game's update is a fraction of an original frame (see [delta-time.md](delta-time.md)). Fractions down to 0.1 are
tested. In the Original physics mode the logic never runs a short update, so Unlimited stops at 1000 drawn frames a second.
In the Smooth mode, with the limit on Unlimited, the window loop holds the drawn rate to 10 times the game's own rate:
`600 / port_game_retraces()`, which is 200 a second in a level and 300 in a menu. An update is then never shorter than
0.1 of an original frame. A numbered limit (20 to 240) is used as it is. The wait sleeps most of the frame and spins the
last 2 ms, since `SDL_Delay` is coarse on Windows. `Settings.fpsCap` (not saved) holds the active cap for the Developer
window and the overlay.

## The log

A Windows build is a GUI program (`-mwindows`): stdout and stderr go nowhere. Every message of the game
(`debugPrintf` and `PRINTF`), `port_log`, `port_fatal` and the platform layer's errors (`die`, save and sound
problems) goes through `plog_vprintf` in `log.c`. It always goes to stderr as before. It also goes to:

- The log file, when Developer, Write a log file is on. The file is `weemitt.log` in the save folder (the menu shows
  the full path; Open folder opens it with the system's file manager). `--log FILE` writes to FILE instead and
  turns the file on for that run. The choice is saved in `settings.ini` (`log_file`). Lines are appended and flushed
  one by one, so a crash loses nothing. A file over 1 MB starts again from empty when it is opened.
- The ring of the latest 300 lines, which the Log window (Developer menu) shows with a Clear button and
  auto-scroll. The ring exists only in a window build and fills from the start of the program.

Each line starts with the seconds since the first message, for example `[    7.091] Jalloc initialised`. Messages
from the sound thread are safe: the writer takes a spin lock.

Verbose (`log_verbose`) logs the frame rate, the game mode and the level once a second. When it is off, the cost is
one test a frame.

The crash handler starts with the window. On Windows (`SetUnhandledExceptionFilter`) it writes the exception
code, the faulting address and the address relative to the exe's base (so it can be looked up in the exe's symbols),
the access type of a memory fault, the registers, up to 16 words on the stack that point into the program, and the
game mode, level and frame state (`port_game_trace`). If the log file is off, the handler opens it for the crash.
It then shows a message box with the log's path. On Linux a signal handler (SIGSEGV, SIGILL, SIGFPE, SIGBUS,
SIGABRT) writes the same kind of lines, without the message box.

`WEEMITT_CRASH_TEST=N` (an environment variable, window builds only) writes through a null pointer after N drawn
frames. It tests the handler.

## settings.ini

The file is in the save folder (SDL's preference path, `weemitt/Glover/`: [playing.md](playing.md) gives the folder
on each system). Without a preference folder it is in the working folder, as the save is, and the log says so. It is
written when the menu closes and when the program ends, only after a setting changed. A write that fails goes to the
log and shows a notice once.

Its sections:

| Section | Keys |
| --- | --- |
| `[graphics]` | `physics_mode` (0 Smooth, 1 Original, 2 Original with no blending), `vsync`, `max_fps` |
| `[window]` | `fullscreen`, `window_x`, `window_y`, `window_w`, `window_h` (`window_w` 0: the default size), `pause_menu`, `pause_unfocused` |
| `[enhancements]` | `easy_ball`, `ballworld` (0 or 1), `camera_follow` (1.0 to 3.0) |
| `[audio]` | `master_volume`, `music_volume`, `effects_volume` (0 to 100), `mute_unfocused` |
| `[developer]` | `log_file`, `log_verbose`, `record_input` (0 or 1) |
| `[controller]` | `stick_dead`, `stick_range` (percents) |
| `[play]` | `skip_intro`, `infinite_lives`, `infinite_health`, `unlock_all`, `level_select`, `pause_cheats` (0 or 1) |
| `[studio]` | `studio_search`, `studio_input`, `studio_level`, `studio_states`: the studio's windows that show. Every build keeps them |
| `[keys]` | `key.NAME`: SDL scancode numbers (4 is A, 44 is Space), two per input, separated by a bar; 0 means no key |
| `[image]` | `render_scale`, `msaa`, `texture_filter`, `widescreen` (values in the Graphics bullet above) |
| `[pads]` | `pad.NAME`: 1 plus the SDL button, or 100 plus twice the SDL axis, plus 1 for the positive direction |
| `[joys]` | `joy.NAME`: 1 plus the button number, 100 plus twice the axis number (plus 1 for the positive direction), or 200 plus 4 times the hat number plus the direction (0 up, 1 down, 2 left, 3 right) |
| `[debug]` | `step_key`, `pause_key` (SDL scancodes, 0 for none), `debug_range`, `collision_types` |

The reader looks at each `name=value` line by its name. The sections only make the file easier to read.

The command-line options act for one run and never change the file: `--no-vsync`, `--max-fps N`, `--original-physics`,
`--smooth-physics`, `--fullscreen`, `--easy-ball`, `--ballworld`, `--camera-follow N`, `--skip-intro`, the four cheat
options, `--render-scale N`, `--msaa N`, `--texture-filter n64|bilinear|nearest` and `--widescreen
off|stretch|horplus`. When the program saves, each setting that did not change during the run keeps the file's own
value (`settings_keep_file` in `settings.c`). A setting that the menu changed is saved. `--record FILE` records that
run whatever `record_input` says. The headless mode neither reads nor writes the file.

## Why the headless build has no menu

`make headless` must build without a C++ compiler and must not change the game's behaviour. The regression check
hashes the program's memory as seen from the game. `Settings` lives on the stack of `main`, not in a global.

The hash counts a word of game memory that lies inside the program's image as one marker, because the game holds
pointers to its own code and data. The image used to end at the linker's `_end`. A new global moved `_end`, and a
word of game data that happens to hold an address in the moved range (for example 0x0d8011a4) was then counted
differently from the baseline, with no change in the game. `hash_ram` in `platform.c` now ends the image at the
baseline build's size (0x0d80110c) on Linux. The check no longer depends on how big the program is. `log.c` adds a
few globals, and `easyBallControls` and `cameraFollowSpeed` add one each, and the check gives 0 differences.

## Test option

`WEEMITT_MENU_TEST=N` (an environment variable, window builds only) opens the menu with every window after N frames,
writes the screen to `weemitt-menu.ppm` (or the file in `WEEMITT_MENU_SHOT`) three frames later and quits.

Screenshots of the picture options need no menu. Under `xvfb-run` with `LIBGL_ALWAYS_SOFTWARE=1`, run
`weemitt --headless 300 --level 5 --shot 299 --shot-size 1280 480 --widescreen horplus --shot-window` (the headless
mode counts frames from 0, so the last frame of 300 is 299). Without `--shot-window` the shot holds only the renderer's
frame buffer (its size follows `--render-scale` and the widescreen mode). With it, the shot holds the window as it
is left after scaling to it. `--interp-check` with the same options checks the interpolation.

`WEEMITT_MENU_TAB=controller` opens the Controls window on its Controller tab for that screenshot.

`WEEMITT_MENU_WINDOWS=none` keeps the menu closed for the screenshot, and `=developer` opens only the Developer window.
`WEEMITT_OVERLAYS` is a list of words that turn the Developer tools on at the same frame: `collision`, `depth`
(hide lines behind surfaces), `names` and `pause`. Example, the collision overlay on level 5 with a closed menu:

```sh
LIBGL_ALWAYS_SOFTWARE=1 SDL_AUDIODRIVER=dummy WEEMITT_MENU_TEST=300 WEEMITT_MENU_WINDOWS=none \
  WEEMITT_OVERLAYS=collision,names,depth WEEMITT_MENU_SHOT=l5.ppm xvfb-run -a build/linux/weemitt --rom ROM --level 5
```

`WEEMITT_WINDOW_SIZE=WxH` sets the size of the window at the start, so that a tall window shows in one screenshot. The
Game debug section and its table of controls open when `WEEMITT_MENU_TEST` is set. `WEEMITT_DEBUG_PAD=second` or
`=keyboard` chooses the debug controller. `WEEMITT_DEBUG_INPUT=FILE` and `WEEMITT_DEBUG_ACTIONS=FRAME:NAME,...` drive it
as `--debug-input` and `--debug-action` do (see [testing.md](testing.md)). `WEEMITT_VIRTUAL_PADS=N` makes N virtual game
controllers, and `WEEMITT_VIRTUAL_PRESS=PAD:BUTTON:FROM:TO` holds an SDL controller button of one of them from
original frame FROM up to TO. Together they test the second controller without hardware. Example, the timing bars on
level 15 with a closed menu:

```sh
LIBGL_ALWAYS_SOFTWARE=1 SDL_AUDIODRIVER=dummy WEEMITT_MENU_TEST=300 WEEMITT_MENU_WINDOWS=none \
  WEEMITT_DEBUG_ACTIONS=100:timing WEEMITT_MENU_SHOT=bars.ppm xvfb-run -a build/linux/weemitt --rom ROM --level 15
```

`WEEMITT_PAUSE_AT=N` presses the pause key once the game has run N original frames, and `WEEMITT_STEP_PRESSES=C` then
presses the step key C times, one press every 5 drawn frames (the keys go through the event queue, as a real press
does). With `--trace`, the frame lines show that C more frames ran and then the game stood still. Both physics modes
were checked this way: pause at frame 40 and 6 presses gave the frames 41 to 46 and no more.

## Cheat list in the pause menu

In the game's own pause menu, R or Z opens a panel to the right of it with the 26 cheats of `cheat.c`. The Cheats window of the
Escape menu has a box, "Cheat list in the pause menu (R or Z)", that turns this on or off. It is on by default and
saved as `pause_cheats` in the `[play]` section. When it is off, R and Z do nothing in the pause menu, as in the
original game (`cheatMenuEnabled` in `cheat.c`, set by `port_set_pause_cheats`). The stick (or
the D-pad) moves the selection and the list scrolls. A toggles the selected cheat. B, R or Z closes the panel, and
Start leaves the pause menu with it. While the panel is open, the pause menu's own input and the cheat code entry are
off. The panel is open only where the code entry works (not in bonus levels or the last space boss).

A toggle calls `ActivateCheat(num, 1)`, the call that a correct code makes, so the effects, the flash, the sound and
the message are the same. The state shown is the cheat's bit in `cheatActivated`. For the spell cheats (14 to 20) it
is the spell itself, because a spell that ended or came from a pick-up leaves the bit out of step.

- `GO` (cheats 11, 12 and 22): one-shot actions. The two camera rotations turn the camera a quarter turn and have no
  off state. ALL CHEATS OFF turns every other cheat off.
- `--`: the cheat is refused now (`CheatAllowed`: for example CONTROL BALL while the glove holds the ball, or the
  death and frog spells in a boss level), or it is locked. In the Space demo, INFINITE LIVES and ALL CHEATS OFF are
  locked because the demo sets that cheat itself (`src/port/spacedemo.c`).
- LOCATE GARIBS is on from the start on the easy difficulty. Turning it off there changes the bit only: the game
  allows the garib locator on easy anyway.
- The spell cheats replace one another, as the pick-ups do. Switching on a second one turns the first off.
- SECRET CHEAT, LEVEL SELECT, OPEN PORTALS and OPEN LEVELS only set their bit. They have an effect in the places that
  read the bit (the Carnival bonus level, the title screen, the hub). Cheats 7 to 10 and 20 are cleared when a level
  starts (`StartNewLevel`).
- Cheats that a Cheats-window option holds on (3, 6, 23, 24 and 25) show as `--` too (`portCheatHold`).

The panel's state is in `cheat.c` (`CheatMenuOpen`, `CheatMenuUpdate`), outside the game's memory. Its counters step
in ticks like the pause menu's.

The panel is a change to the game that the controller can reach. It stays on by default: the headless runs and the
regression check use the default settings. With it on, a press of R or Z in the pause menu opens the panel. Z still
clears a half-typed code first, as on the N64 (`CheckForCheats` runs before the panel opens). While the panel is
open, the C buttons type no code: close it with B, R or Z to type a code. A player who types codes as on the N64 can
turn the box off. The
brute forcer turns the panel off (`OFF0(CheatMenuAvailable)` in `bruteforce/engine/hooks.def`). weemitt has no single
option that turns off every such addition. A later addition that the controller can reach needs its own switch.
