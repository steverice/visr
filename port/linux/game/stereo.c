/*
STEREO.C

The frame's stereo state (port/linux/src/halo_stereo.h): which stereo mode
display.stereo asks for, and, in stereo, where the two eyes are. In HEAD
mode the host's Compositor frame supplies the eyes and the head's turn
(port/ios/host/host_stereo.m), which drives the player's look: the head turns
the view, the right stick turns the body. In SCREEN mode it supplies the
eyes alone, through the theater screen as a window: the look stays the
stick's, as in mono.

Cutscenes, in any stereo mode, are a 3D film on the 16:9 screen while the
letterbox is in (halo_stereo_film), and so, for comfort, are moments with a
scripted camera and no letterbox (the script's camera_control), and a
third-person camera (third_person_on_screen): a camera the head doesn't
steer is easier to watch on a screen. Only the letterbox narrows the view to
its inside (main.c) and brings bars in for titles (cinematics.c); the others
keep the game's 16:9 framing. The film is not the window's true 1:1: the cinematic
camera's own field of view, framed on the letterbox's inside (main.c's
set_window_camera_values), fills the screen, and the head neither steers it
nor moves its eyes. Its depth is the mapping the film shares with SCREEN
gameplay (halo_stereo_tv_eyes): how far behind the screen infinity sits and
what distance lies on the screen's surface, display.film_depth_share and
display.film_convergence. HEAD mode
asks the host for SCREEN eyes meanwhile, so the frame is SCREEN for
everyone downstream: the host draws it on the screen, the head's turn
stays out of the look and the camera, and the render's SCREEN plane shift
leaves the film's eyes, which sit at the camera, at the camera's planes.
*/

#include <math.h>
#include <string.h>

#include "../src/halo_stereo.h"

/* port/linux/src/port_config.c */
const char *config_string(const char *name);
double config_real(const char *name);
int config_boolean(const char *name);

/* port/linux/src/sdl_platform.c (the host's port/ios/host/host_gpu.c on iOS);
   d3d8_device.c declares it the same way */
void platform_video_drawable_size(int *width, int *height);
void platform_log(const char *format, ...);
/* port/linux/src/d3d8_device.c */
void halo_screen_commit_stereo_scale(void);
#ifdef HALO_IOS
/* port/ios/host/host_stereo.m, imported by the guest (guest_host.h): opens the
Compositor's next frame and fills the eyes and the head's turn from it */
void host_stereo_frame(struct halo_stereo_frame *frame);
#endif

/* the debug side-by-side eyes: fixed frusta and a typical eye separation */
#define SIDE_BY_SIDE_TANGENT 0.8f
#define SIDE_BY_SIDE_OFFSET 0.0105f

/* one world unit in meters */
#define METERS_PER_UNIT 3.048f
/* the ranges of the film's mapping (halo_stereo_tv_eyes; its defaults,
0.25 and 1.75 m, are port_config.c's): infinity this share of the viewer's
eye separation behind the screen (display.film_depth_share), never wider
than the eyes, and this many meters ahead of the camera on the screen's
surface (display.film_convergence) */
#define DEPTH_SHARE_MIN 0.05f
#define DEPTH_SHARE_MAX 0.9f
#define CONVERGENCE_MIN 0.3f
#define CONVERGENCE_MAX 10.0f
/* the eyes' separation when the views don't give one (the simulator has
one view), in meters */
#define FILM_DEFAULT_SEPARATION 0.064f
/* the film's shape: the letterbox's inside, 640x360 */
#define FILM_ASPECT (16.0f / 9.0f)
/* the screen's half width in meters where there is no screen (side by
side): the theater's default, 60 degrees across at 4 m */
#define FILM_DEFAULT_HALF_WIDTH 2.309f
/* the default 70-degree camera's vertical half tangent in the film:
0.75 * 0.75 * 0.85 * tan(35 degrees). A placeholder for the frame's begin
only: render.c's eye loop gives the camera's own tangent
(halo_stereo_film_frusta) before anything reads the eyes */
#define FILM_DEFAULT_VERTICAL_TANGENT 0.335f

static struct halo_stereo_frame stereo_frame;
static int stereo_layer = HALO_STEREO_LAYER_MONO;
static int stereo_mode = -1; /* read once, on the first frame */

