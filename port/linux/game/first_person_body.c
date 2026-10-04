/*
FIRST_PERSON_BODY.C

The Master Chief's body below the view in head-tracked stereo
(port/linux/src/halo_stereo.h; the stereo spec's "The first-person body in
head-tracked stereo"). The game hides the player's own unit in first person
(render_objects.c's object_is_first_person_camera); looking down in HEAD
mode then shows no feet. With display.first_person_body, the eye passes
draw that unit anyway, with a render-only copy of its node matrices:

- the head collapsed to its parent's position (the neck in the usual
  naming, a spine node if the chain has none), scale 0, so nothing sits
  inside the view;
- while the first-person weapon shows, the torso from the spine up (the
  spine, the chest, the neck and head, the shoulders and arms) collapsed to
  the spine's parent's position, the pelvis, scale 0, so the legs and
  pelvis are left: the stereo spec's fallback. The first-person arms carry
  the aiming, so the third-person arms would only hold a second weapon below
  the first; and the torso bends forward under the camera as the look
  pitches down (the aiming pose), so looking down put the eyes inside the
  chest (Task 7f's Mac captures: a30 looking down showed the chest plate
  and the backpack torn by the near plane, not the feet; a10's cryo pod,
  where the Chief leans back, put the chest across the lower half of a level
  view). The waist then ends in a cone where vertices are weighted across
  the pelvis and the spine. A model with no node named spine collapses only
  each arm from the upper arm down through the fingers, to the upper arm's
  position. In a first-person seat or zoomed, no first-person weapon draws,
  and the torso and arms stay, with only the head collapsed.

Scale 0 is enough: render_model multiplies each matrix by the node's
runtime_default_inverse_matrix, which keeps the position and the zero
scale, and the GPU's constants fold the scale into the 3x3.

The node matrices themselves are never changed, because markers,
attachments and collision read them; the copy is handed only to
render_model. It lives in one static array, never a stack array and never
reused for the shadow call (which takes the real matrices): transparent
geometry keeps the node matrices' pointer until the eye's transparent pass
(rasterizer_xbox_transparent_geometry.c), and the body draws once per eye
pass, on one thread.

The nodes are found by name, case-insensitively on the name's last word
("bip01 head", "bip01 l upperarm"), and the subtrees through the parent
links. The lookup is cached by the model's node_list_checksum (a tag index
is only valid for one map), and on the first lookup of each model its node
names are logged under debug.gpu_stats, to confirm the cyborg's on the
device.
*/

#include <stdio.h>
#include <string.h>

#ifndef FIRST_PERSON_BODY_PROBE
#include "cseries.h"
#include "math/real_math.h"
#include "tag_files/tag_groups.h"
#include "models/model_definitions.h"
#include "camera/director.h"
#include "game/players.h"
#include "render/render.h"
#include "../src/halo_stereo.h"

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
/* port/linux/src/sdl_platform.c (the host's on iOS) */
void platform_log(const char *format, ...);
#endif

/* models cached at once: in practice the player's one biped, and another
after a map with another (the Elite's or a marine's in a scripted swap) */
#define BODY_CACHE_SIZE 8

struct body_model
{
	unsigned long checksum;
	long node_count;              /* 0: an empty slot */
	int recognized;               /* a head and at least one upper arm */
	/* per node: the node whose position it collapses to, or NONE */
	signed char head_target[MAXIMUM_NODES_PER_MODEL];
	signed char arm_target[MAXIMUM_NODES_PER_MODEL];
	signed char torso_target[MAXIMUM_NODES_PER_MODEL];
};

static struct body_model body_cache[BODY_CACHE_SIZE];
static int body_cache_next;
static int body_setting = -1; /* display.first_person_body, read once */
static real_matrix4x3 body_matrices[MAXIMUM_NODES_PER_MODEL];

/* the name's last word ("bip01 l upperarm": "upperarm") is word (lower
case), ignoring the name's case */
static int node_name_is(const char *name, const char *word)
{
	const char *last = strrchr(name, ' ');

	for (name = last ? last + 1 : name; *name && *word; name++, word++) {
		char c = *name >= 'A' && *name <= 'Z' ? (char)(*name - 'A' + 'a') : *name;

		if (c != *word)
			return 0;
	}
	return !*name && !*word;
}

static void log_node_names(const struct model *model, long model_index)
{
	char line[256];
	size_t length = 0;
	short node_index;

	platform_log("first_person_body: model %ld (node list checksum 0x%08lx), %d nodes:", model_index,
		(unsigned long)model->node_list_checksum, (int)model->nodes.count);
	line[0] = '\0';
	for (node_index = 0; node_index < model->nodes.count; node_index++) {
		const struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node);
		char entry[64];
		size_t entry_length;

		snprintf(entry, sizeof(entry), "%s%d %s (parent %d)", length ? ", " : "", (int)node_index, node->name,
			(int)node->parent_node_index);
		entry_length = strlen(entry);
		if (length + entry_length >= sizeof(line)) {
			platform_log("first_person_body:   %s", line);
			snprintf(entry, sizeof(entry), "%d %s (parent %d)", (int)node_index, node->name,
				(int)node->parent_node_index);
			entry_length = strlen(entry);
			length = 0;
		}
		memcpy(line + length, entry, entry_length + 1);
		length += entry_length;
	}
	if (length)
		platform_log("first_person_body:   %s", line);
}

