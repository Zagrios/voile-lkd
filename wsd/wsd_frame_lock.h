/* wsd_frame_lock.h - which share of a screen's refreshes a game's frames are locked to.
 *
 * A frame stays on screen for a whole number of refreshes, so the steady rates of a screen are its refresh rate divided
 * by 1, 2, 3... A fixed lock names that number. An automatic lock names the highest rate the game should run at: wsd
 * takes the highest steady rate of the window's screen at or below it (60 on a 120 Hz screen for 75, 72 on 144 Hz, 60 on
 * 60 Hz), then watches whether the game keeps up. Three seconds of frames in which more than a tenth of the ticks had
 * no new frame, twice in a row, move it one step slower. After 30 s it tries one step faster again; a try that fails
 * doubles the wait, up to five minutes. A loading screen, where most of the three seconds passed without a frame, is
 * not judged.
 *
 * No Cocoa here, so the rule can be tested on its own (tests/graphics/wsd_frame_lock_test.c).
 */
#ifndef WSD_FRAME_LOCK_H
#define WSD_FRAME_LOCK_H

#include <stdint.h>

#ifndef WSD_LOCK_WINDOW_NS
#define WSD_LOCK_WINDOW_NS 3000000000ull
#endif
#define WSD_LOCK_STALL_NS 300000000ull
#define WSD_LOCK_LATE_PERCENT 10
#define WSD_LOCK_LATE_WINDOWS 2
#define WSD_LOCK_MIN_TICKS 8
#define WSD_LOCK_MIN_HZ 30
#ifndef WSD_LOCK_RETRY_FIRST_NS
#define WSD_LOCK_RETRY_FIRST_NS 30000000000ull
#endif
#define WSD_LOCK_RETRY_MAX_NS 300000000000ull
#ifndef WSD_LOCK_CONFIRM_NS
#define WSD_LOCK_CONFIRM_NS 15000000000ull
#endif

enum wsd_lock_mode { WSD_LOCK_NONE, WSD_LOCK_FIXED, WSD_LOCK_AUTO };
enum wsd_lock_change { WSD_LOCK_SAME, WSD_LOCK_SLOWER, WSD_LOCK_FASTER };

typedef struct wsd_frame_lock {
  enum wsd_lock_mode mode;
  uint32_t cap_hz;     /* WSD_LOCK_AUTO: the highest rate */
  uint32_t refresh_hz; /* of the window's screen; 0 until it is known */
  uint32_t base;       /* the divisor the screen alone calls for */
  uint32_t divisor;    /* refreshes a frame now */
  uint64_t window_start_ns, last_frame_ns, retry_at_ns, confirm_at_ns, retry_wait_ns;
  uint64_t stall_ns; /* of this window, in gaps longer than WSD_LOCK_STALL_NS */
  uint32_t ticks, late, late_windows;
  int trying_faster;
} wsd_frame_lock;

/* The divisor of the highest rate at or below cap_hz. */
static inline uint32_t wsd_lock_divisor_for(uint32_t refresh_hz, uint32_t cap_hz) {
  uint32_t divisor = cap_hz ? (refresh_hz + cap_hz - 1) / cap_hz : 1;
  return divisor ? divisor : 1;
}

static inline void wsd_lock_new_window(wsd_frame_lock *l, uint64_t now_ns) {
  l->window_start_ns = now_ns;
  l->stall_ns = 0;
  l->ticks = l->late = 0;
}

/* The window is on a screen of refresh_hz, for the first time or after a move or a change of mode. */
static inline void wsd_lock_set_screen(wsd_frame_lock *l, uint32_t refresh_hz) {
  l->refresh_hz = refresh_hz;
  if (l->mode == WSD_LOCK_AUTO) l->divisor = l->base = wsd_lock_divisor_for(refresh_hz, l->cap_hz);
  l->window_start_ns = l->last_frame_ns = l->retry_at_ns = l->confirm_at_ns = l->retry_wait_ns = 0;
  l->stall_ns = 0;
  l->ticks = l->late = l->late_windows = 0;
  l->trying_faster = 0;
}

/* One display-link tick. shown: a new frame went to the screen on this tick. visible: macOS shows the window. */
static inline enum wsd_lock_change wsd_lock_tick(wsd_frame_lock *l, uint64_t now_ns, int shown, int visible) {
  if (l->mode != WSD_LOCK_AUTO) return WSD_LOCK_SAME;
  if (!visible) {
    l->window_start_ns = 0;
    l->late_windows = 0;
    return WSD_LOCK_SAME;
  }
  if (!l->window_start_ns) {
    wsd_lock_new_window(l, now_ns);
    l->last_frame_ns = now_ns;
    return WSD_LOCK_SAME;
  }
  l->ticks++;
  if (shown) {
    if (now_ns - l->last_frame_ns > WSD_LOCK_STALL_NS) l->stall_ns += now_ns - l->last_frame_ns;
    l->last_frame_ns = now_ns;
  } else {
    l->late++;
  }
  if (now_ns - l->window_start_ns < WSD_LOCK_WINDOW_NS) return WSD_LOCK_SAME;

  uint32_t ticks = l->ticks, late = l->late;
  uint64_t length = now_ns - l->window_start_ns, stall = l->stall_ns;
  if (now_ns - l->last_frame_ns > WSD_LOCK_STALL_NS) stall += now_ns - l->last_frame_ns;
  int stalled = stall * 2 > length;
  wsd_lock_new_window(l, now_ns);
  if (stalled || ticks < WSD_LOCK_MIN_TICKS) {
    l->late_windows = 0;
    return WSD_LOCK_SAME;
  }
  int is_late = late * 100 > ticks * WSD_LOCK_LATE_PERCENT;
  l->late_windows = is_late ? l->late_windows + 1 : 0;
  if (l->late_windows >= WSD_LOCK_LATE_WINDOWS) {
    l->late_windows = 0;
    if (l->trying_faster) {
      l->trying_faster = 0;
      l->divisor++;
      l->retry_wait_ns = l->retry_wait_ns * 2 > WSD_LOCK_RETRY_MAX_NS ? WSD_LOCK_RETRY_MAX_NS : l->retry_wait_ns * 2;
      l->retry_at_ns = now_ns + l->retry_wait_ns;
      return WSD_LOCK_SLOWER;
    }
    if (l->refresh_hz >= WSD_LOCK_MIN_HZ * (l->divisor + 1)) {
      l->divisor++;
      l->retry_wait_ns = WSD_LOCK_RETRY_FIRST_NS;
      l->retry_at_ns = now_ns + l->retry_wait_ns;
      return WSD_LOCK_SLOWER;
    }
    return WSD_LOCK_SAME;
  }
  if (is_late) return WSD_LOCK_SAME;
  if (l->trying_faster) {
    if (now_ns >= l->confirm_at_ns) {
      l->trying_faster = 0;
      l->retry_wait_ns = WSD_LOCK_RETRY_FIRST_NS;
      l->retry_at_ns = l->divisor > l->base ? now_ns + l->retry_wait_ns : 0;
    }
    return WSD_LOCK_SAME;
  }
  if (l->divisor > l->base && l->retry_at_ns && now_ns >= l->retry_at_ns) {
    l->divisor--;
    l->trying_faster = 1;
    l->confirm_at_ns = now_ns + WSD_LOCK_CONFIRM_NS;
    return WSD_LOCK_FASTER;
  }
  return WSD_LOCK_SAME;
}

#endif
