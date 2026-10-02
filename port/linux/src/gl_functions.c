/*
GL_FUNCTIONS.C

Run-time resolution of the OpenGL entry points listed in gl.h.
*/

/* the platform layer's (xbox_kernel.c; host_gpu.c in the iOS host) */
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
#define GL_FUNCTIONS_DEFINE
#include "gl.h"

#include <SDL3/SDL.h>

#define GL_DEFINE_FUNCTION(name) __typeof__(&name) halo_##name;
GL_FUNCTIONS(GL_DEFINE_FUNCTION)
unsigned long halo_gl_call_count;

void halo_gl_count_call(void)
{
	halo_gl_call_count++;
}

int gl_functions_load(void)
{
	int success = 1;

#define GL_LOAD_FUNCTION(name) \
	halo_##name = (__typeof__(halo_##name))SDL_GL_GetProcAddress(#name); \
	if (!halo_##name) \
	{ \
		platform_log("OpenGL function %s is unavailable", #name); \
		success = 0; \
	}
	GL_FUNCTIONS(GL_LOAD_FUNCTION)
#undef GL_LOAD_FUNCTION
	return success;
}
