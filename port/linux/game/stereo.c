/*
STEREO.C

The frame's stereo state (port/linux/src/halo_stereo.h): which stereo mode
display.stereo asks for, and, in stereo, where the two eyes are. In HEAD
mode the host's Compositor frame supplies the eyes and the head's turn
(port/ios/host/host_stereo.m), which drives the player's look: the head turns
the view, the right stick turns the body. In SCREEN mode the look stays the
stick's, as in mono, and the game is a 3D TV on the theater screen.

Everything on the screen is a 3D TV: the game's camera rendered twice, the
eyes a short way apart along its right at the camera itself (so the game's
own near plane and culling apply), with frusta skewed so a chosen distance
lies on the screen's surface. One mapping sets the depth of all of it
(screen_mapping_eyes, the stereo spec's "One mapping for the film and the
3D TV"): how far behind the screen infinity sits, as a share of the viewer's
eye separation, and the convergence, the distance ahead of the camera that
lies on the surface. The host reports the viewer's eyes against the screen
(host_stereo.m's screen_eyes); the guest takes from them only the eye
separation, the screen's half width and the head's offset, and maps them.

Cutscenes, in any stereo mode, are the 3D film while the letterbox is in
(halo_stereo_film), and so are moments with a director's scripted camera
(camera_control, the scripted perspective). In HEAD mode so is the
player's own camera while a script holds its look: a camera the head doesn't
steer is easier to watch on a screen. HEAD mode asks the host for SCREEN
eyes meanwhile, so the frame is SCREEN for everyone downstream: the host
draws it on the screen, and the head's turn stays out of the look and the
camera. The film has display.film_depth_share and display.film_convergence,
and doesn't lean.

A third-person camera (a vehicle seat's chase camera) stays immersive in
HEAD mode: the head turns the game's camera for the picture only, relative
to its pose when the camera began, and never the player's facing, so the
sticks drive and aim as in mono (halo_stereo_head_orient). With
display.stereo_vehicle_screen it goes on the screen as the film instead.

SCREEN mode's gameplay is the same picture from the player's camera, with
display.screen_depth_share and display.screen_convergence, and the head's
offset from the screen's axis moves the eyes, as through a small window
(the lean). The first-person weapon has its own nearly flat mapping, its
nearest point on the screen's surface (halo_stereo_first_person_eye, for
rasterizer_set_frustum_z). In SCREEN mode the film's scripted reason counts
only the director's camera, so the player's own camera under a script and a
third-person camera stay gameplay; when the frame's mapping changes, it
eases over MAPPING_EASE_SECONDS rather than popping (no cut covers it).

The screen shows the game's horizontal view across 16:9: main.c narrows
the view by 0.75 while halo_stereo_screen_framing says so, which is the
letterbox's film (held through the film's hold) and, with
display.screen_framing = "band", everything in SCREEN mode. The letterbox
brings bars in for titles (cinematics.c), following the held film.
*/

#include <math.h>
#include <stdio.h>
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
/* port/linux/src/xbox_kernel.c: the frame clock, for the mapping's ease
(with debug.fixed_timestep, presented frames at 1/30 s each) */
int platform_fixed_timestep(void);
unsigned long platform_clock_frames(void);
double halo_frame_trace_milliseconds(void);
/* port/linux/src/d3d8_device.c */
void halo_screen_commit_stereo_scale(void);
#ifdef HALO_IOS
/* port/ios/host/host_stereo.m, imported by the guest (guest_host.h): opens the
Compositor's next frame and fills the eyes and the head's turn from it */
void host_stereo_frame(struct halo_stereo_frame *frame);
#endif

/* the debug side-by-side eyes: fixed frusta and a typical eye separation
(halo_stereo.h, which the side-by-side presenter shares) */
#define SIDE_BY_SIDE_TANGENT HALO_STEREO_SIDE_BY_SIDE_TANGENT
#define SIDE_BY_SIDE_OFFSET HALO_STEREO_SIDE_BY_SIDE_OFFSET

/* one world unit in meters */
#define METERS_PER_UNIT 3.048f
/* the mappings (screen_mapping_eyes): infinity's depth behind the screen as
a share of the viewer's eye separation, and the convergence, the meters
ahead of the camera on the screen's surface. The film's and SCREEN
gameplay's are settings (display.film_depth_share, display.film_convergence,
display.screen_depth_share, display.screen_convergence), whose defaults in
port_config.c are these; the first-person weapon's are fixed, its
convergence the nearest point of its arms and weapon on the surface: Task
7g measured the animation nodes in view along the camera's forward, 0.233 m
for the assault rifle and 0.315 m for the sniper rifle (the pistol wasn't
measured), less 0.05 m for the geometry around the nodes, rounded down */
#define FILM_DEPTH_SHARE 0.25f
#define FILM_CONVERGENCE_METERS 1.75f
#define SCREEN_DEPTH_SHARE 0.3f
#define SCREEN_CONVERGENCE_METERS 1.0f
#define FIRST_PERSON_DEPTH_SHARE 0.05f
#define FIRST_PERSON_CONVERGENCE_METERS 0.15f
/* the settings' ranges: infinity never splits wider than the eyes */
#define DEPTH_SHARE_MIN 0.05f
#define DEPTH_SHARE_MAX 0.9f
#define CONVERGENCE_MIN 0.3f
#define CONVERGENCE_MAX 10.0f
/* the head's offset moves gameplay's eyes by this times the stereo's own
scale (the eyes' separation over the viewer's), each axis clamped to this
many meters */
#define SCREEN_LEAN_SCALE 1.0f
#define SCREEN_LEAN_LIMIT_METERS 0.25f
/* a change of mapping (film to gameplay or back) eases over this long */
#define MAPPING_EASE_SECONDS 0.3f
/* the eyes' separation when the views don't give one (the simulator has
one view, the side-by-side view none), in meters */
#define FILM_DEFAULT_SEPARATION 0.064f
/* the film's shape: the letterbox's inside, 640x360 */
#define FILM_ASPECT (16.0f / 9.0f)
/* the screen's half width in meters where there is no screen (side by
side): the theater's default, 60 degrees across at 4 m */
#define FILM_DEFAULT_HALF_WIDTH 2.309f
/* the default 70-degree camera's vertical half tangent on the screen,
narrowed to 16:9: 0.75 * 0.75 * 0.85 * tan(35 degrees). A placeholder for
the frame's begin only: render.c's eye loop gives the camera's own tangent
(halo_stereo_screen_frusta) before anything reads the eyes */
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

