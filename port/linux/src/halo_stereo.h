/*
HALO_STEREO.H

The frame's stereo state for the Apple Vision Pro port: whether this frame
renders two eyes, where each eye sits and what its frustum is. Latched once
per frame by halo_stereo_frame_begin (port/linux/game/stereo.c). Plain C
types only: both the game's files and the platform's files include it.
*/

#ifndef HALO_STEREO_H
#define HALO_STEREO_H

#include <stdint.h>

enum halo_stereo_mode
{
	HALO_STEREO_OFF,
	HALO_STEREO_HEAD,
	HALO_STEREO_SCREEN,
	HALO_STEREO_SIDE_BY_SIDE
};

enum
{
	HALO_STEREO_LAYER_MONO = -1,
	HALO_STEREO_LAYER_HUD = 2,
	/* the zoom's inset (halo_stereo_inset_begin) */
	HALO_STEREO_LAYER_INSET = 3,
	/* the crosshairs (halo_stereo_reticle_overlay): the presenter's
	head-locked reticle quad shows this layer whole */
	HALO_STEREO_LAYER_RETICLE = 4,
	/* d3d8_device.c's key for a HUD group's own target: this plus the group
	(enum halo_hud_group). The game's layer stays HALO_STEREO_LAYER_HUD; the
	device keys the HUD layer's color target by the current group, and the
	HUD layer's own target is the catch-all's (HALO_HUD_GROUP_NONE) */
	HALO_STEREO_LAYER_HUD_GROUP = 8
};

/* The HUD split by the function that draws each element (the stereo spec's
"The reticle split, by draw"; port/linux/game/hud_group.c). The HUD's
drawing functions open a span for their group (hud_weapon.c, hud_unit.c,
hud_messaging.c); in a stereo frame's HUD layer, d3d8_device.c draws each
group into a target of its own and adds each draw's screen extent to its
group's rectangle, which only sizes and places the group's quad
(gpu_stereo_present). A draw in no span takes the group of the corner
hud_calculate_point last placed an element from (halo_hud_group_corner),
else HALO_HUD_GROUP_NONE: the catch-all, the HUD layer itself. What the HUD
projects onto the world (nav points, whose distance numbers CE places from
the top-left corner, damage indicators, players' markers and names) is in
an explicit HALO_HUD_GROUP_NONE span (hud.c), so no corner claims it. The
weapon group holds the grenades, which CE draws in the same top-left
corner. */
enum halo_hud_group
{
	HALO_HUD_GROUP_NONE = -1,
	HALO_HUD_GROUP_WEAPON,             /* render_weapon_hud, render_grenade_hud */
	HALO_HUD_GROUP_UNIT,               /* hud_render_unit_interface, but for its motion sensor */
	HALO_HUD_GROUP_TRACKER,            /* the motion sensor */
	HALO_HUD_GROUP_PROMPT,             /* help text, objectives and state messages (pickup prompts) */
	HALO_HUD_GROUP_MESSAGES,           /* the message list (pickups, checkpoints) */
	/* the unit span's elements CE anchors top left: a vehicle driver's seat
	labels and their bars (halo_hud_group_corner) */
	HALO_HUD_GROUP_SEATS,
	HALO_HUD_GROUP_COUNT
};
/* spans nest: the innermost wins */
void halo_hud_group_begin(int group);
void halo_hud_group_end(void);
/* hud_calculate_point, with the element's corner (_hud_anchor_*): outside
every span, the draws that follow take the corner's group (top left the
weapon's, top right the unit's, bottom left the tracker's) until the next
span begins or ends, the next element's corner, the HUD's end
(halo_hud_group_forget_corner) or the next frame; bottom right and the
center are the catch-all's. Inside the unit's span it splits that span by
corner the same way: top left is the seats' group (a driver's seat labels,
so no group crosses the layout's center), the center the catch-all's, and
the rest stay the unit's */
void halo_hud_group_corner(short corner);
/* interface_draw_hud, as the HUD ends: a corner taken outside the spans
doesn't reach past it */
void halo_hud_group_forget_corner(void);
/* the group a HUD-layer draw goes to now */
int halo_hud_group_current(void);
/* d3d8_device.c, per HUD-layer draw: its screen extent in layout lines (the
target's units: x across the layout's width, y down 480 lines), added to the
current group's rectangle (HALO_HUD_GROUP_NONE's too). With measuring off,
nothing is added (render.c's screen flash, which covers the whole layer) */
void halo_hud_group_extent(float x0, float y0, float x1, float y1);
/* the same for the group whose target the draw went to, which d3d8_device.c
names: while the HUD isn't split (halo_stereo_hud_split: the film, SCREEN
gameplay) every draw goes to the HUD layer, the catch-all's, whatever
span it's in */
void halo_hud_group_extent_in(int group, float x0, float y0, float x1, float y1);
void halo_hud_group_measure(int on);
/* render.c, as its HUD pass begins: every group's rectangle empty again */
void halo_hud_group_frame_begin(void);
/* a group's rectangle this frame (HALO_HUD_GROUP_NONE included): x0, y0,
x1, y1 in layout lines; returns 0 and an empty rectangle (all zero) if
nothing drew */
int halo_hud_group_rectangle(int group, float rectangle[4]);

