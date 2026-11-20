# Frame rate: delta time

On the N64 the game's logic runs once per game frame: 20 times a second in the levels and 30 times a second in
the menus. Every speed, acceleration, damping factor, timer and counter is tuned for that rate. The port runs the
logic once per drawn frame, at any rate. This file describes how: the timing model, the API in `include/delta.h`,
the rules for rewriting the game's code, and the decisions for the hard cases.

The rewrite has one hard requirement. With updates of exactly one original frame, the game must behave bit for bit
as before. `python3 tools/regress.py check` proves it. Every rule below keeps that property.

## Terms

| Term | Meaning |
| --- | --- |
| original frame | one frame of the N64's logic: 1/20 s in the levels, 1/30 s in the menus (`desiredFrameRate` 3 or 2) |
| update | one call of `port_game_update`: the controller (sometimes), the draw path, then the game logic |
| `deltaTime` | the length of the update in original frames of the current mode; 0 < `deltaTime` <= 1 |
| tick | an update in which an original frame ends; `deltaTick` is 1 in it |
| frame window | the updates after one tick, up to and including the next tick; they make up one original frame |
| whole frame | a window of a single update: `deltaNewFrame` and `deltaTick` are both 1 |
| continuous | code that moves something visible and smooth: it runs every update, scaled by `deltaTime` |
| discrete | code that happens once per original frame: it runs only in ticks |

## The timing model

Speeds stay in units per original frame, and accelerations in units per original frame squared. Data from the ROM
keeps its units. Only the code that applies a value changes. Thresholds on speeds and distances therefore keep their
meaning.

The game's per-frame code falls into three kinds:

- **Continuous**: visible motion. Positions, velocities, rotations, scales, colours, alpha, fades of float values,
  camera motion, texture scrolls. It runs in every update and scales with `deltaTime`, using the forms in
  "Writing exact rewrites".
- **Discrete**: everything that happens once per frame. Timers and counters, decisions and state machine steps,
  random chances, spawns, "every N frames", scores, sound triggers, input hold counts. It runs only when
  `deltaTick` is 1, at the original rate. When unsure, and the thing is not visible motion, treat it as discrete.
- **Unchanged**: one-shot impulses (a jump sets a velocity once), responses to events (a bounce reflects a
  velocity), loading, set-up, and code that does not run every frame.

A function that does both must be split: its continuous statements run every update and its discrete statements run
under `deltaTick`. Keep the original order of the statements.

In a frame window the continuous code runs in every update, and the discrete code runs once, in the last update
(the tick). On the N64 one frame also moves things first (`UpdateActor`) and makes its decisions after (`MoveHand`,
the enemy scripts). The window keeps that order.

## The update

`port_game_update` (`src/port/frame.c`) runs one update:

1. `DeltaBeginUpdate(dt)` sets `deltaTime`, `deltaTick`, `deltaPhase`, `deltaNewFrame` and `deltaFrame`.
2. In a tick, the frame rate the game asked for takes effect. In the first update of a frame window
   (`deltaNewFrame`), `ControllerStep` reads the controller. Its button presses (`pad[].pressed`) are visible in
   that update only: in the other updates `DeltaHoldInput` clears them. At the end of the first update
   `DeltaKeepInput` keeps the presses that are left, for the code that runs only in the tick.
3. `GraphicsStep`, the draw path. Outside a tick, the draw path's calls to `Random` leave the game's random seed as
   it was: the frame driver saves `rseed` before `GraphicsStep` and restores it after.
4. `GameStep`, the game logic.
5. `DeltaEndUpdate` records the tick and whether `frameCount` and `frameCount2` moved.

A tick happens when the time since the latest frame boundary reaches one original frame. The remainder carries
into the next window. Two events end the current frame at once and restart the time from zero:

- `gameInfo.mode` differs from its value at the start of the previous update;
- `frameCount` is lower than at the start of the previous update (a level or a screen started again).

The update after such a change is a whole original frame, whatever time has passed: `deltaTime` is 1, and
`deltaTick` and `deltaNewFrame` are 1. The discrete set-up code of the new mode's first frame runs there, and its
motion moves a whole frame, as at `deltaTime` 1. A shorter first update would leave all later motion behind the
discrete logic and the input by the missing part of a frame. The game then runs ahead of real time by less than
one frame, usually behind a fade. `GameStep` runs the menu screens before its switch on the mode. When a screen
there changes the mode, the new mode's code also runs once in the same update, which may not be a tick.

When the game changes `desiredFrameRate`, the change takes effect at a tick. The time left over is measured in the
old mode's frames. `DeltaBeginUpdate` converts it to the new mode's frames.

The platform layer (`src/port/platform.c`) gives each update the time that has passed, and splits a gap longer
than one original frame into updates of at most one frame. After a pause or a hitch it gives updates of exactly 1.0
between shorter ones. Code must work in such mixed runs (see "Writing exact rewrites"). Recorded demos (the attract
mode) always run with updates of exactly one frame: their recorded input only fits the original rate. `--fixed` runs
the whole game that way. The Original physics mode (Settings, Graphics, Physics) also runs whole frames, and draws
between two of them by interpolation ([interpolation.md](interpolation.md)). Its logic has `deltaTime` 1 in every
update.

## The API

`include/delta.h` declares it and `src/game/delta.c` defines it. None of its state is in `port_ram`. Other
files do not change them. A file that needs another helper writes a static function of its own.

Every helper returns its original value when `deltaTime` is 1. Most check `deltaTime == 1.0f` first and return
their argument unchanged. The others are exact for a stated reason.

### The update

| Name | Meaning | When `deltaTime` is 1 |
| --- | --- | --- |
| `deltaTime` | the update's length in original frames | 1.0f |
| `deltaTick` | 1 in a tick | always 1 |
| `deltaPhase` | the time since the latest frame boundary at the end of the update, 0 <= p < 1 | 0.0f |
| `deltaNewFrame` | 1 in the first update of a frame window (the previous update ticked) | always 1 |
| `deltaFrame` | a number for the current frame window; every update of the window has the same number | a new number each update |

### Factors

| Helper | Use | Value | Exact at 1 because |
| --- | --- | --- | --- |
| `DeltaPow(k)`, `DeltaPowD(k)` | a factor on a quantity that is not a velocity: a scale, an alpha, a zoom, a squash amplitude, a growth | k^dt | returns k. `DeltaPowD` keeps a double constant a double: `x *= 0.9` multiplies in double, and 0.9f is a different number |
| `DeltaLerp(k)`, `DeltaLerpD(k)` | approach `x += (t - x) * k` | 1 - (1 - k)^dt; k * dt outside 0..1 | returns k |
| `DeltaDiv(d)`, `DeltaDivD(d)` | approach `x -= (x - t) / d` | 1 / (1 - (1 - 1/d)^dt) | returns d. `x / 3.0f` and `x * (1.0f / 3.0f)` differ, so a division needs its own helper. Division by 2, 4 or 8 equals multiplication by 0.5, 0.25 or 0.125 exactly, and `DeltaLerp` serves |
| `DeltaTake(keep, take)`, `DeltaTakeD` | blend `x = x * keep + t * take` with two separate constants | 1 - keep^dt | returns take: in float 1 - 0.8f is not 0.2f |
| `DT(x)` | a per-frame amount | x * dt | x * 1.0f is x |

### Motion

