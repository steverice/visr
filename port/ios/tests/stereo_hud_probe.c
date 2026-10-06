/* HEAD mode's HUD and UI layout (port/ios/host/host_stereo_hud.c): the
reticle's layer whole at its natural size, centered or along a seat's aim;
one quad per HUD group that drew, showing its whole rectangle, in the bands
above and below the reticle and clear of it, every band element and the UI
inside foveation's sharp region with the head level; the catch-all, the
HUD layer whole where the HUD pass projected it; the zoomed picture's quad,
the level frame's yaw. With --quads WIDTH UI X Y Z it prints the layout's
quads for sample group rectangles instead (one per line: frame, catch-all,
source u0 v0 u1 v1, center, x axis, y axis, layer). */
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

/* a group quad's drawn content, without its margin, in degrees as
level_extent (the margin is the group's own empty texels, so margins may
overlap) */
static void content_extent(const struct host_stereo_hud_quad *quad, float scale, float degrees[4])
{
	struct host_stereo_hud_quad inner = *quad;

	inner.x_axis[0] = fabsf(quad->x_axis[0]) - HOST_STEREO_HUD_GROUP_MARGIN_LINES * scale;
	inner.y_axis[1] = fabsf(quad->y_axis[1]) - HOST_STEREO_HUD_GROUP_MARGIN_LINES * scale;
	level_extent(&inner, degrees);
}

/* a 105 by 90 degree view's half tangents (Vision Pro-like; the HUD pass
takes its projection from the eyes') for the --quads output and the checks */
static const float hud_tangents[2] = { 1.303f, 1.0f };

/* the groups' rectangles as the Mac's 640-line captures measured the
elements (Task 7c's t7c/ folder): the counters, the meters, the tracker, a
prompt and two messages under it; at another width, where the game's rule
puts them (48 lines in at 640 and floor(48 * width / 640) at any width,
rasterizer_xbox.c), the meters anchored right, the rest left */
static void sample_extents(float width, float extents[HALO_HUD_GROUP_COUNT][4])
{
	static const float measured[HALO_HUD_GROUP_COUNT][4] = {
		{ 48, 37, 168, 82 }, { 464, 37, 584, 66 }, { 48, 362, 131, 444 }, { 48, 82.5f, 386, 117 },
		{ 48, 117, 300, 146 } };
	float inset = floorf(48.0f * width / 640.0f);
	int group, axis;

	for (group = 0; group < HALO_HUD_GROUP_COUNT; group++)
		for (axis = 0; axis < 4; axis++)
		{
			float value = measured[group][axis];

			if (axis == 0 || axis == 2)
				value = group == HALO_HUD_GROUP_UNIT ? width - inset - (592.0f - value) : inset + (value - 48.0f);
			extents[group][axis] = value;
		}
	/* on foot there are no seat labels */
	memset(extents[HALO_HUD_GROUP_SEATS], 0, sizeof(extents[0]));
}

static void print_quads(float width, int ui, const float reticle[3])
{
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	float extents[HALO_HUD_GROUP_COUNT][4];
	int count, index;

	sample_extents(width, extents);
	count = host_stereo_hud_layout(width, ui, reticle, hud_tangents, (const float (*)[4])extents, quads);
	for (index = 0; index < count; index++)
	{
		const struct host_stereo_hud_quad *q = &quads[index];

		printf("%d %d %g %g %g %g %g %g %g %g %g %g %g %g %g %d\n", q->frame, q->catch_all, q->source[0], q->source[1],
			q->source[2], q->source[3], q->center[0], q->center[1], q->center[2], q->x_axis[0], q->x_axis[1],
			q->x_axis[2], q->y_axis[0], q->y_axis[1], q->y_axis[2], q->layer);
	}
}

/* the quad showing a layer, or NULL */
static const struct host_stereo_hud_quad *quad_of(const struct host_stereo_hud_quad *quads, int count, int layer)
{
	int index;

	for (index = 0; index < count; index++)
		if (quads[index].layer == layer)
			return &quads[index];
	return NULL;
}

