/*
GPU_METAL.M

The Metal backend of gpu.h (display.renderer = "metal"), in the iOS and tvOS
host. This first version only proves the window: it clears the drawable to a
fixed color at every Present, and answers the front end's other calls so the
game keeps running: handles from a counter, streams into a CPU buffer, every
shader failing to compile (so every draw counts as skipped), and every
visibility test passing.
*/

#include "gpu.h"
#include "ios_host.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

/* the platform layer's (host_gpu.c) */
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static CAMetalLayer *layer;
static uint32_t next_handle;
/* gpu_stream's offsets wrap at this size: the front end only needs offsets
and a buffer handle, so nothing is stored */
#define STREAM_SIZE (16 * 1024 * 1024)
static uint32_t stream_offset;

static uint32_t handle_new(void)
{
	return ++next_handle;
}

static gpu_texture gpu_metal_texture_create(const struct gpu_texture_description *description)
{
	return handle_new();
}

static void gpu_metal_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data,
	uint32_t size)
{
}

static void gpu_metal_texture_copy_level(gpu_texture source, gpu_texture destination, uint32_t level)
{
}

static void gpu_metal_texture_generate_mipmaps(gpu_texture texture, uint32_t base_level)
{
}

static void gpu_metal_texture_destroy(gpu_texture texture)
{
}

static uint32_t gpu_metal_texture_read(gpu_texture texture, void *pixels, uint32_t size)
{
	return 0;
}

static gpu_buffer gpu_metal_buffer_create(uint32_t size)
{
	return handle_new();
}

static void gpu_metal_buffer_write(gpu_buffer buffer, uint32_t offset, uint32_t size, const void *data, uint32_t flags)
{
}

static void gpu_metal_stream_reserve(uint32_t vertex_bytes, uint32_t index_bytes)
{
	if (stream_offset + vertex_bytes > STREAM_SIZE)
		stream_offset = 0;
}

static uint32_t gpu_metal_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer)
{
	uint32_t offset;

	size = (size + 15) & ~15u;
	if (stream_offset + size > STREAM_SIZE)
		stream_offset = 0;
	offset = stream_offset;
	stream_offset += size;
	*buffer = 1;
	return offset;
}

static gpu_shader gpu_metal_shader_create(uint32_t stage, const char *source)
{
	return 0;
}

static void gpu_metal_clear(const struct gpu_clear *clear, const struct gpu_rect *rectangles, uint32_t count)
{
}

static uint32_t gpu_metal_draw(const struct gpu_draw *draw, const struct gpu_constant_store *constants,
	const struct gpu_uniforms *uniforms)
{
	return 0;
}

static void gpu_metal_visibility_begin(void)
{
}

static void gpu_metal_visibility_end(uint32_t slot)
{
}

/* an unanswered test makes the game ask again forever */
static uint32_t gpu_metal_visibility_result(uint32_t slot, uint32_t *samples)
{
	*samples = 1;
	return 1;
}

static void gpu_metal_flush(void)
{
}

static void gpu_metal_present(gpu_texture back_buffer)
{
	@autoreleasepool
	{
		id<CAMetalDrawable> drawable = [layer nextDrawable];
		id<MTLCommandBuffer> commands = [queue commandBuffer];

		if (drawable)
		{
			MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];

			pass.colorAttachments[0].texture = drawable.texture;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			/* Metal's color, so a working window can't be mistaken for GL */
			pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.25, 0.5, 1.0);
			[[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
			[commands presentDrawable:drawable];
		}
		[commands commit];
	}
	stream_offset = 0;
}

static uint32_t gpu_metal_call_count_take(void)
{
	return 0;
}

static void gpu_metal_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
	memset(capabilities, 0, sizeof(*capabilities));
	layer = (__bridge CAMetalLayer *)host_sdl_metal_layer();
	device = MTLCreateSystemDefaultDevice();
	if (!layer || !device)
		host_fatal("Metal is unavailable (layer %p, device %p)", (__bridge void *)layer, (__bridge void *)device);
	queue = [device newCommandQueue];
	layer.device = device;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = YES;
	platform_log("Metal on %s", device.name.UTF8String);
	/* what GL reports on Apple's OpenGL ES 3.0 (gpu_gl.c's gpu_gl_initialize),
	so the front end behaves exactly as it does under GL */
	capabilities->vertex_bgra = 0;
	capabilities->base_vertex = 0;
	capabilities->triangle_fans = 1;
	capabilities->line_loops = 1;
	capabilities->sampler_lod_bias = 0;
	capabilities->occlusion_mode = GPU_OCCLUSION_ANY_SAMPLE;
	capabilities->s3tc = 0;
	capabilities->border_clamp = 0;
	capabilities->max_texture_size = 16384;
	capabilities->shader_language = GPU_SHADER_LANGUAGE_MSL;
	capabilities->shader_es = 0;
	capabilities->clip_y_flip = 0;
	capabilities->clip_z_remap = 0;
	GPU_CAPABILITIES_LOG(platform_log, capabilities);
}

#define GPU_METAL_FUNCTION(type, name, parameters, arguments) .name = gpu_metal_##name,
#define GPU_METAL_PROCEDURE(name, parameters, arguments) .name = gpu_metal_##name,
const struct gpu_backend gpu_backend_metal = { GPU_FUNCTIONS(GPU_METAL_FUNCTION, GPU_METAL_PROCEDURE) };