| Helper | Use | Value | Exact at 1 because |
| --- | --- | --- | --- |
| `DeltaDamp(k)`, `DeltaDampD(k)` | damping a velocity, `v *= k`, where the velocity moves a position by `v * deltaTime` | 1 - (1 - k) dt for 0 <= k <= 1; `DeltaPow(k)` outside | returns k |
| `DeltaAccelPos(dv)` | the position correction for a velocity change dv that gravity (or another sustained acceleration) made after the update's position step | dv (dt - 1) / 2 | returns 0.0f. Do not add it when `deltaTime` is 1: x + 0.0f turns -0.0f into +0.0f |
| `DeltaPerFrame(x)` | a change measured over the update, as a rate per frame | x / dt | x / 1.0f is x |

### Counters and time

| Helper | Use | Value | Exact at 1 because |
| --- | --- | --- | --- |
| `DeltaLag()` | continuous time from a tick counter read before this update's tick step: `count + DeltaLag()` | `deltaPhase` in a tick, `deltaPhase - 1` otherwise | 0.0f, and an integer count plus 0.0f is the count |
| `deltaPhase` | the same for a read after the step: `count + deltaPhase` | | 0.0f |
| `FrameTime()` | `frameCount` as continuous time, for code that runs before `GameStep` steps it: the draw path, `MainGameLoop` and all it calls, the menus | `frameCount + DeltaLag()` while `frameCount` moves; `frameCount` while it stands still (pause, modes 2 and 15) | (f32)frameCount |
| `FrameLag()` | `FrameTime() - frameCount`, for `SineWave`, whose time argument is an s32: `SineWave(f, phase + FrameLag(), frameCount)` | | 0.0f. `phase + 0.0f` differs from `phase` only for -0.0f, and `base + -0.0f` equals `base + 0.0f` for an integer base |
| `FrameTimeAfter()` | `frameCount` as continuous time after `GameStep` stepped it: `CheckForExits` and its callees, `GameOverScreen` | `frameCount + deltaPhase` while it moves | (f32)frameCount |
| `FrameTime2()` | the same as `FrameTime` for `frameCount2` | | (f32)frameCount2 |
| `DeltaTickLerp(prev, cur)` | the value to draw for a quantity that changes only in ticks; `prev` is its value before the latest tick's step | `cur` in a tick, else `prev + (cur - prev) * deltaPhase` | returns cur |

### Events and whole frames

| Helper | Use | Exact at 1 because |
| --- | --- | --- |
| `DeltaFirst(&stamp)` | TRUE the first time it is called with this stamp in a frame window, then FALSE until the next window. For a one-shot block that must run once per frame in whichever update reaches it first. A stamp is a `static u32` set to 0 | every update is its own window, so it always returns TRUE |
| `DeltaTickPressed(pad, button)`, `DeltaTakePress(pad, button)` | the window's press of a button for code that runs only in the tick, and clearing it (see "Input") | the first update is the tick, so it is `pad[].pressed[]` itself |
| `DeltaEnterFixed(&saved)`, `DeltaLeaveFixed(&saved)` | run the code between them as one whole frame: `deltaTime` 1, `deltaTick` 1, `deltaPhase` 0, `deltaNewFrame` 1, a new `deltaFrame`. For loops that simulate frames inside one update. Enter and leave once per simulated frame | the values are already those; only `deltaFrame` changes, and nothing in the game's memory depends on it |
| `AnimDrawTime(anim)` | the animation time to draw (anim.c) | returns `anim->time` when `deltaPhase` is 0 |

## Writing exact rewrites

| Original, per frame | Rewrite | Notes |
| --- | --- | --- |
| `x += v` | `x += v * deltaTime` | every position that a velocity moves |
| `v += a` (thrust, every frame) | `v += a * deltaTime` | a one-shot impulse stays `v += a` |
| `v *= k` (a velocity) | `v *= DeltaDamp(k)`, or `DeltaDampD(k)` for a double constant | |
| gravity: `x += v; ...; v -= g` | `v -= g * deltaTime`, then `x += DeltaAccelPos(-g * deltaTime)` when `deltaTime != 1.0f` | use the change the code really made: none when a terminal speed clamps it |
| `v = (v + a) * k; x += v` (thrust first, damping after) | `v = (v + a * deltaTime * (k / DeltaDamp(k))) * DeltaDamp(k)` | the thrust is damped in the same update, so it needs the factor k / DeltaDamp(k) to reach the N64's total; it is 1.0 at `deltaTime` 1 (the camera's side-offset spring, `actor.c`) |
| a speed cap before a later thrust: `v = MIN(v * k, cap)`, then `v += push` | each update subtracts `deltaTime * MAX(v * k - cap, 0)` | a cap of `cap` in every update would also cut the pushes of the frame's earlier updates (`CapCameraRise`, `actor.c`: the camera's rise) |
| `x *= k` (not a velocity) | `x *= DeltaPow(k)`, or `DeltaPowD(k)` for a double constant | |
| `x += (t - x) * k` | `x += (t - x) * DeltaLerp(k)` | |
| `x -= (x - t) / d` | `x -= (x - t) / DeltaDiv(d)`; for d = 2, 4, 8 also `* DeltaLerp(0.5f)` and so on | |
| `x = x * 0.8 + t * 0.2` | `x = x * DeltaPowD(0.8) + t * DeltaTakeD(0.8, 0.2)` | |
| `angle += w` | `angle += w * deltaTime` | |
| step capped at `s`: `x += MIN(s, (t - x) / n)` | `x += MIN(s * deltaTime, (t - x) * DeltaLerp(1.0f / n))` for n a power of two; `DeltaDiv` otherwise | |
| `frameCount` in a visual formula | `FrameTime()`, `FrameLag()`, `FrameTimeAfter()` | |
| a counter `c` in a visual formula | `c + DeltaLag()` before its step, `c + deltaPhase` after it | for a counter that counts down, subtract instead |
| `timer--` and other discrete code | `if (deltaTick) timer--;` | |

Rules for exactness:

- A constant written as a double (`0.9`, `1.3`) makes the original multiply in double. Use the `D` helpers or
  multiply `deltaTime` into a double expression. `DeltaPow(0.9f)` would change the result at `deltaTime` 1.
- `SCALE_VECTOR` evaluates its factor three times. Compute a helper's value once into a local first.
- Do not add a term that is 0 at `deltaTime` 1 without checking `deltaTime != 1.0f`: adding +0.0f turns -0.0f into
  +0.0f, which the memory hash sees.
- A branch `if (deltaTime == 1.0f) { original } else { new }` is allowed, but the platform layer mixes updates of
  exactly 1 with shorter ones. The original branch must then be safe for any state the new branch leaves. An
  integer that the original steps by 10 towards 255 may wrap if a fractional update left it at 248. Prefer one form
  that is exact at 1 for all updates.
- The build uses SSE arithmetic and `-ffp-contract=off`, so there is no excess precision and no fused multiply-add.
  `x * 1.0f`, `x / 1.0f`, `x * 0.5f` against `x / 2.0f`, and `x + 0.0f` (for x not -0.0f) are exact.
- The helpers use the C library's `pow`, `exp` and `log` when `deltaTime` is not 1. Their last bits may differ
  between Linux and Windows. Traces of the two builds agree at `deltaTime` 1 only.
- Never change the type or size of a global or a structure in `port_ram`, and never rescale data from the ROM.
  New state is a plain C global or a static, which the build places outside `port_ram`.
- Port-side state that shadows game state must be reset where the game resets that state. A copy of a
  `handControl` field, for example, is cleared in `ResetHand` next to the `memclear`.

## Cross-file rules

### frameCount and frameCount2

