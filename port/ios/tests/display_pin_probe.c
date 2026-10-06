#include "host_display_pin.h"
#include <assert.h>
#include <stdio.h>

static void parsed(const char *text, int width, int pixel_width, int pixel_height) {
    struct host_display_pin pin;
    assert(host_display_pin_parse(text, &pin) == 1);
    assert(pin.screen_width == width && pin.pixel_width == pixel_width && pin.pixel_height == pixel_height);
}
static void rejected(const char *text) {
    struct host_display_pin pin;
    assert(host_display_pin_parse(text, &pin) == -1);
}
int main(void) {
    struct host_display_pin pin;
    /* the iPad runner's display on a 2x screen, either way round, and an 11-inch iPad's */
    parsed("1366x1024@2", 640, 2732, 2048);
    parsed("1024x1366@2", 640, 2732, 2048);
    parsed("1194x834@2", 686, 2388, 1668);
    /* unset or empty: the product, nothing pinned */
    assert(host_display_pin_parse(NULL, &pin) == 0);
    assert(host_display_pin_parse("", &pin) == 0);
    /* a typo stops the run instead of leaving it to draw at the window's size */
    rejected("1366x1024");
    rejected("1366x1024@2x");
    rejected("1366 x 1024@2");
    rejected("0x1024@2");
    rejected("1366x-1024@2");
    rejected("1366x1024@0");
    rejected("1366x1024@nan");
    rejected("99999x1024@2");
    puts("PASS: display pins parse, and malformed ones are refused");
    return 0;
}
