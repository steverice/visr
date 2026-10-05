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
the Compositor's reprojection, and the HUD's pieces float in front of it
(host_stereo_hud.h): the reticle where the crosshair points, the rest in
bands that turn with the head's yaw only, level with the room. A frame
without eyes (the main menu, a load) shows its picture on the UI's quad, the
same way.

Stereo on the theater screen (display.stereo = "screen") reads the same frame
for the eyes' positions only: the look stays on the stick, and screen_eyes
reports where the viewer's eyes are against the screen, from which the guest
maps the game as a 3D TV (stereo.c). host_theater_present_eyes puts each
eye's picture on the screen for its view.

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
#include "host_stereo_hud.h"

/* one world unit in meters */
#define METERS_PER_UNIT 3.048f

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
static unsigned long screen_frames, stereo_presents, ui_presents;
/* the last frame's HUD went whole on the UI's quad: -1 none yet since the
space opened */
static int ui_shown = -1;
/* the last frame showed the zoom's inset */
static int inset_logged;
static int depth_reported;
/* the Compositor's frame is open for this game frame (host_stereo_frame)
with display.stereo = "head", whatever the frame is (the full view, the film
on the screen, or no eyes) */
static int head_frame_open;

/* display.stereo = "head", read once */
static int head_configured(void)
{
	static int configured = -1;

	if (configured < 0)
	{
		char value[16];

		host_config_string("display.stereo", "off", value, sizeof(value));
		configured = !strcmp(value, "head");
	}
	return configured;
}

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
	/* one of the HUD's quads (host_stereo_hud.h): its texture's rectangle,
	and its center and half extents in its frame, which clip_from_frame
	takes to the view */
	"struct hud_uniforms { float4x4 clip_from_frame; float4 source; float4 center; float4 x_axis; float4 y_axis;\n"
	"	uint decode_srgb; float brightness; uint opaque; };\n"
	"vertex picture_vertex hud_vertex(uint index [[vertex_id]], constant hud_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float2 corner = float2(index & 1, index >> 1);\n"
	"	picture_vertex out;\n"
	"	out.position = u.clip_from_frame * float4(u.center.xyz + (corner.x * 2 - 1) * u.x_axis.xyz +\n"
	"		(1 - corner.y * 2) * u.y_axis.xyz, 1);\n"
	"	out.coordinate = mix(u.source.xy, u.source.zw, corner);\n"
	"	return out;\n"
	"}\n"
	/* The HUD layer holds premultiplied color and, in alpha, how much of the
	picture still shows (d3d8_device.c, hud_layer_blend): 1 where nothing
	drew. Over the world it's the color plus the world times that, which the
	blend makes of one minus it. Where nothing drew the quad leaves the world
	and its depth; elsewhere its depth is the quad's, so the Compositor
	reprojects it as the quad it is. A mono picture on the UI's quad is
	opaque (its alpha is the game's scratch) */
	"fragment float4 hud_fragment(picture_vertex in [[stage_in]], texture2d<float> hud [[texture(0)]],\n"
	"	sampler linear [[sampler(0)]], constant hud_uniforms &u [[buffer(0)]])\n"
	"{\n"
	/* only inside the quad's rectangle: linear filtering at its edge would
	take a sliver of what's beside it */
	"	float2 half_texel = 0.5 / float2(hud.get_width(), hud.get_height());\n"
	"	float4 color = hud.sample(linear, clamp(in.coordinate, u.source.xy + half_texel, u.source.zw - half_texel));\n"
	"	float covered = u.opaque ? 1.0 : 1.0 - color.a;\n"
	"	if (max(covered, max(color.r, max(color.g, color.b))) < 1.0 / 255.0)\n"
	"		discard_fragment();\n"
	"	if (u.decode_srgb)\n"
	"		color.rgb = select(pow((color.rgb + 0.055) / 1.055, 2.4), color.rgb / 12.92, color.rgb <= 0.04045);\n"
	"	return float4(color.rgb * u.brightness, covered);\n"
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

