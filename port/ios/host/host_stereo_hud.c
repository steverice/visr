/* HEAD mode's HUD and UI layout (host_stereo_hud.h). */
#include "host_stereo_hud.h"
#include "halo_stereo.h"
#include <math.h>
#include <string.h>

#define DEGREES ((float)M_PI / 180.0f)

/* past this pitch the level frame's yaw comes from the head's right */
#define LEVEL_YAW_PITCH_LIMIT (85.0f * DEGREES)

/* The corners: where each HUD group's quad goes (the stereo spec's "The
HUD in the periphery"), in the corner where CE puts it, moved outward. The
weapon's counters and icon (with the grenades) top left, with a vehicle
driver's seat labels, which CE anchors top left too, each where CE puts it
relative to the other, so a Scorpion driver's rider labels stay under the
cannon's counters as on the Xbox; the unit's meters top right; the prompt
and the messages, which CE draws from the top left under the counters, from
display.hud_messages_up down, the prompt over the messages as CE draws
them; the motion tracker bottom left. A corner's groups share one plane, so
they keep CE's arrangement exactly. */
enum
{
	HEIGHT_CORNER_UP,                  /* display.hud_corner_up */
	HEIGHT_MESSAGES_UP,                /* display.hud_messages_up */
	HEIGHT_TRACKER_DOWN                /* display.hud_tracker_down, below the center */
};
struct slot
{
	/* its outer corner's side, across (-1 left, 1 right) and up (1 the top,
	-1 the bottom), and which setting sets that corner's height */
	int x, y, height;
	/* the groups in it, which keep their places relative to each other */
	int groups[2], group_count;
};

static const struct slot slots[] = {
	{ -1, 1, HEIGHT_CORNER_UP, { HALO_HUD_GROUP_WEAPON, HALO_HUD_GROUP_SEATS }, 2 },
	{ 1, 1, HEIGHT_CORNER_UP, { HALO_HUD_GROUP_UNIT }, 1 },
	{ -1, 1, HEIGHT_MESSAGES_UP, { HALO_HUD_GROUP_PROMPT, HALO_HUD_GROUP_MESSAGES }, 2 },
	{ -1, -1, HEIGHT_TRACKER_DOWN, { HALO_HUD_GROUP_TRACKER }, 1 },
};
#define SLOT_COUNT (sizeof(slots) / sizeof(slots[0]))

static const struct host_stereo_hud_placement default_placement = HOST_STEREO_HUD_PLACEMENT_DEFAULT;

/* a group's drawn rectangle in the layout, clipped to it: x0, y0, x1, y1
in lines; 0 if it has none */
static int group_content(const float (*group_extent)[4], int group, float layout_width, float content[4])
{
	const float *extent;

	if (!group_extent || group < 0 || group >= HALO_HUD_GROUP_COUNT)
		return 0;
	extent = group_extent[group];
	content[0] = fmaxf(extent[0], 0.0f);
	content[1] = fmaxf(extent[1], 0.0f);
	content[2] = fminf(extent[2], layout_width);
	content[3] = fminf(extent[3], HOST_STEREO_HUD_LINES);
	return content[2] > content[0] && content[3] > content[1];
}

/* a group's rectangle in the layout with its margin, clipped to the
layout: x0, y0, x1, y1 in lines; 0 if it has none */
static int group_rectangle(const float (*group_extent)[4], int group, float layout_width, float rectangle[4])
{
	float content[4];

	if (!group_content(group_extent, group, layout_width, content))
		return 0;
	rectangle[0] = fmaxf(content[0] - HOST_STEREO_HUD_GROUP_MARGIN_LINES, 0.0f);
	rectangle[1] = fmaxf(content[1] - HOST_STEREO_HUD_GROUP_MARGIN_LINES, 0.0f);
	rectangle[2] = fminf(content[2] + HOST_STEREO_HUD_GROUP_MARGIN_LINES, layout_width);
	rectangle[3] = fminf(content[3] + HOST_STEREO_HUD_GROUP_MARGIN_LINES, HOST_STEREO_HUD_LINES);
	return 1;
}

