# The host world

A host program that loads the library ([library.md](library.md)) can put the glove and the ball into its own world.
It gives the game triangles to collide with, moving bodies, places for the glove and the ball, and a camera. The
host can be another game's engine. The code is
`src/port/hostworld.c` (the game side), `src/lib/coll.c` and `src/lib/view.c` (the `glover_` calls), and
`src/port/hostscene.c` (the test scenes).

Nothing of this runs until a host asks for it. The game reaches it through function pointers
(`src/port/porthooks.h`), which every other program leaves NULL. The game then runs exactly as before.

## Units

The library does not convert units. The host scales its world into game units, and uses the same scale for the
meshes, the poses, the places and the views. The scale is a setting of the host, not of the library. The ball's
radius is 9 game units. A host whose world is much larger picks a scale below 1. A host can offer it as
one of its own settings.

## The calls

All of them are in `glover.h`. Version 2 of the interface brought them; version 3 adds the conveyors and the waters.
They run between updates.

| Call | What it does |
| --- | --- |
| `glover_coll_set_static` | replaces the static world with a copy of the host's triangles; NULL removes it |
| `glover_coll_body_add`, `_move`, `_remove` | a moving body: a mesh in its own space and its pose |
| `glover_coll_body_conveyor` | a conveyor on a body, in the body's own space (version 3) |
| `glover_coll_set_water` | replaces the host's water volumes (version 3) |
| `glover_coll_clear` | removes all host collision and gives the level its own collision back |
| `glover_coll_stats` | triangles, chunks, bodies, heap used and free, rebuilds, conveyors, waters |
| `glover_place` | puts the glove or the ball somewhere, with velocities, the glove's yaw and the camera |
| `glover_set_respawn` | where a restart puts the glove |
| `glover_set_view` | a camera that draws (`GLOVER_VIEW_DRAW`) or that steers the controls (`GLOVER_VIEW_STEER`) |
| `glover_bodies` | the glove and the ball read back: positions, velocities, action, contact, terrain, readiness, voids, safe places and water states |

A triangle's front is the side from which its corners run counter-clockwise. Each triangle has a terrain number,
0 to 13, from the game's terrain table (below). A mesh or triangle with `GLOVER_MESH_CAMERA_PASS` lets the game's
camera through. One with `GLOVER_MESH_VOID` is a void (below).

### Errors

- `GLOVER_ERR_ARG`: a terrain above 13, a mesh or triangle flag other than the two, a `levelOff` bit other than the
  three, an index out of range, a conveyor index above `conveyorCount`, more than 4,096 conveyors, a conveyor velocity
  that is not finite or above 100 in absolute value, a body's mesh with conveyors, more than 256 waters, a water's
  `elemSize` under 40, a water size of 0 or less, a surface more than 0.01 outside the water's box, a current above 100, a coordinate that is not finite or whose absolute value is 100,000 or more, a scale of
  0, a quaternion whose length is not 1 within about 0.001, too many triangles, a mesh with no triangle left after
  the copy (step 1 below), a body with an edge under 0.025 at its scale, a view whose up vector is parallel to its
  direction. A call that fails changes nothing: a failed `glover_coll_set_static` keeps the static world that was
  in. A `GloverCollOptions` from a host built before `levelOff` was added ends before that field: the library takes
  its shorter size and switches nothing off.
- `GLOVER_ERR_MEMORY`: the game's heap would keep less than 2 MB free, or the heap's block table less than 64
  entries.
- `GLOVER_ERR_BUSY`: the game is not in a level, or the glove is not ready. The library keeps the request and makes
  it at the first ready update. A body still gets its id. `GloverCollStats.pending` is 1 while a request waits, and
  `failed` counts requests that then found too little memory.

### Readiness

The glove is ready when the game is played, the glove's action is set up, and no restart runs: the checkpoint's
restart timer is 0, or there is no checkpoint. `GloverBodies.ready` gives it. At the start of a level the game
runs a restart that moves the glove to the level's checkpoint. Anything built or placed before it ends would be
undone, so the library waits.

