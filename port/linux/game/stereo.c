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
letterbox is in (halo_stereo_film). The film is not the window's true 1:1:
the cinematic camera's own field of view, framed on the letterbox's inside
(main.c's set_window_camera_values), fills the screen, and the head neither
steers it nor moves its eyes (film_frusta below has the mapping). HEAD mode
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

/* port/linux/src/sdl_platform.c (the host's port/ios/host/host_gpu.c on iOS);
   d3d8_device.c declares it the same way */
void platform_video_drawable_size(int *width, int *height);
void platform_log(const char *format, ...);
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
/* the film's eyes are this share of the viewer's apart: the screen's own
distance already reads as depth (start small; tune on the device) */
#define FILM_SEPARATION_SHARE 0.25f
/* the eyes' separation when the views don't give one (the simulator has
one view), in meters */
#define FILM_DEFAULT_SEPARATION 0.064f
/* the film's shape: the letterbox's inside, 640x360 */
#define FILM_ASPECT (16.0f / 9.0f)
/* the screen's half width in meters where there is no screen (side by
side): the theater's default, 60 degrees across at 4 m */
#define FILM_DEFAULT_HALF_WIDTH 2.309f
/* the default 70-degree camera's vertical half tangent in the film:
0.75 * 0.75 * 0.85 * tan(35 degrees) */
#define FILM_DEFAULT_VERTICAL_TANGENT 0.335f

static struct halo_stereo_frame stereo_frame;
static int stereo_layer = HALO_STEREO_LAYER_MONO;
static int stereo_mode = -1; /* read once, on the first frame */

static const char *const mode_names[] = {"off", "head", "screen", "side_by_side"};

/* HEAD mode's look: the head's turn the look hasn't taken in yet, and the
stick's snaps (input.turn = "snap") */
static float head_pending_yaw, head_pending_pitch;
static int snap_turn = -1;
static float snap_radians, snap_pending;
static int snap_armed = 1;
/* the stick past this turns one snap; it must come back inside the release
before the next */
#define SNAP_FLICK 0.7f
#define SNAP_RELEASE 0.3f
/* the game's own pitch limit (player_control_modify_desired_angles) */
#define PITCH_LIMIT (85.5f * 3.14159265f / 180.0f)

static float z_near_world, z_far_world;

/* this frame (and the last) is the 3D film; the screen's half width in
world units */
static int film_frame, film_last;
static float film_half_width;

static void film_frusta(float vertical_tangent);

/* the film's eyes for this frame, from the viewer's separation and the
screen's half width (world units): level, at the camera, FILM_SEPARATION_SHARE
of the viewer's apart; their frusta come from the camera at the render
(halo_stereo_film_frusta), the default camera's until then */
static void film_begin(float viewer_separation, float half_width)
{
	float film_separation = FILM_SEPARATION_SHARE * viewer_separation;
	int eye;

	film_half_width = half_width > 0.0f ? half_width : FILM_DEFAULT_HALF_WIDTH / METERS_PER_UNIT;
	for (eye = 0; eye < 2; eye++) {
		struct halo_stereo_eye *e = &stereo_frame.eyes[eye];

		e->offset[0] = (eye == 0 ? -0.5f : 0.5f) * film_separation;
		e->offset[1] = 0.0f;
		e->offset[2] = 0.0f;
	}
	film_frame = 1;
	film_frusta(FILM_DEFAULT_VERTICAL_TANGENT);
	if (!film_last)
		platform_log("stereo: a cutscene, as a 3D film on the screen: eyes %.1f mm apart (of the viewer's %.1f mm), "
			"the screen %.2f m wide", film_separation * METERS_PER_UNIT * 1000.0f,
			viewer_separation * METERS_PER_UNIT * 1000.0f, 2.0f * film_half_width * METERS_PER_UNIT);
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
		const char *turn = config_string("input.turn");
		int recognized;

		stereo_mode = mode_from_name(name, &recognized);
		platform_log("stereo: display.stereo %s, input.turn %s, input.snap_angle %.1f",
			mode_names[stereo_mode], turn ? turn : "(none)", config_real("input.snap_angle"));
		if (!recognized)
			platform_log("stereo: display.stereo \"%s\" is not recognized; using off", name ? name : "(none)");
	}

	memset(&stereo_frame, 0, sizeof(stereo_frame));
	stereo_frame.mode = stereo_mode;
	stereo_layer = HALO_STEREO_LAYER_MONO;
	film = stereo_mode != HALO_STEREO_OFF && halo_cinematic_screen();
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
		/* HEAD mode: the look takes this frame's turn in next frame
		(player_control runs before the render); a turn it never took is
		dropped. SCREEN mode, and the film, have none */
		head_pending_yaw = stereo_frame.mode == HALO_STEREO_HEAD ? stereo_frame.head_yaw : 0.0f;
		head_pending_pitch = stereo_frame.mode == HALO_STEREO_HEAD ? stereo_frame.head_pitch : 0.0f;
	}
}

