/* HEAD mode's HUD and UI layout (host_stereo_hud.h). */
#include "host_stereo_hud.h"
#include "halo_stereo.h"
#include <math.h>
#include <string.h>

#define DEGREES ((float)M_PI / 180.0f)

/* past this pitch the level frame's yaw comes from the head's right */
#define LEVEL_YAW_PITCH_LIMIT (85.0f * DEGREES)

/* The bands' slots: where each HUD group's quad goes, as Task 7c placed its
pieces, with the group's own rectangle in place of a measured one. The
weapon's counters and icon (with the grenades) left and the unit's meters
right sit next to the reticle in the top band; a vehicle driver's seat
labels, which CE anchors top left, share the weapon's slot, each where CE
puts it relative to the other, so a Scorpion driver's rider labels stay
under the cannon's counters as on the Xbox; the prompts and the
messages, which CE draws under the counters, sit above them, centered, in
CE's own arrangement (the prompt over the messages); the motion tracker,
at the bottom of Halo's HUD, hangs centered in the bottom band. */
struct slot
{
	/* its band (1 the top, -1 the bottom), row in the band (0 next to the
	reticle, then outward) and side (-1 left of the center, 1 right, 0
	centered) */
	int band, row, side;
	/* the groups in it, which keep their places relative to each other */
	int groups[2], group_count;
};

static const struct slot slots[] = {
	{ 1, 0, -1, { HALO_HUD_GROUP_WEAPON, HALO_HUD_GROUP_SEATS }, 2 },
	{ 1, 0, 1, { HALO_HUD_GROUP_UNIT }, 1 },
	{ 1, 1, 0, { HALO_HUD_GROUP_PROMPT, HALO_HUD_GROUP_MESSAGES }, 2 },
	{ -1, 0, 0, { HALO_HUD_GROUP_TRACKER }, 1 },
};
#define SLOT_COUNT (sizeof(slots) / sizeof(slots[0]))

/* the gap between a band's inner edge and the reticle's square, and between
a band's left and right slots, in lines at the natural scale; the reticle's
square, which the bands stay clear of, as Task 7c's (every crosshair on the
combined reticle sheet is at most 66 lines across) */
#define BAND_GAP_LINES 8.0f
#define RETICLE_LINES 100.0f

/* a group's rectangle in the layout with its margin, clipped to the
layout: x0, y0, x1, y1 in lines; 0 if it has none */
static int group_rectangle(const float (*group_extent)[4], int group, float layout_width, float rectangle[4])
{
	const float *extent;

	if (!group_extent || group < 0 || group >= HALO_HUD_GROUP_COUNT)
		return 0;
	extent = group_extent[group];
	if (!(extent[2] > extent[0]) || !(extent[3] > extent[1]))
		return 0;
	rectangle[0] = fmaxf(extent[0] - HOST_STEREO_HUD_GROUP_MARGIN_LINES, 0.0f);
	rectangle[1] = fmaxf(extent[1] - HOST_STEREO_HUD_GROUP_MARGIN_LINES, 0.0f);
	rectangle[2] = fminf(extent[2] + HOST_STEREO_HUD_GROUP_MARGIN_LINES, layout_width);
	rectangle[3] = fminf(extent[3] + HOST_STEREO_HUD_GROUP_MARGIN_LINES, HOST_STEREO_HUD_LINES);
	return rectangle[2] > rectangle[0] && rectangle[3] > rectangle[1];
}

/* a slot's rectangle in the layout: the union of its groups'; 0 if none
of them drew */
static int slot_rectangle(const struct slot *slot, const float (*group_extent)[4], float layout_width,
	float rectangle[4])
{
	int index, found = 0;

	for (index = 0; index < slot->group_count; index++)
	{
		float group[4];

		if (!group_rectangle(group_extent, slot->groups[index], layout_width, group))
			continue;
		if (!found)
			memcpy(rectangle, group, sizeof(group));
		else
		{
			rectangle[0] = fminf(rectangle[0], group[0]);
			rectangle[1] = fminf(rectangle[1], group[1]);
			rectangle[2] = fmaxf(rectangle[2], group[2]);
			rectangle[3] = fmaxf(rectangle[3], group[3]);
		}
		found = 1;
	}
	return found;
}

static void source_of(const float rectangle[4], float layout_width, float source[4])
{
	source[0] = rectangle[0] / layout_width;
	source[1] = rectangle[1] / HOST_STEREO_HUD_LINES;
	source[2] = rectangle[2] / layout_width;
	source[3] = rectangle[3] / HOST_STEREO_HUD_LINES;
}

