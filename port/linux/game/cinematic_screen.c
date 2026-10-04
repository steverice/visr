/*
CINEMATIC_SCREEN.C

What stereo's cutscene screen reads from the game (port/linux/src/halo_stereo.h):
whether the letterbox is in, which makes a cutscene the 3D film on a 16:9
screen (stereo.c).

The letterbox, not cinematic_in_progress, marks the cutscene: the bars are
what frame Bungie's 16:9 composition, and scripts toggle them on their own
(cinematic_show_letterbox).
*/

#include "cseries.h"
#include "cutscene/cinematics.h"

int halo_cinematic_screen(void)
{
	return cinematic_globals && cinematic_globals->show_letterbox;
}
