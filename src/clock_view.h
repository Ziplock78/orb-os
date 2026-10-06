#pragma once
#include <lvgl.h>
// A simple full-screen clock, living on its very own LVGL screen so it stays
// completely independent of the radar UI. App two in the shell.
namespace clockview {
    // How many pixels the hand-layer cache draws differently from a full compose, and by
    // how many 565 levels at worst. 0 is the only good answer; -2 means the layers were not
    // in use, so the comparison proved nothing.
    long layerDiffersBy(int *worstOut, int *wx, int *wy);
    void      init();      // build the clock screen; call once after display::begin()
    lv_obj_t* screen();    // the clock's LVGL screen (hand this to app_shell::add)
    // The art contract every screen on this device is supposed to keep, and this one did
    // not: take PSRAM when the app is shown, give it back when it is not.
    //
    // The clock's canvas and its plate-rotation cache are 466x466 buffers, and they were
    // allocated in init() and held for the life of the device. Measured at boot, clockview
    // took 2,460 KB before anything had been looked at, and the weather radar, which
    // initialises last, was left with 444 KB and could allocate ONE of its five frame
    // buffers. A screen nobody is looking at should not be holding the memory a screen
    // somebody IS looking at needs.
    void      onEnter();   // shell is switching to us: take the canvas back
    void      onExit();    // shell is switching away: give it up, plus the decoded face
    void      refresh();   // redraw the face now, for coming back from something that covered it
    void      setSweep(int mode);  // -1 theme decides, 0 force tick, 1 force sweep. Not persisted.
    // FOR THE SELF-TEST, and for nothing else. Two questions, and they exist because the
    // answers were confused with each other in September 2026: whether the last read of the
    // wall clock was believed (a spurious no put every theme's hands at twelve for a frame,
    // see orb_time.h), and where the second hand is placed for a given wall-clock second,
    // which is that second itself unless THIS design asked for the railway stop.
    bool      faceHasTime();
    float     handSeconds(float wallSeconds);
    // How stale the sweep cache, and so the minute hand inside it, is allowed to get, in
    // fractional minutes. Exposed for the self-test: a sweeping design has to come back
    // under a minute or its minute hand steps instead of creeping, and a railway design
    // has to come back at exactly a minute because there the step is the design.
    float     cacheMinutesAllowed();
    // The same question for the cache one layer DOWN, which holds the hour hand. It was only
    // ever dropped when the hour changed, so the hour hand stood still for up to an hour and
    // then jumped a division. Exposed with the pixels its tip covers in that time, because
    // "a pixel" is the whole rule and a threshold in minutes alone cannot be checked.
    float     hourCacheMinutes();
    float     hourTipPixelsIn(float minutes);
    // Where the minute hand belongs, in minutes-of-the-hour. Exposed for the self-test:
    // a railway dial has to land on a whole minute at every second of the minute.
    float     minuteHandMins(bool railway, int min, int sec);
    // How far through its step the minute hand is at a given wall second, or -1 when it is
    // not stepping, and how long the step lasts. Exposed so the self-test can hold the step
    // window inside the railway stop, where the second hand is parked.
    float     minuteStepEase(float wallSecs);
    float     minuteStepSecs();
    // How much of the dial is actually drawn, in pixels that are not black. Exposed for the
    // self-test, which has no other way to ask the question that matters: after a trip to
    // another app and back, is the face THERE. The sweep caches outlive the canvas they were
    // built from, and a cache taken for one canvas and restored into another paints the dial
    // back a hand-width at a time while the rest stays black.
    long      litPixels();
    // Does sweeping all the way round land on exactly the pixels it started from?
    //
    // It has to. The hand's box is restored out of a cache, the hand is drawn, and whatever
    // the design puts above it is drawn again over just those pixels. Every one of those
    // steps has to be confined to what was actually wiped, because anything applied twice to
    // a pixel that already had it moves that pixel: a glass mixed in again lightens it, a
    // shadow laid over itself darkens it. Done once a frame on a sweeping dial, that shows
    // as patches of the dial changing brightness as the hand goes by, which is what
    // Jean-Paul Stringaro filmed and reported on 2026-10-05.
    //
    // Returns HOW MANY pixels are wrong, not how wrong the worst one is. The peak depends
    // on where the hands happen to be when the test runs, so a threshold on it passes at one
    // minute and fails at the next; the count does not move. The fault this guards against
    // was 9,916 pixels, and what is left is about twenty under the hand cap.
    // Beats a second the sweeping hand is stepping at; x3600 for the beats-per-hour a
    // watchmaker would quote (8 is 28,800, 4 is 14,400).
    int       sweepBeat();
    long      sweepDiffersBy(int *x, int *y);
    float     railwayStopStart();
    // Which beat of the second a given instant falls in. Exposed so the self-test can hold the
    // one piece of arithmetic that decides how often a theme ticks.
    long      beatSlot(long sec, long usec, int beat);
    // How long the tick's own timer waits, from a given point in the second. Exposed because
    // the whole value of that timer is that it lands on the second and nothing else.
    uint32_t  beatAim(long usec);
    // Whether a design whose full compose costs this many milliseconds can afford to animate
    // the minute hand's step, rather than clicking it over in one move.
    bool      stepAffordable(float composeMs);
}
