/* HEAD mode's HUD and UI layout (port/ios/host/host_stereo_hud.c): every
band element and the UI inside foveation's sharp region with the head level,
the bands above and below the reticle and clear of it, the reticle centered
and at its natural size, a seat's reticle along the game camera's aim, the
zoom's inset's quad, the level frame's yaw. With --quads WIDTH UI X Y Z it prints the layout's quads
instead (one per line: frame, source u0 v0 u1 v1, center, x axis, y axis),
for the Mac's HEAD-mode composite (t7c/head_composite.py in the stereo
plan's folder). */
#include "host_stereo_hud.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEGREES ((float)M_PI / 180.0f)

static int failures;

static void check(int condition, const char *what)
{
	printf("%s: %s\n", condition ? "ok" : "FAILED", what);
	if (!condition)
		failures++;
}

/* a level quad's corners, in degrees from straight ahead, across and up
(the plane is at z = center z) */
static void level_extent(const struct host_stereo_hud_quad *quad, float degrees[4])
{
	float distance = -quad->center[2];
	float x0 = quad->center[0] - fabsf(quad->x_axis[0]), x1 = quad->center[0] + fabsf(quad->x_axis[0]);
	float y0 = quad->center[1] - fabsf(quad->y_axis[1]), y1 = quad->center[1] + fabsf(quad->y_axis[1]);

	degrees[0] = atanf(x0 / distance) / DEGREES;
	degrees[1] = atanf(y0 / distance) / DEGREES;
	degrees[2] = atanf(x1 / distance) / DEGREES;
	degrees[3] = atanf(y1 / distance) / DEGREES;
}

/* a 105 by 90 degree view's half tangents (Vision Pro-like; the HUD pass
takes its projection from the eyes') for the --quads output and the checks */
static const float hud_tangents[2] = { 1.303f, 1.0f };

static void print_quads(float width, int ui, const float reticle[3])
{
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	int count = host_stereo_hud_layout(width, ui, reticle, hud_tangents, quads), index;

	for (index = 0; index < count; index++)
	{
		const struct host_stereo_hud_quad *q = &quads[index];

		if (q->hidden)
			continue;
		printf("%d %d %g %g %g %g %g %g %g %g %g %g %g %g %g\n", q->frame, q->catch_all, q->source[0], q->source[1], q->source[2],
			q->source[3], q->center[0], q->center[1], q->center[2], q->x_axis[0], q->x_axis[1], q->x_axis[2],
			q->y_axis[0], q->y_axis[1], q->y_axis[2]);
	}
}

