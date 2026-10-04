/* Theater mode (display.immersive, visionOS 26 and later): the game's
picture on a screen standing in the room, drawn into an immersive space's
Compositor Services layer (Theater.swift opens it). The game renders as it does
in the window; gpu_present hands the finished picture here instead of to the
window's drawable. Each eye sees the screen through its own pose and
projection, so the room reads in stereo while the picture is flat; the screen
is fixed in the room (ARKit's device anchor), placed in front of the player
where they first look when the space opens. Without a device anchor (the
simulator) the screen follows the head. The Compositor's frame is shared with
head-tracked stereo (host_stereo.m), which opens it at the game's frame begin
to read the eyes' poses and presents it full view. */
#import <ARKit/ARKit.h>
#import <CompositorServices/CompositorServices.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include <simd/simd.h>
#include "host_config.h"
#include "host_theater.h"
#include "host.h"
#include <math.h>
#include <string.h>

/* about what the Vision Pro's displays resolve at the center of the view: a
picture with more pixels per degree than this is wasted work (MetalFX's cost
grows with its output) */
#define PIXELS_PER_DEGREE 40.0
/* the screen's shape: the game's widescreen layout */
#define SCREEN_ASPECT (16.0 / 9.0)

/* config.toml's display.theater_*: the screen's angle across, in degrees, and
its distance and width in meters; whether it stands in the dark */
static double screen_degrees = 60.0;
static float screen_distance = 4.0f, screen_width = 4.6f;
static int environment_dark;

static cp_layer_renderer_t layer_renderer;
static ar_session_t session;
static ar_world_tracking_provider_t world_tracking;
static BOOL screen_placed;
static simd_float4x4 origin_from_screen;
static id<MTLRenderPipelineState> pipeline, hud_pipeline;
static MTLPixelFormat pipeline_color, pipeline_depth;
static id<MTLDepthStencilState> depth_state;
static id<MTLSamplerState> sampler;
static unsigned long theater_frames;

/* The Compositor's frame, open from host_theater_frame_begin (at the game's
frame begin in head-tracked stereo, host_stereo.m; else at the present) to
host_theater_frame_end after the present, with each drawable's device anchor
queried at its presentation time */
#define MAXIMUM_DRAWABLES 4
static cp_frame_t open_frame;
static size_t open_count;
static cp_drawable_t open_drawables[MAXIMUM_DRAWABLES];
static simd_float4x4 open_origin_from_device[MAXIMUM_DRAWABLES];
static BOOL open_anchored[MAXIMUM_DRAWABLES];

/* forgets the open frame without ending it: the space closed under it */
static void frame_drop(void)
{
	open_frame = NULL;
	open_count = 0;
}

/* the layer's nearest allowed near plane (Theater.swift), 0 until it says */
static float minimum_near;

void host_theater_set_minimum_near(float meters)
{
	minimum_near = meters;
	host_logf(HOST_LOG_INFO, "theater: the nearest near plane the layer allows is %.3f m", meters);
}

float host_theater_minimum_near(void)
{
	return minimum_near > 0.0f ? minimum_near : 0.1f;
}

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

void host_theater_load_settings(void)
{
	char environment[32];

	screen_degrees = host_config_real("display.theater_width", 60.0);
	if (screen_degrees < 20.0)
		screen_degrees = 20.0;
	if (screen_degrees > 100.0)
		screen_degrees = 100.0;
	screen_distance = (float)host_config_real("display.theater_distance", 4.0);
	if (screen_distance < 1.0f)
		screen_distance = 1.0f;
	screen_width = 2.0f * screen_distance * tanf((float)(screen_degrees * M_PI / 360.0));
	host_config_string("display.theater_environment", "passthrough", environment, sizeof(environment));
	environment_dark = !strcmp(environment, "dark");
	host_logf(HOST_LOG_INFO, "theater: a %.0f-degree screen (%.1f m wide) %.1f m away, %s around it",
		screen_degrees, screen_width, screen_distance, environment_dark ? "dark" : "the room");
}

int host_theater_dark(void)
{
	return environment_dark;
}

void host_theater_picture_size(int *width, int *height)
{
	*width = (int)lround(screen_degrees * PIXELS_PER_DEGREE) & ~1;
	*height = (int)lround(*width / SCREEN_ASPECT) & ~1;
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
		frame_drop();
		layer_renderer = nil;
		window_hidden(NO);
		return 0;
	}
	return 1;
}

