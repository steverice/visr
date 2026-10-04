/* SCREEN mode's 3D TV and the film (port/linux/game/stereo.c): the one
mapping both use (screen_mapping_eyes), the ease between them, the reasons
that pick one in SCREEN mode, the screen's framing, and the first-person
weapon's own eye. stereo.c is included, not linked, as stereo_head_probe.c
does, so the checks reach its static functions.

A point z ahead of the camera, x to its right, lands on a screen of half
width w at w ((x - x_eye) / z + x_eye / C_w) / T for an eye of the mapping
at x_eye (the stereo spec's "One mapping for the film and the 3D TV"); the
checks project through each eye's own tangents, which is the same thing for
the mapping's frusta. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "halo_stereo.h"

/* what the game shows, and the host's eyes */
static const char *setting_stereo = "screen";
static const char *setting_framing = "band";
static int game_letterbox, game_director_camera, game_camera_disabled, game_third_person;
static int host_eyes = 1;
/* the head's offset from the screen's axis, meters right and up */
static float host_head[2];
static unsigned long clock_frames;
static char last_log[512];
static int easing_logs;
static char film_log[512], gameplay_log[512];

int halo_cinematic_screen(void) { return game_letterbox; }
int halo_scripted_camera(void) { return game_camera_disabled || game_director_camera; }
int halo_scripted_director_camera(void) { return game_director_camera; }
int halo_third_person_camera(void) { return game_third_person; }
void halo_screen_commit_stereo_scale(void) {}
void platform_video_drawable_size(int *width, int *height) { *width = 1920; *height = 1080; }
int platform_fixed_timestep(void) { return 1; }
unsigned long platform_clock_frames(void) { return clock_frames; }
double halo_frame_trace_milliseconds(void) { return 0.0; }
void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(last_log, sizeof(last_log), format, arguments);
	va_end(arguments);
	if (strstr(last_log, "the screen's mapping easing from"))
		easing_logs++;
	if (strstr(last_log, "as a 3D film on the screen"))
		strcpy(film_log, last_log);
	if (strstr(last_log, "SCREEN gameplay as a 3D TV"))
		strcpy(gameplay_log, last_log);
}

#include "../../linux/game/stereo.c"

/* stereo.c's settings, at their defaults */
const char *config_string(const char *name)
{
	if (!strcmp(name, "display.stereo"))
		return setting_stereo;
	if (!strcmp(name, "display.screen_framing"))
		return setting_framing;
	if (!strcmp(name, "input.turn"))
		return "snap";
	return "";
}
double config_real(const char *name)
{
	if (!strcmp(name, "display.film_depth_share"))
		return FILM_DEPTH_SHARE;
	if (!strcmp(name, "display.film_convergence"))
		return FILM_CONVERGENCE_METERS;
	if (!strcmp(name, "display.screen_depth_share"))
		return SCREEN_DEPTH_SHARE;
	if (!strcmp(name, "display.screen_convergence"))
		return SCREEN_CONVERGENCE_METERS;
	if (!strcmp(name, "input.snap_angle"))
		return 30.0;
	if (!strcmp(name, "input.smooth_turn_speed"))
		return 120.0;
	return 0.0;
}
int config_boolean(const char *name) { (void)name; return 0; }

/* host_stereo.m's SCREEN eyes: 64 mm apart about the head's offset, 4 m in
front of a screen 4.618 m wide (the theater's default), frusta through its
edges */
void host_stereo_frame(struct halo_stereo_frame *frame)
{
	int eye;

	if (!host_eyes)
		return;
	frame->eye_count = 2;
	for (eye = 0; eye < 2; eye++) {
		struct halo_stereo_eye *e = &frame->eyes[eye];
		float ex = (eye == 0 ? -0.032f : 0.032f) + host_head[0], ey = host_head[1];
		float d = 4.0f, half = 2.309f, half_height = half * 9.0f / 16.0f;

		e->offset[0] = ex / 3.048f;
		e->offset[1] = ey / 3.048f;
		e->offset[2] = d / 3.048f;
		e->left = (half + ex) / d;
		e->right = (half - ex) / d;
		e->up = (half_height - ey) / d;
		e->down = (half_height + ey) / d;
	}
}