`GameStep` (`main.c`) steps `frameCount` and `frameCount2` only in ticks, under the same conditions as before
(mode 1; modes 4 and 5 with `frameCount` only while `pauseMode` is 0). `SelectLanguage` (`game.c`) steps its own
`frameCount` only in ticks too. In a frame window `frameCount` holds one value. Every test of the form
`frameCount % N`, `frameCount & N` or `frameCount == N` that triggers something must therefore run under `deltaTick`
or `DeltaFirst`, or it fires in every update of that frame. Visual formulas use `FrameTime()`, `FrameLag()` or
`FrameTimeAfter()`.

`dispFrameCount` steps only in ticks as well (`GraphicsStep`). `jalloc.c` reads it.

### Animation

- The draw path advances animations. `DrawActor` (`object.c`) calls `UpdateAnimations` only in ticks.
  `UpdateAnimations` itself is unchanged: one call is one original frame. `anim->time` therefore steps once per
  original frame, as on the N64.
- `DrawActor` draws the pose at `AnimDrawTime(anim)`: `anim->time + anim->speed * deltaPhase`, kept inside the
  current animation. The drawn pose and the collision spheres that `TransformAndDrawObject` writes move smoothly.
  When the actor does not animate (paused, `animateActors` clear, a frozen enemy), it draws `anim->time`.
- `finishedAnim` is set by a tick's `UpdateAnimations` and cleared by the next one. It stays set through the
  updates in between. Code that acts on it must run under `deltaTick`, or do something that is harmless to repeat.
- `TriggerAtFrame` (`nmeprocess2.c`), `QueryAnimTime` (`anim.c`), `QueryAnimTimeInline` and the inline copies of
  their window return FALSE outside ticks. Their window `[frame, frame + speed)` is unchanged. In ticks they see the
  N64's values, and each key frame fires once.
- `HandSwish` (`object.c`) predicts the pose one update ahead: `time += speed * deltaTime`. Its caller passes
  `AnimDrawTime` of the pole's animation as `time`.
- An animation speed is in animation frames per original frame. `AnimateActor` and `SetAnimationSpeed` take it
  unchanged.

### The actor integrator

`UpdateActor` (`actor.c`) integrates the glove, the ball, the camera, the enemies, the bullets and the debris. It
runs in every update. Every caller must call it in every update, never inside a tick gate.

- `UpdateActorPosAndRot` computes, for each axis, the factor m that the original multiplies the velocity by (one of
  its four friction formulas, unchanged). It then does `pos += vel * deltaTime` and `vel *= DeltaDamp(m)`. The same
  holds for rotation: `rot += rotVel * deltaTime`, `rotVel *= DeltaDamp(rotDrag)`; a quaternion actor turns by
  `spinSpeed * deltaTime` and `spinSpeed *= DeltaDamp(rotDrag)`.
- Gravity is added after the position step. It becomes `gravity * deltaTime` per update. In the free-fall branches
  the position also gets `DeltaAccelPos(dv)`, where dv is the change gravity made to the y velocity in this update
  (0 when the terminal speed clamps it), and only when `deltaTime != 1.0f`. Jump heights then match the N64's.
  Without it, an arc drops by an extra half of one frame's gravity for every frame of flight.
- Free flight (`FallsFreely`): `DeltaAccelPos` applies only to an actor that touched nothing in this update, or only
  slid along walls (`contactFlags` 1, no bounce, every contact plane with y between -0.5 and 0). An edge, another
  actor, a bounce or a ceiling ends it. An actor that began the update with no vertical speed is not in free flight
  either, because other code holds such an actor in place in every update. A sustained lift that code adds to the
  glove's or the ball's speed after the position step (the held-jump lift, the burst ball's extra gravity) is
  corrected in the same way, with `contactFlags == 0` as the test. The enemy scripts' extra accelerations (a bob, a
  sink, a hover) count as per-frame thrust: `deltaTime` times the amount, with no `DeltaAccelPos`.
- Speed clamps (`maxSpeed`, the 15.0 cap) are unchanged: they limit speeds, which keep their units. The exception is
  the camera's rise cap, which is linear (`CapCameraRise`, see "Writing exact rewrites").
- The camera's height speed is damped twice per frame: by the actor's friction `k` in `UpdateActorPosAndRot` and by
  0.8 in `UpdateActorPositionDependencies`. Two linear dampings add their losses, while the N64's factors multiply.
  When `deltaTime` is not 1 the second damping is `DeltaDampD(0.8 * k) / DeltaDampD(k)`, which makes the update's
  total `DeltaDampD(0.8 * k)`.
- An actor resting on a surface reaches the next collision with about `gravity * deltaTime` of downward speed,
  not `gravity`. A threshold that stands for "one frame of falling" must compare per-frame amounts (see the
  collision.c ripple in "Decided sites").

