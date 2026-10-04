/* Head-tracked stereo and stereo on the theater screen (display.stereo =
"head" or "screen", visionOS 26 and later): host_stereo.m */
#pragma once

#include "halo_stereo.h"

/* the guest's import (guest_host.h): at the game's frame begin, in HEAD or
SCREEN mode, opens the Compositor's frame and fills the eyes (and in HEAD
mode the head's turn) from it; eye_count stays 0 without one, and on every
other platform */
void host_stereo_frame(struct halo_stereo_frame *frame);
/* the eyes' picture size in pixels while the head drives the view (the view's
size in the Compositor's texture); 0 otherwise */
int host_stereo_picture_size(int *width, int *height);

#ifdef __OBJC__
#import <Metal/Metal.h>
/* whether a stereo frame can present: host_stereo_frame opened it and the
space is still open */
int host_stereo_ready(void);
/* draws each eye's picture and depth full view into its Compositor view, the
HUD on a head-locked quad over both, and presents the frame; the game's
command buffer, which drew them, is committed already. Brightness (0 to 1)
dims both toward black, for the fade of a cut to or from the screen; the HUD
quad has hud_aspect's shape (its width over its height) */
void host_stereo_present(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right,
	id<MTLTexture> left_depth, id<MTLTexture> right_depth, id<MTLTexture> hud, float hud_aspect,
	float near_meters, float far_meters, float brightness);
#endif