/* One mapping (the stereo spec's "One mapping for the film and the 3D
TV"): the film's, SCREEN gameplay's or the first-person weapon's */
struct screen_mapping
{
	float depth_share;         /* sigma: infinity's on-screen parallax over the viewer's separation */
	float convergence_meters;  /* C: the game-world distance that lies on the screen's surface */
	float lean;                /* the lean's scale: 0, the head's offset doesn't move the eyes; SCREEN_LEAN_SCALE */
};
/* a mapping on its way from one to another over MAPPING_EASE_SECONDS
(screen_mapping_ease): now is this frame's */
struct screen_mapping_easing
{
	struct screen_mapping from, to, now;
	float elapsed;             /* seconds since the target changed */
};

/* this frame (and the last) is the 3D film; this frame (and the last) is
SCREEN gameplay's 3D TV */
static int film_frame, film_last;
static int gameplay_frame, gameplay_last;
/* what this frame's mapping takes: the viewer's eye separation and the
screen's half width (meters), the head's offset from the screen's axis
(meters, right and up, not yet clamped) and the picture's vertical half
tangent (halo_stereo_screen_frusta) */
static float screen_viewer_separation, screen_half_width, screen_head_offset[2], screen_vertical_tangent;
/* the mapping in effect, easing toward the frame's reason's; whether the
last frame was on the screen at all (film or gameplay: a frame that wasn't
takes its mapping at once); which mapping the ease was last logged going to
(0 the film, 1 gameplay; -1 none yet) */
static struct screen_mapping_easing mapping_easing;
static int mapping_on_screen_last, mapping_target_logged = -1;
/* the frame clock's last reading, in seconds (negative: none yet) */
static double mapping_clock = -1.0;
/* the settings, read once (mapping_settings): the film's and gameplay's
mappings; display.screen_framing = "band"; debug.side_by_side_screen and
debug.screen_lean (meters); debug.gpu_stats. film_mapping.depth_share is
negative until read */
static struct screen_mapping film_mapping = { -1.0f, 0.0f, 0.0f }, gameplay_mapping;
static int screen_framing_band = 1, side_by_side_screen, stereo_stats;
static float side_by_side_lean;
/* debug.side_by_side_tangents, read once (mapping_settings): eye 0's left,
right, up and down tangents in the side-by-side view, eye 1 taking left and
right swapped; all four at SIDE_BY_SIDE_TANGENT when the setting is empty.
Asymmetric bounds, as the headset's are, show what symmetric ones hide */
static float side_by_side_tangents[4] = {
	SIDE_BY_SIDE_TANGENT, SIDE_BY_SIDE_TANGENT, SIDE_BY_SIDE_TANGENT, SIDE_BY_SIDE_TANGENT
};
/* display.lod_scale, read once (mapping_settings): HEAD mode's multiplier
on the pixel size the game picks model detail by (halo_stereo_lod_scale) */
#define LOD_SCALE_MIN 0.5f
#define LOD_SCALE_MAX 4.0f
static float lod_scale_setting = 1.0f;
/* the culling camera's distance back, logged under debug.gpu_stats once
for each mode and mapping (halo_stereo_log_culling) */
static unsigned culling_logged;
/* the script fade the eyes draw and the room takes this frame
(halo_stereo_set_fade) */
static float frame_fade[4];
/* display.stereo_vehicle_screen, read once (mapping_settings): in HEAD mode
a third-person camera (a vehicle seat's) goes on the screen as the film
does, the stick still driving it as in mono. Off, it stays head-tracked
(third_person_head). In SCREEN mode it's gameplay either way */
static int vehicle_screen;
/* HEAD mode's third person, without display.stereo_vehicle_screen: this
frame's camera is the game's third-person one, which the head turns for the
picture only (halo_stereo_head_orient); the look takes none of the head's
turn. The turn is the head's since the camera began: its yaw since then
(radians, left positive) and its pitch then, so taking a seat doesn't move
the view by however far the head was turned */
static int third_person_head;
static float third_person_yaw, third_person_pitch_from;
/* the head's yaw in the seat, handed to the look once on the first frame
after a head-tracked third-person camera ends, if that frame is head-tracked
first person: the game glides its camera from the boom to the eyes over
camera_change_pause, so the view is continuous only if the look turns by
it (radians; zero otherwise) */
static float third_person_handover;
/* why the view is on the screen: none, the letterbox, a scripted camera, a
third-person camera; logged as it changes */
static const char *const film_reasons[] = {"none", "a cutscene", "a scripted camera", "a third-person camera"};
static int film_reason, film_reason_logged;
/* the film is held through this many frames with nothing putting the view
on the screen, and leaves on the next: a10 drops its letterbox for two
ticks between two cutscenes (under a white fade), which would otherwise flip
the view to the full view and back */
#define FILM_HOLD_FRAMES 10
static int film_hold;
/* the zoom's inset this frame (halo_stereo_inset_begin): whether the
frame is zoomed in the full view, which keeps the zoom's screen effects out
of the eyes and routes the crosshairs out of the HUD layer, and whether the
inset's pass runs, which it doesn't while its quad won't show; the last
state logged (-1: none yet); the HUD's draws routed into it
(halo_stereo_inset_overlay) */
static int inset_frame, inset_pass, inset_logged = -1, inset_overlay_on;
/* the crosshairs routed out of the HUD layer (halo_stereo_reticle_overlay),
and whether any drew into the reticle's layer this frame */
static int reticle_overlay_on, reticle_drawn;
/* the last stereo frame's HUD layer went whole on the UI's quad */
static int ui_shown_last;

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