/* Foveation's measurement (display.foveation): the Compositor's rate maps,
logged once per opening and whenever they change, since Apple publishes no
screen size, physical size or density for any quality. Per map: its
layers, screen size, granularity and each layer's physical size; per view:
its map and layer, the logical viewport, the drawable's texture size, its
tangents and the density at its center (the screen's pixels per degree
there, times the map's physical-to-screen ratio). Then, per map and layer,
five fixed screen points mapped to physical ones (whether a layered map's
layers are identical), and per view the rate along its center row and
column, sampled every FOVEATION_PROFILE_STEP pixels, with how far from the
center, in degrees, the rate stays at least FOVEATION_SHARP_RATE */
#define FOVEATION_PROFILE_STEP 64.0f
#define FOVEATION_SHARP_RATE 0.9f
/* while the runtime quality eases toward the configured one, a short line
at most this often, in frames; after the limit (a quality the layer never
quite reaches), the full log regardless */
#define FOVEATION_EASING_FRAMES 45
#define FOVEATION_EASING_LIMIT 450

static NSString *foveation_logged_key;
static unsigned long foveation_frames, foveation_easing_logged;

/* a screen point through a map's layer */
static MTLCoordinate2D foveation_physical(id<MTLRasterizationRateMap> map, NSUInteger layer, float x, float y)
{
	return [map mapScreenToPhysicalCoordinates:MTLCoordinate2DMake(x, y) forLayer:layer];
}

static float degrees_from_tangent(float tangent)
{
	return atanf(fabsf(tangent)) * 180.0f / (float)M_PI;
}

/* the rates along a line of a view, from start to end (screen pixels along
one axis, the other fixed), sampled every FOVEATION_PROFILE_STEP pixels:
each sample's rate (physical pixels per screen pixel) into rates, with the
sample's start position; returns the count */
static int foveation_profile(id<MTLRasterizationRateMap> map, NSUInteger layer, BOOL along_x, float fixed,
	float start, float end, float *rates, float *positions, int capacity)
{
	int count = 0;
	float position = start;

	while (position < end - 0.5f && count < capacity)
	{
		float next = fminf(position + FOVEATION_PROFILE_STEP, end);
		MTLCoordinate2D a = along_x ? foveation_physical(map, layer, position, fixed) :
			foveation_physical(map, layer, fixed, position);
		MTLCoordinate2D b = along_x ? foveation_physical(map, layer, next, fixed) :
			foveation_physical(map, layer, fixed, next);

		rates[count] = ((along_x ? b.x - a.x : b.y - a.y)) / (next - position);
		positions[count] = position;
		count++;
		position = next;
	}
	return count;
}

/* from the sample holding center outward (direction -1 or 1), the farthest
screen position at which the rate is still at least the threshold; center
when the center's own sample is below it */
static float foveation_sharp_edge(const float *rates, const float *positions, int count, float end, float center,
	int direction, float threshold)
{
	int index = 0;
	float edge = center;

	while (index + 1 < count && positions[index + 1] <= center)
		index++;
	for (; index >= 0 && index < count && rates[index] >= threshold; index += direction)
		edge = direction > 0 ? (index + 1 < count ? positions[index + 1] : end) : positions[index];
	return edge;
}