Why linear damping. With `x += v * deltaTime`, `v *= DeltaDamp(k)` and `v += a * deltaTime`: a velocity that
per-frame thrust holds steady keeps the N64's speed, a speed capped every frame moves at the cap, and a coasting
velocity covers the N64's total distance. Only the shape of a decay differs a little: after one frame of coasting a
velocity damped by 0.6 has about 0.67 of its value instead of 0.6. Exponential damping (`DeltaPow`) gets that shape
right, but under steady thrust it runs slow (by 17 % at a damping of 0.6, by 35 % at the camera's 0.27), and a
coasting velocity stops short by about as much. A form that is exact for coasting at whole frames (moving by
v (1 - k^dt) / (1 - k)) runs 20 % fast or more whenever code tops up or clamps the velocity in every update, as the
glove's controls and the enemies' steering do.

### Collisions

`CheckCollisions` (`collision.c`) runs in every update. At the end of its no-bounce branch it turns the actor's travel
in this update, collisions included, into a velocity: `DeltaPerFrame(pos - prevPos)`. Several rules adjust this for
updates shorter than a frame.

- **A push out of an old overlap is a displacement.** The push has three parts. One undoes this update's travel into
  the surface, and one follows the growth of a sphere in this update (`collisionGrowth`). Both scale with
  `deltaTime` and stay speeds. The rest moves the actor out of a surface it was already inside when the update began.
  That rest would become a speed of depth over `deltaTime`. `TakeBackOverlapSpeed` takes the excess off, so the rest
  counts as one frame's speed. It runs when `deltaTime` is not 1, for actors that are not the camera or the glove's
  front probe. The edge branch applies it again after it builds the velocity from the travel. An actor in contact with
  a platform counts the platform's travel in this update (`contactPlatform->actor.pos - prevPos`) as part of its own
  travel into the surface: that push stays a speed, and only the rest of the overlap is taken off. Without this, a
  glove that left the ball beside a platform edge was pushed out of the edge at depth over `deltaTime` and flew 7 times
  too high at `deltaTime` 0.05.
- **A sliding glove on a slanted face** (`planes[0]` with y from 0 to 0.9) counts the whole push as a speed per original
  frame (`TakeBackSlidePush`), because the N64 turns that push into a speed once per frame. Other actors use
  `TakeBackOverlapSpeed`.
- **The minimum-speed stop is a decision of the tick**: `deltaTick && LENGTH_2D(travel) < minSpeed * deltaTime`. In every
  update an actor that starts from rest, with less travel per update than `minSpeed`, would never move.
- **The push down** (`vel.y -= gravity / 7`) on a resting actor is per update, scaled by `deltaTime`. On an actor in
  flight it is a test on a speed near 0 at the top of an arc and runs once per original frame, in the first update of
  the window, where the speed is the N64's.
- **Bounce speed.** A bounce reflects the speed of the path in the middle of the original frame. In free flight,
  `ActorFall` moves the actor by `DeltaAccelPos`, so the velocity is the speed of the path in the middle of the
  update. `FlightLift` gives the difference, (1 - `deltaTime`) / 2 frames of gravity. `CheckCollisions` adds it to
  `vel.y` for the tests, together with a shift of `flightGravity * (phaseAtEnd - deltaTime / 2 - 0.5)` that moves the
  speed on to the middle of the frame (`phaseAtEnd` is 1 in a tick, else `deltaPhase`). A bounce takes off twice the
  lift, because the N64 adds a whole frame of gravity after a bounce. The early returns take off the lift and the shift.
  Without this each bounce gains speed, and a bouncing ball never comes to rest.
- **Contact counts** (`obj->contactCount`) count original frames. A count of 0 steps at once, so the contact counts as
  held in the next update. After that a count steps in the first update of a frame (`deltaNewFrame`). The glove's and
  the ball's counts step only when the count started more than n frames before (`contactStart`), because `MoveCam`
  and `MoveHand` read them as `>= 2`.
- **Damage and hurt effects** of `CheckActorCollisions` apply once per pair of spheres per original frame (see
  "Decided sites"). The tunnelling guard and the push apart scale with `deltaTime`.
- **The water ripple** (`terrain == 12`) is discrete. An actor that touched the same plane in the previous update
  reaches it with one update's fall speed, so the test uses `dot / deltaTime`.
- Known limit: a continuous push into a surface by other code (an actor that game code moves into a wall in every
  update, or a moving actor that is not a platform) counts as an old overlap. Its speed then becomes the push per
  update instead of per frame.

### Accelerations and impulses

- `AccelerateActor`, `AccelerateActor2D`, `AccelerateActorFromActor`, `AccelerateActorFromQuaternion` and
  `AccelerateActorAcrossPlane` serve both sustained thrust and one-shot impulses. They stay unscaled.
- A caller that applies thrust every frame passes the amount times `deltaTime` and calls it in every update. A
  one-shot caller passes the full amount once, in one update.
- Logic damping of a velocity (`SCALE_VECTOR(&hand.vel, 0.8)` every frame) uses `DeltaDampD(0.8)`.

### Turning and slerps

- `ActorHandleRotaim` is continuous. Its step becomes `MIN(turnSpeed * deltaTime, shortest * DeltaLerp(1.0f /
  val))`. At `deltaTime` 1 this is the original: `shortest * 0.5f` equals `shortest / 2.0f`. Direct callers must
  call it in every update.
- `QuatSlerpAtFixedSpeed` turns by a fixed angle per call: callers pass `speed * deltaTime`.
- `QuatSlerpAtFixedSpeedOld` is not a fixed-speed turn. Its angle is `PI - phi`, so each call moves a fraction
  `speed / angle` of the way: an approach. `maths.c` changes its slerp fraction to `DeltaLerp(speed / angle)`.
  Callers pass the unscaled speed and call it in every update.
- `QuatSlerp` with a per-frame fraction t: callers pass `DeltaLerp(t)`.

### Speeds from positions

`prevPos` is written in every update, by `UpdateActorPosAndRot` for actors and by the platform code for platforms.
`TransformAndDrawObject` writes the collision spheres' `pos` and `prevPos` in every draw. `pos - prevPos` is
therefore the travel over one update. Code that uses it as a velocity or a speed divides by `deltaTime`:
`DeltaPerFrame`. Code that uses it as a displacement (carrying a rider on a platform, `UpdateActorPlatform`) keeps
it per update. `oldHandPos` (`MainGameLoop`) is also written in every update.

### Events

The physics runs in every update, so its events (a landing, a contact, a hit) can happen in any update. The
discrete logic that reads them runs only in ticks.

- **Physics edges respond in the update where they happen.** `onGround && !wasOnGround`,
  `handControl.prevOnGround`, `ballControl.prevGround`, `obj->contactFlags`, `obj->platform` changes: these are
  physics state, rebuilt every update. Code that responds to them runs in every update, without a tick gate. The
  edge is true in one update only, so the response happens once. Such a response may call `Random` and play a
  sound: it happens once per event.
- **Hit flags that the game clears every update are latched until the logic reads them.**
  `handControl.touchedActor` and bit 8 of `ballControl.hitFlags` are cleared in `MainGameLoop` before the glove and
  the ball move. They are now cleared only when `deltaNewFrame` is 1. Bit 8 of `brain.flags` and bit 0x04000000 of
  an enemy's `actor.flags` are cleared at the end of `UpdateEnemies`. They are now cleared only when `deltaTick`
  is 1. A latched flag stays set from the update in which it was set until the clear. Code that reads it in every
  update must be harmless to repeat, or run under `deltaTick`.
- **A reader on the other side of a clear, or a reader that combines a flag with state that changes in the same
  update** (`ball.speed > 7.0f`, `onGround == 0`), keeps its own latch: a port-side static, one per enemy where
  needed, that it ORs the whole condition into in every update and consumes in the tick.
- **Enemy handlers latch their hit tests.** `LatchHit` (`nmeprocess2.c`) ORs a hit test into a latch for each enemy in
  every update. The latch holds for the rest of the original frame, keyed on `deltaFrame`, and the tick answers it.
  Each handler numbers its own latches from 0. The bowling ball's blast-ring test (`enemy.c`) has its own latch,
  cleared with the flags above at the end of the tick's `UpdateEnemies` and in `RestartEnemy`. The Prehistoric boss
  (`boss.c`) holds the ball's hit in `prehistHit` and `prehistHardHit` until the tick. `hitFlagAnswered` marks a flag
  that the tick has already answered. `enemy.c` needs no change for this.
- `ball.flags` bit 0x40000000, `handControl.flags` bit 1 and `hand.flags` bit 0x1000 keep their clear in every
  update. Their setters and readers run in every update.
- Damage from a contact (`CheckActorCollisions`) is applied once per pair of spheres per original frame.

### Water

The N64 decides once per frame whether an actor is in the water, from where it stands at the start of the frame.
`INWATER_*` (`state->waterState`) is therefore assigned only when `deltaNewFrame` is 1 and cleared in `UpdateWater`
only then. It holds for the whole frame window. The swimming bubble spawn stays gated by `deltaTick`.
`WaterDoFloating` applies the water's drag (0.8, 0.85 and 0.9 on actors that do not float, 0.9 on the floating 2D
case) once per frame window, in the first update, in place of `DeltaDamp`. On the N64 the drag comes before the position
step, and a linear `DeltaDamp` before the step would make a coasting actor go 1/k further. Known limit: an actor that
enters water in the middle of a window gets its water state in the next window. The glove loses up to one frame of fast
falling at the entry.
A floating actor that rests on the surface (the ball under the walking glove) dips in and out of the water in many short
updates. The entry's damping (`vel.y *= 0.5`, `vel.xz *= 0.7`, then `vel.y *= 0.8` at the exit) and the ripple sound
therefore act once per frame window per actor (`FirstWaterEvent`, a stamp table in `water.c`). Damped at every dip,
the ball stuck at the surface with its speed near 0 at `deltaTime` 0.05 and below.

### Things started in a tick

The frame's decisions come after its motion on the N64, so a thing that discrete code creates or starts in a tick,
before the motion code of the same update, gets a whole-frame first push. At `deltaTime` 1 the same update is a whole
frame. The sites:

- A bullet fired in a tick (by `UpdateEnemies` and the like, before `UpdateBullets`) moves a whole frame in its first
  update (`BulletTrack.firstUpdate`, `bullet.c`). `FireBulletFromActor` already adds one velocity at the spawn. Homing
  turn and acceleration in that update use the same step. A bullet first seen outside a tick moves by `deltaTime`.
