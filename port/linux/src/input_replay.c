/*
INPUT_REPLAY.C

Recording and replaying controller 1 (port 0), and a benchmark that times
the frames of a replay, so the same stretch of the game plays the same way
on every build (debug.input_record, debug.input_replay, debug.benchmark).

A recording is the controller's state each time it changes, stamped with the
game time: ticks since the map loaded, plus the fraction into the next tick.
A replay at 45 frames a second and one at 90 then press the same buttons at
the same moments of the game. Only reads while a map's game time runs are
recorded; before that (menus, loading) the real controller drives the game,
and a recording ends when the game time goes back (a new map). During a
replay the real controller is ignored while the game time runs. The look
stick turns the view by its deflection times each frame's length, so aim
can differ slightly between replays at different frame rates.

The file is text: the header line "halo input recording 1", then one line
per change: game time in ticks, the digital buttons (hex), the 8 analog
buttons and the 4 stick axes. A name without a slash is a file in the
current directory (the app's Documents on iOS), with ".input" added.

With debug.benchmark, a replay's frames are timed from the first recorded
state to the last; then the report goes to benchmark-<name>-<time>.txt next
to the recording, a summary to the log, and the game quits.
*/

#include "platform.h"
#include "port_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* source/game/game_time.c */
extern unsigned char game_time_initialized(void);
extern long game_time_get(void);
extern float game_time_get_tick_fraction(void);
/* xbox_kernel.c */
double halo_frame_trace_milliseconds(void);

#define RECORDING_HEADER "halo input recording 1"
/* how long after the last recorded state a replay ends, in ticks */
#define REPLAY_TAIL_TICKS 30.0

struct recorded_state
{
	double ticks;
	XINPUT_GAMEPAD pad;
};

static struct
{
	BOOL configured;
	char record_path[512], replay_path[512], replay_name[256];
	FILE *record_file;
	BOOL record_ended;
	double last_ticks;
	XINPUT_GAMEPAD last_pad;
	struct recorded_state *states;
	long state_count, next_state;
	XINPUT_GAMEPAD replay_pad;
	BOOL replay_finished;
	BOOL benchmark;
	double *frame_milliseconds;
	long frame_count, frame_capacity;
	double previous_frame;
	double started;
} replay;

static void recording_path(const char *name, char *path, size_t size)
{
	if (strchr(name, '/'))
		snprintf(path, size, "%s", name);
	else
		snprintf(path, size, "%s.input", name);
}

static void replay_load(void)
{
	FILE *file = fopen(replay.replay_path, "r");
	char line[256];
	long capacity = 0;

	if (!file)
	{
		platform_log("debug.input_replay: cannot open %s", replay.replay_path);
		replay.replay_finished = TRUE;
		return;
	}
	if (!fgets(line, sizeof(line), file) || strncmp(line, RECORDING_HEADER, strlen(RECORDING_HEADER)))
	{
		platform_log("debug.input_replay: %s isn't an input recording", replay.replay_path);
		fclose(file);
		replay.replay_finished = TRUE;
		return;
	}
	while (fgets(line, sizeof(line), file))
	{
		struct recorded_state state;
		unsigned int buttons, analog[8];
		int sticks[4];
		int index;

		memset(&state, 0, sizeof(state));
		if (sscanf(line, "%lf %x %u %u %u %u %u %u %u %u %d %d %d %d", &state.ticks, &buttons,
			&analog[0], &analog[1], &analog[2], &analog[3], &analog[4], &analog[5], &analog[6], &analog[7],
			&sticks[0], &sticks[1], &sticks[2], &sticks[3]) != 14)
			continue;
		state.pad.wButtons = (WORD)buttons;
		for (index = 0; index < 8; index++)
			state.pad.bAnalogButtons[index] = (BYTE)analog[index];
		state.pad.sThumbLX = (SHORT)sticks[0];
		state.pad.sThumbLY = (SHORT)sticks[1];
		state.pad.sThumbRX = (SHORT)sticks[2];
		state.pad.sThumbRY = (SHORT)sticks[3];
		if (replay.state_count == capacity)
		{
			capacity = capacity ? capacity * 2 : 1024;
			replay.states = realloc(replay.states, (size_t)capacity * sizeof(*replay.states));
		}
		replay.states[replay.state_count++] = state;
	}
	fclose(file);
	platform_log("debug.input_replay: %ld states from %s, %.1f s of game time", replay.state_count,
		replay.replay_path, replay.state_count ? replay.states[replay.state_count - 1].ticks / 30.0 : 0.0);
	if (!replay.state_count)
		replay.replay_finished = TRUE;
}

static void configure(void)
{
	const char *record = config_string("debug.input_record");
	const char *replay_name = config_string("debug.input_replay");

	replay.configured = TRUE;
	if (*record)
	{
		recording_path(record, replay.record_path, sizeof(replay.record_path));
		platform_log("debug.input_record: recording controller 1 to %s", replay.record_path);
	}
	if (*replay_name)
	{
		const char *base = strrchr(replay_name, '/');

		snprintf(replay.replay_name, sizeof(replay.replay_name), "%s", base ? base + 1 : replay_name);
		recording_path(replay_name, replay.replay_path, sizeof(replay.replay_path));
		replay.benchmark = config_boolean("debug.benchmark");
		replay_load();
	}
}

/* the game time, in ticks with the fraction into the next, or a negative
number while no map's game time runs */
static double game_ticks(void)
{
	if (!game_time_initialized())
		return -1.0;
	return (double)game_time_get() + (double)game_time_get_tick_fraction();
}