static NSString *const shader_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct screen_uniforms { float4x4 clip_from_screen; float2 half_size; uint decode_srgb; float brightness; };\n"
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
	"	return float4(color * u.brightness, 1);\n"
	"}\n"
	/* stereo on the screen: the HUD over the eye's picture, both display-
	encoded, as the game would have blended it into its back buffer. The HUD
	layer clears to 0,0,0,0, so what the game draws into it is premultiplied
	(host_stereo.m) */
	"fragment float4 theater_hud_fragment(screen_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	texture2d<float> hud [[texture(1)]], sampler linear [[sampler(0)]], constant screen_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float4 over = hud.sample(linear, in.coordinate);\n"
	"	float3 color = saturate(over.rgb + picture.sample(linear, in.coordinate).rgb * (1 - over.a));\n"
	"	if (u.decode_srgb)\n"
	"		color = select(pow((color + 0.055) / 1.055, 2.4), color / 12.92, color <= 0.04045);\n"
	"	return float4(color * u.brightness, 1);\n"
	"}\n";

struct screen_uniforms
{
	simd_float4x4 clip_from_screen;
	simd_float2 half_size;
	uint32_t decode_srgb;
	float brightness;
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
	descriptor.fragmentFunction = [library newFunctionWithName:@"theater_hud_fragment"];
	if (pipeline)
		hud_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (!pipeline || !hud_pipeline)
	{
		host_logf(HOST_LOG_ERROR, "theater: the screen's pipeline failed: %s", error.description.UTF8String);
		pipeline = hud_pipeline = nil;
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
		{ position.x + forward.x * screen_distance, position.y, position.z + forward.z * screen_distance, 1.0f },
	} };
	screen_placed = YES;
}

/* the screen's pose for the open frame's drawable: placed in front of the
device the first time, and every frame where nothing tracks the room */
static simd_float4x4 screen_pose(size_t index)
{
	if (!screen_placed || (!open_anchored[index] && !world_tracking))
		place_screen(open_origin_from_device[index]);
	return origin_from_screen;
}

void host_theater_screen(size_t index, simd_float4x4 *pose, simd_float2 *half_size)
{
	int width, height;

	host_theater_picture_size(&width, &height);
	*pose = screen_pose(index);
	*half_size = (simd_float2){ screen_width / 2.0f, screen_width / 2.0f * (float)height / (float)width };
}

int host_theater_frame_begin(int fresh)
{
	if (@available(visionOS 26.0, *))
	{
		if (open_frame && !fresh)
			return 1;
		/* a frame that began and never presented (a game frame that drew
		nothing): ended unpresented, so its old anchors aren't handed out */
		if (open_frame)
		{
			static unsigned long stale;

			if (stale++ == 0)
				host_logf(HOST_LOG_INFO, "theater: a frame began and never presented; it's ended unpresented");
			host_theater_frame_end();
		}
		if (!host_theater_active() || cp_layer_renderer_get_state(layer_renderer) != cp_layer_renderer_state_running)
			return 0;
		/* waits for the Compositor's next frame, which paces the game to it */
		cp_frame_t frame = cp_layer_renderer_query_next_frame(layer_renderer);
		if (!frame)
			return 0;
		cp_frame_timing_t timing = cp_frame_predict_timing(frame);
		if (!timing)
			return 0;
		cp_frame_start_update(frame);
		cp_frame_end_update(frame);
		/* the poses are freshest at the optimal input time */
		cp_time_wait_until(cp_frame_timing_get_optimal_input_time(timing));
		cp_frame_start_submission(frame);
		cp_drawable_array_t drawables = cp_frame_query_drawables(frame);
		size_t count = cp_drawable_array_get_count(drawables);
		/* none: the frame was canceled and is discarded */
		if (count == 0)
			return 0;
		if (count > MAXIMUM_DRAWABLES)
			count = MAXIMUM_DRAWABLES;
		for (size_t index = 0; index < count; index++)
		{
			cp_drawable_t drawable = cp_drawable_array_get_drawable(drawables, index);

			open_drawables[index] = drawable;
			open_origin_from_device[index] = matrix_identity_float4x4;
			open_anchored[index] = NO;
			if (world_tracking)
			{
				ar_device_anchor_t anchor = ar_device_anchor_create();
				CFTimeInterval when = cp_time_to_cf_time_interval(cp_drawable_get_frame_timing(drawable) ?
					cp_frame_timing_get_presentation_time(cp_drawable_get_frame_timing(drawable)) :
					cp_frame_timing_get_presentation_time(timing));

				if (ar_world_tracking_provider_query_device_anchor_at_timestamp(world_tracking, when, anchor) ==
					ar_device_anchor_query_status_success)
				{
					open_origin_from_device[index] = ar_anchor_get_origin_from_anchor_transform(anchor);
					cp_drawable_set_device_anchor(drawable, anchor);
					open_anchored[index] = YES;
				}
			}
		}
		open_frame = frame;
		open_count = count;
		return 1;
	}
	return 0;
}