static const char *const mode_names[] = {"off", "head", "screen", "side_by_side"};

/* HEAD mode's look: the head's yaw the look hasn't taken in yet, the head's
pitch (the look's pitch follows it), and the stick's turn: its snaps
(input.turn = "snap"), or this frame's share of a smooth turn ("smooth") */
static float head_pending_yaw, head_pitch_now;
static int head_pitch_known;
static float snap_pending, smooth_yaw;
static int snap_armed = 1;

/* how the right stick turns the look in HEAD mode (input.turn), read once */
enum turn_mode { TURN_SNAP, TURN_SMOOTH, TURN_OFF };
static const char *const turn_names[] = {"snap", "smooth", "off"};
static int turn_mode = -1;
static float snap_degrees, smooth_degrees_per_second;
static int comfort_vignette;
/* input.snap_angle's and input.smooth_turn_speed's ranges, in degrees and
degrees per second */
#define SNAP_ANGLE_MIN 5.0f
#define SNAP_ANGLE_MAX 180.0f
#define SMOOTH_SPEED_MIN 10.0f
#define SMOOTH_SPEED_MAX 720.0f
/* the comfort vignette's strength now (0 to 1), and whether the stick was
read since the last frame began (no read, no turn: no vignette) */
static float vignette_strength;
static int vignette_stick_read;
/* the comfort vignette eases in to full over this long, and out from full
over this long, in seconds */
#define VIGNETTE_EASE_IN 0.1f
#define VIGNETTE_EASE_OUT 0.2f
/* the comfort vignette is full at this turn rate and up, in degrees per
second (input.smooth_turn_speed's default): a slower speed setting, a
fainter vignette */
#define VIGNETTE_FULL_RATE 120.0f
/* the stick past this turns one snap; it must come back inside the release
before the next */
#define SNAP_FLICK 0.7f
#define SNAP_RELEASE 0.3f
/* the game's own pitch limit (player_control_modify_desired_angles) */
#define PITCH_LIMIT (85.5f * 3.14159265f / 180.0f)
/* the smallest change of pitch the head hands the look: 0.03 degrees */
#define HEAD_PITCH_DEADBAND 0.0005f

static float z_near_world, z_far_world;

/* this frame (and the last) is the 3D film; the viewer's eye separation
and the screen's half width, in world units */
static int film_frame, film_last;
static float film_viewer_separation, film_half_width;
/* the film's mapping, read once (display.film_depth_share,
display.film_convergence in meters); negative until read */
static float film_depth_share = -1.0f, film_convergence;
/* the script fade the eyes draw and the room takes this frame
(halo_stereo_set_fade) */
static float frame_fade[4];
/* a third-person camera (a vehicle seat's) goes on the screen as the film
does, the stick still driving it as in mono; the one switch for a later
setting */
static int third_person_on_screen = 1;
/* why the view is on the screen: none, the letterbox, a scripted camera, a
third-person camera; logged as it changes */
static const char *const film_reasons[] = {"none", "a cutscene", "a scripted camera", "a third-person camera"};
static int film_reason, film_reason_logged;
/* the view leaves the screen only once nothing has put it there for this
many frames: a10 drops its letterbox for two ticks between two cutscenes,
which would otherwise flip the view to the full view and back */
#define FILM_HOLD_FRAMES 10
static int film_hold;

static void film_frusta(float vertical_tangent);

/* a setting clamped to its range, logged once if it wasn't in it */
static float clamped_setting(const char *name, float value, float minimum, float maximum, const char *unit)
{
	float clamped = fmaxf(minimum, fminf(maximum, value));

	if (clamped != value || value != value) {
		if (value != value)
			clamped = minimum;
		platform_log("stereo: %s %g is outside %g to %g %s; using %g", name, value, minimum, maximum,
			unit, clamped);
	}
	return clamped;
}

