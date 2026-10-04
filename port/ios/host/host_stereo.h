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
/* the immersive space opened (again): the once-only stereo logs repeat */
void host_stereo_space_opened(void);

#ifdef __OBJC__
#import <Metal/Metal.h>
/* whether a stereo frame can present: host_stereo_frame opened it and the
space is still open */
int host_stereo_ready(void);
/* draws each eye's picture and depth full view into its Compositor view, the
HUD's pieces over both (host_stereo_hud.h), and presents the frame; the
game's command buffer, which drew them, is committed already. Brightness (0
to 1) dims both toward black, for the fade of a cut to or from the screen;
vignette (0 to 1) darkens the eyes' edges, not the HUD, for
input.comfort_vignette. The HUD is laid out at hud_aspect's shape (its width
over its height); with hud_ui it holds a menu, the console or a progress bar
and goes whole on the UI's quad; reticle is where its center points in the
eyes' frame, and hud_tangents the HUD pass's projection, for the catch-all
quad (gpu_stereo_present). inset (nil: none) is the zoom's inset, laid out as the
HUD, whose central square goes on its own quad over the HUD
(host_stereo_hud_inset) */
void host_stereo_present(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right,
	id<MTLTexture> left_depth, id<MTLTexture> right_depth, id<MTLTexture> hud, float hud_aspect,
	int hud_ui, const float reticle[3], const float hud_tangents[2], id<MTLTexture> inset, float near_meters,
	float far_meters, float brightness, float vignette);
/* HEAD mode without eyes this frame (a load: a mono picture), or with a
menu over the film (the main menu's scripted scene, the pause menu in a
cutscene): 1 while the Compositor's frame host_stereo_frame opened can take
host_stereo_present_ui */
int host_stereo_ui_ready(void);
/* that picture, with the HUD layer (nil: none) over it, on the UI's quad,
level and turning with the head's yaw, inside foveation's sharp region
(host_stereo_hud.h), over the dark surroundings or the room */
void host_stereo_present_ui(id<MTLCommandQueue> queue, id<MTLTexture> picture, id<MTLTexture> hud);
#endif
