/* See inputlatch.h. Between two reads of the game an update can see a button down and the next update see it up.
   Without the latch the read would miss the press. */
#include "inputlatch.h"

unsigned short port_buttons_latched;
unsigned short port_buttons_read;
unsigned short port_buttons_up;

void port_input_update(unsigned short buttons) {
    /* A button that goes down after a read counts as held at the next read, even if it is up again by then. A
       button that the latest read saw held is not latched: the next read sees its release. */
    port_buttons_latched |= buttons & ~port_buttons_read;
    port_buttons_up |= ~buttons;
}

unsigned short port_input_read(unsigned short buttons) {
    unsigned short held = buttons | port_buttons_latched;
    /* A button that the last read saw held, that went up and down again since, and that is held now. One read cannot
       show the release and the press. This read shows the release, and the next one the press. At one update per
       read it never happens. */
    unsigned short again = held & port_buttons_read & port_buttons_up;

    port_buttons_read = held & ~again;
    port_buttons_latched = again;
    port_buttons_up = 0;
    return port_buttons_read;
}