/* reads input.turn and its companions, once */
static void turn_settings(void)
{
	const char *turn;
	int mode;

	if (turn_mode >= 0)
		return;
	turn = config_string("input.turn");
	turn_mode = TURN_SNAP;
	for (mode = 0; mode < 3; mode++) {
		if (turn && strcmp(turn, turn_names[mode]) == 0)
			break;
	}
	if (mode < 3)
		turn_mode = mode;
	else
		platform_log("stereo: input.turn \"%s\" is not recognized; using snap", turn ? turn : "(none)");
	snap_degrees = clamped_setting("input.snap_angle", (float)config_real("input.snap_angle"), SNAP_ANGLE_MIN,
		SNAP_ANGLE_MAX, "degrees");
	smooth_degrees_per_second = clamped_setting("input.smooth_turn_speed",
		(float)config_real("input.smooth_turn_speed"), SMOOTH_SPEED_MIN, SMOOTH_SPEED_MAX, "degrees per second");
	comfort_vignette = config_boolean("input.comfort_vignette") != 0;
}

/* reads the film's mapping settings, once */
static void film_settings(void)
{
	if (film_depth_share >= 0.0f)
		return;
	film_depth_share = clamped_setting("display.film_depth_share", (float)config_real("display.film_depth_share"),
		DEPTH_SHARE_MIN, DEPTH_SHARE_MAX, "of the eye separation");
	film_convergence = clamped_setting("display.film_convergence", (float)config_real("display.film_convergence"),
		CONVERGENCE_MIN, CONVERGENCE_MAX, "meters");
}

/* the film's eyes for this frame, from the viewer's separation and the
screen's half width (world units): level, at the camera, by the film's
mapping; the camera's own tangent sets them at the render
(halo_stereo_film_frusta), the default camera's until then */
static void film_begin(float viewer_separation, float half_width)
{
	film_settings();
	film_viewer_separation = viewer_separation;
	film_half_width = half_width > 0.0f ? half_width : FILM_DEFAULT_HALF_WIDTH / METERS_PER_UNIT;
	film_frame = 1;
	film_frusta(FILM_DEFAULT_VERTICAL_TANGENT);
	if (!film_last || film_reason != film_reason_logged)
		platform_log("stereo: %s, as a 3D film on the screen: infinity %.2f of the viewer's %.1f mm behind the "
			"screen, %.2f m ahead on its surface (eyes %.1f mm apart at the default camera), the screen %.2f m wide",
			film_reasons[film_reason], film_depth_share, viewer_separation * METERS_PER_UNIT * 1000.0f,
			film_convergence, (stereo_frame.eyes[1].offset[0] - stereo_frame.eyes[0].offset[0]) * METERS_PER_UNIT *
			1000.0f, 2.0f * film_half_width * METERS_PER_UNIT);
	film_reason_logged = film_reason;
}

/* the mode a display.stereo string names; sets *recognized to 0 for an unknown string */
static int mode_from_name(const char *name, int *recognized)
{
	int mode;

	*recognized = 1;
	for (mode = 0; mode < 4; mode++) {
		if (name && strcmp(name, mode_names[mode]) == 0)
			return mode;
	}
	*recognized = 0;
	return HALO_STEREO_OFF;
}

