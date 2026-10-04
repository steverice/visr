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

Stereo on the theater screen (display.stereo = "screen") reads the same frame
for the eyes' positions only: the look stays on the stick, and the head moves
each eye's frustum through the screen, as through a window (screen_eyes).
host_theater_present_eyes puts each eye's picture on the screen for its view.

Elsewhere host_stereo_frame leaves the frame mono. */
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include "host_stereo.h"
#include "host_config.h"
#include "host.h"
#include <math.h>
#include <string.h>

#if TARGET_OS_VISION
#import <CompositorServices/CompositorServices.h>
#import <Metal/Metal.h>
#include <simd/simd.h>
#include "host_theater.h"
#include "host_stereo_head.h"
#include "host_stereo_vignette.h"

/* one world unit in meters */
#define METERS_PER_UNIT 3.048f
/* the HUD's quad, head-locked: how far ahead and how wide, in meters */
#define HUD_DISTANCE 2.0f
#define HUD_WIDTH 1.6f

/* stereo on the screen: the nearest an eye may be to the screen's plane,
and how far toward its edges an eye's offset may go, as a share of the
screen's half width or height, so the frusta stay valid when the screen is
seen nearly edge-on (where the picture then stops following the eye) */
#define SCREEN_MINIMUM_DISTANCE 0.25f
#define SCREEN_EDGE_SHARE 0.9f

/* the eyes' picture size, while the head drives the view */
static int picture_width, picture_height;
/* the head's last pose, while ARKit places it */
static struct host_stereo_head head;
static unsigned long stereo_frames;
/* frames on the screen, presents, and whether the depth range warning was
logged, since the space opened: the once-only logs repeat for each opening
(host_stereo_space_opened) */
static unsigned long screen_frames, stereo_presents;
static int depth_reported;

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
	move out to the Compositor's (stereo_depth_range). Never 0: the Compositor
	takes depth 0 for nothing there (black, or the room), and the sky, the
	clear and what only transparent effects drew all reach it as 0, so they
	take the depth of something half way to the far plane. The comfort
	vignette (input.comfort_vignette, host_stereo_vignette.h) darkens the
	color only, toward black away from straight ahead, alike in both eyes,
	and never its depth */
	HOST_STEREO_VIGNETTE_STRING(HOST_STEREO_VIGNETTE_SOURCE) "\n"
	"struct eye_uniforms { uint decode_srgb; float depth_scale; float depth_floor; float brightness; float vignette;\n"
	"	float4 tangents; float vignette_inner; float vignette_outer; };\n"
	"fragment eye_pixel eye_fragment(picture_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	depth2d<float> depth [[texture(1)]], sampler linear [[sampler(0)]], sampler nearest [[sampler(1)]],\n"
	"	constant eye_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float3 color = picture.sample(linear, in.coordinate).rgb;\n"
	"	if (u.decode_srgb)\n"
	"		color = select(pow((color + 0.055) / 1.055, 2.4), color / 12.92, color <= 0.04045);\n"
	"	float edge = u.vignette > 0.0 ? host_stereo_vignette_edge(in.coordinate.x, in.coordinate.y, u.tangents.x,\n"
	"		u.tangents.y, u.tangents.z, u.tangents.w, u.vignette_inner, u.vignette_outer) : 0.0;\n"
	"	eye_pixel out;\n"
	"	out.color = float4(color * u.brightness * (1.0 - u.vignette * edge), 1);\n"
	"	out.depth = clamp(depth.sample(nearest, in.coordinate) * u.depth_scale, u.depth_floor, 1.0);\n"
	"	return out;\n"
	"}\n"
	"struct hud_uniforms { float4x4 clip_from_hud; float2 half_size; uint decode_srgb; float brightness; };\n"
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
	"	return float4(color.rgb * u.brightness, color.a);\n"
	"}\n";

/* the eye depth's floor when the game's planes aren't usable: the
Compositor's default range is about 0.1 m to infinity, where this is about
1 km away */
#define STEREO_DEPTH_FLOOR_DEFAULT 0.0001f

