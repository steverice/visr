/* The upscale cache's bookkeeping (texture_cache.h): the recipe key, the action for each texture after an app update,
the manifest, the storage rules and entry writes. A cache entry holds only the new levels; the original levels are uploaded natively. */
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

int texture_cache_manifest_load(const char *directory, struct texture_cache_record **records, size_t *count_out)
{
	char path[1024], line[256];
	FILE *file;
	size_t count = 0, capacity = 0;
	int failed = 0;

	*records = NULL;
	*count_out = 0;
	manifest_path(directory, path, sizeof(path));
	if (!(file = fopen(path, "r")))
		return errno == ENOENT ? 0 : -1;   /* no manifest: an empty cache; one that cannot be read is an error */
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
			{
				failed = 1;
				break;
			}
			*records = grown;
		}
		(*records)[count++] = r;
	}
	failed |= ferror(file) != 0;
	fclose(file);
	if (failed)
	{
		free(*records);
		*records = NULL;
		return -1;
	}
	*count_out = count;
	return 0;
}

/* writes <path> through <path>.tmp: fill, flush, fsync, close, rename; on any failure the .tmp is removed and the old
<path>, if any, is untouched; 0 on success */
static int write_atomically(const char *path, int (*fill)(FILE *, const void *), const void *context)
{
	char temporary[1040];
	FILE *file;
	int failed;

	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if (!(file = fopen(temporary, "wb")))
		return -1;
	failed = fill(file, context) != 0;
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

struct manifest_contents
{
	const struct texture_cache_record *records;
	size_t count;
};

static int fill_manifest(FILE *file, const void *context)
{
	const struct manifest_contents *m = context;
	size_t i;

	for (i = 0; i < m->count; i++)
		if (fprintf(file, "%016" PRIx64 "\t%u\t%u\t%u\t%u\t%016" PRIx64 "\t%016" PRIx64 "\t%" PRIu64 "\n",
			m->records[i].hash, m->records[i].width, m->records[i].height, (unsigned)m->records[i].result,
			(unsigned)m->records[i].treatment, m->records[i].recipe_key, m->records[i].global_key,
			m->records[i].bytes) < 0)
			return -1;
	return 0;
}

int texture_cache_manifest_save(const char *directory, const struct texture_cache_record *records, size_t count)
{
	char path[1024];
	struct manifest_contents m = { records, count };

	manifest_path(directory, path, sizeof(path));
	return write_atomically(path, fill_manifest, &m);
}

/* ---------- storage: entry sizes, the low-storage floor, the level-start decision, entry writes */
static long write_fault = -1;

void texture_cache_set_write_fault(long fail_after_bytes)
{
	write_fault = fail_after_bytes;
}

uint64_t texture_cache_entry_bytes(uint32_t width, uint32_t height, uint32_t scale, uint32_t bits_per_texel)
{
	uint64_t texels = (uint64_t)width * height * 4;   /* the 2x level */

	if (scale == 4)
		texels += (uint64_t)width * height * 16;
	return texels * bits_per_texel / 8;
}

int texture_cache_floor_ok(uint64_t free_bytes, uint64_t missing_bytes)
{
	return free_bytes >= missing_bytes + TEXTURE_CACHE_FLOOR_SLACK;
}

enum texture_cache_level_start texture_cache_level_start(const struct texture_cache_level_plan *plan,
	uint64_t free_bytes)
{
	if (!plan->missing && !plan->redo_blocking)
		return TEXTURE_CACHE_LOAD_UPSCALED;
	return texture_cache_floor_ok(free_bytes, plan->blocking_bytes) ? TEXTURE_CACHE_UPSCALE_FIRST :
		TEXTURE_CACHE_LOW_STORAGE;
}

int texture_cache_background_may_run(uint64_t free_bytes, uint64_t next_entry_bytes)
{
	return texture_cache_floor_ok(free_bytes, next_entry_bytes);
}

struct entry_contents
{
	const void *bytes;
	size_t size;
};

static int fill_entry(FILE *file, const void *context)
{
	const struct entry_contents *e = context;
	size_t size = write_fault >= 0 && (size_t)write_fault < e->size ? (size_t)write_fault : e->size;

	return fwrite(e->bytes, 1, size, file) != e->size ? -1 : 0;
}

int texture_cache_write_entry(const char *directory, const struct texture_cache_record *record, const void *bytes,
	size_t size)
{
	char path[1024];
	struct entry_contents e = { bytes, size };

	snprintf(path, sizeof(path), "%s/%016" PRIx64 "-%ux%u.bin", directory, record->hash, record->width, record->height);
	return write_atomically(path, fill_entry, &e);
}
