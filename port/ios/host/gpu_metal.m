/*
GPU_METAL.M

The Metal backend of gpu.h (display.renderer = "metal"), in the iOS and tvOS
host. It reports the capabilities GL reports on Apple's OpenGL ES 3.0, so the
front end does exactly what it does under GL, and it reproduces GL's results
where Metal works differently:

- GL applies every upload in order with the draws around it; Metal runs a
  frame's commands after they are recorded. A CPU write into a buffer or
  texture that a recorded or running command buffer reads gets new storage
  instead ("renaming", buffer_write and texture_upload).
- Frames in flight are bounded by a semaphore with FRAMES slots, signaled when
  each frame's last command buffer completes; per-frame memory (streams,
  constant snapshots) has a slot per frame.
- Render passes begin at the first draw or clear for a pair of targets and end
  when the pair changes, at copies, mipmap generation, readback and Present.

Every entry point runs in an autorelease pool: the guest runs on the UI thread
but never returns to its run loop, which would otherwise drain them.
*/

#include "gpu.h"
#include "ios_host.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

/* the platform layer's (host_gpu.c) */
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* frames in flight, as gpu_gl.c's STREAM_BUFFER_RING */
#define FRAMES 3
/* the size of a transient chunk: gpu_gl.c's STREAM_BUFFER_SIZE */
#define CHUNK_SIZE (16 * 1024 * 1024)
/* constant buffer offsets: Metal's strictest alignment on any family */
#define CONSTANT_ALIGNMENT 256

/* ---------- records

Handles index tables of these; 0 is none. */

@interface MetalTexture : NSObject
{
@public
	struct gpu_texture_description description;
	id<MTLTexture> texture;          /* nil until an upload texture's first upload */
	unsigned long levels;            /* description.levels, at most what the size allows */
	uint64_t used;                   /* the last command buffer serial that read or wrote it */
}
@end
@implementation MetalTexture
@end

@interface MetalBuffer : NSObject
{
@public
	id<MTLBuffer> buffer;
	uint64_t used;
	BOOL transient;                  /* a stream chunk: never renamed */
}
@end
@implementation MetalBuffer
@end

/* a handle table: index 0 holds NSNull */
@interface MetalTable : NSObject
{
@public
	NSMutableArray *objects;
	NSMutableIndexSet *free_handles;
}
@end
@implementation MetalTable
- (instancetype)init
{
	if ((self = [super init]))
	{
		objects = [NSMutableArray arrayWithObject:[NSNull null]];
		free_handles = [NSMutableIndexSet indexSet];
	}
	return self;
}
- (uint32_t)add:(id)object
{
	NSUInteger handle = free_handles.firstIndex;

	if (handle == NSNotFound)
	{
		[objects addObject:object];
		return (uint32_t)(objects.count - 1);
	}
	[free_handles removeIndex:handle];
	objects[handle] = object;
	return (uint32_t)handle;
}
- (id)get:(uint32_t)handle
{
	id object = handle && handle < objects.count ? objects[handle] : nil;

	return object == [NSNull null] ? nil : object;
}
- (void)remove:(uint32_t)handle
{
	if (!handle || handle >= objects.count || objects[handle] == [NSNull null])
		return;
	objects[handle] = [NSNull null];
	[free_handles addIndex:handle];
}
@end

/* ---------- state */

static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static CAMetalLayer *layer;
static MetalTable *textures, *buffers, *shaders;
static int metal_debug;

/* the open command buffer and render pass */
static id<MTLCommandBuffer> commands;
static id<MTLRenderCommandEncoder> encoder;
static gpu_texture pass_color, pass_depth;
static unsigned long pass_commands;
/* the open command buffer's serial; every earlier one has been committed */
static uint64_t current_serial = 1;
static _Atomic uint64_t completed_serial;

/* frames */
static dispatch_semaphore_t frame_slots;
static int frame_started;
static unsigned long frame_slot;
static unsigned long frames;
static unsigned long renames;

/* ---------- transient memory: a frame slot's streams and constant snapshots,
in chunks that live until the slot's frame comes around again */

struct transient
{
	/* chunk handles in the buffer table */
	gpu_buffer chunks[64];
	unsigned long chunk_count, chunk, offset;
};

static struct transient streams[FRAMES], snapshots[FRAMES];

static MetalBuffer *buffer_record(gpu_buffer handle)
{
	return [buffers get:handle];
}

static MetalTexture *texture_record(gpu_texture handle)
{
	return [textures get:handle];
}

