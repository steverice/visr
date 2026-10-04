/* HEAD mode's HUD and UI layout (port/ios/host/host_stereo_hud.c): every
band element and the UI inside foveation's sharp region with the head level,
the bands above and below the reticle and clear of it, the reticle centered
and at its natural size, a seat's reticle along the game camera's aim, the
level frame's yaw. With --quads WIDTH UI X Y Z it prints the layout's quads
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

static void print_quads(float width, int ui, const float reticle[3])
{
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	int count = host_stereo_hud_layout(width, ui, reticle, quads), index;

	for (index = 0; index < count; index++)
	{
		const struct host_stereo_hud_quad *q = &quads[index];

		printf("%d %g %g %g %g %g %g %g %g %g %g %g %g %g\n", q->frame, q->source[0], q->source[1], q->source[2],
			q->source[3], q->center[0], q->center[1], q->center[2], q->x_axis[0], q->x_axis[1], q->x_axis[2],
			q->y_axis[0], q->y_axis[1], q->y_axis[2]);
	}
}

static void layout_checks(float width)
{
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	int count = host_stereo_hud_layout(width, 0, ahead, quads), index;
	float reach = 0.0f, reticle_half_degrees;
	int inside = 1, clear = 1, sources = 1, above = 0, below = 0, level = 1;
	char what[200];

	printf("layout %.0f lines across (%.3f:1), band scale %.3f mm a line\n", width, width / 480.0f,
		1000.0f * host_stereo_hud_band_scale(width));
	check(count >= 2 && quads[0].frame == HOST_STEREO_HUD_HEAD, "the reticle comes first, head-locked");
	check(fabsf(quads[0].center[0]) < 1e-6f && fabsf(quads[0].center[1]) < 1e-6f &&
		fabsf(quads[0].center[2] + HOST_STEREO_HUD_DISTANCE) < 1e-6f, "the reticle is centered, 2 m ahead");
	check(fabsf(quads[0].x_axis[0] - quads[0].y_axis[1]) < 1e-6f &&
		fabsf(quads[0].x_axis[0] * 2.0f - 96.0f * HOST_STEREO_HUD_METERS_PER_LINE) < 1e-6f,
		"the reticle is a square at the natural scale");
	check(fabsf((quads[0].source[0] + quads[0].source[2]) / 2.0f - 0.5f) < 1e-6f &&
		fabsf((quads[0].source[1] + quads[0].source[3]) / 2.0f - 0.5f) < 1e-6f,
		"the reticle is cut from the layer's center");
	reticle_half_degrees = atanf(quads[0].y_axis[1] / HOST_STEREO_HUD_DISTANCE) / DEGREES;
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
	check(host_stereo_hud_layout(aspect * 480.0f, 1, ahead, quads) == 1 && quads[0].frame == HOST_STEREO_HUD_LEVEL,
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

	check(host_stereo_hud_reticle(640.0f, aim, &quad) == 1, "a seat's reticle shows while its aim is ahead");
	check(fabsf(atan2f(quad.center[0], -quad.center[2]) - 30.0f * DEGREES) < 1e-4f &&
		fabsf(asinf(quad.center[1] / HOST_STEREO_HUD_DISTANCE) - 10.0f * DEGREES) < 1e-4f,
		"a seat's reticle sits along the game camera's aim, 30 degrees right and 10 up");
	facing = (quad.x_axis[1] * quad.y_axis[2] - quad.x_axis[2] * quad.y_axis[1]) * quad.center[0] +
		(quad.x_axis[2] * quad.y_axis[0] - quad.x_axis[0] * quad.y_axis[2]) * quad.center[1] +
		(quad.x_axis[0] * quad.y_axis[1] - quad.x_axis[1] * quad.y_axis[0]) * quad.center[2];
	check(facing < 0.0f && fabsf(quad.x_axis[1]) < 1e-6f, "it faces the eyes, upright");
	check(host_stereo_hud_reticle(640.0f, behind, &quad) == 0, "it leaves the view when the head turns far from the aim");
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
	level_checks();
	printf("%s\n", failures ? "stereo_hud_probe: FAILED" : "stereo_hud_probe: PASS");
	return failures ? 1 : 0;
}
