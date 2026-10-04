/*
CINEMATIC_SCREEN.C

What stereo's cutscene screen reads from the game (port/linux/src/halo_stereo.h):
whether the letterbox is in, which makes a cutscene the 3D film on a 16:9
screen (stereo.c); whether the camera is scripted or third person, which
put the view on the screen too; and the script fade, which tints the immersive space
around the screen (port/ios/host/gpu_metal.m).

The letterbox, not cinematic_in_progress, marks the cutscene: the bars are
what frame Bungie's 16:9 composition, and scripts toggle them on their own
(cinematic_show_letterbox). Only script fades (fade_in, fade_out) tint the
room: the screen flashes of damage, pickups and telefrags stay on the game's
picture.
*/

#include "cseries.h"
#include "camera/director.h"
#include "cutscene/cinematics.h"
#include "game/players.h"
#include "effects/player_effects.h"
#include "game/game.h"
#include "main/console.h"
#include "math/periodic_functions.h"
#include "math/real_math.h"

int halo_cinematic_screen(void)
{
	return cinematic_globals && cinematic_globals->show_letterbox;
}

/* A camera the script drives: the script has taken the camera from the
player (player_camera_control false: a first-person moment the player can't
look around in, as a10's Chief climbing out of the cryo pod), or it runs a
scripted camera (camera_control, director_script_camera). The game's own
flags, rather than whether the player's input is off: input is also off
where the player still looks through their own eyes (a10's "use the right
stick to look around" in the pod, with camera control on). */
int halo_scripted_camera(void)
{
	return player_control_camera_control_disabled() ||
		(director_camera_scripted && *director_camera_scripted) ||
		director_peek_perspective(0) == _director_perspective_scripted;
}

/* the director's third-person camera (following_camera_update): a vehicle
seat whose camera isn't first person */
int halo_third_person_camera(void)
{
	return director_peek_perspective(0) == _director_perspective_third_person;
}

/* The intensity player_effect_get_screen_flash gives the script fade (a
cosine over the fade's ticks, inverted while fading in), computed here
without that function, which ends a finished fade in the game state. The
fade is active exactly when the game's is (whole ticks); its intensity is
taken at the picture's time, the tick before this one plus the fraction, as
the render's interpolation shows objects (render_interpolation_game_time_
sec), so the room changes smoothly at the display's rate and in step with
the picture. */
int halo_screen_fade(float tick_fraction, float rgb_intensity[4])
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
		return 0;
	elapsed_ticks = game_time_get() - start_time;
	if (!fading_out && elapsed_ticks > ticks)
		return 0;
	intensity = ticks > 0
		? transition_function_evaluate(
			_transition_function_cosine,
			PIN(((real)elapsed_ticks - 1.0f + PIN(tick_fraction, 0.0f, 1.0f)) / ticks, 0.0f, 1.0f))
		: 1.0f;
	if (!fading_out)
		intensity = 1.0f - intensity;
	rgb_intensity[0] = color.red;
	rgb_intensity[1] = color.green;
	rgb_intensity[2] = color.blue;
	rgb_intensity[3] = PIN(intensity, 0.0f, 1.0f);
	return 1;
}
