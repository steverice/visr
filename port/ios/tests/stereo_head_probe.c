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
over the picture covers the cut.

And a vehicle's third-person camera: the head turns the picture, relative
to its pose when the camera began (no jump on taking a seat), and never the
player's facing; the stick keeps the game's own turn and pitch. With
display.stereo_vehicle_screen the camera goes on the screen instead.

And the head's yaw at render time: paused (no look between frames) the eye
cameras still turn with the head, and the look takes the whole turn when it
runs again, without a jump; and with the game's camera blended between 30 Hz
ticks (render_interpolation.c), the eye cameras' yaw is the head's every
frame, plus the body's own. */
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
static int game_letterbox, game_scripted_camera, game_third_person;
/* the cutscene's camera: first person (the director's or a scripted one in
first-person mode), the look taken away in it, and whether the camera has
reached the player's eyes (the observer settled) */
static int game_first_person, game_look_disabled, game_settled = 1;

/* stereo.c's imports */
const char *config_string(const char *name)
{
	if (!strcmp(name, "display.stereo"))
		return "head";
	if (!strcmp(name, "input.turn"))
		return setting_turn;
	if (!strcmp(name, "display.screen_framing"))
		return "band";
	return "";
}
double config_real(const char *name)
{
	if (!strcmp(name, "display.lod_scale"))
		return 1.0;
	if (!strcmp(name, "input.smooth_turn_speed"))
		return setting_smooth_turn_speed;
	if (!strcmp(name, "display.film_depth_share"))
		return setting_film_depth_share;
	if (!strcmp(name, "display.film_convergence"))
		return setting_film_convergence;
	if (!strcmp(name, "display.screen_depth_share"))
		return 0.3;
	if (!strcmp(name, "display.screen_convergence"))
		return 1.0;
	if (!strcmp(name, "debug.screen_lean"))
		return 0.0;
	return setting_snap_angle;
}
int config_boolean(const char *name) { return !strcmp(name, "input.comfort_vignette") && setting_comfort_vignette; }
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
int halo_scripted_director_camera(void) { return 0; }
int platform_fixed_timestep(void) { return 1; }
/* the frame clock (debug.fixed_timestep: 1/30 s a frame), which only the
cutscene checks move */
static unsigned long probe_clock;
unsigned long platform_clock_frames(void) { return probe_clock; }
double halo_frame_trace_milliseconds(void) { return 0.0; }
int halo_third_person_camera(void) { return game_third_person; }
int halo_cutscene_camera_first_person(void) { return game_first_person; }
int halo_look_disabled_first_person(void) { return game_look_disabled; }
int halo_cutscene_camera_settled(void) { return game_settled; }
void halo_cutscene_state(struct halo_cutscene_state *state)
{
	memset(state, 0, sizeof(*state));
	state->letterbox = game_letterbox;
	state->look_disabled = game_look_disabled;
	state->observer_finished = game_settled;
	state->distance = game_settled ? 0.0f : 1.0f;
}
void halo_screen_commit_stereo_scale(void) {}

#include "../../linux/game/stereo.c"

#define DEGREES (3.14159265f / 180.0f)

/* the device's pose this frame, columns right, up, back (ARKit's axes) */
static float pose[3][3];
static struct host_stereo_head head;
static int host_closed;

/* host_stereo.m's host_stereo_frame, reduced to the head */
void host_stereo_frame(struct halo_stereo_frame *frame)
{
	frame->head_yaw = frame->head_pitch = frame->head_roll = 0.0f;
	/* the space closed: no eyes, and the host forgets the head's pose */
	if (host_closed) {
		frame->eye_count = 0;
		head.known = 0;
		return;
	}
	frame->eye_count = 2;
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
	head_pending_yaw = 0.0f;
	head_yaw_now = head_yaw_taken = 0.0f;
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

static void film_mapping_check(void)
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

/* the yaw and pitch (degrees) of a camera facing (yaw, pitch) radians after
the render orients it by the head */
static void oriented(float yaw, float pitch, float *oriented_yaw, float *oriented_pitch)
{
	float forward[3] = { cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), sinf(pitch) };
	float up[3] = { -sinf(pitch) * cosf(yaw), -sinf(pitch) * sinf(yaw), cosf(pitch) };

	halo_stereo_head_orient(forward, up);
	*oriented_yaw = atan2f(forward[1], forward[0]) / DEGREES;
	*oriented_pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward[2]))) / DEGREES;
}

/* one frame of the game's loop with the head at (yaw, pitch) degrees and the
stick at (stick_yaw, stick_pitch): the stick, then the look (which adds to
the facing what the head and stick hand it), then the frame and the render's
camera, which faces the facing (the chase camera points along it). Returns
the stick's yaw and pitch as the game keeps them, and the view's angles */
struct loop_frame { float stick_yaw, stick_pitch, view_yaw, view_pitch; int look_turned; };

static struct loop_frame loop(float *facing_yaw, float *facing_pitch, float yaw, float pitch, float stick)
{
	struct loop_frame result;
	float look_yaw, look_pitch;

