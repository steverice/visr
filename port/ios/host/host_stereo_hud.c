/* HEAD mode's HUD and UI layout (host_stereo_hud.h). */
#include "host_stereo_hud.h"
#include "halo_stereo.h"
#include <math.h>
#include <string.h>

#define DEGREES ((float)M_PI / 180.0f)

/* past this pitch the level frame's yaw comes from the head's right */
#define LEVEL_YAW_PITCH_LIMIT (85.0f * DEGREES)

/* The HUD's pieces, in layout lines (480 tall, y down), measured from the
HUD layer's captures on the Mac (side by side, a 640-line layout: a30 at
frames 900, 1200 and 1500 with debug.test_input "walklook:1", a10 at frame
7350; Task 7c's t7c/ folder in the stereo plan): the ammo and grenade
counters at x 48-168, y 37-82; the help text and messages under them from x
48, y 82.5 on, "Picked up 480 rounds for assault rifle" reaching x 386 and
two lines reaching y 146; the shield and health meters at x 464-584, y
37-66; the motion tracker with its range at x 48-131, y 362-444; the
assault rifle's crosshair at x 292-347, y 212-267. In the visionOS
simulator's 853-line layout (a30, frames 2700-4500, Task 7c's fix round)
the same elements sit 62.9 lines in from the left edge and 71.6 from the
right (the meters at 662-782), at the same sizes and heights: the HUD's safe
margin grows with the width (47.8 of 640, 62.9 of 853; safe_inset below
has the game's rule). So each piece is a rectangle from an anchor on the
layout's safe frame (its left or right edge inset by the game's own safe
inset, or its center across; its top or bottom down), with margin, and
follows its elements at any width. The tracker is at the bottom of Halo's
HUD, so it goes in the bottom band; the counters, messages and meters in
the top one. */
enum { ANCHOR_START = -1, ANCHOR_CENTER = 0, ANCHOR_END = 1 };

/* The HUD's safe frame across: the game anchors the HUD's corner elements
to render.camera.window_bounds (hud_draw.c), which are the rasterizer's
frame_bounds, set at startup as 48 lines in at 640 and widened in
proportion, truncated: RASTERIZER_FRAME_BOUNDS_X0 * width /
RASTERIZER_SCREEN_WIDTH, and the width less that on the right
(rasterizer_xbox.c). The captures agree: 47.8 and 62.9 lines in at 640 and
853 */
static float safe_inset(float layout_width)
{
	return floorf(48.0f * layout_width / 640.0f);
}

struct piece
{
	int anchor_x, anchor_y;
	/* the rectangle, in lines from the anchor: x0 < x1 across, y0 < y1 down */
	float x0, y0, x1, y1;
	/* its band (1 the top, -1 the bottom), row in the band (0 next to the
	reticle, then outward) and side (-1 left of the center, 1 right, 0
	centered) */
	int band, row, side;
};

/* the reticle's square at the layout's center, in lines. Every crosshair
the HUD draws comes from the combined reticle sheet
(ui\hud\bitmaps\combined\hud_reticles, the weapons' and the vehicles'
alike), whose Xbox cells are at most 66 by 66 at the HUD's scale of 1
(port/assets/hud/layout.json, the sheet's 12 bitmaps: one 128-pixel sheet
of 66, 18 by 10 and smaller cells and eleven 64 by 64); the assault rifle's
measures 56 across on the layer. The square leaves a margin around the
largest and ends at y 190, where the messages' piece begins, so no layer
region is claimed twice (stereo_hud_probe). A larger crosshair's edge, if a
tag scales one up, shows through the catch-all */
#define RETICLE_LINES 100.0f

static const struct piece pieces[] = {
	/* the ammo and grenade counters, left, and the shield and health
	meters, right: always on, so next to the reticle. The counters end at
	82 and the help text starts at 82.5; the seam is between them (the
	presenter samples only inside each rectangle) */
	{ ANCHOR_START, ANCHOR_START, -16.0f, 30.0f, 140.0f, 82.25f, 1, 0, -1 },
	{ ANCHOR_END, ANCHOR_START, -140.0f, 30.0f, 16.0f, 74.0f, 1, 0, 1 },
	/* the help text and messages, up to four lines, centered above them */
	{ ANCHOR_START, ANCHOR_START, -16.0f, 82.25f, 400.0f, 190.0f, 1, 1, 0 },
	/* the motion tracker, centered under the reticle */
	{ ANCHOR_START, ANCHOR_END, -16.0f, -126.0f, 100.0f, -28.0f, -1, 0, 0 },
};
#define PIECE_COUNT (sizeof(pieces) / sizeof(pieces[0]))

