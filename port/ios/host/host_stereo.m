/* Head-tracked stereo (display.stereo = "head", visionOS 26 and later): the
Compositor's views drive the game's eyes, and the head drives its look.

At the game's frame begin, host_stereo_frame opens the Compositor's frame
(theater mode's, host_theater.m) and reads each view: its offset from the
device in meters, which becomes world units (one is 3.048 m), and its
frustum's tangents, recovered from the projection. The head's yaw and pitch
since the last frame turn the player's look (port/linux/game/stereo.c); its
roll only tilts the eye cameras. The game renders each eye, then
gpu_present_stereo (gpu_metal.m) hands the pictures here: each fills its view,
with the game's depth (already reverse-Z, gpu_metal.m's reversed_depth) for
the Compositor's reprojection, and the HUD floats head-locked in front.

Elsewhere host_stereo_frame leaves the frame mono. */
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include "host_stereo.h"
#include "host.h"
#include <math.h>
#include <string.h>

#if TARGET_OS_VISION
#import <CompositorServices/CompositorServices.h>
#import <Metal/Metal.h>
#include <simd/simd.h>
#include "host_theater.h"

/* one world unit in meters */
#define METERS_PER_UNIT 3.048f
/* the HUD's quad, head-locked: how far ahead and how wide, in meters */
#define HUD_DISTANCE 2.0f
#define HUD_WIDTH 1.6f
/* the Compositor's nearest near plane (the Phase 3 prototype's simulator
reading; cp_drawable_set_depth_range aborts on a nearer one) */
#define MINIMUM_NEAR_METERS 0.1f

/* the eyes' picture size, while the head drives the view */
static int picture_width, picture_height;
/* the head's last yaw, pitch and roll, while ARKit places it */
static BOOL head_known;
static float head_yaw, head_pitch, head_roll;
static unsigned long stereo_frames;

static id<MTLRenderPipelineState> eye_pipeline, hud_pipeline;
static MTLPixelFormat pipeline_color, pipeline_depth;
static id<MTLDepthStencilState> depth_always;
static id<MTLSamplerState> linear_sampler, nearest_sampler;

static NSString *const shader_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct picture_vertex { float4 position [[position]]; float2 coordinate; };\n"
	/* the eye's picture fills its view: its frustum is the view's */
	"vertex picture_vertex eye_vertex(uint index [[vertex_id]])\n"
	"{\n"
	"	float2 corner = float2(index & 1, index >> 1);\n"
	"	picture_vertex out;\n"
	"	out.position = float4(corner.x * 2 - 1, 1 - corner.y * 2, 0, 1);\n"
	"	out.coordinate = corner;\n"
	"	return out;\n"
	"}\n"
	"struct eye_pixel { float4 color [[color(0)]]; float depth [[depth(any)]]; };\n"
	/* the game's colors are display-encoded; an sRGB target encodes what it's
	given. Its depth is reverse-Z already, for its near and far planes, which
	the drawable's depth range is set to; scaled when the near plane had to
	move out to the Compositor's (stereo_depth_range) */
	"struct eye_uniforms { uint decode_srgb; float depth_scale; };\n"
	"fragment eye_pixel eye_fragment(picture_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	depth2d<float> depth [[texture(1)]], sampler linear [[sampler(0)]], sampler nearest [[sampler(1)]],\n"
	"	constant eye_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float3 color = picture.sample(linear, in.coordinate).rgb;\n"
	"	if (u.decode_srgb)\n"
	"		color = select(pow((color + 0.055) / 1.055, 2.4), color / 12.92, color <= 0.04045);\n"
	"	eye_pixel out;\n"
	"	out.color = float4(color, 1);\n"
	"	out.depth = min(depth.sample(nearest, in.coordinate) * u.depth_scale, 1.0);\n"
	"	return out;\n"
	"}\n"
	"struct hud_uniforms { float4x4 clip_from_hud; float2 half_size; uint decode_srgb; };\n"
	"vertex picture_vertex hud_vertex(uint index [[vertex_id]], constant hud_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float2 corner = float2(index & 1, index >> 1);\n"
	"	picture_vertex out;\n"
	"	out.position = u.clip_from_hud * float4((corner.x * 2 - 1) * u.half_size.x, (1 - corner.y * 2) * u.half_size.y, 0, 1);\n"
	"	out.coordinate = corner;\n"
	"	return out;\n"
	"}\n"
	/* The HUD layer clears to 0,0,0,0, so what the game draws into it comes
	out premultiplied: an alpha-blended draw leaves its color times its
	alpha, an additive one (text, glows) its color with alpha 0. Over the
	world, then, it's the color plus the world times one minus alpha. Where
	it covers the world, its depth is the quad's, so the Compositor
	reprojects it as the quad it is */
	"fragment float4 hud_fragment(picture_vertex in [[stage_in]], texture2d<float> hud [[texture(0)]],\n"
	"	sampler linear [[sampler(0)]], constant hud_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float4 color = hud.sample(linear, in.coordinate);\n"
	"	if (max(color.a, max(color.r, max(color.g, color.b))) < 1.0 / 255.0)\n"
	"		discard_fragment();\n"
	"	if (u.decode_srgb)\n"
	"		color.rgb = select(pow((color.rgb + 0.055) / 1.055, 2.4), color.rgb / 12.92, color.rgb <= 0.04045);\n"
	"	return color;\n"
	"}\n";

