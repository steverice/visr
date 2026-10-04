/* Theater mode (display.immersive, visionOS): host_theater.m and Theater.swift */
#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>
/* draws picture on the theater screen for the Compositor's open frame (or the
next one) */
void host_theater_present(id<MTLCommandQueue> queue, id<MTLTexture> picture);
/* stereo on the screen (display.stereo = "screen", and the cutscenes' 3D
film in any mode): each eye's picture on the screen for its view (view 0 the
left eye), the HUD (nil: none) blended over each, for the frame
host_stereo_frame opened. The screen's depth is the Compositor's, as in
mono: the picture is on a flat surface in the room. The script fade (RGB and
intensity) tints the space around the screen: in the dark it's the
surroundings' color times the intensity; in the room, the color over it at
that opacity. Brightness (0 to 1) dims the picture toward black, for the
fade of a cut to or from the screen */
void host_theater_present_eyes(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right,
	id<MTLTexture> hud, const float fade[4], float brightness);
#endif

#if defined(__OBJC__) && !defined(__swift__)
#import <CompositorServices/CompositorServices.h>
/* The Compositor's frame, shared by the theater screen and head-tracked
stereo (host_stereo.m). host_theater_frame_begin opens the next frame: it
waits for the frame and its optimal input time, then queries the drawables
and each one's device anchor at its presentation time; 0 without a frame
(the space isn't open, or the frame was canceled). Fresh (at the game's
frame begin), it first ends any frame still open unpresented; otherwise (a
present) it takes the open frame if there is one.
host_theater_frame_ready says whether the open frame can still be drawn,
dropping it if the space closed meanwhile. A present draws each drawable
and calls host_theater_frame_end. */
int host_theater_frame_begin(int fresh);
int host_theater_frame_ready(void);
size_t host_theater_drawable_count(void);
/* the open frame's drawable, where the device was (identity when not
anchored) and whether ARKit placed it; either pointer may be NULL */
cp_drawable_t host_theater_drawable(size_t index, simd_float4x4 *origin_from_device, int *anchored);
void host_theater_frame_end(void);
/* the screen's pose in the room for the open frame's drawable (placed first
if a present hasn't placed it yet) and its half width and half height in
meters, at the picture's shape (host_theater_picture_size); the screen's
center is its origin, x right, y up and z toward the viewer */
void host_theater_screen(size_t index, simd_float4x4 *origin_from_screen, simd_float2 *half_size);
#endif

/* reads display.theater_* from config.toml (host_config.c) */
void host_theater_load_settings(void);
/* display.theater_environment = "dark" */
int host_theater_dark(void);
/* the picture's size in pixels for the screen's angle */
void host_theater_picture_size(int *width, int *height);
/* opens the immersive space (Theater.swift) */
void host_theater_open(void);
/* whether the space is open, so gpu_present draws there and not in the window */
int host_theater_active(void);
/* for Theater.swift: the space's layer renderer, unretained */
void host_theater_attach(void *renderer);
/* for Theater.swift: the layer's nearest allowed near plane, in meters */
void host_theater_set_minimum_near(float meters);
/* that, or 0.1 m (the simulator's reading) before the layer reports one */
float host_theater_minimum_near(void);
void host_theater_log_c(const char *message);
/* for Theater.swift: host_join_link_open, for a join link the space was opened with */
#include "host_join_link.h"