void halo_stereo_frame_begin(void)
{
	int film;

	if (stereo_mode < 0) {
		const char *name = config_string("display.stereo");
		int recognized;

		stereo_mode = mode_from_name(name, &recognized);
		turn_settings();
		/* the settings in effect (input.* act only in HEAD mode) */
		platform_log("stereo: display.stereo %s, input.turn %s, input.snap_angle %.1f, "
			"input.smooth_turn_speed %.1f, input.comfort_vignette %s", mode_names[stereo_mode],
			turn_names[turn_mode], snap_degrees, smooth_degrees_per_second, comfort_vignette ? "true" : "false");
		if (!recognized)
			platform_log("stereo: display.stereo \"%s\" is not recognized; using off", name ? name : "(none)");
	}
	/* the stick wasn't read since the last frame (no gamepad): no turn, no
	vignette */
	if (!vignette_stick_read)
		vignette_strength = 0.0f;
	vignette_stick_read = 0;

	memset(&stereo_frame, 0, sizeof(stereo_frame));
	stereo_frame.mode = stereo_mode;
	stereo_layer = HALO_STEREO_LAYER_MONO;
	memset(frame_fade, 0, sizeof(frame_fade));
	film_reason = 0;
	if (stereo_mode != HALO_STEREO_OFF) {
		if (halo_cinematic_screen())
			film_reason = 1;
		else if (halo_scripted_camera())
			film_reason = 2;
		else if (third_person_on_screen && halo_third_person_camera())
			film_reason = 3;
	}
	if (film_reason != 0)
		film_hold = FILM_HOLD_FRAMES;
	else if (film_hold > 0) {
		film_hold--;
		film_reason = film_reason_logged;
	}
	film = film_reason != 0;
	film_last = film_frame;
	film_frame = 0;
	/* the film's on the screen: in HEAD mode the frame asks for SCREEN eyes */
	if (film && stereo_mode == HALO_STEREO_HEAD)
		stereo_frame.mode = HALO_STEREO_SCREEN;

	if (stereo_mode == HALO_STEREO_SIDE_BY_SIDE) {
		int width = 0, height = 0;
		int eye;

		platform_video_drawable_size(&width, &height);
		if (width <= 0 || height <= 0)
			return; /* no drawable yet: mono this frame */
		stereo_frame.eye_count = 2;
		stereo_frame.eye_width = width / 2;
		stereo_frame.eye_height = height;
		for (eye = 0; eye < 2; eye++) {
			struct halo_stereo_eye *e = &stereo_frame.eyes[eye];

			e->offset[0] = eye == 0 ? -SIDE_BY_SIDE_OFFSET : SIDE_BY_SIDE_OFFSET;
			e->left = e->right = e->up = e->down = SIDE_BY_SIDE_TANGENT;
		}
		if (film)
			film_begin(2.0f * SIDE_BY_SIDE_OFFSET, FILM_DEFAULT_HALF_WIDTH / METERS_PER_UNIT);
	} else if (stereo_mode == HALO_STEREO_HEAD || stereo_mode == HALO_STEREO_SCREEN) {
		int mode = stereo_frame.mode;

#ifdef HALO_IOS
		host_stereo_frame(&stereo_frame);
#endif
		/* no Compositor frame (the space isn't open): mono, in the window */
		if (stereo_frame.eye_count != 2) {
			memset(&stereo_frame, 0, sizeof(stereo_frame));
			stereo_frame.mode = mode;
		} else if (film) {
			/* the viewer's eyes as the host found them (SCREEN eyes: where they
			are, and frusta through the screen's edges), and the screen's half
			width from the frusta: the tangents add to the width over the
			distance */
			const struct halo_stereo_eye *eyes = stereo_frame.eyes;
			float dx = eyes[1].offset[0] - eyes[0].offset[0];
			float dy = eyes[1].offset[1] - eyes[0].offset[1];
			float dz = eyes[1].offset[2] - eyes[0].offset[2];
			float separation = sqrtf(dx * dx + dy * dy + dz * dz);

			if (separation < 0.001f / METERS_PER_UNIT)
				separation = FILM_DEFAULT_SEPARATION / METERS_PER_UNIT;
			film_begin(separation, eyes[0].offset[2] * (eyes[0].left + eyes[0].right) * 0.5f);
		}
		/* HEAD mode: the look takes this frame's yaw in next frame
		(player_control runs before the render), and a turn it never took is
		dropped; the look's pitch follows the head's. SCREEN mode, and the
		film, have neither */
		head_pending_yaw = stereo_frame.mode == HALO_STEREO_HEAD ? stereo_frame.head_yaw : 0.0f;
		head_pitch_known = stereo_frame.mode == HALO_STEREO_HEAD;
		head_pitch_now = stereo_frame.head_pitch;
	}
	/* the eyes' picture size from this frame on, not the next */
	if (stereo_mode != HALO_STEREO_OFF)
		halo_screen_commit_stereo_scale();
}

int halo_stereo_film(void)
{
	return film_frame && stereo_frame.eye_count == 2;
}

void halo_stereo_set_fade(const float rgb_intensity[4])
{
	memcpy(frame_fade, rgb_intensity, sizeof(frame_fade));
}

void halo_stereo_fade(float rgb_intensity[4])
{
	memcpy(rgb_intensity, frame_fade, sizeof(frame_fade));
}

