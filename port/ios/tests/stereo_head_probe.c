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
check can read the settings afresh.

And the film's (a cutscene on the screen): the mapping it shares with the
3D TV (halo_stereo_tv_eyes) puts infinity a share of the eye separation
behind the screen and the convergence distance on its surface, and the
held reason keeps the cutscene's framing through the hold; any script fade
over the picture covers the cut. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "halo_stereo.h"
#include "host_stereo_head.h"
#include "host_stereo_vignette.h"

/* the eye shader's vignette mask, as C */
HOST_STEREO_VIGNETTE_SOURCE

/* the settings each check reads (NULL: the setting is absent) */
static const char *setting_turn = "smooth";
static double setting_snap_angle = 30.0, setting_smooth_turn_speed = 120.0;
static int setting_comfort_vignette;
static int unrecognized_logged;
static char last_log[256];
/* the film's settings, and what the game shows: the letterbox, a scripted
camera */
static double setting_film_depth_share = 0.25, setting_film_convergence = 1.75;
static int game_letterbox, game_scripted_camera;

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
	if (!strcmp(name, "display.film_depth_share"))
		return setting_film_depth_share;
	if (!strcmp(name, "display.film_convergence"))
		return setting_film_convergence;
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
int halo_cinematic_screen(void) { return game_letterbox; }
int halo_scripted_camera(void) { return game_scripted_camera; }
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
	/* SCREEN eyes (the film in HEAD mode): 64 mm apart, 4 m behind the
	camera, frusta through the edges of a screen 4.6 m wide */
	if (frame->mode == HALO_STEREO_SCREEN) {
		int eye;

		for (eye = 0; eye < 2; eye++) {
			struct halo_stereo_eye *e = &frame->eyes[eye];
			float x = (eye == 0 ? -0.032f : 0.032f) / 3.048f, distance = 4.0f / 3.048f, half = 2.309f / 3.048f;

			e->offset[0] = x;
			e->offset[1] = 0.0f;
			e->offset[2] = distance;
			e->left = (half + x) / distance;
			e->right = (half - x) / distance;
			e->up = e->down = 0.75f * half / distance;
		}
	}
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
	restart("smooth", 30.0, 45.0, 1);
	hold_stick(1.0f, 0.5f, 90.0f, &peak);
	check(fabsf(peak - 45.0f / 120.0f) < 0.001f, "vignette: at 45 deg/s full over, 45/120 strong");
	restart("smooth", 30.0, 360.0, 1);
	hold_stick(1.0f, 0.5f, 90.0f, &peak);
	check(fabsf(peak - 1.0f) < 0.001f, "vignette: at 360 deg/s, full");
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

/* nearly straight up, forward's yaw is ill-conditioned: a head at 89.9
degrees that turns 30 degrees about the room's up must turn the look 30
degrees, read from its right, not by whatever forward's yaw swings to */
static void pole(void)
{
	struct halo_stereo_frame frame;

	memset(&head, 0, sizeof(head));
	memset(&frame, 0, sizeof(frame));
	set_pose(0.0f, 89.9f * DEGREES, 0.0f);
	host_stereo_head_turn(&head, pose[0], pose[1], pose[2], &frame);
	set_pose(30.0f * DEGREES, 89.9f * DEGREES, 0.0f);
	host_stereo_head_turn(&head, pose[0], pose[1], pose[2], &frame);
	printf("%-36s head yaw %.3f deg\n", "a 30 deg turn at 89.9 deg pitch", frame.head_yaw / DEGREES);
	if (fabsf(frame.head_yaw / DEGREES - 30.0f) > 0.01f)
	{
		printf("  FAIL: the turn at the pole isn't 30 degrees\n");
		failures++;
	}
}

