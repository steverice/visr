/* texture_cache.c: the recipe key and the spec's invalidation table ("Cache and versioning") */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "texture_cache.h"

static struct texture_recipe_globals globals(void)
{
	struct texture_recipe_globals g = { "S4g", "ab12", { 1, 1, 1, 1, 1, 1 }, 12, 30, 1, 3, 1, "rgba8" };
	return g;
}

static struct texture_cache_record record(const struct texture_recipe_globals *g, enum texture_policy_result result,
	enum texture_policy_treatment treatment)
{
	struct texture_recipe r = { 0x1234, 64, 64, result, treatment, 1, 0, 0, 2 };
	struct texture_cache_record out = { 0x1234, 64, 64, result, treatment, texture_recipe_key(g, &r),
		texture_recipe_global_key(g, treatment, result), 32768 };
	return out;
}

static enum texture_cache_action after(const struct texture_recipe_globals *old_globals,
	enum texture_policy_result old_result, const struct texture_recipe_globals *new_globals,
	enum texture_policy_result new_result, enum texture_policy_treatment treatment)
{
	struct texture_cache_record stored = record(old_globals, old_result, treatment);
	struct texture_cache_record desired = record(new_globals, new_result, treatment);

	return texture_cache_action(&stored, &desired, texture_recipe_global_key(new_globals, treatment, stored.result));
}

static void invalidation_table(void)
{
	struct texture_recipe_globals g = globals(), threshold = g, model = g, encoder = g, bumpfix = g;
	struct texture_cache_record desired = record(&g, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR);
	struct texture_cache_record original = record(&g, TEXTURE_POLICY_ORIGINAL, TEXTURE_POLICY_COLOR);

	threshold.low = 10;
	model.model_sha256 = "cd34";
	encoder.encoder = "astc-6x6";
	bumpfix.pipeline_version[TEXTURE_POLICY_BUMP_MAP] = 2;
	/* an app update with none of these: nothing */
	assert(after(&g, TEXTURE_POLICY_BPF, &g, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_KEEP);
	/* overrides or rules: only a texture whose result changed */
	assert(after(&g, TEXTURE_POLICY_S4G, &g, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_REPLACE_BACKGROUND);
	assert(texture_cache_action(&desired, &original, desired.global_key) == TEXTURE_CACHE_DELETE_NOW);
	assert(texture_cache_action(NULL, &original, 0) == TEXTURE_CACHE_KEEP);
	assert(texture_cache_action(NULL, &desired, 0) == TEXTURE_CACHE_MISSING);
	/* the threshold or the bp/bpf kernels: bpf entries only */
	assert(after(&g, TEXTURE_POLICY_BPF, &threshold, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_REDO_BLOCKING);
	assert(after(&g, TEXTURE_POLICY_S4G, &threshold, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_KEEP);
	/* the bump fix: bump entries only */
	assert(after(&g, TEXTURE_POLICY_BPF, &bumpfix, TEXTURE_POLICY_BPF, TEXTURE_POLICY_BUMP_MAP) == TEXTURE_CACHE_REDO_BLOCKING);
	assert(after(&g, TEXTURE_POLICY_BPF, &bumpfix, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_KEEP);
	/* the model or the encoder: every upscaled entry */
	assert(after(&g, TEXTURE_POLICY_S4G, &model, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_REDO_BLOCKING);
	assert(after(&g, TEXTURE_POLICY_BPF, &encoder, TEXTURE_POLICY_BPF, TEXTURE_POLICY_MULTIPURPOSE) == TEXTURE_CACHE_REDO_BLOCKING);
	/* a stale global recipe and a result change at once: never mix recipes */
	assert(after(&g, TEXTURE_POLICY_S4G, &model, TEXTURE_POLICY_BPF, TEXTURE_POLICY_COLOR) == TEXTURE_CACHE_REDO_BLOCKING);
}

static void keys(void)
{
	struct texture_recipe_globals g = globals();
	struct texture_recipe a = { 0x1234, 64, 64, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR, 1, 0, 0, 2 }, b = a;

	assert(texture_recipe_key(&g, &a) == texture_recipe_key(&g, &b));
	b.wrap = 0;
	assert(texture_recipe_key(&g, &a) != texture_recipe_key(&g, &b));   /* the address mode is in the key */
	b = a; b.scale = 4;
	assert(texture_recipe_key(&g, &a) != texture_recipe_key(&g, &b));
	b = a; b.alpha = 1;
	assert(texture_recipe_key(&g, &a) != texture_recipe_key(&g, &b));
	assert(texture_recipe_global_key(&g, TEXTURE_POLICY_COLOR, TEXTURE_POLICY_ORIGINAL) == 0);
}

static void manifest_round_trip(void)
{
	char directory[] = "/tmp/texture-cache-probe-XXXXXX";
	struct texture_recipe_globals g = globals();
	struct texture_cache_record records[2] = { record(&g, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR),
		record(&g, TEXTURE_POLICY_BPF, TEXTURE_POLICY_BUMP_MAP) }, *loaded;
	char path[256];

	assert(mkdtemp(directory));
	records[1].hash = 0xfedcba9876543210ull;
	assert(texture_cache_manifest_save(directory, records, 2) == 0);
	assert(texture_cache_manifest_load(directory, &loaded) == 2);
	assert(!memcmp(&loaded[0], &records[0], sizeof(records[0])) && !memcmp(&loaded[1], &records[1], sizeof(records[1])));
	free(loaded);
	snprintf(path, sizeof(path), "%s/manifest.tsv", directory);
	unlink(path);
	assert(texture_cache_manifest_load(directory, &loaded) == 0);       /* no manifest: an empty cache */
	free(loaded);
	rmdir(directory);
}

int main(void)
{
	keys();
	invalidation_table();
	manifest_round_trip();
	puts("texture_cache_probe: ok");
	return 0;
}