/* room for size bytes at alignment in the current chunk, moving to the next
(created as needed, never smaller than CHUNK_SIZE) when it doesn't fit; never
wraps, since recorded draws may still read what is behind */
static MetalBuffer *transient_room(struct transient *memory, uint32_t size, uint32_t alignment)
{
	MetalBuffer *record;

	for (;;)
	{
		if (memory->chunk < memory->chunk_count)
		{
			record = buffer_record(memory->chunks[memory->chunk]);
			memory->offset = (memory->offset + alignment - 1) & ~(unsigned long)(alignment - 1);
			if (memory->offset + size <= record->buffer.length)
				return record;
			memory->chunk++;
			memory->offset = 0;
			continue;
		}
		if (memory->chunk_count == sizeof(memory->chunks) / sizeof(memory->chunks[0]))
			host_fatal("Metal: a frame streamed more than %lu chunks", memory->chunk_count);
		record = [MetalBuffer new];
		record->buffer = [device newBufferWithLength:size > CHUNK_SIZE ? size : CHUNK_SIZE
			options:MTLResourceStorageModeShared];
		record->transient = YES;
		memory->chunks[memory->chunk_count++] = [buffers add:record];
	}
}

/* copies size bytes into transient memory; returns the offset and sets
*buffer to the chunk's handle */
static uint32_t transient_copy(struct transient *memory, const void *data, uint32_t size, uint32_t alignment,
	gpu_buffer *buffer)
{
	MetalBuffer *record = transient_room(memory, size, alignment);
	uint32_t offset = (uint32_t)memory->offset;

	memcpy((unsigned char *)record->buffer.contents + offset, data, size);
	memory->offset += size;
	*buffer = memory->chunks[memory->chunk];
	return offset;
}

static void transient_reset(struct transient *memory)
{
	memory->chunk = 0;
	memory->offset = 0;
}

/* ---------- command buffers and frames */

static BOOL busy(uint64_t used)
{
	return used > atomic_load(&completed_serial);
}

/* waits for the frame slot about to be reused, once per frame, before
anything writes into its transient memory */
static void frame_begin(void)
{
	if (frame_started)
		return;
	dispatch_semaphore_wait(frame_slots, DISPATCH_TIME_FOREVER);
	transient_reset(&streams[frame_slot]);
	transient_reset(&snapshots[frame_slot]);
	frame_started = 1;
}

static id<MTLCommandBuffer> command_buffer(void)
{
	frame_begin();
	if (!commands)
	{
		MTLCommandBufferDescriptor *descriptor = [MTLCommandBufferDescriptor new];

		descriptor.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
		commands = [queue commandBufferWithDescriptor:descriptor];
		if (metal_debug)
			commands.label = [NSString stringWithFormat:@"frame %lu", frames];
	}
	return commands;
}

static void pass_end(void)
{
	[encoder endEncoding];
	encoder = nil;
	pass_color = pass_depth = 0;
	pass_commands = 0;
}

/* commits the open command buffer; the frame's last one (at Present) also
frees its frame slot when it completes */
static void commit(BOOL frame_end)
{
	id<MTLCommandBuffer> committed;
	uint64_t serial = current_serial;

	pass_end();
	committed = command_buffer();
	[committed addCompletedHandler:^(id<MTLCommandBuffer> completed)
	{
		if (completed.status == MTLCommandBufferStatusError)
			platform_log("Metal: command buffer %llu failed: %s", (unsigned long long)serial,
				completed.error.description.UTF8String);
		atomic_store(&completed_serial, serial);
		if (frame_end)
			dispatch_semaphore_signal(frame_slots);
	}];
	[committed commit];
	commands = nil;
	current_serial++;
	if (frame_end)
	{
		frame_started = 0;
		frame_slot = (frame_slot + 1) % FRAMES;
	}
}

static void use_texture(MetalTexture *record)
{
	if (record)
		record->used = current_serial;
}

static void use_buffer(MetalBuffer *record)
{
	if (record)
		record->used = current_serial;
}

/* ---------- textures */

static unsigned long level_dimension(uint32_t size, unsigned long level)
{
	return size >> level ? size >> level : 1;
}

static MTLPixelFormat texture_format(const struct gpu_texture_description *description)
{
	return description->format == GPU_FORMAT_DEPTH_STENCIL ? MTLPixelFormatDepth32Float_Stencil8 :
		MTLPixelFormatBGRA8Unorm;
}

