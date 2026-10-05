/* HEAD mode's HUD and UI layout (host_stereo.m): which of the HUD's layers
go where in the room. Plain C, so a host-side test can check it
(port/ios/tests/stereo_hud_probe.c).

The game draws its HUD laid out as always: 480 lines tall, its width the
window's shape (the HUD's aspect times 480), elements at the corners and the
center. In HEAD mode the guest splits it by the function that draws each
element (halo_stereo.h, the stereo spec's "The reticle split, by draw"):
the crosshairs into the reticle's layer, each HUD group (the weapon with
its grenades, the unit, the motion tracker, the prompts, the messages) into
a target of its own with the rectangle its draws cover, and everything else
into the HUD layer itself. Every one is laid out as the whole HUD. The
presenter puts:
- the reticle's layer whole, at its natural size, centered where the game's
  crosshair is (host_stereo_hud_reticle);
- each group's rectangle of its own target on a quad in the top band above
  the reticle or the bottom band below it, turning with the head's yaw only,
  level with the room, where Task 7c's pieces sat (the counters left, the
  meters right, the prompts and messages above them, the tracker below);
- the HUD layer whole, head-locked at the HUD pass's projection, the
  catch-all: nav points and the multiplayer score where the game projected
  them;
- while a menu, the console or a progress bar is in the layer (the guest's
  hud_ui), or a frame has no eyes (the main menu, a load), all of them whole
  on one level, yaw-following quad of that size instead. */
#ifndef HOST_STEREO_HUD_H
#define HOST_STEREO_HUD_H

#include "halo_stereo.h"

/* how far ahead the HUD and the UI sit, in meters */
#define HOST_STEREO_HUD_DISTANCE 2.0f

/* The radius, in degrees from the view's center with the head level, that
every element of the bands and the UI stays inside, horizontally and
vertically: foveation's sharp region. The research's one published
measurement is about 40 degrees across, about 20 either side of the view's
center (douevenknow.us, May 2024), less a 2 degree margin. Task 10 replaces
it with the radius it measures from the rate map. */
#define HUD_SHARP_RADIUS_DEGREES 18.0f

/* the HUD layer's height in the game's layout lines */
#define HOST_STEREO_HUD_LINES 480.0f

/* the HUD's scale in meters per layout line at HOST_STEREO_HUD_DISTANCE: the
first headset build's head-locked quad (1.6 m wide for a 4:3 layout, 640
lines across, so 480 lines are 1.2 m, about 33 degrees, at 2 m). The reticle
keeps it; the bands keep it unless they'd leave the sharp region, where they
shrink to fit */
#define HOST_STEREO_HUD_METERS_PER_LINE (1.6f / 640.0f)

/* the margin, in layout lines, a group's quad shows around its rectangle,
so linear filtering at the quad's edge never cuts a drawn texel (the target
holds nothing else) */
#define HOST_STEREO_HUD_GROUP_MARGIN_LINES 2.0f

/* the texture a quad shows (the presenter's array of them) */
enum
{
	/* the HUD layer itself: the catch-all, a menu */
	HOST_STEREO_HUD_LAYER_HUD,
	/* the crosshairs' layer */
	HOST_STEREO_HUD_LAYER_RETICLE,
	/* a HUD group's target: this plus the group (enum halo_hud_group) */
	HOST_STEREO_HUD_LAYER_GROUP,
	HOST_STEREO_HUD_LAYER_COUNT = HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_COUNT
};

/* what a quad's frame follows */
enum
{
	/* the device: head-locked */
	HOST_STEREO_HUD_HEAD,
	/* at the device's position, turned by the head's yaw only: level with
	the room (host_stereo_hud_level) */
	HOST_STEREO_HUD_LEVEL
};

struct host_stereo_hud_quad
{
	/* its texture's rectangle: u0, v0, u1, v1 (v down) */
	float source[4];
	/* in its frame's meters, x right, y up, z back: its center, and the
	half extents along its width and height (the texture's u and v reversed:
	+y_axis is the top) */
	float center[3], x_axis[3], y_axis[3];
	int frame;
	/* which texture it shows (HOST_STEREO_HUD_LAYER_*) */
	int layer;
	/* the catch-all: the HUD layer whole, head-locked */
	int catch_all;
	/* covers what it's over whatever its texture's alpha (the zoom's inset,
	a picture) */
	int opaque;
};

/* most quads a layout makes: the reticle, the groups and the catch-all */
#define HOST_STEREO_HUD_MAXIMUM_QUADS (HOST_STEREO_HUD_LAYER_COUNT)

