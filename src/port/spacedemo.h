/* The Space demo (spacedemo.c): a build that starts in Space 1 and joins Space 1, 2 and 3 into one run, with a
   screen of the times at the end. It is off in the normal build. */
#ifndef SPACEDEMO_H
#define SPACEDEMO_H

struct Platform;

/* The game mode of the screen of times. The game's own modes go up to 15. */
#define SPACE_DEMO_RESULTS_MODE 16

/* 1 in the Space demo. The platform layer sets it before the game starts. */
extern s32 spaceDemo;

/* 1 when the player chose the easy difficulty's stick direction on the ball for every difficulty (menu). */
extern s32 easyBallControls;

/* How much faster than the original the camera follows the glove (menu): 1.0 is the original. */
extern f32 cameraFollowSpeed;

void SpaceDemoStart(void);
void SpaceDemoDoor(struct Platform *platform);
void SpaceDemoNextLevel(void);
void SpaceDemoResults(void);
void SpaceDemoRestart(void);

#endif