static void foveation_measure(cp_drawable_t drawable) API_AVAILABLE(visionos(26.0))
{
	float quality, runtime, default_quality;
	const char *layout, *offered;
	size_t map_count = cp_drawable_get_rasterization_rate_map_count(drawable);
	size_t views = cp_drawable_get_view_count(drawable);
	NSMutableString *key, *line;

	if (!host_theater_foveation_state(&quality, &runtime, &default_quality, &layout, &offered))
		return;
	foveation_frames++;
	/* easing toward the configured quality: a short line now and then */
	if (fabsf(runtime - quality) > 0.005f && foveation_frames < FOVEATION_EASING_LIMIT)
	{
		if (foveation_easing_logged == 0 || foveation_frames - foveation_easing_logged >= FOVEATION_EASING_FRAMES)
		{
			id<MTLRasterizationRateMap> first = map_count ? cp_drawable_get_rasterization_rate_map(drawable, 0) : nil;
			MTLSize physical = first ? [first physicalSizeForLayer:0] : (MTLSize){ 0, 0, 0 };

			host_logf(HOST_LOG_INFO, "stereo: foveation easing: runtime quality %.3f toward %.3f, map 0's physical "
				"layer 0 %lux%lu", runtime, quality, (unsigned long)physical.width, (unsigned long)physical.height);
			foveation_easing_logged = foveation_frames;
		}
		return;
	}
	key = [NSMutableString stringWithFormat:@"%.2f %.2f %zu %zu", quality, runtime, map_count, views];
	line = [NSMutableString stringWithFormat:@"stereo: foveation at quality %.3f (runtime %.3f, default %.3f), "
		"layout %s (offered %s): %zu map%s for %zu view%s", quality, runtime, default_quality, layout, offered, map_count,
		map_count == 1 ? "" : "s", views, views == 1 ? "" : "s"];
	for (size_t index = 0; index < map_count; index++)
	{
		id<MTLRasterizationRateMap> map = cp_drawable_get_rasterization_rate_map(drawable, index);
		MTLSize screen = map.screenSize, granularity = map.physicalGranularity;

		[line appendFormat:@"; map %zu: %lu layer%s, screen %lux%lu, granularity %lux%lu", index,
			(unsigned long)map.layerCount, map.layerCount == 1 ? "" : "s", (unsigned long)screen.width,
			(unsigned long)screen.height, (unsigned long)granularity.width, (unsigned long)granularity.height];
		[key appendFormat:@" %lux%lu", (unsigned long)screen.width, (unsigned long)screen.height];
		for (NSUInteger layer = 0; layer < map.layerCount; layer++)
		{
			MTLSize physical = [map physicalSizeForLayer:layer];

			[line appendFormat:@", physical layer %lu %lux%lu", (unsigned long)layer, (unsigned long)physical.width,
				(unsigned long)physical.height];
			[key appendFormat:@" %lux%lu", (unsigned long)physical.width, (unsigned long)physical.height];
		}
	}
	for (size_t view_index = 0; view_index < views; view_index++)
	{
		cp_view_texture_map_t texture_map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, view_index));
		size_t texture = cp_view_texture_map_get_texture_index(texture_map);
		MTLViewport viewport = cp_view_texture_map_get_viewport(texture_map);
		id<MTLTexture> color = cp_drawable_get_color_texture(drawable, texture);
		id<MTLRasterizationRateMap> map = host_theater_view_rate_map(drawable, texture_map);
		size_t slice = cp_view_texture_map_get_slice_index(texture_map);
		NSUInteger layer = map && slice < map.layerCount ? slice : 0;
		simd_float4 t = view_tangents(drawable, view_index);
		float center_x = (float)viewport.originX + (float)viewport.width * t.x / (t.x + t.y);
		float center_y = (float)viewport.originY + (float)viewport.height * t.z / (t.z + t.w);
		float ratio_x = 1.0f, ratio_y = 1.0f;

		if (map)
		{
			MTLCoordinate2D a = foveation_physical(map, layer, center_x - 4.0f, center_y);
			MTLCoordinate2D b = foveation_physical(map, layer, center_x + 4.0f, center_y);
			MTLCoordinate2D c = foveation_physical(map, layer, center_x, center_y - 4.0f);
			MTLCoordinate2D d = foveation_physical(map, layer, center_x, center_y + 4.0f);

			ratio_x = (b.x - a.x) / 8.0f;
			ratio_y = (d.y - c.y) / 8.0f;
		}
		[line appendFormat:@"; view %zu (map %zu layer %lu, texture %zu slice %zu): viewport (%.0f %.0f %.0f %.0f), "
			"drawable texture %lux%lu, view tangents %.3f %.3f %.3f %.3f, center %.1f px/degree (rate %.3f by %.3f; "
			"%.1f px/degree across the view)", view_index,
			map ? (texture < map_count ? texture : 0) : 0, (unsigned long)layer, texture, slice, viewport.originX,
			viewport.originY, viewport.width, viewport.height, (unsigned long)color.width, (unsigned long)color.height,
			t.x, t.y, t.z, t.w, (float)viewport.width / (t.x + t.y) * (float)M_PI / 180.0f * ratio_x, ratio_x,
			ratio_y, (float)viewport.width / (degrees_from_tangent(t.x) + degrees_from_tangent(t.y))];
		[key appendFormat:@" (%.0f %.0f %.0f %.0f) %lux%lu", viewport.originX, viewport.originY, viewport.width,
			viewport.height, (unsigned long)color.width, (unsigned long)color.height];
	}
	if (foveation_logged_key && [key isEqualToString:foveation_logged_key])
		return;
	foveation_logged_key = key;
	host_logf(HOST_LOG_INFO, "%s", line.UTF8String);
	/* the same five screen points through every layer of every map */
	for (size_t index = 0; index < map_count; index++)
	{
		id<MTLRasterizationRateMap> map = cp_drawable_get_rasterization_rate_map(drawable, index);
		float width = (float)map.screenSize.width, height = (float)map.screenSize.height;
		const float points[5][2] = { { 0.5f, 0.5f }, { 0.1f, 0.1f }, { 0.9f, 0.1f }, { 0.1f, 0.9f }, { 0.9f, 0.9f } };

		for (NSUInteger layer = 0; layer < map.layerCount; layer++)
		{
			NSMutableString *text = [NSMutableString stringWithFormat:@"stereo: foveation map %zu layer %lu: points "
				"(screen -> physical)", index, (unsigned long)layer];

			for (int point = 0; point < 5; point++)
			{
				float x = points[point][0] * width, y = points[point][1] * height;
				MTLCoordinate2D physical = foveation_physical(map, layer, x, y);

				[text appendFormat:@"%s (%.1f %.1f) -> (%.2f %.2f)", point ? "," : "", x, y, physical.x, physical.y];
			}
			host_logf(HOST_LOG_INFO, "%s", text.UTF8String);
		}
	}
	/* each view's profile through its map and layer */
	for (size_t view_index = 0; view_index < views; view_index++)
	{
		enum { CAPACITY = 128 };
		cp_view_texture_map_t texture_map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, view_index));
		id<MTLRasterizationRateMap> map = host_theater_view_rate_map(drawable, texture_map);
		size_t texture = cp_view_texture_map_get_texture_index(texture_map);
		size_t slice = cp_view_texture_map_get_slice_index(texture_map);
		MTLViewport viewport = cp_view_texture_map_get_viewport(texture_map);
		simd_float4 t = view_tangents(drawable, view_index);
		float x0 = (float)viewport.originX, y0 = (float)viewport.originY;
		float x1 = x0 + (float)viewport.width, y1 = y0 + (float)viewport.height;
		float center_x = x0 + (float)viewport.width * t.x / (t.x + t.y);
		float center_y = y0 + (float)viewport.height * t.z / (t.z + t.w);
		float row[CAPACITY], row_at[CAPACITY], column[CAPACITY], column_at[CAPACITY];
		int row_count, column_count;
		NSUInteger layer;
		NSMutableString *text;

		if (!map)
			continue;
		layer = slice < map.layerCount ? slice : 0;
		row_count = foveation_profile(map, layer, YES, center_y, x0, x1, row, row_at, CAPACITY);
		column_count = foveation_profile(map, layer, NO, center_x, y0, y1, column, column_at, CAPACITY);
		text = [NSMutableString stringWithFormat:@"stereo: foveation map %zu layer %lu profile (view %zu, every %.0f "
			"px from the view's edge; the center at %.0f %.0f): row", texture < map_count ? texture : 0,
			(unsigned long)layer, view_index, FOVEATION_PROFILE_STEP, center_x, center_y];
		for (int index = 0; index < row_count; index++)
			[text appendFormat:@" %.2f", row[index]];
		[text appendString:@"; column"];
		for (int index = 0; index < column_count; index++)
			[text appendFormat:@" %.2f", column[index]];
		/* the sharp region: how far out the rate stays at least the threshold,
		in degrees from the view's tangents; and against the center's own rate,
		for a quality whose center is below it */
		for (int relative = 0; relative < 2; relative++)
		{
			float center_rate = row_count ? row[0] : 1.0f;

			for (int index = 0; index < row_count; index++)
				if (row_at[index] <= center_x)
					center_rate = row[index];
			float threshold = relative ? FOVEATION_SHARP_RATE * center_rate : FOVEATION_SHARP_RATE;
			float left = foveation_sharp_edge(row, row_at, row_count, x1, center_x, -1, threshold);
			float right = foveation_sharp_edge(row, row_at, row_count, x1, center_x, 1, threshold);
			float up = foveation_sharp_edge(column, column_at, column_count, y1, center_y, -1, threshold);
			float down = foveation_sharp_edge(column, column_at, column_count, y1, center_y, 1, threshold);
			float left_degrees = degrees_from_tangent((center_x - left) / (float)viewport.width * (t.x + t.y));
			float right_degrees = degrees_from_tangent((right - center_x) / (float)viewport.width * (t.x + t.y));
			float up_degrees = degrees_from_tangent((center_y - up) / (float)viewport.height * (t.z + t.w));
			float down_degrees = degrees_from_tangent((down - center_y) / (float)viewport.height * (t.z + t.w));

			[text appendFormat:@"; sharp (rate at least %.2f%s): left %.1f right %.1f up %.1f down %.1f degrees, "
				"radius %.1f", threshold, relative ? ", 0.9 of the center's" : "", left_degrees, right_degrees,
				up_degrees, down_degrees, fminf(fminf(left_degrees, right_degrees), fminf(up_degrees, down_degrees))];
		}
		host_logf(HOST_LOG_INFO, "%s", text.UTF8String);
	}
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
	simd_float4x4 clip_from_frame;
	simd_float4 source, center, x_axis, y_axis;
	uint32_t decode_srgb;
	float brightness;
	uint32_t opaque;
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

