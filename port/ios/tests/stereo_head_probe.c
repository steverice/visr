/* Head-tracked stereo's look (port/linux/game/stereo.c with
port/ios/host/host_stereo_head.c), frame by frame as the game runs it:
the player's look takes in the head's turn, the next frame begins with the
head's new pose, and the render orients its camera. The world the camera
shows must stay put in the room: its up is the room's up, and its forward
doesn't move, while the head pans level at any pitch. A world that drifts
in roll while the head pans is the bug from headset session 1.

And the right stick's turn (input.turn): snaps by default, smooth turning at
input.smooth_turn_speed whatever the frame rate, none at all for "off", and
the comfort vignette's easing. stereo.c is included, not linked, so each
check can read the settings afresh. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "halo_stereo.h"
#include "host_stereo_head.h"

/* the settings each check reads (NULL: the setting is absent) */
static const char *setting_turn = "smooth";
static double setting_snap_angle = 30.0, setting_smooth_turn_speed = 120.0;
static int setting_comfort_vignette;
static int unrecognized_logged;
static char last_log[256];

/* stereo.c's imports */
const char *config_string(const char *name)
{
	if (!strcmp(name, "display.stereo"))
		return "head";
	if (!strcmp(name, "input.turn"))
		return setting_turn;
	return "";
}
double config_real(const char *name)
{
	if (!strcmp(name, "input.smooth_turn_speed"))
		return setting_smooth_turn_speed;
	return setting_snap_angle;
}
int config_boolean(const char *name) { (void)name; return setting_comfort_vignette; }
void platform_video_drawable_size(int *width, int *height) { *width = *height = 0; }
void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(last_log, sizeof(last_log), format, arguments);
	va_end(arguments);
	if (strstr(last_log, "is not recognized; using snap"))
		unrecognized_logged++;
}
int halo_cinematic_screen(void) { return 0; }
int halo_scripted_camera(void) { return 0; }
int halo_third_person_camera(void) { return 0; }
void halo_screen_commit_stereo_scale(void) {}

#include "../../linux/game/stereo.c"

#define DEGREES (3.14159265f / 180.0f)

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

static void rotate_axis(float v[3], int axis, float angle)
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
			if (step == 0) rotate_axis(v, 2, roll);
			if (step == 1) rotate_axis(v, 0, pitch);
			if (step == 2) rotate_axis(v, 1, yaw);
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

/* the settings read afresh, as if the game had just started */
static void restart(const char *turn, double snap_angle, double smooth_turn_speed, int comfort_vignette)
{
	setting_turn = turn;
	setting_snap_angle = snap_angle;
	setting_smooth_turn_speed = smooth_turn_speed;
	setting_comfort_vignette = comfort_vignette;
	turn_mode = -1;
	stereo_mode = -1;
	snap_pending = smooth_yaw = 0.0f;
	snap_armed = 1;
	vignette_strength = 0.0f;
	unrecognized_logged = 0;
	memset(&head, 0, sizeof(head));
	set_pose(0.0f, 0.0f, 0.0f);
	/* a frame with the Compositor's eyes: the head drives the view */
	halo_stereo_frame_begin();
}

/* the game's response curve (globals.globals' look_function), as
player_control.c evaluates it */
static float response(float stick)
{
	static const float curve[6] = { 0.0f, 0.05f, 0.1f, 0.25f, 0.58f, 1.0f };
	float x = fminf(fabsf(stick), 1.0f) * 5.0f;
	int low = (int)x < 5 ? (int)x : 5, high = low < 5 ? low + 1 : 5;
	float value = (curve[high] - curve[low]) * (x - low) + curve[low];

	return stick < 0.0f ? -value : value;
}

/* holds the stick at a yaw for a time at a frame rate, as the game's loop
does (the stick, then the look, then the next frame); returns the look's
turn in degrees, and the vignette's strongest */
static float hold_stick(float stick, float seconds, float frames_per_second, float *vignette_peak)
{
	float turned = 0.0f, peak = 0.0f;
	int frame, frames = (int)lroundf(seconds * frames_per_second);

	for (frame = 0; frame < frames; frame++) {
		float yaw = stick, pitch = 0.3f, look_yaw, look_pitch;

		halo_stereo_stick_look(0, response(stick), 1.0f / frames_per_second, &yaw, &pitch);
		if (yaw != 0.0f || pitch != 0.0f) {
			printf("  FAIL: the game's own stick turn wasn't zeroed (yaw %f, pitch %f)\n", yaw, pitch);
			failures++;
		}
		if (halo_stereo_head_look(0, 0.0f, &look_yaw, &look_pitch))
			turned += look_yaw;
		halo_stereo_frame_begin();
		if (halo_stereo_vignette() > peak)
			peak = halo_stereo_vignette();
	}
	if (vignette_peak)
		*vignette_peak = peak;
	return turned / DEGREES;
}

