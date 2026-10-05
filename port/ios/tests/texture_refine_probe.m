/* texture_refine.m against the Mac's NumPy reference (step-tools/upscale_lib/consistency.py, bump.refine):
   no argument: synthetic checks; a folder: every <name>.trf fixture there, PSNR over 40 dB */
#import <Foundation/Foundation.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "texture_refine.h"

static double psnr(const float *a, const float *b, size_t n, double peak)
{
	double sum = 0;
	size_t i;

	for (i = 0; i < n; i++)
		sum += ((double)a[i] - b[i]) * ((double)a[i] - b[i]);
	return sum == 0 ? 200 : 10 * log10(peak * peak / (sum / n));
}

static void synthetic(struct texture_refine_context *context)
{
	enum { W = 16, H = 16, S = 2 };
	static float reference[W * H], top[W * S * H * S], mask[W * H];
	struct texture_refine_params params = { 12, 30, 1, 3 };
	size_t i;

	for (i = 0; i < W * H; i++)
		reference[i] = 0.5f;
	for (i = 0; i < W * S * H * S; i++)
		top[i] = 0.5f;                       /* consistent: bp is a no-op, nothing is masked */
	assert(texture_refine_color(context, top, reference, W, H, 1, S, 1, &params, 1, mask) == 0);
	for (i = 0; i < W * S * H * S; i++)
		assert(fabsf(top[i] - 0.5f) < 1e-4f);
	for (i = 0; i < W * H; i++)
		assert(mask[i] == 0);
	for (i = 0; i < W * S * H * S; i++)
		top[i] = 0.9f;                       /* 102 levels off everywhere: masked, and pulled to the original */
	assert(texture_refine_color(context, top, reference, W, H, 1, S, 1, &params, 1, mask) == 0);
	for (i = 0; i < W * H; i++)
		assert(mask[i] > 0.99f);
	for (i = 0; i < W * S * H * S; i++)
		assert(fabsf(top[i] - 0.5f) < 1e-3f);
	/* out-of-range input is clipped before the first round (consistency.back_project): a checker of 1.4 and 0.6 along
	   x over an original of 1.0. Clipped, down(top) is 0.8 and one round lifts the 0.6 texels to 0.8; unclipped,
	   down(top) is already 1.0 and they stay at 0.6 */
	params.iterations = 1;
	for (i = 0; i < W * H; i++)
		reference[i] = 1.0f;
	for (i = 0; i < W * S * H * S; i++)
		top[i] = i % 2 ? 0.6f : 1.4f;
	assert(texture_refine_color(context, top, reference, W, H, 1, S, 1, &params, 0, NULL) == 0);
	for (i = 0; i < W * S * H * S; i++)
		assert(fabsf(top[i] - (i % 2 ? 0.8f : 1.0f)) < 1e-4f);
	puts("texture_refine_probe: synthetic ok");
}

static int fixture(struct texture_refine_context *context, NSString *path)
{
	NSData *data = [NSData dataWithContentsOfFile:path];
	const unsigned char *bytes = data.bytes;
	uint32_t header[6], iterations;
	float levels[3];
	const float *arrays;
	size_t w, h, c, W, H, at = 4;
	struct texture_refine_params params;
	double worst = 200;
	int fallback;

	if (data.length < 44 || memcmp(bytes, "TRF1", 4))
		return fprintf(stderr, "%s: not a fixture\n", path.UTF8String), 1;
	memcpy(header, bytes + at, sizeof(header)); at += sizeof(header);
	memcpy(levels, bytes + at, sizeof(levels)); at += sizeof(levels);
	memcpy(&iterations, bytes + at, 4); at += 4;
	params = (struct texture_refine_params){ levels[0], levels[1], levels[2], iterations };
	w = header[1]; h = header[2]; c = header[3]; W = w * header[4]; H = h * header[4];
	arrays = (const float *)(bytes + at);
	for (fallback = 0; fallback <= 1; fallback++)
	{
		float *mask = calloc(w * h, sizeof(float));
		double a, m;

		if (header[0] == 0)
		{
			const float *reference = arrays, *top = reference + w * h * c, *expected = top + W * H * c + (fallback ? W * H * c : 0);
			const float *expected_mask = top + 3 * W * H * c;
			float *out = malloc(W * H * c * sizeof(float));

			memcpy(out, top, W * H * c * sizeof(float));
			if (texture_refine_color(context, out, reference, (uint32_t)w, (uint32_t)h, (uint32_t)c, header[4],
				(int)header[5], &params, fallback, mask))
				return 1;
			a = psnr(out, expected, W * H * c, 1);
			m = psnr(mask, expected_mask, w * h, 1);
			free(out);
		}
		else
		{
			const float *reference = arrays, *top = reference + w * h, *expected = top + W * H + (fallback ? W * H * 3 : 0);
			const float *expected_mask = top + W * H + 2 * W * H * 3;
			float *normals = malloc(W * H * 3 * sizeof(float));

			if (texture_refine_bump(context, top, reference, (uint32_t)w, (uint32_t)h, header[4], &params, fallback,
				normals, mask))
				return 1;
			a = psnr(normals, expected, W * H * 3, 2);   /* normals span -1..1: the same scale as encoded levels */
			m = psnr(mask, expected_mask, w * h, 1);
			free(normals);
		}
		printf("%s %s: output %.1f dB, mask %.1f dB\n", path.lastPathComponent.UTF8String, fallback ? "bpf" : "bp", a, m);
		worst = fmin(worst, fmin(a, m));
		free(mask);
	}
	return worst > 40 ? 0 : 1;
}

int main(int argc, char **argv)
{
	@autoreleasepool
	{
		char error[256];
		struct texture_refine_context *context = texture_refine_create(NULL, error, sizeof(error));
		int failures = 0;

		if (!context)
		{
			printf("texture_refine_probe: skipped (%s)\n", error);
			return 0;
		}
		synthetic(context);
		if (argc > 1)
			for (NSString *name in [[NSFileManager.defaultManager contentsOfDirectoryAtPath:@(argv[1]) error:nil]
				sortedArrayUsingSelector:@selector(compare:)])
				if ([name hasSuffix:@".trf"])
					failures += fixture(context, [@(argv[1]) stringByAppendingPathComponent:name]);
		texture_refine_destroy(context);
		printf("texture_refine_probe: %s\n", failures ? "FAILED" : "ok");
		return failures ? 1 : 0;
	}
}
