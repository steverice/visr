/* Far scenery in a stereo eye pass (port/linux/game/stereo_far.c, included,
not linked): a10's ring, scenery\halo\halo, draws a render-only copy that
moves with the eye, so both eyes see it in the direction the center camera
does (no disparity, at infinity).

Checks: the tag list matches the ring's tag and nothing else (the
destroyed ring's and the cinematic close-up's tags, and other names, stay
as they are); outside an eye pass, for other tags and for too many nodes,
the object's own matrices come back; in an eye pass each eye's direction to
every translated node equals the center camera's to the original node
(x20_halo's place, 44 units beyond the bridge, and the eyes 0.0105 units
to either side), so the disparity is zero, where untranslated it would be
about 4.8e-4 radians; the object's own matrices never change; and two far
objects drawn in one pass get separate arrays, since transparent geometry
keeps the pointer until the pass's end. */
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef float real;
typedef struct { real i, j, k; } real_vector3d;
union real_point3d
{
	real n[3];
	struct { real x, y, z; };
};
typedef union real_point3d real_point3d;
struct real_matrix4x3
{
	real scale;
	real_vector3d forward, left, up;
	real_point3d position;
};
typedef struct real_matrix4x3 real_matrix4x3;
#define MAXIMUM_NODES_PER_MODEL 64

/* stereo.c's accessor, set by the probe as render.c's eye loop would */
static const union real_point3d *probe_eye;
const union real_point3d *halo_stereo_eye_position(void)
{
	return probe_eye;
}
int halo_stereo_far_scenery(const char *tag_name);
const struct real_matrix4x3 *halo_stereo_far_matrices(const char *tag_name,
	const struct real_matrix4x3 *matrices, short node_count, const union real_point3d *center);

#define STEREO_FAR_PROBE
#include "../../linux/game/stereo_far.c"

static int failures;

static void check(int condition, const char *what)
{
	if (!condition)
	{
		printf("stereo far probe: FAIL: %s\n", what);
		failures++;
	}
}

/* the horizontal angle (about z) from a point to another */
static double yaw(const real_point3d *from, const real_point3d *to)
{
	return atan2((double)to->y - from->y, (double)to->x - from->x);
}

int main(void)
{
	const char *ring = "scenery\\halo\\halo";
	real_matrix4x3 nodes[3];
	real_matrix4x3 saved[3];
	real_point3d center = { { -6.6f, 0.0f, 0.62f } };
	real_point3d eyes[2];
	const real_matrix4x3 *drawn[2];
	const real_matrix4x3 *first;
	real first_x;
	short eye;
	short node;

	check(halo_stereo_far_scenery(ring), "the ring's tag is far scenery");
	check(!halo_stereo_far_scenery("scenery\\halo_destroyed\\halo_destroyed"), "the destroyed ring's tag is not");
	check(!halo_stereo_far_scenery("cinematics\\scenery\\planets\\halo_closeup\\halo_closeup"),
		"the close-up's tag is not");
	check(!halo_stereo_far_scenery("scenery\\halo\\halo2"), "a longer name is not");
	check(!halo_stereo_far_scenery(NULL), "no name is not");

	/* x20_halo's place and two more nodes around it, 20 units out */
	memset(nodes, 0, sizeof(nodes));
	for (node = 0; node < 3; node++)
	{
		nodes[node].scale = 1.0f;
		nodes[node].forward.i = nodes[node].left.j = nodes[node].up.k = 1.0f;
		nodes[node].position.x = 35.0f;
		nodes[node].position.y = 4.0f + (node - 1) * 20.0f;
		nodes[node].position.z = 1.0f;
	}
	memcpy(saved, nodes, sizeof(nodes));

	probe_eye = NULL;
	check(halo_stereo_far_matrices(ring, nodes, 3, &center) == nodes, "outside an eye pass, the object's own");

	/* the eyes 0.0105 units to either side of the center camera, which looks along +x */
	eyes[0] = center;
	eyes[0].y += 0.0105f;
	eyes[1] = center;
	eyes[1].y -= 0.0105f;
	probe_eye = &eyes[0];
	check(halo_stereo_far_matrices("scenery\\crates\\crate", nodes, 3, &center) == nodes, "another tag: its own");
	check(halo_stereo_far_matrices(ring, nodes, MAXIMUM_NODES_PER_MODEL + 1, &center) == nodes,
		"too many nodes: its own");
	check(halo_stereo_far_matrices(ring, nodes, 0, &center) == nodes, "no nodes: its own");

	for (eye = 0; eye < 2; eye++)
	{
		probe_eye = &eyes[eye];
		drawn[eye] = halo_stereo_far_matrices(ring, nodes, 3, &center);
		check(drawn[eye] != nodes, "in an eye pass, a copy");
		for (node = 0; node < 3; node++)
		{
			double seen = yaw(&eyes[eye], &drawn[eye][node].position);
			double centers = yaw(&center, &nodes[node].position);

			check(fabs(seen - centers) < 1e-6, "each eye sees the node where the center camera does");
			check(fabs(drawn[eye][node].position.z - nodes[node].position.z) < 1e-6, "no vertical shift");
			check(!memcmp(&drawn[eye][node].forward, &nodes[node].forward, sizeof(real_vector3d) * 3) &&
				drawn[eye][node].scale == nodes[node].scale, "orientation and scale kept");
		}
	}
	{
		double before = yaw(&eyes[0], &nodes[1].position) - yaw(&eyes[1], &nodes[1].position);
		double after = yaw(&eyes[0], &drawn[0][1].position) - yaw(&eyes[1], &drawn[1][1].position);

		printf("stereo far probe: x20_halo's disparity %.2e rad untranslated, %.2e rad drawn\n", before, after);
		check(fabs(before) > 4e-4, "untranslated, the ring has disparity");
		check(fabs(after) < 1e-6, "drawn, it has none");
	}
	check(!memcmp(nodes, saved, sizeof(nodes)), "the object's own matrices never change");

	/* two far objects in one pass: the first's copy survives the second's */
	probe_eye = &eyes[0];
	first = halo_stereo_far_matrices(ring, nodes, 3, &center);
	first_x = first[0].position.x;
	nodes[0].position.x += 100.0f;
	check(halo_stereo_far_matrices(ring, nodes, 3, &center) != first, "a second far object: its own array");
	check(first[0].position.x == first_x, "the first's copy is unchanged");

	if (failures)
	{
		printf("stereo far probe: %d failures\n", failures);
		return 1;
	}
	printf("stereo far probe: PASS\n");
	return 0;
}