/* the gap between a band's inner edge and the reticle's square, and between
a band's left and right pieces, in lines at the natural scale */
#define BAND_GAP_LINES 8.0f

static float layout_x(int anchor, float layout_width, float offset)
{
	float safe = safe_inset(layout_width);

	return (anchor == ANCHOR_START ? safe : anchor == ANCHOR_CENTER ? layout_width / 2.0f : layout_width - safe) +
		offset;
}

static float layout_y(int anchor, float offset)
{
	return (anchor == ANCHOR_START ? 0.0f : anchor == ANCHOR_CENTER ? HOST_STEREO_HUD_LINES / 2.0f :
		HOST_STEREO_HUD_LINES) + offset;
}

/* a piece's layout rectangle: x0, y0, x1, y1 in lines */
static void piece_rectangle(const struct piece *piece, float layout_width, float rectangle[4])
{
	rectangle[0] = layout_x(piece->anchor_x, layout_width, piece->x0);
	rectangle[1] = layout_y(piece->anchor_y, piece->y0);
	rectangle[2] = layout_x(piece->anchor_x, layout_width, piece->x1);
	rectangle[3] = layout_y(piece->anchor_y, piece->y1);
	/* inside the layout */
	rectangle[0] = fmaxf(rectangle[0], 0.0f);
	rectangle[2] = fminf(rectangle[2], layout_width);
	rectangle[1] = fmaxf(rectangle[1], 0.0f);
	rectangle[3] = fminf(rectangle[3], HOST_STEREO_HUD_LINES);
}

static void source_of(const float rectangle[4], float layout_width, float source[4])
{
	source[0] = rectangle[0] / layout_width;
	source[1] = rectangle[1] / HOST_STEREO_HUD_LINES;
	source[2] = rectangle[2] / layout_width;
	source[3] = rectangle[3] / HOST_STEREO_HUD_LINES;
}

/* where a piece goes on the level plane at HOST_STEREO_HUD_DISTANCE, in
lines from the view's center (x right, y up): its rectangle there, x0, y0
(bottom), x1, y1 (top), for the natural scale. The top band's pieces sit on
a line just above the reticle's square, the bottom band's hang from one just
below it; the left ones end just left of the center, the right ones start
just right of it, the centered ones are centered. A band's rows stack
outward from the reticle, each as tall as its tallest piece */
static void piece_place(size_t index, float layout_width, float placed[4])
{
	const struct piece *piece = &pieces[index];
	float rectangle[4];
	float width, height, inner = RETICLE_LINES / 2.0f + BAND_GAP_LINES;
	size_t earlier;
	int row;

	piece_rectangle(piece, layout_width, rectangle);
	width = rectangle[2] - rectangle[0];
	height = rectangle[3] - rectangle[1];
	/* outward past its band's nearer rows */
	for (row = 0; row < piece->row; row++)
	{
		float tallest = 0.0f;

		for (earlier = 0; earlier < PIECE_COUNT; earlier++)
		{
			float other[4];

			if (pieces[earlier].band != piece->band || pieces[earlier].row != row)
				continue;
			piece_rectangle(&pieces[earlier], layout_width, other);
			tallest = fmaxf(tallest, other[3] - other[1]);
		}
		if (tallest > 0.0f)
			inner += tallest + BAND_GAP_LINES;
	}
	placed[0] = piece->side < 0 ? -BAND_GAP_LINES / 2.0f - width : piece->side > 0 ? BAND_GAP_LINES / 2.0f :
		-width / 2.0f;
	placed[2] = placed[0] + width;
	placed[1] = piece->band > 0 ? inner : -inner - height;
	placed[3] = placed[1] + height;
}

float host_stereo_hud_band_scale(float layout_width)
{
	/* the largest distance from the center either way, in lines, at the
	natural scale; the sharp region's edge on the plane, in meters */
	float reach = 0.0f;
	float limit = HOST_STEREO_HUD_DISTANCE * tanf(HUD_SHARP_RADIUS_DEGREES * DEGREES);
	size_t index;

	for (index = 0; index < PIECE_COUNT; index++)
	{
		float placed[4];

		if (pieces[index].band == 0)
			continue;
		piece_place(index, layout_width, placed);
		reach = fmaxf(reach, fmaxf(fmaxf(fabsf(placed[0]), fabsf(placed[2])), fmaxf(fabsf(placed[1]), fabsf(placed[3]))));
	}
	if (reach * HOST_STEREO_HUD_METERS_PER_LINE <= limit || reach <= 0.0f)
		return HOST_STEREO_HUD_METERS_PER_LINE;
	return limit / reach;
}