- An enemy instruction picked in a tick runs at once through the recursive `NMEProcessInstruction` call after
  `UpdateActor`. Its thrust (`NMEActionMoveTo`, `NMEActionMoveTo2D`) takes a whole frame in that run
  (`NMEThrustTime`, `nmeprocess.c`). Instructions that `NMEAnalyseConditionalList` picks before the normal call are
  not changed. The `NMEActionMoveTo2D` push of flag 2 without the actor flag 0x1000000 is limited only by a speed
  test, so it pushes a whole frame in ticks and nothing in other updates.
- A cutscene item's motion (`CameoShare`, `cameo.c`).
- When AI code sets a velocity again after `UpdateActor` in every update and a tick decision then stops it, the stop
  needs the rest of the frame's damping (k / `DeltaDamp(k)`).

### Animation-gated motion

`anim->time` steps only in ticks, and the N64's frame sees the stepped time. A continuous move that is gated on
`anim->time` therefore tests the time that the window's tick will give: `HandAnimTimeAtTick` (`ball.c`) returns
`time + speed` outside a tick. The lift of the held ball onto the spinning glove uses it.

### The ball joining the glove

`ApplyAction(HAND_JOINED)` gives the ball the velocity that it had at the start of the original frame once the frame window has
started (`ballFrameVel` in `main.c`, stamped with `deltaFrame` in the window's first update). The N64 takes back the
frame's gravity and damping. One update would take back only its own. `ball.prevVel` is the velocity at the start of
the update. The test must not use `deltaTime`: at a frame rate limit of 20 a window often has a short first update and
a tick update of exactly one frame, and that tick must still restore the window's velocity.

### Input

The N64 reads the controller at the start of a frame, and the whole frame runs with the new stick and buttons. The
port reads it once per original frame, in the first update of the frame window (`deltaNewFrame`, `frame.c`). The
continuous code of the whole window then uses the frame's input, and the discrete code in the tick sees the same
input as on the N64. A button that goes down after a read counts as held at the next read, even if it is up again
by then. A button that the read saw held is not latched: the next read sees its release. Between two reads:

- `pad[].button[]` and the stick keep their values: a held button stays held.
- `pad[].pressed[]` holds the read's presses in the window's first update only, as the N64's frame has them from
  its start. `DeltaHoldInput` clears it in the other updates. Code that runs in every update and acts on a press
  therefore acts in the first update, before the window's other updates move and collide. At `deltaTime` 1 the first
  update is the tick, and nothing changes.
- Code that runs only in the tick (under `deltaTick`, or in a function that only the tick calls) must not read
  `pad[].pressed[]`: it would miss the press. It reads `DeltaTickPressed(pad, button)`, which is `pad[].pressed[]` in
  the first update and the copy that `DeltaKeepInput` made in the rest of the window. `DeltaTakePress(pad, button)`
  clears a press in both. These sites use them: `ConditionsMet` (the glove's action changes are decided in the tick,
  as the N64 decides them at the end of the frame's glove logic), `CheckForCheats`, `CheatMenuUpdate`, the pause
  menu's cheat list button, `AnyDebouncePressedTick` (the language screen, the credits, the flying-butterfly exits), the
  tip signs (`UpdateTips`, `UpdateTip`) and the hub chicken.
- A press that starts a counter or an animation which the draw path steps in ticks (a fade, `UpdateAnimations`) gets
  its first step in the tick of the same window. On the N64 the graphics run before the game logic in a frame, so
  that step comes one frame later. A menu fade that a press starts therefore ends one original frame sooner at a
  high frame rate. This is the price of acting at the start of the frame.
- `holdTime`, `sincePress`, `prevMag` and the stick angle history step once per read. A test on them that triggers
  something (`holdTime == 4`) must run under `deltaTick`.
- Code that does something every frame while a button is held must scale the action or run under `deltaTick`.
- A one-shot action on a held button (`AnyButtonPressed`, the cutscene skip in `GameStep`) must run under
  `deltaTick`. The held state changes in the first update of the window, before the frame's discrete logic.
- An exception: a decision on a held button at the start of an action's per-frame code, which the N64 makes before
  the action moves the glove, is taken in the first update of the window (`DeltaFirst`). In the tick it would come
  after the window's earlier updates had already run the action. The Z cancels of `HAND_PREWHACK` and `HAND_THROWAIM`
  and the ball walk's Z dismount (`MaintainAction`) are such decisions. The two cancels only record the decision in
  the first update and apply it in the tick, because the ball then gets its thrust and gravity from the next frame
  on, as on the N64. The throw aim's damping by 0.5 also moves to the tick for a cancel: the tick takes the whole
  frame's damping, and an update before it takes `DeltaDampD(0.5)` as usual. The dismount acts in the first update.
  `ProcessInput` goes on pushing the ball for the rest of the window (`ballWalkDismountFrame`): on the N64 the
  dismount comes after the frame's push, so the ball keeps the whole frame's push and its speed.
- The L and R decisions of `HAND_JOINED` (the change to the ball walk, and the ball transformation that R starts) are
  taken in the first update of the window, before the ball and the glove move. The N64 takes them at the start of the
  frame too. The rest of the case runs in ticks. The R press that transforms the ball again in `HAND_SPIN` is taken in
  the first update as well.
- Other held-button decisions were checked for the same pattern and need no change: the pause press and the
  C-button camera-mode exit in `controller.c`.
- `HAND_PUSH`: `ProcessInput` adds no thrust to the glove in updates that are not ticks. `MaintainAction` sets the
  speed into the wall once per original frame and discards the frame's thrust. Thrust added between ticks would raise
  the speed into the wall, and a slanted wall turns that speed into lift.
- A value that the draw path of a tick uses for a discrete step, such as an animation speed, must be set in ticks
  when it depends on the input. The tick's `UpdateAnimations` then steps with the value of the previous frame's
  logic, as on the N64. Set in every update, it would already have the new frame's value.
- Recorded demos run at `deltaTime` 1, so `ReadControllerFromData` needs no change. Recording (`autoPlayback` 1)
  at a high frame rate stores tick-aligned keys, but the physics between them differs.

#### How fast the glove can step on and off the ball

Pressing L changes the glove between `HAND_JOINED` and `HAND_BALLWALK` at once. In both cases `MaintainAction` takes
the press of L from `pad[].pressed[]` and calls `ApplyAction`. It tests neither `actionLock` nor `actionDelay`: the
lock (15 frames after stepping on, 4 after the join) only holds back the glove's other action changes
(`ConditionsMet`). A press is not lost during a lock and not queued: it acts in the frame it is read. What limits
the pace is the controller read. `ControllerStep` reads once per game frame (`ReadController`), and a press is the
frame where the button is held and was not held at the previous read. L must therefore be up for one read between
two presses: the fastest sequence is down, up, down on consecutive frames, one press every 2 game frames (10 a second
in a level at 20 frames a second). Held L gives one press.

Checked in the real game: the ROM ran in mupen64plus under gloveworld's `tools/emu` scripts, level 9 (the practice
course, the glove joined to the ball), the button set at each change of `frameCount` so that one value held for a
whole game frame, and `hand.state->mode` read each frame. L pressed for 1 frame and released for k-1 frames, 12
presses each, k = 2, 3, 4, 6 and 8: every press changed the mode (12 of 12, 3 to 20 and back), and the game saw
`pressed[L]` on every press. The port at `deltaTime` 1 gives the same (`tools/tapcheck.py`): 12 of 12 for k from 2 to 8.
`tools/tapcheck.py` also gives 12 of 12 at `--dt 0.333 --pace`, `--dt 0.1`, `--dt 0.25` and `--dt 0.333 --pace
--dt-jitter 0.1`. Original physics runs at `deltaTime` 1.

#### Taps shorter than a drawn frame

The window loop polls SDL events and then reads the keyboard and controller as a state (`read_input`) once per drawn
frame. The state shows a key or button that is down at that moment. Without more care, a tap that goes down and up
between two drawn frames (a key press shorter than a drawn frame, such as a 10 ms tap at 60 frames a second) is
lost, and in the Original physics mode, where updates come every 50 ms, any tap that ends before the next update is
lost. Two taps in one game frame, or a release and a press in one game frame, give one read as on the N64 (the N64
reads once per frame and sees a button held or not). The port handles them as follows:

- `input_event_buttons` (`inputevent.c`) turns each press event (key down without repeat, controller or joystick
  button down, an axis past the 16000 threshold, a hat direction coming on) into N64 buttons through the current
  bindings. The window loop collects them in `tapPending`. `run_update` adds them to `port_buttons` for the next
  update only, so the recording holds them like any other held button and a replay is exact.
- `port_input_update` and `port_input_read` (`inputlatch.c`) keep the latch: a button that goes down after a read
  counts as held at the next read, even if it is up again. New: a button that the last read saw held, that was seen up
  and is held again at this read, shows up at this read and held at the next one. The read cannot show both. At one
  update per read (`deltaTime` 1) this never happens, so the logic at the original timing is unchanged.
- Without the second rule, a window run with frame jitter around a limit of 20 frames a second lost about a third of
  the presses spaced 2 frames apart (the drawn frame between two reads showed the release and no read saw it).

Test: `tools/inputtest/build.sh` (no ROM, no window) checks the event mapping and the latch with a model of the loop.
`WEEMITT_TAPS=START,SPACING,COUNT` makes a window build push a key down and a key up event for L at once, COUNT times,
every SPACING original frames from frame START. On level 35 with `--trace-updates`, 12 taps at spacing 2 and 3 changed
the action 12 times in both physics modes at 60 and 144 frames a second. With the event latch switched off, none did.

### Random numbers

`Random` shares one seed (`rseed`) with every caller. Call it only in discrete code (under `deltaTick`) or from a
one-shot event. Every spawn function that uses it (`AddDebris`, `AddStarBurst`, `GiveActorRandomQRotVel`,
`InitSpriteAnimation` for type 4, `KillHand`, `KillEnemy`, ...) must be called the same way. The draw path's calls
are safe outside ticks: the frame driver restores the seed. Within a frame window the draw path then makes the same
random choices in every non-tick update, and new ones in the tick.

Event-driven calls can happen in a non-tick update, so at a high frame rate the order of random calls can differ
from the N64's. That run diverges in detail from a run at `deltaTime` 1. The rate of random calls stays the same.

### Sound

`PlaySample`, `PlaySampleRadius`, `PlaySampleNot3D`, `PlayContinuousSample`, `StopContinuousSample`, `PrepareSong`
and `MusHandleStop` are triggers: call them from discrete code or from a one-shot event.
`UpdateContinuousSample` and the setters only store values and may run in every update. Integer volume and pitch
fades of `ContinuousSample` are discrete.

### Text

`ShowText` (in the draw path) marks a one-shot text (`oneShot` 1) as shown, and `ClearText` at the start of the
next `GameStep` frees it. A one-shot text therefore lives one update. Code that creates one-shot texts must keep
running in every update, even where the logic around it becomes discrete: split the creation out of the tick gate.
`ClearText` and `ShowText` stay unchanged. Persistent texts (`oneShot` 0) are created once, in discrete code.

### The draw path

`GraphicsStep` and everything it calls run in every update. Every change it makes to game state happens only in
ticks: animation time (`DrawActor`), `UpdateFade` (gated where `GraphicsStep` calls it), `DeformTextures` (the
whole function), the shadow fades (`ProcessShadows`), the footprint fades and lifetimes (`DrawFootprints`), the
lightning timers (`UpdateLightning`), the blur fades and counts, the sprite animation steps (`AnimateSprite`), the
overlay flipbooks (`PrintOverlays`) and the HUD pops (`PrintStats`). Drawing itself happens in every update.

Values that change only in ticks are drawn with `DeltaTickLerp` where a stepped look would show. The screen fade
is one: `PrintFadeOverlay` draws the fade overlay's alpha and the star depth blended from their values at the
latest tick's draw. A continuous draw-path motion (`starOverlay.rot.z += 0.12`) scales with `deltaTime`.

### The camera

`MoveCam`, `PlayerSpinCamera` and `DoCameraCollision` run in every update. Their accelerations scale with
`deltaTime`, their velocity damping uses `DeltaDamp`, their approaches `DeltaLerp` or `DeltaDiv`, and their timers
(`overrideTimer`, `turnTimer`, `fallTime`, `followTimer`) and the zoom button are discrete.

`MoveCam` ends by resetting the camera requests that other code sets every frame: `targetHeight`,
`targetDistance`, `angleLock` (into `prevAngleLock`), `orbiting`, `destActive` and `slopeAdjust`. The code that sets
them (camera zones in `CheckPuzzles`, `CamCirclePoint`, the bosses, the bonus levels) becomes discrete. `MoveCam`
saves the requests into `tickCameraRequests` at its start when `deltaTick` is 1. `ResetCamera` and the whole frames of
`RestartPlayer` renew the copy too. In other updates `MainGameLoop` saves the live requests, loads the tick's copy,
runs `MoveCam` and `PlayerSpinCamera`, and puts the live requests back. The camera then follows the tick's requests
through the window, and the original reset still happens in every update.

Other parts of `MoveCam` that stay in every update: the clamp of `camera.vel.y` to 0 in the fall branch and the
convergence test that sets `turnTimer` to 0 (only `fallTime++`, the fall targets and `turnTimer--` are in ticks), and
the C-down zoom, which relies on presses existing only in ticks. The destination approach uses `DeltaLerp(0.25f)` for x
and z, `DeltaLerp(0.5f)` for y, and a cap of `20 * deltaTime`. The side-offset spring damps the push with the speed
(see "Writing exact rewrites").

Code that writes `cameraUpVect` (`CheckCameraForWater`, `DoEarthquake`) runs in every update. A roll set only in
ticks would be undone by the lerp in the other updates.

### Platforms' collision faces

`Platform.lastUpdate` caches a platform's collision faces for one value of `frameCount` (`collision.c`,
`footprint.c`, `platform.c`). `frameCount` now holds one value through a window while platforms move in every
update. In an update that is not a whole frame, the platform code marks the faces of each platform it moves as old
(`lastUpdate = frameCount - 1`), and the next collision query rebuilds them. The cache key and the readers stay
unchanged.

