/* Theater mode (display.immersive, visionOS 26 and later): the game's
picture on a screen standing in the room, drawn into an immersive space's
Compositor Services layer (Theater.swift opens it). The game renders as it does
in the window; gpu_present hands the finished picture here instead of to the
window's drawable. Each eye sees the screen through its own pose and
projection, so the room reads in stereo while the picture is flat; the screen
is fixed in the room (ARKit's device anchor), placed in front of the player
where they first look when the space opens. Without a device anchor (the
simulator) the screen follows the head. */
#import <ARKit/ARKit.h>
#import <CompositorServices/CompositorServices.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include <simd/simd.h>
#include "host_theater.h"
#include "host.h"

/* the screen: its width in meters, and how far in front of the player it stands */
#define SCREEN_WIDTH 2.0f
#define SCREEN_DISTANCE 2.5f

static cp_layer_renderer_t layer_renderer;
static ar_session_t session;
static ar_world_tracking_provider_t world_tracking;
static BOOL screen_placed;
static simd_float4x4 origin_from_screen;
static id<MTLRenderPipelineState> pipeline;
static MTLPixelFormat pipeline_color, pipeline_depth;
static id<MTLDepthStencilState> depth_state;
static id<MTLSamplerState> sampler;
static unsigned long theater_frames;

void host_theater_log_c(const char *message)
{
	host_logf(HOST_LOG_INFO, "%s", message);
}

/* hides or shows the game's window (SDL's), which would stand in front of
the screen while the space is open: its frames go to the screen meanwhile */
static void window_hidden(BOOL hidden)
{
	for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
	{
		if (![scene isKindOfClass:UIWindowScene.class])
			continue;
		for (UIWindow *window in ((UIWindowScene *)scene).windows)
			window.hidden = hidden;
	}
}

void host_theater_attach(void *renderer)
{
	layer_renderer = (__bridge cp_layer_renderer_t)renderer;
	screen_placed = NO;
	window_hidden(YES);
	host_logf(HOST_LOG_INFO, "theater: the immersive space is open");
	if (!session && ar_world_tracking_provider_is_supported())
	{
		ar_world_tracking_configuration_t configuration = ar_world_tracking_configuration_create();

		world_tracking = ar_world_tracking_provider_create(configuration);
		session = ar_session_create();
		ar_session_run(session, ar_data_providers_create_with_data_providers(world_tracking, nil));
	}
	else if (!session)
		host_logf(HOST_LOG_INFO, "theater: no world tracking here; the screen follows the head");
}

int host_theater_active(void)
{
	if (!layer_renderer)
		return 0;
	if (cp_layer_renderer_get_state(layer_renderer) == cp_layer_renderer_state_invalidated)
	{
		host_logf(HOST_LOG_INFO, "theater: the immersive space closed; back to the window");
		layer_renderer = nil;
		window_hidden(NO);
		return 0;
	}
	return 1;
}

static NSString *const shader_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct screen_uniforms { float4x4 clip_from_screen; float2 half_size; uint decode_srgb; };\n"
	"struct screen_vertex { float4 position [[position]]; float2 coordinate; };\n"
	"vertex screen_vertex theater_vertex(uint index [[vertex_id]], constant screen_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float2 corner = float2(index & 1, index >> 1);\n"
	"	screen_vertex out;\n"
	"	out.position = u.clip_from_screen * float4((corner.x * 2 - 1) * u.half_size.x, (1 - corner.y * 2) * u.half_size.y, 0, 1);\n"
	"	out.coordinate = corner;\n"
	"	return out;\n"
	"}\n"
	"fragment float4 theater_fragment(screen_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	sampler linear [[sampler(0)]], constant screen_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float3 color = picture.sample(linear, in.coordinate).rgb;\n"
	/* the game's colors are display-encoded; an sRGB target encodes what it's given */
	"	if (u.decode_srgb)\n"
	"		color = select(pow((color + 0.055) / 1.055, 2.4), color / 12.92, color <= 0.04045);\n"
	"	return float4(color, 1);\n"
	"}\n";

