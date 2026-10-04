/* The first-person body's render-only node matrices
(port/linux/game/first_person_body.c, included, not linked): on a synthetic
node list in the cyborg's shape, the head collapses to its parent's
position, each arm from the upper arm down collapses to the upper arm when
asked, everything else is left bit-identical, the input is never written,
nothing past node_count is read or written (the matrices are allocated
exactly, under the address sanitizer), and a model whose names aren't a
biped's comes back unchanged. And the predicate: only the local player's
unit, in the director's first person, in an eye layer of a stereo frame in
HEAD mode or the side-by-side view, never the film or SCREEN gameplay. */
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
static long probe_unit = 7;
static int probe_first_person = 1;
const struct halo_stereo_frame *halo_stereo_frame(void) { return &probe_frame; }
int halo_stereo_current_layer(void) { return probe_layer; }
int halo_stereo_film(void) { return probe_film; }
int halo_stereo_screen_gameplay(void) { return probe_screen_gameplay; }

#define FIRST_PERSON_BODY_PROBE
static int first_person_body_local_unit(long object_index)
{
	return probe_first_person && object_index == probe_unit;
}

#include "../../linux/game/first_person_body.c"

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

static int same_matrix(const real_matrix4x3 *a, const real_matrix4x3 *b)
{
	return !memcmp(a, b, sizeof(*a));
}

/* collapsed: scale 0 at the target's position, the rotation untouched */
static int collapsed_to(const real_matrix4x3 *result, const real_matrix4x3 *original, const real_matrix4x3 *target)
{
	return result->scale == 0.0f &&
		!memcmp(&result->position, &target->position, sizeof(result->position)) &&
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
	"bip01 r foot", "bip01 spine", "bip01 spine1", "bip01 neck", "Bip01 Head",
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

static int arm_upper(int node)
{
	if (node >= L_UPPERARM && node <= L_FINGER1)
		return L_UPPERARM;
	if (node >= R_UPPERARM && node <= R_FINGER1)
		return R_UPPERARM;
	return NONE;
}

static void check_cyborg(void)
{
	long model = add_model(0xC1B0u, cyborg_names, cyborg_parents, CYBORG_NODES);
	real_matrix4x3 *input = make_matrices(CYBORG_NODES);
	real_matrix4x3 saved[CYBORG_NODES];
	const real_matrix4x3 *result;
	int arms, node;

	memcpy(saved, input, sizeof(saved));
	for (arms = 1; arms >= 0; arms--) {
		char what[160];

		logs = 0;
		result = halo_first_person_body_matrices(model, input, CYBORG_NODES, arms);
		check(result != input, "the cyborg gets a copy");
		check(!memcmp(saved, input, sizeof(saved)), "the input is never written");
		for (node = 0; node < CYBORG_NODES; node++) {
			snprintf(what, sizeof(what), "%s, collapse_arms %d: node %s", "cyborg", arms, cyborg_names[node]);
			if (node == HEAD)
				check(collapsed_to(&result[node], &input[node], &input[NECK]),
					"the head collapses to its parent's (the neck's) position, scale 0");
			else if (arms && arm_upper(node) != NONE)
				check(collapsed_to(&result[node], &input[node], &input[arm_upper(node)]), what);
			else
				check(same_matrix(&result[node], &input[node]), what);
		}
	}
	/* a second call doesn't log the node names again */
	logs = 0;
	(void)halo_first_person_body_matrices(model, input, CYBORG_NODES, 1);
	check(logs == 0, "the node names log once per model");

	/* the object has fewer node matrices than the model: nothing past them is
	read or written (the allocation is exact, under the address sanitizer) */
	{
		real_matrix4x3 *fewer = make_matrices(R_CLAVICLE);

		result = halo_first_person_body_matrices(model, fewer, R_CLAVICLE, 1);
		check(collapsed_to(&result[L_HAND], &fewer[L_HAND], &fewer[L_UPPERARM]), "fewer nodes: the left hand collapses");
		check(same_matrix(&result[NECK], &fewer[NECK]), "fewer nodes: the neck stays");
		free(fewer);
	}
	free(input);
}

/* a chain with no neck: the head's parent is the spine */
static void check_no_neck(void)
{
	static const char *const names[] = {
		"bip01 pelvis", "bip01 l thigh", "bip01 spine1", "bip01 head", "bip01 l upperarm", "bip01 l forearm",
		"bip01 r upperarm", "bip01 r forearm",
	};
	static const short parents[] = { NONE, 0, 0, 2, 2, 4, 2, 6 };
	long model = add_model(0x5E1Fu, names, parents, 8);
	real_matrix4x3 *input = make_matrices(8);
	const real_matrix4x3 *result = halo_first_person_body_matrices(model, input, 8, 0);

	check(collapsed_to(&result[3], &input[3], &input[2]), "no neck: the head collapses to the spine");
	check(same_matrix(&result[4], &input[4]) && same_matrix(&result[5], &input[5]),
		"no neck, collapse_arms 0: the arms stay");
	check(same_matrix(&result[0], &input[0]) && same_matrix(&result[2], &input[2]),
		"no neck: the pelvis and spine stay");
	free(input);
}

/* a Warthog's names: no head, no arms, so the input comes back */
static void check_vehicle(void)
{
	static const char *const names[] = {
		"frame", "front axle", "left front tire", "right front tire", "rear axle", "gun mount",
	};
	static const short parents[] = { NONE, 0, 1, 1, 0, 0 };
	long model = add_model(0x3A47u, names, parents, 6);
	real_matrix4x3 *input = make_matrices(6);

	logs = 0;
	check(halo_first_person_body_matrices(model, input, 6, 1) == input, "a Warthog's nodes: the input comes back");
	check(logs >= 1 && strstr(last_log, "first_person_body") != NULL, "an unrecognized model is logged");
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
	check(halo_first_person_body(probe_unit), "back on");
}

int main(void)
{
	check_cyborg();
	check_no_neck();
	check_vehicle();
	check_predicate();
	if (failures) {
		fprintf(stderr, "first-person body probe: %d failures\n", failures);
		return 1;
	}
	printf("first-person body probe: PASS\n");
	return 0;
}
