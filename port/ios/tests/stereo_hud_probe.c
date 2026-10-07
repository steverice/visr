/* HEAD mode's HUD and UI layout (port/ios/host/host_stereo_hud.c): the
reticle's layer whole at its natural size, centered or along a seat's aim;
one quad per HUD group that drew, showing its whole rectangle, in the
periphery, each group's outer corner at its CE corner's angles from the
display.hud_* settings (clamped to the eyes' shared view), facing the eyes
from 2 m on the level, yaw-only frame; the UI inside foveation's sharp
region; the catch-all, the HUD layer whole where the HUD pass projected it;
the zoomed picture's quad, the level frame's yaw. With --quads WIDTH UI X Y
Z it prints the layout's quads for sample group rectangles instead (one per
line: frame, catch-all, source u0 v0 u1 v1, center, x axis, y axis, layer). */
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
	count = host_stereo_hud_layout(width, ui, reticle, hud_tangents, (const float (*)[4])extents, NULL, quads);
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
	count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, NULL, quads);
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
	count = host_stereo_hud_layout(width, 0, seat, hud_tangents, (const float (*)[4])extents, NULL, quads);
	check(quads[0].layer == HOST_STEREO_HUD_LAYER_RETICLE &&
		fabsf(atan2f(quads[0].center[0], -quads[0].center[2]) / DEGREES - 60.0f) < 0.01f,
		"in a seat the crosshairs' layer is centered along the aim");
	count = host_stereo_hud_layout(width, 0, (const float[3]){ 0.0f, 0.0f, 1.0f }, hud_tangents,
		(const float (*)[4])extents, NULL, quads);
	check(!quad_of(quads, count, HOST_STEREO_HUD_LAYER_RETICLE) && quads[count - 1].catch_all,
		"with the aim behind, there's no reticle quad, and the rest stays");
	check(host_stereo_hud_layout(width, 0, ahead, NULL, (const float (*)[4])extents, NULL, quads) == count,
		"without the HUD pass's projection there's no catch-all");
}

/* the brief's starting angles, which the defaults are */
static const struct host_stereo_hud_placement defaults = HOST_STEREO_HUD_PLACEMENT_DEFAULT;

