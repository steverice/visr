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
	HALO_STEREO_LAYER_HUD = 2
};

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
};

void halo_stereo_frame_begin(void);                    /* latches this frame's state */
const struct halo_stereo_frame *halo_stereo_frame(void);
void halo_stereo_layer(int layer);                     /* -1 mono, 0/1 an eye, 2 the HUD */
int halo_stereo_current_layer(void);
/* The HUD layer clears to transparent, and the presenters put it over the
eyes by its alpha (premultiplied). The game's screen geometry writes color
only (D3DRS_COLORWRITEENABLE without alpha), so in that layer it adds light
and never covers: a black quad there shows nothing. While covering is set,
screen geometry writes alpha too (rasterizer_xbox_dynavobgeom.c), for what
must hide the picture: the cutscene bars (cinematics.c) */
void halo_stereo_set_covering(int covering);
int halo_stereo_covering(void);

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