static void check(int passed, const char *what)
{
	printf("  %s: %s\n", passed ? "ok" : "FAIL", what);
	if (!passed)
		failures++;
}

static void turning(void)
{
	float turned, peak, rate;

	printf("right-stick turning (input.turn):\n");
	restart(NULL, 30.0, 120.0, 0);
	check(turn_mode == TURN_SNAP, "absent: snap");
	restart("", 30.0, 120.0, 0);
	check(turn_mode == TURN_SNAP, "empty: snap");
	restart("spin", 30.0, 120.0, 0);
	check(turn_mode == TURN_SNAP && unrecognized_logged == 1, "unrecognized: snap, logged once");
	check(strstr(last_log, "input.turn snap, input.snap_angle 30.0, input.smooth_turn_speed 120.0, "
		"input.comfort_vignette false") != NULL, "the settings line logs the effective settings");
	hold_stick(1.0f, 0.1f, 90.0f, NULL);
	check(unrecognized_logged == 1, "unrecognized: not logged again");

	/* snap: one flick, one snap, however long it's held; back inside the
	release and out again, another */
	restart("snap", 30.0, 120.0, 0);
	turned = hold_stick(1.0f, 1.0f, 90.0f, NULL);
	check(fabsf(turned - 30.0f) < 0.001f, "snap: a held flick turns 30 degrees once");
	hold_stick(0.0f, 0.1f, 90.0f, NULL);
	turned = hold_stick(-0.8f, 0.5f, 30.0f, NULL);
	check(fabsf(turned + 30.0f) < 0.001f, "snap: a flick the other way turns -30 degrees");
	turned = hold_stick(0.6f, 0.5f, 90.0f, NULL);
	check(turned == 0.0f, "snap: under the flick threshold, no turn");
	restart("snap", 1.0, 120.0, 0);
	check(fabsf(hold_stick(1.0f, 0.1f, 90.0f, NULL) - 5.0f) < 0.001f, "snap: input.snap_angle 1 clamps to 5");
	restart("snap", 400.0, 120.0, 0);
	check(fabsf(hold_stick(1.0f, 0.1f, 90.0f, NULL) - 180.0f) < 0.001f, "snap: input.snap_angle 400 clamps to 180");

	restart("off", 30.0, 120.0, 0);
	turned = hold_stick(1.0f, 1.0f, 90.0f, NULL) + hold_stick(0.0f, 0.1f, 90.0f, NULL) + hold_stick(-1.0f, 1.0f, 90.0f, NULL);
	check(turned == 0.0f, "off: the stick never turns the look");

	/* smooth: input.smooth_turn_speed a second at full deflection, at 30, 60
	and 90 frames a second; the curve's share of it part way over */
	restart("smooth", 30.0, 120.0, 0);
	rate = hold_stick(1.0f, 1.0f, 90.0f, &peak);
	check(fabsf(rate - 120.0f) < 1.2f, "smooth: full deflection turns 120 degrees in a second at 90 Hz");
	check(peak == 0.0f, "smooth: no vignette unless input.comfort_vignette");
	check(fabsf(hold_stick(-1.0f, 1.0f, 30.0f, NULL) + 120.0f) < 1.2f, "smooth: -120 degrees in a second at 30 Hz");
	check(fabsf(hold_stick(1.0f, 1.0f, 60.0f, NULL) - 120.0f) < 1.2f, "smooth: 120 degrees in a second at 60 Hz");
	check(fabsf(hold_stick(0.5f, 1.0f, 90.0f, NULL) - 120.0f * response(0.5f)) < 0.1f,
		"smooth: half way over turns the curve's share (0.175)");
	restart("smooth", 30.0, 45.0, 0);
	check(fabsf(hold_stick(1.0f, 1.0f, 90.0f, NULL) - 45.0f) < 0.45f, "smooth: input.smooth_turn_speed 45");
	printf("  smooth at 120 deg/s: %.3f degrees in a second at 90 Hz\n", rate);

	/* the vignette: only smooth with input.comfort_vignette, as strong as
	the turn, gone with the turn and for snaps */
	restart("smooth", 30.0, 120.0, 1);
	hold_stick(1.0f, 0.5f, 90.0f, &peak);
	check(fabsf(peak - 1.0f) < 0.001f && fabsf(halo_stereo_vignette() - 1.0f) < 0.001f,
		"vignette: full while turning at full speed");
	hold_stick(0.0f, 0.3f, 90.0f, NULL);
	check(halo_stereo_vignette() == 0.0f, "vignette: gone 0.3 s after the turn stops");
	hold_stick(0.5f, 0.5f, 90.0f, &peak);
	check(fabsf(peak - response(0.5f)) < 0.001f, "vignette: half way over, the turn's share");
	restart("snap", 30.0, 120.0, 1);
	hold_stick(1.0f, 0.5f, 90.0f, &peak);
	check(peak == 0.0f, "vignette: none for snaps");
}