## How the static world is built

1. **Copy.** The library copies the host's mesh into its own memory, outside the game's memory. Vertices with
   exactly the same coordinates become one. Triangles of no area, and triangles with an edge shorter than 0.025,
   are left out. The game's inner-edge test (`CheckForInnerEdges`) reads outside its table `conv[]` for an edge
   under 0.02: both ends of the edge then lie within 0.01 of one corner of the other face.
2. **Chunks.** Each triangle goes to the cell of the ground plane (x and z) that holds its centroid. The cell's
   edge is `cellSize`, 256 by default. A cell with more than `maxChunkTris` triangles (1024 by default, at most
   32,767) is cut in two at the median of its longest axis, again and again. Cells come in the order of their z
   and then x index, and a cell's triangles keep the host's order. Triangles that the camera passes through form
   chunks of their own. The order depends on nothing but the mesh.
3. **The game's structures.** Each chunk becomes a `LANDSCAPE_ACTOR` pseudo-actor with an `ObjectInstance`, an
   `Object`, a `Mesh`, its vertices, its face indices and its `CollFace` planes. All chunks share one block from
   the game's heap (`JallocAlloc`): every pointer the game follows must lie in `port_ram`, and the heap's block
   table has room for few more blocks. The game's own `InitPlanes` builds the planes, so they are the game's to
   the bit. The vertices keep their coordinates: the actor's position, the chunk's centre, serves only the bounding
   sphere. The planes are then the same however the triangles are cut.
4. **Seams.** `InitPlanes` marks the inner edges within a chunk: an edge that two coplanar faces share, which the
   ball must not catch on. A second pass applies the game's own test (`CheckForInnerEdges`) to the faces of
   different chunks that share an edge, with the earlier chunk's face first. A single chunk would test every pair
   of faces; the seam pass tests only faces that share both vertices exactly. Vertices that are within 0.01 but not
   equal are therefore not joined across chunks.
5. **Linking.** The chunk actors go at the head of `collisionList`, in chunk order. Unless `keepLevel` is 1, the
   level's own `LANDSCAPE_ACTOR` and `WATER_ACTOR` entries leave `collisionList` (they stay in `actorList`), the
   level's water boxes get a width of -1, which holds no point, and the level's platforms get the game's
   pass-through flag 0x80. Without this the water would float the ball, splash and fog the camera where the level
   has water. The platforms stay in `platformList` and the waters in `waterList`: boss and bonus levels find them
   there by their tags. `glover_coll_clear` gives all of it back.
6. **The level's own things.** `GloverCollOptions.levelOff` switches off the level's enemies
   (`GLOVER_OFF_ENEMIES`), its garibs, extra lives and potions (`GLOVER_OFF_COLLECTABLES`) and its ambient sounds at
   points (`GLOVER_OFF_AMBIENT`) while the static world is in (below).

### The level's enemies, collectables and ambient sounds

The host world plays in a Glover level, and that level's enemies, garibs and potions stay where they were, in
Glover's space, unless the host switches them off with `GloverCollOptions.levelOff`. The library then takes them
out before each update, and again right after the game's restart, which brings the enemies back
(`RestartEnemy`).

- **An enemy** leaves `actorList` and `collisionList` and gets `active` 0, as when `KillEnemy` has run, but without
  its score, its garib, its sounds and its effects. It lets go of the ball and the glove first. The enemies stay in
  the game's `enemies[]`, where the boss levels, the puzzles and the bullets look for them.
- **A garib or an extra life** goes as when it is collected, without its score: `active` 0, out of the sprite list.
  A puzzle that waits for a group of garibs to be collected then acts.
- **A potion** leaves `actorList` and stays in `collectableList`, where the puzzles look for it. It does not come
  back.