/* the storage a texture's description asks for */
static id<MTLTexture> texture_storage(MetalTexture *record)
{
	const struct gpu_texture_description *description = &record->description;
	MTLTextureDescriptor *storage = [MTLTextureDescriptor new];

	storage.textureType = description->type == GPU_TEXTURE_CUBE ? MTLTextureTypeCube :
		description->type == GPU_TEXTURE_3D ? MTLTextureType3D : MTLTextureType2D;
	storage.pixelFormat = texture_format(description);
	storage.width = description->width ? description->width : 1;
	storage.height = description->height ? description->height : 1;
	storage.depth = description->type == GPU_TEXTURE_3D && description->depth ? description->depth : 1;
	storage.mipmapLevelCount = record->levels;
	if (description->usage == GPU_USAGE_RENDER_TARGET)
	{
		storage.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		storage.storageMode = MTLStorageModePrivate;
	}
	else
	{
		/* the GPU only reads upload textures, so the CPU's copy is always current */
		storage.usage = MTLTextureUsageShaderRead;
		storage.storageMode = MTLStorageModeShared;
	}
	return [device newTextureWithDescriptor:storage];
}

/* a new render target's contents are undefined under Metal, and zero under
GL, which shows on a frame that presents a back buffer nothing drew. The
clear runs in a command buffer of its own, committed now, so the open pass
stays open: the queue runs it before anything that uses the target. */
static void target_zero(id<MTLTexture> texture)
{
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	id<MTLCommandBuffer> zero = [queue commandBuffer];

	if (texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
	{
		pass.depthAttachment.texture = texture;
		pass.depthAttachment.loadAction = MTLLoadActionClear;
		pass.depthAttachment.storeAction = MTLStoreActionStore;
		pass.depthAttachment.clearDepth = 0.0;
		pass.stencilAttachment.texture = texture;
		pass.stencilAttachment.loadAction = MTLLoadActionClear;
		pass.stencilAttachment.storeAction = MTLStoreActionStore;
		pass.stencilAttachment.clearStencil = 0;
	}
	else
	{
		pass.colorAttachments[0].texture = texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
	}
	[[zero renderCommandEncoderWithDescriptor:pass] endEncoding];
	[zero commit];
}

static gpu_texture gpu_metal_texture_create(const struct gpu_texture_description *description)
{
	@autoreleasepool
	{
		MetalTexture *record = [MetalTexture new];
		uint32_t largest = description->width;
		unsigned long possible = 1;

		record->description = *description;
		if (description->height > largest)
			largest = description->height;
		if (description->type == GPU_TEXTURE_3D && description->depth > largest)
			largest = description->depth;
		while (largest >> possible)
			possible++;
		/* the D3D level count comes from the format word unchecked
		(xbox_textures.c); GL ignores levels past the smallest, Metal can't
		create them */
		record->levels = description->levels < 1 ? 1 : description->levels > possible ? possible : description->levels;
		if (description->format != GPU_FORMAT_BGRA8 && description->format != GPU_FORMAT_DEPTH_STENCIL)
			platform_log("Metal: texture format %u is not supported yet; it samples black", description->format);
		/* render targets have storage at once; upload textures get theirs
		from their first upload, as under GL, and sample black until then */
		else if (description->usage == GPU_USAGE_RENDER_TARGET)
		{
			record->texture = texture_storage(record);
			target_zero(record->texture);
		}
		return [textures add:record];
	}
}

static void gpu_metal_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
	@autoreleasepool
	{
		MetalTexture *record = texture_record(texture);
		const struct gpu_texture_description *description;
		unsigned long width, height, depth, row;

		if (!record || record->description.format != GPU_FORMAT_BGRA8 || level >= record->levels)
			return;
		description = &record->description;
		if (!record->texture || busy(record->used))
		{
			id<MTLTexture> previous = record->texture;

			record->texture = texture_storage(record);
			/* nothing has used the new version yet: the rest of this
			refresh's levels go into it instead of renaming it again */
			record->used = 0;
			/* a refresh starts at face 0, level 0 and uploads everything;
			anything else keeps the levels it doesn't replace */
			if (previous && (face || level))
			{
				unsigned long copy_face, copy_level, faces = description->type == GPU_TEXTURE_CUBE ? 6 : 1;

				platform_log("Metal: texture %u face %u level %u uploaded while in use", texture, face, level);
				for (copy_face = 0; copy_face < faces; copy_face++)
					for (copy_level = 0; copy_level < record->levels; copy_level++)
					{
						unsigned long w = level_dimension(description->width, copy_level);
						unsigned long h = level_dimension(description->height, copy_level);
						unsigned long d = description->type == GPU_TEXTURE_3D ? level_dimension(description->depth, copy_level) : 1;
						NSMutableData *texels = [NSMutableData dataWithLength:w * h * d * 4];

						[previous getBytes:texels.mutableBytes bytesPerRow:w * 4 bytesPerImage:w * h * 4
							fromRegion:MTLRegionMake3D(0, 0, 0, w, h, d) mipmapLevel:copy_level slice:copy_face];
						[record->texture replaceRegion:MTLRegionMake3D(0, 0, 0, w, h, d) mipmapLevel:copy_level
							slice:copy_face withBytes:texels.bytes bytesPerRow:w * 4 bytesPerImage:w * h * 4];
					}
			}
			if (previous)
				renames++;
		}
		width = level_dimension(description->width, level);
		height = level_dimension(description->height, level);
		depth = description->type == GPU_TEXTURE_3D ? level_dimension(description->depth, level) : 1;
		row = width * 4;
		if (size < row * height * depth)
			return;
		[record->texture replaceRegion:MTLRegionMake3D(0, 0, 0, width, height, depth) mipmapLevel:level
			slice:description->type == GPU_TEXTURE_CUBE ? face : 0 withBytes:data bytesPerRow:row
			bytesPerImage:row * height];
	}
}