/* the catch-all: a layer point no piece claims (a nav point, the
multiplayer score) shows at the direction the HUD pass projected it; a
point a piece claims doesn't show there */
static void catch_all_checks(float width)
{
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	/* a seat aiming 60 degrees right: the reticle's quad is there, and the
	center's square still isn't the catch-all's */
	const float seat[3] = { 0.866f, 0.0f, -0.5f };
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	int count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, quads);
	const struct host_stereo_hud_quad *all = &quads[count - 1];
	/* a nav point drawn at three quarters across and a third down the
	layer, outside every piece */
	float u = 0.75f, v = 0.33f;
	float x = all->center[0] + (2.0f * u - 1.0f) * all->x_axis[0];
	float y = all->center[1] + (1.0f - 2.0f * v) * all->y_axis[1];
	float across = atanf(x / HOST_STEREO_HUD_DISTANCE), up = atanf(y / HOST_STEREO_HUD_DISTANCE);
	float game_across = atanf((2.0f * u - 1.0f) * hud_tangents[0]), game_up = atanf((1.0f - 2.0f * v) * hud_tangents[1]);
	char what[200];

	snprintf(what, sizeof(what), "a nav point outside the pieces shows at %.2f, %.2f degrees, where the HUD pass "
		"projected it (%.2f, %.2f)", across / DEGREES, up / DEGREES, game_across / DEGREES, game_up / DEGREES);
	check(!host_stereo_hud_claimed(quads, count, u, v) && fabsf(across - game_across) < 1e-5f &&
		fabsf(up - game_up) < 1e-5f, what);
	check(host_stereo_hud_claimed(quads, count, 0.5f, 0.5f), "the reticle's square is its own, not the catch-all's");
	/* the elements as measured (layout lines: x0, y0, x1, y1), at 640
	lines across (the Mac) and 853 (the visionOS simulator): every corner
	is a piece's, so none shows on the catch-all at its other scale */
	{
		static const float measured[2][5][4] = {
			{ { 48, 37, 168, 82 }, { 48, 82.5f, 386, 146 }, { 464, 37, 584, 66 }, { 48, 362, 131, 444 },
				{ 292, 212, 347, 267 } },
			{ { 62.9f, 36.9f, 182.4f, 82 }, { 62.9f, 82.5f, 298.2f, 117.6f }, { 662, 36.9f, 781.8f, 66 },
				{ 63.1f, 361.3f, 146, 444.7f }, { 398.4f, 211.8f, 453.6f, 266.9f } } };
		static const float widths[2] = { 640.0f, 853.3f };
		int w, e, corner, claimed = 1;

		for (w = 0; w < 2; w++)
		{
			int n = host_stereo_hud_layout(widths[w], 0, ahead, hud_tangents, quads);

			for (e = 0; e < 5; e++)
				for (corner = 0; corner < 4; corner++)
					claimed &= host_stereo_hud_claimed(quads, n, measured[w][e][corner & 1 ? 2 : 0] / widths[w],
						measured[w][e][corner & 2 ? 3 : 1] / 480.0f);
		}
		/* and at 1600 lines, the elements where the game's rule puts them:
		their 640-line offsets from the safe frame, 48 lines in at 640 and
		floor(48 * width / 640) at any width (rasterizer_xbox.c) */
		{
			float inset = floorf(48.0f * 1600.0f / 640.0f);
			int n = host_stereo_hud_layout(1600.0f, 0, ahead, hud_tangents, quads);

			for (e = 0; e < 5; e++)
				for (corner = 0; corner < 4; corner++)
				{
					float x = measured[0][e][corner & 1 ? 2 : 0], y = measured[0][e][corner & 2 ? 3 : 1];

					/* the meters anchor right, the reticle centered, the rest left */
					x = e == 2 ? 1600.0f - inset - (592.0f - x) : e == 4 ? 800.0f + (x - 320.0f) : inset + (x - 48.0f);
					claimed &= host_stereo_hud_claimed(quads, n, x / 1600.0f, y / 480.0f);
				}
		}
		check(claimed, "every measured element, at 640, 853 and 1600 lines across, is inside a piece");
		count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, quads);
	}
	count = host_stereo_hud_layout(width, 0, seat, hud_tangents, quads);
	check(host_stereo_hud_claimed(quads, count, 0.5f, 0.5f) && !quads[0].hidden &&
		fabsf(atan2f(quads[0].center[0], -quads[0].center[2]) / DEGREES - 60.0f) < 0.01f,
		"in a seat the crosshair is on its own quad along the aim, and the catch-all leaves the center to it");
	count = host_stereo_hud_layout(width, 0, (const float[3]){ 0.0f, 0.0f, 1.0f }, hud_tangents, quads);
	check(quads[0].hidden && host_stereo_hud_claimed(quads, count, 0.5f, 0.5f),
		"with the aim behind, the crosshair hides and the catch-all still leaves the center alone");
	check(host_stereo_hud_layout(width, 0, ahead, NULL, quads) == count - 1,
		"without the HUD pass's projection there's no catch-all");
}