struct eye_uniforms
{
	uint32_t decode_srgb;
	float depth_scale;
};

struct hud_uniforms
{
	simd_float4x4 clip_from_hud;
	simd_float2 half_size;
	uint32_t decode_srgb;
};

static BOOL prepare(id<MTLDevice> device, MTLPixelFormat color, MTLPixelFormat depth)
{
	if (eye_pipeline && pipeline_color == color && pipeline_depth == depth)
		return YES;
	NSError *error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:shader_source options:nil error:&error];
	if (!library)
	{
		host_logf(HOST_LOG_ERROR, "stereo: the presenter's shaders didn't compile: %s", error.description.UTF8String);
		return NO;
	}
	MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
	descriptor.vertexFunction = [library newFunctionWithName:@"eye_vertex"];
	descriptor.fragmentFunction = [library newFunctionWithName:@"eye_fragment"];
	descriptor.colorAttachments[0].pixelFormat = color;
	descriptor.depthAttachmentPixelFormat = depth;
	if (depth == MTLPixelFormatDepth32Float_Stencil8)
		descriptor.stencilAttachmentPixelFormat = depth;
	eye_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	descriptor.vertexFunction = [library newFunctionWithName:@"hud_vertex"];
	descriptor.fragmentFunction = [library newFunctionWithName:@"hud_fragment"];
	descriptor.colorAttachments[0].blendingEnabled = YES;
	descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
	descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	/* the view stays opaque under the HUD */
	descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
	descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	if (eye_pipeline)
		hud_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (!eye_pipeline || !hud_pipeline)
	{
		host_logf(HOST_LOG_ERROR, "stereo: the presenter's pipelines failed: %s", error.description.UTF8String);
		eye_pipeline = hud_pipeline = nil;
		return NO;
	}
	pipeline_color = color;
	pipeline_depth = depth;
	/* the eyes write the game's depth as it is; the HUD draws over them,
	depth test off */
	MTLDepthStencilDescriptor *depth_descriptor = [MTLDepthStencilDescriptor new];
	depth_descriptor.depthCompareFunction = MTLCompareFunctionAlways;
	depth_descriptor.depthWriteEnabled = YES;
	depth_always = [device newDepthStencilStateWithDescriptor:depth_descriptor];
	MTLSamplerDescriptor *sampler_descriptor = [MTLSamplerDescriptor new];
	sampler_descriptor.sAddressMode = sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
	nearest_sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
	sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
	sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
	linear_sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
	return YES;
}