/* the mask (host_stereo_vignette.h) at a direction, azimuth right positive
and elevation up positive in radians, through an eye's frustum tangents
(left, right, up, down); -1 if the eye doesn't see it */
static float mask_at(const float tangents[4], float azimuth, float elevation, float inner, float outer)
{
	float x = tanf(azimuth), y = tanf(elevation) / cosf(azimuth);
	float u = (x + tangents[0]) / (tangents[0] + tangents[1]);
	float v = (tangents[2] - y) / (tangents[2] + tangents[3]);

	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
		return -1.0f;
	return host_stereo_vignette_edge(u, v, tangents[0], tangents[1], tangents[2], tangents[3], inner, outer);
}

/* the mask both eyes see: the Vision Pro's measured frusta (task-7b-report.md),
the right eye's mirroring the left's */
static void vignette_binocular(void)
{
	static const float left_eye[4] = { 1.757f, 1.014f, 1.014f, 1.209f };
	static const float right_eye[4] = { 1.014f, 1.757f, 1.014f, 1.209f };
	float outer = atanf(1.014f), inner = HOST_STEREO_VIGNETTE_CLEAR_SHARE * outer;
	float worst = 0.0f;
	int both = 0, a, e;

	printf("comfort vignette in both eyes:\n");
	for (a = -60; a <= 60; a += 2) {
		for (e = -40; e <= 40; e += 2) {
			float left = mask_at(left_eye, a * DEGREES, e * DEGREES, inner, outer);
			float right = mask_at(right_eye, a * DEGREES, e * DEGREES, inner, outer);

			if (left < 0.0f || right < 0.0f)
				continue;
			both++;
			if (fabsf(left - right) > worst)
				worst = fabsf(left - right);
		}
	}
	printf("  %d directions seen by both eyes: the masks differ by at most %.6f\n", both, worst);
	check(both > 500 && worst < 0.0001f, "a direction both eyes see is darkened alike in each");
	check(mask_at(left_eye, 0.0f, 0.0f, inner, outer) == 0.0f && mask_at(right_eye, 0.0f, 0.0f, inner, outer) == 0.0f,
		"straight ahead is clear in both eyes");
	check(mask_at(left_eye, 25.0f * DEGREES, 0.0f, inner, outer) == 0.0f &&
		mask_at(left_eye, -25.0f * DEGREES, 0.0f, inner, outer) == 0.0f,
		"the left eye is clear 25 degrees to either side (centered on straight ahead)");
	check(fabsf(mask_at(left_eye, 40.0f * DEGREES, 0.0f, inner, outer) -
		mask_at(right_eye, 40.0f * DEGREES, 0.0f, inner, outer)) < 0.0001f &&
		mask_at(left_eye, 40.0f * DEGREES, 0.0f, inner, outer) > 0.5f,
		"40 degrees right: the same in both eyes, over half dark");
	check(fabsf(mask_at(left_eye, -50.0f * DEGREES, 0.0f, inner, outer) - 1.0f) < 0.0001f,
		"50 degrees out (past the nearest edge's angle): full");
}

/* a tilted head nodding across the 85-degree line, with no turn about the
room's up, must not turn the look (the yaw's source changes there) */
static void pole_crossing(void)
{
	struct halo_stereo_frame frame;
	float worst = 0.0f;
	int step;

	memset(&head, 0, sizeof(head));
	for (step = 0; step <= 40; step++)
	{
		/* 80 to 90 degrees and back, with 10 degrees of roll */
		float pitch = (step <= 20 ? 80.0f + step * 0.5f : 90.0f - (step - 20) * 0.5f) * DEGREES;

		memset(&frame, 0, sizeof(frame));
		set_pose(20.0f * DEGREES, fminf(pitch, 89.9f * DEGREES), 10.0f * DEGREES);
		host_stereo_head_turn(&head, pose[0], pose[1], pose[2], &frame);
		if (fabsf(frame.head_yaw) > worst)
			worst = fabsf(frame.head_yaw);
	}
	printf("%-36s worst head yaw %.3f deg\n", "a tilted nod across 85 deg", worst / DEGREES);
	if (worst / DEGREES > 0.5f)
	{
		printf("  FAIL: nodding with the head tilted turned the look\n");
		failures++;
	}
}