static float length3(const float v[3])
{
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* a point's angles in the level frame, in degrees: across (right positive)
and up */
static void angles_of(const float point[3], float angles[2])
{
	angles[0] = atan2f(point[0], -point[2]) / DEGREES;
	angles[1] = atan2f(point[1], hypotf(point[0], point[2])) / DEGREES;
}

/* the corner of a group quad's drawn content (the quad less its margin,
HOST_STEREO_HUD_GROUP_MARGIN_LINES at scale times the natural meters a
line) on side x (-1 left, 1 right) and y (1 top, -1 bottom) */
static void content_corner(const struct host_stereo_hud_quad *quad, float scale, int x, int y, float point[3])
{
	float margin = HOST_STEREO_HUD_GROUP_MARGIN_LINES * HOST_STEREO_HUD_METERS_PER_LINE * scale;
	float x_length = length3(quad->x_axis), y_length = length3(quad->y_axis);
	int axis;

	for (axis = 0; axis < 3; axis++)
		point[axis] = quad->center[axis] + (float)x * quad->x_axis[axis] * (x_length - margin) / x_length +
			(float)y * quad->y_axis[axis] * (y_length - margin) / y_length;
}

/* a group quad's drawn content as the box of its corners' angles: across
from, up from, across to, up to, in degrees */
static void content_box(const struct host_stereo_hud_quad *quad, float scale, float box[4])
{
	int x, y;

	box[0] = box[1] = 1e9f;
	box[2] = box[3] = -1e9f;
	for (x = -1; x <= 1; x += 2)
		for (y = -1; y <= 1; y += 2)
		{
			float point[3], angles[2];

			content_corner(quad, scale, x, y, point);
			angles_of(point, angles);
			box[0] = fminf(box[0], angles[0]);
			box[1] = fminf(box[1], angles[1]);
			box[2] = fmaxf(box[2], angles[0]);
			box[3] = fmaxf(box[3], angles[1]);
		}
}

/* whether two group quads' drawn contents overlap: on one plane (a
corner's groups share theirs), as rectangles on it; else as the boxes of
their corners' angles */
static int contents_overlap(const struct host_stereo_hud_quad *a, const struct host_stereo_hud_quad *b, float scale)
{
	float right[3], up[3], a_box[4], b_box[4];
	float margin = HOST_STEREO_HUD_GROUP_MARGIN_LINES * HOST_STEREO_HUD_METERS_PER_LINE * scale;
	int axis, coplanar = 1;

	for (axis = 0; axis < 3; axis++)
	{
		right[axis] = a->x_axis[axis] / length3(a->x_axis);
		up[axis] = a->y_axis[axis] / length3(a->y_axis);
		coplanar &= fabsf(right[axis] - b->x_axis[axis] / length3(b->x_axis)) < 1e-6f &&
			fabsf(up[axis] - b->y_axis[axis] / length3(b->y_axis)) < 1e-6f;
	}
	if (coplanar)
	{
		float a_x = a->center[0] * right[0] + a->center[1] * right[1] + a->center[2] * right[2];
		float a_y = a->center[0] * up[0] + a->center[1] * up[1] + a->center[2] * up[2];
		float b_x = b->center[0] * right[0] + b->center[1] * right[1] + b->center[2] * right[2];
		float b_y = b->center[0] * up[0] + b->center[1] * up[1] + b->center[2] * up[2];
		float a_w = length3(a->x_axis) - margin, a_h = length3(a->y_axis) - margin;
		float b_w = length3(b->x_axis) - margin, b_h = length3(b->y_axis) - margin;

		return fabsf(a_x - b_x) < a_w + b_w - 1e-6f && fabsf(a_y - b_y) < a_h + b_h - 1e-6f;
	}
	content_box(a, scale, a_box);
	content_box(b, scale, b_box);
	return a_box[0] < b_box[2] - 1e-4f && b_box[0] < a_box[2] - 1e-4f && a_box[1] < b_box[3] - 1e-4f &&
		b_box[1] < a_box[3] - 1e-4f;
}

/* whether a group's content corner on side x, y sits at across, up
degrees, within a thousandth of a degree; prints where it is */
static int corner_at(const struct host_stereo_hud_quad *quad, float scale, int x, int y, float across, float up,
	const char *name)
{
	float point[3], angles[2];

	if (!quad)
		return 0;
	content_corner(quad, scale, x, y, point);
	angles_of(point, angles);
	printf("  %s: its %s %s corner at %.4f across, %.4f up (wanted %.1f, %.1f)\n", name, y > 0 ? "top" : "bottom",
		x < 0 ? "left" : "right", angles[0], angles[1], across, up);
	return fabsf(angles[0] - across) < 1e-3f && fabsf(angles[1] - up) < 1e-3f;
}

/* whether a group's quad faces the eyes' midpoint from a plane
HOST_STEREO_HUD_DISTANCE away, upright (its width level), turning with the
head's yaw only, at scale times the natural size, showing its rectangle */
static int on_the_sphere(const struct host_stereo_hud_quad *quad, float width, float scale)
{
	float normal[3], normal_length, plane, facing;
	float natural_width, natural_height;

	normal[0] = quad->x_axis[1] * quad->y_axis[2] - quad->x_axis[2] * quad->y_axis[1];
	normal[1] = quad->x_axis[2] * quad->y_axis[0] - quad->x_axis[0] * quad->y_axis[2];
	normal[2] = quad->x_axis[0] * quad->y_axis[1] - quad->x_axis[1] * quad->y_axis[0];
	normal_length = length3(normal);
	/* the plane's distance from the eyes, and whether its front (the
	texture's, u right and v up) faces them */
	plane = fabsf(normal[0] * quad->center[0] + normal[1] * quad->center[1] + normal[2] * quad->center[2]) /
		normal_length;
	facing = normal[0] * quad->center[0] + normal[1] * quad->center[1] + normal[2] * quad->center[2];
	natural_width = (quad->source[2] - quad->source[0]) * width * HOST_STEREO_HUD_METERS_PER_LINE * scale;
	natural_height = (quad->source[3] - quad->source[1]) * HOST_STEREO_HUD_LINES * HOST_STEREO_HUD_METERS_PER_LINE *
		scale;
	return quad->frame == HOST_STEREO_HUD_LEVEL && fabsf(plane - HOST_STEREO_HUD_DISTANCE) < 1e-4f && facing < 0.0f &&
		fabsf(quad->x_axis[1]) < 1e-6f && fabsf(2.0f * length3(quad->x_axis) - natural_width) < 1e-5f &&
		fabsf(2.0f * length3(quad->y_axis) - natural_height) < 1e-5f;
}

/* the placement in "The HUD in the periphery": each group's outer corner
(the edge nearest its CE corner) at the table's angles on the level,
yaw-only frame, the weapon top left, the unit top right, the tracker bottom
left, the prompt and the messages top left from display.hud_messages_up
down; a wider element grows toward the center */
static void layout_checks(float width)
{
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	float extents[HALO_HUD_GROUP_COUNT][4];
	const struct host_stereo_hud_quad *weapon, *unit, *tracker, *prompt, *messages;
	int count, index, group, sphere = 1, sources = 1, whole = 1, inside = 1;
	float reach = 0.0f;
	char what[200];

	sample_extents(width, extents);
	count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, NULL, quads);
	printf("layout %.0f lines across (%.3f:1)\n", width, width / 480.0f);
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
		float box[4];
		int axis;

		sphere &= on_the_sphere(&quads[index], width, 1.0f);
		content_box(&quads[index], 1.0f, box);
		for (axis = 0; axis < 4; axis++)
		{
			reach = fmaxf(reach, fabsf(box[axis]));
			if (quads[index].source[axis] < 0.0f || quads[index].source[axis] > 1.0f)
				sources = 0;
		}
		printf("  group %d: %.2f to %.2f degrees across, %.2f to %.2f up\n",
			quads[index].layer - HOST_STEREO_HUD_LAYER_GROUP, box[0], box[2], box[1], box[3]);
	}
	check(sphere, "every group's quad faces the eyes' midpoint from 2 m, upright in the level, yaw-only frame, at "
		"0.072 degrees a line");
	check(sources, "the groups are cut from inside their targets");
	snprintf(what, sizeof(what), "every group stays inside the eyes' shared view, 40 degrees across and up (the "
		"farthest %.2f)", reach);
	inside = reach <= 40.0f;
	check(inside, what);
	weapon = quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON);
	unit = quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_UNIT);
	tracker = quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_TRACKER);
	prompt = quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_PROMPT);
	messages = quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_MESSAGES);
	check(corner_at(weapon, 1.0f, -1, 1, -defaults.across, defaults.up, "weapon"),
		"the weapon's top left corner is at -28 across, +20 up");
	check(corner_at(unit, 1.0f, 1, 1, defaults.across, defaults.up, "unit"),
		"the unit's top right corner is at +28 across, +20 up");
	check(corner_at(tracker, 1.0f, -1, -1, -defaults.across, -defaults.tracker_down, "tracker"),
		"the tracker's bottom left corner is at -28 across, -22 up");
	check(corner_at(prompt, 1.0f, -1, 1, -defaults.across, defaults.messages_up, "prompt"),
		"the prompt's top left corner is at -28 across, +12 up");
	/* the messages hang under the prompt, left edges together, as CE draws
	them (the sample's prompt ends where the messages begin) */
	{
		float prompt_corner[3], messages_corner[3];
		int axis, together = 1;

		content_corner(prompt, 1.0f, -1, -1, prompt_corner);
		content_corner(messages, 1.0f, -1, 1, messages_corner);
		for (axis = 0; axis < 3; axis++)
			together &= fabsf(prompt_corner[axis] - messages_corner[axis]) < 1e-5f;
		check(together, "the messages hang under the prompt, their left edges together, as CE draws them");
	}
	/* no two groups' drawn content overlaps */
	{
		int a, b, overlap = 0;

		for (a = 1; a < count - 1; a++)
			for (b = a + 1; b < count - 1; b++)
				overlap |= contents_overlap(&quads[a], &quads[b], 1.0f);
		check(!overlap, "no two groups' rectangles overlap");
	}
	/* a wider element, such as the needler's icon, or a longer prompt:
	its group's quad grows with it, toward the center, and still shows all
	of it, its outer corner where it was */
	{
		float wide[HALO_HUD_GROUP_COUNT][4], narrow_box[4], wide_box[4];
		const struct host_stereo_hud_quad *wide_weapon, *wide_prompt;
		int n;

		content_box(weapon, 1.0f, narrow_box);
		memcpy(wide, extents, sizeof(wide));
		wide[HALO_HUD_GROUP_WEAPON][2] += 80.0f;
		wide[HALO_HUD_GROUP_PROMPT][2] = fminf(width - 10.0f, wide[HALO_HUD_GROUP_PROMPT][2] + 160.0f);
		n = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])wide, NULL, quads);
		wide_weapon = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON);
		wide_prompt = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_PROMPT);
		content_box(wide_weapon, 1.0f, wide_box);
		check(shows_whole(wide_weapon, width, wide[HALO_HUD_GROUP_WEAPON]) &&
			shows_whole(wide_prompt, width, wide[HALO_HUD_GROUP_PROMPT]),
			"a wider weapon icon and a longer prompt show whole, each on its group's quad");
		check(corner_at(wide_weapon, 1.0f, -1, 1, -defaults.across, defaults.up, "wider weapon") &&
			corner_at(wide_prompt, 1.0f, -1, 1, -defaults.across, defaults.messages_up, "longer prompt") &&
			wide_box[2] > narrow_box[2] + 5.0f, "they grow toward the center; their outer corners stay");
	}
	/* a vehicle's driver seat (the Mac's b30 Warthog: the passenger's label
	and bar at 60 to 140 lines across, 37 to 90 down, the Warthog's bar at
	463 to 597, 36 to 70), and a Scorpion's, which adds the cannon's
	counters top left (CE puts the rider labels under them): the labels are
	the seats' group, in the weapon's corner (CE anchors them top left), the
	bars the unit's, top right; next to the counters the labels keep CE's
	place relative to them, and no two quads overlap */
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
			const struct host_stereo_hud_quad *seat_unit, *seats, *cannon;
			int n, a, b, overlap = 0, placed;
			char name[160];

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
			n = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])driver, NULL, quads);
			seat_unit = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_UNIT);
			seats = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_SEATS);
			cannon = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON);
			for (a = 1; a < n - 1; a++)
				for (b = a + 1; b < n - 1; b++)
					overlap |= contents_overlap(&quads[a], &quads[b], 1.0f);
			if (!scorpion)
				placed = corner_at(seats, 1.0f, -1, 1, -defaults.across, defaults.up, "Warthog seats");
			else
			{
				/* the labels 13 lines right of the counters' left edge and 55
				below their top, on the counters' plane */
				float from[3], to[3], offset[3], right, down, k = HOST_STEREO_HUD_METERS_PER_LINE;
				int axis;

				content_corner(cannon, 1.0f, -1, 1, from);
				content_corner(seats, 1.0f, -1, 1, to);
				for (axis = 0; axis < 3; axis++)
					offset[axis] = to[axis] - from[axis];
				right = (offset[0] * cannon->x_axis[0] + offset[1] * cannon->x_axis[1] + offset[2] * cannon->x_axis[2]) /
					length3(cannon->x_axis);
				down = -(offset[0] * cannon->y_axis[0] + offset[1] * cannon->y_axis[1] + offset[2] * cannon->y_axis[2]) /
					length3(cannon->y_axis);
				printf("  Scorpion seats: %.2f lines right of the counters, %.2f lines below them\n", right / k, down / k);
				placed = corner_at(cannon, 1.0f, -1, 1, -defaults.across, defaults.up, "Scorpion counters") &&
					fabsf(right - 13.0f * k) < 1e-5f && fabsf(down - 55.0f * k) < 1e-5f;
			}
			snprintf(name, sizeof(name), "a %s driver's seat labels sit in the top left corner%s, the bars top right, "
				"no quads overlapping", scorpion ? "Scorpion" : "Warthog", scorpion ? ", under the cannon's counters" : "");
			check(shows_whole(seats, width, driver[HALO_HUD_GROUP_SEATS]) &&
				shows_whole(seat_unit, width, driver[HALO_HUD_GROUP_UNIT]) && placed &&
				corner_at(seat_unit, 1.0f, 1, 1, defaults.across, defaults.up, "seat unit") && !overlap, name);
		}
	}
	/* a group that didn't draw has no quad; with no rectangles at all only
	the reticle and the catch-all are left */
	{
		float some[HALO_HUD_GROUP_COUNT][4];

		memcpy(some, extents, sizeof(some));
		memset(some[HALO_HUD_GROUP_PROMPT], 0, sizeof(some[0]));
		count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])some, NULL, quads);
		check(count == HALO_HUD_GROUP_COUNT && !quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_PROMPT),
			"an empty group has no quad");
		check(corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_MESSAGES), 1.0f, -1, 1,
			-defaults.across, defaults.messages_up, "messages alone"),
			"without a prompt, the messages' top left corner is at -28 across, +12 up");
		check(host_stereo_hud_layout(width, 0, ahead, hud_tangents, NULL, NULL, quads) == 2,
			"without the groups' rectangles, only the reticle and the catch-all");
	}
	catch_all_checks(width);
}

