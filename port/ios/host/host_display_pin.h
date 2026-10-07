/* HALO_HOST_DISPLAY=WIDTHxHEIGHT@SCALE: the display the native Mac runner pins (host_main.m), what the
   iPad runner's SDL reports on a 2x screen (1366x1024@2). Returns 1 with pin filled, 0 when text is unset
   or empty (the product: nothing pinned), -1 when it is malformed (the run must stop). */
#pragma once
#include <stdio.h>

struct host_display_pin {
	int screen_width;   /* HALO_DISPLAY_WIDTH: 480 rows at the display's shape, even */
	int pixel_width;    /* HALO_DISPLAY_PIXEL_WIDTH, the longer side */
	int pixel_height;   /* HALO_DISPLAY_PIXEL_HEIGHT */
};

static inline int host_display_pin_parse(const char *text, struct host_display_pin *pin)
{
	int width = 0, height = 0, consumed = 0;
	float scale = 0;
	if (!text || !*text)
		return 0;
	if (sscanf(text, "%dx%d@%f%n", &width, &height, &scale, &consumed) != 3 || text[consumed] ||
		width <= 0 || height <= 0 || width > 16384 || height > 16384 || !(scale > 0 && scale <= 8))
		return -1;
	int longer = width > height ? width : height, shorter = width > height ? height : width;
	pin->screen_width = (480 * longer / shorter) & ~1;
	pin->pixel_width = (int)(longer * scale + 0.5f);
	pin->pixel_height = (int)(shorter * scale + 0.5f);
	return 1;
}