/* reads the mappings' settings and the screen's framing, once */
static void mapping_settings(void)
{
	const char *framing;

	if (film_mapping.depth_share >= 0.0f)
		return;
	film_mapping.depth_share = clamped_setting("display.film_depth_share",
		(float)config_real("display.film_depth_share"), DEPTH_SHARE_MIN, DEPTH_SHARE_MAX, "of the eye separation");
	film_mapping.convergence_meters = clamped_setting("display.film_convergence",
		(float)config_real("display.film_convergence"), CONVERGENCE_MIN, CONVERGENCE_MAX, "meters");
	film_mapping.lean = 0.0f;
	gameplay_mapping.depth_share = clamped_setting("display.screen_depth_share",
		(float)config_real("display.screen_depth_share"), DEPTH_SHARE_MIN, DEPTH_SHARE_MAX, "of the eye separation");
	gameplay_mapping.convergence_meters = clamped_setting("display.screen_convergence",
		(float)config_real("display.screen_convergence"), CONVERGENCE_MIN, CONVERGENCE_MAX, "meters");
	gameplay_mapping.lean = SCREEN_LEAN_SCALE;
	framing = config_string("display.screen_framing");
	screen_framing_band = 1;
	if (framing && strcmp(framing, "wide") == 0)
		screen_framing_band = 0;
	else if (!framing || strcmp(framing, "band") != 0)
		platform_log("stereo: display.screen_framing \"%s\" is not recognized; using band", framing ? framing : "(none)");
	side_by_side_screen = config_boolean("debug.side_by_side_screen") != 0;
	vehicle_screen = config_boolean("display.stereo_vehicle_screen") != 0;
	side_by_side_lean = (float)config_real("debug.screen_lean");
	if (side_by_side_lean != side_by_side_lean)
		side_by_side_lean = 0.0f;
	{
		const char *tangents = config_string("debug.side_by_side_tangents");
		float parsed[4];
		int consumed = -1;

		if (tangents && tangents[0] != '\0') {
			/* %n notes how far the four tangents reached: anything after
			them but whitespace ("1,1,0.6,1.4,9") is rejected */
			if (sscanf(tangents, "%f,%f,%f,%f%n", &parsed[0], &parsed[1], &parsed[2], &parsed[3], &consumed) == 4 &&
				consumed >= 0 && tangents[consumed + strspn(tangents + consumed, " \t\r\n")] == '\0' &&
				parsed[0] > 0.0f && parsed[1] > 0.0f && parsed[2] > 0.0f && parsed[3] > 0.0f &&
				parsed[0] <= 4.0f && parsed[1] <= 4.0f && parsed[2] <= 4.0f && parsed[3] <= 4.0f) {
				memcpy(side_by_side_tangents, parsed, sizeof(parsed));
				platform_log("stereo: debug.side_by_side_tangents left %.3f, right %.3f, up %.3f, down %.3f",
					parsed[0], parsed[1], parsed[2], parsed[3]);
			} else
				platform_log("stereo: debug.side_by_side_tangents \"%s\" is not four tangents in (0, 4] "
					"(\"left,right,up,down\"); using %.1f", tangents, SIDE_BY_SIDE_TANGENT);
		}
	}
	stereo_stats = config_boolean("debug.gpu_stats") != 0;
	lod_scale_setting = clamped_setting("display.lod_scale", (float)config_real("display.lod_scale"),
		LOD_SCALE_MIN, LOD_SCALE_MAX, "times the Xbox's pixel scale");
}

/* SCREEN gameplay's eyes rather than HEAD-like ones: SCREEN mode, or the
side-by-side view with debug.side_by_side_screen */
static int screen_mode(void)
{
	return stereo_mode == HALO_STEREO_SCREEN || (stereo_mode == HALO_STEREO_SIDE_BY_SIDE && side_by_side_screen);
}

/* the frame clock's time since its last reading, in seconds, at most a
tenth of a second (a load or a pause doesn't run a whole ease in one frame) */
static float mapping_time_delta(void)
{
	double now = platform_fixed_timestep() ? (double)platform_clock_frames() / 30.0 :
		halo_frame_trace_milliseconds() / 1000.0;
	double delta = mapping_clock < 0.0 ? 0.0 : now - mapping_clock;

	mapping_clock = now;
	return (float)fmax(0.0, fmin(0.1, delta));
}