- **An ambient sound at a point**, such as a waterfall's, stops and loses its place, as `ClearAmbientSfx` leaves
  the level's sounds: `UpdateAmbientSounds` then skips it. It stays in `ambientSoundList`, where the puzzles look for
  it. The level's sounds without a place, which play everywhere, stay. Level 12 has five ambient sounds at points.
  With the camera's distances at a third (`GLOVER_OPT_CAMERA_DISTANCE_SCALE`), they are heard over a host's world
  from three times as far. A puzzle can add an ambient sound at a point during an update (`ACTION_START_SOUND`): it
  is switched off as it is made. A puzzle's sound that plays once at a point does not play. A sound switched off no
  longer runs its timers, which draw random numbers. The level's random sequence then changes, and with it the
  logic that uses random numbers.

The rest of the level runs: its exit, its puzzles, its platforms and its water (pass-through and empty unless
`keepLevel` is 1), and its other hazards. A boss is an enemy too: on a boss level the boss's logic then waits. What
the option took out stays out when the static world goes, until the next restart or level start. The headless
mode's last line gives the enemies and the collectables that are still in the game. `off.scene` tests the option,
and `hwtest.py` runs it in every level, 0 to 42, under the pointer check.

`CheckCollisions` and `Altitude` skip an actor whose bounding sphere is far away, so a far chunk costs one distance
test. With `linkRadius` above 0, only the chunks within that distance of the glove, the ball or the camera are
linked, again in chunk order, before each update. This is for meshes with very many triangles.

Without the level's landscape and water, a bullet that would have hit them can fall past the host's world. Level 8
makes drops that live 1000 original frames, one a frame, and they would fill the heap's block table. While the
level's own collision is out, a bullet 500 units below the lowest point of the static world ends in the next
update, as when it hits something (its `lifeTimer` becomes 1).

The heap holds about 140 bytes per triangle. The game's heap has about 21 MB free in a level.

### Freeing

Before the library frees a static world or a body, it clears every pointer of the game's state into it: the
contact planes of every actor, the glove, the ball and the camera, the shadows' floors, the glove's cling planes,
and the normals of the garibs, tips and footprints. A footprint keeps a copy of its normal.

A footprint on a body is left to the game's `KillPlatform`, which moves the glove's footprint into world space and
removes the others.

### Level reloads and other levels

