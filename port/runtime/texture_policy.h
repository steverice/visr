/* The per-texture upscale policy on the device (spec 2026-10-04-halo-ce-ios-texture-upscale-policy-design.md):
the same rules as step-tools/upscale_lib/policy.py, over the imported maps, keyed by (level-0 hash, width, height)
as the runtime override lookup is. Host-side C; the import pipeline (piece 3) calls it. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum texture_policy_result { TEXTURE_POLICY_S4G, TEXTURE_POLICY_BPF, TEXTURE_POLICY_ORIGINAL };
enum texture_policy_kind
{
	TEXTURE_POLICY_GRAPHIC, TEXTURE_POLICY_SURFACE, TEXTURE_POLICY_COMPANION, TEXTURE_POLICY_BUMP,
	TEXTURE_POLICY_KIND_ORIGINAL
};
/* classes.treatment with alpha folded (cutout is color) */
enum texture_policy_treatment
{
	TEXTURE_POLICY_SKIP, TEXTURE_POLICY_COLOR, TEXTURE_POLICY_FIRST_PERSON, TEXTURE_POLICY_EMISSIVE,
	TEXTURE_POLICY_MULTIPURPOSE, TEXTURE_POLICY_BUMP_MAP
};

struct texture_policy_override
{
	const char *tag;
	int index;        /* -1: every bitmap of the tag */
	const char *hash; /* NULL, or the retail disc's 16 hex digits */
	enum texture_policy_result result;
};

struct texture_policy_table
{
	int schema, policy_version;
	enum texture_policy_result graphic, surface, companion, bump;
	float low, high, sigma;
	int iterations;
	const char *const *graphic_names;
	size_t graphic_name_count;
	const char *const *graphic_groups;
	size_t graphic_group_count;
	const char *const *decal_surface_prefixes;
	size_t decal_surface_prefix_count;
	const struct texture_policy_override *overrides;
	size_t override_count;
};
/* port/assets/texture-policy.json, by tools/embed_texture_policy.py */
extern const struct texture_policy_table texture_policy_embedded;

struct texture_policy_reference
{
	char group[5];   /* the referencing tag's group, e.g. "senv" */
	char slot[40];   /* "env.base", "model.multipurpose", "deca", ... as xbox_map.SLOTS names them */
	char shader[256];/* the referencing tag's path */
};

/* one bitmap of one map, as xbox_map.bitmaps reads it */
struct texture_policy_bitmap
{
	const char *tag;
	int index;
	int has_hash;    /* a 2D, non-linear texture: keyed by its level-0 hash */
	uint64_t hash;
	uint32_t width, height;
	int type;        /* 0 2d, 1 3d, 2 cube, 3 white */
	int format;      /* the bitmap format code (14 dxt1, 15 dxt3, 16 dxt5, 17 p8 bump, ...) */
	unsigned flags;  /* 0x1 power of two, 0x10 linear */
	int usage;       /* 2 height map, 3 detail map, 4 light map, 5 vector map */
	int group_type;  /* 3 sprites, 4 interface bitmaps */
	int alpha_tested;
	const struct texture_policy_reference *refs;
	size_t ref_count;
};

struct texture_policy_decision
{
	uint64_t hash;
	uint32_t width, height;
	enum texture_policy_kind kind;
	enum texture_policy_result result;
	enum texture_policy_treatment treatment;
};

typedef void (*texture_policy_log_proc)(void *context, const char *line);
struct texture_policy_catalog;

struct texture_policy_catalog *texture_policy_catalog_new(void);
void texture_policy_catalog_free(struct texture_policy_catalog *catalog);
/* 0 on success; maps must be added in sorted name order, as the Mac tool does, so first-seen tags agree */
int texture_policy_add_bitmap(struct texture_policy_catalog *catalog, const struct texture_policy_bitmap *bitmap);
/* a cache-version-5 map already inflated (0x800-byte header, then the zlib body inflated) */
int texture_policy_add_map(struct texture_policy_catalog *catalog, const unsigned char *data, size_t size,
	char *error, size_t error_size);
/* reads and inflates a .map file, then texture_policy_add_map */
int texture_policy_add_map_file(struct texture_policy_catalog *catalog, const char *path, char *error,
	size_t error_size);
/* the decisions for every keyed bitmap, sorted by hash, width and height; free() *out. log may be NULL */
size_t texture_policy_classify(struct texture_policy_catalog *catalog, const struct texture_policy_table *table,
	struct texture_policy_decision **out, texture_policy_log_proc log, void *log_context);
const char *texture_policy_result_name(enum texture_policy_result result);
const char *texture_policy_kind_name(enum texture_policy_kind kind);