static int failures;

static void check(int ok, const char *what)
{
	printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		failures++;
}

#define SEPARATION 0.064f
#define HALF_WIDTH 2.309f
#define VERTICAL 0.335f

/* where a point (world units: right, up, ahead) lands on the screen, in
meters from its center, as the eye sees it through its own frustum */
static void on_screen(const struct halo_stereo_eye *e, const float point[3], float at[2])
{
	float tx = (point[0] - e->offset[0]) / point[2], ty = (point[1] - e->offset[1]) / point[2];
	float half_height = HALF_WIDTH * 9.0f / 16.0f;

	at[0] = -HALF_WIDTH + 2.0f * HALF_WIDTH * (tx + e->left) / (e->left + e->right);
	at[1] = -half_height + 2.0f * half_height * (ty + e->down) / (e->up + e->down);
}

/* the right eye's place minus the left's, meters on the screen */
static float parallax(const struct halo_stereo_eye eyes[2], const float point[3])
{
	float left[2], right[2];

	on_screen(&eyes[0], point, left);
	on_screen(&eyes[1], point, right);
	return right[0] - left[0];
}

static void mapping(const char *name, struct screen_mapping m)
{
	const float c = m.convergence_meters / METERS_PER_UNIT, sigma_e = m.depth_share * SEPARATION;
	const float none[2] = { 0.0f, 0.0f }, lean[2] = { 0.1f, 0.0f }, full[2] = { 1.0f, 0.0f };
	const float limit[2] = { SCREEN_LEAN_LIMIT_METERS, 0.0f };
	float on_axis[3] = { 0.0f, 0.0f, c }, off_axis[3] = { 0.2f * c, -0.1f * c, c };
	float far[3] = { 0.0f, 0.0f, 1e6f }, near[3] = { 0.0f, 0.0f, 0.6f / METERS_PER_UNIT };
	struct halo_stereo_eye eyes[2], leaned[2], half[2], other[2];
	float expected, a[2], b[2], worst = 0.0f;
	int eye;

	printf("%s (sigma %.2f, C %.2f m, lean %.1f):\n", name, m.depth_share, m.convergence_meters, m.lean);
	screen_mapping_eyes(&m, VERTICAL, SEPARATION, HALF_WIDTH, NULL, eyes);
	check(fabsf(parallax(eyes, on_axis)) < 1e-5f && fabsf(parallax(eyes, off_axis)) < 1e-5f,
		"what lies at C has no parallax");
	check(fabsf(parallax(eyes, far) - sigma_e) < 0.01f * sigma_e, "infinity sigma e behind the screen");
	expected = sigma_e * (1.0f - c / near[2]);
	printf("    at 0.6 m: %.2f mm (expected %.2f)\n", parallax(eyes, near) * 1000.0f, expected * 1000.0f);
	check(fabsf(parallax(eyes, near) - expected) < 1e-5f, "a point at 0.6 m has sigma e (1 - C / z)");
	check(eyes[0].offset[2] == 0.0f && eyes[1].offset[2] == 0.0f && eyes[0].offset[1] == 0.0f &&
		eyes[1].offset[1] == 0.0f, "no offset back, and none up without a lean");
	screen_mapping_eyes(&m, VERTICAL * 0.5f, SEPARATION, HALF_WIDTH, NULL, half);
	check(fabsf((half[1].offset[0] - half[0].offset[0]) - 0.5f * (eyes[1].offset[0] - eyes[0].offset[0])) < 1e-8f &&
		fabsf(parallax(half, far) - parallax(eyes, far)) < 1e-6f,
		"zoom: half the tangent halves s, infinity stays where it was");
	/* the lean */
	screen_mapping_eyes(&m, VERTICAL, SEPARATION, HALF_WIDTH, none, other);
	check(!memcmp(other, eyes, sizeof(other)), "no head offset is no lean");
	screen_mapping_eyes(&m, VERTICAL, SEPARATION, HALF_WIDTH, lean, leaned);
	for (eye = 0; eye < 2; eye++) {
		on_screen(&eyes[eye], off_axis, a);
		on_screen(&leaned[eye], off_axis, b);
		worst = fmaxf(worst, fmaxf(fabsf(a[0] - b[0]), fabsf(a[1] - b[1])));
	}
	check(worst < 1e-5f, "a lean leaves what lies at C where it was, in both eyes");
	if (m.lean != 0.0f) {
		for (eye = 0; eye < 2; eye++) {
			on_screen(&eyes[eye], far, a);
			on_screen(&leaned[eye], far, b);
			if (fabsf((b[0] - a[0]) - m.depth_share * lean[0]) > 0.01f * m.depth_share * lean[0])
				worst = 1.0f;
		}
		check(worst < 1e-5f, "a lean of h moves infinity by sigma h, with the head");
		screen_mapping_eyes(&m, VERTICAL, SEPARATION, HALF_WIDTH, full, leaned);
		screen_mapping_eyes(&m, VERTICAL, SEPARATION, HALF_WIDTH, limit, other);
		check(!memcmp(leaned, other, sizeof(other)), "a lean of 1 m is a lean of the limit");
	} else {
		check(!memcmp(leaned, eyes, sizeof(eyes)), "a mapping without a lean ignores the head");
	}
}