/* a square quad showing the layer's square of the given side (lines) at its
center, half a meters across, facing back along direction, upright, at
distance along it or centered at position (host_stereo_hud_reticle's
rule); 0 if none */
static int facing_square(float layout_width, float side, float half, float distance, const float position[3],
	const float direction[3], struct host_stereo_hud_quad *quad)
{
	float d[3] = { direction[0], direction[1], direction[2] };
	float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	float right[3], up[3], right_length;
	float rectangle[4] = { layout_width / 2.0f - side / 2.0f, HOST_STEREO_HUD_LINES / 2.0f - side / 2.0f,
		layout_width / 2.0f + side / 2.0f, HOST_STEREO_HUD_LINES / 2.0f + side / 2.0f };
	int axis;

	if (!(length > 1e-6f))
		return 0;
	for (axis = 0; axis < 3; axis++)
		d[axis] /= length;
	/* not ahead of the eyes: off the view (a seat's gun aiming where the
	head has turned away from) */
	if (d[2] > -0.1f && !position)
		return 0;
	/* facing straight up or down there's no upright: none */
	if (d[0] * d[0] + d[2] * d[2] < 1e-6f)
		return 0;
	/* facing the eyes, upright in their frame: right is the direction
	across the eyes' up, up is right across the direction */
	right[0] = -d[2];
	right[1] = 0.0f;
	right[2] = d[0];
	right_length = sqrtf(right[0] * right[0] + right[2] * right[2]);
	for (axis = 0; axis < 3; axis++)
		right[axis] /= right_length;
	up[0] = right[1] * d[2] - right[2] * d[1];
	up[1] = right[2] * d[0] - right[0] * d[2];
	up[2] = right[0] * d[1] - right[1] * d[0];
	memset(quad, 0, sizeof(*quad));
	source_of(rectangle, layout_width, quad->source);
	for (axis = 0; axis < 3; axis++)
	{
		quad->center[axis] = position ? position[axis] : d[axis] * distance;
		quad->x_axis[axis] = right[axis] * half;
		quad->y_axis[axis] = up[axis] * half;
	}
	quad->frame = HOST_STEREO_HUD_HEAD;
	return 1;
}

int host_stereo_hud_reticle(float layout_width, const float position[3], const float direction[3],
	struct host_stereo_hud_quad *quad)
{
	return facing_square(layout_width, RETICLE_LINES, RETICLE_LINES / 2.0f * HOST_STEREO_HUD_METERS_PER_LINE,
		HOST_STEREO_HUD_DISTANCE, position, direction, quad);
}

int host_stereo_hud_inset(float layout_width, const float position[3], const float direction[3],
	struct host_stereo_hud_quad *quad)
{
	if (!(layout_width > 0.0f))
		layout_width = 640.0f;
	if (!facing_square(layout_width, fminf(HALO_STEREO_INSET_LINES, layout_width), HALO_STEREO_INSET_WIDTH_METERS / 2.0f,
		HALO_STEREO_INSET_DISTANCE_METERS, position, direction, quad))
		return 0;
	/* its picture covers what it's over */
	quad->opaque = 1;
	return 1;
}

void host_stereo_hud_ui(float aspect, struct host_stereo_hud_quad *quad)
{
	float half = HOST_STEREO_HUD_DISTANCE * tanf(HUD_SHARP_RADIUS_DEGREES * DEGREES);
	float half_width = half, half_height = half;

	if (!(aspect > 0.0f))
		aspect = 4.0f / 3.0f;
	if (aspect >= 1.0f)
		half_height = half / aspect;
	else
		half_width = half * aspect;
	memset(quad, 0, sizeof(*quad));
	quad->source[2] = 1.0f;
	quad->source[3] = 1.0f;
	quad->center[2] = -HOST_STEREO_HUD_DISTANCE;
	quad->x_axis[0] = half_width;
	quad->y_axis[1] = half_height;
	quad->frame = HOST_STEREO_HUD_LEVEL;
}

int host_stereo_hud_claimed(const struct host_stereo_hud_quad *quads, int count, float u, float v)
{
	int index;

	for (index = 0; index < count; index++)
		if (!quads[index].catch_all && u >= quads[index].source[0] && u < quads[index].source[2] &&
			v >= quads[index].source[1] && v < quads[index].source[3])
			return 1;
	return 0;
}