/* The reticle's quad: the crosshairs' layer whole (layout_width lines by
480) at its natural size (HOST_STEREO_HUD_METERS_PER_LINE), its center on
the crosshair, in the eyes' frame (x right, y up, z back), facing back along
direction, upright. Without a position it sits
HOST_STEREO_HUD_DISTANCE along direction (the guest's halo_stereo_reticle):
straight ahead, head-locked, on foot; where the game's camera aims in a
head-tracked third-person seat; and returns 0 (no quad) when that's not
ahead of the eyes. With one (meters; Task 11's point where the controller's
aim hits) it's centered there. This is the one place the reticle is
placed: tracked-controller aiming (Task 11) replaces its caller's
arguments. */
int host_stereo_hud_reticle(float layout_width, const float position[3], const float direction[3],
	struct host_stereo_hud_quad *quad);

/* The zoom's inset (halo_stereo.h): the inset layer's central square,
HALO_STEREO_INSET_LINES on a side (the layer is laid out as the HUD,
layout_width lines across), on an opaque square quad
HALO_STEREO_INSET_WIDTH_METERS wide, facing the eyes, by the reticle's
rule (host_stereo_hud_reticle) at HALO_STEREO_INSET_DISTANCE_METERS:
straight ahead and head-locked on foot, along a head-tracked seat's aim,
none when that's not ahead, or centered at position (Task 11: the scope on
the gun). At the HUD's distance, it's drawn before the HUD's pieces, so the
counters, meters and tracker stay readable over it with no clash of depth;
the game draws the crosshairs into the inset meanwhile, so the reticle's
quad is empty */
int host_stereo_hud_inset(float layout_width, const float position[3], const float direction[3],
	struct host_stereo_hud_quad *quad);

/* the quads for a HEAD-mode frame's HUD laid out layout_width lines across,
given each HUD group's rectangle (group_extent: x0, y0, x1, y1 in layout
lines, the guest's gpu_stereo_present hud_group_extent; an empty one, or
NULL for all, draws no quad): with ui, the reticle's layer, every group's
target and the HUD layer, each whole, on the UI's quad, in that order;
else the reticle's quad (reticle: its direction, as above; none when it's
off the view) first, then one quad per group with a rectangle, in the
bands, and last, given the HUD pass's half tangents across and up (the
guest's halo_stereo_hud_tangents; NULL or 0: none), the catch-all: the HUD
layer whole, head-locked at that projection. Returns the count; quads holds
HOST_STEREO_HUD_MAXIMUM_QUADS */
int host_stereo_hud_layout(float layout_width, int ui, const float reticle[3], const float hud_tangents[2],
	const float (*group_extent)[4], struct host_stereo_hud_quad *quads);

/* the UI's quad for a picture of the given aspect (width over height): the
whole picture, centered ahead, as large as fits inside the sharp region */
void host_stereo_hud_ui(float aspect, struct host_stereo_hud_quad *quad);

/* The level frame's yaw from the device's axes in the room (ARKit's: x
right, y up, z back), radians, left positive: its forward's, or past 85
degrees of pitch its right's (host_stereo_head.c does the same). The frame
sits at the device's position, turned by that yaw about the room's up */
float host_stereo_hud_level_yaw(const float right[3], const float back[3]);

/* the HUD's scale for the bands at a layout width with the groups'
rectangles (as host_stereo_hud_layout), meters per line: the natural
scale, or smaller if the bands would leave the sharp region */
float host_stereo_hud_band_scale(float layout_width, const float (*group_extent)[4]);

/* The fade through black when HEAD mode's view changes between the full
view, the screen (the film, SCREEN gameplay) and the UI's quad (a menu over
the film, a load): the new view starts black and comes up over
HOST_STEREO_CUT_SECONDS (6 frames at 90 Hz, 3 at 45), unless a script fade
already covers the cut. The pause menu over gameplay keeps the full view and
doesn't fade. */
#define HOST_STEREO_CUT_SECONDS (6.0f / 90.0f)
enum host_stereo_view
{
	HOST_STEREO_VIEW_NONE = -1,        /* no stereo frame (mono, the space closed) */
	HOST_STEREO_VIEW_FULL,
	HOST_STEREO_VIEW_SCREEN,
	HOST_STEREO_VIEW_UI
};
struct host_stereo_cut
{
	int shown;                         /* enum host_stereo_view: what the last frame showed */
	float elapsed;                     /* seconds since the cut began; HOST_STEREO_CUT_SECONDS or more: none */
};
#define HOST_STEREO_CUT_INITIAL { HOST_STEREO_VIEW_NONE, HOST_STEREO_CUT_SECONDS }
/* this frame's brightness (0 to 1) for a frame showing shown, covered when
a script fade is over the picture, which stays up frame_seconds (the
refresh period times the repeat count plus one); *switched (may be NULL)
says whether this frame began a fade. A frame with no view forgets the
last, so a switch from it doesn't fade */
float host_stereo_cut_brightness(struct host_stereo_cut *cut, int shown, int covered, float frame_seconds,
	int *switched);

#endif