static void natural_convergence(void)
{
	const float horizontal = VERTICAL * FILM_ASPECT, w_world = HALF_WIDTH / METERS_PER_UNIT;
	struct screen_mapping m = { 0.25f, w_world / horizontal * METERS_PER_UNIT, 0.0f };
	struct halo_stereo_eye eyes[2];
	float worst = 0.0f;
	int eye;

	printf("the film at the screen's natural distance:\n");
	screen_mapping_eyes(&m, VERTICAL, SEPARATION, HALF_WIDTH, NULL, eyes);
	for (eye = 0; eye < 2; eye++) {
		float x = eyes[eye].offset[0];

		worst = fmaxf(worst, fabsf(eyes[eye].left - horizontal * (1.0f + x / w_world)));
		worst = fmaxf(worst, fabsf(eyes[eye].right - horizontal * (1.0f - x / w_world)));
	}
	check(fabsf((eyes[1].offset[0] - eyes[0].offset[0]) - 0.25f * SEPARATION / METERS_PER_UNIT) < 1e-7f &&
		worst < 1e-6f, "C_w = w / T, sigma 0.25: Task 7's film before the convergence, T (1 +- x / w)");
}

/* steps the ease from the film's mapping to gameplay's at a frame rate;
every frame moves a point at most its total change times the frame's share
of the ease */
static void ease_at(float rate)
{
	const struct screen_mapping film = { FILM_DEPTH_SHARE, FILM_CONVERGENCE_METERS, 0.0f };
	const struct screen_mapping gameplay = { SCREEN_DEPTH_SHARE, SCREEN_CONVERGENCE_METERS, SCREEN_LEAN_SCALE };
	struct screen_mapping_easing easing;
	float points[2][3] = { { 0.0f, 0.0f, 0.5f / METERS_PER_UNIT }, { 0.0f, 0.0f, 1e6f } };
	float start[2], end[2], last[2], worst = 0.0f, dt = 1.0f / rate, elapsed = 0.0f;
	struct halo_stereo_eye eyes[2];
	int p, frame, reached = 0, reached_early = 0;
	char what[128];

	easing.from = easing.to = easing.now = film;
	easing.elapsed = MAPPING_EASE_SECONDS;
	for (p = 0; p < 2; p++) {
		screen_mapping_eyes(&film, VERTICAL, SEPARATION, HALF_WIDTH, NULL, eyes);
		start[p] = last[p] = parallax(eyes, points[p]);
		screen_mapping_eyes(&gameplay, VERTICAL, SEPARATION, HALF_WIDTH, NULL, eyes);
		end[p] = parallax(eyes, points[p]);
	}
	for (frame = 0; frame < (int)(rate); frame++) {
		screen_mapping_ease(&easing, &gameplay, dt);
		elapsed += dt;
		screen_mapping_eyes(&easing.now, VERTICAL, SEPARATION, HALF_WIDTH, NULL, eyes);
		for (p = 0; p < 2; p++) {
			float now = parallax(eyes, points[p]);
			float allowed = fabsf(end[p] - start[p]) * dt / MAPPING_EASE_SECONDS * 1.01f + 1e-7f;

			worst = fmaxf(worst, fabsf(now - last[p]) / allowed);
			last[p] = now;
		}
		if (!memcmp(&easing.now, &gameplay, sizeof(gameplay))) {
			if (!reached && elapsed < MAPPING_EASE_SECONDS - 0.5f * dt)
				reached_early = 1;
			reached = 1;
		} else if (elapsed > MAPPING_EASE_SECONDS + 0.5f * dt) {
			reached = 0;
		}
	}
	snprintf(what, sizeof(what), "at %.0f Hz, no frame moves a point more than its share of 0.3 s (worst %.3f "
		"of the allowance)", rate, worst);
	check(worst <= 1.0f, what);
	check(reached && !reached_early, "and the mapping reaches gameplay's at 0.3 s");
}