/* The drawable's depth range for the game's planes, and the factor its depth
takes. Reverse-Z depth with a far plane f is n (f - z) / (z (f - n)): for a
near plane the Compositor allows, n' (no nearer than MINIMUM_NEAR_METERS),
it's the game's times n' (f - n) / (n (f - n')), and what's nearer than n'
pins to 1. Without usable planes (none, or the far not beyond the near) the
Compositor keeps its default range and the depth is written as far: the
picture is reprojected as if distant. */
static simd_float2 stereo_depth_range(float near_meters, float far_meters, float *scale)
{
	float near = fmaxf(near_meters, MINIMUM_NEAR_METERS);
	static int reported;

	if (!(near_meters > 0.0f) || !(far_meters > near * 1.001f) || !isfinite(far_meters))
	{
		if (!reported++)
			host_logf(HOST_LOG_WARN, "stereo: depth range %.3f to %.1f m isn't usable; depth is written as far",
				near_meters, far_meters);
		*scale = 0.0f;
		return (simd_float2){ 0.0f, 0.0f };
	}
	*scale = near * (far_meters - near_meters) / (near_meters * (far_meters - near));
	return (simd_float2){ far_meters, near };
}

/* angle wrapped into -pi..pi */
static float wrapped(float angle)
{
	while (angle > (float)M_PI)
		angle -= 2.0f * (float)M_PI;
	while (angle < -(float)M_PI)
		angle += 2.0f * (float)M_PI;
	return angle;
}

/* the head's turn since the last frame and its roll now, from the device's
pose in the room (ARKit's axes: x right, y up, z back) */
static void head_turn(struct halo_stereo_frame *frame, simd_float4x4 origin_from_device)
{
	simd_float3 right = origin_from_device.columns[0].xyz;
	simd_float3 up = origin_from_device.columns[1].xyz;
	simd_float3 forward = -origin_from_device.columns[2].xyz;
	/* yaw about the room's up, left positive, as the game's yaw; pitch up
	positive; roll left ear down positive */
	float yaw = atan2f(-forward.x, -forward.z);
	float pitch = asinf(simd_clamp(forward.y, -1.0f, 1.0f));
	float roll = atan2f(right.y, up.y);

	if (head_known)
	{
		frame->head_yaw = wrapped(yaw - head_yaw);
		frame->head_pitch = pitch - head_pitch;
	}
	frame->head_roll = roll;
	head_yaw = yaw;
	head_pitch = pitch;
	head_roll = roll;
	head_known = YES;
}

