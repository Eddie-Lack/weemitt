# Original physics and interpolation

The port has two ways to run the game's logic. The player chooses one in Settings, Graphics, Physics.

| Choice | Logic | Drawing |
| --- | --- | --- |
| Smooth (frame-rate independent) | one update per drawn frame, for the time that has passed, in steps of 0.1 to 1 original frame ([delta-time.md](delta-time.md)) | each drawn frame shows the latest update |
| Original (interpolated) | one whole update per original frame: 20 a second in a level, 30 in a menu. Each update has `deltaTime` 1, as on the N64 and as `--fixed` runs it | the window draws at its own rate and blends the previous original frame with the latest one |

The Original choice exists for glitches that need whole frames. A wall clip that works at 20 updates a second stops
working when the logic runs in shorter steps. In Original mode the logic is the N64's, frame for frame, so the
clip works. The picture is still smooth at any frame rate.

This file describes how the Original mode works. `--original-physics` and `--smooth-physics` set the choice for one
run. `settings.ini` keeps it as `physics_mode` (0 Smooth, 1 Original). The default is Original.

## Terms

| Term | Meaning |
| --- | --- |
| game frame | one original frame of the logic: one call of `port_game_update(1.0f)` |
| drawn frame | one picture the window draws and swaps |
| `t` | how far a drawn frame is from the previous game frame to the latest one, from 0 to 1 |
| record | a matrix load (`G_MTX`) of one game frame, with its key and its matrix |
| key | a number that names the same object in two game frames |
| cut | a change that is too large to blend: a teleport, a camera cut, a level change |

## Timing

The window loop (`main` in `src/port/platform.c`) adds the real time to an accumulator. For each whole original
frame in it, the loop runs one update of length 1.0 and tells the renderer about the new game frame. The time left
in the accumulator is the time since the latest game frame. A drawn frame uses `t` = that time divided by the length
of an original frame (`port_game_retraces()` sixtieths of a second), always below 1.

At `t` = 0 the picture shows the previous game frame. At `t` close to 1 it shows the latest one. So the picture is
one game frame behind the game: 50 ms in a level and 33 ms in a menu, at most. The input is read at the start of each
game frame, as on the N64, so the delay also applies to what the player sees of the response.

The frame-rate limit and VSync apply to drawing in both modes. In Original mode no update is shorter than a frame,
so "Unlimited" stops at 1000 drawn frames a second instead of 10 times the game's rate.

### Pacing with VSync

With VSync on, the window loop asks SDL for the display's refresh rate (`SDL_GetCurrentDisplayMode`) and lets VSync
set the pace in two cases. The limit is at or above the refresh rate: the software limiter is skipped. The limit is a
whole fraction of the refresh rate (60 on a 120 Hz display): the swap interval is set to that number (2) and the
software limiter is skipped. Any other limit uses a swap interval of 1 and the software limiter (sleep, then spin
for the last 2 ms). If frames return much sooner than a refresh 30 times in a row, VSync is not working (a virtual
display, a driver that ignores it) and the software limiter takes over.

The reason: the software limiter measures from the start of each loop pass, and the pass starts a little after the
swap returns. The start of a frame therefore drifts against the refresh by that little time each frame. When it
passes a refresh, the swap waits a whole extra refresh: one frame lasts one refresh longer and the next one is
short. The time `t` is taken at the start of the pass, so the delay from that moment to the picture on the screen
wanders between about 1.5 ms and one refresh. A model of the loop (120 Hz, 1.5 ms of work a frame, 0.1 ms of loop
overhead) gives an uneven frame every 80 frames, and every 10 frames with 1 ms of overhead. With the limiter
skipped, each pass starts right after a refresh and the delay is the same every time.

The player videos that show an exact repeat every fifth frame were captured at 60 frames a second from a 120 Hz
display. The model gives uneven frames at periods that depend on the loop overhead, and the same period at limits
of 60 and 120 would be a coincidence. The capture software resampling the 120 Hz output is the more likely cause of
a steady period of five. The change above removes the port's own source of uneven frames. The camera follow speed
option works in both modes: it changes the camera's factors, not the update length.

A recorded demo (the attract mode) runs whole frames in both modes. In Smooth mode it is drawn without
interpolation. In Original mode it is interpolated like the rest.

`--fixed` still runs whole frames with no interpolation.

## What the renderer does

The game builds the display lists of a game frame before its logic runs, and keeps them in one of two buffers
(`dynamic[0]` and `dynamic[1]`, switched by `draw_buffer`). Nothing changes the latest frame's lists and matrices
until the next update. The renderer can therefore interpret the latest lists as often as it likes. It needs no copy.

