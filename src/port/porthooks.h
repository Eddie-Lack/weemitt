/* Hooks for a host world (src/port/hostworld.c, docs/host-world.md): collision, bodies and a camera that a host
   program gives. Each is NULL until a host asks for one, and the game then runs as before. frame.c defines them. */
#ifndef PORTHOOKS_H
#define PORTHOOKS_H

struct Actor;

/* Before each update, after a requested level start (frame.c). */
extern void (*port_pre_update_hook)(void);
/* Right after UpdatePlatforms, in a played and unpaused game (main.c, MainGameLoop). */
extern void (*port_platforms_hook)(void);
/* After each update's game logic (frame.c). */
extern void (*port_post_update_hook)(void);
/* At the start of FreeLevel2, before the level's memory is freed (game.c). */
extern void (*port_free_level_hook)(void);
/* In SetupViewing, after the projection and the view are built and before the display list uses them (main.c). Also
   in ClearViewing with view NULL: the projection only, which the frame's earlier lists use too. */
extern void (*port_view_hook)(Mtx *projection, Mtx *view, u16 *perspNorm);
/* In MainGameLoop where the game moves its camera. A result of 1 skips MoveCam and PlayerSpinCamera (main.c). */
extern s32 (*port_camera_hook)(void);
/* In CollideActorWithPlane, before a crystal ball shatters on a face. A result of 1 keeps the ball whole (collision.c). */
extern s32 (*port_no_shatter_hook)(void *face);

/* In AddAmbientSfxAtPoint and in a puzzle's ACTION_START_SOUND at a point. A result of 1 switches the sound off: an
   ambient sound loses its place, as ClearAmbientSfx leaves it, and a puzzle's sound does not play (sound.c,
   puzzle.c). */
extern s32 (*port_point_sound_off_hook)(void);

/* A host's scale for the distances from the game's camera in the sound (sound.c), the wakes on water (CreateWake) and
   the debris (AddDebris), the shadows (shadow.c): each distance is multiplied by it before the game's tests and its fade. A host whose camera
   stands farther from the glove in game units than the game's own sets it below 1 (GLOVER_OPT_CAMERA_DISTANCE_SCALE).
   It is 1 until a host sets it, and a distance times 1 is the same number, so the game runs as before. */
extern f32 port_camera_dist_scale;

/* Where each part of the frame's display lists begins (src/port/drawmark.c): GraphicsStep (main.c) and CreateObjects
   (object.c) call it with the kind of what they draw next. Only the library sets it. */
#define PORT_DRAW_FRAME 0        /* the frame's lists begin: the clears and the setup */
#define PORT_DRAW_SKY 1          /* the backdrops */
#define PORT_DRAW_WORLD 2        /* the objects, the debris, the bullets, the sprites, the shadows and the rest */
#define PORT_DRAW_HUD 3          /* the overlays, the text and the fade */
#define PORT_DRAW_ACTOR 4        /* one actor of the drawing list, until PORT_DRAW_ACTOR_END */
#define PORT_DRAW_ACTOR_END 5
extern void (*port_draw_mark_hook)(s32 kind, struct Actor *actor);

#endif
