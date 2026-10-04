/* The first-person body's render-only node matrices
(port/linux/game/first_person_body.c, included, not linked): on a synthetic
node list in the cyborg's shape, the torso (the spine's subtree) collapses
to the spine node's own position, every node is set back along the facing
made horizontal, the shadow's copy is the whole silhouette set back the
same way in its own array, the input is never written, nothing past
node_count is read or written (the matrices are allocated exactly, under
the address sanitizer), a model with no spine collapses only its head, and
one whose names aren't a biped's comes back unchanged. The pose guard skips
a pelvis away from under the camera. And the predicate: only the local
player's unit, in the director's first person, in an eye layer of a stereo
frame in HEAD mode or the side-by-side view, never the film or SCREEN
gameplay, and only in a pose that takes a body, logging the first skip of
each stretch. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the game's types first_person_body.c reads, in the shape it reads them */
typedef float real;
typedef unsigned char boolean;
typedef struct { real i, j, k; } real_vector3d;
typedef struct { real x, y, z; } real_point3d;
struct real_matrix4x3
{
	real scale;
	real_vector3d forward, left, up;
	real_point3d position;
};
typedef struct real_matrix4x3 real_matrix4x3;
struct tag_block { long count; void *address; };
struct model_node
{
	char name[32];
	short next_sibling_node_index, first_child_node_index, parent_node_index;
};
struct model
{
	unsigned long node_list_checksum;
	struct tag_block nodes;
};
#define NONE -1
#define MAXIMUM_NODES_PER_MODEL 64
#define TAG_BLOCK_GET_ELEMENT(block, index, type) \
	((index) >= 0 && (index) < (block)->count ? &((type *)(block)->address)[(index)] : (type *)abort_index())
static void *abort_index(void) { fprintf(stderr, "FAIL: a node index out of the block\n"); exit(1); }

static struct model *probe_models[8];
#define model_definition_get(index) (probe_models[(index)])

/* first_person_body.c's imports */
#define PROBE_OFFSET 0.08f
static int logs, setting_on = 1, gpu_stats = 1;
static char last_log[512];
int config_boolean(const char *name)
{
	if (!strcmp(name, "display.first_person_body"))
		return setting_on;
	if (!strcmp(name, "debug.gpu_stats"))
		return gpu_stats;
	return 0;
}
double config_real(const char *name)
{
	return !strcmp(name, "display.first_person_body_offset") ? PROBE_OFFSET : 0.0;
}
void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(last_log, sizeof(last_log), format, arguments);
	va_end(arguments);
	logs++;
}

#include "halo_stereo.h"

static struct halo_stereo_frame probe_frame;
static int probe_layer, probe_film, probe_screen_gameplay;
const struct halo_stereo_frame *halo_stereo_frame(void) { return &probe_frame; }
int halo_stereo_current_layer(void) { return probe_layer; }
int halo_stereo_film(void) { return probe_film; }
int halo_stereo_screen_gameplay(void) { return probe_screen_gameplay; }

#define FIRST_PERSON_BODY_PROBE
#include "../../linux/game/first_person_body.c"

static long probe_unit = 7;
static int probe_first_person = 1;
static enum body_skip probe_pose = BODY_DRAWS;
static int first_person_body_local_unit(long object_index)
{
	return probe_first_person && object_index == probe_unit;
}
static enum body_skip first_person_body_pose(long object_index)
{
	(void)object_index;
	return probe_pose;
}

static int failures;

static void check(int condition, const char *what)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

/* a node list from names and parent indices; the next model index */
static long add_model(unsigned long checksum, const char *const *names, const short *parents, short count)
{
	static long next_index;
	struct model *model = calloc(1, sizeof(*model));
	struct model_node *nodes = calloc((size_t)count, sizeof(*nodes));
	short i;

	for (i = 0; i < count; i++) {
		snprintf(nodes[i].name, sizeof(nodes[i].name), "%s", names[i]);
		nodes[i].parent_node_index = parents[i];
		nodes[i].first_child_node_index = nodes[i].next_sibling_node_index = NONE;
	}
	/* the child links, for completeness (first_person_body.c reads parents) */
	for (i = count - 1; i > 0; i--) {
		struct model_node *parent = &nodes[parents[i]];

		nodes[i].next_sibling_node_index = parent->first_child_node_index;
		parent->first_child_node_index = i;
	}
	model->node_list_checksum = checksum;
	model->nodes.count = count;
	model->nodes.address = nodes;
	probe_models[next_index] = model;
	return next_index++;
}