#if TARGET_OS_VISION
/* a view's frustum tangents (left, right, up, down, all positive) from its
projection: an off-axis projection's x scale is 2 / (left + right) and its
x offset (right - left) / (left + right); likewise y */
static simd_float4 view_tangents(cp_drawable_t drawable, size_t view_index) API_AVAILABLE(visionos(26.0))
{
	simd_float4x4 projection = cp_drawable_compute_projection(drawable,
		cp_axis_direction_convention_right_up_back, view_index);

	return (simd_float4){
		(1.0f - projection.columns[2].x) / projection.columns[0].x,
		(1.0f + projection.columns[2].x) / projection.columns[0].x,
		(1.0f + projection.columns[2].y) / projection.columns[1].y,
		(1.0f - projection.columns[2].y) / projection.columns[1].y,
	};
}
#endif

struct eye_uniforms
{
	uint32_t decode_srgb;
	float depth_scale;
	float depth_floor;
	float brightness;
	float vignette;
	/* the view's frustum: left, right, up, down tangents; the vignette's
	inner and outer angles in radians, the same for both eyes */
	simd_float4 tangents;
	float vignette_inner, vignette_outer;
};

struct hud_uniforms
{
	simd_float4x4 clip_from_hud;
	simd_float2 half_size;
	uint32_t decode_srgb;
	float brightness;
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
near plane the Compositor allows, n' (no nearer than the layer's minimum,
host_theater_minimum_near; cp_drawable_set_depth_range aborts on a nearer one),
it's the game's times n' (f - n) / (n (f - n')), and what's nearer than n'
pins to 1. Without usable planes (none, or the far not beyond the near) the
Compositor keeps its default range and the depth is written as far: the
picture is reprojected as if distant. The floor is the depth half way to the
far plane, n' / (f - n'): far enough to reproject as distant, and not 0, which
the Compositor takes for nothing there; without usable planes, a small depth
in the Compositor's default range. The floor also hides a game depth that is
wrongly empty (static geometry without depth, headset session 1): it would
reproject as if half way to the far plane rather than vanish, so watch the
game's own empty share instead (d3d8_device.c, debug.gpu_stats). A far plane
nearer than twice the near would put the floor above 1; it is kept below. */
static simd_float2 stereo_depth_range(float near_meters, float far_meters, float *scale, float *floor)
{
	float near = fmaxf(near_meters, host_theater_minimum_near());

	if (!(near_meters > 0.0f) || !(far_meters > near * 1.001f) || !isfinite(far_meters))
	{
		if (!depth_reported++)
			host_logf(HOST_LOG_WARN, "stereo: depth range %.3f to %.1f m isn't usable; depth is written as far",
				near_meters, far_meters);
		*scale = 0.0f;
		*floor = STEREO_DEPTH_FLOOR_DEFAULT;
		return (simd_float2){ 0.0f, 0.0f };
	}
	*scale = near * (far_meters - near_meters) / (near_meters * (far_meters - near));
	*floor = fminf(near / (far_meters - near), 0.5f);
	return (simd_float2){ far_meters, near };
}

/* the head's turn since the last frame and its roll now, from the device's
pose in the room (host_stereo_head.c) */
static void head_turn(struct halo_stereo_frame *frame, simd_float4x4 origin_from_device)
{
	float right[3] = { origin_from_device.columns[0].x, origin_from_device.columns[0].y, origin_from_device.columns[0].z };
	float up[3] = { origin_from_device.columns[1].x, origin_from_device.columns[1].y, origin_from_device.columns[1].z };
	float back[3] = { origin_from_device.columns[2].x, origin_from_device.columns[2].y, origin_from_device.columns[2].z };

	host_stereo_head_turn(&head, right, up, back, frame);
}

