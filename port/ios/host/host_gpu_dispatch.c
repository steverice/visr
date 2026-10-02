/*
HOST_GPU_DISPATCH.C

gpu.h's entry points in the iOS and tvOS host, which the guest's host_gpu_*
imports call (tools/ios_bridges.py): each calls the backend gpu_initialize
chose.
*/

#include "gpu.h"

extern const struct gpu_backend gpu_backend_gl;

/* nothing calls gpu.h before gpu_initialize (d3d8_device.c's gl_initialize) */
static const struct gpu_backend *backend = &gpu_backend_gl;

void gpu_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
	backend = &gpu_backend_gl;
	backend->initialize(flags, capabilities);
}

#define DISPATCH_FUNCTION(type, name, parameters, arguments) type gpu_##name parameters { return backend->name arguments; }
#define DISPATCH_PROCEDURE(name, parameters, arguments) void gpu_##name parameters { backend->name arguments; }
GPU_OPERATIONS(DISPATCH_FUNCTION, DISPATCH_PROCEDURE)