/* a slot's drawn rectangle in the layout: the union of its groups'; 0 if
none of them drew */
static int slot_content(const struct slot *slot, const float (*group_extent)[4], float layout_width,
	float content[4])
{
	int index, found = 0;

	for (index = 0; index < slot->group_count; index++)
	{
		float group[4];

		if (!group_content(group_extent, slot->groups[index], layout_width, group))
			continue;
		if (!found)
			memcpy(content, group, sizeof(group));
		else
		{
			content[0] = fminf(content[0], group[0]);
			content[1] = fminf(content[1], group[1]);
			content[2] = fmaxf(content[2], group[2]);
			content[3] = fmaxf(content[3], group[3]);
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

/* the plane distance out along the direction at yaw (right positive) and
pitch (up positive), facing the eyes, upright: its center on the sphere,
and its right (level) and up, unit length */
static void plane_at(float distance, float yaw, float pitch, float center[3], float right[3], float up[3])
{
	right[0] = cosf(yaw);
	right[1] = 0.0f;
	right[2] = sinf(yaw);
	/* right across forward */
	up[0] = -sinf(yaw) * sinf(pitch);
	up[1] = cosf(pitch);
	up[2] = cosf(yaw) * sinf(pitch);
	center[0] = distance * sinf(yaw) * cosf(pitch);
	center[1] = distance * sinf(pitch);
	center[2] = -distance * cosf(yaw) * cosf(pitch);
}

/* The plane of a rectangle half_width by half_height meters whose corner on
side x, y (as struct slot's) lies along the direction across, up (radians)
in the level frame: the plane facing the eyes from the sphere distance
out, its center found by stepping the center's angles by the corner's miss
until the corner is there (each step's miss is a small fraction of the
last). */
static void corner_plane(float distance, float across, float up, int x, int y, float half_width, float half_height,
	float center[3], float right[3], float upward[3])
{
	float yaw = across - (float)x * atanf(half_width / distance);
	float pitch = up - (float)y * atanf(half_height / distance);
	int step, axis;

	for (step = 0; step < 32; step++)
	{
		float corner[3], miss_across, miss_up;

		plane_at(distance, yaw, pitch, center, right, upward);
		for (axis = 0; axis < 3; axis++)
			corner[axis] = center[axis] + (float)x * half_width * right[axis] + (float)y * half_height * upward[axis];
		miss_across = across - atan2f(corner[0], -corner[2]);
		miss_up = up - atan2f(corner[1], hypotf(corner[0], corner[2]));
		if (fabsf(miss_across) < 1e-7f && fabsf(miss_up) < 1e-7f)
			return;
		yaw += miss_across;
		pitch += miss_up;
	}
	plane_at(distance, yaw, pitch, center, right, upward);
}

static int clamp_setting(float *value, float minimum, float maximum, float fallback)
{
	float clamped = *value != *value ? fallback : fmaxf(minimum, fminf(maximum, *value));

	if (clamped == *value)
		return 0;
	*value = clamped;
	return 1;
}

int host_stereo_hud_placement_clamp(struct host_stereo_hud_placement *placement)
{
	int changed = 0;

	changed |= clamp_setting(&placement->across, 0.0f, HOST_STEREO_HUD_SHARED_DEGREES, default_placement.across);
	changed |= clamp_setting(&placement->up, 0.0f, HOST_STEREO_HUD_SHARED_DEGREES, default_placement.up);
	changed |= clamp_setting(&placement->tracker_down, 0.0f, HOST_STEREO_HUD_SHARED_DEGREES,
		default_placement.tracker_down);
	changed |= clamp_setting(&placement->messages_up, 0.0f, HOST_STEREO_HUD_SHARED_DEGREES,
		default_placement.messages_up);
	changed |= clamp_setting(&placement->scale, HOST_STEREO_HUD_SCALE_MIN, HOST_STEREO_HUD_SCALE_MAX,
		default_placement.scale);
	changed |= clamp_setting(&placement->distance, HOST_STEREO_HUD_DISTANCE_MIN, HOST_STEREO_HUD_DISTANCE_MAX,
		default_placement.distance);
	return changed;
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

int host_stereo_hud_reticle(float layout_width, const float position[3], const float direction[3], float distance,
	struct host_stereo_hud_quad *quad)
{
	float whole[4] = { 0.0f, 0.0f, layout_width, HOST_STEREO_HUD_LINES };
	/* a line's meters at the distance (exactly 1 at HOST_STEREO_HUD_LINE_DISTANCE) */
	float line_scale = distance / HOST_STEREO_HUD_LINE_DISTANCE;

	if (!(layout_width > 0.0f))
		return 0;
	if (!facing_rectangle(layout_width, whole, layout_width / 2.0f * HOST_STEREO_HUD_METERS_PER_LINE * line_scale,
		HOST_STEREO_HUD_LINES / 2.0f * HOST_STEREO_HUD_METERS_PER_LINE * line_scale, distance, position, direction,
		quad))
		return 0;
	quad->layer = HOST_STEREO_HUD_LAYER_RETICLE;
	return 1;
}

int host_stereo_hud_zoom(const float tangents[2], float distance, struct host_stereo_hud_quad *quad)
{
	if (!(tangents[0] > 0.0f) || !(tangents[1] > 0.0f))
		return 0;
	memset(quad, 0, sizeof(*quad));
	quad->source[2] = quad->source[3] = 1.0f;
	quad->center[2] = -distance;
	quad->x_axis[0] = tangents[0] * distance;
	quad->y_axis[1] = tangents[1] * distance;
	quad->frame = HOST_STEREO_HUD_HEAD;
	/* its picture covers what it's over */
	quad->opaque = 1;
	return 1;
}

void host_stereo_hud_ui(float aspect, float distance, struct host_stereo_hud_quad *quad)
{
	float half = distance * tanf(HUD_SHARP_RADIUS_DEGREES * DEGREES);
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
	quad->center[2] = -distance;
	quad->x_axis[0] = half_width;
	quad->y_axis[1] = half_height;
	quad->frame = HOST_STEREO_HUD_LEVEL;
}

int host_stereo_hud_layout(float layout_width, int ui, const float reticle[3], const float hud_tangents[2],
	const float (*group_extent)[4], const struct host_stereo_hud_placement *placement,
	struct host_stereo_hud_quad *quads)
{
	float scale;
	size_t index;
	int count = 0;

	if (!(layout_width > 0.0f))
		layout_width = 640.0f;
	if (!placement)
		placement = &default_placement;
	if (host_stereo_hud_reticle(layout_width, NULL, reticle, placement->distance, &quads[count]))
		count++;
	/* meters a line on the HUD's sphere (the line scale exactly 1 at
	HOST_STEREO_HUD_LINE_DISTANCE) */
	scale = HOST_STEREO_HUD_METERS_PER_LINE * placement->scale * (placement->distance / HOST_STEREO_HUD_LINE_DISTANCE);
	for (index = 0; index < SLOT_COUNT; index++)
	{
		const struct slot *slot = &slots[index];
		float content[4], center[3], right[3], upward[3], across, up;
		int member;

		if (!slot_content(slot, group_extent, layout_width, content))
			continue;
		across = (float)slot->x * placement->across;
		up = slot->height == HEIGHT_CORNER_UP ? placement->up : slot->height == HEIGHT_MESSAGES_UP ?
			placement->messages_up : -placement->tracker_down;
		/* the slot's drawn rectangle with its outer corner at the angles */
		corner_plane(placement->distance, across * DEGREES, up * DEGREES, slot->x, slot->y, (content[2] - content[0]) / 2.0f * scale,
			(content[3] - content[1]) / 2.0f * scale, center, right, upward);
		/* each group, with its margin, where it is in the slot's rectangle,
		on the slot's plane */
		for (member = 0; member < slot->group_count && count < HOST_STEREO_HUD_MAXIMUM_QUADS; member++)
		{
			struct host_stereo_hud_quad *quad = &quads[count];
			int group = slot->groups[member], axis;
			float rectangle[4], offset_right, offset_up, half_width, half_height;

			if (!group_rectangle(group_extent, group, layout_width, rectangle))
				continue;
			offset_right = ((rectangle[0] + rectangle[2]) - (content[0] + content[2])) / 2.0f * scale;
			offset_up = -((rectangle[1] + rectangle[3]) - (content[1] + content[3])) / 2.0f * scale;
			half_width = (rectangle[2] - rectangle[0]) / 2.0f * scale;
			half_height = (rectangle[3] - rectangle[1]) / 2.0f * scale;
			memset(quad, 0, sizeof(*quad));
			source_of(rectangle, layout_width, quad->source);
			for (axis = 0; axis < 3; axis++)
			{
				quad->center[axis] = center[axis] + offset_right * right[axis] + offset_up * upward[axis];
				quad->x_axis[axis] = half_width * right[axis];
				quad->y_axis[axis] = half_height * upward[axis];
			}
			quad->frame = HOST_STEREO_HUD_LEVEL;
			quad->layer = HOST_STEREO_HUD_LAYER_GROUP + group;
			count++;
		}
	}
	/* the HUD layer, which holds what no group drew: whole, head-locked, at
	the HUD pass's own projection the distance ahead, so a nav point or the
	multiplayer score shows where the game projected it */
	if (hud_tangents && hud_tangents[0] > 0.0f && hud_tangents[1] > 0.0f && count < HOST_STEREO_HUD_MAXIMUM_QUADS)
	{
		struct host_stereo_hud_quad *quad = &quads[count];

		memset(quad, 0, sizeof(*quad));
		quad->source[2] = 1.0f;
		quad->source[3] = 1.0f;
		quad->center[2] = -placement->distance;
		quad->x_axis[0] = placement->distance * hud_tangents[0];
		quad->y_axis[1] = placement->distance * hud_tangents[1];
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
		host_stereo_hud_ui(layout_width / HOST_STEREO_HUD_LINES, placement->distance, &quads[count]);
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

float host_stereo_cut_brightness(struct host_stereo_cut *cut, int shown, int covered, int requested,
	float frame_seconds, int *switched)
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
	/* held black: the fade starts when the request drops */
	if (requested)
	{
		cut->elapsed = 0.0f;
		return 0.0f;
	}
	if (cut->elapsed >= HOST_STEREO_CUT_SECONDS)
		return 1.0f;
	brightness = cut->elapsed / HOST_STEREO_CUT_SECONDS;
	cut->elapsed += frame_seconds;
	return brightness;
}