### Set-up code and one-shot blocks

- The update after a change of mode or level is a tick (see "The update"). A screen's set-up keyed on a counter
  that steps in ticks (`levelSelectCount == 0`, `endOfLevelCount == 0`) and gated on `deltaTick` therefore runs in
  the screen's first or second update. Code of the screen that depends on the set-up (`EndOfWorldScreen` reads
  `foregroundObjects[2]`) must not run before it: gate it the same way, or run the set-up with `DeltaFirst`.
- A non-idempotent block keyed on `frameCount == K` or on a counter's start value, in code that runs every update,
  uses `DeltaFirst` or `deltaTick`. Use `DeltaFirst` when continuous code in the same frame depends on the block (it
  then runs in the first update of the frame). Use `deltaTick` otherwise.
- A sequence counter that starts at a set-up step may take its first step immediately:
  `step = deltaTick || counter == start`. The sequence then runs up to one frame early.

### Whole frames inside one update

Some code simulates several original frames inside one update: the camera warm-up loops in `RestartPlayer`
(`platform.c`), the head-start loops of the level loader (`parse.c`: `UpdatePlatform` and `UpdateVent` N times) and
`RestartEnemy`'s call of `NMEProcessFroggy` (`enemy.c`). Each simulated frame runs between `DeltaEnterFixed` and
`DeltaLeaveFixed`. Enter and leave inside the loop body, once per simulated frame. `DeltaFirst` then sees a new
frame each time.