/* the mapping a mapping's ease takes this frame: linear on the screen,
since sigma and sigma C (and sigma times the lean's scale) move linearly, so
every point's parallax moves at a steady rate, the same at any frame rate,
and stops at the target after MAPPING_EASE_SECONDS. A new target starts
over from where the mapping is now */
static void screen_mapping_ease(struct screen_mapping_easing *easing, const struct screen_mapping *target,
	float time_delta)
{
	const struct screen_mapping *from = &easing->from, *to = &easing->to;
	float t, depth_share;

	if (target->depth_share != to->depth_share || target->convergence_meters != to->convergence_meters ||
		target->lean != to->lean) {
		easing->from = easing->now;
		easing->to = *target;
		easing->elapsed = 0.0f;
	}
	easing->elapsed = fminf(MAPPING_EASE_SECONDS, easing->elapsed + fmaxf(0.0f, time_delta));
	t = easing->elapsed / MAPPING_EASE_SECONDS;
	/* (frame times summed in floats fall a hair short of the whole) */
	if (t >= 0.9999f) {
		easing->now = *to;
		return;
	}
	depth_share = from->depth_share + (to->depth_share - from->depth_share) * t;
	easing->now.depth_share = depth_share;
	easing->now.convergence_meters = (from->depth_share * from->convergence_meters +
		(to->depth_share * to->convergence_meters - from->depth_share * from->convergence_meters) * t) / depth_share;
	easing->now.lean = (from->depth_share * from->lean +
		(to->depth_share * to->lean - from->depth_share * from->lean) * t) / depth_share;
}

/* a mapping's eyes (halo_stereo_tv_eyes has the math) for a picture of
vertical half tangent V, the viewer's eye separation e and the screen's half
width w (meters), with the head's offset h (meters, right and up; NULL for
none): the lean moves each eye by clamp(h, +-SCREEN_LEAN_LIMIT_METERS) times
the mapping's lean scale times s / e, the stereo's own scale, so what lies
at C stays put on the screen, infinity moves with the head by sigma h, and
nearer things against it */
static void screen_mapping_eyes(const struct screen_mapping *mapping, float vertical_tangent,
	float viewer_separation_meters, float screen_half_width_meters, const float head_offset_meters[2],
	struct halo_stereo_eye eyes[2])
{
	float lean[2] = { 0.0f, 0.0f };

	if (head_offset_meters && mapping->lean != 0.0f && viewer_separation_meters > 0.0f &&
		screen_half_width_meters > 0.0f) {
		float separation = mapping->depth_share * viewer_separation_meters * vertical_tangent * FILM_ASPECT *
			(mapping->convergence_meters / METERS_PER_UNIT) / screen_half_width_meters;
		int axis;

		for (axis = 0; axis < 2; axis++) {
			float h = fmaxf(-SCREEN_LEAN_LIMIT_METERS, fminf(SCREEN_LEAN_LIMIT_METERS, head_offset_meters[axis]));

			lean[axis] = h * mapping->lean * separation / viewer_separation_meters;
		}
	}
	halo_stereo_tv_eyes(mapping->depth_share, mapping->convergence_meters, viewer_separation_meters,
		screen_half_width_meters, vertical_tangent, lean, eyes);
}

/* the frame's eyes at a vertical half tangent, by the mapping in effect */
static void screen_frusta(float vertical_tangent)
{
	screen_vertical_tangent = vertical_tangent;
	screen_mapping_eyes(&mapping_easing.now, vertical_tangent, screen_viewer_separation, screen_half_width,
		screen_head_offset, stereo_frame.eyes);
}