static void stereo_frame(struct halo_stereo_frame *frame) API_AVAILABLE(visionos(26.0))
{
	simd_float4x4 origin_from_device;
	int anchored;
	size_t views;
	int eye;

	if (frame->mode != HALO_STEREO_HEAD || !host_theater_frame_begin(1))
	{
		head_known = NO;
		picture_width = picture_height = 0;
		return;
	}
	cp_drawable_t drawable = host_theater_drawable(0, &origin_from_device, &anchored);
	views = cp_drawable_get_view_count(drawable);
	if (views == 0)
		return;
	/* the simulator has one view: both eyes are it */
	for (eye = 0; eye < 2; eye++)
	{
		size_t view_index = (size_t)eye < views ? (size_t)eye : views - 1;
		cp_view_t view = cp_drawable_get_view(drawable, view_index);
		simd_float4x4 device_from_view = cp_view_get_transform(view);
		simd_float4x4 projection = cp_drawable_compute_projection(drawable,
			cp_axis_direction_convention_right_up_back, view_index);
		struct halo_stereo_eye *e = &frame->eyes[eye];

		/* right, up and back, as the device's axes and the eye's offset are */
		e->offset[0] = device_from_view.columns[3].x / METERS_PER_UNIT;
		e->offset[1] = device_from_view.columns[3].y / METERS_PER_UNIT;
		e->offset[2] = device_from_view.columns[3].z / METERS_PER_UNIT;
		/* an off-axis projection's x scale is 2 / (left + right) and its
		x offset (right - left) / (left + right); likewise y */
		e->right = (1.0f + projection.columns[2].x) / projection.columns[0].x;
		e->left = (1.0f - projection.columns[2].x) / projection.columns[0].x;
		e->up = (1.0f + projection.columns[2].y) / projection.columns[1].y;
		e->down = (1.0f - projection.columns[2].y) / projection.columns[1].y;
		/* once: the tangents beside the projection they come from, to check
		the signs on a device (cp_view_get_tangents, the other source, aborts
		with __BUG_IN_CLIENT__ when an app built with this SDK calls it) */
		if (stereo_frames == 0)
			host_logf(HOST_LOG_INFO, "stereo: view %zu of %zu: offset %.4f %.4f %.4f m, forward %.3f %.3f %.3f; "
				"tangents left %.3f right %.3f up %.3f down %.3f from the projection's x scale %.4f, y scale %.4f, "
				"x offset %.4f, y offset %.4f", view_index, views, device_from_view.columns[3].x,
				device_from_view.columns[3].y, device_from_view.columns[3].z, -device_from_view.columns[2].x,
				-device_from_view.columns[2].y, -device_from_view.columns[2].z, e->left, e->right, e->up, e->down,
				projection.columns[0].x, projection.columns[1].y, projection.columns[2].x, projection.columns[2].y);
	}
	{
		MTLViewport viewport = cp_view_texture_map_get_viewport(
			cp_view_get_view_texture_map(cp_drawable_get_view(drawable, 0)));

		/* even, as the picture size the game renders at (host_stereo_picture_size) */
		frame->eye_width = (int32_t)viewport.width & ~1;
		frame->eye_height = (int32_t)viewport.height & ~1;
		picture_width = frame->eye_width;
		picture_height = frame->eye_height;
	}
	/* without ARKit's pose (the simulator, or a lost anchor) the head holds
	its last pose: no turn */
	if (anchored)
		head_turn(frame, origin_from_device);
	else
		frame->head_roll = head_known ? head_roll : 0.0f;
	frame->eye_count = 2;
	if (stereo_frames++ == 0)
		host_logf(HOST_LOG_INFO, "stereo: the head drives the view; %zu view%s of %dx%d, %s", views,
			views == 1 ? "" : "s", frame->eye_width, frame->eye_height,
			anchored ? "ARKit places the head" : "no device anchor (the head holds still)");
}
#endif

void host_stereo_frame(struct halo_stereo_frame *frame)
{
	/* no eyes and no turn unless this frame supplies them, whatever the guest
	left in the struct: a turn replayed from an earlier frame would spin the
	view while ARKit has lost the head */
	frame->eye_count = 0;
	frame->head_yaw = 0.0f;
	frame->head_pitch = 0.0f;
	frame->head_roll = 0.0f;
	frame->eye_width = 0;
	frame->eye_height = 0;
#if TARGET_OS_VISION
	if (@available(visionOS 26.0, *))
		stereo_frame(frame);
#endif
}

int host_stereo_picture_size(int *width, int *height)
{
#if TARGET_OS_VISION
	if (picture_width > 0 && picture_height > 0)
	{
		*width = picture_width;
		*height = picture_height;
		return 1;
	}
#endif
	(void)width;
	(void)height;
	return 0;
}

int host_stereo_ready(void)
{
#if TARGET_OS_VISION
	return host_theater_frame_ready();
#else
	return 0;
#endif
}

