/* debug.frame_counter: the frame number and the game time over the game, so a
moment can be named ("frame 840") and found again. With debug.fixed_timestep
a frame is the same moment in every run and under either renderer. A UIKit
label over the game's view: the renderer never draws it, so screenshots and
debug.gpu_stats don't change. Updated from gpu_present, on the UI thread the
game runs on. */
#import <UIKit/UIKit.h>
#include "ios_host.h"
#include <time.h>

static UILabel *label;
static int fixed;
static struct timespec first;

void host_frame_counter_start(int fixed_timestep) {
    fixed=fixed_timestep;
    host_logf(HOST_LOG_INFO,"frame counter on (%s time)",fixed?"game":"wall");
}

static UILabel *make_label(void) {
    UIWindow *window=nil;
    for(UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if(![scene isKindOfClass:UIWindowScene.class])continue;
        for(UIWindow *candidate in ((UIWindowScene *)scene).windows)
            if(candidate.isKeyWindow)window=candidate;
    }
    UIView *root=window.rootViewController.view;
    if(!root)return nil;
    UILabel *made=[UILabel new];
    made.font=[UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightSemibold];
    made.textColor=UIColor.whiteColor;
    made.backgroundColor=[UIColor colorWithWhite:0 alpha:0.55];
    made.textAlignment=NSTextAlignmentCenter;
    made.layer.cornerRadius=6;made.clipsToBounds=YES;
    made.userInteractionEnabled=NO;
    made.translatesAutoresizingMaskIntoConstraints=NO;
    [root addSubview:made];
    [made.leadingAnchor constraintEqualToAnchor:root.safeAreaLayoutGuide.leadingAnchor constant:8].active=YES;
    [made.topAnchor constraintEqualToAnchor:root.safeAreaLayoutGuide.topAnchor constant:8].active=YES;
    [made.widthAnchor constraintEqualToConstant:190].active=YES;
    [made.heightAnchor constraintEqualToConstant:26].active=YES;
    return made;
}

void host_frame_counter_show(unsigned long frame) {
    @autoreleasepool {
        /* in the log too, to check the count against debug.gpu_stats' frames */
        if(frame%600==0)host_logf(HOST_LOG_INFO,"frame counter: %lu",frame);
        if(!label && !(label=make_label()))return;
        double seconds;
        if(fixed) seconds=frame/30.0;
        else {
            struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
            if(!frame)first=now;
            seconds=(double)(now.tv_sec-first.tv_sec)+(now.tv_nsec-first.tv_nsec)/1e9;
        }
        unsigned long whole=(unsigned long)seconds;
        /* game time as video timecode, minutes:seconds:frames (30 frames a
           second); wall time with hundredths */
        label.text=fixed?
            [NSString stringWithFormat:@"%lu  %lu:%02lu:%02lu",frame,whole/60,whole%60,frame%30]:
            [NSString stringWithFormat:@"%lu  %lu:%05.2f",frame,whole/60,seconds-60*(whole/60)];
    }
}