/* the model's collapse targets, from its node names and parent links */
static const struct body_model *body_model_get(long model_index)
{
	const struct model *model = model_definition_get(model_index);
	struct body_model *body;
	short count = (short)model->nodes.count;
	short node_index;
	int heads = 0, arms = 0;
	int slot;

	for (slot = 0; slot < BODY_CACHE_SIZE; slot++) {
		body = &body_cache[slot];
		if (body->node_count == count && count > 0 && body->checksum == model->node_list_checksum)
			return body;
	}
	body = &body_cache[body_cache_next];
	body_cache_next = (body_cache_next + 1) % BODY_CACHE_SIZE;
	memset(body, 0, sizeof(*body));
	body->checksum = model->node_list_checksum;
	body->node_count = count;
	if (count <= 0 || count > MAXIMUM_NODES_PER_MODEL) {
		platform_log("first_person_body: model %ld has %d nodes (at most %d); the body draws whole",
			model_index, (int)count, MAXIMUM_NODES_PER_MODEL);
		return body;
	}

	if (config_boolean("debug.gpu_stats"))
		log_node_names(model, model_index);
	for (node_index = 0; node_index < count; node_index++) {
		short ancestor = node_index;
		short steps;

		body->head_target[node_index] = body->arm_target[node_index] = body->torso_target[node_index] = NONE;
		/* the nearest head or upper arm up the chain, the node itself
		included: everything below it collapses with it */
		for (steps = 0; steps < count && ancestor >= 0 && ancestor < count; steps++) {
			const struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, ancestor, struct model_node);

			if (node_name_is(node->name, "head")) {
				short parent = node->parent_node_index;

				/* to the head's parent's position; a head with no parent
				collapses where it is */
				body->head_target[node_index] = (signed char)(parent >= 0 && parent < count ? parent : ancestor);
				if (ancestor == node_index)
					heads++;
				break;
			}
			if (node_name_is(node->name, "upperarm")) {
				body->arm_target[node_index] = (signed char)ancestor;
				if (ancestor == node_index)
					arms++;
				break;
			}
			ancestor = node->parent_node_index;
		}
		/* the spine up the chain, the node itself included: the torso
		collapses to the spine's parent */
		for (ancestor = node_index, steps = 0; steps < count && ancestor >= 0 && ancestor < count; steps++) {
			const struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, ancestor, struct model_node);

			if (node_name_is(node->name, "spine")) {
				short parent = node->parent_node_index;

				body->torso_target[node_index] = (signed char)(parent >= 0 && parent < count ? parent : ancestor);
				break;
			}
			ancestor = node->parent_node_index;
		}
	}
	body->recognized = heads > 0 && arms > 0;
	if (!body->recognized)
		platform_log("first_person_body: model %ld (node list checksum 0x%08lx) has %d head and %d upper arm "
			"nodes by name; the body draws whole", model_index, (unsigned long)model->node_list_checksum,
			heads, arms);
	return body;
}

const real_matrix4x3 *halo_first_person_body_matrices(long model_index, const real_matrix4x3 *matrices,
	short node_count, int collapse_arms)
{
	const struct body_model *body;
	short node_index;

	if (model_index == NONE || !matrices || node_count <= 0)
		return matrices;
	body = body_model_get(model_index);
	if (!body->recognized)
		return matrices;
	if (node_count > body->node_count)
		node_count = (short)body->node_count;

	memcpy(body_matrices, matrices, (size_t)node_count * sizeof(*matrices));
	for (node_index = 0; node_index < node_count; node_index++) {
		short target = collapse_arms ? body->torso_target[node_index] : NONE;

		if (target == NONE)
			target = body->head_target[node_index];
		if (target == NONE && collapse_arms)
			target = body->arm_target[node_index];
		if (target != NONE && target < node_count) {
			body_matrices[node_index].scale = 0.0f;
			body_matrices[node_index].position = matrices[target].position;
		}
	}
	return body_matrices;
}

#ifndef FIRST_PERSON_BODY_PROBE
/* the local player's unit, in the director's first person (not a scripted
camera's first-person object, which render_objects.c also hides) */
static int first_person_body_local_unit(long object_index)
{
	long player_index;

	if (render.local_player_index == NONE)
		return 0;
	player_index = local_player_get_player_index(render.local_player_index);
	if (player_index == NONE)
		return 0;
	return player_get(player_index)->unit_index == object_index &&
		director_get_perspective(render.local_player_index) == _director_perspective_first_person;
}
#endif

int halo_first_person_body(long object_index)
{
	const struct halo_stereo_frame *frame = halo_stereo_frame();
	int layer;

	if (object_index == NONE || frame->eye_count != 2 ||
		(frame->mode != HALO_STEREO_HEAD && frame->mode != HALO_STEREO_SIDE_BY_SIDE))
		return 0;
	layer = halo_stereo_current_layer();
	if (layer != 0 && layer != 1)
		return 0;
	/* the film (HEAD mode's is SCREEN already; the side-by-side view's
	isn't) and the side-by-side view's SCREEN gameplay draw as before */
	if (halo_stereo_film() || halo_stereo_screen_gameplay())
		return 0;
	if (!first_person_body_local_unit(object_index))
		return 0;
	if (body_setting < 0)
		body_setting = config_boolean("display.first_person_body") != 0;
	return body_setting;
}