/* whether a quad's source holds the whole rectangle (layout lines) */
static int shows_whole(const struct host_stereo_hud_quad *quad, float width, const float rectangle[4])
{
	return quad && quad->source[0] * width <= rectangle[0] + 1e-3f && quad->source[1] * 480.0f <= rectangle[1] + 1e-3f &&
		quad->source[2] * width >= rectangle[2] - 1e-3f && quad->source[3] * 480.0f >= rectangle[3] - 1e-3f;
}

/* the catch-all: the HUD layer whole, where the HUD pass projected it, so a
layer point (a nav point, the multiplayer score) shows at the direction
the game put it */
static void catch_all_checks(float width)
{
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	/* a seat aiming 60 degrees right: the reticle's quad is there */
	const float seat[3] = { 0.866f, 0.0f, -0.5f };
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	float extents[HALO_HUD_GROUP_COUNT][4];
	int count;
	const struct host_stereo_hud_quad *all;
	/* a nav point drawn at three quarters across and a third down the layer */
	float u = 0.75f, v = 0.33f, x, y, across, up, game_across, game_up;
	char what[200];

	sample_extents(width, extents);
	count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, quads);
	all = &quads[count - 1];
	x = all->center[0] + (2.0f * u - 1.0f) * all->x_axis[0];
	y = all->center[1] + (1.0f - 2.0f * v) * all->y_axis[1];
	across = atanf(x / HOST_STEREO_HUD_DISTANCE);
	up = atanf(y / HOST_STEREO_HUD_DISTANCE);
	game_across = atanf((2.0f * u - 1.0f) * hud_tangents[0]);
	game_up = atanf((1.0f - 2.0f * v) * hud_tangents[1]);
	snprintf(what, sizeof(what), "a nav point in the HUD layer shows at %.2f, %.2f degrees, where the HUD pass "
		"projected it (%.2f, %.2f)", across / DEGREES, up / DEGREES, game_across / DEGREES, game_up / DEGREES);
	check(all->catch_all && all->layer == HOST_STEREO_HUD_LAYER_HUD && all->source[0] == 0.0f &&
		all->source[1] == 0.0f && all->source[2] == 1.0f && all->source[3] == 1.0f &&
		fabsf(across - game_across) < 1e-5f && fabsf(up - game_up) < 1e-5f, what);
	count = host_stereo_hud_layout(width, 0, seat, hud_tangents, (const float (*)[4])extents, quads);
	check(quads[0].layer == HOST_STEREO_HUD_LAYER_RETICLE &&
		fabsf(atan2f(quads[0].center[0], -quads[0].center[2]) / DEGREES - 60.0f) < 0.01f,
		"in a seat the crosshairs' layer is centered along the aim");
	count = host_stereo_hud_layout(width, 0, (const float[3]){ 0.0f, 0.0f, 1.0f }, hud_tangents,
		(const float (*)[4])extents, quads);
	check(!quad_of(quads, count, HOST_STEREO_HUD_LAYER_RETICLE) && quads[count - 1].catch_all,
		"with the aim behind, there's no reticle quad, and the rest stays");
	check(host_stereo_hud_layout(width, 0, ahead, NULL, (const float (*)[4])extents, quads) == count,
		"without the HUD pass's projection there's no catch-all");
}