static void gpu_metal_texture_destroy(gpu_texture texture)
{
	@autoreleasepool
	{
		/* a committed command buffer keeps what it uses. The handle can be
		reused at once, so a pass still open on it ends first: a new target
		with the same handle would otherwise continue the old one's pass */
		if (texture && (texture == pass_color || texture == pass_depth))
			pass_end();
		[textures remove:texture];
	}
}

static uint32_t gpu_metal_texture_read(gpu_texture texture, void *pixels, uint32_t size)
{
	@autoreleasepool
	{
		MetalTexture *record = texture_record(texture);
		const struct gpu_texture_description *description = record ? &record->description : NULL;
		unsigned long width, height;
		id<MTLBuffer> staging;
		id<MTLBlitCommandEncoder> blit;

		if (!record || !record->texture || description->type != GPU_TEXTURE_2D ||
			description->format != GPU_FORMAT_BGRA8)
			return 0;
		width = description->width;
		height = description->height;
		if (size < width * height * 4)
			return 0;
		/* BGRA8Unorm is already the BGRA byte order gpu.h reads back, and row
		0 is the top */
		if (description->usage != GPU_USAGE_RENDER_TARGET)
		{
			[record->texture getBytes:pixels bytesPerRow:width * 4 fromRegion:MTLRegionMake2D(0, 0, width, height)
				mipmapLevel:0];
			return 1;
		}
		staging = [device newBufferWithLength:width * height * 4 options:MTLResourceStorageModeShared];
		pass_end();
		blit = [command_buffer() blitCommandEncoder];
		[blit copyFromTexture:record->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			sourceSize:MTLSizeMake(width, height, 1) toBuffer:staging destinationOffset:0
			destinationBytesPerRow:width * 4 destinationBytesPerImage:width * height * 4];
		[blit endEncoding];
		use_texture(record);
		{
			id<MTLCommandBuffer> waited = commands;

			commit(NO);
			[waited waitUntilCompleted];
		}
		memcpy(pixels, staging.contents, width * height * 4);
		return 1;
	}
}