static void layout_checks(float width)
{
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	int count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, quads), index;
	float reach = 0.0f, reticle_half_degrees;
	int inside = 1, clear = 1, sources = 1, above = 0, below = 0, level = 1;
	char what[200];

	printf("layout %.0f lines across (%.3f:1), band scale %.3f mm a line\n", width, width / 480.0f,
		1000.0f * host_stereo_hud_band_scale(width));
	check(count >= 2 && quads[0].frame == HOST_STEREO_HUD_HEAD, "the reticle comes first, head-locked");
	check(fabsf(quads[0].center[0]) < 1e-6f && fabsf(quads[0].center[1]) < 1e-6f &&
		fabsf(quads[0].center[2] + HOST_STEREO_HUD_DISTANCE) < 1e-6f, "the reticle is centered, 2 m ahead");
	check(fabsf(quads[0].x_axis[0] - quads[0].y_axis[1]) < 1e-6f &&
		fabsf(quads[0].x_axis[0] * 2.0f - 100.0f * HOST_STEREO_HUD_METERS_PER_LINE) < 1e-6f,
		"the reticle is a square at the natural scale");
	check(fabsf((quads[0].source[0] + quads[0].source[2]) / 2.0f - 0.5f) < 1e-6f &&
		fabsf((quads[0].source[1] + quads[0].source[3]) / 2.0f - 0.5f) < 1e-6f,
		"the reticle is cut from the layer's center");
	reticle_half_degrees = atanf(quads[0].y_axis[1] / HOST_STEREO_HUD_DISTANCE) / DEGREES;
	check(quads[count - 1].catch_all && quads[count - 1].frame == HOST_STEREO_HUD_HEAD,
		"the catch-all comes last, head-locked");
	/* the bands' pieces: all but the reticle and the catch-all */
	count--;
	for (index = 1; index < count; index++)
	{
		float degrees[4];
		int axis;

		if (quads[index].frame != HOST_STEREO_HUD_LEVEL || quads[index].x_axis[1] != 0.0f ||
			quads[index].x_axis[2] != 0.0f || quads[index].y_axis[0] != 0.0f || quads[index].y_axis[2] != 0.0f)
			level = 0;
		level_extent(&quads[index], degrees);
		for (axis = 0; axis < 4; axis++)
			reach = fmaxf(reach, fabsf(degrees[axis]));
		if (fmaxf(fabsf(degrees[0]), fabsf(degrees[2])) > HUD_SHARP_RADIUS_DEGREES ||
			fmaxf(fabsf(degrees[1]), fabsf(degrees[3])) > HUD_SHARP_RADIUS_DEGREES)
			inside = 0;
		/* clear of the reticle's square: wholly above or below it */
		if (degrees[1] >= reticle_half_degrees)
			above++;
		else if (degrees[3] <= -reticle_half_degrees)
			below++;
		else
			clear = 0;
		for (axis = 0; axis < 4; axis++)
			if (quads[index].source[axis] < 0.0f || quads[index].source[axis] > 1.0f)
				sources = 0;
		printf("  piece %d: %.1f to %.1f degrees across, %.1f to %.1f up\n", index, degrees[0], degrees[2],
			degrees[1], degrees[3]);
	}
	snprintf(what, sizeof(what), "every band element within %.0f degrees across and up (the farthest %.1f)",
		HUD_SHARP_RADIUS_DEGREES, reach);
	check(inside, what);
	check(clear && above > 0 && below > 0, "the bands are above and below the reticle, clear of it");
	check(level, "the bands are level quads in the yaw-following frame");
	check(sources, "the pieces are cut from inside the layer");
	/* pieces of a band don't overlap */
	{
		int a, b, overlap = 0;

		for (a = 1; a < count; a++)
			for (b = a + 1; b < count; b++)
			{
				float da[4], db[4];

				level_extent(&quads[a], da);
				level_extent(&quads[b], db);
				if (da[0] < db[2] && db[0] < da[2] && da[1] < db[3] && db[1] < da[3])
					overlap = 1;
			}
		check(!overlap, "no two pieces overlap");
	}
	/* no layer region belongs to two quads: the reticle's square and every
	piece, in the layer (the catch-all, which takes only what's left,
	aside) */
	{
		struct host_stereo_hud_quad all[HOST_STEREO_HUD_MAXIMUM_QUADS];
		int n = host_stereo_hud_layout(width, 0, ahead, hud_tangents, all), a, b, shared = 0;

		for (a = 0; a < n; a++)
			for (b = a + 1; b < n; b++)
				if (!all[a].catch_all && !all[b].catch_all && all[a].source[0] < all[b].source[2] &&
					all[b].source[0] < all[a].source[2] && all[a].source[1] < all[b].source[3] &&
					all[b].source[1] < all[a].source[3])
				{
					printf("  quads %d and %d share the layer's %.1f-%.1f by %.1f-%.1f lines\n", a, b,
						fmaxf(all[a].source[0], all[b].source[0]) * width, fminf(all[a].source[2], all[b].source[2]) * width,
						fmaxf(all[a].source[1], all[b].source[1]) * 480.0f, fminf(all[a].source[3], all[b].source[3]) * 480.0f);
					shared = 1;
				}
		check(!shared, "no two quads (the reticle included) cut the same region of the layer");
	}
	/* the bands' rows: the counters and meters next to the reticle, the
	messages above them */
	{
		float counters[4], messages[4];

		level_extent(&quads[1], counters);
		level_extent(&quads[3], messages);
		check(counters[1] < messages[1], "the always-on counters and meters are nearer the reticle than the messages");
	}
	catch_all_checks(width);
}