/* the vignette's easing, alone */
static void vignette_easing(void)
{
	float strength = 0.0f;
	int step;

	printf("comfort vignette easing:\n");
	for (step = 0; step < 8; step++)
		strength = halo_stereo_vignette_ease(strength, 1.0f, 1.0f / 90.0f);
	check(fabsf(strength - 8.0f / 9.0f) < 0.001f, "in: 8/9 after 8 frames at 90 Hz");
	strength = halo_stereo_vignette_ease(strength, 1.0f, 1.0f / 90.0f);
	check(strength > 0.999f, "in: full after 0.1 s");
	strength = halo_stereo_vignette_ease(strength, 1.0f, 1.0f / 90.0f);
	strength = halo_stereo_vignette_ease(strength, 1.0f, 1.0f / 90.0f);
	check(strength == 1.0f, "in: never past the target");
	strength = halo_stereo_vignette_ease(1.0f, 0.0f, 0.1f);
	check(fabsf(strength - 0.5f) < 0.0001f, "out: half after 0.1 s");
	strength = halo_stereo_vignette_ease(strength, 0.0f, 0.1f);
	check(strength == 0.0f, "out: gone after 0.2 s");
	check(fabsf(halo_stereo_vignette_ease(0.0f, 0.4f, 1.0f) - 0.4f) < 0.0001f, "in: to the turn's share, no further");
	check(fabsf(halo_stereo_vignette_ease(1.0f, 0.4f, 0.05f) - 0.75f) < 0.0001f, "out: toward the turn's share");
	check(halo_stereo_vignette_ease(0.3f, 2.0f, 0.0f) == 0.3f, "no time: no change");
	check(halo_stereo_vignette_ease(0.0f, 2.0f, 1.0f) == 1.0f, "a share over 1: full");
}

/* straight up the yaw can't be read: a head at 89.9 degrees whose
ill-conditioned yaw swings by 170 degrees in a frame must not turn the look */
static void pole(void)
{
	struct halo_stereo_frame frame;

	memset(&head, 0, sizeof(head));
	memset(&frame, 0, sizeof(frame));
	set_pose(0.0f, 89.9f * DEGREES, 0.0f);
	host_stereo_head_turn(&head, pose[0], pose[1], pose[2], &frame);
	set_pose(170.0f * DEGREES, 89.9f * DEGREES, 0.0f);
	host_stereo_head_turn(&head, pose[0], pose[1], pose[2], &frame);
	printf("%-36s head yaw %.3f deg\n", "a swing at 89.9 deg pitch", frame.head_yaw / DEGREES);
	if (fabsf(frame.head_yaw) > 1e-6f)
	{
		printf("  FAIL: the head turned the look at the pole\n");
		failures++;
	}
}

int main(void)
{
	pole();
	pan("level pan at 0 deg pitch", 0.0f, 0.0f);
	pan("level pan at -10 deg pitch", -10.0f * DEGREES, 0.0f);
	/* the views' forward (0.005, 0.009, -1) from the session-1 log */
	pan("level pan at the views' tilt", asinf(0.009f), 0.0f);
	pan("level pan at -10 deg, 5 deg roll", -10.0f * DEGREES, 5.0f * DEGREES);
	pan("level pan at 30 deg pitch", 30.0f * DEGREES, 0.0f);
	turning();
	vignette_easing();
	if (failures)
	{
		printf("stereo head probe: %d failed\n", failures);
		return 1;
	}
	printf("stereo head probe: PASS\n");
	return 0;
}