/* where something at (x, y, z) in the camera's frame (right, up, ahead;
world units) lands on a screen of half width w, as an eye of the mapping
sees it: its tangents from the eye, placed in the eye's frustum, which spans
the screen */
static void on_screen(const struct halo_stereo_eye *e, float w, float aspect, const float point[3], float at[2])
{
	float tx = (point[0] - e->offset[0]) / point[2], ty = (point[1] - e->offset[1]) / point[2];

	at[0] = -w + 2.0f * w * (tx + e->left) / (e->left + e->right);
	at[1] = -w / aspect + 2.0f * (w / aspect) * (ty + e->down) / (e->up + e->down);
}

static void film_mapping(void)
{
	const float share = 0.25f, convergence = 1.75f, separation = 0.064f / 3.048f, w = 2.309f / 3.048f;
	const float vertical = 0.335f, aspect = 16.0f / 9.0f, horizontal = vertical * aspect;
	const float c = convergence / 3.048f, lean[2] = { 0.01f, -0.02f };
	struct halo_stereo_eye eyes[2];
	float worst = 0.0f;
	int k;

	printf("the film's mapping:\n");
	halo_stereo_tv_eyes(share, convergence, separation, w, vertical, NULL, eyes);
	check(fabsf(eyes[0].left - eyes[1].right) < 1e-6f && fabsf(eyes[0].right - eyes[1].left) < 1e-6f &&
		eyes[0].up == eyes[0].down && fabsf(eyes[0].offset[0] + eyes[1].offset[0]) < 1e-7f,
		"the eyes mirror each other");
	/* parallax (right eye's place minus the left's) against share e (1 - C / z), at
	any field of view, with and without a lean */
	for (k = 0; k < 2; k++) {
		const float *with = k ? lean : NULL;
		float distances[] = { c * 0.5f, c, c * 2.0f, c * 8.0f, 1e5f }, v;
		int d;

		for (v = 0.2f; v < 0.8f; v += 0.29f) {
			halo_stereo_tv_eyes(share, convergence, separation, w, v, with, eyes);
			for (d = 0; d < 5; d++) {
				float z = distances[d], point[3] = { 0.3f * z * v, -0.2f * z * v, z }, left[2], right[2];
				float expected = share * separation * (1.0f - c / z);

				on_screen(&eyes[0], w, aspect, point, left);
				on_screen(&eyes[1], w, aspect, point, right);
				worst = fmaxf(worst, fabsf((right[0] - left[0]) - expected) / (share * separation));
				worst = fmaxf(worst, fabsf(right[1] - left[1]) / (share * separation));
			}
		}
	}
	printf("  worst parallax error %.2e of infinity's\n", worst);
	check(worst < 1e-3f, "infinity share * e behind the screen, C on it, nearer in front, level");
	/* at the convergence where the camera's view is as wide as the screen, the
	eyes are share * e apart */
	halo_stereo_tv_eyes(share, w / horizontal * 3.048f, separation, w, vertical, NULL, eyes);
	check(fabsf((eyes[1].offset[0] - eyes[0].offset[0]) - share * separation) < 1e-6f,
		"s = share * e at C = w / T");
	/* a lean right and down moves both eyes so, and skews their frusta the
	other way: the picture's rectangle at C stays put */
	halo_stereo_tv_eyes(share, convergence, separation, w, vertical, lean, eyes);
	check(eyes[0].offset[0] + eyes[1].offset[0] > 0.0f && eyes[0].offset[1] < 0.0f &&
		eyes[0].left + eyes[1].left > eyes[0].right + eyes[1].right && eyes[0].up > eyes[0].down &&
		fabsf(eyes[0].up - (vertical - lean[1] / c)) < 1e-6f &&
		fabsf(eyes[1].left - (horizontal + eyes[1].offset[0] / c)) < 1e-6f, "the lean's signs");
}

