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
	float head_yaw, head_pitch, head_roll; /* radians, the head's rotation since the last frame (HEAD mode) */
	struct halo_stereo_eye eyes[2];
	int32_t eye_width, eye_height;     /* pixels per eye picture */
};

void halo_stereo_frame_begin(void);                    /* latches this frame's state */
const struct halo_stereo_frame *halo_stereo_frame(void);
void halo_stereo_layer(int layer);                     /* -1 mono, 0/1 an eye, 2 the HUD */
int halo_stereo_current_layer(void);

#endif
