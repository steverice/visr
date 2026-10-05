/* metal_state_cache.h: which encoder calls a draw makes once the Metal
backend skips the ones that set what the encoder already holds. A model of
gpu_metal_draw's calls (draw_calls) counts the calls a draw issues. */
#include "metal_state_cache.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* what one draw sets: stand-ins for the objects (their addresses) and the
values gpu_metal_draw passes to the encoder */
struct model_draw
{
	const void *pipeline, *depth_stencil, *textures[4], *samplers[4], *constants, *attributes[16];
	uint64_t constants_offset;
	uint32_t stencil_reference;
	double viewport[6];
	uint64_t scissor[4];
	uint32_t cull_mode, winding;
	float depth_bias[3], blend_color[4];
};

static int objects[64];

static struct model_draw base_draw(void)
{
	struct model_draw draw = {
		.pipeline = &objects[0], .depth_stencil = &objects[1], .constants = &objects[2], .constants_offset = 256,
		.stencil_reference = 0, .viewport = { 0, 0, 2732, 2048, 0, 1 }, .scissor = { 0, 0, 2732, 2048 },
		.cull_mode = 2, .winding = 0, .depth_bias = { 0, 0, 0 }, .blend_color = { 0, 0, 0, 1 },
	};
	int index;

	for (index = 0; index < 4; index++)
	{
		draw.textures[index] = &objects[4 + index];
		draw.samplers[index] = &objects[8 + index];
	}
	for (index = 0; index < 16; index++)
		draw.attributes[index] = index < 3 ? (const void *)&objects[16 + index] : (const void *)&objects[63];
	return draw;
}

/* gpu_metal_draw's encoder calls, in its order; returns the calls issued */
static unsigned long draw_calls(struct metal_state_cache *cache, const struct model_draw *draw)
{
	unsigned long before = cache->issued;
	int index;

	metal_state_object(cache, METAL_STATE_PIPELINE, draw->pipeline, 0);
	metal_state_object(cache, METAL_STATE_DEPTH_STENCIL, draw->depth_stencil, 0);
	metal_state_value(cache, METAL_STATE_STENCIL_REFERENCE, &draw->stencil_reference, sizeof(draw->stencil_reference));
	metal_state_value(cache, METAL_STATE_VIEWPORT, draw->viewport, sizeof(draw->viewport));
	metal_state_value(cache, METAL_STATE_SCISSOR, draw->scissor, sizeof(draw->scissor));
	metal_state_value(cache, METAL_STATE_CULL_MODE, &draw->cull_mode, sizeof(draw->cull_mode));
	metal_state_value(cache, METAL_STATE_WINDING, &draw->winding, sizeof(draw->winding));
	metal_state_value(cache, METAL_STATE_DEPTH_BIAS, draw->depth_bias, sizeof(draw->depth_bias));
	metal_state_value(cache, METAL_STATE_BLEND_COLOR, draw->blend_color, sizeof(draw->blend_color));
	for (index = 0; index < 4; index++)
	{
		metal_state_object(cache, METAL_STATE_TEXTURE + index, draw->textures[index], 0);
		metal_state_object(cache, METAL_STATE_SAMPLER + index, draw->samplers[index], 0);
	}
	metal_state_object(cache, METAL_STATE_VERTEX_BUFFER, draw->constants, draw->constants_offset);
	metal_state_object(cache, METAL_STATE_VERTEX_BUFFER + 1, draw->constants, draw->constants_offset + 3072);
	metal_state_object(cache, METAL_STATE_FRAGMENT_BUFFER, draw->constants, draw->constants_offset + 3072);
	for (index = 0; index < 16; index++)
		metal_state_object(cache, METAL_STATE_ATTRIBUTE_BUFFER + index, draw->attributes[index], 0);
	metal_state_always(cache); /* the attribute table (setVertexBytes) */
	metal_state_always(cache); /* the draw */
	metal_state_draw(cache);
	return cache->issued - before;
}

int main(void)
{
	struct metal_state_cache cache;
	struct model_draw draw = base_draw(), changed;

	assert(METAL_STATE_SLOTS <= 64);

	/* a new encoder holds nothing: every call goes through */
	metal_state_initialize(&cache, 1);
	metal_state_reset(&cache);
	assert(draw_calls(&cache, &draw) == METAL_STATE_DRAW_CALLS);

	/* the same draw again: only the attribute table and the draw */
	assert(draw_calls(&cache, &draw) == 2);
	assert(cache.skipped == METAL_STATE_DRAW_CALLS - 2);

	/* a new texture in stage 0, a renamed stream buffer and a moved
	constant snapshot (the same buffer at another offset) */
	changed = draw;
	changed.textures[0] = &objects[30];
	changed.attributes[1] = &objects[31];
	changed.constants_offset = 512;
	assert(draw_calls(&cache, &changed) == 2 + 1 + 1 + 3);

	/* values compare by value: another viewport and cull mode */
	changed.viewport[2] = 1366;
	changed.cull_mode = 0;
	assert(draw_calls(&cache, &changed) == 2 + 2);

	/* a new encoder (pass_begin) and an ended one (pass_finish) forget it all */
	metal_state_reset(&cache);
	assert(draw_calls(&cache, &changed) == METAL_STATE_DRAW_CALLS);

	/* the clear quad sets the pipeline, depth-stencil state, stencil
	reference, cull mode, depth bias, viewport, scissor, and bytes at vertex
	buffer 0 and fragment buffer 0 (gpu_metal_clear) */
	metal_state_clear_quad(&cache);
	assert(draw_calls(&cache, &changed) == 2 + 9);

	/* a pipeline change alone doesn't touch the bindings: Metal keeps them */
	changed.pipeline = &objects[32];
	assert(draw_calls(&cache, &changed) == 2 + 1);

	/* off (debug.metal_state_cache = false): every call goes through */
	metal_state_initialize(&cache, 0);
	metal_state_reset(&cache);
	assert(draw_calls(&cache, &draw) == METAL_STATE_DRAW_CALLS);
	assert(draw_calls(&cache, &draw) == METAL_STATE_DRAW_CALLS);
	assert(cache.skipped == 0 && cache.draws == 2);

	/* the per-report counts start again */
	metal_state_take_counts(&cache);
	assert(cache.issued == 0 && cache.skipped == 0 && cache.draws == 0);

	printf("PASS: Metal state cache: %d encoder calls a draw, 2 for a repeated draw\n", METAL_STATE_DRAW_CALLS);
	return 0;
}
