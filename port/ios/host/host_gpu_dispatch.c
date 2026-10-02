/*
HOST_GPU_DISPATCH.C

gpu.h's entry points in the iOS and tvOS host, which the guest's host_gpu_*
imports call (tools/ios_bridges.py): each calls the backend gpu_initialize
chose: GL (port/linux/src/gpu_gl.c) or, with GPU_INITIALIZE_METAL
(display.renderer = "metal"), Metal (gpu_metal.m).
*/

#include "gpu.h"
#include "ios_host.h"

extern const struct gpu_backend gpu_backend_gl, gpu_backend_metal;

/* nothing calls gpu.h before gpu_initialize (d3d8_device.c's gl_initialize) */
static const struct gpu_backend *backend = &gpu_backend_gl;

/* the chosen backend's table with a gpu_present that first holds while the
app is in the background (host_lifecycle.m) and, with debug.frame_counter,
then counts the frame, so the count is the front end's frame number (one
Present each) under either backend */
static struct gpu_backend wrapped;
static const struct gpu_backend *wrapped_backend;
static int frame_counter;
static unsigned long presented;

static void present_wrapped(gpu_texture back_buffer)
{
	host_lifecycle_hold();
	wrapped_backend->present(back_buffer);
	if (frame_counter)
		host_frame_counter_show(presented++);
}

void gpu_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
	backend = (flags & GPU_INITIALIZE_METAL) ? &gpu_backend_metal : &gpu_backend_gl;
	wrapped_backend = backend;
	wrapped = *backend;
	wrapped.present = present_wrapped;
	backend = &wrapped;
	frame_counter = (flags & GPU_INITIALIZE_FRAME_COUNTER) != 0;
	if (frame_counter)
		host_frame_counter_start((flags & GPU_INITIALIZE_FIXED_TIMESTEP) != 0, (flags & GPU_INITIALIZE_METAL) != 0);
	host_lifecycle_install();
	backend->initialize(flags, capabilities);
}

#define DISPATCH_FUNCTION(type, name, parameters, arguments) type gpu_##name parameters { return backend->name arguments; }
#define DISPATCH_PROCEDURE(name, parameters, arguments) void gpu_##name parameters { backend->name arguments; }
GPU_OPERATIONS(DISPATCH_FUNCTION, DISPATCH_PROCEDURE)