/* where a slot goes on the level plane at HOST_STEREO_HUD_DISTANCE, in lines
from the view's center (x right, y up): x0, y0 (bottom), x1, y1 (top), for
the natural scale. The top band's slots sit on a line just above the
reticle's square, the bottom band's hang from one just below it; the left
ones end just left of the center, the right ones start just right of it,
the centered ones are centered. A band's rows stack outward from the
reticle, each as tall as its tallest slot; a row with nothing in it takes
no room */
static void slot_place(size_t index, const float rectangle[4], const float (*group_extent)[4], float layout_width,
	float placed[4])
{
	const struct slot *slot = &slots[index];
	float width = rectangle[2] - rectangle[0], height = rectangle[3] - rectangle[1];
	float inner = RETICLE_LINES / 2.0f + BAND_GAP_LINES;
	size_t earlier;
	int row;

	/* outward past its band's nearer rows */
	for (row = 0; row < slot->row; row++)
	{
		float tallest = 0.0f;

		for (earlier = 0; earlier < SLOT_COUNT; earlier++)
		{
			float other[4];

			if (slots[earlier].band != slot->band || slots[earlier].row != row ||
				!slot_rectangle(&slots[earlier], group_extent, layout_width, other))
				continue;
			tallest = fmaxf(tallest, other[3] - other[1]);
		}
		if (tallest > 0.0f)
			inner += tallest + BAND_GAP_LINES;
	}
	placed[0] = slot->side < 0 ? -BAND_GAP_LINES / 2.0f - width : slot->side > 0 ? BAND_GAP_LINES / 2.0f :
		-width / 2.0f;
	placed[2] = placed[0] + width;
	placed[1] = slot->band > 0 ? inner : -inner - height;
	placed[3] = placed[1] + height;
}

float host_stereo_hud_band_scale(float layout_width, const float (*group_extent)[4])
{
	/* the largest distance from the center either way, in lines, at the
	natural scale; the sharp region's edge on the plane, in meters */
	float reach = 0.0f;
	float limit = HOST_STEREO_HUD_DISTANCE * tanf(HUD_SHARP_RADIUS_DEGREES * DEGREES);
	size_t index;

	for (index = 0; index < SLOT_COUNT; index++)
	{
		float rectangle[4], placed[4];

		if (!slot_rectangle(&slots[index], group_extent, layout_width, rectangle))
			continue;
		slot_place(index, rectangle, group_extent, layout_width, placed);
		reach = fmaxf(reach, fmaxf(fmaxf(fabsf(placed[0]), fabsf(placed[2])), fmaxf(fabsf(placed[1]), fabsf(placed[3]))));
	}
	if (reach * HOST_STEREO_HUD_METERS_PER_LINE <= limit || reach <= 0.0f)
		return HOST_STEREO_HUD_METERS_PER_LINE;
	return limit / reach;
}

/* a quad showing the layer's rectangle (lines) at the given half extents
(meters), facing back along direction, upright, at distance along it or
centered at position (host_stereo_hud_reticle's rule); 0 if none */
static int facing_rectangle(float layout_width, const float rectangle[4], float half_width, float half_height,
	float distance, const float position[3], const float direction[3], struct host_stereo_hud_quad *quad)
{
	float d[3] = { direction[0], direction[1], direction[2] };
	float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	float right[3], up[3], right_length;
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
		quad->x_axis[axis] = right[axis] * half_width;
		quad->y_axis[axis] = up[axis] * half_height;
	}
	quad->frame = HOST_STEREO_HUD_HEAD;
	return 1;
}

int host_stereo_hud_reticle(float layout_width, const float position[3], const float direction[3],
	struct host_stereo_hud_quad *quad)
{
	float whole[4] = { 0.0f, 0.0f, layout_width, HOST_STEREO_HUD_LINES };

	if (!(layout_width > 0.0f))
		return 0;
	if (!facing_rectangle(layout_width, whole, layout_width / 2.0f * HOST_STEREO_HUD_METERS_PER_LINE,
		HOST_STEREO_HUD_LINES / 2.0f * HOST_STEREO_HUD_METERS_PER_LINE, HOST_STEREO_HUD_DISTANCE, position, direction,
		quad))
		return 0;
	quad->layer = HOST_STEREO_HUD_LAYER_RETICLE;
	return 1;
}

int host_stereo_hud_inset(float layout_width, const float position[3], const float direction[3],
	struct host_stereo_hud_quad *quad)
{
	float side, square[4];