A level start (`FreeLevel2`) wipes the game's heap and its lists. The library then marks its world as lost. It also
counts each `JallocInit` (`portHeapInits`, outside the game's memory), which catches the other places that set the
heap up again, such as the title. At the first ready update the library builds the world again from its copy, puts
the bodies back at their latest poses, and counts `rebuilds`.

- **The glove and the ball** start at the level's own start again, as the reload puts them. A host places them
  again with `glover_place` when `GloverCollStats.rebuilds` rises.
- **The respawn** goes into the new level's checkpoint again. It stays until `glover_set_respawn` gives another or
  `glover_coll_clear` removes it.
- **Another level.** The host world stays when the game goes to another level, and is built there too. Call
  `glover_coll_clear` before `glover_goto_level` to a level that should have its own collision. Some levels have no
  ball at their start (level 5, for one): the ball then waits at a height of -5000.

### Falling out of the world

The library does not watch the glove and the ball. One that falls past the host's mesh falls for about 110 frames,
to a height of about -1800, before the game's restart begins. A host that wants a quicker return watches
`glover_bodies` and calls `glover_place`. A restart puts the glove at the respawn, with the ball about 100 units
away and 100 above.

### A held ball

While the glove holds the ball, or walks on it (`HAND_BALLWALK`), a `glover_place` of the glove alone, or of the
ball alone, moves both by the same distance, with the same velocity, and the glove keeps the ball. A place of both
makes the glove let go first, as when it turns into a frog, and each goes to its place. The game's hold would
otherwise pull the two back together from wherever they were put. Places close together let the game pick the ball
up again at once: on a flat floor, 18 units apart it did, 30 apart it did not.

### Voids

A void is a triangle with `GLOVER_MESH_VOID`, such as the floor of a pit where the host would kill or respawn its
own player. It collides as any other triangle, with its terrain. The library does not kill
the glove or the ball on it, and it does not move them: the host places them, usually at the last safe place, with
`glover_place`.

- **What the host reads.** `GloverBodies.handVoid` and `ballVoid` are 1 while the floor under the glove or the ball
  is a void triangle, and 2 while it touches one. `handVoids` and `ballVoids` count the times each began to touch a
  void triangle. A ball that bounces on the void counts each bounce. While the glove holds the ball, or stands on
  it, the two fall together: the glove then reports the ball's void as its own when the ball's is the larger, and
  counts its touches too.
- **The safe places.** `handSafe` and `ballSafe` are where the glove and the ball last stood on a triangle of the
  static world that is not a void and has neither terrain 6 (death) nor 9 (it hurts the glove). Bit 0 of
  `safeKnown` says that `handSafe` holds a place, bit 1 `ballSafe`. A new static world, and `glover_coll_clear`,
  forget them.
- **No death from a fall.** The game hurts the glove when it lands after a long fall, and kills it when it falls 17
  original frames with no floor under it. It bursts the ball when it lands after 19 frames of falling, or after 41
  frames in the air. While the glove or the ball is over a void or touches one, the library holds the game's fall
  counters (`handControl.fallTime`, `ballControl.fallTime`) at 0, after the game's restart and before the glove and
  the ball move. None of this happens then, and no life is lost. A crystal ball does not shatter on a void
  triangle, however hard it lands (`port_no_shatter_hook` in `CollideActorWithPlane`).
- **Fast placed speeds.** A void catches what falls onto it at the game's own speeds. A `glover_place` with a
  downward speed far above them, such as 1000 units per frame, can carry the ball below the void in one update.
  It then falls with no void under it, and the game bursts it as after any other long fall.

The library reads the voids after each update, through the hook after `GameStep` (`port_post_update_hook`). A test
of it is `void.scene`: the ball and then the glove fall 1500 units into a pit whose floor is a void, and the host
places them back. `hwtest.py` also runs it with the glove holding the ball, and with the void floor as a body.

## Conveyors

A conveyor moves what stands on it: each original frame, by its velocity, in game units. The game has conveyors on
its platforms only (`TOKEN_CONVEYOR` and `TOKEN_CURRENT` in `parse.c` set the platform's flag 0x200 and its
`conveyorVel`). The library makes the host's conveyors into such platforms.

- **On the static world.** `GloverMesh.conveyor` gives each triangle a conveyor: 0 for none, or k for the velocity
  `conveyors[3 (k - 1)]` to `[3 (k - 1) + 2]`. `conveyorCount` is at most 4,096, and each value is within 100.
- **The chunks.** The triangles of one conveyor form chunks of their own, after all the cells: a conveyor stays in
  one chunk unless it has more than `maxChunkTris` triangles. Each such chunk is a `Platform` in `platformList`, not
  a `LANDSCAPE_ACTOR`, with the game's flag 0x200 and the conveyor's velocity. Its vertices are relative to its centre,
  rounded to whole units, with no rotation. It lies in the static world's block, and the seam pass joins its edges to
  the other chunks' as any chunk's. Without conveyors the chunks and their order are as before.
- **Why one chunk.** The game carries what leaves one conveyor platform for another with the first one's speed, on
  top of the second's carry: a conveyor cut at each cell would push a unit further at each cut.
- **The carry.** The game's `UpdateActorPlatform` moves the glove and the ball by the conveyor's velocity times
  `deltaTime` while their floor is that platform. A floor counts from a slope with a normal's y above 0.3
  (`collision.c`). A ball that lands on a conveyor also gets at least the conveyor's speed as its own velocity
  (`AddConveyorSpeedToVel` in `CollideActorWithPlane`), which then slows by its friction. A rolling ball lands again
  each time it passes over an inner vertex of the conveyor's triangles, where no other floor lies close under it. On
  `conveyor.scene` (50-unit squares) the ball rides at 1.42 units per frame where the glove rides at 1, with a kick
  at every 50 units. The same strip as a body's mesh gives the same 1.42. With a floor 1 unit under the strip, or with
  the strip as one quad, the ball rides at 1.000.
- **On a body.** `glover_coll_body_conveyor(g, id, vel)` gives a body a conveyor, in the body's own space: the game
  turns it with the body's rotation (`AddConveyorSpeed`). NULL or 0, 0, 0 removes it. A body that waits to be built
  gets `GLOVER_ERR_BUSY`, and the conveyor when it is built.
- **Freeing.** Before the library frees the static world, and when a level reload frees it, it clears the game's
  pointers to the conveyor platforms as `KillPlatform` does (the platforms of the actors and the debris, the glove's
  platforms, the footprints), and takes them out of `platformList`. The game's own loop never frees them.
- **What the host reads.** `GloverCollStats.conveyorTris` counts the static triangles on a conveyor, and
  `conveyorPlatforms` the conveyor chunks and the bodies with a conveyor.

## Water

`glover_coll_set_water(g, waters, count, elemSize)` replaces the host's water volumes: at most 256, `elemSize` bytes
apart. NULL or a count of 0 removes them. Each `GloverWater` is a box (its lowest corner and its size), the height of
its surface, and a current.

- **The game's water.** Each becomes one of the game's `Water`s, with the box, the surface as its `height` and
  `targetHeight`, the current, and the tag -1. The game then floats the ball, slows and carries what is under the
  surface, lets the glove swim, splashes, makes wakes and bubbles, ends the bullets that fall in, and fogs the camera
  under the surface, all with its own code (`water.c`).
- **The box.** As for the level's own waters (`TOKEN_WATER` in `parse.c`), the box reaches at least 25 units below
  and above the surface. The box has no bottom of its own: the water lies on the host's triangles. A host whose water
  volume reaches below floors at several heights under one surface cuts it into several waters, each down to the
  first floor under it.
- **Areas.** A host whose world is split into areas, where a water counts only in its own area, passes only the
  waters of the player's area and calls `glover_coll_set_water` again when the area changes.
- **The current.** The game adds the current to the velocity of what is in the water once per original frame, and
  slows it by 0.8 a frame (0.9 for what floats). The glove then drifts at about 4 times the current, and the floating
  ball at about 8 (7.9 on `hostwater.scene`: the ball's own drag takes off a little more).
- **Where they go.** The library builds them in one heap block ("HostWater") at the first ready update, and links
  them at the tail of `waterList` by hand. The level's own waters stay first there, and `FindTaggedWater` finds them by
  their tags as before. They stay full while the level's waters are emptied (above).
- **Level reloads.** `FreeLevel2` frees each water of `waterList` on its own. The library takes its waters out of the
  list before that, at the start of `FreeLevel2`, and builds them again at the first ready update. This counts as a
  rebuild.
- **What the host reads.** `GloverBodies.handWater` and `ballWater` are the game's `waterState` of the glove and the
  ball: 0 out of water, 1 paddling, 2 wading, 3 swimming, 4 floating. `GloverCollStats.waters` counts the host's
  waters.

## Moving bodies

A body is a `Platform` in the game's `platformList`, with its own mesh and faces, built by the game's
`InitPlatformInfo`. A hook right after `UpdatePlatforms` (in a played, unpaused game) puts each body at the pose
the host gave for the end of the update. It sets the previous position to the old pose and the velocity to the
movement divided by `deltaTime`: the game's carry reads the velocity per original frame. The game's own platform
movement earlier in the update is overwritten. The body is then exactly at the host's pose when the glove and the
ball collide.

While the game is paused it moves no platforms. A body that the host moves meanwhile goes to its pose before the next
update, with no velocity: nothing on it is carried. A carry of the whole distance on the first update after the pause
would throw the glove.

A glove or a ball on a body moves with it, up, down and sideways, and turns with it about the vertical axis.

- **The turn.** The game's carry (`UpdateActorPlatform`) turns what stands on a platform by `motion->rot.y -
  motion->oldRot.y`: its offset from the platform's centre, its velocity and the glove's yaw. Each body has a
  `PlatformMotion` in its block for this, with no speed and no limits, which the game's own `SpinPlatform` and
  `RotatePlatform` leave alone.
- **The angles.** The hook sets `oldRot.y` to the turn of the body's previous pose about y (its twist, `2 atan2(y,
  w)` of the quaternion) and `rot.y` to that of the new pose, the shorter way round. `port_atan2` (`portmath.c`)
  uses only additions, multiplications and divisions, so every build gets the same bits.
- **Bodies that only move.** The body gets the motion only while its previous or its new pose is turned (its
  quaternion is not 0, 0, 0, 1). A lift or a slider keeps none, and the game's `RecalculatePlatformInfo` keeps its
  fast path for it: its trace is the same as before the motion was added.
- **Limit.** Only the turn about y is carried. The game's carry turns about one axis, so the tilt of a body about x
  or z does not tilt or move what stands on it.
- **A shadow.** Each body, and each conveyor chunk, also has an `ActorShadow` with no floor in its block, which the
  game never draws. On level 24 (the Pirates bonus) `ProcessShadows` reads the shadow of every platform above the
  water; without one it read through a NULL pointer.
- **Freeing.** The motion and the shadow lie in the body's block. Before the game's `KillPlatform` runs on a body,
  the library sets `motion` and `shadow` to NULL: `KillPlatform` would otherwise free them on their own.

Removing a body calls the game's `KillPlatform`, after the library has cleared the pointers into it.

## The camera

`glover_set_view` takes an eye, a point to look at, an up vector, a vertical field of view in degrees, an aspect
(width / height) and the near and far distances, all in game units.

- **`GLOVER_VIEW_DRAW`**: a hook in `SetupViewing` (`main.c`), after the game builds its matrices, builds them again
  from the view with the port's own `guPerspective` and `guLookAt`. The sprites (`overlay.c`) use the same
  matrices. The game's logic does not change.
- **`GLOVER_VIEW_STEER`**: before each update the game's camera is put at the eye, looking at the point. The game
  then skips `MoveCam` and `PlayerSpinCamera`. The controls, the sound and the game's distance tests follow the
  host's camera: with the stick pushed up the glove walks away from the eye. The game's camera logic does not run
  meanwhile. Its pending changes, such as a camera override that the logic starts and `MoveCam` counts down, wait
  until the view goes.

The game measures what it draws from its own camera. It leaves out the actors and the sprites farther than its far
distance (`farPlaneDist`: `IsActorVisible`, `PrintSprite`), and the shadows farther than 600 units (`shadow.c`) except
the glove's and the ball's. With `GLOVER_VIEW_DRAW` alone, that camera is still the game's. With
`GLOVER_VIEW_STEER` it is at the host's eye.

While a drawing view is on, the far distance is the view's `farZ`, at most 32,767, in place of the level's own (1000
in level 12). A host at a large scale has its camera farther from the glove in game units: at 2 game units per
host unit, the level's 1000 units are 500 of the host's. The hook writes `farPlaneDist` at each drawn frame, and the
logic between two frames sees the view's value too. A level load writes the level's value again, and the next frame
replaces it. When the view stops drawing, the level's value comes back.

Four other rules measure the distance from the camera: the sound fades out at the level's sound distance
(`MAX_SFX_DIST`, 1000 units in level 12), wakes on the water are made within 283 units (`CreateWake`), debris
within 1500, and shadows within 600. The far distance does not change them. A host at a large scale gives them its
own scale with `GLOVER_OPT_CAMERA_DISTANCE_SCALE` ([library.md](library.md#sound)). At 2 game units per host
unit, a scale of 0.5 measures the four in the host's units: the sound then fades out at 1000 host units.

Both apply while a level is played. The menus and the attract mode keep the game's own camera.

The host's aspect is in the projection. Draw with `glover_gl_options` widescreen 0 or 1: the wider view (2) would
widen the picture a second time, so `glover_gl_options` refuses it while a drawing view is set, and
`glover_set_view` refuses a drawing view while it is on.

The test flag `GLOVER_VIEW_TEST_GAME` (`0x80000000`, `HW_VIEW_MIRROR` in `hostworld.h`) draws with the game's own
camera values, taken in the hook. The tests use it to prove that the host's path draws the game's own picture to
the bit. Its matrices are the game's own, so the renderer's wider view (widescreen 2) applies to it as to the
game's.

### What the host gets to draw

`glover_gl_draw` draws the game's whole picture: the host level's own sky, scenery and HUD, with the glove and the
ball. It does not draw the host's triangles. `glover_gl_read_frame` gives the 4:3 picture; `glover_gl_draw`
stretches it to the window with widescreen 1.

To put the glove and the ball over its own picture, the host draws a layer of the actors alone with
`glover_gl_draw_layer` (`GLOVER_LAYER_ACTORS`): their colour with alpha, and the depth of each pixel in the drawing
view's projection. `glover_gl_composite` then draws the layer over the host's framebuffer, hidden where the host's
own depth is nearer. [library.md](library.md#drawing-over-the-hosts-picture) describes the calls, the depth and a
shader for hosts that composite on their own. The level's own scenery is not in that layer, so the host world
replaces it in the picture as it does in the collision.

- **The projection.** The drawing view's projection is built in `SetupViewing` (`port_view_hook`), and again in
  `ClearViewing`: the frame's earlier lists point at the same projection matrix, and the RSP reads it only after the
  frame is built. Before, `ClearViewing` put the game's own field of view, aspect and planes back into it, so the
  3D picture was drawn with them.

`glover_bodies` gives the glove's position as the ball's while the glove holds the ball (`ballHeld`). A ball placed
right above the glove is picked up.

## Terrain

The library takes Glover's terrain numbers only. The values of the game's table (ROM 0xEE74C) are slip, bounce,
slide speed scale, slide slope scale, gravity scale, shatter speed and the glove's slide scale.

| # | Values | What the code does with it |
| --- | --- | --- |
| 0 | 1, 1, 1, 1, 1, 10, 1 | |
| 1 | 0.5, 1, 1, 0.25, 1, 14, 1 | |
| 2 | 6, 1, 6, 36, 1, 10, 3 | very slippery |
| 3 | 0.3, 0.6, 1, 0.09, 1, 17, 1 | |
| 4 | 1, 0.7, 1, 1, 1, 14, 1 | |
| 5 | 0.6, 0.8, 1, 0.36, 1, 12, 1 | water: splashes and wakes |
| 6 | 1, 0.5, 1, 1, 1, 17, 1 | death: bursts the ball, hurts the glove (with actor flag 0x400000) |
| 7 | 1, 1, 1, 1, 0.2, 10, 1 | low gravity |
| 8 | 3, 1, 3, 9, 1, 10, 1.5 | slippery |
| 9 | 0.5, 0.7, 1, 1, 1, 15, 1 | hurts the glove |
| 10 | 0.9, 0.9, 0.9, 0.81, 1, 15, 1 | turns the ball into a snowball |
| 11 | 7, 1, 7, 49, 1, 10, 4 | turns the ball into a snowball |
| 12 | as 0 | a water surface: ripples |
| 13 | 1, 1, 1, 1, 0.9, 10, 1 | a gravity scale below 1: the glove does not slide on it, and a slow ball stays on it |

## Limits

- 65,536 static triangles, at most 32,767 in a chunk (the game's `s16` counts), and at most 8,192 chunks.
- 256 bodies, each with at most 4,096 triangles.
- Coordinates below 100,000 in absolute value. The game kills actors below a height of -10,000.

## Test scenes

A host scene is a text file that a test gives the host world. weemitt's headless mode reads it with
`--host-scene FILE` (with `--level N`), and the library's test host with `--scene FILE`. Both make the same requests
at the same moments, through `src/port/hostscene.c`. Each line is one item; `#` starts a comment. A frame F is a
time in original frames: a request at F is made before the first update that starts at F or later.

```
scale S                                       game units per file unit (1 by default); it scales the
                                              conveyors' velocities and the waters' currents too
options [cell C] [maxtris N] [link R] [keep 0|1] [off MASK]   MASK: 1 the enemies, 2 the collectables
static F                                      when the triangles and the bodies are given (default 0)
conveyor K VX VY VZ                           conveyor K (1 to 4096): V per original frame
tri X1 Y1 Z1 X2 Y2 Z2 X3 Y3 Z3 [TERRAIN [FLAGS [CONVEYOR]]]   FLAGS: 1 the camera passes, 2 a void;
                                              CONVEYOR: K, on static triangles only
body ID X Y Z QX QY QZ QW SX SY SZ            its tri lines follow, in its own space, up to "end"
end
move ID sine PERIOD DX DY DZ                  the body at its start pose plus D sin(2 pi t / PERIOD)
move ID spin PERIOD                           the body turned about y, once per PERIOD frames
pose F ID X Y Z QX QY QZ QW SX SY SZ
bodyconveyor F ID VX VY VZ                    a conveyor on the body, in its own space; 0 0 0: none
water X Y Z SX SY SZ SURFACE [CX CY CZ]       a water volume; the waters go in with the static triangles
water F off                                   removes the waters
remove F ID
place F [hand X Y Z] [ball X Y Z] [handvel X Y Z] [ballvel X Y Z] [yaw A] [camera]
respawn F X Y Z YAW
view F off | view F FLAGS EX EY EZ AX AY AZ UX UY UZ FOVY ASPECT NEAR FAR
clear F
goto F LEVEL
```

With `--trace`, both programs print a `bodies` line after each frame's line: the glove's action, whether the glove
holds the ball, the contacts and their terrains, readiness, the restarts, the velocities as float bits, the void
states and touches, the safe places as float bits, and the water states.

The scenes in `tools/hostworld/` are made by hand or by `make_scenes.py` from simple shapes, never from a game's
data:

| Scene | What it shows |
| --- | --- |
| `flat.scene` | a floor: the ball rests at 9 (its radius), the glove at 13 |
| `ramp.scene` | the ball rolls down a ramp |
| `steps.scene` | three steps and a wall |
| `seams.scene` | a grid of 1,152 triangles in 144 chunks; the ball rolls across their seams |
| `terrains.scene` | pads of terrain 0, 8, 12, 9 and 6 |
| `void.scene` | a ledge and a pit whose floor is a void |
| `off.scene` | a floor and a death pad, with the level's enemies and collectables switched off |
| `lift.scene`, `slider.scene`, `turntable.scene` | a body that rises and falls, slides, or turns (and turns the glove) |
| `remove.scene` | the slider removed while the glove stands on it |
| `conveyor.scene` | a floor with a conveyor strip that moves the glove along +x |
| `beltbody.scene` | a slider and a turntable with conveyors of their own |
| `hostwater.scene` | a host water over a floor: the glove swims, the ball floats, then the water goes |
| `reload.scene` | a reload of the level: the world is built again |
| `respawn.scene` | a respawn that stays after a reload of the level |
| `view.scene`, `steer.scene`, `mirror.scene` | a drawing view, a steering view, and the mirror view |

[testing.md](testing.md#the-host-world) lists the tests.