static void layout_checks(float width)
{
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	float extents[HALO_HUD_GROUP_COUNT][4];
	int count, index, group;
	float reach = 0.0f, reticle_half_degrees = atanf(50.0f * HOST_STEREO_HUD_METERS_PER_LINE /
		HOST_STEREO_HUD_DISTANCE) / DEGREES;
	int inside = 1, clear = 1, sources = 1, above = 0, below = 0, level = 1, whole = 1;
	char what[200];

	sample_extents(width, extents);
	count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, quads);
	printf("layout %.0f lines across (%.3f:1), band scale %.3f mm a line\n", width, width / 480.0f,
		1000.0f * host_stereo_hud_band_scale(width, (const float (*)[4])extents));
	check(count == 1 + HALO_HUD_GROUP_COUNT, "a quad for the reticle, each group that drew (all but the seats) and the catch-all");
	check(quads[0].frame == HOST_STEREO_HUD_HEAD && quads[0].layer == HOST_STEREO_HUD_LAYER_RETICLE,
		"the reticle's layer comes first, head-locked");
	check(fabsf(quads[0].center[0]) < 1e-6f && fabsf(quads[0].center[1]) < 1e-6f &&
		fabsf(quads[0].center[2] + HOST_STEREO_HUD_DISTANCE) < 1e-6f, "the reticle is centered, 2 m ahead");
	check(quads[0].source[0] == 0.0f && quads[0].source[1] == 0.0f && quads[0].source[2] == 1.0f &&
		quads[0].source[3] == 1.0f, "the reticle's quad shows its layer whole, no square cut from anything");
	check(fabsf(quads[0].x_axis[0] * 2.0f - width * HOST_STEREO_HUD_METERS_PER_LINE) < 1e-6f &&
		fabsf(quads[0].y_axis[1] * 2.0f - 480.0f * HOST_STEREO_HUD_METERS_PER_LINE) < 1e-6f,
		"the reticle's layer is at its natural size");
	check(quads[count - 1].catch_all && quads[count - 1].frame == HOST_STEREO_HUD_HEAD,
		"the catch-all comes last, head-locked");
	for (group = 0; group < HALO_HUD_GROUP_COUNT; group++)
		if (group != HALO_HUD_GROUP_SEATS)
			whole &= shows_whole(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + group), width, extents[group]);
	check(whole, "each group's quad shows its whole rectangle, from its own target");
	/* the groups' quads: all but the reticle and the catch-all */
	for (index = 1; index < count - 1; index++)
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
		printf("  group %d: %.1f to %.1f degrees across, %.1f to %.1f up\n",
			quads[index].layer - HOST_STEREO_HUD_LAYER_GROUP, degrees[0], degrees[2], degrees[1], degrees[3]);
	}
	snprintf(what, sizeof(what), "every band element within %.0f degrees across and up (the farthest %.1f)",
		HUD_SHARP_RADIUS_DEGREES, reach);
	check(inside, what);
	check(clear && above > 0 && below > 0, "the bands are above and below the reticle, clear of it");
	check(level, "the bands are level quads in the yaw-following frame");
	check(sources, "the groups are cut from inside their targets");
	/* the groups' drawn rectangles don't overlap on the plane */
	{
		float scale = host_stereo_hud_band_scale(width, (const float (*)[4])extents);
		int a, b, overlap = 0;

		for (a = 1; a < count - 1; a++)
			for (b = a + 1; b < count - 1; b++)
			{
				float da[4], db[4];

				content_extent(&quads[a], scale, da);
				content_extent(&quads[b], scale, db);
				if (da[0] < db[2] - 1e-4f && db[0] < da[2] - 1e-4f && da[1] < db[3] - 1e-4f && db[1] < da[3] - 1e-4f)
					overlap = 1;
			}
		check(!overlap, "no two groups' rectangles overlap");
	}
	/* where Task 7c's pieces were: the counters left and the meters right
	next to the reticle, the prompt and messages above them, the tracker
	below */
	{
		float weapon[4], unit[4], tracker[4], prompt[4], messages[4];
		float scale = host_stereo_hud_band_scale(width, (const float (*)[4])extents);

		content_extent(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON), scale, weapon);
		content_extent(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_UNIT), scale, unit);
		content_extent(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_TRACKER), scale, tracker);
		content_extent(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_PROMPT), scale, prompt);
		content_extent(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_MESSAGES), scale, messages);
		check(weapon[2] < 0.0f && unit[0] > 0.0f && weapon[1] > 0.0f && unit[1] > 0.0f,
			"the weapon's group is left of the center and the unit's right, in the top band");
		check(prompt[1] > weapon[3] && messages[1] > unit[3] && tracker[3] < 0.0f,
			"the prompt and messages sit above them, the tracker below the reticle");
		printf("  prompt %.3f to %.3f up, from %.3f across; messages %.3f to %.3f up, from %.3f across\n", prompt[1],
			prompt[3], prompt[0], messages[1], messages[3], messages[0]);
		check(fabsf(prompt[1] - messages[3]) < 1e-3f && fabsf(prompt[0] - messages[0]) < 1e-3f,
			"the prompt stays over the messages, their left edges together, as CE draws them");
	}
	/* a wider element, such as the needler's icon, or a longer prompt:
	its group's quad grows with it and still shows all of it */
	{
		float wide[HALO_HUD_GROUP_COUNT][4];
		const struct host_stereo_hud_quad *weapon, *prompt;
		int n;

		memcpy(wide, extents, sizeof(wide));
		wide[HALO_HUD_GROUP_WEAPON][2] += 80.0f;
		wide[HALO_HUD_GROUP_PROMPT][2] = fminf(width - 10.0f, wide[HALO_HUD_GROUP_PROMPT][2] + 160.0f);
		n = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])wide, quads);
		weapon = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON);
		prompt = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_PROMPT);
		check(shows_whole(weapon, width, wide[HALO_HUD_GROUP_WEAPON]) &&
			shows_whole(prompt, width, wide[HALO_HUD_GROUP_PROMPT]),
			"a wider weapon icon and a longer prompt show whole, each on its group's quad");
	}
	/* a vehicle's driver seat (the Mac's b30 Warthog: the passenger's label
	and bar at 60 to 140 lines across, 37 to 90 down, the Warthog's bar at
	463 to 597, 36 to 70), and a Scorpion's, which adds the cannon's
	counters top left (CE puts the rider labels under them): the labels are
	the seats' group, in the weapon's slot, left of the center, the bars the
	unit's, right of it; no group crosses the center and no two quads
	overlap */
	{
		float inset = floorf(48.0f * width / 640.0f);
		float driver[HALO_HUD_GROUP_COUNT][4] = { { 0 } };
		int scorpion;

		driver[HALO_HUD_GROUP_SEATS][0] = inset + 12.0f;
		driver[HALO_HUD_GROUP_SEATS][1] = 37.0f;
		driver[HALO_HUD_GROUP_SEATS][2] = inset + 92.0f;
		driver[HALO_HUD_GROUP_SEATS][3] = 90.0f;
		driver[HALO_HUD_GROUP_UNIT][0] = width - inset - 129.0f;
		driver[HALO_HUD_GROUP_UNIT][1] = 36.0f;
		driver[HALO_HUD_GROUP_UNIT][2] = width - inset + 5.0f;
		driver[HALO_HUD_GROUP_UNIT][3] = 70.0f;
		for (scorpion = 0; scorpion < 2; scorpion++)
		{
			const struct host_stereo_hud_quad *unit, *seats, *weapon;
			float scale, du[4], ds[4], dw[4];
			int n, a, b, overlap = 0;
			char what[160];

			if (scorpion)
			{
				/* the cannon's counters, and the rider labels moved under them */
				driver[HALO_HUD_GROUP_WEAPON][0] = inset - 1.0f;
				driver[HALO_HUD_GROUP_WEAPON][1] = 37.0f;
				driver[HALO_HUD_GROUP_WEAPON][2] = inset + 210.0f;
				driver[HALO_HUD_GROUP_WEAPON][3] = 88.0f;
				driver[HALO_HUD_GROUP_SEATS][1] = 92.0f;
				driver[HALO_HUD_GROUP_SEATS][3] = 200.0f;
			}
			n = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])driver, quads);
			scale = host_stereo_hud_band_scale(width, (const float (*)[4])driver);
			unit = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_UNIT);
			seats = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_SEATS);
			weapon = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON);
			content_extent(unit, scale, du);
			content_extent(seats, scale, ds);
			if (weapon)
				content_extent(weapon, scale, dw);
			for (a = 1; a < n - 1; a++)
				for (b = a + 1; b < n - 1; b++)
				{
					float da[4], db[4];

					content_extent(&quads[a], scale, da);
					content_extent(&quads[b], scale, db);
					if (da[0] < db[2] - 1e-4f && db[0] < da[2] - 1e-4f && da[1] < db[3] - 1e-4f && db[1] < da[3] - 1e-4f)
						overlap = 1;
				}
			snprintf(what, sizeof(what), "a %s driver's seat labels sit left of the center, the bars right, %s no quads "
				"overlapping", scorpion ? "Scorpion" : "Warthog", scorpion ? "under the cannon's counters," : "with");
			check(shows_whole(seats, width, driver[HALO_HUD_GROUP_SEATS]) &&
				shows_whole(unit, width, driver[HALO_HUD_GROUP_UNIT]) && ds[2] < 0.0f && du[0] > 0.0f && !overlap &&
				(!scorpion || (weapon && ds[3] <= dw[1] + 1e-3f && fabsf((seats->center[0] - seats->x_axis[0]) -
				(weapon->center[0] - weapon->x_axis[0]) - 13.0f * scale) < 1e-5f)), what);
		}
	}
	/* a group that didn't draw has no quad; with no rectangles at all only
	the reticle and the catch-all are left */
	{
		float some[HALO_HUD_GROUP_COUNT][4];

		memcpy(some, extents, sizeof(some));
		memset(some[HALO_HUD_GROUP_PROMPT], 0, sizeof(some[0]));
		count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])some, quads);
		check(count == HALO_HUD_GROUP_COUNT && !quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_PROMPT),
			"an empty group has no quad");
		check(host_stereo_hud_layout(width, 0, ahead, hud_tangents, NULL, quads) == 2,
			"without the groups' rectangles, only the reticle and the catch-all");
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
	{
		float extents[HALO_HUD_GROUP_COUNT][4];
		struct host_stereo_hud_quad pieces[HOST_STEREO_HUD_MAXIMUM_QUADS];
		int count, piece_count;

		sample_extents(aspect * 480.0f, extents);
		piece_count = host_stereo_hud_layout(aspect * 480.0f, 0, ahead, hud_tangents, (const float (*)[4])extents,
			pieces);
		count = host_stereo_hud_layout(aspect * 480.0f, 1, ahead, hud_tangents, (const float (*)[4])extents, quads);
		check(count == piece_count + 1 && count <= HOST_STEREO_HUD_MAXIMUM_QUADS &&
			!memcmp(quads, pieces, (size_t)piece_count * sizeof(quads[0])) &&
			quads[count - 1].layer == HOST_STEREO_HUD_LAYER_UI && quads[count - 1].frame == HOST_STEREO_HUD_LEVEL &&
			!memcmp(quads[count - 1].center, quad.center, sizeof(quad.center)) &&
			!memcmp(quads[count - 1].x_axis, quad.x_axis, sizeof(quad.x_axis)) &&
			!memcmp(quads[count - 1].y_axis, quad.y_axis, sizeof(quad.y_axis)),
			"a menu: the HUD stays in its pieces, and only the UI layer goes whole on the UI's quad, over them");
	}
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

