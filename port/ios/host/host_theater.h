/* Theater mode (display.immersive, visionOS): host_theater.m and Theater.swift */
#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>
/* draws picture on the theater screen for the Compositor's next frame */
void host_theater_present(id<MTLCommandQueue> queue, id<MTLTexture> picture);
#endif

/* opens the immersive space (Theater.swift) */
void host_theater_open(void);
/* whether the space is open, so gpu_present draws there and not in the window */
int host_theater_active(void);
/* for Theater.swift: the space's layer renderer, unretained */
void host_theater_attach(void *renderer);
void host_theater_log_c(const char *message);