static void ui_checks(float aspect)
{
	struct host_stereo_hud_quad quad;
	float degrees[4];
	char what[160];
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];

	host_stereo_hud_ui(aspect, &quad);
	level_extent(&quad, degrees);
	snprintf(what, sizeof(what), "the UI at %.3f:1 is %.1f by %.1f degrees, inside the sharp region, with its shape",
		aspect, degrees[2] - degrees[0], degrees[3] - degrees[1]);
	check(quad.frame == HOST_STEREO_HUD_LEVEL && fmaxf(degrees[2], degrees[3]) <= HUD_SHARP_RADIUS_DEGREES + 1e-3f &&
		fmaxf(degrees[2], degrees[3]) >= HUD_SHARP_RADIUS_DEGREES - 1e-3f &&
		fabsf(quad.x_axis[0] / quad.y_axis[1] - aspect) < 1e-4f && quad.source[2] == 1.0f && quad.source[3] == 1.0f,
		what);
	check(host_stereo_hud_layout(aspect * 480.0f, 1, ahead, hud_tangents, quads) == 1 &&
		quads[0].frame == HOST_STEREO_HUD_LEVEL,
		"a menu in the HUD layer puts the whole layer on the UI's quad, alone");
}

static void reticle_checks(void)
{
	struct host_stereo_hud_quad quad;
	/* a seat: the game camera aims 30 degrees right of where the head
	turned the picture, and 10 degrees up */
	float yaw = -30.0f * DEGREES, pitch = 10.0f * DEGREES;
	float aim[3] = { -sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch) };
	float behind[3] = { 0.3f, 0.0f, 1.0f };
	float facing;

	check(host_stereo_hud_reticle(640.0f, NULL, aim, &quad) == 1, "a seat's reticle shows while its aim is ahead");
	check(fabsf(atan2f(quad.center[0], -quad.center[2]) - 30.0f * DEGREES) < 1e-4f &&
		fabsf(asinf(quad.center[1] / HOST_STEREO_HUD_DISTANCE) - 10.0f * DEGREES) < 1e-4f,
		"a seat's reticle sits along the game camera's aim, 30 degrees right and 10 up");
	facing = (quad.x_axis[1] * quad.y_axis[2] - quad.x_axis[2] * quad.y_axis[1]) * quad.center[0] +
		(quad.x_axis[2] * quad.y_axis[0] - quad.x_axis[0] * quad.y_axis[2]) * quad.center[1] +
		(quad.x_axis[0] * quad.y_axis[1] - quad.x_axis[1] * quad.y_axis[0]) * quad.center[2];
	check(facing < 0.0f && fabsf(quad.x_axis[1]) < 1e-6f, "it faces the eyes, upright");
	check(host_stereo_hud_reticle(640.0f, NULL, behind, &quad) == 0, "it leaves the view when the head turns far from the aim");
	/* Task 11: at a point (where a controller's aim hits), facing back
	along the aim */
	{
		float point[3] = { 0.3f, -0.2f, -4.0f };

		check(host_stereo_hud_reticle(640.0f, point, aim, &quad) == 1 && quad.center[0] == 0.3f &&
			quad.center[1] == -0.2f && quad.center[2] == -4.0f && fabsf(quad.x_axis[1]) < 1e-6f,
			"given a point, the reticle is centered there, upright, facing back along the aim");
	}
}

