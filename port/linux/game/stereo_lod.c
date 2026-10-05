/*
STEREO_LOD.C

The culling frustum's pixel scale in a stereo frame (the stereo spec's
"Pop-in: the game picks model detail for half the Xbox's pixels").

render_player_frame_stereo (render.c) culls both eyes with one frustum whose
bounds are the union of the eyes' tangents, and render_window makes it the
frame's render.frustum. Everything the game sizes in pixels reads that
frustum's projection_world_to_screen: model detail and the model cull
(models.c:757, through object_get_level_of_detail_pixels), particles' cutoff
and minimum size (render_particles.c) and sprites (render_sprite.c). With
HEAD mode's union, up plus down about 2.05 tangents against mono's 1.05, the
scale is about 0.51 times mono's, so models step down in detail and drop
out at about half the Xbox's distance. halo_stereo_lod_projection puts
mono's scale back, .j times display.lod_scale (halo_stereo_lod_scale), and
leaves the planes alone: what the union culls is still right.

Its own file rather than stereo.c's, since it reads the game's camera and
frustum structures, which stereo.c (included by probes with no game
headers) never does. port/ios/tests/stereo_lod_probe.c includes it with the
game's projection math in the shapes it reads.
*/

#ifndef STEREO_LOD_PROBE
#include "cseries.h"
#include "math/real_math.h"
#include "render/render_cameras.h"
#include "render/render_cameras_internal.h"
#include "../src/halo_stereo.h"

/* port/linux/src/sdl_platform.c (the host's on iOS) */
void platform_log(const char *format, ...);
#endif

/* the level-of-detail scale is logged on the first stereo frame, and again
when j moves more than LOD_LOG_CHANGE of the value last logged, at most once
every LOD_LOG_INTERVAL stereo frames (b30's first stereo frame is its
cutscene's camera, gameplay's comes later) */
#define LOD_LOG_CHANGE 0.01f
#define LOD_LOG_INTERVAL 600ul
static float lod_logged_j;
static unsigned long lod_frames, lod_logged_frame;

void halo_stereo_lod_projection(const struct render_camera *camera, struct render_frustum *cull_frustum)
{
	real_rectangle2d mono_bounds;
	struct render_frustum mono_frustum;
	float lod_scale = halo_stereo_lod_scale();
	float union_j = cull_frustum->projection_world_to_screen.j;
	float change;

	/* mono's frustum for the same camera: the window's own bounds, as
	render_player_frame builds them, not widened to the eyes' union */
	render_camera_build_frustum_bounds(camera, &mono_bounds);
	render_camera_build_frustum(camera, &mono_bounds, &mono_frustum, TRUE);
	/* port: model detail, the model cull (models.c:757), particles and sprites read the frustum's pixel scale; keep mono's, not the union's */
	/* sprites alone read .i (render_sprite.c), to turn a pixel width into a
	world size: it stays mono's, so display.lod_scale never resizes them */
	cull_frustum->projection_world_to_screen.i = mono_frustum.projection_world_to_screen.i;
	cull_frustum->projection_world_to_screen.j = mono_frustum.projection_world_to_screen.j * lod_scale;
	lod_frames++;
	change = cull_frustum->projection_world_to_screen.j - lod_logged_j;
	if (change < 0.0f)
		change = -change;
	if (lod_logged_j <= 0.0f ||
		(change > lod_logged_j * LOD_LOG_CHANGE && lod_frames - lod_logged_frame >= LOD_LOG_INTERVAL))
	{
		lod_logged_j = cull_frustum->projection_world_to_screen.j;
		lod_logged_frame = lod_frames;
		platform_log("stereo: level of detail scale j %.1f (mono %.1f, union %.1f, lod_scale %.2f)",
			(double)cull_frustum->projection_world_to_screen.j, (double)mono_frustum.projection_world_to_screen.j,
			(double)union_j, (double)lod_scale);
	}
}