int host_theater_frame_ready(void)
{
	if (!open_frame)
		return 0;
	/* the space closing mid-frame: the frame is dropped, and
	host_theater_active returns the game to the window */
	if (!host_theater_active() || cp_layer_renderer_get_state(layer_renderer) != cp_layer_renderer_state_running)
	{
		frame_drop();
		return 0;
	}
	return 1;
}

size_t host_theater_drawable_count(void)
{
	return open_count;
}

cp_drawable_t host_theater_drawable(size_t index, simd_float4x4 *origin_from_device, int *anchored)
{
	if (origin_from_device)
		*origin_from_device = open_origin_from_device[index];
	if (anchored)
		*anchored = open_anchored[index];
	return open_drawables[index];
}

void host_theater_frame_end(void)
{
	if (open_frame)
		cp_frame_end_submission(open_frame);
	frame_drop();
}

/* the pictures on the screen for the open frame (or the next one): view 0's
the left one, the others the right; the HUD, if any, over each; the
pictures at a brightness */
static void present_pictures(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right,
	id<MTLTexture> hud, float brightness)
{
	if (@available(visionOS 26.0, *))
	{
		/* the frame stereo opened at the game's frame begin (in mono, when
		this frame presents mono after all: a menu, a load), or the next one */
		if (!host_theater_frame_begin(0) || !host_theater_frame_ready())
			return;
		size_t count = open_count;
		for (size_t index = 0; index < count; index++)
		{
			cp_drawable_t drawable = open_drawables[index];
			simd_float4x4 origin_from_device = open_origin_from_device[index];

			/* placed now unless stereo on the screen placed it at the frame's
			begin, to put the eyes' frusta through it */
			screen_pose(index);
			id<MTLTexture> first = cp_drawable_get_color_texture(drawable, 0);
			if (!prepare(queue.device, first.pixelFormat, cp_drawable_get_depth_texture(drawable, 0).pixelFormat))
			{
				/* the submission has started: end it, as host_stereo_present does */
				host_theater_frame_end();
				return;
			}
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
				id<MTLTexture> picture = view_index == 0 ? left : right;
				struct screen_uniforms uniforms;
				simd_float4x4 origin_from_view = simd_mul(origin_from_device, cp_view_get_transform(view));
				simd_float4x4 projection = cp_drawable_compute_projection(drawable,
					cp_axis_direction_convention_right_up_back, view_index);

				pass.colorAttachments[0].texture = color;
				pass.colorAttachments[0].slice = cp_view_texture_map_get_slice_index(map);
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				uint32_t decode_srgb = color.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ||
					color.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB;

				pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, environment_dark ? 1.0 : 0.0);
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
				uniforms.half_size = (simd_float2){ screen_width / 2.0f,
					screen_width / 2.0f * (float)picture.height / (float)picture.width };
				uniforms.decode_srgb = decode_srgb;
				uniforms.brightness = brightness;
				id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
				[encoder setViewport:cp_view_texture_map_get_viewport(map)];
				[encoder setRenderPipelineState:hud ? hud_pipeline : pipeline];
				[encoder setDepthStencilState:depth_state];
				[encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
				[encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
				[encoder setFragmentTexture:picture atIndex:0];
				if (hud)
					[encoder setFragmentTexture:hud atIndex:1];
				[encoder setFragmentSamplerState:sampler atIndex:0];
				[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
				[encoder endEncoding];
			}
			cp_drawable_encode_present(drawable, commands);
			[commands commit];
		}
		host_theater_frame_end();
		if (left == right && theater_frames++ == 0)
			host_logf(HOST_LOG_INFO, "theater: first frame on the screen (%zu drawable%s)", count, count == 1 ? "" : "s");
	}
}

void host_theater_present(id<MTLCommandQueue> queue, id<MTLTexture> picture)
{
	present_pictures(queue, picture, picture, nil, 1.0f);
}

void host_theater_present_eyes(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right,
	id<MTLTexture> hud, float brightness)
{
	static unsigned long presents;

	if (presents++ == 0)
		host_logf(HOST_LOG_INFO, "theater: first stereo frame on the screen: eyes %lux%lu, %s",
			(unsigned long)left.width, (unsigned long)left.height, hud ? "a HUD" : "no HUD");
	present_pictures(queue, left, right, hud, brightness);
}