struct halo_stereo_eye
{
	float offset[3];                   /* world units, in the camera's frame: right, up, back */
	float left, right, up, down;       /* positive tangents of the eye's frustum */
};

struct halo_stereo_frame
{
	int32_t eye_count;                 /* 0: mono this frame; 2: stereo */
	int32_t mode;                      /* enum halo_stereo_mode */
	/* HEAD mode, radians: the head's yaw since the last frame (about the
	room's up, left positive), and its pitch (up positive) and roll (left ear
	down positive) now. The yaw adds to the player's look, the pitch sets it;
	the roll only tilts the eye cameras, since the game's look has none. */
	float head_yaw, head_pitch, head_roll;
	struct halo_stereo_eye eyes[2];
	int32_t eye_width, eye_height;     /* pixels per eye picture */
	/* HEAD mode's full view with display.foveation: 1 while the views render
	through the Compositor's rate maps. eye_width and eye_height are then the
	maps' screen (logical) size, and display.render_scale doesn't apply: the
	render quality takes its place */
	int32_t foveated;
	/* and with debug.foveation_eye_passes (host_stereo_foveated_size): the
	size each eye's screen-sized targets are allocated at, the drawable's
	color texture per view, which the eye's rate map fills from the top left
	(its physical size, at most this); the game's eye passes render through
	the map. 0 when the eyes render unfoveated at eye_width by eye_height
	(composite-only foveation, or none) */
	int32_t foveated_width, foveated_height;
};

void halo_stereo_frame_begin(void);                    /* latches this frame's state */
const struct halo_stereo_frame *halo_stereo_frame(void);
/* -1 mono, 0/1 an eye, 2 the HUD, 3 the zoom's inset, 4 the reticle. The
HUD layer (and the reticle's and each HUD group's target) holds
premultiplied color and, in alpha, how much of the picture still shows
under it (d3d8_device.c, hud_layer_blend): the presenters put it over each
eye as rgb + eye * alpha */
void halo_stereo_layer(int layer);
int halo_stereo_current_layer(void);
/* 1 in a stereo pass that draws the frame's moment again after eye 0 (eye
1, the zoom's inset): what advances by the frame's time while it renders
(glow, fog's wind) advances in eye 0 only */
int halo_stereo_repeat_pass(void);

