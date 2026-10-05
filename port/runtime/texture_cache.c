/* The upscale cache's bookkeeping (texture_cache.h): the recipe key, the action for each texture after an app update,
the manifest. A cache entry holds only the new levels; the original levels are uploaded natively. */
#include "texture_cache.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "texture_override.h"

static const char *const result_names[] = { "s4g", "bpf", "original" };

static void manifest_path(const char *directory, char *path, size_t size)
{
	snprintf(path, size, "%s/manifest.tsv", directory);
}

uint64_t texture_recipe_global_key(const struct texture_recipe_globals *g, enum texture_policy_treatment treatment,
	enum texture_policy_result result)
{
	char text[512];
	int length;

	if (result == TEXTURE_POLICY_ORIGINAL)
		return 0;
	length = snprintf(text, sizeof(text), "model=%s;weights=%s;pipeline=%u;encoder=%s", g->model_id, g->model_sha256,
		g->pipeline_version[treatment], g->encoder);
	if (length < 0 || (size_t)length >= sizeof(text))
		length = (int)sizeof(text) - 1;
	if (result == TEXTURE_POLICY_BPF && (size_t)length < sizeof(text) - 1)
		length += snprintf(text + length, sizeof(text) - (size_t)length, ";low=%.3f;high=%.3f;sigma=%.3f;iterations=%u;post=%u",
			g->low, g->high, g->sigma, g->iterations, g->post_version);
	if ((size_t)length >= sizeof(text))
		length = (int)sizeof(text) - 1;
	return texture_override_hash((const unsigned char *)text, (size_t)length);
}

uint64_t texture_recipe_key(const struct texture_recipe_globals *g, const struct texture_recipe *r)
{
	char text[256];
	int length = snprintf(text, sizeof(text),
		"source=%016" PRIx64 ";size=%ux%u;result=%s;treatment=%d;address=%s;dilate=%d;alpha=%d;scale=%u;global=%016" PRIx64,
		r->source_hash, r->width, r->height, result_names[r->result], (int)r->treatment, r->wrap ? "wrap" : "clamp",
		r->dilates, r->alpha, r->scale, texture_recipe_global_key(g, r->treatment, r->result));

	if (length < 0 || (size_t)length >= sizeof(text))
		length = (int)sizeof(text) - 1;
	return texture_override_hash((const unsigned char *)text, (size_t)length);
}

enum texture_cache_action texture_cache_action(const struct texture_cache_record *stored,
	const struct texture_cache_record *desired, uint64_t stored_global_now)
{
	if (desired->result == TEXTURE_POLICY_ORIGINAL)
		return stored ? TEXTURE_CACHE_DELETE_NOW : TEXTURE_CACHE_KEEP;
	if (!stored)
		return TEXTURE_CACHE_MISSING;
	if (stored->global_key != stored_global_now)
		return TEXTURE_CACHE_REDO_BLOCKING;
	if (stored->recipe_key != desired->recipe_key)
		return TEXTURE_CACHE_REPLACE_BACKGROUND;
	return TEXTURE_CACHE_KEEP;
}

size_t texture_cache_manifest_load(const char *directory, struct texture_cache_record **records)
{
	char path[1024], line[256];
	FILE *file;
	size_t count = 0, capacity = 0;

	*records = NULL;
	manifest_path(directory, path, sizeof(path));
	if (!(file = fopen(path, "r")))
		return 0;
	while (fgets(line, sizeof(line), file))
	{
		struct texture_cache_record r;
		unsigned result, treatment;

		memset(&r, 0, sizeof(r));
		if (sscanf(line, "%" SCNx64 "\t%u\t%u\t%u\t%u\t%" SCNx64 "\t%" SCNx64 "\t%" SCNu64, &r.hash, &r.width,
			&r.height, &result, &treatment, &r.recipe_key, &r.global_key, &r.bytes) != 8 || result > 2 || treatment > 5)
			continue;   /* a damaged line: that texture is simply not cached */
		r.result = (enum texture_policy_result)result;
		r.treatment = (enum texture_policy_treatment)treatment;
		if (count == capacity)
		{
			struct texture_cache_record *grown = realloc(*records, (capacity = capacity ? capacity * 2 : 256) * sizeof(r));

			if (!grown)
				break;
			*records = grown;
		}
		(*records)[count++] = r;
	}
	fclose(file);
	return count;
}

int texture_cache_manifest_save(const char *directory, const struct texture_cache_record *records, size_t count)
{
	char path[1024], temporary[1040];
	FILE *file;
	size_t i;
	int failed = 0;

	manifest_path(directory, path, sizeof(path));
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if (!(file = fopen(temporary, "w")))
		return -1;
	for (i = 0; i < count && !failed; i++)
		failed = fprintf(file, "%016" PRIx64 "\t%u\t%u\t%u\t%u\t%016" PRIx64 "\t%016" PRIx64 "\t%" PRIu64 "\n",
			records[i].hash, records[i].width, records[i].height, (unsigned)records[i].result,
			(unsigned)records[i].treatment, records[i].recipe_key, records[i].global_key, records[i].bytes) < 0;
	failed |= fflush(file) != 0;
	failed |= fsync(fileno(file)) != 0;
	failed |= fclose(file) != 0;
	if (failed || rename(temporary, path))
	{
		unlink(temporary);
		return -1;
	}
	return 0;
}
