# The library

The game builds as a library with no window: `libglover.so` on Linux and `glover.dll` on Windows, each 64-bit or
32-bit, and `libglover.dylib` on macOS and `glover.framework` on iOS, both arm64. A host program loads it, gives it the player's ROM, and runs the game one update at a time. The host gets the display
lists, the sound and the game's memory, and can let the library draw the picture with the host's OpenGL context.
Possible hosts are another game's engine or a mod for a Steam game. The interface is one C header,
`src/lib/glover.h`. [port64.md](port64.md) describes the 64-bit build.

## Building

| Target | Output |
| --- | --- |
| `make lib64` | `build/lib64/libglover.so`: the real sound player and the renderer |
| `make lib64-stub` | `build/lib64-stub/libglover.so`: the stand-in sound player, no renderer. The tests use it |
| `make lib64-hostgfx` | `build/lib64-hostgfx/libglover.so`: the real sound player and `glover_host_gfx`, with no OpenGL (`RENDERER=hostgfx`) |
| `make lib-windows64` | `build/lib-windows64/glover.dll` and its import library `libglover.dll.a` |
| `make lib-windows64-stub` | `build/lib-windows64-stub/glover.dll`: the stand-in sound player, no renderer |
| `make lib32`, `lib32-stub`, `lib-windows32`, `lib-windows32-stub` | the same as 32-bit libraries, in `build/lib32` and so on |
| `make libhost64`, `libhost32` | `build/libhost64/glover-host` and `build/libhost32/glover-host`: the test host (below) |
| `make libhost-windows64`, `libhost-windows32` | `build/libhost-windows64/glover-host.exe` and the 32-bit one |
| `make libglshot64`, `libglshot32` | `build/libglshot64/glover-glshot` and the 32-bit one: the picture test |
| `make libcheck` | runs `tools/libcheck.py` ([testing.md](testing.md#the-library)) |
| `make lib-macos` | `build/lib-macos/libglover.dylib`: macOS on arm64, the real sound player and `glover_host_gfx` |
| `make lib-macos-stub` | `build/lib-macos-stub/libglover.dylib`: the stand-in sound player, no renderer |
| `make lib-ios` | `build/lib-ios/glover.framework`: iPhones and iPads (arm64), the real sound player and `glover_host_gfx` |
| `make lib-iossim` | `build/lib-iossim/glover.framework`: the same for the iOS Simulator on arm64 Macs |
| `make lib-xcframework` | `build/glover.xcframework`: the two iOS frameworks joined |
| `make libhost-macos`, `libhost-iossim` | `build/libhost-macos/glover-host` and the Simulator's: the test host |

`LIB=1` builds any configuration as the library, with `BITS=64` or the default 32. The library has
the game, the sound player, the ROM checks (`src/port/rom.c`), the memory hash (`src/port/statehash.c`), the
renderer (`src/gfx`) and `src/lib`. It does not have the platform layer, the menu or SDL.

- Linux: the code is position-independent (`-fPIC`). Only the `glover_` functions are exported: the code is
  compiled with `-fvisibility=hidden`, and a version script (`src/lib/glover.map`) hides the symbols of `gen/ram.S`
  too. The library needs only `libc.so.6` and `libm.so.6`. The 64-bit one uses `fmod` from glibc 2.38, so it does
  not load with an older glibc, such as the Steam Runtime's. The 32-bit one needs glibc 2.17.
- Windows: the functions marked `GLOVER_API` (`dllexport`) are exported, and nothing else. The DLL needs only
  `KERNEL32.dll` and `msvcrt.dll`. It may load at any address, also above 4 GB. It gives the same game, trace for
  trace and hash for hash, as the Linux library. The 32-bit DLL's functions realign the stack on entry
  (`force_align_arg_pointer`): a host built with another compiler may call them with a stack aligned on 4 bytes.
- 32-bit: the library stores the game's pointers as offsets, as the 64-bit builds do
  ([port64.md](port64.md#the-representation)), because it cannot choose its load address. So `GloverMemory.wordBase`
  is `0x0C000000` in every library, and the memory hash is the 32-bit program's.

- Apple (macOS and iOS): see [Apple's systems](#apples-systems).

`src/lib/glover.syms` lists the exported names. `tools/libcheck.py` checks the libraries against it.

### Apple's systems

The Apple libraries build only on macOS, with Xcode's clang (`xcrun`). GitHub's macOS runners can build and test
them. `TARGET=macos`, `ios` or `iossim` with `LIB=1 BITS=64` builds any
configuration for them; `MACOS_MIN` (11.0) and `IOS_MIN` (15.0) give the oldest systems they run on.

- The game's flags are the same as on Linux, with `-ffp-contract=off` (no fused multiply-add). Clang has no
  `-fno-aggressive-loop-optimizations`, and `-Wno-c++11-narrowing` lets it take the game's C++ initializers as GCC
  does. The game's files are compiled with `-nostdinc++`: libc++'s `<math.h>` would add the C++ overloads of `sqrt`
  and `abs` that `port_pre.h` refuses.
- `gen/ram.S` puts an underscore before each name (`SYM`), as Mach-O does, and starts `port_ram` on 16 KB.
- `port_ptr_delta` (`src/port/loaders.c`) is set by a constructor. As an initialized variable it is an address
  before the library's start, which the chained fixups of iOS 15 cannot hold: the linker wrote a wrong value, and
  the library crashed in the Simulator.
- Only the names in `glover.syms` are exported (`-exported_symbols_list`, with the underscore). The library needs
  only libSystem.
- iOS takes a library only in a framework. `glover.framework` holds the binary `glover` (its install name is
  `@rpath/glover.framework/glover`), an `Info.plist` (`src/lib/Info.plist.in`) and `Headers/glover.h`. An app puts it
  in its `Frameworks` folder. `make lib-xcframework` joins the device and Simulator frameworks with `xcodebuild
  -create-xcframework`.
- Clang has no `__builtin_setjmp` for arm64. The jump back from a fatal error uses `_setjmp` and `_longjmp` there
  (`src/lib/lib.h`). The sound player's flush-to-zero uses the FPCR's FZ bit, which flushes the inputs too.
- `glover_build_id` names the system as `macos-arm64`, `ios-arm64` or `iossim-arm64`.
- The test host runs in the Simulator with `xcrun simctl spawn`, with `SIMCTL_CHILD_GLOVER_LIB` naming the
  framework's binary.

## Loading and calling

A host includes `glover.h` and either links with the library or loads it at run time (`dlopen`, `LoadLibrary`). The
test host `tools/libhost/libhost.c` is a complete example of the second way, in plain C.

```c
GloverConfig c;
Glover *g;
char err[256];

glover_default_config(&c);
c.rom = romBytes;               /* the player's ROM: .z64, .v64 or .n64; the library copies it */
c.romSize = romSize;
c.eeprom = saveBytes;           /* 512 bytes, or NULL for an empty save */
c.startLevel = -1;              /* -1: the game starts as at power-on; or a level, as weemitt's --level */
c.ballworld = 0;                /* 1: the Ballworld mode, as weemitt's --ballworld */
if (glover_create(&c, &g, err, sizeof(err)) != GLOVER_OK) {
    /* err says why */
}
for (;;) {
    GloverInput in = { buttons, stickX, stickY };
    GloverFrame f = { sizeof(f) };

    glover_update(g, 1.0f, &in, &f);   /* one original frame */
    /* f.lists: the display lists, when f.listCount >= 0 */
}
```

Every call returns `GLOVER_OK` (0) or a negative `GLOVER_ERR_` code. A structure that may grow in a later version
starts with its size: set it to `sizeof` the structure. `glover_api_version()` gives `GLOVER_API_VERSION`, 4, and
`glover_create` accepts a configuration made for version 1 to 4 and refuses a later one.

| Version | What it added |
| --- | --- |
| 1 | the game, its input and state, sound, the renderer |
| 2 | the host's world: collision, bodies, places, the respawn, a camera; `GloverBodies` grew |
| 3 | turning bodies, conveyors (`GloverMesh` grew), water volumes, draw marks, the layer and the composite; `GloverCollStats` and `GloverBodies` grew; later the volume options 7 and 8, which a library from before them refuses with `GLOVER_ERR_ARG` |
| 4 | `glover_host_gfx`: the lists for the host's own F3DEX2 interpreter; later the host's fog (`GLOVER_HOSTGFX_FOG`, `GloverHostGfx` grew), which a library from before it refuses with `GLOVER_ERR_ARG` |

One change of version 3 reaches older hosts too. A drawing view (`GLOVER_VIEW_DRAW`) is now drawn with its own field of
view, aspect and planes. Earlier libraries drew it with the game camera's projection, because `ClearViewing` wrote that
projection back at the end of each frame. A version 2 host with a drawing view gets a different, correct picture.

### The rules

- **One game per process.** The game's state is in the library's globals. A second `glover_create` returns
  `GLOVER_ERR_BUSY`, also after `glover_destroy` and after a start that failed. For a new game, the host unloads the
  library and loads it again (`dlclose` and `dlopen`, `FreeLibrary` and `LoadLibrary`); the game then starts as in a
  new process. `libcheck.py` tests this.
- **Memory.** `glover_create` returns `GLOVER_ERR_MEMORY` when it cannot allocate its copy of the ROM.
- **The end.** `glover_destroy` frees the library's copy of the ROM. The handle stays valid: later calls with it
  return `GLOVER_ERR_DEAD`, and `glover_destroy` again returns `GLOVER_OK`. It returns `GLOVER_ERR_REENTER` while
  another call runs.
- **Arguments.** A start level outside -1 to 47, a `glover_goto_level` level outside 0 to 42, renderer options
  outside their ranges, a structure whose `size` is too small, and a dt outside (0, 1] give `GLOVER_ERR_ARG`.
  `glover_error` holds only the message of a fatal error: an argument error leaves no text.
  `glover_create` writes the reason of an error to `err`, and an empty string on success.
- **One thread at a time.** The calls may come from any thread, but not from two at once. A second call while one
  runs returns `GLOVER_ERR_REENTER`, and so does a call from the log callback. The exceptions are
  `glover_audio_read` and `glover_audio_rate`, which may run on one other thread at the same time.
- **The floating-point mode.** Each call sets the SSE control register (MXCSR) to the default, `0x1F80`, and puts
  the host's value back before it returns. On arm64 it does the same with the FPCR and its default, 0. A host that runs with flush-to-zero or another rounding mode therefore
  gets the same game.
- **Fatal errors.** When the game stops (`port_fatal`: out of memory, a missing bank, bad sound data), the call
  returns `GLOVER_ERR_FATAL`, and `glover_error` gives the message, which starts with `fatal: `. Every later call
  that runs the game returns `GLOVER_ERR_DEAD`; `glover_destroy`, `glover_error`, `glover_audio_read`,
  `glover_audio_rate` and `glover_gl_shutdown` still work. When the game stops inside `glover_create`, the call
  returns `GLOVER_ERR_FATAL` with the message in `err`, gives no handle, and frees the ROM's copy. The library never
  ends the process. It jumps from the game's code back to the start of the call (`__builtin_longjmp`; on arm64 `_longjmp`), past the
  library's own frames only.
- **The log.** `GloverConfig.log` gets the game's messages, one line at a time, on the thread of the call. It must
  not call the library.

### Time: the update's length

`glover_update(g, dt, ...)` runs dt original frames, with 0 < dt <= 1 ([delta-time.md](delta-time.md)). At dt 1
the logic is exactly the N64's. An original frame lasts `GloverStatus.retraces` sixtieths of a second: 3 in a level
(20 frames a second) and 2 in the menus (30 a second).

- A host that runs the game at the N64's own rate calls `glover_update` with dt 1 once every `retraces / 60`
  seconds. This is the exact game.
- A host that draws at another rate gives each update the time that has passed, in original frames: dt = seconds *
  60 / retraces. A gap longer than one original frame is split into updates of at most 1. weemitt's window loop
  (the Smooth mode in `platform.c`) also runs a whole frame when the time is within 15% of one, and carries the
  rest to the next drawn frame.
- While a recorded demo plays (`GloverStatus.demo`, in the attract mode), pass dt 1: the demo's input was recorded
  per original frame.

`GloverFrame.ticked` is 1 when an original frame ended in the update. The game reads the controller once per
original frame, at the first update of the frame.

The game's profiler timers read a clock, and they are in the game's memory. `GloverConfig.clockUs` gives that
clock. Without it, the library uses its own: after each update it adds dt * retraces / 60 seconds. Either way the
game's logic does not depend on it. Two runs give the same memory hash only with the same clock; the test host
gives the clock of weemitt's headless mode.

### Input and state

- `GloverInput` is controller 1: libultra's button bits (`GLOVER_BTN_`) and the stick, about -80 to 80.
- `glover_status` gives the game's mode and level, whether a demo plays, the frame's retraces, the frames since the
  start, the controller word that the game read last, and the sound queue's counts.
- `glover_bodies` gives the positions of the glove, the ball and the camera, and since version 2 their velocities,
  the glove's yaw and action, whether it holds the ball, the contacts and their terrain, whether the glove is ready,
  the count of restarts, the voids and the safe places ([host-world.md](host-world.md#voids)). Set
  `GloverBodies.size` first: a host of version 1 gives the old size, 40 bytes, and gets the positions only. A host
  built before the voids gives a size that ends before `handVoid`, and gets the fields up to there. Version 3 adds
  `handWater` and `ballWater`, the game's water state ([host-world.md](host-world.md#water)).
- `glover_set_option` sets the options of weemitt's Enhancements and Cheats menus: the easy ball controls, the
  camera's follow speed (1 to 3), skipping the start-up screens, the cheat list in the pause menu (on by default, as
  in weemitt), the cheats held on, and the volume of the songs and of the sound effects. One more option is the
  host's: the scale of the camera's distances ([Sound](#sound)). They take effect at the next update. `glover_goto_level` starts a level, 0 to
  42, where `GloverStatus.gotoPossible` is 1 (in a level or the title menu).
- `GloverConfig.ballworld` 1 plays every level as the ball alone: the Ballworld mode of weemitt's Enhancements menu
  ([menu.md](menu.md)). It holds for the whole game. weemitt keeps a separate save file for the mode
  (`eeprom-ballworld.bin`), and a host should do the same. `glover_get_option(GLOVER_OPT_BALLWORLD)` gives it;
  `glover_set_option` refuses it. A host built before the field gives a size that ends before it, and gets the mode
  off.
- `glover_eeprom` gives the save's 512 bytes, and 1 when the game wrote it since the last call. The host keeps the
  file.
- `glover_memory` gives the game's memory, read-only: N64 address `0x80000000 + n` is `ram[n]`, in little-endian.
  A pointer in it, and a display list's address, is a 32-bit word w for `ram + (w - wordBase)`
  ([port64.md](port64.md#the-representation)). `glover_trace` gives weemitt's `--trace` line and
  `glover_hash_memory` weemitt's memory hash.

### Sound

`glover_audio_rate` gives the rate (22047 Hz), or 0 in a build with the stand-in sound player. It may run on the
sound thread. The sound player runs
on the game's clock, as in weemitt's headless mode: each update makes the frames for its time. The library queues
them, 65,536 frames of 16-bit stereo. They are the same samples, byte for byte, as weemitt's headless mode writes with
`--audio-out`. `glover_audio_read` takes them, on the game's thread or on one other thread.
When the queue is full, new frames are dropped and counted (`GloverStatus.audioDropped`). The queue is outside the
game's memory: reading the sound or not changes nothing in the game. A host plays the frames at their rate, and
resamples them for its own device.

`GLOVER_OPT_MUSIC_VOLUME` and `GLOVER_OPT_EFFECTS_VOLUME` set the volume of the songs and of the sound effects in the
frames, each from 0 to 100 percent. Both are 100 at the start. They work as weemitt's Settings, Audio: the values live
outside the game's memory, and the logic stays the same. The synthesizer's voices in the game's memory get the scaled
volume, so the memory hash (`glover_hash_memory`) can differ. A note that plays already changes at its next volume
update. With both at 0 the frames are silent. The stand-in sound player keeps the values and plays nothing.

`GLOVER_OPT_CAMERA_DISTANCE_SCALE` multiplies the distances from the game's camera in four rules, from 0.01 to 100.
It is 1 at `glover_create`.

- **The sound** (`sound.c`): an effect's volume fades with its distance from the camera, and it is silent from the
  level's sound distance on (`MAX_SFX_DIST`, 1000 units in level 12). This holds for `PlaySample`,
  `PlaySampleRadius` and `UpdateContinuousSample`. With a radius, the distance minus the radius, times the scale, is
  the distance. The pan comes from the direction alone, so the scale does not change it.
- **The wakes on water** (`CreateWake`): made within the square root of twice `DETAIL_RANGE`, 283 units in most
  levels.
- **The debris** (`AddDebris`), such as the splashes on water: made within 1500 units.
- **The shadows** (`shadow.c`) of the bullets, the actors and the garibs: cast within 600 units, and faded out from
  570. The game also finds the floor under each of them only within 600 units (`CalculateShadows`). The glove and
  the ball always have their shadows.

A host whose camera stands farther from the glove in game units than the game's own camera does sets a scale below
1. Take a host that draws the glove a third as tall as at the size it was made for: its camera stands three times as
far in game units, and a scale of 1/3 gives the sound and the wakes of that size. The other distances from the camera
stay the game's. What the game draws follows the host's far distance ([host-world.md](host-world.md#the-camera)).
So do the enemies it updates: those within the far distance plus 200 units. The value lives outside the game's
memory. At 1 every distance is the same number as before, so the game's logic, its memory and its sound stay the
same, byte for byte. At another value the logic can change too:

- `AddDebris` draws a random number for each debris it makes, from the game's only random sequence. Debris near the
  limit then changes every later random number, such as the glove's choice between its two jump sounds.
- The game's logic reads some sound handles: a Samtex enemy keeps updating while its sound plays.
- Enemies read the floor under them that `CalculateShadows` finds.

### Drawing

With `make lib64` (or `lib32`, `lib-windows64`, `lib-windows32`) the library has weemitt's OpenGL 3.3 renderer. The
renderer's calls go on the thread whose OpenGL 3.3 core context is current, and never during `glover_update`.

1. `glover_gl_init(g, load, user)` once: `load(user, "glGenBuffers")` must return the context's functions. It
   returns `GLOVER_ERR_GL` when one is missing, and `GLOVER_ERR_ARG` when the renderer is already set up.
2. `glover_gl_options` sets the resolution scale (50 to 400 percent), the anti-aliasing samples (0, 2, 4 or 8), the
   texture filter (0 the N64's, 1 bilinear, 2 nearest) and the widescreen mode (0 off, 1 stretched, 2 a wider view).
   The defaults are weemitt's: 100%, 4 samples, the N64's filter, no widescreen.
3. `glover_gl_draw(g, fbo, w, h)` draws the latest display lists into framebuffer `fbo` (0: the default) of `w` x
   `h` pixels, black outside the 4:3 picture. It sets every OpenGL state it uses. It draws whole game frames: it does
   not blend two frames as weemitt's Original physics mode does.
4. `glover_gl_read_frame` reads the latest picture at its own size, RGBA rows from the top. It returns
   `GLOVER_ERR_ARG` with the size in `w` and `h` when the buffer is too small, and with 0 and 0 before the first
   picture or before `glover_gl_init`.
5. `glover_gl_shutdown` frees the renderer's objects. It works also after `glover_destroy` or a fatal error, but
   must come before the context goes.

**The OpenGL state.** Each of these calls leaves OpenGL's state as in a new context, except three things: the
viewport (`glover_gl_draw` sets it to 0, 0, w, h), the scissor box, and the framebuffer bound to `GL_FRAMEBUFFER`
(read and draw), which is the `fbo` of the latest `glover_gl_draw` or `glover_gl_composite`, or 0 before one. So after each call: no program,
vertex array, array buffer, renderbuffer or texture bound (units 0 and 1), texture unit 0 active, the depth test,
blending, culling, the scissor test and the polygon offset off, dithering on, the depth mask on, the depth function
`GL_LESS`, the blend function `GL_ONE`, `GL_ZERO`, the clear colour 0, 0, 0, 0, the clear depth 1, and the pixel
store alignments 4. A host that keeps other values sets them again after the call. The picture test checks each of
these after every call.

The stub builds return `GLOVER_ERR_UNSUPPORTED`. A host with its own renderer reads the display lists from
`glover_memory` instead.

**Draw marks.** `glover_draw_marks(g, out, max, count)` tells a host that reads the display lists itself which
commands draw what, for the latest lists. Each mark is an address and a layer: from that command on, the commands
belong to the layer, until the next mark. The layers are the `GLOVER_LAYER_` bits: the backdrops (`SKY`), the level's
landscape, water and platform actors (`SCENERY`), everything else of the 3D picture (`ACTORS`: the glove, the ball,
the enemies, the collectables, the bullets, the debris, the sprites, the shadows and the footprints) and the overlays
(`HUD`). Layer 0 is the frame's setup: the clears, the matrices and the modes. A list that a command calls belongs to
that command's layer. The addresses rise through the frame, across its lists.

- **How.** The game calls `port_draw_mark_hook` (`src/port/porthooks.h`) where each part begins: in `GraphicsStep`
  (`main.c`) at the frame's start, before the backdrops, before the objects and before the overlays, and in
  `CreateObjects` (`object.c`) around each actor of the drawing list. `src/port/drawmark.c` keeps the address of the
  next command and the layer whenever the layer changes, at most 4,096 a frame.
- **Exactness.** The library sets the hook at `glover_create`; weemitt's programs leave it NULL. The marks live
  outside the game's memory, and the game never reads them: its memory and its logic stay the same.
- **A frame of level 5.** Frame 100 has 36 marks: the setup, the sky, then the world's actors and scenery in turn,
  and the HUD (`libcheck.py` checks this order).

### The host's world

Version 2 of the interface lets a host put the glove and the ball into its own world: its triangles to collide
with, moving bodies, places for the glove and the ball, where a restart puts the glove, and a camera that draws or
steers. [host-world.md](host-world.md) describes the calls (`glover_coll_`, `glover_place`, `glover_set_respawn`,
`glover_set_view`). Version 3 adds conveyors and water volumes to the host's world.

`glover_create` accepts a configuration of version 1, 2 or 3. A host built for an earlier version keeps working:

- Its `GloverMesh` ends before `conveyor` (32 bytes on 32 bits, 56 on 64 bits). The library takes the shorter size
  and gives the mesh no conveyors.
- Its `GloverCollStats` is 40 bytes. The library fills that part.
- Its `GloverBodies` ends earlier. The library fills the part that it holds.

### Drawing over the host's picture

A host that draws its own world, such as another game's engine, puts Glover's actors over it. Since
version 3 the library draws a layer for this: some parts of the latest picture, with alpha, and with the depth of
each pixel.

```c
GloverGlLayer l = { sizeof(l) };
l.layers = GLOVER_LAYER_ACTORS;            /* the glove, the ball, enemies, collectables, effects, shadows */
l.width = w;                               /* the host's picture's size; the scale of glover_gl_options applies */
l.height = h;
glover_gl_draw_layer(g, &l);               /* l.colorTexture, l.depthTexture, l.texWidth, l.texHeight, the planes */
/* the host draws its opaque scene into fbo, with its depth buffer */
glover_gl_composite(g, fbo, w, h, 0.0005f, GLOVER_COMPOSITE_DEPTH);   /* hidden where the host's scene is nearer */
/* the host draws its translucent passes (water, fog volumes, particles), tested against the glove's depth */
```

- **When.** Until `GloverBodies.ready` is 1, the game plays its own start of the level at its own places: the glove
  falls in sparkles at the level's start point. A host leaves the layer out until then, and after each restart
  (`GloverBodies.restarts` changes) until `ready` is 1 again.
- **The parts.** `layers` takes the `GLOVER_LAYER_` bits of the draw marks (above): `ACTORS`, `SCENERY` (the level's
  landscape, water and platform actors), `SKY` (the backdrops, and the colour that the frame is cleared to) and
  `HUD`. `GLOVER_LAYER_NO_FOG` draws without the game's fog: its colour is the game level's, not the host's. The
  renderer leaves out each primitive whose layer is not asked for; the state commands still run. A full-screen fill
  of a part that is left out (a fade of the HUD, say) is left out too.
- **The colour.** An RGBA8 texture, rows from the bottom, with premultiplied alpha: 0, 0, 0, 0 where nothing is
  drawn. A primitive that does not blend with the picture under it writes alpha 1. One that mixes with it by its
  alpha writes that alpha over what is there. One that adds to it (the game's glows) adds colour and no alpha.
  Anti-aliasing (the multisampling of `glover_gl_options`) gives the edges their share of alpha.
- **The depth.** A `DEPTH_COMPONENT32F` texture of the same size: the window depth (0 at the near plane, 1 at the far
  plane) of the nearest pixel that any primitive of the layer drew, 1 elsewhere. The renderer draws the frame a second
  time for it, depth only: a pixel that the colour pass leaves without alpha (or, where the primitive adds, without
  colour) writes no depth. Rectangles without a depth of their own (the HUD, the 2D sprites) lie at 0. The
  renderer passes the game's clip-space z and w as they are, so the depth is OpenGL's standard depth for the game's
  projection: with `GLOVER_VIEW_DRAW`, the host's view and its `nearZ` and `farZ`, in game units. The game's
  projection matrix is s15.16 fixed point, so its terms differ a little from the exact ones: `projZ` gives them.
- **The size.** The 4:3 picture is stretched over `width` x `height`, as widescreen 1 shows it; the drawing view's
  aspect is in its projection. The HUD is stretched too. With all four parts and a 4:3 size the layer's colour equals
  `glover_gl_draw`'s picture, pixel for pixel, with alpha 1 everywhere. A size beyond the GPU's largest renderbuffer
  is scaled down to it, keeping its shape; `texWidth` and `texHeight` give the size made.
- **The textures** belong to the library. They stay valid until the next `glover_gl_draw_layer` or
  `glover_gl_shutdown`. The OpenGL state promise above holds for both calls; `glover_gl_composite` leaves its `fbo`
  bound, as `glover_gl_draw` does. A `glover_gl_draw_layer` that fails leaves no layer: `glover_gl_composite` then
  returns `GLOVER_ERR_ARG`.
- **The composite.** `glover_gl_composite(g, fbo, w, h, bias, flags)` draws one full-screen triangle over `fbo`: the
  layer's colour with premultiplied blending (`GL_ONE`, `GL_ONE_MINUS_SRC_ALPHA`), and its depth with the depth test
  `GL_LEQUAL` against `fbo`'s depth buffer. It assumes OpenGL's default depth range, 0 to 1. It uses the projection
  that the layer was drawn with: a change of the view or a `glover_update` in between does not change it. Each pixel's
  depth is pulled towards the eye by `bias`, a share of its view depth, from 0 up to but not including 1. Without
  `GLOVER_COMPOSITE_DEPTH` it writes no depth. With it, it then writes the layer's depth where the layer's alpha is at
  least 0.5 and the layer is nearer: the host's translucent passes after it are hidden behind the glove and blend over
  it. The edges with less alpha keep the host's depth.
- **The bias.** The layer's depth, made linear with `projZ`, gives the view depth of the game's own vertices. What is
  left is the s15.16 rounding of the actors' matrices and of the camera matrix, and the host's own depth precision. In
  `libcheck.py`'s scene the glove at 1549 units comes back at the same view depth within 0.02% for near planes of 1
  and of 50. A bias of 0.0005 (0.5 units at 1000) keeps the glove's shadow and feet from flickering into the host's
  floor. Made linear with `nearZ` and `farZ` instead, a near plane of 1 puts the glove 7 units (0.45%) too near at
  1549 units.

**Compositing without the library's call.** A host whose depth is not OpenGL's default (reversed depth, another
range, a depth in its own units) composites from the two textures. The view depth (the distance along the view axis,
not to the eye) of a layer pixel of window depth `d`, with the layer's `projZ`:

```
z_ndc = 2 d - 1
e     = projZ[1] / (z_ndc + projZ[0])
```

The host then compares `e * (1 - bias)` with the view depth of its own pixel, in the same units. The library's
shader, for a host depth of OpenGL's default with the planes `n` and `f` (`nearZ` and `farZ`):

```glsl
uniform sampler2D uColor, uDepth;   /* the layer's textures */
uniform vec4 uSize;                 /* the target's width and height, the textures' width and height */
uniform vec4 uPlanes;               /* nearZ, farZ, bias, and 0 (the colour pass) or 0.5 (the depth pass) */
uniform vec4 uProjZ;                /* projZ[0], projZ[1] */
out vec4 fragColor;
void main() {
    ivec2 t = ivec2(gl_FragCoord.xy * uSize.zw / uSize.xy);
    vec4 c = texelFetch(uColor, t, 0);
    float d = texelFetch(uDepth, t, 0).r;
    if (uPlanes.w > 0.0 ? c.a < uPlanes.w : c.a <= 0.0 && max(max(c.r, c.g), c.b) <= 0.0) discard;
    if (d > 0.0 && d < 1.0) {
        float n = uPlanes.x, f = uPlanes.y;
        float e = uProjZ.y / (2.0 * d - 1.0 + uProjZ.x);
        e = max(e * (1.0 - uPlanes.z), n);
        d = ((f + n) - 2.0 * n * f / e) / (f - n) * 0.5 + 0.5;
    }
    gl_FragDepth = d;
    fragColor = c;
}
```

The colour pass blends with `GL_ONE`, `GL_ONE_MINUS_SRC_ALPHA`, tests `GL_LEQUAL` and writes no depth. The depth pass
(`GLOVER_COMPOSITE_DEPTH`) writes depth only, with the colour mask off.

- **Precision.** The layer's depth is the host's only when the library draws from the host's camera
  (`GLOVER_VIEW_DRAW`) with the host's `nearZ` and `farZ` in game units: then both project the same points to the same
  depths, at any scale. Without a drawing view the layer is in the game camera's projection (`nearZ` and `farZ` give
  its planes), which a host cannot match.
- **What the composite cannot do.** The host's fog and tints do not reach the layer: the glove keeps the game's fog
  (or none, with `GLOVER_LAYER_NO_FOG`). A host that wants its own fog on the glove draws the layer's colour through
  its own shader, from the textures.
- **The tests.** `libcheck.py --gl` checks the calls' arguments, the OpenGL state after each call, the full layer
  against `glover_gl_draw`, and on a scene with a drawing view: alpha 0 at the corners of a layer of the actors, alpha
  1 on the glove, its depth made linear within the glove's radius of its distance along the view, the composite over
  a red host picture (hidden where the red is nearer and shown where it is farther), the depth that
  `GLOVER_COMPOSITE_DEPTH` writes, no composite after a failed layer, and the same view depth for near planes of 1 and
  50.

### Lists for the host's interpreter

A host that runs its own interpreter of display lists draws Glover with it. An F3DEX2 interpreter such as Fast3D
can draw with OpenGL, Metal or Direct3D. Since version 4, `glover_host_gfx` translates the latest lists into F3DEX2,
the microcode such an interpreter is built for. A command is two words of pointer size, as the interpreter's `Gfx`
holds it on that host. The library then needs no OpenGL: `RENDERER=hostgfx` builds it without the renderer
(`make lib64-hostgfx`).

```c
GloverHostGfx h = { sizeof(h) };
h.gbi = GLOVER_GBI_F3DEX2;
h.layers = GLOVER_LAYER_ACTORS;            /* as for the layer: the parts whose triangles are drawn */
h.t = -1.0f;                               /* the latest frame; 0 to 1: blended from the previous one */
h.projection[0] = hostProjection;          /* replaces each projection group of the game; NULL: the game's own */
h.projection[1] = hostView;                /* multiplied in, in order, up to a NULL or the fourth */
h.flags = GLOVER_HOSTGFX_SPLIT;            /* or 0: everything in h.list */
h.flags |= GLOVER_HOSTGFX_FOG;             /* optional: the host's fog on every triangle */
h.fogColor = 0x80A0C000;                   /* red, green, blue, alpha, as G_SETFOGCOLOR */
h.fogMultiplier = hostFogMul;              /* as G_MW_FOG */
h.fogOffset = hostFogOffset;
glover_host_gfx(g, &h);                    /* h.list: the entry, which ends with G_ENDDL; h.xlu the same */
/* the host runs h.list in its interpreter among its opaque surfaces, for example as a G_DL from its own list, and
   h.xlu after its translucent ones */
```

- **What it writes.** Each F3DEX command becomes its F3DEX2 command: the matrix flags, the vertex loads and triangles,
  the geometry mode bits, the other modes, the texture scale, the lights, the light count and the fog. The RDP
  commands pass as they are. Each list that a `G_DL` calls is written again, so the calls, the branches and the
  ends of lists stay where the game has them. The Sprite2D tasks (the HUD's backdrops) are left out.
- **The parts.** The state commands of every part run, as for the layer; the vertex loads and triangles only in the
  parts that `layers` names. `GLOVER_LAYER_NO_FOG` leaves `G_FOG` out of the geometry mode.
- **Two lists.** With `GLOVER_HOSTGFX_SPLIT`, the triangles whose render mode does not write depth (no `Z_UPD`) go to
  a second entry list, `xlu`: the glove's and the ball's shadows, the rings on the water, the 3D sprites. The rest
  stay in `list`. Both lists carry every state command and vertex load, so each runs on its own. The host runs `list`
  among its opaque surfaces and `xlu` after its translucent ones, in the same drawn frame. A flat effect then lies on
  the host's translucent ground and water. Run in one place, it would lie under them: a translucent surface drawn
  later paints over a triangle that left no depth. The invalidations are in `list` only.
- **The host's fog.** With `GLOVER_HOSTGFX_FOG` every triangle takes the host's fog instead of the game level's.
  Each geometry mode keeps `G_FOG`, the first cycle of each render mode blends in the fog colour
  (`G_RM_FOG_SHADE_A`), each `G_SETFOGCOLOR` gives `fogColor`, and each fog position gives `fogMultiplier` and
  `fogOffset`. Each entry list sets them at its start too, and the 3D sprites keep `G_FOG`. The game's own render
  modes do the same in a level with fog. The shade's alpha is then the fog amount, so a surface that takes its alpha
  from the shade is opaque; the game's surfaces take theirs from the texture or the primitive colour. The fog amount
  comes from the depth in the host's projection, so with the host's own fog values the glove fogs as the host's world
  does at the same depth. The fog cannot be used with `GLOVER_LAYER_NO_FOG`, nor with a `GloverHostGfx` that ends
  before `fogColor` (`GLOVER_HOSTGFX_SIZE_V4`, the size before the fog was added). Such a structure works without it.
- **The state it starts from.** Each entry list first sets the state that weemitt's renderer starts each frame with:
  no geometry mode, other modes or combiner, white primitive and environment colours, an opaque black fog colour,
  and the texture off. A list then draws the same wherever the host runs it.
- **What it leaves out.** The colour and depth images and their clears, the scissor, the viewport and the segments: the
  host's stay, and the library resolves the game's segments itself. A fill rectangle in a requested part, a texture
  rectangle in the copy mode, and a change of a vertex's colour (`G_MODIFYVTX` of its colour, which the interpreter may
  read as a change of its texture coordinates) are left out and counted in `skipped`, as are `G_MW_MATRIX` and
  `G_MW_POINTS`, which change a matrix or a vertex in place. `G_MW_LIGHTCOL`, which the interpreter may not take,
  becomes a `G_MOVEMEM` of the light with its new colour. `unknown` counts commands of no known microcode.
- **Texture rectangles.** The actors draw 3D sprites as texture rectangles: the dust under the walking glove, sparkles,
  collectables. The interpreter may draw a texture rectangle at the near end, without depth. So each one in a requested
  part becomes two triangles in the world: its corners are taken back through the game's projection at its depth (the
  primitive depth with `G_ZS_PRIM`, else just behind the near plane) and drawn with the host's projection. Lighting,
  fog, culling and texture generation are off for them, the depth test follows their other mode's `Z_CMP`, and the
  game's geometry mode and texture scale are set again after them. `rects` counts them. The corners go into the vertex
  slots 32 to 35, which F3DEX does not have (the interpreter needs at least 36), so the game's own vertices stay. The
  sprite's model matrix is loaded without a push, and the game's modelview is loaded again after it: the interpreter
  refuses a push on a full stack. With `t >= 0` they are blended between frames as the window program blends them: a 3D
  sprite at its blended place in the world.
- **The depth source.** Every other mode in the list takes the pixel's depth: `G_ZS_PRIM` is cleared. The game's
  primitive depth belongs to its own projection, and its sprites carry their depth in their corners. An
  interpreter may ignore `G_ZS_PRIM`, or it may write the primitive depth for every triangle drawn with it.
- **Formats.** With a TLUT the game's 4-bit and 8-bit textures are colour-indexed whatever their tile's format says, and
  without one an RGBA format of that size is intensity, as `gfx_tex.c` decodes them. The interpreter takes a tile's
  format as it is, so the translation writes CI or I into those tiles, and sets them again when the TLUT mode changes.
  It reads every palette as RGBA16: an IA16 palette is given as RGBA16 entries (grey, with alpha from bit 7 of the alpha
  byte).
- **The projection.** A projection group is a projection load and the projection multiplies that follow it, as the
  game's `SetupViewing` writes them in `main.c`, `shadow.c` and `object.c`. With `projection[0]` set, each group
  becomes `projection[0]` loaded and the rest multiplied in order. For row vectors this gives
  `P = projection[3] x projection[2] x projection[1] x projection[0]`. The game's model matrices map to game units,
  and its view is in its projection, so the host's matrices take a point in game units to the host's clip space.
  A host can pass, for example, its projection, a scale towards the eye, its view and a scale from game units to its
  own units.
- **Pointers.** Every pointer in the lists is even (the interpreter reads an odd one as a segment address). Matrices,
  vertices, lights and 4-bit and 8-bit texels point into the game's memory: valid until the next `glover_update`. The
  lists and the blended matrices are in the library's arena: valid until the next `glover_host_gfx`. The game keeps
  16-bit and 32-bit texels and every palette in the host's byte order, and the interpreter reads the N64's, so these are
  copies in the N64's order. The translation reads ahead from each `G_SETTIMG` to its load and to the tile that draws
  it: a 4-bit texture that the game loads as 16-bit words is passed as it is. A copy stays valid for the game's life, or
  until a list carries the host's texture invalidation (opcode 0x34) for its address.
- **Textures that change.** The game rewrites some textures. Each texture, copied or not, is hashed once after each
  update, when it is first used. When it changed, its copy is converted again in place, and the entry list starts with
  an invalidation (opcode 0x34) of its address (the interpreter's texture cache is keyed by the address). A palette that
  changed also invalidates each texture drawn with it, one by one: an invalidation of address 0 would empty the
  interpreter's whole cache, the host's textures too. A texture first seen at an address, and each new copy, gets an
  invalidation too: the host may hold an older texture there, such as one of its own from memory it freed. A copy unused
  for 3600 updates is freed, with an invalidation. Textures loaded in parts that are not requested go through the same
  copies: a later part may draw with them. `glover_destroy` frees the copies and the lists and forgets every texture.
- **The blend.** A call with `t >= 0` makes each later update record its new lists (the matrices of
  `docs/interpolation.md`), with the cuts of `GloverFrame.cut`. A `t` from 0 to below 1 then blends the model
  matrices between the last two recorded frames, as the window program's interpolated drawing does; the blended ones
  are written into the arena. The projection groups are the host's, so its camera decides the view. The recording
  follows every list, as the translation does, and does not cull by the game's view: a list outside the game's 4:3
  view but inside a wider host window still has its record to blend. Matrices too large for the interpreter's fixed
  point are clamped, the same on every processor.
- **The rules.** Call it between updates, on the game's thread, after the first lists. As for the layer, leave the
  lists out until `GloverBodies.ready` is 1. It reads the game's memory and writes none: the memory hash does not
  change. `commands`, `triangles` (in the requested parts), `textures` (the images loaded there), `skipped` and
  `unknown` tell what it wrote. It returns `GLOVER_ERR_UNSUPPORTED` in a library without `src/gfx` (`lib64-stub`).
- **What the interpreter may do differently.** It may light in model space, where weemitt normalises the normal in
  eye space. It may have no flat shading or not cull with `G_CULLDL`, and its texture filter and combiner may round
  differently. The picture then differs from the layer's in these ways.
- **The tests.** `libcheck.py` runs the test host with `--hostgfx 1` over five levels, with the latest frame, with
  `--hostgfx-t 0.5`, and with that and `--hostgfx-split` (the triangles of both lists together, no invalidation in
  `xlu`): no failed call, no unknown command, every pointer even, the vertices inside the game's memory
  (but for a texture rectangle's four), every list ended, and the triangles equal to those of the actors in the game's
  own lists, counted by the test host's own walk. `--hostgfx-dump F` prints the lists of the first call from frame F
  on that writes a texture rectangle. The trace and the memory hash of level 5 stay the same with the call in use. The translation takes about
  0.06 to 0.2 ms a call on the test machine.

## What the library does not do yet

[port64.md](port64.md#what-is-left) lists the rest: savestates, a second game in one process, a pacer, sound pulled
by a host thread, interpolated drawing, and a Direct3D path.