/* Zoom in head-tracked stereo (the stereo spec's D5: a mono inset, as
HaloCEVR does). While the local player is zoomed in HEAD mode's full view
(or the side-by-side view, which stands for it on the Mac), the eyes keep
the headset's field of view, at normal scale, without the zoom's screen
effects (the scope mask, the zoom's blur), and the zoomed camera renders
once more, mono, into the inset layer: the game's screen as mono draws it
zoomed, at square pixels, HALO_STEREO_INSET_HEIGHT_SHARE of the eyes'
height, with the zoom's screen effects, its crosshairs and the zoomed
view's HUD elements (halo_stereo_inset_overlay) and the frame's flash. The
presenter shows the inset's central square, HALO_STEREO_INSET_LINES layout
lines on a side, on a quad HALO_STEREO_INSET_WIDTH_METERS wide and
HALO_STEREO_INSET_DISTANCE_METERS ahead along the reticle's direction
(halo_stereo_reticle): head-locked on foot, on the HUD's plane and under the
HUD's pieces, which stay readable over it. The zoomed camera's vertical
field of view is the quad's angle over the zoom's magnification
(halo_stereo_inset_field_of_view), so the picture is that many times the
world around it. Not on the screen (SCREEN mode, the film): there the
game's own zoom shows on the screen, as in mono. */
/* the debug side-by-side view's eyes (stereo.c): each eye's half tangents,
and its offset from the camera in world units */
#define HALO_STEREO_SIDE_BY_SIDE_TANGENT 0.8f
#define HALO_STEREO_SIDE_BY_SIDE_OFFSET 0.0105f
#define HALO_STEREO_INSET_DISTANCE_METERS 2.0f
#define HALO_STEREO_INSET_WIDTH_METERS 0.8f
#define HALO_STEREO_INSET_HEIGHT_SHARE 0.5f
#define HALO_STEREO_INSET_LINES 480.0f
/* render.c's eye loop, before the eyes and after halo_stereo_head_orient:
whether the local player is zoomed this frame; returns 1 if the frame
renders the inset's pass. The pass waits while the last frame's HUD layer
held a menu (halo_stereo_set_ui_shown) or a seat's aim
(halo_stereo_reticle) points behind the eyes, where the presenter wouldn't
show it; the frame is still the inset's for the eyes
(halo_stereo_eye_unzoomed) and the HUD (halo_stereo_inset_overlay), so the
eyes never show the zoom's mask */
int halo_stereo_inset_begin(int zoomed);
/* d3d8_device.c, as it presents a stereo frame: whether the HUD layer went
whole on the UI's quad (a menu, the console, a progress bar) */
void halo_stereo_set_ui_shown(int shown);
/* the inset's pass shades only the square the presenter shows and a margin
beside it (d3d8_device.c), as wide as the zoom's screen effect reads past a
pixel: at least HALO_STEREO_INSET_MARGIN_LINES of the screen's 480 lines,
and more if a blur or warp has read farther this run, plus
HALO_STEREO_INSET_MARGIN_SLACK_LINES for the bilinear taps.
rasterizer_xbox_screen_effect.c reports each reach in the inset's pass
(the convolution radius, twice it for a warp without a mask) */
#define HALO_STEREO_INSET_MARGIN_LINES 30.0f
#define HALO_STEREO_INSET_MARGIN_SLACK_LINES 2.0f
void halo_stereo_inset_blur_reach(float lines);
float halo_stereo_inset_margin_lines(void);
/* the inset's vertical field of view in radians for the zoom's
magnification (the game's weapon_get_zoom_magnification): the quad's
angle with its tangent divided by the magnification, so things in it are
that many times their size beside it */
float halo_stereo_inset_field_of_view(float magnification);
/* port/linux/game/cinematic_screen.c: the local player's zoom
magnification, from the weapon of the unit that aims (a seat's gun too); 1
unzoomed */
float halo_zoom_magnification(short local_player_index);
/* 1 while this frame renders the inset's pass (halo_stereo_inset_begin's
result, to the frame's end) */
int halo_stereo_inset(void);
/* 1 in an eye's layer of a frame with the inset: the zoom's screen effects
(interface.c) stay out of the eyes */
int halo_stereo_eye_unzoomed(void);
/* hud_weapon.c, around the weapon's crosshairs and the zoomed view's
elements (the scope's angle ticks and range numbers): with on, in the HUD
layer of a frame with the inset, they draw into the inset instead; off puts
the HUD layer back */
void halo_stereo_inset_overlay(int on);
/* 1 while this frame's HUD is split into the reticle's layer and the HUD
groups' targets: a stereo frame of the full view (HEAD mode, or the
side-by-side view, which stands for it on the Mac), not the film or SCREEN
gameplay, where the HUD goes on the screen whole */
int halo_stereo_hud_split(void);
/* hud_weapon.c, around crosshairs_draw: with on, in the HUD layer, the
crosshairs draw into the reticle's layer (HALO_STEREO_LAYER_RETICLE) while
the HUD is split, or in a frame with the inset into the inset, as
halo_stereo_inset_overlay; off puts the HUD layer back. Nothing else draws
into the reticle's layer */
void halo_stereo_reticle_overlay(int on);
/* 1 once a crosshair drew into the reticle's layer this frame (render.c
flashes it as it does the HUD layer); d3d8_device.c calls
halo_stereo_reticle_drew as a draw goes to that layer */
int halo_stereo_reticle_drawn(void);
void halo_stereo_reticle_drew(void);
/* the inset's camera, from the game's camera before the head turned it
(render.c): on foot it turns with the head as the eyes' cameras do
(halo_stereo_head_orient), since the look is the head's; in a
head-tracked third-person seat it stays the game's camera, where the gun
aims, which halo_stereo_reticle points the presenter's quad along */
void halo_stereo_inset_orient(float forward[3], float up[3]);