/* level 0 of source into level of destination (mip composites); the size is
the destination level's, as gpu_gl.c copies it */
static void gpu_metal_texture_copy_level(gpu_texture source, gpu_texture destination, uint32_t level)
{
	@autoreleasepool
	{
		MetalTexture *from = texture_record(source), *to = texture_record(destination);
		id<MTLBlitCommandEncoder> blit;
		unsigned long width, height;

		if (!from || !to || !from->texture || !to->texture || level >= to->levels)
			return;
		width = level_dimension(to->description.width, level);
		height = level_dimension(to->description.height, level);
		if (width > from->texture.width || height > from->texture.height)
			return;
		pass_end();
		blit = [command_buffer() blitCommandEncoder];
		[blit copyFromTexture:from->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			sourceSize:MTLSizeMake(width, height, 1) toTexture:to->texture destinationSlice:0
			destinationLevel:level destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		use_texture(from);
		use_texture(to);
	}
}

/* the levels after base_level from base_level */
static void gpu_metal_texture_generate_mipmaps(gpu_texture texture, uint32_t base_level)
{
	@autoreleasepool
	{
		MetalTexture *record = texture_record(texture);
		id<MTLTexture> levels;
		id<MTLBlitCommandEncoder> blit;

		if (!record || !record->texture || base_level + 1 >= record->levels)
			return;
		levels = base_level ? [record->texture newTextureViewWithPixelFormat:record->texture.pixelFormat
			textureType:MTLTextureType2D levels:NSMakeRange(base_level, record->levels - base_level)
			slices:NSMakeRange(0, 1)] : record->texture;
		pass_end();
		blit = [command_buffer() blitCommandEncoder];
		[blit generateMipmapsForTexture:levels];
		[blit endEncoding];
		use_texture(record);
	}
}

/* ---------- buffers and streams */

static gpu_buffer gpu_metal_buffer_create(uint32_t size)
{
	@autoreleasepool
	{
		MetalBuffer *record = [MetalBuffer new];

		record->buffer = [device newBufferWithLength:size ? size : 1 options:MTLResourceStorageModeShared];
		return [buffers add:record];
	}
}

static void gpu_metal_buffer_write(gpu_buffer buffer, uint32_t offset, uint32_t size, const void *data, uint32_t flags)
{
	@autoreleasepool
	{
		MetalBuffer *record = buffer_record(buffer);

		if (!record || offset > record->buffer.length || size > record->buffer.length - offset)
			return;
		/* a range no queued draw reads (GPU_WRITE_UNUSED), or a buffer nothing
		recorded or running uses, takes the write in place; otherwise the
		buffer gets new storage holding the old contents and the write, and
		the commands already recorded keep the old storage */
		if (!(flags & GPU_WRITE_UNUSED) && busy(record->used))
		{
			id<MTLBuffer> renamed = [device newBufferWithLength:record->buffer.length
				options:MTLResourceStorageModeShared];

			memcpy(renamed.contents, record->buffer.contents, record->buffer.length);
			record->buffer = renamed;
			record->used = 0;
			renames++;
		}
		memcpy((unsigned char *)record->buffer.contents + offset, data, size);
	}
}

static void gpu_metal_stream_reserve(uint32_t vertex_bytes, uint32_t index_bytes)
{
	@autoreleasepool
	{
		/* a new chunk never discards the frame's earlier streams, so there is
		nothing to keep together; only the slot must be ours */
		frame_begin();
	}
}

static uint32_t gpu_metal_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer)
{
	@autoreleasepool
	{
		MetalBuffer *record;
		uint32_t offset;

		frame_begin();
		/* copies size bytes and keeps the next stream 16-aligned (gpu_gl.c
		copies the rounded size, reading past the caller's data) */
		offset = transient_copy(&streams[frame_slot], data, size, 16, buffer);
		record = buffer_record(*buffer);
		use_buffer(record);
		return offset;
	}
}

/* ---------- shaders (Task 6) */

static gpu_shader gpu_metal_shader_create(uint32_t stage, const char *source)
{
	return 0;
}

/* ---------- render passes */

static void target_size(gpu_texture color, gpu_texture depth, unsigned long *width, unsigned long *height)
{
	MetalTexture *record = texture_record(color ? color : depth);

	*width = record && record->texture ? record->texture.width : 0;
	*height = record && record->texture ? record->texture.height : 0;
}

/* opens a pass on the pair, unless it is the open one; a load action of
Clear clears what it names before anything in the pass */
static BOOL pass_begin(gpu_texture color, gpu_texture depth, const struct gpu_clear *clear)
{
	MetalTexture *color_record = texture_record(color), *depth_record = texture_record(depth);
	MTLRenderPassDescriptor *pass;

	if (encoder && pass_color == color && pass_depth == depth && !clear)
		return YES;
	if ((color && (!color_record || !color_record->texture)) || (depth && (!depth_record || !depth_record->texture)) ||
		(!color && !depth))
		return NO;
	pass_end();
	pass = [MTLRenderPassDescriptor renderPassDescriptor];
	if (color)
	{
		pass.colorAttachments[0].texture = color_record->texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		if (clear && (clear->flags & GPU_CLEAR_COLOR))
		{
			float red = (float)((clear->color >> 16) & 0xff) / 255.0f, green = (float)((clear->color >> 8) & 0xff) / 255.0f;
			float blue = (float)(clear->color & 0xff) / 255.0f, alpha = (float)(clear->color >> 24) / 255.0f;

			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(red, green, blue, alpha);
		}
		use_texture(color_record);
	}
	if (depth)
	{
		pass.depthAttachment.texture = depth_record->texture;
		pass.depthAttachment.loadAction = clear && (clear->flags & GPU_CLEAR_DEPTH) ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.depthAttachment.storeAction = MTLStoreActionStore;
		pass.depthAttachment.clearDepth = clear ? clear->depth : 1.0;
		pass.stencilAttachment.texture = depth_record->texture;
		pass.stencilAttachment.loadAction = clear && (clear->flags & GPU_CLEAR_STENCIL) ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.stencilAttachment.storeAction = MTLStoreActionStore;
		pass.stencilAttachment.clearStencil = clear ? clear->stencil & 0xff : 0;
		use_texture(depth_record);
	}
	encoder = [command_buffer() renderCommandEncoderWithDescriptor:pass];
	if (metal_debug)
		encoder.label = [NSString stringWithFormat:@"targets %u/%u", color, depth];
	pass_color = color;
	pass_depth = depth;
	pass_commands = 0;
	return YES;
}