/* The 3D film's and the 3D TV's mapping (the stereo spec's "One mapping
for the film and the 3D TV"). The camera is the center of two eyes s apart
along its right vector, level whatever the head does, each with a frustum
skewed so the picture's rectangle C ahead of the camera (C_w world units)
is the same for both: that distance lies on the screen's surface.
	s     = sigma e T C_w / w
	left  = T + x / C_w      right = T - x / C_w
	up    = V - y / C_w      down  = V + y / C_w
for the eye at x = -+s/2 (plus the lean) and y (the lean), the picture's
half tangents V and T = 16/9 V, the viewer's eye separation e and the
screen's half width w (any one unit for both). On the screen, something z
ahead has a parallax of sigma e (1 - C_w / z): infinity sigma e behind the
screen, at any field of view (s narrows with T), nearer than C in front.
The eyes sit at the camera (no offset back), so the render keeps the
camera's near and far planes. */
void halo_stereo_tv_eyes(float depth_share, float convergence_meters, float viewer_separation, float half_width,
	float vertical_tangent, const float lean[2], struct halo_stereo_eye eyes[2])
{
	float horizontal_tangent = vertical_tangent * FILM_ASPECT;
	float convergence = convergence_meters / METERS_PER_UNIT;
	float separation = half_width > 0.0f ?
		depth_share * viewer_separation * horizontal_tangent * convergence / half_width : 0.0f;
	int eye;

	for (eye = 0; eye < 2; eye++) {
		struct halo_stereo_eye *e = &eyes[eye];
		float x = (eye == 0 ? -0.5f : 0.5f) * separation + (lean ? lean[0] : 0.0f);
		float y = lean ? lean[1] : 0.0f;

		e->offset[0] = x;
		e->offset[1] = y;
		e->offset[2] = 0.0f;
		e->left = horizontal_tangent + x / convergence;
		e->right = horizontal_tangent - x / convergence;
		e->up = vertical_tangent - y / convergence;
		e->down = vertical_tangent + y / convergence;
	}
}

/* the film's eyes at a camera's vertical tangent: the film's mapping, no
lean */
static void film_frusta(float vertical_tangent)
{
	halo_stereo_tv_eyes(film_depth_share, film_convergence, film_viewer_separation, film_half_width,
		vertical_tangent, NULL, stereo_frame.eyes);
}

void halo_stereo_film_frusta(float vertical_tangent)
{
	if (halo_stereo_film() && vertical_tangent > 0.0f)
		film_frusta(vertical_tangent);
}

const struct halo_stereo_frame *halo_stereo_frame(void)
{
	return &stereo_frame;
}

void halo_stereo_layer(int layer)
{
	stereo_layer = layer;
}

int halo_stereo_current_layer(void)
{
	return stereo_layer;
}

/* whether the head drives the view: HEAD mode with the Compositor's eyes (the
last frame's state, since the look runs before the frame begins) */
static int head_tracking(short gamepad_index)
{
	return gamepad_index == 0 && stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2;
}

float halo_stereo_vignette_ease(float strength, float turn_fraction, float time_delta)
{
	float target = fmaxf(0.0f, fminf(1.0f, turn_fraction));

	if (time_delta <= 0.0f)
		return strength;
	if (strength < target)
		return fminf(target, strength + time_delta / VIGNETTE_EASE_IN);
	return fmaxf(target, strength - time_delta / VIGNETTE_EASE_OUT);
}

void halo_stereo_stick_look(short gamepad_index, float yaw_response, float time_delta, float *yaw, float *pitch)
{
	float magnitude, turn_fraction = 0.0f;

	if (gamepad_index != 0)
		return;
	vignette_stick_read = 1;
	smooth_yaw = 0.0f;
	if (!head_tracking(gamepad_index)) {
		vignette_strength = 0.0f;
		return;
	}
	*pitch = 0.0f;
	turn_settings();
	if (turn_mode == TURN_SMOOTH) {
		/* the game's response curve, at the speed asked for, steady and for
		this frame's time: the game's own turn would speed up to three times
		as fast while the stick is held over */
		smooth_yaw = yaw_response * smooth_degrees_per_second * (3.14159265f / 180.0f) * time_delta;
		turn_fraction = fabsf(yaw_response) * smooth_degrees_per_second / VIGNETTE_FULL_RATE;
	} else if (turn_mode == TURN_SNAP) {
		/* a snap turns the way the stick would turn the look smoothly */
		float snap_radians = snap_degrees * 3.14159265f / 180.0f;

		magnitude = fabsf(*yaw);
		if (snap_armed && magnitude > SNAP_FLICK) {
			snap_pending += *yaw > 0.0f ? snap_radians : -snap_radians;
			snap_armed = 0;
		} else if (magnitude < SNAP_RELEASE) {
			snap_armed = 1;
		}
	}
	/* the turn, if any, comes in with the head's (halo_stereo_head_look) */
	*yaw = 0.0f;
	vignette_strength = comfort_vignette && turn_mode == TURN_SMOOTH ?
		halo_stereo_vignette_ease(vignette_strength, turn_fraction, time_delta) : 0.0f;
}