/* render.c sets this around what draws a menu, the console or the progress
bar into the HUD layer: in HEAD mode the presenter then puts the whole layer
on the UI's quad rather than splitting it into the HUD's pieces
(d3d8_device.c notes whether anything drew under it) */
void halo_stereo_set_ui_span(int on);
int halo_stereo_ui_span(void);
/* HEAD mode: where the game's crosshair (the center of the HUD layer)
points, as a direction in the eye cameras' frame (x right, y up, z back,
unnormalized): straight ahead (0, 0, -1) on foot, where the game's look is
the head's; in a head-tracked third-person camera (a vehicle seat), the game
camera's forward, which the gun follows, in the picture the head turned
(halo_stereo_head_orient). For the presenter's reticle (gpu_stereo_present) */
void halo_stereo_reticle(float direction[3]);
/* the HUD pass's projection this frame (render.c): its half tangents across
and up, centered on the eye cameras' forward; 0 before it runs. The
presenter's catch-all quad shows the HUD layer at that size, so what the
game projects onto the HUD (nav points) points where it projected it */
void halo_stereo_set_hud_tangents(float across, float up);
void halo_stereo_hud_tangents(float tangents[2]);

/* HEAD mode's look (source/game/player_control.c): the stick turns yaw only,
as input.turn says (in input.snap_angle steps, smoothly at
input.smooth_turn_speed, or not at all), and never pitch; it takes the
stick's yaw (*yaw, -1 to 1), the game's response curve at the stick's own
yaw, without the diagonal boost the pitch gives *yaw (yaw_response)
and the frame's time in seconds, and zeroes the game's own stick turn. And the
head's yaw since the look last took it in, with the stick's turn, unscaled by
the zoom, and the pitch that brings the look's (current_pitch) to the head's.
Both do nothing unless the head drives the view, and nothing in a
third-person camera (a vehicle seat's chase camera), where the stick turns
and pitches the look as in mono and the head never turns it. */
void halo_stereo_stick_look(short gamepad_index, float yaw_response, float time_delta, float *yaw, float *pitch);
int halo_stereo_head_look(short gamepad_index, float current_pitch, float *yaw, float *pitch);
/* 1 while the head drives the look (HEAD mode with the Compositor's eyes),
for the look's autolevel */
int halo_stereo_head_drives_look(short gamepad_index);
/* input.comfort_vignette: how strongly (0 to 1) the HEAD-mode presenter
darkens the eyes' edges this frame, while the stick turns the look smoothly;
0 otherwise */
float halo_stereo_vignette(void);
/* the vignette's next strength from its strength now, the turn rate's share of
120 degrees per second (0 to 1, more is full) and the time since, in seconds: it eases
toward the share, in over 0.1 s and out over 0.2 s */
float halo_stereo_vignette_ease(float strength, float turn_fraction, float time_delta);
/* HEAD mode's render (source/render/render.c): turns a camera by the head's
yaw the look hasn't taken in yet (it does next frame) and gives it the head's
pitch and roll, so the camera's orientation is the head's this frame. In a
third-person camera (unless display.stereo_vehicle_screen puts it on the
screen) it turns the game's camera by the head's yaw and change of pitch
since that camera began, and gives it the head's roll: the picture only */
void halo_stereo_head_orient(float forward[3], float up[3]);
/* The first-person body (port/linux/game/first_person_body.c): nonzero
while object_index is the local player's unit, drawn as legs below the view
in first person: HEAD mode or the side-by-side view, an eye layer, not the
film or SCREEN gameplay, the director's first person (not a scripted
camera), display.first_person_body, and a pose the game drives under the
camera (on foot, no custom animation, not dead, the pelvis under the
camera). render_objects.c draws it after the first-person weapon, under
that weapon's stencil */
int halo_first_person_body(long object_index);
/* the render-only node matrices for that body, set back along the facing
(facing: the unit's forward; only its horizontal part counts) by
display.first_person_body_offset: the body's with the torso (the spine's
subtree) collapsed to the spine node (a model with no spine node: the head
to its parent), or with shadow, the whole silhouette for its shadow. Each
in its own static array, valid until the next call of the same kind;
node_count is the smaller of the model's nodes and the object's node
matrices; returns matrices unchanged if the model's nodes aren't
recognized */
struct real_matrix4x3;
const struct real_matrix4x3 *halo_first_person_body_matrices(long model_index,
	const struct real_matrix4x3 *matrices, short node_count, const float facing[3], int shadow);