Two passes use the lists.

1. **The recording pass** (`gfx_interp_record`, called once after each game update). It reads the latest lists
   without drawing. It follows the matrices, the vertex loads (for the clip codes that `G_CULLDL` needs), the
   viewport and the texture rectangles. It keeps a record of each. Then it finds the partner of each record in
   the previous game frame. It takes about 0.25 ms for a level frame.
2. **The drawing pass** (`gfx_draw_frame`, called for each drawn frame). It interprets the latest lists as it
   always did. At each `G_MTX`, texture rectangle, Sprite2D draw and viewport it finds the record again and replaces
   the value with the blend of the partner's value and the latest value at `t`. A record without a partner is
   drawn as it is. When `t` is 1 or more, or the frame has no previous frame, the pass changes nothing. Its cost is
   0.05 ms more than a plain frame.

Because the vertices, textures, lights and colours come from the latest frame, only the matrices and the 2D
positions move between game frames.

### Keys

A matrix load gets a key from three things:

- the kind: model-view or projection, load or multiply (`push` is left out);
- the call path: a hash of the addresses of the display lists that are being run (`G_DL` targets), but only the
  addresses outside the game's two dynamic buffers, which are rebuilt every frame and move;
- the address that the matrix is used for: the next `G_VTX` or `G_DL` command after the load, found by reading
  ahead (0 when the next thing is another matrix load, and 1 when the address is in a dynamic buffer). Object
  display lists and vertex arrays live in the object banks or the heap and keep their address.

The matrices form a tree: a load that multiplies the matrix on top of the stack is a child of the record that
loaded that matrix. Each record stores its parent's index. This keeps the bones of one enemy apart from the
bones of another enemy of the same kind.

### Matching

A record of the latest frame takes a record of the previous frame that is unused, has the same key, and has the
parent that the latest record's parent matched. If there are several, the nearest by translation wins (a tie goes to
the one with the same occurrence number). A record without a partner then
tries again by kind and call path alone, within 80 units. The game keeps two copies of some meshes and swaps
them every frame, and the address that such a matrix is used for alternates.

A pair blends in one of three ways.

- **Full blend**: the change is small. The rotation, scale and translation all blend.
- **Move only**: the translation changed by less than its limit, but the turn or the stretch is beyond its limit.
  The translation blends. The rotation and scale take the latest frame's value. A ball that spins more than 170
  degrees in a frame still moves smoothly. Its children match and blend in the parent's frame as before.
- **Cut**: the translation is beyond its limit, the two matrices are of different kinds, or the camera turned or
  moved too far. The record is drawn at the latest frame's value, and so is everything below it in the tree.

The limits, in `gfx_interp.c`:

| Quantity | Limit | Beyond it |
| --- | --- | --- |
| translation of a model matrix | 150 units | cut |
| camera position | 100 units | cut |
| turn of the camera | 60 degrees | cut |
| turn of a model matrix, shortest way | 170 degrees | move only |
| turn of a model matrix, long way (see "Turns of more than half a turn") | 300 degrees | move only |
| scale of a model row, or change of the stretch | a factor of 2 | move only |

The values come from runs of levels 5, 10 and 35: in normal play the camera moves up to 25 units a frame, and the
camera cuts that the game makes move it by 120 units or more. A model matrix moves up to 18 units a frame, and
respawns and teleports move it by hundreds. A model that spins more than 170 degrees in a frame, or whose scale
grows from zero, blends its translation only.

The cut flag of `port_game_cut` (`frame.c`) is also set when the game mode or the level changes or the frame
counter goes back. A frame with the flag has no partners.

### Blending