static void frames(int count)
{
	int frame;

	for (frame = 0; frame < count; frame++) {
		clock_frames++;
		halo_stereo_frame_begin();
	}
}

/* a fresh SCREEN mode game, gameplay with nothing scripted */
static void reset_game(void)
{
	game_letterbox = game_director_camera = game_camera_disabled = game_third_person = 0;
	host_eyes = 1;
	host_head[0] = host_head[1] = 0.0f;
	frames(3 * FILM_HOLD_FRAMES);
}

static void reasons(void)
{
	float midpoint;
	int logs;

	printf("SCREEN mode's reasons:\n");
	stereo_mode = -1;
	frames(1);
	check(halo_stereo_screen_gameplay() && !halo_stereo_film() && halo_stereo_screen_framing(),
		"gameplay is the 3D TV, framed as the band");
	check(strstr(gameplay_log, "SCREEN gameplay as a 3D TV: eyes 5.0 mm apart (of the viewer's 64.0 mm), converged "
		"at 1.00 m, infinity 19.2 mm behind the screen; the weapon converged at") != NULL &&
		strstr(gameplay_log, "framing band") != NULL, "its log line");
	printf("    %s\n", gameplay_log);
	game_camera_disabled = 1;
	logs = easing_logs;
	frames(20);
	check(halo_stereo_screen_gameplay() && easing_logs == logs,
		"the player's camera under a script stays gameplay, with no change of mapping");
	game_camera_disabled = 0;
	game_third_person = 1;
	frames(20);
	check(halo_stereo_screen_gameplay() && easing_logs == logs, "so does a third-person camera");
	vehicle_screen = 1;
	frames(20);
	check(halo_stereo_screen_gameplay() && easing_logs == logs,
		"display.stereo_vehicle_screen (HEAD mode's) leaves it gameplay");
	vehicle_screen = 0;
	game_third_person = 0;
	game_director_camera = 1;
	frames(1);
	check(halo_stereo_film() && !halo_stereo_screen_gameplay() && easing_logs == logs + 1 &&
		halo_stereo_screen_framing(), "a director's scripted camera is the film, eased into, framed the same");
	game_director_camera = 0;
	game_letterbox = 1;
	frames(20);
	check(halo_stereo_film_letterbox() && halo_stereo_screen_framing(), "the letterbox is the film");
	check(strstr(film_log, "a cutscene, as a 3D film on the screen: eyes 7.2 mm apart (of the viewer's 64.0 mm), "
		"converged at 1.75 m, infinity 16.0 mm behind the screen; the screen 4.62 m wide") != NULL,
		"the film's log line");
	printf("    %s\n", film_log);
	check(mapping_easing.now.depth_share == FILM_DEPTH_SHARE &&
		mapping_easing.now.convergence_meters == FILM_CONVERGENCE_METERS, "the film's mapping, once eased");
	game_letterbox = 0;
	logs = easing_logs;
	frames(FILM_HOLD_FRAMES);
	check(halo_stereo_film() && easing_logs == logs, "held through the hold");
	frames(1);
	check(halo_stereo_screen_gameplay() && easing_logs == logs + 1 && halo_stereo_screen_framing() &&
		mapping_easing.now.depth_share < SCREEN_DEPTH_SHARE, "then gameplay, easing, still framed as the band");
	frames(9);
	check(mapping_easing.now.depth_share == SCREEN_DEPTH_SHARE &&
		mapping_easing.now.convergence_meters == SCREEN_CONVERGENCE_METERS &&
		mapping_easing.now.lean == SCREEN_LEAN_SCALE, "and at gameplay's mapping 0.3 s later (9 frames at 30)");

	/* the head's offset leans gameplay's eyes; the host's own eyes don't
	survive into the frame */
	host_head[0] = 0.1f;
	host_head[1] = -0.05f;
	frames(1);
	halo_stereo_screen_frusta(VERTICAL);
	midpoint = 0.5f * (halo_stereo_frame()->eyes[0].offset[0] + halo_stereo_frame()->eyes[1].offset[0]);
	{
		float s = halo_stereo_frame()->eyes[1].offset[0] - halo_stereo_frame()->eyes[0].offset[0];

		check(fabsf(midpoint - 0.1f * s / SEPARATION) < 1e-7f && halo_stereo_frame()->eyes[0].offset[2] == 0.0f &&
			halo_stereo_frame()->eyes[0].offset[1] < 0.0f, "the head's offset leans the eyes by h s / e, at the camera");
	}
	host_head[0] = host_head[1] = 0.0f;

	/* the immersive space closed: mono, Hor+ */
	host_eyes = 0;
	frames(1);
	check(!halo_stereo_screen_gameplay() && !halo_stereo_screen_framing(), "with no eyes, the window keeps Hor+");
	host_eyes = 1;
	frames(1);

	/* "wide": gameplay unnarrowed, the letterbox still narrowed */
	screen_framing_band = 0;
	check(halo_stereo_screen_gameplay() && !halo_stereo_screen_framing(), "wide: gameplay keeps Hor+");
	game_letterbox = 1;
	frames(1);
	check(halo_stereo_screen_framing(), "wide: the letterbox still narrows");
	screen_framing_band = 1;
	reset_game();
}