/* the zoom's inset (halo_stereo.h): the inset layer's central square on an
opaque square quad 0.8 m wide, 2 m ahead (the HUD's plane), by the
reticle's rule */
static void inset_checks(float width)
{
	struct host_stereo_hud_quad quad;
	float ahead[3] = { 0.0f, 0.0f, -1.0f };
	float yaw = -30.0f * DEGREES, pitch = 10.0f * DEGREES;
	float aim[3] = { -sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch) };
	float behind[3] = { 0.3f, 0.0f, 1.0f };
	float point[3] = { 0.1f, -0.3f, -0.4f };
	float side = fminf(480.0f, width);
	char what[160];

	check(host_stereo_hud_inset(width, NULL, ahead, &quad) == 1 && quad.opaque && quad.frame == HOST_STEREO_HUD_HEAD &&
		!quad.catch_all && !quad.hidden, "the inset is an opaque, head-locked quad");
	check(fabsf(quad.center[0]) < 1e-6f && fabsf(quad.center[1]) < 1e-6f &&
		fabsf(quad.center[2] + HOST_STEREO_HUD_DISTANCE) < 1e-6f,
		"on foot it's straight ahead, 2 m away, on the HUD's plane");
	snprintf(what, sizeof(what), "it's 0.8 m wide and square (%.3f by %.3f m, %.1f degrees across)",
		2.0f * quad.x_axis[0], 2.0f * quad.y_axis[1], 2.0f * atanf(quad.x_axis[0] / 2.0f) / DEGREES);
	check(fabsf(2.0f * quad.x_axis[0] - 0.8f) < 1e-6f && fabsf(2.0f * quad.y_axis[1] - 0.8f) < 1e-6f &&
		fabsf(quad.x_axis[1]) < 1e-6f && fabsf(quad.y_axis[0]) < 1e-6f, what);
	snprintf(what, sizeof(what), "at %.0f lines across it shows the layer's central %.0f-line square (u %.4f to %.4f)",
		width, side, quad.source[0], quad.source[2]);
	check(fabsf(quad.source[0] - (width - side) / 2.0f / width) < 1e-6f &&
		fabsf(quad.source[2] - (width + side) / 2.0f / width) < 1e-6f && quad.source[1] == 0.0f &&
		quad.source[3] == 1.0f && fabsf((quad.source[2] - quad.source[0]) * width - (quad.source[3] - quad.source[1]) * 480.0f) <
		1e-3f * width || side < 480.0f, what);
	check(host_stereo_hud_inset(width, NULL, aim, &quad) == 1 &&
		fabsf(atan2f(quad.center[0], -quad.center[2]) - 30.0f * DEGREES) < 1e-4f &&
		fabsf(asinf(quad.center[1] / 2.0f) - 10.0f * DEGREES) < 1e-4f && fabsf(quad.x_axis[1]) < 1e-6f,
		"in a seat it sits along the gun's aim, 30 degrees right and 10 up, upright");
	check(host_stereo_hud_inset(width, NULL, behind, &quad) == 0, "with the aim behind the eyes there's none");
	check(host_stereo_hud_inset(width, point, aim, &quad) == 1 && quad.center[0] == 0.1f && quad.center[1] == -0.3f &&
		quad.center[2] == -0.4f && fabsf(2.0f * quad.x_axis[0] * 2.0f * quad.x_axis[0] +
		2.0f * quad.x_axis[2] * 2.0f * quad.x_axis[2] - 0.64f) < 1e-5f,
		"given a point (Task 11's scope on the gun), it's centered there, facing back along the aim");
}