### Spawns, lifetimes and "die next update"

Spawns run in discrete code. Lifetimes (`lifeTimer`, `life`, `timer`) count in ticks. Collisions set a bullet's
`lifeTimer` to 1 to mean "remove at the start of the next update" (`collision.c`, `bullet.c`); debris has the same
pattern. With the countdown in ticks, a hit in a non-tick update would leave the bullet flying until the tick. The
bullet and debris code therefore removes an object whose `lifeTimer` was set to 1 by someone else in the next
update. A port-side copy of the last counted value per slot tells the two cases apart.

### Enemy scripts

The enemy scripts (`nmeprocess.c`, `enemy.c`) are discrete: `NMEAnalyseConditionalList`, `NMEGetNewInstruction`,
`NMEConditionsMet`, the `delayTimer` countdown and the instruction transitions run in ticks. The movement actions
(`NMEActionMoveTo`, `NMEActionMoveTo2D`, `NMEActionCircle`, `NMEActionCharge`, `NMEActionFollowPlayer`,
`NMEActionFleePlayer`, `NMEActionRandomMove`, `NMEActionFaceTo`) are continuous: they scale their velocity changes
inside, and their callers call them in every update, outside tick gates. Their return values ("arrived") may only
be acted on in ticks. `NMEProcessActorScript` calls the per-type `NMEProcess*` functions in every update. Each one
gates its own discrete parts.

### Menus at 30 Hz

`deltaTime` is measured in frames of the current mode. The menus' constants are per 30 Hz frame and keep their
values. The credits switch to 20 Hz (`CreditsInit2`) and back; the frame driver converts the time left over at each
change.

## Integer motion state

Many visible quantities are integers in the game's structures: alphas, scales of sprites and overlays, the screen
fade, volumes, texture coordinates. A fractional step on an integer is lost, and the original's truncation is part
of its timing. The treatment:

1. **Discrete by default.** An integer that is stepped per frame (`x += c`, `x -= (x - t) / n`) is stepped in ticks.
   This covers `ContinuousSample.volume` and `pitch`; `Footprint` alpha, age, life and width; `ActorShadow` and
   `Garib` shadow alphas; `Blur` alphas and fades; `Lightning.timer`; `GameLight.timer`; vent and buzzer timers;
   `Debris` and sprite lifetimes; `ScreenDebris` timers and alpha steps; `Stat.scale` and `Stat.alpha`;
   `Overlay.frame`; `SpriteAnimState`; mesh texture coordinates; `Actor.alpha` fades; `gameInfo.fade`, `fade2`,
   `starZ`; `musVol`, `sfxVol`; `fieldOfView`; pause, tip and text alphas; `Weather` and `Earthquake` counters; and
   every frame counter (`NMEBrain` timers, `hitFadeTimer`, `squashTimer`, `teleportTimer`, `Collectable.timer`).
2. **Smooth drawing where the steps would show.** The draw code shows `DeltaTickLerp(prev, cur)`, where `prev` is a
   port-side copy of the value before the latest tick's step. The logic keeps reading the integer. The screen fade
   uses this; HUD pops may.
3. **Continuous phase for counters that drive a visual formula.** `squashTimer` (the squash wobble), `Tip.timer`,
   `Collectable.timer` (shrink and grow), `teleportTimer` (teleport scale), `NMEBrain.instrTime` (the Bugle's squash)
   and `bobCounter` step in ticks; the formula adds `DeltaLag()` or `deltaPhase`.
4. **Outputs.** An integer recomputed every update from float state (`GameSprite` scales from `Garib.scale`, text
   positions from float positions, alphas from a sine, backdrop positions from the camera) is computed in every
   update and needs nothing.
5. **Float shadows** are allowed only for an integer that feeds continuous motion and would stall otherwise. A
   shadow is a port-side float. Before each use, it takes the field's value again when `(type)shadow != field`
   (another file wrote the field). The code integrates the shadow and stores the truncated value. Its result at
   `deltaTime` 1 must equal the original's for every value reachable in a mixed run (see the score box in
   `EndOfLevelScreen`: use the float form at every `deltaTime`).

## Decided sites

These sites needed a design decision. The decisions:

| Site | Decision |
| --- | --- |
| hand.c `MaintainAction`, the glove's landing (`onGround == 1 && !prevOnGround`) | a physics edge: runs in every update. `handControl.prevOnGround` keeps being written in every update. `groundStateTime` (`MoveHand`) counts in ticks and compares with a port-side copy of the ground state at the latest tick, cleared in `ResetHand(1)` with the `memclear` |
| hand.c `MaintainAction`, the ball's landing (`!ballControl.prevGround[0]`) | the same: runs in every update; `prevGround[0]` is written in every update |
| enemy.c `RestartEnemy`, `NMEProcessFroggy(enemy)` | the call runs between `DeltaEnterFixed` and `DeltaLeaveFixed`: the frog turns back at once |
| platform.c `UpdatePlatform`, `CalculatePlatformInfo`, `RecalculatePlatformInfo` (`lastUpdate`); collision.c `CheckCollisions`, `Altitude`; footprint.c `DrawFootprint` | keep the `frameCount` key. In an update that is not a whole frame, the platform code sets `lastUpdate = frameCount - 1` on each platform it moves. collision.c and footprint.c do not change |
| platform.c `OrbitPlatform`, the `frameCount == 0` start angle | no change: frame 0 lasts at most two updates, and a second run moves the reference angle by less than one frame of orbit |
| platform.c `RestartPlayer`, the camera warm-up loops (30 and 20 frames) | each iteration runs between `DeltaEnterFixed` and `DeltaLeaveFixed` |
| ball.c `MoveBall`, `BreakOffSnow` on a hit or a hard landing | evaluate the condition in every update and OR it into a port-side pending flag; break the snow in the next tick and clear the flag. Clear it also at level start and when the snowball ends |
| collision.c `CheckActorCollisions`, damage and hurt effects | apply the effects once per pair of spheres per original frame: a port-side table of the pairs hit, keyed by `deltaFrame`. The physics of the contact runs in every update |
| collision.c `CollideActorWithPlane`, the water ripple (`terrain == 12`, `dot < -0.5`) | discrete (`deltaTick`). When the actor touched the same plane in the previous update (resting), compare `dot / deltaTime` |
| camera.c `MoveCam`, the request reset; main.c `MainGameLoop`, `MoveCam(); PlayerSpinCamera();` | the camera request snapshot (see "The camera") |
| object.c `HandSwish` | `time += speed * deltaTime`; hand.c passes `AnimDrawTime` of the pole's animation and runs the swish pose in every update |
| boss.c `DoFortressBoss`, the slam on the platform | evaluated in every update with a port-side latch: fire once when the glove is in `HAND_SLAM2ST` with `actionTime == 1` and lands; re-arm when that state ends |
| frontend.c `spinInFGObjects`, the bounce (`spinInCount < 1.0f`) | the test runs only in ticks (`deltaTick && spinInCount[i] < 1.0f`), once per original frame on the position at the end of the frame. Between ticks a box can rest at 0 |
| puzzle.c `PuzzleExecuteAction`, the camera-zone actions | no replay: the camera request snapshot covers them |
| puzzle.c `PuzzleEvaluateCondition`, `HAND_JUST_ON`, `BALL_JUST_ON`, `NME_JUST_ON`, the three `JUST_TOUCHED` and `JUST_CONFINED` | at the end of each tick's `CheckPuzzles`, store a port-side snapshot of `obj->platform` and `obj->contactPlatform` (glove, ball, each enemy) and of the confined platforms' positions. In an update that is a whole frame, compare with the original `prev` fields; otherwise compare with the snapshot |
| light.c `DrawLightning`, the bolt jitter | no change: the frame driver restores the random seed after a non-tick draw |
| controller.c `ProcessInput`, the stick flip on a ceiling | flip once per frame window, the first time `ProcessInput` runs with the glove on a ceiling (`DeltaFirst`). The flipped stick then holds until the next read |
| bonus.c `DoAtlantisBonus`, the log velocities set every update | `MovePlatform` damps a platform's velocity by `DeltaDamp(f)` and then moves it by `vel * deltaTime`. The log must move c per frame, so its velocity becomes `c / DeltaDamp(f)` (`1.0f / DeltaDamp(atbonusval1)`, `-2.0f / DeltaDamp(atbonusval2)`): at `deltaTime` 1 that is the original `c / f` |
| parse.c `ParseTokenBridge` (two loops), `ParseTokenPlatformGeneric` (`TOKEN_HEADSTART`) | each iteration runs between `DeltaEnterFixed` and `DeltaLeaveFixed` |
| water.c `WaterDoFloating`, the drag | once per frame window, in the first update (see "Water") |
| platform.c `RestartPlayer`, `restartTimer` steps | the steps at 65, 35 and 10 and the last step (1 to 0) run in the first update of the window (`DeltaFirst`, one static stamp per step). The other steps run in ticks |
| platform.c `MovePlatform`, the path logic | runs once per original frame, in the first update of the window: the waits, the arrival test with the N64's one-frame tolerance, and a whole frame of path acceleration. When the frame began inside the previous update, the logic works from the platform's position at the start of the frame (the carry). An arrival inside a window moves the platform onto the point through the window (a table of 64 `PathArrival` records, cleared in `InitPlatformList` and `KillPlatform`). `idleCount` stays per update |
| platform.c `RotatePlatform` | zeroes a turn below 0.0005 only in ticks. `ScalePlatform` also counts an axis as arrived when its scale passes the end of its range. `SpinPlatform`'s tilt return uses `DeltaLerp(accel / rotLimit)` when `deltaTime` is not 1. `ConfinePlatform`'s ground bounce runs in ticks |
| boss.c `DoPrehistBoss` | the ball's hit is held until the tick (see "Events") |
| hand.c `HAND_CATCH` | the catch test runs in ticks, on the positions without the current frame's pull (`CatchInReach`, `catchPullBall`, `catchPullHand`, cleared by `DeltaFirst`). Once the ball is within reach the live positions stop the pull. The ball and the glove each move `DeltaLerp(2 * scale) / 2` of the gap |
| hand.c `HAND_ROTOR` | `rotorSpeed` decays by `DeltaDampD(0.95)`. The lift has no `DeltaAccelPos` (see "Known limits") |
| hand.c `MaintainAction` | the glove's gravity flags (bits 1, 0x40 and 0x200 of `hand.flags`) are set only in ticks. `HAND_PUSH` copies its push into `hand.vel` only in ticks, and `rotAim.y` follows it in every update |
| nmeprocess2.c `TriggerAtFrame` callers | no local tick gate: the function returns FALSE outside ticks |
| nmeprocess.c `NMEProcessInstruction` | ignores `needNewInstruction` outside ticks. The re-entry for a new instruction (`numInstThisFrame < 4`) happens only in ticks |

Other decisions:

- The continuous frame counter is derived, never accumulated: `FrameTime()` and the helpers above, not a
  `frameCountF` global. The names `tickFraction`, `deltaFrac`, `tickFrac` and `DeltaTickFrac` all mean
  `deltaPhase`. `deltaFrameStart` is `deltaNewFrame`.
- Animation time steps in ticks and the draw interpolates (`AnimDrawTime`). Proposals to advance it by
  `speed * deltaTime`, to widen `TriggerAtFrame`'s window by a tick span, or to add `AnimTickTime` are not used.
- `UpdateFade` steps in ticks; no float shadow of `gameInfo.fade`. `ScreenFadedOut` keeps the N64's timing.
- `PrintStats` needs no cache of its random pickup bubbles: the seed restore covers them.
- `ClearText` and `ShowText` stay unchanged; text producers run in every update.
- The enemies' movement actions are continuous and called every update (not the tick-only variant).
- `platformClock` and `updateSerial` are not used; see "Platforms' collision faces".
- Velocities use linear damping (`DeltaDamp`) and plain `deltaTime` for thrust; see "The actor integrator".
  `DeltaGeom`, `DeltaAccelDamped`, `DeltaGeomSum` and a per-actor acceleration scale are not used.
- `DeltaShareF`, `DeltaPowRest`, `DeltaAccelSemi`, `DeltaAccelExplicit`, `DeltaIntApproach`, `IntShadow`,
  `SmoothCount`, `VisualRandom` and `TickLag` are not shared helpers. The forms above cover them, or the owning file
  writes a static function.

## Verification

- `make headless -j4` builds the test binary. The Makefile does not track headers: after a header change, touch the
  `.c` files that include it or remove `build/headless`.
- `python3 tools/regress.py check` must report 0 runs differing after every change. It compares every frame's state
  and a hash of the game's memory with the baseline, at `deltaTime` 1. It covers the attract mode and every level,
  level 40 included.
- `python3 tools/dtcheck.py LEVEL...` compares a level at `deltaTime` 1 and 0.25 with the same input. It shows
  whether `frameCount` and the speeds of the glove, the ball and the camera agree. A run at a short `deltaTime` can
  take a different path after a small legitimate difference. Judge a level by the first frame where the runs depart
  and why, not by the speed ratios alone. Running the level with several input seeds helps: a frame-rate error shows
  with every seed, a chaotic split does not.
- The memory hash reads the words in `[PORT_PTR_BASE, +24 MB)` as offsets into `port_ram`: the pointers that the
  game stores (Ptr32 words, see [port64.md](port64.md)). On the normal builds `PORT_PTR_BASE` is the address of
  `port_ram`, so the hash needs `port_ram` at the same address in the build and in the baseline. On Linux
  `gen/ram.S` puts `port_ram` at a 32 MB boundary. Its address then stays the same while the code before it grows
  within the same 32 MB. `regress.py` gives the baseline's sources the same alignment before it builds them. The
  displaced test build stores the same words with `port_ram` elsewhere, and its hashes equal the baseline's too.

## Known limits

These differences from the N64 at a short `deltaTime` are known and accepted.

- A mode switch that the tick decides starts its per-update approach late. For example `MoveHandToBall` closes a share
  of the gap in each update, so after the switch the glove lags by up to 3/4 of a frame at the end of a frame.
- When `deltaTime` does not divide 1 (0.7, 0.45), a tick decision reads positions up to one update past the frame's
  end, so a menu can close one frame early or late.
- `HAND_ROTOR`: the lift of `hand.vel.y` has no `DeltaAccelPos`. A correction was tested and gave mixed results. A glove
  on the ground cannot take off with the rotor at `deltaTime` below 1, because the ground hold of `vel.y` takes each
  update's share.
- Gravity in contact with a wall has no correction. A correction was tested and gave mixed results.
- A continuous push into a surface by other code counts as an old overlap (see "Collisions").
- An actor that enters water in the middle of a window gets its water state in the next window (see "Water").
- Random calls from events can come in a different order than on the N64 (see "Random numbers"), so a run at a short
  `deltaTime` diverges in detail from a run at 1.