/* the settings (display.hud_corner_across, _corner_up, _tracker_down,
_messages_up, _scale): the angles move the corners, the scale the size,
each angle clamped to the eyes' shared view, the edge included; a head
pitch or roll moves no piece, a body yaw moves them all */
static void placement_checks(void)
{
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	const float width = 854.0f;
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS], natural[HOST_STEREO_HUD_MAXIMUM_QUADS];
	float extents[HALO_HUD_GROUP_COUNT][4];
	int count, index;

	sample_extents(width, extents);
	/* a bracket from the headset's checklist */
	{
		struct host_stereo_hud_placement placement = { 20.0f, 25.0f, 18.0f, 10.0f, 1.0f };

		count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, &placement, quads);
		check(corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON), 1.0f, -1, 1, -20.0f,
			25.0f, "weapon") &&
			corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_UNIT), 1.0f, 1, 1, 20.0f, 25.0f,
			"unit") &&
			corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_TRACKER), 1.0f, -1, -1, -20.0f,
			-18.0f, "tracker") &&
			corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_PROMPT), 1.0f, -1, 1, -20.0f,
			10.0f, "prompt"),
			"the settings' angles move each corner: across 20, up 25, tracker 18 down, messages 10 up");
	}
	/* display.hud_scale: twice the size, the corners where they were */
	{
		struct host_stereo_hud_placement placement = HOST_STEREO_HUD_PLACEMENT_DEFAULT;
		int natural_count, sized = 1, corners;

		placement.scale = 2.0f;
		natural_count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, NULL, natural);
		count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, &placement, quads);
		for (index = 1; index < count - 1 && count == natural_count; index++)
			sized &= on_the_sphere(&quads[index], width, 2.0f) &&
				fabsf(length3(quads[index].x_axis) - 2.0f * length3(natural[index].x_axis)) < 1e-5f;
		corners = corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON), 2.0f, -1, 1,
			-defaults.across, defaults.up, "weapon at scale 2") &&
			corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_TRACKER), 2.0f, -1, -1,
			-defaults.across, -defaults.tracker_down, "tracker at scale 2");
		check(count == natural_count && sized && corners && quads[0].x_axis[0] == natural[0].x_axis[0],
			"display.hud_scale 2 doubles every group's size, keeps its outer corner, and leaves the reticle alone");
	}
	/* the clamp: each angle 0 to 40 degrees, the edge included; the scale
	0.5 to 2; not a number takes the default */
	{
		struct host_stereo_hud_placement placement = { 90.0f, -5.0f, 41.0f, 40.0f, 0.0f };
		struct host_stereo_hud_placement unchanged = HOST_STEREO_HUD_PLACEMENT_DEFAULT;
		struct host_stereo_hud_placement odd = { NAN, 20.0f, 22.0f, 12.0f, NAN };
		float reach = 0.0f;

		check(host_stereo_hud_placement_clamp(&placement) && placement.across == 40.0f && placement.up == 0.0f &&
			placement.tracker_down == 40.0f && placement.messages_up == 40.0f && placement.scale == 0.5f,
			"the angles clamp to 0 to 40 degrees, 40 itself kept, and the scale to 0.5");
		check(!host_stereo_hud_placement_clamp(&unchanged) && !memcmp(&unchanged, &defaults, sizeof(defaults)),
			"the defaults pass unchanged");
		check(host_stereo_hud_placement_clamp(&odd) && odd.across == defaults.across && odd.scale == defaults.scale,
			"a setting that isn't a number takes its default");
		/* at the edge, every corner of every group stays inside 40 degrees */
		placement = (struct host_stereo_hud_placement){ 40.0f, 40.0f, 40.0f, 40.0f, 2.0f };
		count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, &placement, quads);
		for (index = 1; index < count - 1; index++)
		{
			float box[4];
			int axis;

			content_box(&quads[index], 2.0f, box);
			for (axis = 0; axis < 4; axis++)
				reach = fmaxf(reach, fabsf(box[axis]));
		}
		{
			char what[160];

			snprintf(what, sizeof(what), "at the clamp's edge and twice the size every group stays inside the shared "
				"view, 40 degrees (the farthest %.4f)", reach);
			check(reach <= 40.0f + 1e-3f, what);
		}
	}
	/* a head pitch or roll moves none of them; a body yaw moves all of them
	(the level frame, host_stereo_hud_level_yaw, at the device: x' = x cos
	yaw + z sin yaw, z' = z cos yaw - x sin yaw) */
	{
		float pitches[] = { 0.0f, 35.0f, -45.0f }, rolls[] = { 0.0f, 25.0f }, yaws[] = { 0.0f, 30.0f };
		float rooms[2][3][2][HOST_STEREO_HUD_MAXIMUM_QUADS][3];
		size_t y, p, r;
		float still = 0.0f, turned = 1e9f;

		count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, NULL, quads);
		for (y = 0; y < 2; y++)
			for (p = 0; p < 3; p++)
				for (r = 0; r < 2; r++)
				{
					float a = yaws[y] * DEGREES, b = pitches[p] * DEGREES, c = rolls[r] * DEGREES;
					float forward[3] = { -sinf(a) * cosf(b), sinf(b), -cosf(a) * cosf(b) };
					float level_right[3] = { cosf(a), 0.0f, -sinf(a) };
					float up0[3] = { sinf(a) * sinf(b), cosf(b), cosf(a) * sinf(b) };
					float right[3], back[3], yaw;
					int k;

					for (k = 0; k < 3; k++)
					{
						right[k] = level_right[k] * cosf(c) - up0[k] * sinf(c);
						back[k] = -forward[k];
					}
					yaw = host_stereo_hud_level_yaw(right, back);
					for (index = 1; index < count - 1; index++)
					{
						const float *center = quads[index].center;

						rooms[y][p][r][index][0] = center[0] * cosf(yaw) + center[2] * sinf(yaw);
						rooms[y][p][r][index][1] = center[1];
						rooms[y][p][r][index][2] = center[2] * cosf(yaw) - center[0] * sinf(yaw);
					}
				}
		for (y = 0; y < 2; y++)
			for (p = 0; p < 3; p++)
				for (r = 0; r < 2; r++)
					for (index = 1; index < count - 1; index++)
					{
						int k;
						float moved = 0.0f;

						for (k = 0; k < 3; k++)
							still = fmaxf(still, fabsf(rooms[y][p][r][index][k] - rooms[y][0][0][index][k]));
						if (y == 1)
						{
							for (k = 0; k < 3; k++)
								moved += (rooms[1][p][r][index][k] - rooms[0][p][r][index][k]) *
									(rooms[1][p][r][index][k] - rooms[0][p][r][index][k]);
							turned = fminf(turned, sqrtf(moved));
						}
					}
		check(still < 1e-5f, "a head pitch or roll moves no group in the room");
		check(turned > 0.5f, "a body yaw of 30 degrees moves every group in the room");
	}
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
			NULL, pieces);
		count = host_stereo_hud_layout(aspect * 480.0f, 1, ahead, hud_tangents, (const float (*)[4])extents, NULL, quads);
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

	brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, 0, refresh, &switched);
	check(brightness == 1.0f && !switched, "the first view after none shows at once");
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, 0, refresh, &switched) == 1.0f && !switched,
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
		brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, frame_seconds, &switched);
		check(switched && brightness == 0.0f, "the UI's quad to the full view starts black");
		last = brightness;
		for (frame = 1; frame < frames; frame++)
		{
			brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, frame_seconds, &switched);
			check(!switched && brightness > last && brightness < 1.0f, "the full view comes up over the cut");
			last = brightness;
		}
		snprintf(what, sizeof(what), "the full view is at full brightness after %d frames at %d frames a second "
			"(%.0f ms)", frames, repeat ? 45 : 90, 1000.0f * HOST_STEREO_CUT_SECONDS);
		check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, frame_seconds, NULL) == 1.0f, what);
	}
	/* the full view to the UI's quad (a load starts), and the UI's quad to
	the screen */
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, 0, refresh, &switched) == 0.0f && switched,
		"the full view to the UI's quad starts black");
	cut.elapsed = HOST_STEREO_CUT_SECONDS;
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_SCREEN, 0, 0, refresh, &switched) == 0.0f && switched,
		"the UI's quad to the screen starts black");
	cut.elapsed = HOST_STEREO_CUT_SECONDS;
	/* a script fade covers the cut */
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 1, 0, refresh, &switched) == 1.0f && !switched,
		"a cut under a script fade doesn't go through black");
	/* no view (mono) forgets the last: the next view shows at once */
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_NONE, 0, 0, refresh, &switched) == 1.0f && !switched &&
		cut.shown == HOST_STEREO_VIEW_NONE, "a frame without a view forgets the last");
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, 0, refresh, &switched) == 1.0f && !switched,
		"a view after none shows at once");
	/* a switch in the middle of a fade starts it over */
	host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, refresh, NULL);
	host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, refresh, NULL);
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, 0, refresh, &switched) == 0.0f && switched,
		"a cut during a fade starts again from black");
	/* stereo's cut over a seat's exit glide (halo_stereo_cut_requested): the
	full view held black while requested, its fade not advancing, then up
	over the cut's time as after any cut */
	{
		int held = 1, rising = 1;

		cut.shown = HOST_STEREO_VIEW_FULL;
		cut.elapsed = HOST_STEREO_CUT_SECONDS;
		for (frame = 0; frame < 20; frame++)
			held &= host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 1, refresh, &switched) == 0.0f &&
				cut.elapsed == 0.0f && !switched;
		check(held, "a requested cut holds the full view black for as long as it's requested");
		last = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, refresh, &switched);
		check(last == 0.0f && !switched, "when the request drops the view starts up from black");
		for (frame = 1; frame < 6; frame++) {
			brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, refresh, &switched);
			rising &= brightness > last && brightness < 1.0f;
			last = brightness;
		}
		check(rising && host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, refresh, NULL) == 1.0f,
			"then comes up over HOST_STEREO_CUT_SECONDS (6 frames at 90 Hz)");
		/* a request under a script fade still holds black */
		check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 1, 1, refresh, NULL) == 0.0f,
			"a requested cut holds black under a script fade too");
	}
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
	placement_checks();
	zoom_checks();
	level_checks();
	cut_checks();
	printf("%s\n", failures ? "stereo_hud_probe: FAILED" : "stereo_hud_probe: PASS");
	return failures ? 1 : 0;
}