float halo_stereo_vignette(void)
{
	return stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2 ? vignette_strength : 0.0f;
}

int halo_stereo_head_drives_look(short gamepad_index)
{
	return head_tracking(gamepad_index);
}

/* the head's pitch inside the game's limit */
static float head_pitch_limited(void)
{
	return fmaxf(-PITCH_LIMIT, fminf(PITCH_LIMIT, head_pitch_now));
}

int halo_stereo_head_look(short gamepad_index, float current_pitch, float *yaw, float *pitch)
{
	*yaw = 0.0f;
	*pitch = 0.0f;
	if (!head_tracking(gamepad_index))
	{
		/* a snap armed in the frame the space closed doesn't wait to fire
		when it opens again */
		if (gamepad_index == 0)
			snap_pending = 0.0f;
		return 0;
	}
	*yaw = head_pending_yaw + snap_pending + smooth_yaw;
	/* the look's pitch is the head's, whatever moved it meanwhile (the game
	levels it as the player walks, a script sets it) */
	/* changes under a few hundredths of a degree are noise: they would set
	the look's up and down action flags every frame, which scripts test
	(player_action_test_look_relative_up, a10's look lesson) */
	if (head_pitch_known)
	{
		*pitch = head_pitch_limited() - current_pitch;
		if (fabsf(*pitch) < HEAD_PITCH_DEADBAND)
			*pitch = 0.0f;
	}
	head_pending_yaw = 0.0f;
	snap_pending = 0.0f;
	smooth_yaw = 0.0f;
	return *yaw != 0.0f || *pitch != 0.0f;
}

/* v turned by angle about the unit axis (Rodrigues) */
static void rotate(float v[3], const float axis[3], float angle)
{
	float c = cosf(angle), s = sinf(angle);
	float d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
	float cross[3] = {
		axis[1] * v[2] - axis[2] * v[1],
		axis[2] * v[0] - axis[0] * v[2],
		axis[0] * v[1] - axis[1] * v[0],
	};
	int i;

	for (i = 0; i < 3; i++)
		v[i] = v[i] * c + cross[i] * s + axis[i] * d * (1.0f - c);
}

static void normalize(float v[3])
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (length > 0.0f) {
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
}

void halo_stereo_head_orient(float forward[3], float up[3])
{
	float yaw, pitch;

	if (stereo_frame.mode != HALO_STEREO_HEAD || stereo_frame.eye_count != 2)
		return;
	/* the camera's yaw (the look's, which has the head's turns up to the last
	frame) turned about the world's up by this frame's, left positive as the
	game's yaw; then the head's own pitch, inside the game's limit, so the
	camera is level when the head is, whatever the look's pitch was */
	yaw = atan2f(forward[1], forward[0]) + stereo_frame.head_yaw;
	pitch = head_pitch_limited();
	forward[0] = cosf(pitch) * cosf(yaw);
	forward[1] = cosf(pitch) * sinf(yaw);
	forward[2] = sinf(pitch);
	up[0] = -sinf(pitch) * cosf(yaw);
	up[1] = -sinf(pitch) * sinf(yaw);
	up[2] = cosf(pitch);
	/* the roll tilts up about forward; left ear down tilts it to the left,
	which is a turn the other way about forward */
	rotate(up, forward, -stereo_frame.head_roll);
	normalize(forward);
	normalize(up);
}

void halo_stereo_set_depth_range(float z_near, float z_far)
{
	z_near_world = z_near;
	z_far_world = z_far;
}

void halo_stereo_depth_range(float *z_near, float *z_far)
{
	*z_near = z_near_world;
	*z_far = z_far_world;
}
