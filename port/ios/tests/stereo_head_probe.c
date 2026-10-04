/* Head-tracked stereo's look (port/linux/game/stereo.c with
port/ios/host/host_stereo_head.c), frame by frame as the game runs it:
the player's look takes in the head's turn, the next frame begins with the
head's new pose, and the render orients its camera. The world the camera
shows must stay put in the room: its up is the room's up, and its forward
doesn't move, while the head pans level at any pitch. A world that drifts
in roll while the head pans is the bug from headset session 1. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "halo_stereo.h"
#include "host_stereo_head.h"

#define DEGREES (3.14159265f / 180.0f)

/* stereo.c's imports */
const char *config_string(const char *name)
{
	if (!strcmp(name, "display.stereo"))
		return "head";
	if (!strcmp(name, "input.turn"))
		return "smooth";
	return "";
}
double config_real(const char *name) { (void)name; return 30.0; }
void platform_video_drawable_size(int *width, int *height) { *width = *height = 0; }
void platform_log(const char *format, ...) { (void)format; }
int halo_cinematic_screen(void) { return 0; }
int halo_scripted_camera(void) { return 0; }
int halo_third_person_camera(void) { return 0; }

/* the device's pose this frame, columns right, up, back (ARKit's axes) */
static float pose[3][3];
static struct host_stereo_head head;

/* host_stereo.m's host_stereo_frame, reduced to the head */
void host_stereo_frame(struct halo_stereo_frame *frame)
{
	frame->eye_count = 2;
	frame->head_yaw = frame->head_pitch = frame->head_roll = 0.0f;
	host_stereo_head_turn(&head, pose[0], pose[1], pose[2], frame);
}

static void rotate(float v[3], int axis, float angle)
{
	float c = cosf(angle), s = sinf(angle), x = v[0], y = v[1], z = v[2];

	if (axis == 0) { v[1] = c * y - s * z; v[2] = s * y + c * z; }
	if (axis == 1) { v[0] = c * x + s * z; v[2] = -s * x + c * z; }
	if (axis == 2) { v[0] = c * x - s * y; v[1] = s * x + c * y; }
}

/* yaw about the room's up (left positive), then pitch about the head's right
(up positive), then roll about its back (left ear down positive) */
static void set_pose(float yaw, float pitch, float roll)
{
	int column, step;

	for (column = 0; column < 3; column++)
	{
		float *v = pose[column];

		v[0] = column == 0; v[1] = column == 1; v[2] = column == 2;
		for (step = 0; step < 3; step++)
		{
			if (step == 0) rotate(v, 2, roll);
			if (step == 1) rotate(v, 0, pitch);
			if (step == 2) rotate(v, 1, yaw);
		}
	}
}

/* the room's vector for a world vector, through the camera (the game's
axes: x forward, y left, z up) and the device */
static void world_in_room(const float forward[3], const float up[3], const float world[3], float room[3])
{
	float right[3] = {
		forward[1] * up[2] - forward[2] * up[1],
		forward[2] * up[0] - forward[0] * up[2],
		forward[0] * up[1] - forward[1] * up[0],
	};
	float x = right[0] * world[0] + right[1] * world[1] + right[2] * world[2];
	float y = up[0] * world[0] + up[1] * world[1] + up[2] * world[2];
	float z = -(forward[0] * world[0] + forward[1] * world[1] + forward[2] * world[2]);
	int i;

	for (i = 0; i < 3; i++)
		room[i] = x * pose[0][i] + y * pose[1][i] + z * pose[2][i];
}

static int failures;

/* pans the head level from 0 to 60 degrees left, then to 60 right, at a pitch
and roll, starting with the game's look level */
static void pan(const char *name, float pitch, float roll)
{
	float game_yaw = 0.0f, game_pitch = 0.0f, first_forward[3] = { 0 };
	float worst_tilt = 0.0f, worst_drift = 0.0f, worst_sideways = 0.0f;
	int frame;

	memset(&head, 0, sizeof(head));
	for (frame = 0; frame <= 90; frame++)
	{
		float yaw = (frame <= 30 ? frame * 2.0f : 60.0f - (frame - 30) * 2.0f) * DEGREES;
		float look_yaw, look_pitch;
		float forward[3], up[3], world_up[3] = { 0, 0, 1 }, world_forward[3] = { 1, 0, 0 };
		float room_up[3], room_forward[3];
		float tilt, drift;

		/* player_control: the turn the last frame read */
		if (halo_stereo_head_look(0, game_pitch, &look_yaw, &look_pitch))
		{
			game_yaw += look_yaw;
			game_pitch += look_pitch;
		}
		set_pose(yaw, pitch, roll);
		halo_stereo_frame_begin();
		/* the render's camera, from the look, oriented by the head */
		forward[0] = cosf(game_pitch) * cosf(game_yaw);
		forward[1] = cosf(game_pitch) * sinf(game_yaw);
		forward[2] = sinf(game_pitch);
		up[0] = -sinf(game_pitch) * cosf(game_yaw);
		up[1] = -sinf(game_pitch) * sinf(game_yaw);
		up[2] = cosf(game_pitch);
		halo_stereo_head_orient(forward, up);
		world_in_room(forward, up, world_up, room_up);
		world_in_room(forward, up, world_forward, room_forward);
		if (frame == 0)
			memcpy(first_forward, room_forward, sizeof(first_forward));
		/* the world's up against the room's, and the world's forward against
		where it was: both must hold */
		tilt = acosf(fminf(1.0f, room_up[1])) / DEGREES;
		drift = acosf(fminf(1.0f, room_forward[0] * first_forward[0] + room_forward[1] * first_forward[1] +
			room_forward[2] * first_forward[2])) / DEGREES;
		if (tilt > worst_tilt)
			worst_tilt = tilt;
		if (drift > worst_drift)
			worst_drift = drift;
		/* how far the world's up leans from the room's across the head's view:
		roll as the eye sees it */
		{
			float sideways = fabsf(room_up[0] * pose[0][0] + (room_up[1] - 1.0f) * pose[0][1] +
				room_up[2] * pose[0][2]);

			sideways = asinf(fminf(1.0f, sideways)) / DEGREES;
			if (sideways > worst_sideways)
				worst_sideways = sideways;
		}
	}
	printf("%-36s world tilt %.3f deg, drift %.3f deg, sideways %.3f deg\n", name, worst_tilt, worst_drift,
		worst_sideways);
	if (worst_tilt > 0.05f || worst_drift > 0.05f || worst_sideways > 0.05f)
	{
		printf("  FAIL: the world moves in the room while the head pans\n");
		failures++;
	}
}

int main(void)
{
	pan("level pan at 0 deg pitch", 0.0f, 0.0f);
	pan("level pan at -10 deg pitch", -10.0f * DEGREES, 0.0f);
	/* the views' forward (0.005, 0.009, -1) from the session-1 log */
	pan("level pan at the views' tilt", asinf(0.009f), 0.0f);
	pan("level pan at -10 deg, 5 deg roll", -10.0f * DEGREES, 5.0f * DEGREES);
	pan("level pan at 30 deg pitch", 30.0f * DEGREES, 0.0f);
	if (failures)
	{
		printf("stereo head probe: %d failed\n", failures);
		return 1;
	}
	printf("stereo head probe: PASS\n");
	return 0;
}
