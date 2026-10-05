/*
HUD_GROUP.C

The HUD split by the function that draws each element (the stereo spec's
"The reticle split, by draw"; halo_stereo.h has the interface). Splitting
the HUD layer by screen rectangles cut elements whose drawn extent crossed
them: the flashlight meter's bar apart from the shield bar, a Warthog's
seat labels apart from their bars, the needler's icon, a long a10 prompt.
The game already draws each element from its own function, so the HUD's
drawing functions open a span for their group, and each draw goes to the
innermost span's group. A draw in no span takes the group of the corner
hud_calculate_point last placed an element from, while no span has begun
or ended since, else none (the catch-all).

Each group's rectangle is the union of its draws' screen extents this
frame, which d3d8_device.c measures (halo_hud_group_extent); the presenter
sizes and places the group's quad by it. render.c empties them as the HUD
pass begins.

port/ios/tests/hud_group_probe.c includes this file.
*/

#include "../src/halo_stereo.h"

/* deeper spans than this are counted but keep the deepest group listed */
#define HUD_GROUP_SPAN_DEPTH 8

/* the anchors' order (hud_definitions.h's _hud_anchor_*) */
enum
{
	HUD_ANCHOR_TOP_LEFT,
	HUD_ANCHOR_TOP_RIGHT,
	HUD_ANCHOR_BOTTOM_LEFT,
	HUD_ANCHOR_BOTTOM_RIGHT,
	HUD_ANCHOR_CENTER
};

static int spans[HUD_GROUP_SPAN_DEPTH];
static int span_depth;
/* the group of the last element's corner outside every span */
static int corner_group = HALO_HUD_GROUP_NONE;
/* the last element's corner inside the unit's span (-1: none yet), which
splits it: a vehicle driver's seat labels, which CE anchors top left, are
the seats' group, and an element anchored at the center the catch-all's */
static int unit_corner = -1;
static int measuring = 1;
/* each group's rectangle (x0, y0, x1, y1), the catch-all's first; empty
while x1 <= x0 */
static float rectangles[HALO_HUD_GROUP_COUNT + 1][4];

static float *group_rectangle(int group)
{
	return rectangles[group + 1];
}

void halo_hud_group_begin(int group)
{
	if (group < HALO_HUD_GROUP_NONE || group >= HALO_HUD_GROUP_COUNT)
		group = HALO_HUD_GROUP_NONE;
	if (span_depth < HUD_GROUP_SPAN_DEPTH)
		spans[span_depth] = group;
	span_depth++;
	corner_group = HALO_HUD_GROUP_NONE;
	unit_corner = -1;
}

void halo_hud_group_end(void)
{
	if (span_depth > 0)
		span_depth--;
	corner_group = HALO_HUD_GROUP_NONE;
	unit_corner = -1;
}

void halo_hud_group_corner(short corner)
{
	if (span_depth > 0)
	{
		unit_corner = corner;
		return;
	}
	corner_group = corner == HUD_ANCHOR_TOP_LEFT ? HALO_HUD_GROUP_WEAPON :
		corner == HUD_ANCHOR_TOP_RIGHT ? HALO_HUD_GROUP_UNIT :
		corner == HUD_ANCHOR_BOTTOM_LEFT ? HALO_HUD_GROUP_TRACKER : HALO_HUD_GROUP_NONE;
}

int halo_hud_group_current(void)
{
	if (span_depth > 0)
	{
		int group = spans[(span_depth < HUD_GROUP_SPAN_DEPTH ? span_depth : HUD_GROUP_SPAN_DEPTH) - 1];

		if (group == HALO_HUD_GROUP_UNIT && unit_corner == HUD_ANCHOR_TOP_LEFT)
			return HALO_HUD_GROUP_SEATS;
		if (group == HALO_HUD_GROUP_UNIT && unit_corner == HUD_ANCHOR_CENTER)
			return HALO_HUD_GROUP_NONE;
		return group;
	}
	return corner_group;
}

void halo_hud_group_extent(float x0, float y0, float x1, float y1)
{
	halo_hud_group_extent_in(halo_hud_group_current(), x0, y0, x1, y1);
}

void halo_hud_group_extent_in(int group, float x0, float y0, float x1, float y1)
{
	float *rectangle;

	if (!measuring || !(x1 > x0) || !(y1 > y0) || group < HALO_HUD_GROUP_NONE || group >= HALO_HUD_GROUP_COUNT)
		return;
	rectangle = group_rectangle(group);
	if (!(rectangle[2] > rectangle[0]))
	{
		rectangle[0] = x0;
		rectangle[1] = y0;
		rectangle[2] = x1;
		rectangle[3] = y1;
		return;
	}
	if (x0 < rectangle[0])
		rectangle[0] = x0;
	if (y0 < rectangle[1])
		rectangle[1] = y0;
	if (x1 > rectangle[2])
		rectangle[2] = x1;
	if (y1 > rectangle[3])
		rectangle[3] = y1;
}

void halo_hud_group_measure(int on)
{
	measuring = on != 0;
}

void halo_hud_group_frame_begin(void)
{
	int index, axis;

	for (index = 0; index <= HALO_HUD_GROUP_COUNT; index++)
		for (axis = 0; axis < 4; axis++)
			rectangles[index][axis] = 0.0f;
	/* a span left open by the last frame (it never is) ends here */
	span_depth = 0;
	corner_group = HALO_HUD_GROUP_NONE;
	unit_corner = -1;
	measuring = 1;
}

int halo_hud_group_rectangle(int group, float rectangle[4])
{
	const float *from;
	int axis;

	if (group < HALO_HUD_GROUP_NONE || group >= HALO_HUD_GROUP_COUNT)
	{
		for (axis = 0; axis < 4; axis++)
			rectangle[axis] = 0.0f;
		return 0;
	}
	from = group_rectangle(group);
	for (axis = 0; axis < 4; axis++)
		rectangle[axis] = from[axis];
	return from[2] > from[0];
}