/* the screen's eyes for this frame, the film's or SCREEN gameplay's, from
the viewer's separation and the screen's half width (meters) and the head's
offset (meters, or NULL): level, at the camera, by the mapping, which eases
from the last frame's if that was on the screen too. The camera's own
tangent sets them at the render (halo_stereo_screen_frusta), the default
camera's until then */
static void screen_begin(int gameplay, float viewer_separation, float half_width, const float head_offset[2],
	float time_delta)
{
	static const char *const mapping_names[] = {"film", "gameplay"};
	const struct screen_mapping *target = gameplay ? &gameplay_mapping : &film_mapping;
	struct halo_stereo_eye target_eyes[2];
	float separation_mm, infinity_mm;

	screen_viewer_separation = viewer_separation > 0.001f ? viewer_separation : FILM_DEFAULT_SEPARATION;
	screen_half_width = half_width > 0.0f ? half_width : FILM_DEFAULT_HALF_WIDTH;
	screen_head_offset[0] = head_offset ? head_offset[0] : 0.0f;
	screen_head_offset[1] = head_offset ? head_offset[1] : 0.0f;
	if (!mapping_on_screen_last) {
		mapping_easing.from = mapping_easing.to = mapping_easing.now = *target;
		mapping_easing.elapsed = MAPPING_EASE_SECONDS;
	} else {
		if (mapping_target_logged >= 0 && mapping_target_logged != gameplay)
			platform_log("stereo: the screen's mapping easing from %s to %s", mapping_names[mapping_target_logged],
				mapping_names[gameplay]);
		screen_mapping_ease(&mapping_easing, target, time_delta);
	}
	mapping_target_logged = gameplay;
	film_frame = !gameplay;
	gameplay_frame = gameplay;
	screen_frusta(FILM_DEFAULT_VERTICAL_TANGENT);
	/* the logs give the target's eyes at the default camera, not a moment
	of the ease */
	screen_mapping_eyes(target, FILM_DEFAULT_VERTICAL_TANGENT, screen_viewer_separation, screen_half_width, NULL,
		target_eyes);
	separation_mm = (target_eyes[1].offset[0] - target_eyes[0].offset[0]) * METERS_PER_UNIT * 1000.0f;
	infinity_mm = target->depth_share * screen_viewer_separation * 1000.0f;
	if (!gameplay && (!film_last || film_reason != film_reason_logged))
		platform_log("stereo: %s, as a 3D film on the screen: eyes %.1f mm apart (of the viewer's %.1f mm), "
			"converged at %.2f m, infinity %.1f mm behind the screen; the screen %.2f m wide",
			film_reasons[film_reason], separation_mm, screen_viewer_separation * 1000.0f, target->convergence_meters,
			infinity_mm, 2.0f * screen_half_width);
	if (gameplay && !gameplay_last)
		platform_log("stereo: SCREEN gameplay as a 3D TV: eyes %.1f mm apart (of the viewer's %.1f mm), converged "
			"at %.2f m, infinity %.1f mm behind the screen; the weapon converged at %.2f m; framing %s",
			separation_mm, screen_viewer_separation * 1000.0f, target->convergence_meters, infinity_mm,
			FIRST_PERSON_CONVERGENCE_METERS, screen_framing_band ? "band" : "wide");
	if (!gameplay)
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

/* HEAD mode's third person, after the host's frame: whether this frame is
one, and the head's turn since its camera began. The look last took the
head's pose of the last frame if that frame was head-tracked first person
(head_pitch_known), so the turn starts from there, this frame's yaw already
in it; otherwise (after the film) from the head's pose now. The next
first-person frame keeps the view where it was: the look takes the head's
turn in the seat once (third_person_handover), then follows the head */
static void third_person_begin(void)
{
	int third_person = stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2 &&
		!vehicle_screen && halo_third_person_camera();

	if (third_person && !third_person_head) {
		third_person_yaw = head_pitch_known ? stereo_frame.head_yaw : 0.0f;
		third_person_pitch_from = fmaxf(-PITCH_LIMIT, fminf(PITCH_LIMIT,
			head_pitch_known ? head_pitch_now : stereo_frame.head_pitch));
		platform_log("stereo: a third-person camera, head-tracked: the head turns the picture, the sticks drive");
	} else if (third_person) {
		third_person_yaw = remainderf(third_person_yaw + stereo_frame.head_yaw, 2.0f * 3.14159265f);
	}
	third_person_handover = 0.0f;
	if (!third_person && third_person_head) {
		if (stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2)
			third_person_handover = third_person_yaw;
		platform_log("stereo: the head-tracked third-person camera ended; the look takes the head's %.1f degrees",
			third_person_handover * 180.0f / 3.14159265f);
	}
	/* a snap armed in third person (the stick swings the boom there) doesn't
	fire when the look is the head's again: the stick comes back first */
	if (third_person)
		snap_armed = 0;
	third_person_head = third_person;
}

/* render.c's UI spans this frame (halo_stereo_set_ui_span) */
static int ui_span;

void halo_stereo_set_ui_span(int on)
{
	ui_span = on;
}

int halo_stereo_ui_span(void)
{
	return ui_span;
}

/* where the game's crosshair points in the eye cameras' frame (x right, y up,
z back): straight ahead unless halo_stereo_head_orient turned a third-person
camera */
static float reticle_direction[3] = { 0.0f, 0.0f, -1.0f };

/* the game camera's forward (aim) in the frame of the eye camera whose forward
and up are given; NULLs: straight ahead */
static void reticle_set(const float aim[3], const float forward[3], const float up[3])
{
	float right[3];

	if (!aim) {
		reticle_direction[0] = 0.0f;
		reticle_direction[1] = 0.0f;
		reticle_direction[2] = -1.0f;
		return;
	}
	/* the world's z is up and its yaw is left positive, so right is forward
	cross up */
	right[0] = forward[1] * up[2] - forward[2] * up[1];
	right[1] = forward[2] * up[0] - forward[0] * up[2];
	right[2] = forward[0] * up[1] - forward[1] * up[0];
	reticle_direction[0] = aim[0] * right[0] + aim[1] * right[1] + aim[2] * right[2];
	reticle_direction[1] = aim[0] * up[0] + aim[1] * up[1] + aim[2] * up[2];
	reticle_direction[2] = -(aim[0] * forward[0] + aim[1] * forward[1] + aim[2] * forward[2]);
}

/* the HUD pass's projection: half tangents across and up (render.c), 0
until it runs this frame */
static float hud_tangents[2];

void halo_stereo_set_hud_tangents(float across, float up)
{
	hud_tangents[0] = across;
	hud_tangents[1] = up;
}

void halo_stereo_hud_tangents(float tangents[2])
{
	tangents[0] = hud_tangents[0];
	tangents[1] = hud_tangents[1];
}

void halo_stereo_reticle(float direction[3])
{
	direction[0] = reticle_direction[0];
	direction[1] = reticle_direction[1];
	direction[2] = reticle_direction[2];
}

void halo_stereo_frame_begin(void)
{
	int film, screen, on_screen, head_look_frame;
	float time_delta;

	if (stereo_mode < 0) {
		const char *name = config_string("display.stereo");
		int recognized;

		stereo_mode = mode_from_name(name, &recognized);
		turn_settings();
		mapping_settings();
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
	time_delta = mapping_time_delta();

	memset(&stereo_frame, 0, sizeof(stereo_frame));
	stereo_frame.mode = stereo_mode;
	stereo_layer = HALO_STEREO_LAYER_MONO;
	ui_span = 0;
	inset_frame = 0;
	inset_pass = 0;
	inset_overlay_on = 0;
	reticle_overlay_on = 0;
	reticle_drawn = 0;
	reticle_set(NULL, NULL, NULL);
	hud_tangents[0] = hud_tangents[1] = 0.0f;
	memset(frame_fade, 0, sizeof(frame_fade));
	/* why the view is the film. In SCREEN mode everything is on the screen
	already, so only the director's own cameras are the film: the player's
	camera with its look taken away (a10's pod) and a third-person camera
	stay gameplay, which no change of mapping interrupts */
	screen = screen_mode();
	film_reason = 0;
	if (stereo_mode != HALO_STEREO_OFF) {
		if (halo_cinematic_screen())
			film_reason = 1;
		else if (screen ? halo_scripted_director_camera() : halo_scripted_camera())
			film_reason = 2;
		else if (!screen && vehicle_screen && halo_third_person_camera())
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
	gameplay_last = gameplay_frame;
	film_frame = gameplay_frame = 0;
	/* the film's on the screen: in HEAD mode the frame asks for SCREEN eyes */
	if (film && stereo_mode == HALO_STEREO_HEAD)
		stereo_frame.mode = HALO_STEREO_SCREEN;

	if (stereo_mode == HALO_STEREO_SIDE_BY_SIDE) {
		int width = 0, height = 0;
		int eye;

		platform_video_drawable_size(&width, &height);
		if (width <= 0 || height <= 0) {
			mapping_on_screen_last = 0;
			return; /* no drawable yet: mono this frame */
		}
		stereo_frame.eye_count = 2;
		stereo_frame.eye_width = width / 2;
		stereo_frame.eye_height = height;
		for (eye = 0; eye < 2; eye++) {
			struct halo_stereo_eye *e = &stereo_frame.eyes[eye];

			e->offset[0] = eye == 0 ? -SIDE_BY_SIDE_OFFSET : SIDE_BY_SIDE_OFFSET;
			/* eye 1 mirrors eye 0 left to right */
			e->left = side_by_side_tangents[eye == 0 ? 0 : 1];
			e->right = side_by_side_tangents[eye == 0 ? 1 : 0];
			e->up = side_by_side_tangents[2];
			e->down = side_by_side_tangents[3];
		}
		/* the default screen and typical eyes; debug.screen_lean leans */
		if (film || screen) {
			const float lean[2] = { side_by_side_lean, 0.0f };

			screen_begin(!film, FILM_DEFAULT_SEPARATION, FILM_DEFAULT_HALF_WIDTH, lean, time_delta);
		}
	} else if (stereo_mode == HALO_STEREO_HEAD || stereo_mode == HALO_STEREO_SCREEN) {
		int mode = stereo_frame.mode;

#ifdef HALO_IOS
		host_stereo_frame(&stereo_frame);
#endif
		/* no Compositor frame (the space isn't open): mono, in the window */
		if (stereo_frame.eye_count != 2) {
			memset(&stereo_frame, 0, sizeof(stereo_frame));
			stereo_frame.mode = mode;
		} else if (film || screen) {
			/* the viewer's eyes as the host found them (SCREEN eyes: where they
			are against the screen, and frusta through its edges): their
			separation, the screen's half width from the frusta (the tangents
			add to the width over the distance) and the head's offset from
			the screen's axis, the eyes' midpoint */
			const struct halo_stereo_eye *eyes = stereo_frame.eyes;
			float dx = eyes[1].offset[0] - eyes[0].offset[0];
			float dy = eyes[1].offset[1] - eyes[0].offset[1];
			float dz = eyes[1].offset[2] - eyes[0].offset[2];
			float head_offset[2] = {
				0.5f * (eyes[0].offset[0] + eyes[1].offset[0]) * METERS_PER_UNIT,
				0.5f * (eyes[0].offset[1] + eyes[1].offset[1]) * METERS_PER_UNIT,
			};

			screen_begin(!film, sqrtf(dx * dx + dy * dy + dz * dz) * METERS_PER_UNIT,
				eyes[0].offset[2] * (eyes[0].left + eyes[0].right) * 0.5f * METERS_PER_UNIT, head_offset, time_delta);
		}
		third_person_begin();
		head_look_frame = stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2 && !third_person_head;
		/* HEAD mode: the look takes this frame's yaw in next frame
		(player_control runs before the render), and a turn it never took is
		dropped; the look's pitch follows the head's. SCREEN mode, the film
		and HEAD mode's third person have neither */
		head_pending_yaw = head_look_frame ? stereo_frame.head_yaw + third_person_handover : 0.0f;
		head_pitch_known = head_look_frame;
		head_pitch_now = stereo_frame.head_pitch;
	} else
		third_person_head = 0;
	/* without eyes (the space closed, a load) the next zoom logs again, and
	no menu holds the inset back */
	if (stereo_frame.eye_count != 2) {
		inset_logged = -1;
		ui_shown_last = 0;
	}
	on_screen = film_frame || gameplay_frame;
	mapping_on_screen_last = on_screen;
	if (!on_screen)
		mapping_target_logged = -1;
	/* the eyes' picture size from this frame on, not the next */
	if (stereo_mode != HALO_STEREO_OFF)
		halo_screen_commit_stereo_scale();
}

int halo_stereo_film(void)
{
	return film_frame && stereo_frame.eye_count == 2;
}

int halo_stereo_film_letterbox(void)
{
	return halo_stereo_film() && film_reason == 1;
}

int halo_stereo_screen_gameplay(void)
{
	return gameplay_frame && stereo_frame.eye_count == 2;
}

int halo_stereo_hud_split(void)
{
	return stereo_frame.eye_count == 2 && (stereo_frame.mode == HALO_STEREO_HEAD ||
		stereo_frame.mode == HALO_STEREO_SIDE_BY_SIDE) && !halo_stereo_film() && !halo_stereo_screen_gameplay();
}

int halo_stereo_screen_framing(void)
{
	if (stereo_frame.eye_count != 2)
		return 0;
	return halo_stereo_film_letterbox() || (screen_mode() && screen_framing_band);
}

void halo_stereo_screen_frusta(float vertical_tangent)
{
	if ((halo_stereo_film() || halo_stereo_screen_gameplay()) && vertical_tangent > 0.0f)
		screen_frusta(vertical_tangent);
}

int halo_stereo_first_person_eye(struct halo_stereo_eye *eye)
{
	struct screen_mapping weapon;
	struct halo_stereo_eye eyes[2];

	/* every frame on the screen in SCREEN mode, the film's too: the film's
	hold after a cutscene shows the player's first-person camera, weapon and
	all, and the weapon must not lunge out of the screen under the film's
	mapping and snap back when gameplay's begins. Under a true cutscene or a
	director's camera the weapon isn't drawn anyway */
	if (!screen_mode() || !(film_frame || gameplay_frame) || stereo_frame.eye_count != 2 ||
		(stereo_layer != 0 && stereo_layer != 1))
		return 0;
	/* the weapon's mapping, leaning with the scene's (which eases) */
	weapon.depth_share = FIRST_PERSON_DEPTH_SHARE;
	weapon.convergence_meters = FIRST_PERSON_CONVERGENCE_METERS;
	weapon.lean = mapping_easing.now.lean;
	screen_mapping_eyes(&weapon, screen_vertical_tangent, screen_viewer_separation, screen_half_width,
		screen_head_offset, eyes);
	*eye = eyes[stereo_layer];
	return 1;
}

void halo_stereo_log_culling(float distance_back)
{
	unsigned key;

	if (!stereo_stats || stereo_frame.eye_count != 2)
		return;
	key = 1u << ((unsigned)stereo_frame.mode * 3u + (halo_stereo_film() ? 1u : halo_stereo_screen_gameplay() ? 2u : 0u));
	if (culling_logged & key)
		return;
	culling_logged |= key;
	platform_log("stereo: the culling camera %.4f units (%.1f mm) behind the camera, display.stereo %s, %s",
		distance_back, distance_back * METERS_PER_UNIT * 1000.0f, mode_names[stereo_frame.mode],
		halo_stereo_film() ? "the film" : halo_stereo_screen_gameplay() ? "SCREEN gameplay" : "the full view");
}

int halo_stereo_cut_covered(void)
{
	return frame_fade[3] > 0.0f;
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

int halo_stereo_repeat_pass(void)
{
	return stereo_layer == 1 || stereo_layer == HALO_STEREO_LAYER_INSET;
}

/* the zoom's inset: HEAD mode's full view and the side-by-side view (the
Mac's stand-in for it), never the screen (SCREEN gameplay, the film), where
the game's own zoom shows as in mono */
int halo_stereo_inset_begin(int zoomed)
{
	int mode = stereo_frame.mode;

	inset_frame = zoomed && stereo_frame.eye_count == 2 && !halo_stereo_film() && !halo_stereo_screen_gameplay() &&
		(mode == HALO_STEREO_HEAD || mode == HALO_STEREO_SIDE_BY_SIDE);
	/* the pass alone waits while its quad won't show: under the last frame's
	menu, or with a seat's aim off the view (the presenter's rule,
	host_stereo_hud.c). The eyes still leave the zoom out, so they don't
	blink a masked view around a menu */
	inset_pass = inset_frame && !ui_shown_last && reticle_direction[2] <= -0.1f;
	/* the first time, and each change under debug.gpu_stats */
	if (inset_frame != inset_logged && (inset_logged < 0 ? inset_frame : stereo_stats))
		platform_log(inset_frame ? "stereo: zoomed: the zoomed view on the inset, %.0f%% of the eyes' height, its "
			"central %.0f lines on a quad %.2f m wide, %.2f m ahead" : "stereo: unzoomed: no inset",
			100.0f * HALO_STEREO_INSET_HEIGHT_SHARE, HALO_STEREO_INSET_LINES, HALO_STEREO_INSET_WIDTH_METERS,
			HALO_STEREO_INSET_DISTANCE_METERS);
	if (inset_frame || inset_logged >= 0)
		inset_logged = inset_frame;
	return inset_pass;
}

void halo_stereo_set_ui_shown(int shown)
{
	ui_shown_last = shown != 0;
}

float halo_stereo_inset_field_of_view(float magnification)
{
	float half_tangent = HALO_STEREO_INSET_WIDTH_METERS / 2.0f / HALO_STEREO_INSET_DISTANCE_METERS;

	if (!(magnification >= 1.0f))
		magnification = 1.0f;
	return 2.0f * atanf(half_tangent / magnification);
}

int halo_stereo_inset(void)
{
	return inset_pass;
}

/* the farthest the zoom's blur or warp has read past a pixel in the inset's
pass this run, in the screen's lines (halo_stereo_inset_blur_reach) */
static float inset_blur_reach;

void halo_stereo_inset_blur_reach(float lines)
{
	if (!(lines > inset_blur_reach))
		return;
	inset_blur_reach = lines;
	if (stereo_stats || inset_blur_reach + HALO_STEREO_INSET_MARGIN_SLACK_LINES > HALO_STEREO_INSET_MARGIN_LINES)
		platform_log("stereo: the zoom's screen effect reads %.1f lines past a pixel; the inset shades a margin of "
			"%.1f lines beside its square", lines, halo_stereo_inset_margin_lines());
}

float halo_stereo_inset_margin_lines(void)
{
	float margin = inset_blur_reach + HALO_STEREO_INSET_MARGIN_SLACK_LINES;

	return margin > HALO_STEREO_INSET_MARGIN_LINES ? margin : HALO_STEREO_INSET_MARGIN_LINES;
}

int halo_stereo_eye_unzoomed(void)
{
	return inset_frame && (stereo_layer == 0 || stereo_layer == 1);
}

void halo_stereo_inset_overlay(int on)
{
	if (on) {
		if (inset_frame && stereo_layer == HALO_STEREO_LAYER_HUD) {
			stereo_layer = HALO_STEREO_LAYER_INSET;
			inset_overlay_on = 1;
		}
	} else if (inset_overlay_on) {
		stereo_layer = HALO_STEREO_LAYER_HUD;
		inset_overlay_on = 0;
	}
}

void halo_stereo_reticle_overlay(int on)
{
	if (on) {
		if (stereo_layer == HALO_STEREO_LAYER_HUD && (inset_frame || halo_stereo_hud_split())) {
			stereo_layer = inset_frame ? HALO_STEREO_LAYER_INSET : HALO_STEREO_LAYER_RETICLE;
			reticle_overlay_on = 1;
		}
	} else if (reticle_overlay_on) {
		stereo_layer = HALO_STEREO_LAYER_HUD;
		reticle_overlay_on = 0;
	}
}

void halo_stereo_reticle_drew(void)
{
	reticle_drawn = 1;
}

int halo_stereo_reticle_drawn(void)
{
	return reticle_drawn;
}

/* whether the head drives the look: HEAD mode with the Compositor's eyes,
not in third person, where the stick has the look as in mono (the last
frame's state, since the look runs before the frame begins) */
static int head_tracking(short gamepad_index)
{
	return gamepad_index == 0 && stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2 &&
		!third_person_head;
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
	/* not head-tracked, or HEAD mode's third person: the game's own turn and
	pitch, which swing a vehicle's chase camera and aim its guns; no snaps,
	no vignette */
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

/* the eye pass's camera position (halo_stereo.h): render.c's eye loop sets
it around each eye's render_window */
static const union real_point3d *eye_position;

void halo_stereo_set_eye_position(const union real_point3d *position)
{
	eye_position = position;
}

const union real_point3d *halo_stereo_eye_position(void)
{
	return eye_position;
}

float halo_stereo_lod_scale(void)
{
	return stereo_mode == HALO_STEREO_HEAD || stereo_mode == HALO_STEREO_SIDE_BY_SIDE ? lod_scale_setting : 1.0f;
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
	float aim[3] = { forward[0], forward[1], forward[2] };

	if (stereo_frame.mode != HALO_STEREO_HEAD || stereo_frame.eye_count != 2)
		return;
	if (third_person_head) {
		/* the game's third-person camera turned by the head since it began:
		its yaw about the world's up, and its pitch by the head's change of
		pitch, inside the game's limit. The roll is the head's own, as in
		first person, so the camera's up is the room's whenever the camera
		is level */
		yaw = atan2f(forward[1], forward[0]) + third_person_yaw;
		pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward[2]))) + head_pitch_limited() - third_person_pitch_from;
		pitch = fmaxf(-PITCH_LIMIT, fminf(PITCH_LIMIT, pitch));
	} else {
		/* the camera's yaw (the look's, which has the head's turns up to the
		last frame) turned about the world's up by the turn it takes next
		frame (this frame's, and the seat's on leaving one), left positive
		as the game's yaw; then the head's own pitch, inside the
		game's limit, so the camera is level when the head is, whatever the
		look's pitch was */
		yaw = atan2f(forward[1], forward[0]) + head_pending_yaw;
		pitch = head_pitch_limited();
	}
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
	/* a seat's gun aims along the game's camera, which only the stick turns:
	the crosshair goes where that points in the picture the head turned
	(halo_stereo_reticle). On foot the game's look is the head's, and the
	crosshair is straight ahead */
	if (third_person_head) {
		normalize(aim);
		reticle_set(aim, forward, up);
	} else
		reticle_set(NULL, NULL, NULL);
}

void halo_stereo_inset_orient(float forward[3], float up[3])
{
	if (!third_person_head)
		halo_stereo_head_orient(forward, up);
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
