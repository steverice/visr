/*
RENDER_RANDOM.C

The render's own local random sequence.

The game has two random sequences (random_math.c): the global one, which the
simulation draws from and which rendering may not touch (main_game_render
locks it), and the local one, meant for what the player sees. Rendering
draws local numbers in amounts that depend on the frame: the particle
systems it draws (particle_systems_render), the camera shake
(player_effect_get_camera_effect_matrix), and the looping sounds whose next
permutation sound_render picks once the audio device reports the last one
finished, which follows the device's real time even with
debug.fixed_timestep. The game's tick draws from the same sequence
(effects, particles, decals, contrails, sounds), and some of what it draws
there reaches the simulation. So two b30 runs that differed only in
display.model_lod, or in nothing but the audio device's timing, drew
different local numbers in the tick within the first minute, and objects
then moved differently (a bullet's spawn point at tick 1341 in one pair).

main_game_render and main_pregame_render bracket their work with
halo_render_random_begin and halo_render_random_end, which swap the render's
own seed in and the game's back out, so the game's local sequence advances
only by the game's own draws. The render's sequence starts as the bitwise
complement of the game's seed at the first frame.

A stereo frame (render_player_frame_stereo, render.c) draws each eye from
the same render seed, so a lightning bolt has one shape in both eyes (a
zoomed frame's one zoomed pass draws as mono does, without these calls):
halo_render_random_stereo_pass puts the seed eye 0
started from back before each later pass. Some draws happen in eye 0 alone
(weather's spawns, the fog screen's wind and layer offsets, a new particle's
first sprite), so the seed a later pass leaves behind lacks them; were the
frame to leave the seed there, a last pass that drew nothing would start
every frame from the same seed, and eye 0 would repeat the same weather,
sprites and fog forever. halo_render_random_stereo_end, after the passes,
leaves the seed where eye 0 left it.

port/ios/tests/render_random_probe.c includes this file with a stand-in for
the seed.
*/

#ifndef RENDER_RANDOM_PROBE
#include "cseries.h"
#include "math/real_math.h"
#endif

static unsigned long render_seed;
static unsigned long game_seed;
static int render_seed_started;
static int depth;

void halo_render_random_begin(void)
{
	unsigned long *seed = get_global_local_random_seed_address();

	if (depth++ != 0)
		return;
	if (!render_seed_started)
	{
		render_seed = ~*seed;
		render_seed_started = 1;
	}
	game_seed = *seed;
	*seed = render_seed;
}

void halo_render_random_end(void)
{
	unsigned long *seed = get_global_local_random_seed_address();

	if (depth <= 0 || --depth != 0)
		return;
	render_seed = *seed;
	*seed = game_seed;
}

static unsigned long stereo_start_seed;
static unsigned long stereo_eye_0_end_seed;

void halo_render_random_stereo_pass(short eye)
{
	unsigned long *seed = get_global_local_random_seed_address();

	if (eye == 0)
	{
		stereo_start_seed = *seed;
		stereo_eye_0_end_seed = *seed;
		return;
	}
	if (eye == 1)
		stereo_eye_0_end_seed = *seed;
	*seed = stereo_start_seed;
}

void halo_render_random_stereo_end(void)
{
	*get_global_local_random_seed_address() = stereo_eye_0_end_seed;
}
