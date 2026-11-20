/* Copied from the same author's Ballworld mod for the N64 game. Keep it in sync with the mod's file:
   replace this copy as a whole and do not edit it here. */
#ifndef BALLWORLD_H
#define BALLWORLD_H

/* Ballworld: every level is played as the ball alone. This header is shared by the N64 build and the PC port. */

/* 1 turns the mode on. The N64 build sets it at boot; the PC port sets it from its option. */
extern u8 ballworldOn;

/* After ProcessInput, before MoveBall and MoveHand: the ball's own buttons. */
void BallworldInputFrame(void);
/* Before DoBoss, after the frame's movement: the pound. */
void BallworldBeforeBoss(void);
/* After DoBoss, at the end of each game frame: hands the control to the ball. */
void BallworldEndFrame(void);
/* The PC port calls this when it turns the mode off: Glover gets back the health that the mode held. */
void BallworldLeave(void);
/* HurtHand calls this first. In the mode, a hit on Glover becomes a hit on the ball. */
void BallworldHandHurt(void);

/* The game calls this before ApplyAction(HAND_ROTOR) in ActivateCollectable, and before ApplyAction(HAND_RELEASE)
 * where a hit makes Glover let go of the ball: 1 means skip the action. */
s32 BallworldSkipAction(s32 action);
/* MainGameLoop calls this just before CheckPuzzles. */
void BallworldBeforePuzzles(void);
/* MaintainAction calls this before ReleaseBall in deep water: 1 means skip it. */
s32 BallworldSkipRelease(void);
/* Around the update of a boss that takes only a thrown ball (NMEProcessSpank, NMEProcessSelwynWhale), and around
 * DoBoss: in the mode the ball counts as thrown while they run. */
void BallworldThrownBegin(void);
void BallworldThrownEnd(void);
/* Around DoBoss: the bosses whose own code (DoCarnivalBoss, DoPrehistBoss) asks whether the glove holds the ball. */
void BallworldBossBegin(void);
void BallworldBossEnd(void);

#endif
