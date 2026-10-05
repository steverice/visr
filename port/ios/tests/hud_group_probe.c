/* The HUD split by draw (port/linux/game/hud_group.c, included): extents
added inside nested spans go to the innermost span's group, and outside
every span to the catch-all (HALO_HUD_GROUP_NONE), or to the group of the
corner hud_calculate_point last placed an element from; each frame's
rectangles empty again as the HUD pass begins; a group nothing drew in
reports an empty rectangle; the screen flash, drawn with measuring off,
adds nothing. */
#include <stdio.h>

#include "../../linux/game/hud_group.c"

static int failures;

static void check(int condition, const char *what)
{
	printf("%s: %s\n", condition ? "ok" : "FAILED", what);
	if (!condition)
		failures++;
}

static int rectangle_is(int group, float x0, float y0, float x1, float y1)
{
	float rectangle[4];

	return halo_hud_group_rectangle(group, rectangle) && rectangle[0] == x0 && rectangle[1] == y0 &&
		rectangle[2] == x1 && rectangle[3] == y1;
}

static int empty(int group)
{
	float rectangle[4];

	return !halo_hud_group_rectangle(group, rectangle) && rectangle[0] == 0.0f && rectangle[2] == 0.0f;
}

int main(void)
{
	int group;

	halo_hud_group_frame_begin();
	for (group = HALO_HUD_GROUP_NONE; group < HALO_HUD_GROUP_COUNT; group++)
		check(empty(group), "a frame begins with every group's rectangle empty");

	/* hud_render_unit_interface's span with the motion sensor's inside */
	halo_hud_group_begin(HALO_HUD_GROUP_UNIT);
	check(halo_hud_group_current() == HALO_HUD_GROUP_UNIT, "inside a span, draws go to its group");
	halo_hud_group_extent(464.0f, 37.0f, 584.0f, 66.0f);
	halo_hud_group_begin(HALO_HUD_GROUP_TRACKER);
	check(halo_hud_group_current() == HALO_HUD_GROUP_TRACKER, "a nested span's group wins");
	halo_hud_group_extent(48.0f, 362.0f, 131.0f, 444.0f);
	halo_hud_group_end();
	check(halo_hud_group_current() == HALO_HUD_GROUP_UNIT, "after the nested span, the outer span's group again");
	/* the flashlight meter, drawn after the tracker, beside the shield bar */
	halo_hud_group_extent(470.0f, 66.0f, 560.0f, 74.0f);
	halo_hud_group_end();
	check(rectangle_is(HALO_HUD_GROUP_UNIT, 464.0f, 37.0f, 584.0f, 74.0f),
		"the unit's rectangle is the union of its own draws");
	check(rectangle_is(HALO_HUD_GROUP_TRACKER, 48.0f, 362.0f, 131.0f, 444.0f),
		"the tracker's draws are the tracker's alone, not the unit's");

	/* a nav point: outside every span */
	check(halo_hud_group_current() == HALO_HUD_GROUP_NONE, "outside every span, draws go to the catch-all");
	halo_hud_group_extent(400.0f, 200.0f, 410.0f, 210.0f);
	check(rectangle_is(HALO_HUD_GROUP_NONE, 400.0f, 200.0f, 410.0f, 210.0f), "the catch-all has its own rectangle");

	/* an element outside every span placed from a corner takes the
	corner's group, until a span begins */
	halo_hud_group_corner(1);
	check(halo_hud_group_current() == HALO_HUD_GROUP_UNIT, "a top-right element outside the spans goes with the unit");
	halo_hud_group_corner(0);
	check(halo_hud_group_current() == HALO_HUD_GROUP_WEAPON, "a top-left one with the weapon");
	halo_hud_group_corner(2);
	check(halo_hud_group_current() == HALO_HUD_GROUP_TRACKER, "a bottom-left one with the tracker");
	halo_hud_group_corner(3);
	check(halo_hud_group_current() == HALO_HUD_GROUP_NONE, "a bottom-right one with the catch-all");
	halo_hud_group_corner(4);
	check(halo_hud_group_current() == HALO_HUD_GROUP_NONE, "a centered one with the catch-all");
	halo_hud_group_corner(0);
	halo_hud_group_begin(HALO_HUD_GROUP_PROMPT);
	halo_hud_group_corner(1);
	check(halo_hud_group_current() == HALO_HUD_GROUP_PROMPT, "inside a span a corner changes nothing");
	halo_hud_group_end();
	check(halo_hud_group_current() == HALO_HUD_GROUP_NONE, "a span's end forgets an earlier corner");

	/* the screen flash covers the whole layer, with measuring off */
	halo_hud_group_measure(0);
	halo_hud_group_extent(0.0f, 0.0f, 640.0f, 480.0f);
	halo_hud_group_measure(1);
	check(rectangle_is(HALO_HUD_GROUP_NONE, 400.0f, 200.0f, 410.0f, 210.0f), "a draw with measuring off adds nothing");
	/* nothing in a degenerate extent */
	halo_hud_group_extent(10.0f, 10.0f, 10.0f, 20.0f);
	check(rectangle_is(HALO_HUD_GROUP_NONE, 400.0f, 200.0f, 410.0f, 210.0f), "an empty extent adds nothing");

	/* a draw the device sent to another group's target (the HUD not split:
	the catch-all's) counts there, whatever span it's in */
	halo_hud_group_begin(HALO_HUD_GROUP_UNIT);
	halo_hud_group_extent_in(HALO_HUD_GROUP_NONE, 420.0f, 190.0f, 430.0f, 200.0f);
	halo_hud_group_end();
	check(rectangle_is(HALO_HUD_GROUP_NONE, 400.0f, 190.0f, 430.0f, 210.0f) &&
		rectangle_is(HALO_HUD_GROUP_UNIT, 464.0f, 37.0f, 584.0f, 74.0f),
		"an extent named for a group goes to that group, not the span's");
	check(empty(HALO_HUD_GROUP_WEAPON) && empty(HALO_HUD_GROUP_PROMPT) && empty(HALO_HUD_GROUP_MESSAGES),
		"a group nothing drew in is flagged empty");
	{
		float rectangle[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

		check(!halo_hud_group_rectangle(HALO_HUD_GROUP_COUNT, rectangle) && rectangle[2] == 0.0f,
			"a group out of range is empty");
	}

	/* the unit's span split by corner: a driver's seat labels (top left)
	are the seats' group, a centered element the catch-all's, the rest the
	unit's; a nested span doesn't inherit it, and the span's end forgets it */
	halo_hud_group_begin(HALO_HUD_GROUP_UNIT);
	check(halo_hud_group_current() == HALO_HUD_GROUP_UNIT, "the unit's span starts as the unit's group");
	halo_hud_group_corner(0);
	check(halo_hud_group_current() == HALO_HUD_GROUP_SEATS, "a top-left element in the unit's span is the seats' group");
	halo_hud_group_corner(4);
	check(halo_hud_group_current() == HALO_HUD_GROUP_NONE, "a centered one is the catch-all's");
	halo_hud_group_corner(1);
	check(halo_hud_group_current() == HALO_HUD_GROUP_UNIT, "a top-right one is the unit's");
	halo_hud_group_corner(0);
	halo_hud_group_begin(HALO_HUD_GROUP_TRACKER);
	check(halo_hud_group_current() == HALO_HUD_GROUP_TRACKER, "the tracker's span inside it stays the tracker's");
	halo_hud_group_corner(2);
	halo_hud_group_end();
	check(halo_hud_group_current() == HALO_HUD_GROUP_UNIT, "after it, the unit's span is the unit's again");
	halo_hud_group_end();
	halo_hud_group_begin(HALO_HUD_GROUP_WEAPON);
	halo_hud_group_corner(0);
	check(halo_hud_group_current() == HALO_HUD_GROUP_WEAPON, "other spans aren't split by corner");
	halo_hud_group_end();
	/* the HUD's end forgets a corner taken outside the spans */
	halo_hud_group_corner(0);
	halo_hud_group_forget_corner();
	check(halo_hud_group_current() == HALO_HUD_GROUP_NONE, "the HUD's end forgets the last corner");

	/* the next frame's HUD pass */
	halo_hud_group_begin(HALO_HUD_GROUP_WEAPON);
	halo_hud_group_frame_begin();
	for (group = HALO_HUD_GROUP_NONE; group < HALO_HUD_GROUP_COUNT; group++)
		check(empty(group), "the next frame's rectangles start empty");
	check(halo_hud_group_current() == HALO_HUD_GROUP_NONE, "and no span is left open");

	printf("%s\n", failures ? "hud_group_probe: FAILED" : "hud_group_probe: PASS");
	return failures ? 1 : 0;
}