/* debug.gpu_stats, once a second of game time: the camera and the drawn
pelvis and feet in the camera's frame */
void halo_first_person_body_log(long object_index, const struct real_matrix4x3 *drawn, short node_count);
/* the eye cameras' near and far planes in world units, for the presenter's
depth (d3d8_device.c), set by the eye loop each stereo frame */
void halo_stereo_set_depth_range(float z_near, float z_far);
void halo_stereo_depth_range(float *z_near, float *z_far);

/* The screen. In stereo, while the letterbox is in, the game plays as a 3D
film on a 16:9 screen (stereo.c): the cinematic camera rendered twice,
framed on the letterbox's inside with no bars except while a title shows.
A director's scripted camera goes on the screen the same way, and in HEAD
mode the player's camera under a script, and a third-person camera with
display.stereo_vehicle_screen. In
SCREEN mode the rest is gameplay as a 3D TV: the player's camera rendered
twice, by the same mapping with gameplay's own depth, leaning with the
head. */
/* 1 when this frame is the 3D film: stereo with eyes, and the letterbox in
or a camera the head doesn't steer (above) */
int halo_stereo_film(void);
/* 1 while this frame's eyes are SCREEN gameplay's: SCREEN mode (or the
side-by-side view with debug.side_by_side_screen), not the film */
int halo_stereo_screen_gameplay(void);
/* the film's or SCREEN gameplay's frusta from the camera's vertical half
tangent, after set_window_camera_values (render.c's eye loop, before it
reads the eyes); nothing unless the frame is one of them */
void halo_stereo_screen_frusta(float vertical_tangent);
/* 1 while the screen's framing applies: the letterbox's film, or SCREEN
mode (or debug.side_by_side_screen) with display.screen_framing = "band",
in either case only while the frame has two eyes (held across the film's
hold); main.c then narrows the vertical view by 0.75, so the screen shows
the game's horizontal view across 16:9. With the immersive space closed,
SCREEN mode's window keeps mono's Hor+ view */
int halo_stereo_screen_framing(void);
/* in an eye's layer of a frame on the screen in SCREEN mode (or
debug.side_by_side_screen), gameplay or the film (its hold shows the
player's camera): the first-person weapon's eye (its offset right and up
from the camera, world units, and its tangents) by the weapon's own nearly
flat mapping, its nearest point on the screen's surface
(rasterizer_set_frustum_z); returns 0 elsewhere, HEAD mode included */
int halo_stereo_first_person_eye(struct halo_stereo_eye *eye);
/* debug.gpu_stats: logs the culling camera's distance back behind the
center camera (render.c), once for each mode and mapping */
void halo_stereo_log_culling(float distance_back);
/* display.lod_scale in HEAD mode and the side-by-side view (1.0 otherwise):
a multiplier on the pixel size the game picks model detail and particle
distance by, 1.0 the Xbox's, clamped to 0.5 to 4 */
float halo_stereo_lod_scale(void);
/* render.c's stereo frame, after it builds the culling frustum
(port/linux/game/stereo_lod.c): sets that frustum's pixel scale
(projection_world_to_screen) to mono's for the same camera, .j times
halo_stereo_lod_scale (model detail, the model cull, particles), .i not
(sprites' size), since they read it; the union of the eyes' bounds would give about half
mono's. The planes are unchanged */
struct render_camera;
struct render_frustum;
void halo_stereo_lod_projection(const struct render_camera *camera, struct render_frustum *cull_frustum);
/* The eye pass's camera position (world units). render_player_frame_stereo
(render.c) sets it around each eye's render_window and clears it (NULL)
after; it stays NULL in mono, the HUD, the zoom's inset and a mirror's
window. render_sky centers the sky on it, so the sky sits at infinity in
each eye. The pointer is the caller's, valid only while it is set */
union real_point3d;
void halo_stereo_set_eye_position(const union real_point3d *position);
const union real_point3d *halo_stereo_eye_position(void);
/* Far scenery (port/linux/game/stereo_far.c): objects that stand in for
something far bigger and farther than their model, as a10's ring
(scenery\halo\halo, about 120 m across, 134 m beyond the bridge's window),
which stereo would otherwise show at its real size and distance. 1 if the
tag name is on the list */
int halo_stereo_far_scenery(const char *tag_name);
/* render_objects.c, as an object draws: in an eye pass
(halo_stereo_eye_position), a far scenery object's node matrices translated
by the eye's position less the center camera's (center), a render-only copy,
so each eye sees it where the center camera does: no disparity, at
infinity. Otherwise matrices, unchanged */
struct real_matrix4x3;
const struct real_matrix4x3 *halo_stereo_far_matrices(const char *tag_name,
	const struct real_matrix4x3 *matrices, short node_count, const union real_point3d *center);