struct screen_uniforms
{
	simd_float4x4 clip_from_screen;
	simd_float2 half_size;
	uint32_t decode_srgb;
};

static BOOL prepare(id<MTLDevice> device, MTLPixelFormat color, MTLPixelFormat depth)
{
	if (pipeline && pipeline_color == color && pipeline_depth == depth)
		return YES;
	NSError *error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:shader_source options:nil error:&error];
	if (!library)
	{
		host_logf(HOST_LOG_ERROR, "theater: the screen's shaders didn't compile: %s", error.description.UTF8String);
		return NO;
	}
	MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
	descriptor.vertexFunction = [library newFunctionWithName:@"theater_vertex"];
	descriptor.fragmentFunction = [library newFunctionWithName:@"theater_fragment"];
	descriptor.colorAttachments[0].pixelFormat = color;
	descriptor.depthAttachmentPixelFormat = depth;
	if (depth == MTLPixelFormatDepth32Float_Stencil8)
		descriptor.stencilAttachmentPixelFormat = depth;
	pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (!pipeline)
	{
		host_logf(HOST_LOG_ERROR, "theater: the screen's pipeline failed: %s", error.description.UTF8String);
		return NO;
	}
	pipeline_color = color;
	pipeline_depth = depth;
	MTLDepthStencilDescriptor *depth_descriptor = [MTLDepthStencilDescriptor new];
	/* the Compositor's projections are reverse-Z */
	depth_descriptor.depthCompareFunction = MTLCompareFunctionGreater;
	depth_descriptor.depthWriteEnabled = YES;
	depth_state = [device newDepthStencilStateWithDescriptor:depth_descriptor];
	MTLSamplerDescriptor *sampler_descriptor = [MTLSamplerDescriptor new];
	sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
	sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
	sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
	return YES;
}

/* stands the screen in front of where the device looks, upright and facing it */
static void place_screen(simd_float4x4 origin_from_device)
{
	simd_float3 position = origin_from_device.columns[3].xyz;
	simd_float3 forward = -origin_from_device.columns[2].xyz;
	float yaw;

	forward.y = 0.0f;
	if (simd_length(forward) < 1e-3f)
		forward = (simd_float3){ 0.0f, 0.0f, -1.0f };
	forward = simd_normalize(forward);
	yaw = atan2f(-forward.x, -forward.z);
	origin_from_screen = (simd_float4x4){ {
		{ cosf(yaw), 0.0f, -sinf(yaw), 0.0f },
		{ 0.0f, 1.0f, 0.0f, 0.0f },
		{ sinf(yaw), 0.0f, cosf(yaw), 0.0f },
		{ position.x + forward.x * SCREEN_DISTANCE, position.y, position.z + forward.z * SCREEN_DISTANCE, 1.0f },
	} };
	screen_placed = YES;
	host_logf(HOST_LOG_INFO, "theater: the screen stands %.1f m ahead, %.1f m wide", SCREEN_DISTANCE, SCREEN_WIDTH);
}

