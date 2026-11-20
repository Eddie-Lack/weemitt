/* How the buttons reach the game's controller reads. The game reads the controller once per original frame. The
   platform layer sets port_buttons before each update, and the latch makes the reads see every press. Plain C. */
#ifndef PORT_INPUTLATCH_H
#define PORT_INPUTLATCH_H

extern unsigned short port_buttons_latched; /* pressed since the latest read, and that read saw them up */
extern unsigned short port_buttons_read;    /* what the latest read returned */
extern unsigned short port_buttons_up;      /* seen up in an update since the latest read */

/* Called at the start of every update with the buttons that update has. */
void port_input_update(unsigned short buttons);
/* The game's read of the controller: returns the button word and starts the next interval. */
unsigned short port_input_read(unsigned short buttons);

#endif
