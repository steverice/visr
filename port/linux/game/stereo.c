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
double config_real(const char *name);

/* port/linux/src/sdl_platform.c (the host's port/ios/host/host_gpu.c on iOS);
   d3d8_device.c declares it the same way */
void platform_video_drawable_size(int *width, int *height);
void platform_log(const char *format, ...);

/* the debug side-by-side eyes: fixed frusta and a typical eye separation */
#define SIDE_BY_SIDE_TANGENT 0.8f
#define SIDE_BY_SIDE_OFFSET 0.0105f

static struct halo_stereo_frame stereo_frame;
static int stereo_layer = HALO_STEREO_LAYER_MONO;
static int stereo_mode = -1; /* read once, on the first frame */

static const char *const mode_names[] = {"off", "head", "screen", "side_by_side"};

static float z_near_world, z_far_world;

/* the mode a display.stereo string names; sets *recognized to 0 for an unknown string */
static int mode_from_name(const char *name, int *recognized)
{
	int mode;

	*recognized = 1;
	for (mode = 0; mode < 4; mode++) {
		if (name && strcmp(name, mode_names[mode]) == 0)
			return mode;
	}
	*recognized = 0;
	return HALO_STEREO_OFF;
}

void halo_stereo_frame_begin(void)
{
	if (stereo_mode < 0) {
		const char *name = config_string("display.stereo");
		const char *turn = config_string("input.turn");
		int recognized;

		stereo_mode = mode_from_name(name, &recognized);
		platform_log("stereo: display.stereo %s, input.turn %s, input.snap_angle %.1f",
			mode_names[stereo_mode], turn ? turn : "(none)", config_real("input.snap_angle"));
		if (!recognized)
			platform_log("stereo: display.stereo \"%s\" is not recognized; using off", name ? name : "(none)");
	}

	memset(&stereo_frame, 0, sizeof(stereo_frame));
	stereo_frame.mode = stereo_mode;
	stereo_layer = HALO_STEREO_LAYER_MONO;

	if (stereo_mode == HALO_STEREO_SIDE_BY_SIDE) {
		int width = 0, height = 0;
		int eye;

		platform_video_drawable_size(&width, &height);
		if (width <= 0 || height <= 0)
			return; /* no drawable yet: mono this frame */
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

void halo_stereo_set_depth_range(float z_near, float z_far)
{
	z_near_world = z_near;
	z_far_world = z_far;
}

void halo_stereo_depth_range(float *z_near, float *z_far)
{
	*z_near = z_near_world;
	*z_far = z_far_world;
}