void host_theater_present(id<MTLCommandQueue> queue, id<MTLTexture> picture)
{
	if (@available(visionOS 26.0, *))
	{
		if (!host_theater_active() || cp_layer_renderer_get_state(layer_renderer) != cp_layer_renderer_state_running)
			return;
		/* waits for the Compositor's next frame, which paces the game to it */
		cp_frame_t frame = cp_layer_renderer_query_next_frame(layer_renderer);
		if (!frame)
			return;
		cp_frame_timing_t timing = cp_frame_predict_timing(frame);
		if (!timing)
			return;
		cp_frame_start_update(frame);
		cp_frame_end_update(frame);
		cp_frame_start_submission(frame);
		cp_drawable_array_t drawables = cp_frame_query_drawables(frame);
		size_t count = cp_drawable_array_get_count(drawables);
		for (size_t index = 0; index < count; index++)
		{
			cp_drawable_t drawable = cp_drawable_array_get_drawable(drawables, index);
			simd_float4x4 origin_from_device = matrix_identity_float4x4;
			BOOL anchored = NO;

			if (world_tracking)
			{
				ar_device_anchor_t anchor = ar_device_anchor_create();
				CFTimeInterval when = cp_time_to_cf_time_interval(cp_drawable_get_frame_timing(drawable) ?
					cp_frame_timing_get_presentation_time(cp_drawable_get_frame_timing(drawable)) :
					cp_frame_timing_get_presentation_time(timing));

				if (ar_world_tracking_provider_query_device_anchor_at_timestamp(world_tracking, when, anchor) ==
					ar_device_anchor_query_status_success)
				{
					origin_from_device = ar_anchor_get_origin_from_anchor_transform(anchor);
					cp_drawable_set_device_anchor(drawable, anchor);
					anchored = YES;
				}
			}
			if (!screen_placed || (!anchored && !world_tracking))
				place_screen(origin_from_device);
			id<MTLTexture> first = cp_drawable_get_color_texture(drawable, 0);
			if (!prepare(queue.device, first.pixelFormat, cp_drawable_get_depth_texture(drawable, 0).pixelFormat))
				return;
			id<MTLCommandBuffer> commands = [queue commandBuffer];
			size_t views = cp_drawable_get_view_count(drawable);
			for (size_t view_index = 0; view_index < views; view_index++)
			{
				cp_view_t view = cp_drawable_get_view(drawable, view_index);
				cp_view_texture_map_t map = cp_view_get_view_texture_map(view);
				size_t texture = cp_view_texture_map_get_texture_index(map);
				id<MTLTexture> color = cp_drawable_get_color_texture(drawable, texture);
				id<MTLTexture> depth = cp_drawable_get_depth_texture(drawable, texture);
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				struct screen_uniforms uniforms;
				simd_float4x4 origin_from_view = simd_mul(origin_from_device, cp_view_get_transform(view));
				simd_float4x4 projection = cp_drawable_compute_projection(drawable,
					cp_axis_direction_convention_right_up_back, view_index);

				pass.colorAttachments[0].texture = color;
				pass.colorAttachments[0].slice = cp_view_texture_map_get_slice_index(map);
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
				pass.depthAttachment.texture = depth;
				pass.depthAttachment.slice = cp_view_texture_map_get_slice_index(map);
				pass.depthAttachment.loadAction = MTLLoadActionClear;
				pass.depthAttachment.storeAction = MTLStoreActionStore;
				pass.depthAttachment.clearDepth = 0.0;
				if (depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
				{
					pass.stencilAttachment.texture = depth;
					pass.stencilAttachment.slice = cp_view_texture_map_get_slice_index(map);
					pass.stencilAttachment.loadAction = MTLLoadActionClear;
					pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
				}
				if (color.textureType == MTLTextureType2DArray)
					pass.renderTargetArrayLength = 1;
				uniforms.clip_from_screen = simd_mul(projection,
					simd_mul(simd_inverse(origin_from_view), origin_from_screen));
				uniforms.half_size = (simd_float2){ SCREEN_WIDTH / 2.0f,
					SCREEN_WIDTH / 2.0f * (float)picture.height / (float)picture.width };
				uniforms.decode_srgb = color.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ||
					color.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB;
				id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
				[encoder setViewport:cp_view_texture_map_get_viewport(map)];
				[encoder setRenderPipelineState:pipeline];
				[encoder setDepthStencilState:depth_state];
				[encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
				[encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
				[encoder setFragmentTexture:picture atIndex:0];
				[encoder setFragmentSamplerState:sampler atIndex:0];
				[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
				[encoder endEncoding];
			}
			cp_drawable_encode_present(drawable, commands);
			[commands commit];
		}
		cp_frame_end_submission(frame);
		if (theater_frames++ == 0)
			host_logf(HOST_LOG_INFO, "theater: first frame on the screen (%zu drawable%s)", count, count == 1 ? "" : "s");
	}
}