	result.stick_yaw = stick;
	result.stick_pitch = 0.3f;
	halo_stereo_stick_look(0, response(stick), 1.0f / 90.0f, &result.stick_yaw, &result.stick_pitch);
	result.look_turned = halo_stereo_head_look(0, *facing_pitch, &look_yaw, &look_pitch);
	if (result.look_turned) {
		*facing_yaw += look_yaw;
		*facing_pitch += look_pitch;
	}
	set_pose(yaw * DEGREES, pitch * DEGREES, 0.0f);
	halo_stereo_frame_begin();
	oriented(*facing_yaw, *facing_pitch, &result.view_yaw, &result.view_pitch);
	return result;
}

static float degrees_apart(float a, float b)
{
	return fabsf(remainderf(a - b, 360.0f));
}

static void third_person(void)
{
	float facing_yaw = 0.0f, facing_pitch = 0.0f, entry_yaw, entry_pitch, worst_jump = 0.0f, worst_facing = 0.0f;
	float seated_view_yaw, seated_facing_yaw;
	struct loop_frame f;
	int frame, passed, snapped;

	printf("a vehicle's third-person camera:\n");
	restart("smooth", 30.0, 120.0, 1);
	game_third_person = 0;
	/* first person: the head turns 40 degrees left and pitches 15 down, and
	the look follows it */
	for (frame = 1; frame <= 20; frame++)
		loop(&facing_yaw, &facing_pitch, 2.0f * frame, -0.75f * frame, 0.0f);
	f = loop(&facing_yaw, &facing_pitch, 40.0f, -15.0f, 0.0f);
	printf("  first person, the head at 40 deg left, 15 deg down: facing %.3f, %.3f deg\n", facing_yaw / DEGREES,
		facing_pitch / DEGREES);
	check(fabsf(facing_yaw / DEGREES - 40.0f) < 0.01f && fabsf(facing_pitch / DEGREES + 15.0f) < 0.01f,
		"first person: the look follows the head");

	/* take a seat with the head still where it was */
	game_third_person = 1;
	entry_yaw = facing_yaw;
	entry_pitch = facing_pitch;
	f = loop(&facing_yaw, &facing_pitch, 40.0f, -15.0f, 0.0f);
	worst_jump = fmaxf(degrees_apart(f.view_yaw, entry_yaw / DEGREES), fabsf(f.view_pitch - entry_pitch / DEGREES));
	printf("  the seat's first frame: the view %.4f deg from the chase camera's\n", worst_jump);
	check(worst_jump < 0.01f && halo_stereo_frame()->mode == HALO_STEREO_HEAD && !halo_stereo_film(),
		"taking a seat with the head turned doesn't jump the view, and it stays head-tracked");

	/* the head turns 30 degrees left and pitches 10 up over a third of a
	second, with the stick held part way over */
	passed = 1;
	for (frame = 1; frame <= 30; frame++) {
		f = loop(&facing_yaw, &facing_pitch, 40.0f + frame, -15.0f + frame / 3.0f, 0.5f);
		worst_facing = fmaxf(worst_facing, fmaxf(fabsf(facing_yaw - entry_yaw), fabsf(facing_pitch - entry_pitch)));
		passed &= !f.look_turned && f.stick_yaw == 0.5f && f.stick_pitch == 0.3f;
	}
	printf("  the head 30 deg left, 10 up: the view %.4f, %.4f deg from the chase camera's; the facing moved %.6f deg\n",
		f.view_yaw - entry_yaw / DEGREES, f.view_pitch - entry_pitch / DEGREES, worst_facing / DEGREES);
	check(fabsf(f.view_yaw - entry_yaw / DEGREES - 30.0f) < 0.01f, "a 30 degree head yaw turns the eye cameras 30 degrees");
	check(fabsf(f.view_pitch - entry_pitch / DEGREES - 10.0f) < 0.01f, "a 10 degree head pitch pitches them 10 degrees");
	check(worst_facing == 0.0f && passed, "the facing never moves: the look takes no turn from the head");
	check(passed, "the stick keeps the game's own turn and pitch (it swings the boom)");
	check(halo_stereo_vignette() == 0.0f, "no comfort vignette for the stick's turn");
	check(!halo_stereo_head_drives_look(0), "the head doesn't drive the look (the autolevel is the game's)");
	/* the seat's gun aims along the game's camera (the facing): the
	crosshair goes where that points in the head-turned picture, 30 degrees
	right of the view's center and below it, at the angle between them */
	{
		float reticle[3], length, game[3], view[3], apart;

		halo_stereo_reticle(reticle);
		length = sqrtf(reticle[0] * reticle[0] + reticle[1] * reticle[1] + reticle[2] * reticle[2]);
		game[0] = cosf(facing_pitch) * cosf(facing_yaw);
		game[1] = cosf(facing_pitch) * sinf(facing_yaw);
		game[2] = sinf(facing_pitch);
		view[0] = cosf(f.view_pitch * DEGREES) * cosf(f.view_yaw * DEGREES);
		view[1] = cosf(f.view_pitch * DEGREES) * sinf(f.view_yaw * DEGREES);
		view[2] = sinf(f.view_pitch * DEGREES);
		apart = acosf(fminf(1.0f, game[0] * view[0] + game[1] * view[1] + game[2] * view[2])) / DEGREES;
		printf("  the seat's crosshair: %.4f %.4f %.4f in the eyes' frame, %.3f deg off center (the aim and the "
			"view %.3f deg apart)\n", reticle[0], reticle[1], reticle[2], acosf(-reticle[2] / length) / DEGREES,
			apart);
		check(fabsf(length - 1.0f) < 1e-4f && reticle[0] > 0.4f && reticle[1] < -0.1f &&
			fabsf(acosf(-reticle[2] / length) / DEGREES - apart) < 0.01f,
			"a seat's crosshair points where the gun aims: right of and below the head-turned view's center");
	}

	/* the stick swings the camera (the facing, which the chase camera
	follows): the head's turn stays on top of it */
	facing_yaw += 20.0f * DEGREES;
	f = loop(&facing_yaw, &facing_pitch, 70.0f, -5.0f, 0.0f);
	check(fabsf(f.view_yaw - facing_yaw / DEGREES - 30.0f) < 0.01f, "the head's turn rides on the stick's swing");

	/* back in first person with the head still (the game has glided its
	camera from the boom to the eyes, along the facing): the view's yaw
	doesn't move, the look takes the head's 30 degrees in the seat once, and
	the look's pitch is the head's own again */
	game_third_person = 0;
	seated_view_yaw = f.view_yaw;
	seated_facing_yaw = facing_yaw;
	f = loop(&facing_yaw, &facing_pitch, 70.0f, -5.0f, 0.0f);
	{
		float reticle[3];

		halo_stereo_reticle(reticle);
		check(reticle[0] == 0.0f && reticle[1] == 0.0f && reticle[2] == -1.0f,
			"on foot the crosshair is head-locked, straight ahead");
	}
	printf("  first person again: the view %.4f deg from the seat's last\n", degrees_apart(f.view_yaw, seated_view_yaw));
	check(degrees_apart(f.view_yaw, seated_view_yaw) < 0.01f && fabsf(f.view_pitch + 5.0f) < 0.01f,
		"leaving: the view's yaw holds (the seat's head turn kept), the pitch the head's");
	f = loop(&facing_yaw, &facing_pitch, 72.0f, -5.0f, 0.0f);
	check(f.look_turned && fabsf(facing_pitch / DEGREES + 5.0f) < 0.01f &&
		degrees_apart(facing_yaw / DEGREES, seated_facing_yaw / DEGREES + 30.0f) < 0.01f &&
		degrees_apart(facing_yaw / DEGREES, f.view_yaw - 2.0f) < 0.01f,
		"the look takes the seat's 30 degrees once, then follows the head from there");
	f = loop(&facing_yaw, &facing_pitch, 72.0f, -5.0f, 0.0f);
	check(degrees_apart(facing_yaw / DEGREES, seated_facing_yaw / DEGREES + 32.0f) < 0.01f &&
		degrees_apart(f.view_yaw, seated_view_yaw + 2.0f) < 0.01f, "and not again");

	/* snaps: the stick held over in the seat doesn't snap on leaving it */
	restart("snap", 30.0, 120.0, 0);
	facing_yaw = facing_pitch = 0.0f;
	game_third_person = 1;
	/* (the stick's read before the frame begins: the seat's first frame
	still has the first person's look) */
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 0.0f);
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 1.0f);
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 1.0f);
	game_third_person = 0;
	for (frame = 0; frame < 10; frame++)
		loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 1.0f);
	snapped = facing_yaw != 0.0f;
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 0.0f);
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 1.0f);
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 1.0f);
	check(!snapped && fabsf(facing_yaw / DEGREES - 30.0f) < 0.01f,
		"a stick held over from the seat doesn't snap; a fresh flick does");

	/* display.stereo_vehicle_screen: the camera goes on the screen */
	vehicle_screen = 1;
	game_third_person = 1;
	halo_stereo_frame_begin();
	check(halo_stereo_film() && film_reason == 3 && halo_stereo_frame()->mode == HALO_STEREO_SCREEN,
		"display.stereo_vehicle_screen: the third-person camera is the film");
	vehicle_screen = 0;
	game_third_person = 0;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++)
		halo_stereo_frame_begin();
}

