/* The parts of a game frame (engine.h: engine_read, engine_draw, engine_logic). weemitt's frame driver
   (src/port/frame.c, port_game_update) runs the controller, the graphics and the game in one update. These run each
   part as an update of its own, with the same steps around it. At one whole frame per update weemitt's delta-time
   helpers all reduce to whole frames, so a part behaves as it does inside port_game_update. tests/run.py checks
   that a read, a draw and a logic frame give the same state as engine_step, and tests/sync.py plays these parts
   against the ROM in mupen64plus. */
#include "engine.h"

/* The game's side, in plain C types. */
void ControllerStep(void);
void GraphicsStep(void);
void GameStep(void);
void DeltaBeginUpdate(float dt);
void DeltaEndUpdate(void);
void port_input_update(unsigned short buttons);
extern unsigned short port_buttons;
extern signed char port_stick_x, port_stick_y;
extern unsigned char desiredFrameRate, newDesiredFrameRate, currentTask;

void engine_count_logic(Engine *e);

void engine_read(Engine *e, const EngineInput *in) {
    (void)e;
    port_buttons = in->buttons;
    port_stick_x = in->x;
    port_stick_y = in->y;
    DeltaBeginUpdate(1.0f);
    port_input_update(port_buttons);
    /* The scheduler takes the new frame rate when it sends the frame message (sched.c). */
    desiredFrameRate = newDesiredFrameRate ? newDesiredFrameRate : desiredFrameRate;
    ControllerStep();
    DeltaEndUpdate();
}

void engine_draw(Engine *e) {
    (void)e;
    DeltaBeginUpdate(1.0f);
    currentTask = 1;
    GraphicsStep();
    DeltaEndUpdate();
}

void engine_logic(Engine *e) {
    DeltaBeginUpdate(1.0f);
    GameStep();
    DeltaEndUpdate();
    engine_count_logic(e);
}