static void level_checks(void)
{
	float yaws[] = { 0.0f, 40.0f, -120.0f, 179.0f };
	float pitches[] = { 0.0f, 30.0f, -60.0f, 89.0f };
	float rolls[] = { 0.0f, 20.0f };
	size_t y, p, r;
	float worst = 0.0f;
	char what[120];

	for (y = 0; y < 4; y++)
		for (p = 0; p < 4; p++)
			for (r = 0; r < 2; r++)
			{
				/* the device's axes in the room for yaw (left positive), then
				pitch, then roll */
				float a = yaws[y] * DEGREES, b = pitches[p] * DEGREES, c = rolls[r] * DEGREES;
				float forward[3] = { -sinf(a) * cosf(b), sinf(b), -cosf(a) * cosf(b) };
				float level_right[3] = { cosf(a), 0.0f, -sinf(a) };
				float up0[3] = { sinf(a) * sinf(b), cosf(b), cosf(a) * sinf(b) };
				float right[3], back[3], difference;
				int k;

				for (k = 0; k < 3; k++)
				{
					right[k] = level_right[k] * cosf(c) - up0[k] * sinf(c);
					back[k] = -forward[k];
				}
				difference = host_stereo_hud_level_yaw(right, back) - a;
				while (difference > (float)M_PI)
					difference -= 2.0f * (float)M_PI;
				while (difference < -(float)M_PI)
					difference += 2.0f * (float)M_PI;
				/* past 85 degrees the right's yaw is off by up to the roll's
				share; below it, exact */
				if (fabsf(pitches[p]) < 85.0f || rolls[r] == 0.0f)
					worst = fmaxf(worst, fabsf(difference));
			}
	snprintf(what, sizeof(what), "the level frame turns with the head's yaw at any pitch and roll (worst %.5f degrees)",
		worst / DEGREES);
	check(worst < 1e-3f * DEGREES, what);
}

/* the fade through black between the full view, the screen and the UI's
quad (host_stereo_cut_brightness) */
static void cut_checks(void)
{
	struct host_stereo_cut cut = HOST_STEREO_CUT_INITIAL;
	int switched, frame;
	float brightness, last;

	brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, &switched);
	check(brightness == 1.0f && !switched, "the first view after none shows at once");
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, &switched) == 1.0f && !switched,
		"the same view again doesn't fade");
	/* a load ends: the UI's quad to the full view */
	brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, &switched);
	check(switched && brightness == 0.0f, "the UI's quad to the full view starts black");
	last = brightness;
	for (frame = 1; frame < HOST_STEREO_CUT_FRAMES; frame++)
	{
		brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, &switched);
		check(!switched && brightness > last && brightness < 1.0f, "the full view comes up over the cut's frames");
		last = brightness;
	}
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, NULL) == 1.0f,
		"the full view is at full brightness after HOST_STEREO_CUT_FRAMES frames");
	/* the full view to the UI's quad (a load starts), and the screen to it */
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, &switched) == 0.0f && switched,
		"the full view to the UI's quad starts black");
	cut.frames = 0;
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_SCREEN, 0, &switched) == 0.0f && switched,
		"the UI's quad to the screen starts black");
	cut.frames = 0;
	/* a script fade covers the cut */
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 1, &switched) == 1.0f && !switched,
		"a cut under a script fade doesn't go through black");
	/* no view (mono) forgets the last: the next view shows at once */
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_NONE, 0, &switched) == 1.0f && !switched &&
		cut.shown == HOST_STEREO_VIEW_NONE, "a frame without a view forgets the last");
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, &switched) == 1.0f && !switched,
		"a view after none shows at once");
	/* a switch in the middle of a fade starts it over */
	host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, NULL);
	host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, NULL);
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, &switched) == 0.0f && switched,
		"a cut during a fade starts again from black");
}

int main(int argc, char **argv)
{
	if (argc == 7 && !strcmp(argv[1], "--quads"))
	{
		float reticle[3] = { (float)atof(argv[4]), (float)atof(argv[5]), (float)atof(argv[6]) };

		print_quads((float)atof(argv[2]), atoi(argv[3]), reticle);
		return 0;
	}
	/* 4:3 (the Mac runner), 16:9 and the widest the game lays out */
	layout_checks(640.0f);
	layout_checks(854.0f);
	layout_checks(1600.0f);
	ui_checks(4.0f / 3.0f);
	ui_checks(16.0f / 9.0f);
	ui_checks(0.9f);
	reticle_checks();
	inset_checks(640.0f);
	inset_checks(854.0f);
	inset_checks(1600.0f);
	level_checks();
	cut_checks();
	printf("%s\n", failures ? "stereo_hud_probe: FAILED" : "stereo_hud_probe: PASS");
	return failures ? 1 : 0;
}