void host_stereo_present(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right,
	id<MTLTexture> left_depth, id<MTLTexture> right_depth, id<MTLTexture> hud,
	float near_meters, float far_meters)
{
#if TARGET_OS_VISION
	if (@available(visionOS 26.0, *))
	{
		id<MTLTexture> colors[2] = { left, right }, depths[2] = { left_depth, right_depth };
		size_t count = host_theater_drawable_count();
		float depth_scale;
		simd_float2 depth_range = stereo_depth_range(near_meters, far_meters, &depth_scale);
		static unsigned long presents;

		if (presents++ == 0)
			host_logf(HOST_LOG_INFO, "stereo: first present: eyes %lux%lu, %s, depth range %.3f to %.1f m, "
				"%zu drawable%s", (unsigned long)left.width, (unsigned long)left.height,
				hud ? "a HUD" : "no HUD", near_meters, far_meters, count, count == 1 ? "" : "s");

		for (size_t index = 0; index < count; index++)
		{
			cp_drawable_t drawable = host_theater_drawable(index, NULL, NULL);
			id<MTLCommandBuffer> commands;
			size_t views;

			/* reverse-Z: far first; the projections follow it */
			if (depth_range.x > 0.0f)
				cp_drawable_set_depth_range(drawable, depth_range);
			if (!prepare(queue.device, cp_drawable_get_color_texture(drawable, 0).pixelFormat,
				cp_drawable_get_depth_texture(drawable, 0).pixelFormat))
			{
				host_theater_frame_end();
				return;
			}
			commands = [queue commandBuffer];
			views = cp_drawable_get_view_count(drawable);
			for (size_t view_index = 0; view_index < views; view_index++)
			{
				cp_view_t view = cp_drawable_get_view(drawable, view_index);
				cp_view_texture_map_t map = cp_view_get_view_texture_map(view);
				size_t texture = cp_view_texture_map_get_texture_index(map);
				size_t slice = cp_view_texture_map_get_slice_index(map);
				id<MTLTexture> color = cp_drawable_get_color_texture(drawable, texture);
				id<MTLTexture> depth = cp_drawable_get_depth_texture(drawable, texture);
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				/* view 0 is the left eye */
				int eye = view_index == 0 ? 0 : 1;
				uint32_t decode_srgb = color.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ||
					color.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB;
				struct eye_uniforms eye_uniforms = { decode_srgb, depth_scale };

				pass.colorAttachments[0].texture = color;
				pass.colorAttachments[0].slice = slice;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
				pass.depthAttachment.texture = depth;
				pass.depthAttachment.slice = slice;
				pass.depthAttachment.loadAction = MTLLoadActionClear;
				pass.depthAttachment.storeAction = MTLStoreActionStore;
				pass.depthAttachment.clearDepth = 0.0;
				if (depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
				{
					pass.stencilAttachment.texture = depth;
					pass.stencilAttachment.slice = slice;
					pass.stencilAttachment.loadAction = MTLLoadActionClear;
					pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
				}
				if (color.textureType == MTLTextureType2DArray)
					pass.renderTargetArrayLength = 1;
				id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
				[encoder setViewport:cp_view_texture_map_get_viewport(map)];
				[encoder setDepthStencilState:depth_always];
				[encoder setRenderPipelineState:eye_pipeline];
				[encoder setFragmentTexture:colors[eye] atIndex:0];
				[encoder setFragmentTexture:depths[eye] atIndex:1];
				[encoder setFragmentSamplerState:linear_sampler atIndex:0];
				[encoder setFragmentSamplerState:nearest_sampler atIndex:1];
				[encoder setFragmentBytes:&eye_uniforms length:sizeof(eye_uniforms) atIndex:0];
				[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
				if (hud)
				{
					struct hud_uniforms uniforms;
					simd_float4x4 projection = cp_drawable_compute_projection(drawable,
						cp_axis_direction_convention_right_up_back, view_index);
					simd_float4x4 device_from_hud = matrix_identity_float4x4;

					/* head-locked: in the device's frame, ahead of it */
					device_from_hud.columns[3] = (simd_float4){ 0.0f, 0.0f, -HUD_DISTANCE, 1.0f };
					uniforms.clip_from_hud = simd_mul(projection,
						simd_mul(simd_inverse(cp_view_get_transform(view)), device_from_hud));
					uniforms.half_size = (simd_float2){ HUD_WIDTH / 2.0f,
						HUD_WIDTH / 2.0f * (float)hud.height / (float)hud.width };
					uniforms.decode_srgb = decode_srgb;
					[encoder setRenderPipelineState:hud_pipeline];
					[encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
					[encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
					[encoder setFragmentTexture:hud atIndex:0];
					[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
				}
				[encoder endEncoding];
			}
			cp_drawable_encode_present(drawable, commands);
			[commands commit];
		}
		host_theater_frame_end();
	}
#else
	(void)queue;
	(void)left;
	(void)right;
	(void)left_depth;
	(void)right_depth;
	(void)hud;
	(void)near_meters;
	(void)far_meters;
#endif
}