/* Stereo on the screen: the viewer's eyes against the screen, in the
screen's frame (its center the origin, x right, y up, z toward the viewer).

For an eye at (ex, ey, d) in meters, d in front of the screen, the frame
reports the offset (ex, ey, d) / 3.048 (world units: right, up, back) and
the frustum through the screen's edges:
	left = (half_width + ex) / d     right = (half_width - ex) / d
	up = (half_height - ey) / d      down = (half_height + ey) / d
The guest doesn't render these eyes: it takes from them the viewer's eye
separation, the screen's half width (the tangents add to the width over the
distance) and the head's offset from the screen's axis (the eyes' midpoint),
and maps the game onto the screen as a 3D TV, the eyes at the game's camera
(stereo.c's screen_mapping_eyes; the film and SCREEN gameplay). The
orientation of the head plays no part. */
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
	head_frame_open = head_configured();
	/* the rate maps, whatever this frame shows: a10 opens on a film */
	foveation_measure(drawable);
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
		cp_view_texture_map_t texture_map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, 0));
		MTLViewport viewport = cp_view_texture_map_get_viewport(texture_map);
		id<MTLRasterizationRateMap> rate_map = host_theater_view_rate_map(drawable, texture_map);

		/* even, as the picture size the game renders at (host_stereo_picture_size).
		Foveated (display.foveation), the map's screen size: the presenter
		writes the view through the map, and the game renders its eyes
		unfoveated at that size (composite-only foveation; the eye passes
		through the maps come later, so debug.foveation_eye_passes renders the
		same way for now) */
		if (rate_map)
		{
			frame->eye_width = (int32_t)rate_map.screenSize.width & ~1;
			frame->eye_height = (int32_t)rate_map.screenSize.height & ~1;
			frame->foveated = 1;
			if (stereo_frames == 0)
				host_logf(HOST_LOG_INFO, "stereo: foveated: the eyes render unfoveated at the rate map's screen size "
					"%dx%d (the view's logical viewport %.0fx%.0f), and only the presenter goes through the maps "
					"(composite-only)", frame->eye_width, frame->eye_height, viewport.width, viewport.height);
		}
		else
		{
			frame->eye_width = (int32_t)viewport.width & ~1;
			frame->eye_height = (int32_t)viewport.height & ~1;
		}
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

