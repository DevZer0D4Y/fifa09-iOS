// iOS display clock for the guest vblank (patches/rexglue-ios.patch looks this symbol up).
//
// The SDK's guest vblank (which paces the game: it presents once per vblank) normally comes
// from a 1 ms sleep loop at exactly 60.000 Hz. The screen refreshes on its own clock, so the two
// drift apart and every few seconds one game frame is shown twice: a visible stutter at "60 fps"
// even when the phone is fast enough. Here a CADisplayLink, locked to the requested rate
// (preferredFrameRateRange 60/60/60, also on 120 Hz ProMotion screens), ticks on its own thread
// and the SDK's vblank thread waits for it, so game frames and screen refreshes stay in step.
//
//   bool rex_wait_display_vblank(double hz, uint32_t timeout_ms)
// returns true at the screen's next refresh; false if the screen cannot run at hz, on timeout,
// or while the app is in the background (the display link stops): the SDK then falls back to
// its timer for that wait.

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <pthread.h>
#include <sys/qos.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace {

std::mutex g_mutex;
std::condition_variable g_tick_cv;
uint64_t g_ticks = 0;          // display refreshes seen
std::atomic<double> g_hz{0};   // the rate the link is locked to (0: not started)
std::atomic<double> g_max_hz{0};
std::once_flag g_start_once;

}  // namespace

@interface RexDisplayLinkTarget : NSObject
- (void)tick:(CADisplayLink*)link;
@end

@implementation RexDisplayLinkTarget
- (void)tick:(CADisplayLink*)link {
  (void)link;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ticks++;
  }
  g_tick_cv.notify_all();
}
@end

namespace {

// The display link runs on its own thread and run loop: the main thread is UIKit's and may be
// busy (SDL event pumping, ImGui), and a late tick there is exactly the jitter this removes.
void StartDisplayLink(double hz) {
  // UIScreen is main-thread only; asked asynchronously (never block this thread on UIKit).
  // Until the answer arrives the requested rate is assumed to be available.
  dispatch_async(dispatch_get_main_queue(), ^{
    g_max_hz = double(UIScreen.mainScreen.maximumFramesPerSecond);
  });
  g_hz = hz;
  NSThread* thread = [[NSThread alloc] initWithBlock:^{
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    RexDisplayLinkTarget* target = [RexDisplayLinkTarget new];
    CADisplayLink* link = [CADisplayLink displayLinkWithTarget:target selector:@selector(tick:)];
    const float rate = float(hz);
    link.preferredFrameRateRange = CAFrameRateRangeMake(rate, rate, rate);
    [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
    for (;;) {
      @autoreleasepool {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate distantFuture]];
      }
    }
  }];
  thread.name = @"Display vblank";
  thread.qualityOfService = NSQualityOfServiceUserInteractive;
  [thread start];
}

}  // namespace

extern "C" __attribute__((visibility("default"), used)) bool rex_wait_display_vblank(double hz,
                                                                                    uint32_t timeout_ms) {
  if (!(hz > 0)) return false;
  if ([NSThread isMainThread]) return false;  // would block UIKit
  std::call_once(g_start_once, [hz] { StartDisplayLink(hz); });
  // The screen cannot refresh this fast (a 60 Hz phone asked for more): keep the SDK's timer.
  const double max_hz = g_max_hz.load();
  if ((max_hz > 0 && max_hz + 1.0 < hz) || std::fabs(g_hz.load() - hz) > 1.0) return false;
  std::unique_lock<std::mutex> lock(g_mutex);
  const uint64_t seen = g_ticks;
  return g_tick_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [seen] { return g_ticks != seen; });
}
