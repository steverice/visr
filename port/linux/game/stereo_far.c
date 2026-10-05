/*
STEREO_FAR.C

Far scenery in a stereo eye pass (the stereo spec's "First-person scale and
the body", the ring). Some objects stand in for something far bigger and
farther than their model: a10's ring is scenery\halo\halo, a vehicle-group
tag about 120 m across (bounding radius 20 world units), which the script
halo_setup creates as x20_halo about 44 units (134 m) beyond the bridge's
window. The flat game hides the scale with perspective alone; stereo shows
it at its real size and distance, a model nearby, not a ring thousands of
kilometers across.

In an eye pass (halo_stereo_eye_position), render_objects.c hands such an
object's node matrices here before it draws, and draws a copy translated by
the eye's position less the center camera's: about 0.0105 units (3 cm),
moving with the eye. Each eye then sees the object in the direction the
center camera sees it, at the same place on the screen: no disparity, so it
reads as infinitely far, as the sky does (render_sky, which centers on the
eye the same way). The shift is far below anything the depth test against
nearer geometry could notice.

The object's own node matrices never change (markers, attachments and
collision read them; the game state must not see the render). The copies
live in FAR_SCENERY_SLOTS static arrays used in turn, never a stack array:
transparent geometry keeps the node matrices' pointer until the eye's
transparent pass (rasterizer_xbox_transparent_geometry.c), so each far
object drawn in one eye pass needs its own array. More than
FAR_SCENERY_SLOTS far objects in one pass would reuse the first one's.

port/ios/tests/stereo_far_probe.c includes this file with the game's types
in the shapes it reads.
*/

#include <string.h>

#ifndef STEREO_FAR_PROBE
#include "cseries.h"
#include "math/real_math.h"
#include "models/model_definitions.h"
#include "../src/halo_stereo.h"
#endif

/* the tags drawn at infinity in an eye pass, by their full tag name: add
another level's stand-ins here (c40 and others may have similar props) */
static const char *const far_scenery_tags[] =
{
	"scenery\\halo\\halo",
};

#define FAR_SCENERY_SLOTS 8

static real_matrix4x3 far_scenery_matrices[FAR_SCENERY_SLOTS][MAXIMUM_NODES_PER_MODEL];
static int far_scenery_next_slot;

int halo_stereo_far_scenery(const char *tag_name)
{
	unsigned i;

	if (!tag_name)
		return 0;
	for (i = 0; i < sizeof(far_scenery_tags) / sizeof(far_scenery_tags[0]); i++)
	{
		if (!strcmp(tag_name, far_scenery_tags[i]))
			return 1;
	}
	return 0;
}

const struct real_matrix4x3 *halo_stereo_far_matrices(const char *tag_name,
	const struct real_matrix4x3 *matrices, short node_count, const union real_point3d *center)
{
	const union real_point3d *eye = halo_stereo_eye_position();
	real_matrix4x3 *copy;
	real offset_x, offset_y, offset_z;
	short i;

	if (!eye || !center || !matrices || node_count <= 0 || node_count > MAXIMUM_NODES_PER_MODEL ||
		!halo_stereo_far_scenery(tag_name))
	{
		return matrices;
	}
	/* port: in an eye pass far scenery moves with the eye, so it has no disparity (at infinity) */
	offset_x = eye->x - center->x;
	offset_y = eye->y - center->y;
	offset_z = eye->z - center->z;
	copy = far_scenery_matrices[far_scenery_next_slot];
	far_scenery_next_slot = (far_scenery_next_slot + 1) % FAR_SCENERY_SLOTS;
	for (i = 0; i < node_count; i++)
	{
		copy[i] = matrices[i];
		copy[i].position.x += offset_x;
		copy[i].position.y += offset_y;
		copy[i].position.z += offset_z;
	}
	return copy;
}