	if (!(layout_width > 0.0f))
		layout_width = 640.0f;
	side = fminf(HALO_STEREO_INSET_LINES, layout_width);
	square[0] = layout_width / 2.0f - side / 2.0f;
	square[1] = HOST_STEREO_HUD_LINES / 2.0f - side / 2.0f;
	square[2] = layout_width / 2.0f + side / 2.0f;
	square[3] = HOST_STEREO_HUD_LINES / 2.0f + side / 2.0f;
	if (!facing_rectangle(layout_width, square, HALO_STEREO_INSET_WIDTH_METERS / 2.0f,
		HALO_STEREO_INSET_WIDTH_METERS / 2.0f, HALO_STEREO_INSET_DISTANCE_METERS, position, direction, quad))
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

int host_stereo_hud_layout(float layout_width, int ui, const float reticle[3], const float hud_tangents[2],
	const float (*group_extent)[4], struct host_stereo_hud_quad *quads)
{
	float scale;
	size_t index;
	int count = 0, group;

	if (!(layout_width > 0.0f))
		layout_width = 640.0f;
	if (host_stereo_hud_reticle(layout_width, NULL, reticle, &quads[count]))
		count++;
	scale = host_stereo_hud_band_scale(layout_width, group_extent);
	for (index = 0; index < SLOT_COUNT; index++)
	{
		float slot[4], placed[4];
		int member;

		if (!slot_rectangle(&slots[index], group_extent, layout_width, slot))
			continue;
		slot_place(index, slot, group_extent, layout_width, placed);
		/* each group at its place in the slot: as far from the slot's left
		and top edges as in the layout */
		for (member = 0; member < slots[index].group_count && count < HOST_STEREO_HUD_MAXIMUM_QUADS; member++)
		{
			struct host_stereo_hud_quad *quad = &quads[count];
			float rectangle[4], x0, x1, top, bottom;

			group = slots[index].groups[member];
			if (!group_rectangle(group_extent, group, layout_width, rectangle))
				continue;
			x0 = placed[0] + (rectangle[0] - slot[0]);
			x1 = x0 + (rectangle[2] - rectangle[0]);
			top = placed[3] - (rectangle[1] - slot[1]);
			bottom = top - (rectangle[3] - rectangle[1]);
			memset(quad, 0, sizeof(*quad));
			source_of(rectangle, layout_width, quad->source);
			quad->center[0] = (x0 + x1) / 2.0f * scale;
			quad->center[1] = (bottom + top) / 2.0f * scale;
			quad->center[2] = -HOST_STEREO_HUD_DISTANCE;
			quad->x_axis[0] = (x1 - x0) / 2.0f * scale;
			quad->y_axis[1] = (top - bottom) / 2.0f * scale;
			quad->frame = HOST_STEREO_HUD_LEVEL;
			quad->layer = HOST_STEREO_HUD_LAYER_GROUP + group;
			count++;
		}
	}
	/* the HUD layer, which holds what no group drew: whole, head-locked, at
	the HUD pass's own projection 2 m ahead, so a nav point or the
	multiplayer score shows where the game projected it */
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
		quad->layer = HOST_STEREO_HUD_LAYER_HUD;
		quad->catch_all = 1;
		count++;
	}
	/* a menu, a help panel, the console or a progress bar: the UI layer
	whole on the UI's quad, over the HUD's pieces as the game draws it over
	the HUD */
	if (ui && count < HOST_STEREO_HUD_MAXIMUM_QUADS)
	{
		host_stereo_hud_ui(layout_width / HOST_STEREO_HUD_LINES, &quads[count]);
		quads[count++].layer = HOST_STEREO_HUD_LAYER_UI;
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

float host_stereo_cut_brightness(struct host_stereo_cut *cut, int shown, int covered, float frame_seconds,
	int *switched)
{
	float brightness;

	if (switched)
		*switched = 0;
	if (shown == HOST_STEREO_VIEW_NONE)
	{
		cut->shown = HOST_STEREO_VIEW_NONE;
		cut->elapsed = HOST_STEREO_CUT_SECONDS;
		return 1.0f;
	}
	if (cut->shown != HOST_STEREO_VIEW_NONE && shown != cut->shown && !covered)
	{
		cut->elapsed = 0.0f;
		if (switched)
			*switched = 1;
	}
	cut->shown = shown;
	if (cut->elapsed >= HOST_STEREO_CUT_SECONDS)
		return 1.0f;
	brightness = cut->elapsed / HOST_STEREO_CUT_SECONDS;
	cut->elapsed += frame_seconds;
	return brightness;
}