/* Stereo on the screen: the eyes from where the viewer's eyes are in the
screen's frame (its center the origin, x right, y up, z toward the viewer).

The screen is a window, and the game's camera sits in it: the screen's
center is the player's eye in the game, and the game's world lies behind
the screen at its true scale (one world unit is 3.048 m). An eye at
(ex, ey, d) in meters, d in front of the screen, is therefore offset from
the game's camera by (ex, ey, d) / 3.048 world units right, up and back,
and its frustum passes through the screen's edges:
	left = (half_width + ex) / d     right = (half_width - ex) / d
	up = (half_height - ey) / d      down = (half_height + ey) / d
So the eye's picture fills the screen exactly as that eye sees it, and what
the game shows at a distance z ahead of its camera appears z behind the
screen: a point straight ahead on the screen's surface has no parallax, and
the parallax rises toward the eyes' separation with distance (at 64 mm and
a 4 m screen, a quarter of it 1.3 m behind the screen, half of it 4 m
behind). Nothing in front of the game's camera comes out of the screen; the
eye loop moves each eye's near plane out to the screen (render.c), so what
is behind the game's camera, between it and the eye, isn't drawn. The game's
field of view becomes the screen's angle (display.theater_width), and
leaning or stepping moves the frusta as a window would. The orientation of
the head plays no part. */
static void screen_eyes(struct halo_stereo_frame *frame, cp_drawable_t drawable, simd_float4x4 origin_from_device,
	int anchored) API_AVAILABLE(visionos(26.0))
{
	simd_float4x4 origin_from_screen, screen_from_device;
	simd_float2 half_size;
	size_t views = cp_drawable_get_view_count(drawable);
	int width, height;
	int eye;
	static int screen_stats = -1;

	if (views == 0)
		return;
	if (screen_stats < 0)
	{
		char value[16];

		host_config_string("debug.gpu_stats", "false", value, sizeof(value));
		screen_stats = !strcmp(value, "true");
	}
	host_theater_screen(0, &origin_from_screen, &half_size);
	screen_from_device = simd_mul(simd_inverse(origin_from_screen), origin_from_device);
	/* the simulator has one view: both eyes are it */
	for (eye = 0; eye < 2; eye++)
	{
		size_t view_index = (size_t)eye < views ? (size_t)eye : views - 1;
		simd_float4x4 device_from_view = cp_view_get_transform(cp_drawable_get_view(drawable, view_index));
		simd_float4 position = simd_mul(screen_from_device, device_from_view.columns[3]);
		float ex = simd_clamp(position.x, -SCREEN_EDGE_SHARE * half_size.x, SCREEN_EDGE_SHARE * half_size.x);
		float ey = simd_clamp(position.y, -SCREEN_EDGE_SHARE * half_size.y, SCREEN_EDGE_SHARE * half_size.y);
		float d = fmaxf(position.z, SCREEN_MINIMUM_DISTANCE);
		struct halo_stereo_eye *e = &frame->eyes[eye];

		e->offset[0] = ex / METERS_PER_UNIT;
		e->offset[1] = ey / METERS_PER_UNIT;
		e->offset[2] = d / METERS_PER_UNIT;
		e->left = (half_size.x + ex) / d;
		e->right = (half_size.x - ex) / d;
		e->up = (half_size.y - ey) / d;
		e->down = (half_size.y + ey) / d;
		if (screen_frames == 0)
			host_logf(HOST_LOG_INFO, "stereo: on the screen, view %zu of %zu: the eye at %.4f %.4f %.4f m from "
				"the screen's center (%.2f by %.2f m); tangents left %.3f right %.3f up %.3f down %.3f",
				view_index, views, position.x, position.y, position.z, 2.0f * half_size.x, 2.0f * half_size.y,
				e->left, e->right, e->up, e->down);
		/* debug.gpu_stats: where the left eye is against the screen every
		few seconds, to check that leaning moves the eyes (the window's
		parallax), and whether ARKit placed the head */
		if (eye == 0 && screen_stats && screen_frames % 270 == 0)
			host_logf(HOST_LOG_INFO, "stereo: on the screen, frame %lu: the left eye at %.3f %.3f %.3f m from the "
				"screen's center, %s", screen_frames, position.x, position.y, position.z,
				anchored ? "ARKit places the head" : "no device anchor (the eyes hold still)");
	}
	/* the screen's picture size and shape: the eyes' frusta have its shape */
	host_theater_picture_size(&width, &height);
	frame->eye_width = width;
	frame->eye_height = height;
	frame->eye_count = 2;
	if (screen_frames++ == 0)
		host_logf(HOST_LOG_INFO, "stereo: the game is in stereo on the screen; %zu view%s, eyes %dx%d", views,
			views == 1 ? "" : "s", width, height);
}