/* a seat begun with the head moving on that frame, after the film, and
after the space reopens: the view on the seat's first frame is the head's
turn since the pose the facing has, which is the last frame's after
head-tracked first person and the head's now otherwise */
static void third_person_entries(void)
{
	float facing_yaw, facing_pitch;
	struct loop_frame f;
	int frame;

	printf("a third-person camera's first frame:\n");
	/* the head moving: the facing has the last frame's pose, so the view is
	turned by this frame's change */
	restart("snap", 30.0, 120.0, 0);
	game_third_person = 0;
	facing_yaw = facing_pitch = 0.0f;
	for (frame = 1; frame <= 20; frame++)
		loop(&facing_yaw, &facing_pitch, 2.0f * frame, -0.75f * frame, 0.0f);
	game_third_person = 1;
	f = loop(&facing_yaw, &facing_pitch, 42.0f, -14.0f, 0.0f);
	printf("  the head 2 deg left and 1 up on the seat's first frame: the view %.4f, %.4f deg from the facing\n",
		f.view_yaw - facing_yaw / DEGREES, f.view_pitch - facing_pitch / DEGREES);
	check(fabsf(facing_yaw / DEGREES - 40.0f) < 0.01f && fabsf(facing_pitch / DEGREES + 15.0f) < 0.01f &&
		fabsf(f.view_yaw - facing_yaw / DEGREES - 2.0f) < 0.01f && fabsf(f.view_pitch - facing_pitch / DEGREES - 1.0f) < 0.01f,
		"moving into the seat: the view moves by the frame's change only, no jump");
	game_third_person = 0;
	for (frame = 0; frame < 3; frame++)
		loop(&facing_yaw, &facing_pitch, 42.0f, -14.0f, 0.0f);

	/* after the film: a cutscene in which the head turns 50 degrees and
	looks down, then the seat once the film's hold ends */
	restart("snap", 30.0, 120.0, 0);
	facing_yaw = 10.0f * DEGREES;
	facing_pitch = 5.0f * DEGREES;
	game_letterbox = 1;
	for (frame = 0; frame <= 20; frame++)
		loop(&facing_yaw, &facing_pitch, 2.5f * frame, -1.0f * frame, 0.0f);
	game_letterbox = 0;
	game_third_person = 1;
	for (frame = 0; frame < FILM_HOLD_FRAMES; frame++)
		loop(&facing_yaw, &facing_pitch, 50.0f, -20.0f, 0.0f);
	f = loop(&facing_yaw, &facing_pitch, 51.0f, -20.0f, 0.0f);
	printf("  the seat after the film: the view %.4f, %.4f deg from the chase camera's\n",
		f.view_yaw - facing_yaw / DEGREES, f.view_pitch - facing_pitch / DEGREES);
	check(!halo_stereo_film() && fabsf(facing_yaw / DEGREES - 10.0f) < 0.01f &&
		degrees_apart(f.view_yaw, facing_yaw / DEGREES) < 0.01f && fabsf(f.view_pitch - facing_pitch / DEGREES) < 0.01f,
		"after the film: the head's pose then is the reference, no jump");
	game_third_person = 0;
	for (frame = 0; frame < 3; frame++)
		loop(&facing_yaw, &facing_pitch, 51.0f, -20.0f, 0.0f);

	/* after the space reopens: closed while the head turned 60 degrees and
	looked down, reopened in the seat */
	restart("snap", 30.0, 120.0, 0);
	facing_yaw = facing_pitch = 0.0f;
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 0.0f);
	host_closed = 1;
	game_third_person = 1;
	for (frame = 0; frame <= 10; frame++)
		loop(&facing_yaw, &facing_pitch, 6.0f * frame, -2.0f * frame, 0.0f);
	host_closed = 0;
	f = loop(&facing_yaw, &facing_pitch, 61.0f, -20.0f, 0.0f);
	printf("  the seat after the space reopens: the view %.4f, %.4f deg from the chase camera's\n",
		f.view_yaw - facing_yaw / DEGREES, f.view_pitch - facing_pitch / DEGREES);
	check(halo_stereo_frame()->eye_count == 2 && degrees_apart(f.view_yaw, facing_yaw / DEGREES) < 0.01f &&
		fabsf(f.view_pitch - facing_pitch / DEGREES) < 0.01f,
		"after the space reopens: the head's pose then is the reference, no jump");
	f = loop(&facing_yaw, &facing_pitch, 71.0f, -10.0f, 0.0f);
	check(fabsf(f.view_yaw - facing_yaw / DEGREES - 10.0f) < 0.01f && fabsf(f.view_pitch - facing_pitch / DEGREES - 10.0f) < 0.01f,
		"then the head turns it from there");
	game_third_person = 0;
	for (frame = 0; frame < 3; frame++)
		loop(&facing_yaw, &facing_pitch, 71.0f, -10.0f, 0.0f);
}

