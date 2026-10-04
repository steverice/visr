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

/* HEAD mode's look (source/game/player_control.c): the stick turns yaw only,
smoothly or in input.snap_angle steps, and never pitch; and the head's yaw
since the look last took it in, with any snap, unscaled by the zoom, and the
pitch that brings the look's (current_pitch) to the head's. Both do nothing
unless the head drives the view. */
void halo_stereo_stick_look(short gamepad_index, float *yaw, float *pitch);
int halo_stereo_head_look(short gamepad_index, float current_pitch, float *yaw, float *pitch);
/* HEAD mode's render (source/render/render.c): turns a camera by the head's
yaw the look hasn't taken in yet (it does next frame) and gives it the head's
pitch and roll, so the camera's orientation is the head's this frame */
void halo_stereo_head_orient(float forward[3], float up[3]);
/* the eye cameras' near and far planes in world units, for the presenter's
depth (d3d8_device.c), set by the eye loop each stereo frame */
void halo_stereo_set_depth_range(float z_near, float z_far);
void halo_stereo_depth_range(float *z_near, float *z_far);

/* The cutscene screen. In stereo, while the letterbox is in, the game plays
as a 3D film on a 16:9 screen (stereo.c): the cinematic camera rendered
twice, framed on the letterbox's inside with no bars except while a title
shows. A scripted camera without the letterbox, and a third-person camera,
go on the screen the same way, at the game's own 16:9 framing. */
/* 1 when this frame is the 3D film: stereo with eyes, and the letterbox in,
the camera scripted or (by default) third person */
int halo_stereo_film(void);
/* the film's frusta, from the camera's own vertical tangent (render.c's eye
loop, before it reads the eyes); nothing unless this frame is the film */
void halo_stereo_film_frusta(float vertical_tangent);
/* port/linux/game/cinematic_screen.c: 1 while the letterbox is in; while
the camera is scripted (the script's camera_control); while it's third
person */
int halo_cinematic_screen(void);
int halo_scripted_camera(void);
int halo_third_person_camera(void);
/* the script fade (fade_in, fade_out; never a screen flash) with the tick
fraction: its RGB and intensity, zero intensity if none */
void halo_screen_fade(float rgb_intensity[4]);
/* source/cutscene/cinematics.c: 0..1, how far the bars are in, following
the active title's fades */
float halo_cinematic_title_bars(void);

#endif