static void record(const XINPUT_GAMEPAD *pad, double ticks)
{
	int index;

	if (replay.record_ended)
		return;
	if (!replay.record_file)
	{
		replay.record_file = fopen(replay.record_path, "w");
		if (!replay.record_file)
		{
			platform_log("debug.input_record: cannot write %s", replay.record_path);
			replay.record_ended = TRUE;
			return;
		}
		fprintf(replay.record_file, "%s\n", RECORDING_HEADER);
		replay.last_ticks = ticks;
		memset(&replay.last_pad, 0xff, sizeof(replay.last_pad));
	}
	if (ticks + 1.0 < replay.last_ticks)
	{
		platform_log("debug.input_record: the game time went back (a new map); the recording ends");
		fclose(replay.record_file);
		replay.record_file = NULL;
		replay.record_ended = TRUE;
		return;
	}
	replay.last_ticks = ticks;
	if (!memcmp(pad, &replay.last_pad, sizeof(*pad)))
		return;
	replay.last_pad = *pad;
	fprintf(replay.record_file, "%.4f %04x", ticks, (unsigned int)pad->wButtons);
	for (index = 0; index < 8; index++)
		fprintf(replay.record_file, " %u", (unsigned int)pad->bAnalogButtons[index]);
	fprintf(replay.record_file, " %d %d %d %d\n", pad->sThumbLX, pad->sThumbLY, pad->sThumbRX, pad->sThumbRY);
	fflush(replay.record_file);
}

static void play(XINPUT_GAMEPAD *pad, double ticks)
{
	while (replay.next_state < replay.state_count && replay.states[replay.next_state].ticks <= ticks)
		replay.replay_pad = replay.states[replay.next_state++].pad;
	*pad = replay.replay_pad;
	if (replay.next_state == replay.state_count &&
		ticks > replay.states[replay.state_count - 1].ticks + REPLAY_TAIL_TICKS)
		replay.replay_finished = TRUE;
}

/* controller 1's state as the game reads it (xinput_sdl.c's XInputGetState):
recorded, or replaced by the replay's */
void input_replay_filter(XINPUT_GAMEPAD *pad)
{
	double ticks;

	if (!replay.configured)
		configure();
	if (!*replay.record_path && !*replay.replay_path)
		return;
	ticks = game_ticks();
	if (ticks < 0.0)
		return;
	if (*replay.record_path)
		record(pad, ticks);
	if (*replay.replay_path && !replay.replay_finished)
		play(pad, ticks);
}

/* ---------- the benchmark */

static int compare_doubles(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static void benchmark_report(long width, long height)
{
	double *sorted, total = 0.0;
	static const double budgets[] = { 1000.0 / 90.0, 1000.0 / 60.0, 1000.0 / 45.0, 1000.0 / 30.0 };
	long over[4] = { 0 };
	long index;
	char path[600], stamp[32];
	time_t now = time(NULL);
	FILE *file;

	if (replay.frame_count < 2)
	{
		platform_log("debug.benchmark: no frames timed");
		return;
	}
	sorted = malloc((size_t)replay.frame_count * sizeof(*sorted));
	memcpy(sorted, replay.frame_milliseconds, (size_t)replay.frame_count * sizeof(*sorted));
	qsort(sorted, (size_t)replay.frame_count, sizeof(*sorted), compare_doubles);
	for (index = 0; index < replay.frame_count; index++)
	{
		int budget;

		total += sorted[index];
		for (budget = 0; budget < 4; budget++)
			if (sorted[index] > budgets[budget] * 1.05)
				over[budget]++;
	}
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
	snprintf(path, sizeof(path), "benchmark-%s-%s.txt", replay.replay_name, stamp);
	file = fopen(path, "w");
	for (index = 0; index < 2; index++)
	{
		FILE *out = index ? file : NULL;
		char text[1024];

		snprintf(text, sizeof(text),
			"benchmark %s: %ld frames in %.2f s, %.1f frames a second; render %ldx%ld\n"
			"frame ms: average %.2f, median %.2f, 95th %.2f, 99th %.2f, worst %.2f\n"
			"frames over 90 Hz %ld, 60 Hz %ld, 45 Hz %ld, 30 Hz %ld (of %ld)\n",
			replay.replay_name, replay.frame_count, total / 1000.0, 1000.0 * (double)replay.frame_count / total,
			width, height, total / (double)replay.frame_count, sorted[replay.frame_count / 2],
			sorted[replay.frame_count * 95 / 100], sorted[replay.frame_count * 99 / 100], sorted[replay.frame_count - 1],
			over[0], over[1], over[2], over[3], replay.frame_count);
		if (out)
			fputs(text, out);
		else
			platform_log("%s", text);
	}
	if (file)
	{
		fputs("frame ms, in order:\n", file);
		for (index = 0; index < replay.frame_count; index++)
			fprintf(file, "%.3f\n", replay.frame_milliseconds[index]);
		fclose(file);
		platform_log("debug.benchmark: report in %s", path);
	}
	free(sorted);
}

/* each presented frame (d3d8_device.c), with the render size */
void input_replay_frame(long width, long height)
{
	double now;

	if (!replay.benchmark)
		return;
	now = halo_frame_trace_milliseconds();
	if (replay.replay_finished)
	{
		benchmark_report(width, height);
		platform_log("debug.benchmark: the replay ended; quitting");
		exit(EXIT_SUCCESS);
	}
	if (replay.next_state == 0)
		return;
	if (replay.previous_frame > 0.0)
	{
		if (replay.frame_count == replay.frame_capacity)
		{
			replay.frame_capacity = replay.frame_capacity ? replay.frame_capacity * 2 : 4096;
			replay.frame_milliseconds = realloc(replay.frame_milliseconds,
				(size_t)replay.frame_capacity * sizeof(*replay.frame_milliseconds));
		}
		replay.frame_milliseconds[replay.frame_count++] = now - replay.previous_frame;
	}
	replay.previous_frame = now;
}