/* world matrices with distinct positions, scale 1, in an allocation of
exactly count matrices */
static real_matrix4x3 *make_matrices(short count)
{
	real_matrix4x3 *matrices = malloc((size_t)count * sizeof(*matrices));
	short i;

	for (i = 0; i < count; i++) {
		matrices[i].scale = 1.0f;
		matrices[i].forward = (real_vector3d){ 1.0f, 0.0f, 0.0f };
		matrices[i].left = (real_vector3d){ 0.0f, 1.0f, 0.0f };
		matrices[i].up = (real_vector3d){ 0.0f, 0.0f, 1.0f };
		matrices[i].position = (real_point3d){ 0.1f * i, 1.0f + 0.2f * i, 2.0f + 0.3f * i };
	}
	return matrices;
}

static int near(real a, real b)
{
	return a - b < 1e-5f && b - a < 1e-5f;
}

/* the result is original's rotation at target's position set back by
back, with the given scale */
static int placed(const real_matrix4x3 *result, const real_matrix4x3 *original, const real_matrix4x3 *target,
	const float back[2], real scale)
{
	return result->scale == scale &&
		near(result->position.x, target->position.x + back[0]) &&
		near(result->position.y, target->position.y + back[1]) &&
		near(result->position.z, target->position.z) &&
		!memcmp(&result->forward, &original->forward, sizeof(result->forward)) &&
		!memcmp(&result->left, &original->left, sizeof(result->left)) &&
		!memcmp(&result->up, &original->up, sizeof(result->up));
}

/* the cyborg's shape, in 3ds Max's biped naming */
enum
{
	PELVIS, L_THIGH, L_CALF, L_FOOT, R_THIGH, R_CALF, R_FOOT, SPINE, SPINE1, NECK, HEAD,
	L_CLAVICLE, L_UPPERARM, L_FOREARM, L_HAND, L_FINGER0, L_FINGER1,
	R_CLAVICLE, R_UPPERARM, R_FOREARM, R_HAND, R_FINGER0, R_FINGER1,
	CYBORG_NODES
};
static const char *const cyborg_names[CYBORG_NODES] = {
	"bip01 pelvis", "bip01 l thigh", "bip01 l calf", "bip01 l foot", "bip01 r thigh", "bip01 r calf",
	"bip01 r foot", "Bip01 Spine", "bip01 spine1", "bip01 neck", "Bip01 Head",
	"bip01 l clavicle", "bip01 l upperarm", "bip01 l forearm", "bip01 l hand", "bip01 l finger0",
	"bip01 l finger1",
	"bip01 r clavicle", "bip01 r UpperArm", "bip01 r forearm", "bip01 r hand", "bip01 r finger0",
	"bip01 r finger1",
};
static const short cyborg_parents[CYBORG_NODES] = {
	NONE, PELVIS, L_THIGH, L_CALF, PELVIS, R_THIGH, R_CALF, PELVIS, SPINE, SPINE1, NECK,
	NECK, L_CLAVICLE, L_UPPERARM, L_FOREARM, L_HAND, L_HAND,
	NECK, R_CLAVICLE, R_UPPERARM, R_FOREARM, R_HAND, R_HAND,
};