/* ---------- clears */

/* the pipelines that draw a clear as a triangle covering the scissor: by
color format (or none), depth-stencil format (or none) and write mask */
static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *clear_pipelines;
static NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>> *clear_depth_states;
static id<MTLFunction> clear_vertex, clear_fragment, present_vertex, present_fragment;
static id<MTLRenderPipelineState> present_pipeline;
static id<MTLSamplerState> present_sampler;

static NSString *const utility_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct ClearOut { float4 position [[position]]; };\n"
	"vertex ClearOut clear_vertex(uint id [[vertex_id]], constant float &depth [[buffer(0)]])\n"
	"{\n"
	"\tClearOut out;\n"
	"\tout.position = float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, depth, 1.0);\n"
	"\treturn out;\n"
	"}\n"
	"fragment float4 clear_fragment(constant float4 &color [[buffer(0)]]) { return color; }\n"
	"struct PresentOut { float4 position [[position]]; float2 coordinates; };\n"
	"vertex PresentOut present_vertex(uint id [[vertex_id]])\n"
	"{\n"
	"\tPresentOut out;\n"
	"\tfloat2 corner = float2(id & 1, id >> 1);\n"
	"\tout.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);\n"
	"\tout.coordinates = corner;\n"
	"\treturn out;\n"
	"}\n"
	"fragment float4 present_fragment(PresentOut in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"\tsampler linear [[sampler(0)]])\n"
	"{\n"
	"\treturn float4(picture.sample(linear, in.coordinates).rgb, 1.0);\n"
	"}\n";

static id<MTLRenderPipelineState> clear_pipeline(MTLPixelFormat color, MTLPixelFormat depth, uint32_t mask)
{
	NSNumber *key = @((color << 20) | (depth << 4) | mask);
	id<MTLRenderPipelineState> pipeline = clear_pipelines[key];

	if (!pipeline)
	{
		MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
		NSError *error = nil;

		descriptor.vertexFunction = clear_vertex;
		/* a fragment function that writes a color with no color attachment
		is rejected; depth and stencil need none */
		descriptor.fragmentFunction = color == MTLPixelFormatInvalid ? nil : clear_fragment;
		descriptor.colorAttachments[0].pixelFormat = color;
		descriptor.colorAttachments[0].writeMask = (mask & GPU_CHANNEL_RED ? MTLColorWriteMaskRed : 0) |
			(mask & GPU_CHANNEL_GREEN ? MTLColorWriteMaskGreen : 0) | (mask & GPU_CHANNEL_BLUE ? MTLColorWriteMaskBlue : 0) |
			(mask & GPU_CHANNEL_ALPHA ? MTLColorWriteMaskAlpha : 0);
		descriptor.depthAttachmentPixelFormat = depth;
		descriptor.stencilAttachmentPixelFormat = depth;
		pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
		if (!pipeline)
			host_fatal("Metal: cannot create the clear pipeline: %s", error.description.UTF8String);
		clear_pipelines[key] = pipeline;
	}
	return pipeline;
}

static id<MTLDepthStencilState> clear_depth_state(BOOL depth, BOOL stencil)
{
	NSNumber *key = @((depth ? 1 : 0) | (stencil ? 2 : 0));
	id<MTLDepthStencilState> state = clear_depth_states[key];

	if (!state)
	{
		MTLDepthStencilDescriptor *descriptor = [MTLDepthStencilDescriptor new];
		MTLStencilDescriptor *replace = [MTLStencilDescriptor new];

		descriptor.depthCompareFunction = MTLCompareFunctionAlways;
		descriptor.depthWriteEnabled = depth;
		if (stencil)
		{
			replace.stencilCompareFunction = MTLCompareFunctionAlways;
			replace.depthStencilPassOperation = MTLStencilOperationReplace;
			replace.writeMask = 0xff;
			descriptor.frontFaceStencil = replace;
			descriptor.backFaceStencil = replace;
		}
		state = [device newDepthStencilStateWithDescriptor:descriptor];
		clear_depth_states[key] = state;
	}
	return state;
}