/* paused: three frames with the head turning 5 degrees each and no look
between them (player_control's camera control is off while the game is
paused, so halo_stereo_head_look doesn't run), then the look again */
static void paused(void)
{
	float facing_yaw = 0.0f, facing_pitch = 0.0f, view_yaw, view_pitch, look_yaw, look_pitch, paused_view = 0.0f;
	struct loop_frame f;
	int frame, passed = 1;

	printf("paused (no look between frames):\n");
	restart("snap", 30.0, 120.0, 0);
	for (frame = 0; frame < 3; frame++)
		loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 0.0f);
	for (frame = 1; frame <= 3; frame++) {
		set_pose(5.0f * frame * DEGREES, 0.0f, 0.0f);
		halo_stereo_frame_begin();
		oriented(facing_yaw, facing_pitch, &view_yaw, &view_pitch);
		printf("  paused frame %d: the head %.1f deg, the eye cameras %.4f deg\n", frame, 5.0f * frame, view_yaw);
		passed &= fabsf(view_yaw - 5.0f * frame) < 0.01f;
		paused_view = view_yaw;
	}
	check(passed, "paused, the eye cameras turn with the head: 5, 10 and 15 degrees");
	check(halo_stereo_head_look(0, facing_pitch, &look_yaw, &look_pitch) &&
		fabsf(look_yaw / DEGREES - 15.0f) < 0.01f, "the look takes the paused frames' 15 degrees at once");
	facing_yaw += look_yaw;
	check(halo_stereo_head_look(0, facing_pitch, &look_yaw, &look_pitch) == 0 && look_yaw == 0.0f,
		"and clears them");
	set_pose(15.0f * DEGREES, 0.0f, 0.0f);
	halo_stereo_frame_begin();
	oriented(facing_yaw, facing_pitch, &view_yaw, &view_pitch);
	printf("  unpaused: the facing %.4f deg, the eye cameras %.4f deg (paused last at %.4f)\n",
		facing_yaw / DEGREES, view_yaw, paused_view);
	check(fabsf(view_yaw - facing_yaw / DEGREES) < 0.01f && fabsf(view_yaw - paused_view) < 0.01f,
		"the next frame's eye cameras match the look: unpausing doesn't jump the view");
	f = loop(&facing_yaw, &facing_pitch, 17.0f, 0.0f, 0.0f);
	check(fabsf(f.view_yaw - 17.0f) < 0.01f, "and the head turns it from there");

	/* the film between the pause and the look: its turns are dropped, as
	before (the film's hold ends on a head-tracked frame) */
	restart("snap", 30.0, 120.0, 0);
	facing_yaw = facing_pitch = 0.0f;
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 0.0f);
	set_pose(10.0f * DEGREES, 0.0f, 0.0f);
	halo_stereo_frame_begin();
	game_letterbox = 1;
	halo_stereo_frame_begin();
	game_letterbox = 0;
	for (frame = 0; frame < FILM_HOLD_FRAMES + 1; frame++)
		halo_stereo_frame_begin();
	check(halo_stereo_head_look(0, facing_pitch, &look_yaw, &look_pitch) == 0 && look_yaw == 0.0f,
		"a film in between drops the paused turn");
}

