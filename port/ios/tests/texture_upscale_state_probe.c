/* the per-level gate: the switch and a skip take effect at the next map load, never mid-level */
#include <assert.h>
#include <stdio.h>
#include "texture_upscale_state.h"

int main(void)
{
	struct texture_upscale_state s = { 0 };

	assert(!texture_upscale_state_enabled(&s));          /* nothing before the first map */
	texture_upscale_state_map_loaded(&s, 1);
	assert(texture_upscale_state_enabled(&s));
	texture_upscale_state_request_original(&s);         /* asked mid-level: this level is unchanged */
	assert(texture_upscale_state_enabled(&s));
	texture_upscale_state_map_loaded(&s, 1);             /* the next load plays original ... */
	assert(!texture_upscale_state_enabled(&s));
	texture_upscale_state_map_loaded(&s, 1);             /* ... and only that one */
	assert(texture_upscale_state_enabled(&s));
	texture_upscale_state_map_loaded(&s, 0);             /* the switch off */
	assert(!texture_upscale_state_enabled(&s));
	texture_upscale_state_map_loaded(&s, 1);
	assert(texture_upscale_state_enabled(&s));
	puts("texture_upscale_state_probe: ok");
	return 0;
}