/* rect clamped to a width x height target; NO if nothing is left */
static BOOL clamp_rect(const struct gpu_rect *rect, unsigned long width, unsigned long height, MTLScissorRect *clamped)
{
	long left = rect->x < 0 ? 0 : rect->x, top = rect->y < 0 ? 0 : rect->y;
	long right = (long)rect->x + rect->width, bottom = (long)rect->y + rect->height;

	if (right > (long)width)
		right = (long)width;
	if (bottom > (long)height)
		bottom = (long)height;
	if (right <= left || bottom <= top)
		return NO;
	*clamped = (MTLScissorRect){ (NSUInteger)left, (NSUInteger)top, (NSUInteger)(right - left), (NSUInteger)(bottom - top) };
	return YES;
}

/* each rectangle, as glClear inside glScissor does it: no viewport, depth
range, color mask (the clear's channel_mask instead) or visibility test */
static void gpu_metal_clear(const struct gpu_clear *clear, const struct gpu_rect *rectangles, uint32_t count)
{
	@autoreleasepool
	{
		uint32_t flags = clear->flags & (GPU_CLEAR_COLOR | (clear->depth_target ? GPU_CLEAR_DEPTH | GPU_CLEAR_STENCIL : 0));
		unsigned long width, height, index;
		uint32_t mask = flags & GPU_CLEAR_COLOR ? clear->channel_mask & 0xf : 0;
		MetalTexture *color = texture_record(clear->color_target), *depth = texture_record(clear->depth_target);
		float color_value[4], depth_value = clear->depth;

		frame_begin();
		if (!flags || !count || (!mask && !(flags & (GPU_CLEAR_DEPTH | GPU_CLEAR_STENCIL))))
			return;
		target_size(clear->color_target, clear->depth_target, &width, &height);
		/* a whole-target clear of every channel, before anything else in the
		pass, is the pass's load action */
		if (count == 1 && rectangles[0].x <= 0 && rectangles[0].y <= 0 &&
			(long)rectangles[0].x + rectangles[0].width >= (long)width &&
			(long)rectangles[0].y + rectangles[0].height >= (long)height &&
			(!(flags & GPU_CLEAR_COLOR) || mask == 0xf) &&
			!(encoder && pass_color == clear->color_target && pass_depth == clear->depth_target && pass_commands))
		{
			struct gpu_clear whole = *clear;

			whole.flags = flags;
			pass_begin(clear->color_target, clear->depth_target, &whole);
			return;
		}
		if (!pass_begin(clear->color_target, clear->depth_target, NULL))
			return;
		color_value[0] = (float)((clear->color >> 16) & 0xff) / 255.0f;
		color_value[1] = (float)((clear->color >> 8) & 0xff) / 255.0f;
		color_value[2] = (float)(clear->color & 0xff) / 255.0f;
		color_value[3] = (float)(clear->color >> 24) / 255.0f;
		[encoder setRenderPipelineState:clear_pipeline(color && color->texture ? color->texture.pixelFormat :
			MTLPixelFormatInvalid, depth && depth->texture ? depth->texture.pixelFormat : MTLPixelFormatInvalid, mask)];
		[encoder setDepthStencilState:clear_depth_state((flags & GPU_CLEAR_DEPTH) != 0, (flags & GPU_CLEAR_STENCIL) != 0)];
		[encoder setStencilReferenceValue:clear->stencil & 0xff];
		[encoder setCullMode:MTLCullModeNone];
		[encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
		[encoder setViewport:(MTLViewport){ 0.0, 0.0, (double)width, (double)height, 0.0, 1.0 }];
		[encoder setVertexBytes:&depth_value length:sizeof(depth_value) atIndex:0];
		[encoder setFragmentBytes:color_value length:sizeof(color_value) atIndex:0];
		for (index = 0; index < count; index++)
		{
			MTLScissorRect scissor;

			if (!clamp_rect(&rectangles[index], width, height, &scissor))
				continue;
			[encoder setScissorRect:scissor];
			[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			pass_commands++;
		}
	}
}

/* ---------- draws (Task 6) */

static uint32_t gpu_metal_draw(const struct gpu_draw *draw, const struct gpu_constant_store *constants,
	const struct gpu_uniforms *uniforms)
{
	return 0;
}

/* ---------- visibility tests (the a10 plan)

Every test answers "visible" until Metal's visibility results are wired up:
an unanswered test would make the game ask again forever. */

static void gpu_metal_visibility_begin(void)
{
}

static void gpu_metal_visibility_end(uint32_t slot)
{
}

static uint32_t gpu_metal_visibility_result(uint32_t slot, uint32_t *samples)
{
	*samples = 1;
	return 1;
}

/* ---------- frames */

/* the game's KickPushBuffer: committing here would split render passes */
static void gpu_metal_flush(void)
{
}

/* the back buffer letterboxed into the drawable, as gpu_gl.c's gpu_present,
without its vertical flip: GL's window shows row 0 at the bottom, Metal's
drawable at the top */
static void gpu_metal_present(gpu_texture back_buffer)
{
	@autoreleasepool
	{
		MetalTexture *record = texture_record(back_buffer);
		id<CAMetalDrawable> drawable;

		command_buffer();
		pass_end();
		drawable = [layer nextDrawable];
		/* nil in the background: the frame still commits, so its slot is
		freed */
		if (drawable && record && record->texture)
		{
			MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
			long window_width = (long)drawable.texture.width, window_height = (long)drawable.texture.height;
			long width = window_width, height = window_width * (long)record->description.height / (long)record->description.width;
			id<MTLRenderCommandEncoder> present;

			if (height > window_height)
			{
				height = window_height;
				width = window_height * (long)record->description.width / (long)record->description.height;
			}
			pass.colorAttachments[0].texture = drawable.texture;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
			present = [commands renderCommandEncoderWithDescriptor:pass];
			[present setRenderPipelineState:present_pipeline];
			[present setViewport:(MTLViewport){ (double)((window_width - width) / 2), (double)((window_height - height) / 2),
				(double)width, (double)height, 0.0, 1.0 }];
			[present setFragmentTexture:record->texture atIndex:0];
			[present setFragmentSamplerState:present_sampler atIndex:0];
			[present drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
			[present endEncoding];
			use_texture(record);
			[commands presentDrawable:drawable];
		}
		drawable = nil;
		commit(YES);
		frames++;
		if (frames % 600 == 0 && renames)
		{
			platform_log("Metal: %lu renames in the last 600 frames", renames);
			renames = 0;
		}
	}
}

static uint32_t gpu_metal_call_count_take(void)
{
	return 0;
}

/* ---------- initialization */

static void gpu_metal_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
	@autoreleasepool
	{
		NSError *error = nil;
		id<MTLLibrary> utilities;
		MTLRenderPipelineDescriptor *present_descriptor = [MTLRenderPipelineDescriptor new];
		MTLSamplerDescriptor *linear = [MTLSamplerDescriptor new];

		memset(capabilities, 0, sizeof(*capabilities));
		metal_debug = (flags & GPU_INITIALIZE_DEBUG) != 0;
		layer = (__bridge CAMetalLayer *)host_sdl_metal_layer();
		device = MTLCreateSystemDefaultDevice();
		if (!layer || !device)
			host_fatal("Metal is unavailable (layer %p, device %p)", (__bridge void *)layer, (__bridge void *)device);
		queue = [device newCommandQueue];
		/* SDL's view sets the layer's scale and drawableSize (from layoutSubviews) */
		layer.device = device;
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		layer.framebufferOnly = YES;
		textures = [MetalTable new];
		buffers = [MetalTable new];
		shaders = [MetalTable new];
		frame_slots = dispatch_semaphore_create(FRAMES);
		clear_pipelines = [NSMutableDictionary dictionary];
		clear_depth_states = [NSMutableDictionary dictionary];
		utilities = [device newLibraryWithSource:utility_source options:nil error:&error];
		if (!utilities)
			host_fatal("Metal: cannot compile the backend's own shaders: %s", error.description.UTF8String);
		clear_vertex = [utilities newFunctionWithName:@"clear_vertex"];
		clear_fragment = [utilities newFunctionWithName:@"clear_fragment"];
		present_vertex = [utilities newFunctionWithName:@"present_vertex"];
		present_fragment = [utilities newFunctionWithName:@"present_fragment"];
		present_descriptor.vertexFunction = present_vertex;
		present_descriptor.fragmentFunction = present_fragment;
		present_descriptor.colorAttachments[0].pixelFormat = layer.pixelFormat;
		present_pipeline = [device newRenderPipelineStateWithDescriptor:present_descriptor error:&error];
		if (!present_pipeline)
			host_fatal("Metal: cannot create the present pipeline: %s", error.description.UTF8String);
		linear.minFilter = MTLSamplerMinMagFilterLinear;
		linear.magFilter = MTLSamplerMinMagFilterLinear;
		present_sampler = [device newSamplerStateWithDescriptor:linear];
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
}

#define GPU_METAL_FUNCTION(type, name, parameters, arguments) .name = gpu_metal_##name,
#define GPU_METAL_PROCEDURE(name, parameters, arguments) .name = gpu_metal_##name,
const struct gpu_backend gpu_backend_metal = { GPU_FUNCTIONS(GPU_METAL_FUNCTION, GPU_METAL_PROCEDURE) };
