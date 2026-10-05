/* The render's own local random sequence (port/linux/game/render_random.c,
included, with a stand-in for random_math.c's seed).

Rendering draws local random numbers in amounts that depend on the frame
(what it draws, and the audio device's timing), and the game's tick draws
from the same local sequence for things that reach the simulation. The probe
checks that the game's sequence, with frames drawn between its ticks, comes
out as it does with no frames at all, however many numbers each frame takes;
that the render's own sequence carries on from frame to frame rather than
repeating; that a nested bracket swaps once; that a stereo frame's restore
of the seed between eyes (render.c) repeats the eye's numbers and leaves the
game's alone; and that an unmatched end changes nothing. */
#include <assert.h>
#include <stdio.h>

#define RENDER_RANDOM_PROBE 1

static unsigned long local_seed;

unsigned long *get_global_local_random_seed_address(void)
{
	return &local_seed;
}

#include "../../linux/game/render_random.c"

/* random_math.c's seed_random, in 32 bits as on the guest */
static unsigned short draw(void)
{
	local_seed = (unsigned long)(unsigned int)(local_seed * 1664525UL + 1013904223UL);
	return (unsigned short)(local_seed >> 16);
}

/* a tick's draws: a fixed count, as the game's own consumers make */
static unsigned long tick_draws(int tick)
{
	unsigned long sum = 0;
	int i;

	for (i = 0; i < 5 + tick % 3; i++)
		sum = sum * 31 + draw();
	return sum;
}

int main(void)
{
	unsigned long reference[64];
	unsigned short first_frame[8];
	unsigned short later_frame[8];
	unsigned short eye[2][4];
	unsigned long game_before;
	int tick;
	int i;

	/* the game's sequence with no frames drawn */
	local_seed = 0x12345678UL;
	for (tick = 0; tick < 64; tick++)
		reference[tick] = tick_draws(tick);

	/* the same ticks, with frames between them taking 0 to 12 numbers each,
	two or three frames a tick */
	local_seed = 0x12345678UL;
	for (tick = 0; tick < 64; tick++)
	{
		int frame;

		for (frame = 0; frame < 2 + tick % 2; frame++)
		{
			halo_render_random_begin();
			for (i = 0; i < (tick * 7 + frame * 5) % 13; i++)
				draw();
			halo_render_random_end();
		}
		assert(tick_draws(tick) == reference[tick]);
	}

	/* the render's sequence carries on: a later frame does not repeat the first's */
	halo_render_random_begin();
	for (i = 0; i < 8; i++)
		first_frame[i] = draw();
	halo_render_random_end();
	halo_render_random_begin();
	for (i = 0; i < 8; i++)
		later_frame[i] = draw();
	halo_render_random_end();
	for (i = 0; i < 8 && first_frame[i] == later_frame[i]; i++)
		;
	assert(i < 8);

	/* a nested bracket (a frame inside a frame) swaps once, at the outer end */
	game_before = local_seed;
	halo_render_random_begin();
	draw();
	halo_render_random_begin();
	draw();
	halo_render_random_end();
	assert(local_seed != game_before);
	draw();
	halo_render_random_end();
	assert(local_seed == game_before);

	/* a stereo frame: render.c saves the seed before eye 0 and puts it back
	before eye 1, so both eyes draw the same numbers, and the game's seed
	comes back after the frame */
	game_before = local_seed;
	halo_render_random_begin();
	{
		unsigned long saved = local_seed;
		int e;

		for (e = 0; e < 2; e++)
		{
			local_seed = saved;
			for (i = 0; i < 4; i++)
				eye[e][i] = draw();
		}
	}
	halo_render_random_end();
	for (i = 0; i < 4; i++)
		assert(eye[0][i] == eye[1][i]);
	assert(local_seed == game_before);

	/* an end without a begin changes nothing */
	halo_render_random_end();
	assert(local_seed == game_before);
	halo_render_random_begin();
	halo_render_random_end();
	assert(local_seed == game_before);

	puts("PASS: render random: the game's local sequence ignores the frames between ticks, "
		"the render's carries on, nested and stereo brackets, unmatched ends");
	return 0;
}
