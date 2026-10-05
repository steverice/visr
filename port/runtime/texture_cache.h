/* The upscale cache's recipe key, the action for each texture after an app update, and the manifest (spec
2026-10-04-halo-ce-ios-texture-upscale-policy-design.md, "Cache and versioning"). The cache never keys on
`policy_version`: a policy edit changes only the textures whose result changes. Host-side C; the import pipeline
(piece 3) calls it. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "texture_policy.h"

struct texture_recipe_globals
{
	const char *model_id;              /* "S4g: PBRify SPAN V4 per channel" */
	const char *model_sha256;          /* the weights' SHA-256, hex */
	uint32_t pipeline_version[6];      /* by enum texture_policy_treatment; raised when that path's code changes */
	float low, high, sigma;            /* bpf only */
	uint32_t iterations, post_version; /* bpf only: the bp and bpf kernels' version */
	const char *encoder;               /* piece 2's encoding and its settings, e.g. "rgba8" until then */
};
struct texture_recipe
{
	uint64_t source_hash;
	uint32_t width, height;
	enum texture_policy_result result;
	enum texture_policy_treatment treatment;
	int wrap, dilates;
	int alpha;                         /* 0 none, 1 binary, 2 graded */
	uint32_t scale;                    /* 2, or 4 for first-person paths */
};
struct texture_cache_record
{
	uint64_t hash;
	uint32_t width, height;
	enum texture_policy_result result;
	enum texture_policy_treatment treatment;
	uint64_t recipe_key, global_key, bytes;
};
enum texture_cache_action
{
	TEXTURE_CACHE_KEEP,                /* up to date, or original and absent */
	TEXTURE_CACHE_MISSING,             /* not cached: the blocking screen or the background queue makes it */
	TEXTURE_CACHE_DELETE_NOW,          /* now original: deleted at the next level load, no queue */
	TEXTURE_CACHE_REPLACE_BACKGROUND,  /* S4g <-> bpf or a per-texture input changed: the old entry serves until replaced */
	TEXTURE_CACHE_REDO_BLOCKING        /* on an old global recipe: redone on the blocking screen, never mixed */
};
uint64_t texture_recipe_global_key(const struct texture_recipe_globals *, enum texture_policy_treatment,
	enum texture_policy_result);
uint64_t texture_recipe_key(const struct texture_recipe_globals *, const struct texture_recipe *);
enum texture_cache_action texture_cache_action(const struct texture_cache_record *stored /* NULL: none */,
	const struct texture_cache_record *desired, uint64_t stored_global_now);
/* the manifest: <directory>/manifest.tsv, one record per line; load returns the count, *records malloc'd */
size_t texture_cache_manifest_load(const char *directory, struct texture_cache_record **records);
int texture_cache_manifest_save(const char *directory, const struct texture_cache_record *records, size_t count);