/* the zoom (halo_stereo.h, "Zoom fills the view"): the zoomed picture whole
on an opaque, head-locked quad on the HUD's plane, as wide and tall as the
view's half tangents there */
static void zoom_checks(void)
{
	struct host_stereo_hud_quad quad;
	const float tangents[2] = { 1.776f, 1.05f };
	const float none[2] = { 0.0f, 1.0f };
	char what[200];

	check(host_stereo_hud_zoom(tangents, &quad) == 1 && quad.opaque && quad.frame == HOST_STEREO_HUD_HEAD &&
		!quad.catch_all, "the zoomed picture is an opaque, head-locked quad");
	check(fabsf(quad.center[0]) < 1e-6f && fabsf(quad.center[1]) < 1e-6f &&
		fabsf(quad.center[2] + HOST_STEREO_HUD_DISTANCE) < 1e-6f && HOST_STEREO_HUD_DISTANCE ==
		HALO_STEREO_ZOOM_DISTANCE_METERS, "straight ahead on the HUD's plane, 2 m away: its depth is the HUD's");
	snprintf(what, sizeof(what), "it spans the view: %.3f by %.3f m there (%.1f by %.1f degrees)",
		2.0f * quad.x_axis[0], 2.0f * quad.y_axis[1], 2.0f * atanf(tangents[0]) / DEGREES,
		2.0f * atanf(tangents[1]) / DEGREES);
	check(fabsf(quad.x_axis[0] - tangents[0] * HOST_STEREO_HUD_DISTANCE) < 1e-5f &&
		fabsf(quad.y_axis[1] - tangents[1] * HOST_STEREO_HUD_DISTANCE) < 1e-5f && quad.x_axis[1] == 0.0f &&
		quad.x_axis[2] == 0.0f && quad.y_axis[0] == 0.0f && quad.y_axis[2] == 0.0f, what);
	check(quad.source[0] == 0.0f && quad.source[1] == 0.0f && quad.source[2] == 1.0f && quad.source[3] == 1.0f,
		"and shows the zoomed picture whole: its frustum is the view's");
	check(host_stereo_hud_zoom(none, &quad) == 0, "without a view's tangents there's none");
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
	const float refresh = 1.0f / 90.0f;
	struct host_stereo_cut cut = HOST_STEREO_CUT_INITIAL;
	int switched, frame, repeat;
	float brightness, last;

	brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, refresh, &switched);
	check(brightness == 1.0f && !switched, "the first view after none shows at once");
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, refresh, &switched) == 1.0f && !switched,
		"the same view again doesn't fade");
	/* a load ends: the UI's quad to the full view, at 90 and at 45 frames a
	second (display.frame_repeat 0 and 1): the same duration, so 6 frames
	and 3 */
	for (repeat = 0; repeat <= 1; repeat++)
	{
		float frame_seconds = refresh * (float)(repeat + 1);
		int frames = repeat ? 3 : 6;
		char what[160];

		cut.shown = HOST_STEREO_VIEW_UI;
		cut.elapsed = HOST_STEREO_CUT_SECONDS;
		brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, frame_seconds, &switched);
		check(switched && brightness == 0.0f, "the UI's quad to the full view starts black");
		last = brightness;
		for (frame = 1; frame < frames; frame++)
		{
			brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, frame_seconds, &switched);
			check(!switched && brightness > last && brightness < 1.0f, "the full view comes up over the cut");
			last = brightness;
		}
		snprintf(what, sizeof(what), "the full view is at full brightness after %d frames at %d frames a second "
			"(%.0f ms)", frames, repeat ? 45 : 90, 1000.0f * HOST_STEREO_CUT_SECONDS);
		check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, frame_seconds, NULL) == 1.0f, what);
	}
	/* the full view to the UI's quad (a load starts), and the UI's quad to
	the screen */
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, refresh, &switched) == 0.0f && switched,
		"the full view to the UI's quad starts black");
	cut.elapsed = HOST_STEREO_CUT_SECONDS;
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_SCREEN, 0, refresh, &switched) == 0.0f && switched,
		"the UI's quad to the screen starts black");
	cut.elapsed = HOST_STEREO_CUT_SECONDS;
	/* a script fade covers the cut */
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 1, refresh, &switched) == 1.0f && !switched,
		"a cut under a script fade doesn't go through black");
	/* no view (mono) forgets the last: the next view shows at once */
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_NONE, 0, refresh, &switched) == 1.0f && !switched &&
		cut.shown == HOST_STEREO_VIEW_NONE, "a frame without a view forgets the last");
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, refresh, &switched) == 1.0f && !switched,
		"a view after none shows at once");
	/* a switch in the middle of a fade starts it over */
	host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, refresh, NULL);
	host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, refresh, NULL);
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, refresh, &switched) == 0.0f && switched,
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
	zoom_checks();
	level_checks();
	cut_checks();
	printf("%s\n", failures ? "stereo_hud_probe: FAILED" : "stereo_hud_probe: PASS");
	return failures ? 1 : 0;
}