static void stereo_frame(struct halo_stereo_frame *frame) API_AVAILABLE(visionos(26.0))
{
	simd_float4x4 origin_from_device;
	int anchored;
	size_t views;
	int eye;

	if ((frame->mode != HALO_STEREO_HEAD && frame->mode != HALO_STEREO_SCREEN) || !host_theater_frame_begin(1))
	{
		head.known = 0;
		picture_width = picture_height = 0;
		return;
	}
	cp_drawable_t drawable = host_theater_drawable(0, &origin_from_device, &anchored);
	/* on the screen, the head moves only the eyes: no turn, and the game
	renders at the screen's picture size (host_theater_picture_size) */
	if (frame->mode == HALO_STEREO_SCREEN)
	{
		head.known = 0;
		picture_width = picture_height = 0;
		screen_eyes(frame, drawable, origin_from_device, anchored);
		return;
	}
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
		host_stereo_head_hold(&head, frame);
	frame->eye_count = 2;
	if (stereo_frames++ == 0)
		host_logf(HOST_LOG_INFO, "stereo: the head drives the view; %zu view%s of %dx%d, %s", views,
			views == 1 ? "" : "s", frame->eye_width, frame->eye_height,
			anchored ? "ARKit places the head" : "no device anchor (the head holds still)");
}
#endif

void host_stereo_space_opened(void)
{
#if TARGET_OS_VISION
	stereo_frames = 0;
	screen_frames = 0;
	stereo_presents = 0;
	depth_reported = 0;
#endif
}

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
	id<MTLTexture> left_depth, id<MTLTexture> right_depth, id<MTLTexture> hud, float hud_aspect,
	float near_meters, float far_meters, float brightness, float vignette)
{
#if TARGET_OS_VISION
	if (@available(visionOS 26.0, *))
	{
		id<MTLTexture> colors[2] = { left, right }, depths[2] = { left_depth, right_depth };
		size_t count = host_theater_drawable_count();
		float depth_scale, depth_floor;
		simd_float2 depth_range = stereo_depth_range(near_meters, far_meters, &depth_scale, &depth_floor);

		if (stereo_presents++ == 0)
			host_logf(HOST_LOG_INFO, "stereo: first present: eyes %lux%lu, %s (laid out at %.3f:1, a %.2f by %.2f m "
				"quad %.1f m ahead), depth range %.3f to %.1f m, %zu drawable%s", (unsigned long)left.width,
				(unsigned long)left.height, hud ? "a HUD" : "no HUD", hud_aspect, HUD_WIDTH,
				hud_aspect > 0.0f ? HUD_WIDTH / hud_aspect : 0.0f, HUD_DISTANCE, near_meters, far_meters, count,
				count == 1 ? "" : "s");

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
			/* the comfort vignette's angles, the same for every view: full at
			the nearest edge of any view's frustum */
			float vignette_outer = 0.0f;
			for (size_t view_index = 0; view_index < views; view_index++)
			{
				simd_float4 tangents = view_tangents(drawable, view_index);
				float nearest = atanf(fminf(fminf(tangents.x, tangents.y), fminf(tangents.z, tangents.w)));

				if (view_index == 0 || nearest < vignette_outer)
					vignette_outer = nearest;
			}
			if (!(vignette_outer > 0.0f))
				vignette = 0.0f;
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
				struct eye_uniforms eye_uniforms = { decode_srgb, depth_scale, depth_floor, brightness,
					fmaxf(0.0f, fminf(1.0f, vignette)), view_tangents(drawable, view_index),
					HOST_STEREO_VIGNETTE_CLEAR_SHARE * vignette_outer, vignette_outer };

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
					/* at the shape the game lays the HUD out in: its texture is
					the eyes' size, whose pixels needn't be square */
					uniforms.half_size = (simd_float2){ HUD_WIDTH / 2.0f, HUD_WIDTH / 2.0f /
						(hud_aspect > 0.0f ? hud_aspect : (float)hud.width / (float)hud.height) };
					uniforms.decode_srgb = decode_srgb;
					uniforms.brightness = brightness;
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
	(void)hud_aspect;
	(void)near_meters;
	(void)far_meters;
	(void)brightness;
#endif
}
