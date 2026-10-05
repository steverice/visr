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
complement of the game's seed at the first frame. render_player_frame_stereo
(render.c) saves and restores the seed between its eyes inside the bracket,
so the eyes still draw the same render numbers.

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