/* the game's camera in the render with interpolation (render_interpolation.c):
the facing is posed into the camera every frame (first_person_camera_update),
kept after each tick with the head yaw the look had taken
(halo_stereo_head_yaw_taken), and the frame draws the last two kept, blended
by how far the clock is into the next tick (nlerp of the forward). The
earlier one is first turned about the world's up by the head yaw the later
holds and it doesn't, so the blend holds the later one's head yaw exactly
(render_interpolation_blended_camera), and the render is told so
(halo_stereo_camera_head_yaw) */
struct ticked_camera
{
	float previous[3], latest[3];
	float previous_head, latest_head;
	/* posed from the facing (the director's settled first person), not a
	glide or another camera (camera_facing_posed) */
	int previous_posed, latest_posed;
	int kept;
};

/* a tick's camera: facing a yaw, posed from the facing or not */
static void camera_keep_posed(struct ticked_camera *camera, float yaw, int posed)
{
	memcpy(camera->previous, camera->latest, sizeof(camera->latest));
	camera->previous_head = camera->latest_head;
	camera->previous_posed = camera->latest_posed;
	camera->latest[0] = cosf(yaw);
	camera->latest[1] = sinf(yaw);
	camera->latest[2] = 0.0f;
	camera->latest_head = halo_stereo_head_yaw_taken();
	camera->latest_posed = posed;
	if (!camera->kept++) {
		memcpy(camera->previous, camera->latest, sizeof(camera->latest));
		camera->previous_head = camera->latest_head;
		camera->previous_posed = posed;
	}
}

static void camera_keep(struct ticked_camera *camera, float facing_yaw)
{
	camera_keep_posed(camera, facing_yaw, 1);
}

/* the blend's yaw as render_interpolation.c makes it: each camera posed from
the facing turned by the head yaw the look took since it was kept, so it
holds the head yaw as of now (a glide stays as it is), and what it holds
reported as the look's now */
static float camera_blended_yaw(const struct ticked_camera *camera, float t)
{
	float previous[3], latest[3], blended[3];
	float taken = halo_stereo_head_yaw_taken();
	int i;

	memcpy(previous, camera->previous, sizeof(previous));
	memcpy(latest, camera->latest, sizeof(latest));
	if (camera->previous_posed)
		rotate_axis(previous, 2, remainderf(taken - camera->previous_head, TWO_PI));
	if (camera->latest_posed)
		rotate_axis(latest, 2, remainderf(taken - camera->latest_head, TWO_PI));
	for (i = 0; i < 3; i++)
		blended[i] = previous[i] + (latest[i] - previous[i]) * t;
	halo_stereo_camera_head_yaw(taken, t, camera->previous_posed && camera->latest_posed ? "blended" : "edge");
	return atan2f(blended[1], blended[0]);
}