/* frames of the game after the letterbox goes, each with a script fade of
this intensity over the picture (render.c's halo_stereo_set_fade); returns
how many frames the film's letterbox reason held, and whether every one of
them covered the cut */
static int frames_held(float fade_intensity, int *covered)
{
	const float fade[4] = { 1.0f, 1.0f, 1.0f, fade_intensity };
	int frame;

	*covered = 1;
	game_letterbox = 0;
	for (frame = 0; frame < 3 * FILM_HOLD_FRAMES; frame++) {
		halo_stereo_frame_begin();
		halo_stereo_set_fade(fade);
		if (!halo_stereo_film_letterbox())
			break;
		*covered &= halo_stereo_cut_covered();
	}
	return frame;
}

static void film_hold_reason(void)
{
	int held, covered;

	printf("the film's held reason:\n");
	restart("snap", 30.0, 120.0, 0);
	game_letterbox = 1;
	halo_stereo_frame_begin();
	check(halo_stereo_film() && halo_stereo_film_letterbox() && halo_stereo_frame()->mode == HALO_STEREO_SCREEN,
		"the letterbox puts the film on the screen, for a cutscene");
	check(!halo_stereo_cut_covered(), "no script fade: a cut isn't covered");
	held = frames_held(0.0f, &covered);
	printf("  held %d frames after the letterbox went\n", held);
	check(held == FILM_HOLD_FRAMES, "the cutscene's framing holds through the hold");
	check(!halo_stereo_film() && halo_stereo_frame()->mode == HALO_STEREO_HEAD, "then the full view, head-tracked");
	check(!halo_stereo_cut_covered(), "a fade at zero covers no cut");

	/* a10's two-tick gap between cutscenes, under a white fade: the framing
	doesn't change */
	game_letterbox = 1;
	halo_stereo_frame_begin();
	game_letterbox = 0;
	halo_stereo_frame_begin();
	halo_stereo_frame_begin();
	held = halo_stereo_film_letterbox();
	game_letterbox = 1;
	halo_stereo_frame_begin();
	check(held && halo_stereo_film_letterbox(), "a two-frame gap in the letterbox keeps the cutscene's framing");

	/* a colored fade-in still under way when the hold ends: no black inside it */
	held = frames_held(0.3f, &covered);
	check(held == FILM_HOLD_FRAMES && covered && halo_stereo_cut_covered(),
		"a fade over the picture holds the film as long, and covers the cut");

	/* a scripted camera is the film, without the cutscene's framing */
	game_scripted_camera = 1;
	halo_stereo_frame_begin();
	check(halo_stereo_film() && !halo_stereo_film_letterbox(), "a scripted camera's film has no letterbox reason");
	game_scripted_camera = 0;
	for (held = 0; held < 2 * FILM_HOLD_FRAMES; held++)
		halo_stereo_frame_begin();
}

int main(void)
{
	pole_crossing();
	pole();
	pan("level pan at 0 deg pitch", 0.0f, 0.0f);
	pan("level pan at -10 deg pitch", -10.0f * DEGREES, 0.0f);
	/* the views' forward (0.005, 0.009, -1) from the session-1 log */
	pan("level pan at the views' tilt", asinf(0.009f), 0.0f);
	pan("level pan at -10 deg, 5 deg roll", -10.0f * DEGREES, 5.0f * DEGREES);
	pan("level pan at 30 deg pitch", 30.0f * DEGREES, 0.0f);
	turning();
	vignette_easing();
	vignette_binocular();
	film_mapping();
	film_hold_reason();
	if (failures)
	{
		printf("stereo head probe: %d failed\n", failures);
		return 1;
	}
	printf("stereo head probe: PASS\n");
	return 0;
}