static void check_cyborg(void)
{
	/* a facing with a vertical part: the set-back is horizontal, 0.08 long */
	static const float facing[3] = { 0.6f, 0.8f, 0.5f };
	const float back[2] = { -PROBE_OFFSET * 0.6f, -PROBE_OFFSET * 0.8f };
	long model = add_model(0xC1B0u, cyborg_names, cyborg_parents, CYBORG_NODES);
	real_matrix4x3 *input = make_matrices(CYBORG_NODES);
	real_matrix4x3 saved[CYBORG_NODES];
	const real_matrix4x3 *body, *shadow;
	int node;

	/* the torso's cap: the spine node raised along the pelvis's up (+z here) */
	real_matrix4x3 cap;

	memcpy(saved, input, sizeof(saved));
	cap = input[SPINE];
	cap.position.z += BODY_WAIST_RAISE;
	logs = 0;
	body = halo_first_person_body_matrices(model, input, CYBORG_NODES, facing, 0);
	check(logs > 0 && strstr(last_log, "first_person_body") != NULL, "the node names log on the first lookup");
	check(body != input, "the cyborg gets a copy");
	for (node = 0; node < CYBORG_NODES; node++) {
		char what[160];

		snprintf(what, sizeof(what), "the body: node %s", cyborg_names[node]);
		if (node >= SPINE)
			check(placed(&body[node], &input[node], &cap, back, 0.0f), what);
		else
			check(placed(&body[node], &input[node], &input[node], back, 1.0f), what);
	}
	shadow = halo_first_person_body_matrices(model, input, CYBORG_NODES, facing, 1);
	check(shadow != body && shadow != input, "the shadow has its own copy");
	for (node = 0; node < CYBORG_NODES; node++) {
		char what[160];

		snprintf(what, sizeof(what), "the shadow: node %s, whole, set back", cyborg_names[node]);
		check(placed(&shadow[node], &input[node], &input[node], back, 1.0f), what);
	}
	check(placed(&body[HEAD], &input[HEAD], &cap, back, 0.0f), "the shadow's call leaves the body's copy");
	check(!memcmp(saved, input, sizeof(saved)), "the input is never written");

	/* a second call doesn't log the node names again */
	logs = 0;
	(void)halo_first_person_body_matrices(model, input, CYBORG_NODES, facing, 0);
	check(logs == 0, "the node names log once per model");

	/* a vertical facing: no set-back */
	{
		static const float up[3] = { 0.0f, 0.0f, 1.0f };
		const float none[2] = { 0.0f, 0.0f };

		body = halo_first_person_body_matrices(model, input, CYBORG_NODES, up, 0);
		check(placed(&body[L_FOOT], &input[L_FOOT], &input[L_FOOT], none, 1.0f), "a vertical facing: no set-back");
	}

	/* the object has fewer node matrices than the model: nothing past them is
	read or written (the allocation is exact, under the address sanitizer) */
	{
		real_matrix4x3 *fewer = make_matrices(R_CLAVICLE);
		real_matrix4x3 fewer_cap = fewer[SPINE];

		fewer_cap.position.z += BODY_WAIST_RAISE;
		body = halo_first_person_body_matrices(model, fewer, R_CLAVICLE, facing, 0);
		check(placed(&body[L_HAND], &fewer[L_HAND], &fewer_cap, back, 0.0f), "fewer nodes: the left hand collapses");
		check(placed(&body[R_FOOT], &fewer[R_FOOT], &fewer[R_FOOT], back, 1.0f), "fewer nodes: the right foot stays");
		shadow = halo_first_person_body_matrices(model, fewer, R_CLAVICLE, facing, 1);
		check(placed(&shadow[L_HAND], &fewer[L_HAND], &fewer[L_HAND], back, 1.0f), "fewer nodes: the shadow's hand");
		free(fewer);
	}

	/* the pose guard: the pelvis within 0.15 of the camera horizontally,
	at any height */
	{
		float camera[3] = { input[PELVIS].position.x + 0.1f, input[PELVIS].position.y + 0.1f,
			input[PELVIS].position.z + 0.6f };

		check(body_pose_skip(model, input, CYBORG_NODES, camera) == BODY_DRAWS, "the pelvis 0.14 away: draws");
		camera[1] += 0.02f;
		check(body_pose_skip(model, input, CYBORG_NODES, camera) == BODY_SKIP_PELVIS_FAR,
			"the pelvis 0.156 away: skipped");
		camera[0] = input[PELVIS].position.x;
		camera[1] = input[PELVIS].position.y;
		camera[2] = input[PELVIS].position.z + 5.0f;
		check(body_pose_skip(model, input, CYBORG_NODES, camera) == BODY_DRAWS, "the height doesn't count");
	}
	free(input);
}

/* a chain with no node named spine and no neck (the head's parent is
spine1): only the head collapses, to its parent */
static void check_no_spine(void)
{
	static const char *const names[] = {
		"bip01 pelvis", "bip01 l thigh", "bip01 spine1", "bip01 head", "bip01 l upperarm", "bip01 l forearm",
		"bip01 r upperarm", "bip01 r forearm",
	};
	static const short parents[] = { NONE, 0, 0, 2, 2, 4, 2, 6 };
	static const float facing[3] = { 1.0f, 0.0f, 0.0f };
	const float back[2] = { -PROBE_OFFSET, 0.0f };
	long model = add_model(0x5E1Fu, names, parents, 8);
	real_matrix4x3 *input = make_matrices(8);
	const real_matrix4x3 *result = halo_first_person_body_matrices(model, input, 8, facing, 0);
	short node;

	check(placed(&result[3], &input[3], &input[2], back, 0.0f), "no spine: the head collapses to its parent");
	for (node = 0; node < 8; node++)
		if (node != 3)
			check(placed(&result[node], &input[node], &input[node], back, 1.0f),
				"no spine: everything else stays, set back");
	free(input);
}

