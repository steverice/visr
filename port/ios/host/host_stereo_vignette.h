/* The comfort vignette's mask for head-tracked stereo (input.comfort_vignette),
written once in the subset of C and the Metal Shading Language both take:
host_stereo.m compiles HOST_STEREO_VIGNETTE_SOURCE into the eye shader as
text, and port/ios/tests/stereo_head_probe.c runs it as C.

The mask darkens by the angle from straight ahead (the view's forward), not
by the place in the picture: the eyes' frusta are off-axis and mirrored
(about 1.76 to the temple, 1.01 to the nose), so a mask on the picture's
middle would sit about 20 degrees toward each temple and the two eyes would
darken different directions. By angle, with the same thresholds for both
eyes, a direction both eyes see is darkened alike in each.

The math functions are called as (atan)(...) and (sqrt)(...) so that no
function-like macro (tgmath.h's) rewrites them while the source is turned
into text. */
#ifndef HOST_STEREO_VIGNETTE_H
#define HOST_STEREO_VIGNETTE_H

/* 0 (clear) to 1 (full) at the picture's coordinate u, v (0,0 its top left,
1,1 its bottom right) of a view with the frustum's positive tangents left,
right, up and down: 0 within inner radians of straight ahead, 1 beyond
outer, smoothly between */
#define HOST_STEREO_VIGNETTE_SOURCE \
static inline float host_stereo_vignette_edge(float u, float v, float left, float right, float up, float down, \
	float inner, float outer) \
{ \
	float x = (1.0f - u) * -left + u * right; \
	float y = (1.0f - v) * up - v * down; \
	float t = ((float)(atan)((sqrt)(x * x + y * y)) - inner) / (outer - inner); \
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); \
	return t * t * (3.0f - 2.0f * t); \
}

/* the clear share of the narrowest half of the view: the vignette starts at
this share of the angle to the nearest edge of either eye's frustum and is
full at that edge */
#define HOST_STEREO_VIGNETTE_CLEAR_SHARE 0.6f

#define HOST_STEREO_VIGNETTE_TEXT(...) #__VA_ARGS__
#define HOST_STEREO_VIGNETTE_STRING(...) HOST_STEREO_VIGNETTE_TEXT(__VA_ARGS__)

#endif