/* a steady head turn at 90 frames a second while the game ticks at 30 (three
frames a tick, the blend 0, 1/3, 2/3), with the body turned by the stick too
(smooth turning) in one case: the eye cameras' yaw must be the head's plus the
body's own, which the game's camera shows blended a tick behind, every frame */
static void blended_turn(const char *name, float head_degrees_per_frame, float stick, int frames_per_tick)
{
	struct ticked_camera camera = { { 0 }, { 0 }, 0.0f, 0.0f, 0 };
	float facing_yaw = 0.0f, facing_pitch = 0.0f, look_yaw, look_pitch, head_yaw = 0.0f, worst = 0.0f;
	/* the body's own yaw (the facing less the head's turn the look took) at
	each tick kept, the first at the start */
	float body[64] = { 0.0f };
	int frame, ticks = 1;

	restart("smooth", 30.0, 120.0, 0);
	camera_keep(&camera, facing_yaw);
	for (frame = 1; frame <= 60; frame++) {
		float yaw_in = stick, pitch_in = 0.3f, t, view, expected, error, earlier, later;

		/* player_control: the stick, then the look */
		halo_stereo_stick_look(0, response(stick), 1.0f / 90.0f, &yaw_in, &pitch_in);
		if (halo_stereo_head_look(0, facing_pitch, &look_yaw, &look_pitch))
			facing_yaw += look_yaw;
		/* the tick (game_time_update), then the frame, the blend that far
		into the next tick */
		if (frame % frames_per_tick == 0) {
			camera_keep(&camera, facing_yaw);
			body[ticks % 64] = remainderf(facing_yaw - halo_stereo_head_yaw_taken(), TWO_PI);
			ticks++;
		}
		t = (float)(frame % frames_per_tick) / (float)frames_per_tick;
		head_yaw += head_degrees_per_frame * DEGREES;
		set_pose(head_yaw, 0.0f, 0.0f);
		halo_stereo_frame_begin();
		oriented(camera_blended_yaw(&camera, t), 0.0f, &view, &look_pitch);
		earlier = body[(ticks >= 2 ? ticks - 2 : 0) % 64];
		later = body[(ticks - 1) % 64];
		expected = (head_yaw + earlier + remainderf(later - earlier, TWO_PI) * t) / DEGREES;
		error = degrees_apart(view, expected);
		if (error > worst)
			worst = error;
	}
	printf("  %-44s the eye cameras %.4f deg at worst from the head's yaw plus the body's\n", name, worst);
	check(worst < 0.02f, name);
}

/* leaving a seat: the director's glide toward the eyes, a camera not posed
from the facing, over 6 ticks at 3 frames a tick, between first-person
cameras posed from it; the glide starts at the facing's pose then and
closes on the facing's pose as it moves (as the observer chases its
command), reaching it on its last tick. During the glide the eye cameras are
the glide's blend turned by only the head yaw the look hasn't taken (its
turn since the last frame): no step at a tick from head yaw the glide never
had. And at its edges, with the head turning or still, the body's yaw (the
view's less the head's) moves without a step: no frame turns it by more
than a frame of the head's turn plus a frame of the glide's own */
static void glide(void)
{
	struct ticked_camera camera;
	float facing_yaw, facing_pitch = 0.0f, look_yaw, look_pitch, head_yaw, glide_from = 0.0f;
	float worst_glide = 0.0f, worst_step[2] = { 0.0f, 0.0f }, last_body = 0.0f;
	int frame, still;

	printf("a glide between cameras (the director's, leaving a seat), the game's camera blended between ticks:\n");
	for (still = 0; still < 2; still++) {
		float head_step = still ? 0.0f : 1.0f * DEGREES;

		restart("snap", 30.0, 120.0, 0);
		memset(&camera, 0, sizeof(camera));
		facing_yaw = head_yaw = 0.0f;
		camera_keep(&camera, facing_yaw);
		/* 6 ticks posed, 6 gliding, 6 posed again */
		for (frame = 1; frame <= 54; frame++) {
			int tick = frame % 3 == 0, tick_index = frame / 3;
			float t = (float)(frame % 3) / 3.0f, camera_yaw, view, view_pitch, pending, body;

			if (halo_stereo_head_look(0, facing_pitch, &look_yaw, &look_pitch))
				facing_yaw += look_yaw;
			if (tick) {
				if (tick_index == 6)
					glide_from = facing_yaw;
				if (tick_index >= 6 && tick_index < 12)
					camera_keep_posed(&camera, glide_from + (facing_yaw - glide_from) * (float)(tick_index - 5) / 6.0f,
						0);
				else
					camera_keep(&camera, facing_yaw);
			}
			head_yaw += head_step;
			set_pose(head_yaw, 0.0f, 0.0f);
			halo_stereo_frame_begin();
			pending = head_pending_yaw;
			camera_yaw = camera_blended_yaw(&camera, t);
			oriented(camera_yaw, 0.0f, &view, &view_pitch);
			/* both cameras the glide's: its own yaw plus the turn not yet
			taken, nothing more */
			if (!camera.previous_posed && !camera.latest_posed)
				worst_glide = fmaxf(worst_glide, degrees_apart(view, (camera_yaw + pending) / DEGREES));
			/* the body's yaw from frame to frame, past the start */
			body = view - head_yaw / DEGREES;
			if (frame > 1)
				worst_step[still] = fmaxf(worst_step[still], fabsf(remainderf(body - last_body, 360.0f)));
			last_body = body;
		}
	}
	printf("  gliding with the head turning: the eye cameras %.4f deg at worst from the glide's yaw plus the turn "
		"not yet taken\n", worst_glide);
	check(worst_glide < 0.01f, "a glide gets no head yaw it never had: no step at its ticks");
	/* a frame's turn of the head (1 degree) and of the glide's own (its 6
	ticks close on the facing, which the head turns 3 degrees a tick: at
	most about 1 degree a frame) */
	printf("  into the glide and out of it, the body's yaw steps %.4f deg at worst in a frame with the head turning "
		"1 deg a frame, %.4f with it still\n", worst_step[0], worst_step[1]);
	check(worst_step[0] <= 2.05f && worst_step[1] < 0.01f, "into a glide and out of it, no hitch, turning or still");
}

