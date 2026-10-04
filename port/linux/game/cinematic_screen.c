/*
CINEMATIC_SCREEN.C

What stereo's cutscene screen reads from the game (port/linux/src/halo_stereo.h):
whether the letterbox is in, which makes a cutscene the 3D film on a 16:9
screen (stereo.c), and the script fade, which tints the immersive space
around the screen (port/ios/host/gpu_metal.m).

The letterbox, not cinematic_in_progress, marks the cutscene: the bars are
what frame Bungie's 16:9 composition, and scripts toggle them on their own
(cinematic_show_letterbox). Only script fades (fade_in, fade_out) tint the
room: the screen flashes of damage, pickups and telefrags stay on the game's
picture.
*/

#include "cseries.h"
#include "cutscene/cinematics.h"
#include "effects/player_effects.h"
#include "game/game.h"
#include "main/console.h"
#include "math/periodic_functions.h"
#include "math/real_math.h"

int halo_cinematic_screen(void)
{
	return cinematic_globals && cinematic_globals->show_letterbox;
}

/* The intensity player_effect_get_screen_flash gives the script fade (a
cosine over the fade's ticks, inverted while fading in), computed here
without that function, which ends a finished fade in the game state, and
with the tick fraction, so the room changes smoothly at the display's
rate. */
void halo_screen_fade(float rgb_intensity[4])
{
	real_rgb_color color;
	long start_time;
	short ticks;
	boolean fading_out;
	long elapsed_ticks;
	real intensity;

	rgb_intensity[0] = rgb_intensity[1] = rgb_intensity[2] = rgb_intensity[3] = 0.0f;
	/* the game draws no fade while the console is up */
	if (console_is_active() || !player_effect_get_screen_fade(&color, &start_time, &ticks, &fading_out))
		return;
	elapsed_ticks = game_time_get() - start_time;
	if (!fading_out && elapsed_ticks > ticks)
		return;
	intensity = ticks > 0
		? transition_function_evaluate(
			_transition_function_cosine,
			PIN(((real)elapsed_ticks + game_time_get_tick_fraction()) / ticks, 0.0f, 1.0f))
		: 1.0f;
	if (!fading_out)
		intensity = 1.0f - intensity;
	rgb_intensity[0] = color.red;
	rgb_intensity[1] = color.green;
	rgb_intensity[2] = color.blue;
	rgb_intensity[3] = PIN(intensity, 0.0f, 1.0f);
}