static void first_person_eye(void)
{
	const float c = FIRST_PERSON_CONVERGENCE_METERS / METERS_PER_UNIT;
	float on_axis[3] = { 0.0f, 0.0f, c }, low[3] = { 0.15f * c, -0.2f * c, c }, far[3] = { 0.0f, 0.0f, 1e6f };
	struct halo_stereo_eye eyes[2], leaned[2], none;
	float a[2], b[2], worst = 0.0f;
	int eye;

	printf("the first-person weapon's eye (C %.2f m):\n", FIRST_PERSON_CONVERGENCE_METERS);
	reset_game();
	halo_stereo_screen_frusta(VERTICAL);
	for (eye = 0; eye < 2; eye++) {
		halo_stereo_layer(eye);
		if (!halo_stereo_first_person_eye(&eyes[eye]))
			worst = 1.0f;
	}
	check(worst == 0.0f, "a SCREEN gameplay eye has one");
	check(fabsf(parallax(eyes, on_axis)) < 1e-5f && fabsf(parallax(eyes, low)) < 1e-5f,
		"its nearest point lies on the screen's surface");
	check(parallax(eyes, far) <= FIRST_PERSON_DEPTH_SHARE * SEPARATION * 1.0001f && parallax(eyes, far) > 0.0f,
		"the rest at most its share of the eyes behind it");
	printf("    infinity %.2f mm behind; the eyes %.3f mm apart\n", parallax(eyes, far) * 1000.0f,
		(eyes[1].offset[0] - eyes[0].offset[0]) * METERS_PER_UNIT * 1000.0f);
	halo_stereo_layer(HALO_STEREO_LAYER_HUD);
	check(!halo_stereo_first_person_eye(&none), "the HUD layer has none");
	/* the same lean: the weapon holds still on the surface */
	host_head[0] = 0.2f;
	frames(1);
	halo_stereo_screen_frusta(VERTICAL);
	for (eye = 0; eye < 2; eye++) {
		halo_stereo_layer(eye);
		halo_stereo_first_person_eye(&leaned[eye]);
		on_screen(&eyes[eye], low, a);
		on_screen(&leaned[eye], low, b);
		worst = fmaxf(worst, fmaxf(fabsf(a[0] - b[0]), fabsf(a[1] - b[1])));
	}
	check(worst < 1e-5f && leaned[0].offset[0] != eyes[0].offset[0], "leaning, it stays put on the screen");
	host_head[0] = 0.0f;
	game_letterbox = 1;
	frames(1);
	halo_stereo_layer(0);
	check(halo_stereo_film() && halo_stereo_first_person_eye(&none), "the film in SCREEN mode has one");
	/* the film's hold after a cutscene shows the player's camera: the weapon
	stays on the surface through it, not under the film's mapping */
	frames(FILM_HOLD_FRAMES);
	game_letterbox = 0;
	{
		int frame, held = 0, on_surface = 1;

		for (frame = 0; frame < FILM_HOLD_FRAMES + 2; frame++) {
			frames(1);
			halo_stereo_screen_frusta(VERTICAL);
			held += halo_stereo_film();
			for (eye = 0; eye < 2; eye++) {
				halo_stereo_layer(eye);
				if (!halo_stereo_first_person_eye(&leaned[eye]))
					on_surface = 0;
			}
			if (on_surface && (fabsf(parallax(leaned, on_axis)) > 1e-5f ||
				fabsf(parallax(leaned, far) - parallax(eyes, far)) > 1e-6f))
				on_surface = 0;
		}
		check(held == FILM_HOLD_FRAMES && on_surface,
			"through the film's hold and into gameplay, the weapon keeps its own eye, no jump");
	}
	stereo_mode = HALO_STEREO_HEAD;
	game_letterbox = 1;
	frames(1);
	halo_stereo_layer(0);
	check(halo_stereo_film() && !halo_stereo_first_person_eye(&none), "HEAD mode's film has none");
	game_letterbox = 0;
	frames(3 * FILM_HOLD_FRAMES);
	halo_stereo_layer(0);
	check(!halo_stereo_first_person_eye(&none) && !halo_stereo_screen_gameplay() && !halo_stereo_screen_framing(),
		"HEAD mode has none, and keeps its framing");
	stereo_mode = HALO_STEREO_SCREEN;
	halo_stereo_layer(HALO_STEREO_LAYER_MONO);
}