static void interpolated_turns(void)
{
	printf("the head's yaw at render time, the game's camera blended between ticks:\n");
	blended_turn("a steady turn, 1 deg a frame, 3 frames a tick", 1.0f, 0.0f, 3);
	blended_turn("a fast turn, 3 deg a frame, 3 frames a tick", 3.0f, 0.0f, 3);
	blended_turn("a turn, 2 frames a tick", 1.5f, 0.0f, 2);
	blended_turn("a turn with the stick turning the body", 1.0f, 0.6f, 3);
}

/* one frame of a cutscene check: the clock moves a thirtieth of a second */
static void cutscene_frame(void)
{
	probe_clock++;
	halo_stereo_frame_begin();
}

static int immersive(void)
{
	return !halo_stereo_film() && halo_stereo_frame()->mode == HALO_STEREO_HEAD;
}

static int on_film(void)
{
	return halo_stereo_film() && halo_stereo_frame()->mode == HALO_STEREO_SCREEN;
}

/* frames until the film is gone (at most limit) */
static int frames_to_full_view(int limit)
{
	int frame;

	for (frame = 0; frame < limit && halo_stereo_film(); frame++)
		cutscene_frame();
	return frame;
}

/* the session-4 rule: in HEAD mode the film is only for a cutscene's camera
that isn't first person, from the cutscene's first such camera to its end */
static void first_person_cutscenes(void)
{
	int frame, passed;

	printf("first-person cutscene cameras:\n");
	restart("snap", 30.0, 120.0, 0);
	game_first_person = 0;
	game_settled = 1;
	/* a letterbox with a third-person scripted camera: the film */
	game_letterbox = 1;
	cutscene_frame();
	check(on_film(), "a cutscene's third-person camera is on the film");
	game_letterbox = 0;
	frames_to_full_view(4 * FILM_HOLD_FRAMES);
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();

	/* a letterbox with a first-person camera, throughout: immersive throughout */
	game_letterbox = 1;
	game_first_person = 1;
	passed = 1;
	for (frame = 0; frame < 60; frame++) {
		cutscene_frame();
		passed &= immersive();
	}
	game_letterbox = 0;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++) {
		cutscene_frame();
		passed &= immersive();
	}
	check(passed, "a cutscene first person throughout stays immersive throughout");

	/* a scripted first-person camera without the letterbox: immersive too */
	game_scripted_camera = 1;
	cutscene_frame();
	check(immersive(), "a scripted camera in first person stays immersive");
	game_scripted_camera = 0;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++)
		cutscene_frame();

	/* b30's intro, in the other order too: first person, then a third-person
	shot, then first person again; the film from the first third-person shot
	to the cutscene's end, never back */
	game_letterbox = 1;
	game_first_person = 1;
	for (frame = 0; frame < 10; frame++)
		cutscene_frame();
	check(immersive(), "a cutscene that begins first person begins immersive");
	game_first_person = 0;
	cutscene_frame();
	check(on_film(), "its first third-person shot puts it on the film");
	game_first_person = 1;
	passed = 1;
	for (frame = 0; frame < 60; frame++) {
		cutscene_frame();
		passed &= on_film();
	}
	game_first_person = 0;
	cutscene_frame();
	game_first_person = 1;
	for (frame = 0; frame < 60; frame++) {
		cutscene_frame();
		passed &= on_film();
	}
	check(passed, "a first-person shot after it stays on the film: no flip back");
	/* a10's two-frame gap in the letterbox, then a first-person stretch:
	still the same cutscene */
	game_letterbox = 0;
	cutscene_frame();
	cutscene_frame();
	game_letterbox = 1;
	cutscene_frame();
	check(on_film(), "a two-frame gap in the letterbox keeps one cutscene on the film");
	game_letterbox = 0;
	frame = frames_to_full_view(4 * FILM_HOLD_FRAMES);
	/* (the frame that ends it counted) */
	check(frame == FILM_HOLD_FRAMES + 1, "the cutscene's end: the film through its hold, then the full view");
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();
	/* the next cutscene starts afresh */
	game_letterbox = 1;
	cutscene_frame();
	check(immersive(), "the next cutscene, first person, is immersive again");
	game_letterbox = 0;
	game_first_person = 0;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++)
		cutscene_frame();
}