int host_stereo_hud_layout(float layout_width, int ui, const float reticle[3], const float hud_tangents[2],
	struct host_stereo_hud_quad *quads)
{
	float scale;
	size_t index;
	int count = 0;

	if (!(layout_width > 0.0f))
		layout_width = 640.0f;
	if (ui)
	{
		host_stereo_hud_ui(layout_width / HOST_STEREO_HUD_LINES, &quads[0]);
		return 1;
	}
	/* the reticle's square is claimed even when its quad is off the view,
	so the catch-all never shows the crosshair at the center */
	if (host_stereo_hud_reticle(layout_width, NULL, reticle, &quads[count]))
		count++;
	else
	{
		float ahead[3] = { 0.0f, 0.0f, -1.0f };

		host_stereo_hud_reticle(layout_width, NULL, ahead, &quads[count]);
		quads[count].hidden = 1;
		count++;
	}
	scale = host_stereo_hud_band_scale(layout_width);
	for (index = 0; index < PIECE_COUNT && count < HOST_STEREO_HUD_MAXIMUM_QUADS; index++)
	{
		struct host_stereo_hud_quad *quad = &quads[count];
		float rectangle[4], placed[4];

		if (pieces[index].band == 0)
			continue;
		piece_rectangle(&pieces[index], layout_width, rectangle);
		if (!(rectangle[2] > rectangle[0]) || !(rectangle[3] > rectangle[1]))
			continue;
		piece_place(index, layout_width, placed);
		memset(quad, 0, sizeof(*quad));
		source_of(rectangle, layout_width, quad->source);
		quad->center[0] = (placed[0] + placed[2]) / 2.0f * scale;
		quad->center[1] = (placed[1] + placed[3]) / 2.0f * scale;
		quad->center[2] = -HOST_STEREO_HUD_DISTANCE;
		quad->x_axis[0] = (placed[2] - placed[0]) / 2.0f * scale;
		quad->y_axis[1] = (placed[3] - placed[1]) / 2.0f * scale;
		quad->frame = HOST_STEREO_HUD_LEVEL;
		count++;
	}
	/* everything no piece claims: the whole layer, head-locked, at the HUD
	pass's own projection 2 m ahead, so a nav point, the multiplayer score,
	long help text or a large crosshair's edge shows where the game
	projected it; the presenter discards what the pieces claim */
	if (hud_tangents && hud_tangents[0] > 0.0f && hud_tangents[1] > 0.0f && count < HOST_STEREO_HUD_MAXIMUM_QUADS)
	{
		struct host_stereo_hud_quad *quad = &quads[count];

		memset(quad, 0, sizeof(*quad));
		quad->source[2] = 1.0f;
		quad->source[3] = 1.0f;
		quad->center[2] = -HOST_STEREO_HUD_DISTANCE;
		quad->x_axis[0] = HOST_STEREO_HUD_DISTANCE * hud_tangents[0];
		quad->y_axis[1] = HOST_STEREO_HUD_DISTANCE * hud_tangents[1];
		quad->frame = HOST_STEREO_HUD_HEAD;
		quad->catch_all = 1;
		count++;
	}
	return count;
}

float host_stereo_hud_level_yaw(const float right[3], const float back[3])
{
	float forward[3] = { -back[0], -back[1], -back[2] };
	float pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward[1])));

	/* near straight up or down forward's horizontal part vanishes; the
	head's right stays level there */
	if (fabsf(pitch) >= LEVEL_YAW_PITCH_LIMIT)
		return atan2f(-right[2], right[0]);
	return atan2f(-forward[0], -forward[2]);
}

float host_stereo_cut_brightness(struct host_stereo_cut *cut, int shown, int covered, int *switched)
{
	float brightness;

	if (switched)
		*switched = 0;
	if (shown == HOST_STEREO_VIEW_NONE)
	{
		cut->shown = HOST_STEREO_VIEW_NONE;
		cut->frames = 0;
		return 1.0f;
	}
	if (cut->shown != HOST_STEREO_VIEW_NONE && shown != cut->shown && !covered)
	{
		cut->frames = HOST_STEREO_CUT_FRAMES;
		if (switched)
			*switched = 1;
	}
	cut->shown = shown;
	if (cut->frames <= 0)
		return 1.0f;
	brightness = 1.0f - (float)cut->frames / (float)HOST_STEREO_CUT_FRAMES;
	cut->frames--;
	return brightness;
}
