/*
STEREO.C

The frame's stereo state (port/linux/src/halo_stereo.h): which stereo mode
display.stereo asks for, and, in stereo, where the two eyes are. Nothing
renders differently yet; later code reads halo_stereo_frame().
*/

#include <string.h>

#include "../src/halo_stereo.h"

/* port/linux/src/port_config.c */
const char *config_string(const char *name);

/* port/linux/src/sdl_platform.h */
void platform_video_drawable_size(int *width, int *height);

/* the debug side-by-side eyes: fixed frusta and a typical eye separation */
#define SIDE_BY_SIDE_TANGENT 0.8f
#define SIDE_BY_SIDE_OFFSET 0.0105f

static struct halo_stereo_frame stereo_frame;
static int stereo_layer = HALO_STEREO_LAYER_MONO;
static int stereo_mode = -1; /* read once, on the first frame */

static int mode_from_name(const char *name)
{
	if (name) {
		if (strcmp(name, "head") == 0)
			return HALO_STEREO_HEAD;
		if (strcmp(name, "screen") == 0)
			return HALO_STEREO_SCREEN;
		if (strcmp(name, "side_by_side") == 0)
			return HALO_STEREO_SIDE_BY_SIDE;
	}
	return HALO_STEREO_OFF;
}

void halo_stereo_frame_begin(void)
{
	if (stereo_mode < 0)
		stereo_mode = mode_from_name(config_string("display.stereo"));

	memset(&stereo_frame, 0, sizeof(stereo_frame));
	stereo_frame.mode = stereo_mode;
	stereo_layer = HALO_STEREO_LAYER_MONO;

	if (stereo_mode == HALO_STEREO_SIDE_BY_SIDE) {
		int width = 0, height = 0;
		int eye;

		platform_video_drawable_size(&width, &height);
		stereo_frame.eye_count = 2;
		stereo_frame.eye_width = width / 2;
		stereo_frame.eye_height = height;
		for (eye = 0; eye < 2; eye++) {
			struct halo_stereo_eye *e = &stereo_frame.eyes[eye];

			e->offset[0] = eye == 0 ? -SIDE_BY_SIDE_OFFSET : SIDE_BY_SIDE_OFFSET;
			e->left = e->right = e->up = e->down = SIDE_BY_SIDE_TANGENT;
		}
	}
	/* HEAD and SCREEN stay mono (eye_count 0) until the host supplies the
	   eyes (Task 5 connects host_stereo_frame here). */
}

const struct halo_stereo_frame *halo_stereo_frame(void)
{
	return &stereo_frame;
}

void halo_stereo_layer(int layer)
{
	stereo_layer = layer;
}

int halo_stereo_current_layer(void)
{
	return stereo_layer;
}