/* a first-person moment with the look taken away (a10's cryo pod, as the
spec has it): immersive, the head turning the picture only, its turn handed
to the look on the way out */
static void look_disabled(void)
{
	float facing_yaw = 0.0f, facing_pitch = 0.0f, entry_yaw, worst_facing = 0.0f, last_view_yaw;
	struct loop_frame f;
	int frame, turned = 0, passed = 1;

	printf("a first-person camera with the look taken away:\n");
	restart("snap", 30.0, 120.0, 0);
	game_first_person = 1;
	for (frame = 1; frame <= 10; frame++)
		loop(&facing_yaw, &facing_pitch, 1.0f * frame, 0.0f, 0.0f);
	/* (the look takes the last frame's turn) */
	loop(&facing_yaw, &facing_pitch, 10.0f, 0.0f, 0.0f);
	game_scripted_camera = 1;
	game_look_disabled = 1;
	entry_yaw = facing_yaw;
	f = loop(&facing_yaw, &facing_pitch, 10.0f, 0.0f, 0.0f);
	check(immersive(), "the look taken away in first person: immersive, not the film");
	check(degrees_apart(f.view_yaw, entry_yaw / DEGREES) < 0.01f, "its first frame doesn't move the view");
	for (frame = 1; frame <= 30; frame++) {
		f = loop(&facing_yaw, &facing_pitch, 10.0f + frame, 0.0f, 0.0f);
		turned |= f.look_turned;
		worst_facing = fmaxf(worst_facing, fabsf(facing_yaw - entry_yaw));
		passed &= immersive();
	}
	printf("  the head 30 deg left: the view %.4f deg from the camera's, the facing moved %.6f deg\n",
		f.view_yaw - entry_yaw / DEGREES, worst_facing / DEGREES);
	check(passed && fabsf(f.view_yaw - entry_yaw / DEGREES - 30.0f) < 0.01f,
		"the head turns the picture: 30 degrees for 30");
	check(!turned && worst_facing == 0.0f, "halo_stereo_head_look returns no turn: the look takes none of it");
	/* the look back: the view holds, and the look takes the 30 degrees once */
	game_scripted_camera = 0;
	game_look_disabled = 0;
	last_view_yaw = f.view_yaw;
	f = loop(&facing_yaw, &facing_pitch, 40.0f, 0.0f, 0.0f);
	check(degrees_apart(f.view_yaw, last_view_yaw) < 0.01f, "the look back: the view doesn't jump");
	f = loop(&facing_yaw, &facing_pitch, 40.0f, 0.0f, 0.0f);
	check(f.look_turned && degrees_apart(facing_yaw / DEGREES, entry_yaw / DEGREES + 30.0f) < 0.01f &&
		degrees_apart(f.view_yaw, last_view_yaw) < 0.01f, "the look takes the moment's head turn once, as a seat's");
	game_first_person = 0;
}

/* the cutscene's end: the film holds until the camera reaches the eyes,
at most 2.5 s */
static void cutscene_end(void)
{
	int frame, held;

	printf("a cutscene's end:\n");
	restart("snap", 30.0, 120.0, 0);
	game_first_person = 0;
	/* the observer still gliding to the eyes when the letterbox goes */
	game_letterbox = 1;
	game_settled = 1;
	cutscene_frame();
	game_letterbox = 0;
	game_settled = 0;
	for (held = 0; held < 40 && halo_stereo_film(); held++)
		cutscene_frame();
	printf("  the camera away from the eyes: the film held %d frames\n", held);
	check(held == 40 && on_film() && halo_stereo_film_letterbox(),
		"the film holds while the camera hasn't reached the eyes, past the 10-frame hold");
	game_settled = 1;
	cutscene_frame();
	check(immersive(), "then the full view, once it has");
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();

	/* a camera that never reaches the eyes: the hold gives up after 2.5 s,
	and the film ends through black, as before */
	game_letterbox = 1;
	cutscene_frame();
	game_letterbox = 0;
	game_settled = 0;
	held = frames_to_full_view(200);
	printf("  never settling: the film held %d frames\n", held);
	check(held == 75, "a camera that never reaches the eyes: the film holds 2.5 s, no longer");
	game_settled = 1;
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();
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
	film_mapping_check();
	film_hold_reason();
	third_person();
	third_person_entries();
	paused();
	interpolated_turns();
	glide();
	first_person_cutscenes();
	look_disabled();
	cutscene_end();
	if (failures)
	{
		printf("stereo head probe: %d failed\n", failures);
		return 1;
	}
	printf("stereo head probe: PASS\n");
	return 0;
}