int main(void)
{
	const struct screen_mapping gameplay = { SCREEN_DEPTH_SHARE, SCREEN_CONVERGENCE_METERS, SCREEN_LEAN_SCALE };
	const struct screen_mapping film = { FILM_DEPTH_SHARE, FILM_CONVERGENCE_METERS, 0.0f };
	const struct screen_mapping weapon = { FIRST_PERSON_DEPTH_SHARE, FIRST_PERSON_CONVERGENCE_METERS, SCREEN_LEAN_SCALE };
	struct halo_stereo_eye eyes[2];
	float near[3] = { 0.0f, 0.0f, 0.6f / METERS_PER_UNIT };

	mapping("gameplay", gameplay);
	screen_mapping_eyes(&gameplay, VERTICAL, SEPARATION, HALF_WIDTH, NULL, eyes);
	check(parallax(eyes, near) < 0.0f, "gameplay: a wall at 0.6 m comes in front of the screen");
	mapping("the film", film);
	mapping("the first-person weapon", weapon);
	natural_convergence();
	printf("the ease:\n");
	ease_at(90.0f);
	ease_at(45.0f);
	reasons();
	first_person_eye();
	if (failures) {
		printf("stereo screen probe: %d failed\n", failures);
		return 1;
	}
	printf("stereo screen probe: PASS\n");
	return 0;
}