/* display.frame_repeat, clamped to 0..3: how many extra refreshes each of
the Compositor's frames stays up in stereo (0: every refresh, 90 Hz; 1: 45
Hz). Theater mode's mono screen keeps every refresh */
int host_stereo_frame_repeat(void)
{
#if TARGET_OS_VISION
	char value[16];
	int repeat;

	if (!head_configured())
	{
		host_config_string("display.stereo", "off", value, sizeof(value));
		if (strcmp(value, "screen"))
			return 0;
	}
	repeat = (int)host_config_real("display.frame_repeat", 0.0);
	return repeat < 0 ? 0 : repeat > 3 ? 3 : repeat;
#else
	return 0;
#endif
}

void host_stereo_space_opened(void *layer_renderer)
{
#if TARGET_OS_VISION
	/* the frame rate: the layer renderer's repeat count, set for each
	opening (a new layer renderer starts at 0) */
	if (layer_renderer)
	{
		cp_layer_renderer_t renderer = (__bridge cp_layer_renderer_t)layer_renderer;
		int repeat = host_stereo_frame_repeat();

		cp_layer_renderer_set_minimum_frame_repeat_count(renderer, repeat);
		host_logf(HOST_LOG_INFO, "stereo: the frame repeat count is %d (asked %d): a frame every %d refresh%s",
			cp_layer_renderer_get_minimum_frame_repeat_count(renderer), repeat, repeat + 1, repeat ? "es" : "");
	}
	stereo_frames = 0;
	screen_frames = 0;
	stereo_presents = 0;
	ui_presents = 0;
	ui_shown = -1;
	inset_logged = 0;
	depth_reported = 0;
	foveation_logged_key = nil;
	foveation_frames = 0;
	foveation_easing_logged = 0;
#else
	(void)layer_renderer;
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
	frame->foveated = 0;
#if TARGET_OS_VISION
	head_frame_open = 0;
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

#if TARGET_OS_VISION
/* the level frame in the device's frame: at the device, turned by its yaw
in the room only (host_stereo_hud_level_yaw) */
static simd_float4x4 device_from_level(simd_float4x4 origin_from_device)
{
	float right[3] = { origin_from_device.columns[0].x, origin_from_device.columns[0].y,
		origin_from_device.columns[0].z };
	float back[3] = { origin_from_device.columns[2].x, origin_from_device.columns[2].y,
		origin_from_device.columns[2].z };
	float yaw = host_stereo_hud_level_yaw(right, back);
	simd_float4x4 origin_from_level = (simd_float4x4){ {
		{ cosf(yaw), 0.0f, -sinf(yaw), 0.0f },
		{ 0.0f, 1.0f, 0.0f, 0.0f },
		{ sinf(yaw), 0.0f, cosf(yaw), 0.0f },
		origin_from_device.columns[3],
	} };

	return simd_mul(simd_inverse(origin_from_device), origin_from_level);
}

/* draws the quads over a view, each from its texture in layers (indexed by
the quad's layer, HOST_STEREO_HUD_LAYER_*; a quad whose texture is nil or
past layer_count isn't drawn): clip_from_device is the view's projection
from the device's frame, level the level frame in the device's */
static void hud_draw(id<MTLRenderCommandEncoder> encoder, __unsafe_unretained id<MTLTexture> const *layers,
	int layer_count, const struct host_stereo_hud_quad *quads, int count, simd_float4x4 clip_from_device,
	simd_float4x4 level, uint32_t decode_srgb, float brightness, uint32_t opaque) API_AVAILABLE(visionos(26.0))
{
	int index;

	[encoder setRenderPipelineState:hud_pipeline];
	[encoder setDepthStencilState:depth_always];
	[encoder setFragmentSamplerState:linear_sampler atIndex:0];
	for (index = 0; index < count; index++)
	{
		const struct host_stereo_hud_quad *quad = &quads[index];
		struct hud_uniforms uniforms;

		if (quad->layer < 0 || quad->layer >= layer_count || !layers[quad->layer])
			continue;
		memset(&uniforms, 0, sizeof(uniforms));
		uniforms.clip_from_frame = quad->frame == HOST_STEREO_HUD_LEVEL ? simd_mul(clip_from_device, level) :
			clip_from_device;
		uniforms.source = (simd_float4){ quad->source[0], quad->source[1], quad->source[2], quad->source[3] };
		uniforms.center = (simd_float4){ quad->center[0], quad->center[1], quad->center[2], 0.0f };
		uniforms.x_axis = (simd_float4){ quad->x_axis[0], quad->x_axis[1], quad->x_axis[2], 0.0f };
		uniforms.y_axis = (simd_float4){ quad->y_axis[0], quad->y_axis[1], quad->y_axis[2], 0.0f };
		uniforms.decode_srgb = decode_srgb;
		uniforms.brightness = brightness;
		uniforms.opaque = opaque || quad->opaque;
		[encoder setFragmentTexture:layers[quad->layer] atIndex:0];
		[encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
		[encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	}
}
#endif

void host_stereo_present(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right,
	id<MTLTexture> left_depth, id<MTLTexture> right_depth, __unsafe_unretained id<MTLTexture> const *hud_layers,
	const float (*hud_group_extent)[4], float hud_aspect, int hud_ui, const float reticle[3],
	const float hud_tangents[2], id<MTLTexture> inset, float near_meters, float far_meters, float brightness,
	float vignette)
{
	id<MTLTexture> hud = hud_layers[HOST_STEREO_HUD_LAYER_HUD];

#if TARGET_OS_VISION
	if (@available(visionOS 26.0, *))
	{
		id<MTLTexture> colors[2] = { left, right }, depths[2] = { left_depth, right_depth };
		size_t count = host_theater_drawable_count();
		float depth_scale, depth_floor;
		simd_float2 depth_range = stereo_depth_range(near_meters, far_meters, &depth_scale, &depth_floor);
		float layout_width = (hud_aspect > 0.0f ? hud_aspect : 4.0f / 3.0f) * HOST_STEREO_HUD_LINES;
		struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
		int quad_count = hud ? host_stereo_hud_layout(layout_width, hud_ui, reticle, hud_tangents, hud_group_extent,
			quads) : 0;
		/* the zoom's inset, under the HUD, along the reticle's direction; not
		while a menu holds the HUD layer, whose UI quad it would cover */
		struct host_stereo_hud_quad inset_quad;
		int inset_shown = inset && !hud_ui && host_stereo_hud_inset(layout_width, NULL, reticle, &inset_quad);

		if (stereo_presents++ == 0)
			host_logf(HOST_LOG_INFO, "stereo: first present: eyes %lux%lu, %s (laid out at %.3f:1; its bands at "
				"%.2f mm a line, the reticle at %.2f, %.1f m ahead, inside %.0f degrees of the center; the rest "
				"head-locked at the HUD pass's half tangents %.3f by %.3f), depth range %.3f to %.1f m, %zu drawable%s",
				(unsigned long)left.width, (unsigned long)left.height, hud ? "a HUD" : "no HUD", hud_aspect,
				1000.0f * host_stereo_hud_band_scale(layout_width, hud_group_extent), 1000.0f * HOST_STEREO_HUD_METERS_PER_LINE,
				HOST_STEREO_HUD_DISTANCE, HUD_SHARP_RADIUS_DEGREES, hud_tangents ? hud_tangents[0] : 0.0f,
				hud_tangents ? hud_tangents[1] : 0.0f, near_meters, far_meters, count, count == 1 ? "" : "s");
		/* the zoom's inset as it comes and goes */
		if ((inset_shown != 0) != inset_logged)
		{
			if (inset_shown)
				host_logf(HOST_LOG_INFO, "stereo: zoomed: the inset (%lux%lu, its central %.0f lines) on a quad %.2f m "
					"wide, %.2f m ahead along (%.3f, %.3f, %.3f)", (unsigned long)inset.width,
					(unsigned long)inset.height, fminf(HALO_STEREO_INSET_LINES, layout_width),
					HALO_STEREO_INSET_WIDTH_METERS, HALO_STEREO_INSET_DISTANCE_METERS, inset_quad.center[0],
					inset_quad.center[1], inset_quad.center[2]);
			else
				host_logf(HOST_LOG_INFO, "stereo: the inset is gone");
			inset_logged = inset_shown != 0;
		}
		/* each change between the HUD's pieces and the UI's quad */
		if (hud && (hud_ui != 0) != ui_shown)
		{
			host_logf(HOST_LOG_INFO, "stereo: the HUD layer %s", hud_ui ? "holds a menu, the console or a progress "
				"bar: whole, on the UI's quad" : "is the HUD: the reticle and the bands");
			ui_shown = hud_ui != 0;
		}

		for (size_t index = 0; index < count; index++)
		{
			simd_float4x4 origin_from_device;
			cp_drawable_t drawable = host_theater_drawable(index, &origin_from_device, NULL);
			simd_float4x4 level = device_from_level(origin_from_device);
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
				/* foveated: every draw into the view goes through its rate map,
				the eye's picture, the inset and the HUD's quads alike; the
				viewport stays the texture map's, the logical one in the map's
				screen coordinates */
				pass.rasterizationRateMap = host_theater_view_rate_map(drawable, map);
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
				/* the zoom's inset, on the HUD's plane, then the HUD's pieces at
				the shape the game lays it out in (its texture is the eyes' size,
				whose pixels needn't be square), which stay readable over it */
				if (inset_shown || quad_count > 0)
				{
					simd_float4x4 projection = cp_drawable_compute_projection(drawable,
						cp_axis_direction_convention_right_up_back, view_index);
					simd_float4x4 clip_from_device = simd_mul(projection, simd_inverse(cp_view_get_transform(view)));

					if (inset_shown)
					{
						__unsafe_unretained id<MTLTexture> inset_layers[1] = { inset };

						hud_draw(encoder, inset_layers, 1, &inset_quad, 1, clip_from_device, level, decode_srgb,
							brightness, 1);
					}
					if (quad_count > 0)
						hud_draw(encoder, hud_layers, HOST_STEREO_HUD_LAYER_COUNT, quads, quad_count, clip_from_device,
							level, decode_srgb, brightness, 0);
				}
				[encoder endEncoding];
			}
			cp_drawable_encode_present(drawable, commands);
			gpu_metal_count_gpu_time(commands);
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
	(void)hud_layers;
	(void)hud_group_extent;
	(void)hud_aspect;
	(void)hud_ui;
	(void)reticle;
	(void)hud_tangents;
	(void)inset;
	(void)near_meters;
	(void)far_meters;
	(void)brightness;
#endif
}

int host_stereo_ui_ready(void)
{
#if TARGET_OS_VISION
	return head_frame_open && host_theater_frame_ready();
#else
	return 0;
#endif
}

void host_stereo_present_ui(id<MTLCommandQueue> queue, id<MTLTexture> picture, id<MTLTexture> hud, float brightness)
{
#if TARGET_OS_VISION
	if (@available(visionOS 26.0, *))
	{
		size_t count = host_theater_drawable_count();
		struct host_stereo_hud_quad quad;
		BOOL dark = host_theater_dark();

		host_stereo_hud_ui((float)picture.width / (float)picture.height, &quad);
		if (ui_presents++ == 0)
			host_logf(HOST_LOG_INFO, "stereo: a menu or a load: its %lux%lu picture%s on the UI's quad, %.2f by "
				"%.2f m %.1f m ahead, level, turning with the head", (unsigned long)picture.width,
				(unsigned long)picture.height, hud ? " and the HUD layer" : "", 2.0f * quad.x_axis[0],
				2.0f * quad.y_axis[1], HOST_STEREO_HUD_DISTANCE);
		for (size_t index = 0; index < count; index++)
		{
			simd_float4x4 origin_from_device;
			cp_drawable_t drawable = host_theater_drawable(index, &origin_from_device, NULL);
			simd_float4x4 level = device_from_level(origin_from_device);
			id<MTLCommandBuffer> commands;
			size_t views;

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
				uint32_t decode_srgb = color.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ||
					color.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB;
				simd_float4x4 projection = cp_drawable_compute_projection(drawable,
					cp_axis_direction_convention_right_up_back, view_index);

				/* around the quad, the theater's surroundings: dark, or the room
				(passthrough) */
				pass.colorAttachments[0].texture = color;
				pass.colorAttachments[0].slice = slice;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, dark ? 1.0 : 0.0);
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
				/* foveated: every draw into the view goes through its rate map,
				the eye's picture, the inset and the HUD's quads alike; the
				viewport stays the texture map's, the logical one in the map's
				screen coordinates */
				pass.rasterizationRateMap = host_theater_view_rate_map(drawable, map);
				id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
				[encoder setViewport:cp_view_texture_map_get_viewport(map)];
				simd_float4x4 clip_from_device = simd_mul(projection, simd_inverse(cp_view_get_transform(view)));

				{
					__unsafe_unretained id<MTLTexture> picture_layers[1] = { picture };

					hud_draw(encoder, picture_layers, 1, &quad, 1, clip_from_device, level, decode_srgb, brightness, 1);
				}
				/* the menu over it, by its transmittance; the quad's depth is
				written already */
				if (hud)
				{
					__unsafe_unretained id<MTLTexture> menu_layers[1] = { hud };

					hud_draw(encoder, menu_layers, 1, &quad, 1, clip_from_device, level, decode_srgb, brightness, 0);
				}
				[encoder endEncoding];
			}
			cp_drawable_encode_present(drawable, commands);
			gpu_metal_count_gpu_time(commands);
			[commands commit];
		}
		host_theater_frame_end();
	}
#else
	(void)queue;
	(void)picture;
	(void)hud;
	(void)brightness;
#endif
}