A model matrix is split into a stretch, a rotation (a quaternion) and a translation. The blend turns the rotation
by a part `t` of the turn from the partner to the record (see below) and blends the stretch entry by entry. Two kinds of split
exist. When the rows of the 3x3 part are at right angles, the stretch is the length of each row (scale). When they
are not, the model is squashed or sheared, as the ball is after a bounce: the split is then a polar decomposition
(the rotation that Newton's iteration finds, and a symmetric stretch). A spinning, squashed ball then blends as a
turn. A matrix with a
mirror, a scale near zero or a perspective part is blended entry by entry, and the projection matrix is blended that
way. Two matrices of different kinds (a scale and a polar stretch, or a hidden model that comes back) blend their
translation only when the entries differ much.

The camera (a view matrix, the multiply on the projection stack) is a rigid motion. Its translation is the position
of the eye turned into view space. The blend works on the eye position, then recomputes the translation, so the
camera moves in a straight line in the world and not in view space.

### Turns of more than half a turn

Two rotations alone do not say how far a part turned. A part that spins 240 degrees in a frame looks, from the two
frames, like a part that turned 120 degrees the other way. Each record therefore stores the turn from its partner
(`Rec.turn`: an axis times an angle). The next frame's record uses it as the last known turn.

- The turn is the shortest way, up to 170 degrees. Beyond that the record is move only.
- When the partner has a last turn and the rotation is at least 60 degrees from its partner's, the long way is a
  candidate (the other direction, 360 degrees minus the shortest angle, up to 300 degrees). It wins when it lies
  nearer to the last turn than the shortest way does, and within half of the last turn's size plus 20 degrees.
  A part that keeps spinning in the same direction at 200 or 250 degrees a frame then blends along its path.
- The first frame of a spin has no last turn, so it takes the shortest way.

The glove's spin when the ball transforms (hold R with the ball in hand) turns the palm by 115 degrees a frame,
and its fingers by 150 to 175 degrees in the first frame. Most of these are below the limit of 170 degrees and
blend.
In the drawn frames between two game frames the palm turns by 10 to 60 degrees each time, and the end of one game
frame is the start of the next.

### Rectangles and the viewport

A texture rectangle (the game's HUD, text, particles and stars) and a Sprite2D draw (a backdrop) are records too.
The key is the image, the part of it shown and the flip. Many letters of a text share one key, so a 2D rectangle is
not matched by distance: the nearest letter with the same key is often in another line, and the letters then seem to
swap places. The game draws a screen's rectangles in the same order in every frame. The two frames' lists of keys are
therefore aligned in order (a longest common subsequence; lists too long for the table are aligned by position). An
aligned pair is kept when it moves less than 3 pixels, or when it belongs to a run of three or more pairs that
follow each other in both lists, the run moves less than 40 pixels as a whole, and the pair's move is within 8 pixels
of the run's middle move. A text that slides, waves or grows keeps its letters together, and one or two letters that
align by chance with letters of another text are not blended. A pair must also move less than 80 pixels, and the sizes must be alike. A rectangle left over may take
the previous frame's rectangle of the same image (any part) within 40 pixels, when that image is drawn once in each
frame (a HUD icon whose picture changes in every frame). The four corners are blended. The viewport is blended by its
number within the frame, if the change is below 60 pixels.

### 3D sprites

The game places a 3D sprite (a garib, a particle, a trail) itself, in `PrintSprite`: it projects the sprite's
position with the camera of the latest frame, and draws a texture rectangle with a primitive depth. The recording
pass therefore also follows the render mode and the primitive depth. A rectangle with the depth source set to the
primitive depth is a 3D sprite. The renderer keeps what it needs to undo the projection: the view-projection matrix,
the viewport and the depth.

- **Recording**: the centre of the rectangle and its depth are turned back into a world position (the inverse of
  the view-projection matrix). The distance to the camera is kept too. The view-projection matrix at that moment is
  the latest frame's, so the world position is exact.
- **Matching**: a 3D sprite takes the nearest unused 3D sprite of the previous frame by world position, within 80
  units. A sprite with another image counts 40 units farther, since the images of a spinning or animated sprite change
  every frame. The screen position plays no part, so a fast camera turn does not stop a match.
- **Drawing**: the world position is the blend of the partner's and the latest one (the latest one when there is no
  partner). It is projected with the view-projection matrix of the drawn frame, which holds the blended camera. The
  rectangle keeps its size scaled by the ratio of the distances to the camera, and its clip depth is the new depth.
  A sprite that does not move then stays fixed in the world as the camera turns, and a garib that bobs or moves
  blends along its path.

The recording pass runs the render mode commands `G_SETOTHERMODE_H`, `G_SETOTHERMODE_L`, `G_SETOTHERMODE` and
`G_SETPRIMDEPTH`. Its rectangles then have the same corners as the drawn ones, in copy mode too.

### Not interpolated

- Vertices that the game builds or changes on the CPU. They follow the latest game frame. The level runs showed the
  sparkle particles and the animated figures of level 30 as the largest remaining differences.
- A 2D rectangle whose image changes in every frame (a spinning star on the HUD): it matches by image only, when the
  image is drawn once in each frame, and it moves only if the change is below 40 pixels.
- A letter or icon that moves 3 pixels or more and is not part of a run of three aligned rectangles, or that moves
  more than 8 pixels away from its run's move. It follows the latest game frame.
- A 3D sprite whose partner is wrong (two sprites that swap places within 40 units). The sprite then slides to
  its neighbour's place over one game frame.
- A sprite is placed in the world at its centre. A sprite whose anchor is not at its centre grows about its centre
  instead of its anchor, by a fraction of a pixel.
- Colours, fades, fog, lights and texture animation. A fade shows the latest value.
- Anything with no partner (a new object) or a cut.

## Results

Matrices matched with a partner, over 400 to 600 frames of play with generated input (`GLOVER_INTERP_STATS=1`):

| Level | Matrices a frame | With a partner | Changed from the partner | By kind only |
| --- | --- | --- | --- | --- |
| 0 | 144 | 99.6% | 19.7% | 0.7% |
| 1 | 227 | 99.6% | 55.9% | 0.5% |
| 5 | 176 | 99.0% | 53.2% | 1.0% |
| 10 | 71 | 97.6% | 51.0% | 4.3% |
| 16 | 57 | 97.0% | 31.0% | 1.8% |
| 20 | 67 | 97.8% | 35.6% | 3.0% |
| 22 | 53 | 96.6% | 40.4% | 2.0% |
| 35 | 128 | 99.0% | 38.0% | 3.5% |

The attract mode (6000 frames) gives 99.5%. The matrices without a partner are mostly new objects and cuts.

`--interp-check` measures the picture. For each game frame it draws the latest frame at `t` = 0 and compares it with
the previous frame's own picture, and prints the mean difference in colour levels (of 255). It also prints the
difference between the two plain frames. To repeat the check for a level, run the window build under a virtual
display with input for that level. `input_script` in `tools/regress.py` writes generated play in the format of
`--input`:

```sh
python3 -c 'import sys; sys.path.insert(0, "tools"); import regress; print(regress.input_script(5, 302), end="")' > in.txt
xvfb-run -a ./build/linux/weemitt --rom ROM --headless 300 --level 5 --input in.txt --interp-check
```

With 300 frames of generated play, the mean difference at `t` = 0 is about 0.02 in level 0. In levels 5, 10, 16, 20,
22 and 35 it is between about 0.2 and 2.3. Two plain frames differ far more: 7 to 10 in level 35. The differences
left are particles, morphing meshes and fades (see "Not interpolated").

## Tests

| Option | Effect |
| --- | --- |
| `--headless N --interp-shot F --shot-prefix P` | after game frame F, save P`F`_t000.png, _t025, _t050, _t075 and _t100: the picture at `t` = 0, 0.25, 0.5, 0.75 and 1. Needs a display (`xvfb-run`) |
| `--headless N --interp-check` | print, for every game frame, the difference between the picture at `t` = 0 and the previous frame's picture, and between the two plain frames |
| `GLOVER_INTERP_STATS=1` | log, for every game frame, the number of matrices, the ones with a partner, the ones that moved, the cuts and the rectangles |
| `GLOVER_GFX_STATS=1` | also log the time of the recording pass |
| `GLOVER_INTERP_DEBUG=1` | log each cut, each move-only pair and each record without a partner, with its numbers: the distance, the shortest turn and the turn the blend follows, the scale before and after, the position. With `=2` also the pairs that blend in full and turn more than 40 degrees |
| `GLOVER_INTERP_TRACK=KEY` | log, for each drawn frame, the world position and the screen position (in 320 by 240 pixels) of the matrix with this key (hexadecimal, from a log line), and of a point 10 units along the model's x axis (`tip`), which shows the turn. A drawn frame at `t` = 1 is not logged. A pair that blends has the previous frame's value at `t` = 0, so the line for `t` = 0 of the next game frame is the line for `t` = 1 |

Example: `xvfb-run -a ./build/linux/weemitt --rom ROM --headless 120 --level 35 --input in.txt --interp-shot 100
--shot-prefix shots/a`. The pictures differ by the camera's motion: in level 35 the scene moves 9 pixels to
the left between `t` = 0 and `t` = 1, and 5 pixels at `t` = 0.5.

`tools/regress.py check` must still report 0 runs that differ. The headless build has no renderer, so the mode does
not affect it. The game's logic in Original mode is the logic of the existing `--fixed` path.

## Files

| File | Contents |
| --- | --- |
| `src/gfx/gfx_interp.c` | the records, matching, blending |
| `src/gfx/gfx_dl.c` | the hooks in the display list interpreter, the recording pass (`rsp.dry`), the key's call path and read-ahead, the view data of 3D sprites |
| `src/gfx/gfx.h` | `gfx_interp_record`, `gfx_interp_set`, `gfx_interp_set_volatile`, `gfx_interp_stats` |
| `src/port/frame.c` | `port_game_cut`, `port_game_dynamic_range` |
| `src/port/platform.c` | the timing loop, the frame limiter and VSync, and the options |