/* 1 when this frame is the film for a cutscene: the letterbox, held
through the film's hold after it drops. The 16:9 narrowing (main.c) and the
title bars (cinematics.c) follow this, not the letterbox flag itself, so the
framing doesn't change in the frames before the cut */
int halo_stereo_film_letterbox(void);
/* 1 while a script fade shows over the picture this frame (any intensity;
halo_stereo_set_fade): a cut between the full view and the screen then needs
no fade through black, which would otherwise dip a colored fade to black */
int halo_stereo_cut_covered(void);
/* the script fade the eyes draw this frame, with the picture's tick
fraction (render.c sets it before the eyes; zero until then), which the
host's room tint takes as well, so the two move in step */
void halo_stereo_set_fade(const float rgb_intensity[4]);
void halo_stereo_fade(float rgb_intensity[4]);
/* The film's and the 3D TV's one mapping (stereo.c has the math): from
the depth share (infinity's parallax as a share of the viewer's eye
separation), the convergence (meters ahead of the camera on the screen's
surface), the viewer's eye separation and the screen's half width (one unit
for both), the picture's vertical half tangent and an optional lean (world
units, right and up), each eye's offset and frustum */
void halo_stereo_tv_eyes(float depth_share, float convergence_meters, float viewer_separation, float half_width,
	float vertical_tangent, const float lean[2], struct halo_stereo_eye eyes[2]);
/* port/linux/game/cinematic_screen.c: 1 while the letterbox is in; while
the camera is scripted (the script's camera_control, or the player's camera
with its look taken away); while the director's scripted camera alone is
(camera_control, the scripted perspective); while it's third person */
int halo_cinematic_screen(void);
int halo_scripted_camera(void);
int halo_scripted_director_camera(void);
int halo_third_person_camera(void);
/* the script fade (fade_in, fade_out; never a screen flash) at the picture's
time: the game's tick before this one plus tick_fraction (the render's
interpolation fraction; 1.0 gives the game's own whole-tick intensity). Its
RGB and intensity into rgb_intensity, zero intensity if none; 1 while the
game has a fade active (it draws one, perhaps at intensity 0) */
int halo_screen_fade(float tick_fraction, float rgb_intensity[4]);
/* source/cutscene/cinematics.c: 0..1, how far the bars are in, following
the active title's fades */
float halo_cinematic_title_bars(void);

#endif