int halo_stereo_film(void)
{
	return film_frame && stereo_frame.eye_count == 2;
}

/* The 3D film's frusta. The cinematic camera is the center of a pair of
eyes s apart in the world (FILM_SEPARATION_SHARE of the viewer's), level
with each other whatever the head does, and converged at the distance C
where the camera's view is as wide as the screen at the world's scale:
C = w / T, for the screen's half width w (world units) and the film's
horizontal half tangent T (16:9 at the camera's vertical tangent, which
main.c narrows to the letterbox's inside). An eye x to the side keeps its
frustum's edges on the center frustum's at that distance:
	left = T (1 + x / w)   right = T (1 - x / w)   up = down = vertical
So the film fills the screen exactly as the cinematic camera frames it, what
is C ahead of the camera lies on the screen's surface at its true size, and
on the screen the parallax of something z ahead is s * 3.048 m (1 - C / z):
toward a quarter of the viewer's separation behind the screen at infinity,
and in front of it nearer than C (at the default screen, 1.3 units). The
eyes sit at the camera (no offset back), so the render keeps the camera's
near and far planes. */
static void film_frusta(float vertical_tangent)
{
	float horizontal_tangent = vertical_tangent * FILM_ASPECT;
	int eye;

	for (eye = 0; eye < 2; eye++) {
		struct halo_stereo_eye *e = &stereo_frame.eyes[eye];
		float share = e->offset[0] / film_half_width;

		e->left = horizontal_tangent * (1.0f + share);
		e->right = horizontal_tangent * (1.0f - share);
		e->up = e->down = vertical_tangent;
	}
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

void halo_stereo_stick_look(short gamepad_index, float *yaw, float *pitch)
{
	float magnitude;

	if (!head_tracking(gamepad_index))
		return;
	*pitch = 0.0f;
	if (snap_turn < 0) {
		const char *turn = config_string("input.turn");

		snap_turn = turn && strcmp(turn, "snap") == 0;
		snap_radians = (float)config_real("input.snap_angle") * 3.14159265f / 180.0f;
	}
	if (!snap_turn)
		return;
	/* a snap turns the way the stick would turn the look smoothly */
	magnitude = fabsf(*yaw);
	if (snap_armed && magnitude > SNAP_FLICK) {
		snap_pending += *yaw > 0.0f ? snap_radians : -snap_radians;
		snap_armed = 0;
	} else if (magnitude < SNAP_RELEASE) {
		snap_armed = 1;
	}
	*yaw = 0.0f;
}

int halo_stereo_head_look(short gamepad_index, float *yaw, float *pitch)
{
	*yaw = 0.0f;
	*pitch = 0.0f;
	if (!head_tracking(gamepad_index))
		return 0;
	*yaw = head_pending_yaw + snap_pending;
	*pitch = head_pending_pitch;
	head_pending_yaw = 0.0f;
	head_pending_pitch = 0.0f;
	snap_pending = 0.0f;
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
	static const float world_up[3] = {0.0f, 0.0f, 1.0f};
	float right[3];
	float pitch, turn;

	if (stereo_frame.mode != HALO_STEREO_HEAD || stereo_frame.eye_count != 2)
		return;
	/* yaw about the world's up, left positive, as the game's yaw */
	rotate(forward, world_up, stereo_frame.head_yaw);
	rotate(up, world_up, stereo_frame.head_yaw);
	/* pitch about the camera's right, up positive, inside the game's limit */
	pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward[2])));
	turn = stereo_frame.head_pitch;
	if (pitch + turn > PITCH_LIMIT)
		turn = fmaxf(0.0f, PITCH_LIMIT - pitch);
	if (pitch + turn < -PITCH_LIMIT)
		turn = fminf(0.0f, -PITCH_LIMIT - pitch);
	right[0] = forward[1] * up[2] - forward[2] * up[1];
	right[1] = forward[2] * up[0] - forward[0] * up[2];
	right[2] = forward[0] * up[1] - forward[1] * up[0];
	normalize(right);
	rotate(forward, right, turn);
	rotate(up, right, turn);
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