/* a Warthog's names: no spine, no head, so the input comes back */
static void check_vehicle(void)
{
	static const char *const names[] = {
		"frame", "front axle", "left front tire", "right front tire", "rear axle", "gun mount",
	};
	static const short parents[] = { NONE, 0, 1, 1, 0, 0 };
	static const float facing[3] = { 1.0f, 0.0f, 0.0f };
	static const float camera[3] = { 0.0f, 0.0f, 0.0f };
	long model = add_model(0x3A47u, names, parents, 6);
	real_matrix4x3 *input = make_matrices(6);

	logs = 0;
	check(halo_first_person_body_matrices(model, input, 6, facing, 0) == input, "a Warthog's nodes: the input comes back");
	check(logs >= 1 && strstr(last_log, "no spine or head") != NULL, "an unrecognized model is logged");
	check(body_pose_skip(model, input, 6, camera) == BODY_SKIP_MODEL, "an unrecognized model: no body");
	free(input);
}

static void check_predicate(void)
{
	static const int modes[] = { HALO_STEREO_OFF, HALO_STEREO_HEAD, HALO_STEREO_SCREEN, HALO_STEREO_SIDE_BY_SIDE };
	int i;

	probe_frame.eye_count = 2;
	probe_layer = 0;
	for (i = 0; i < 4; i++) {
		probe_frame.mode = modes[i];
		check(!!halo_first_person_body(probe_unit) ==
			(modes[i] == HALO_STEREO_HEAD || modes[i] == HALO_STEREO_SIDE_BY_SIDE), "the body's modes");
	}
	probe_frame.mode = HALO_STEREO_HEAD;
	check(halo_first_person_body(probe_unit) && !halo_first_person_body(probe_unit + 1), "only the player's unit");
	probe_layer = 1;
	check(halo_first_person_body(probe_unit), "the right eye");
	probe_layer = HALO_STEREO_LAYER_HUD;
	check(!halo_first_person_body(probe_unit), "not the HUD layer");
	probe_layer = HALO_STEREO_LAYER_MONO;
	check(!halo_first_person_body(probe_unit), "not a mono layer");
	probe_layer = 0;
	probe_frame.eye_count = 0;
	check(!halo_first_person_body(probe_unit), "not without eyes");
	probe_frame.eye_count = 2;
	probe_film = 1;
	check(!halo_first_person_body(probe_unit), "not the film");
	probe_film = 0;
	probe_screen_gameplay = 1;
	check(!halo_first_person_body(probe_unit), "not SCREEN gameplay");
	probe_screen_gameplay = 0;
	probe_first_person = 0;
	check(!halo_first_person_body(probe_unit), "not out of the director's first person");
	probe_first_person = 1;

	/* a pose that takes no body: skipped, logged once for the stretch */
	logs = 0;
	probe_pose = BODY_SKIP_CUSTOM_ANIMATION;
	check(!halo_first_person_body(probe_unit) && !halo_first_person_body(probe_unit), "not in a custom animation");
	check(logs == 1 && strstr(last_log, "custom animation") != NULL, "the skip logs once");
	probe_pose = BODY_SKIP_SEATED;
	check(!halo_first_person_body(probe_unit), "not in a seat");
	check(logs == 2 && strstr(last_log, "seat") != NULL, "a new reason logs again");
	probe_pose = BODY_DRAWS;
	check(halo_first_person_body(probe_unit), "back on");
	probe_pose = BODY_SKIP_SEATED;
	check(!halo_first_person_body(probe_unit) && logs == 3, "a new stretch logs again");
	probe_pose = BODY_DRAWS;
	check(halo_first_person_body(probe_unit), "back on again");
}

int main(void)
{
	check_cyborg();
	check_no_spine();
	check_vehicle();
	check_predicate();
	if (failures) {
		fprintf(stderr, "first-person body probe: %d failures\n", failures);
		return 1;
	}
	printf("first-person body probe: PASS\n");
	return 0;
}
