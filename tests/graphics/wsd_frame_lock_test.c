/* wsd_frame_lock_test.c - the frame lock's rule, without a window. Build and run:
 *   clang -std=c11 -Wall -Wextra -I wsd -o /tmp/lock_test tests/graphics/wsd_frame_lock_test.c && /tmp/lock_test */
#include <stdio.h>
#include "wsd_frame_lock.h"

static int failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failed++; } } while (0)
#define SECOND 1000000000ull

static wsd_frame_lock auto_lock(uint32_t refresh, uint32_t cap) {
  wsd_frame_lock l = {0};
  l.mode = WSD_LOCK_AUTO;
  l.cap_hz = cap;
  wsd_lock_set_screen(&l, refresh);
  return l;
}

static uint32_t hz(const wsd_frame_lock *l) { return l->refresh_hz / l->divisor; }

/* Runs `seconds` of ticks at the lock's rate, one tick in `missed` without a new frame (0: none); counts changes. */
static uint64_t run(wsd_frame_lock *l, uint64_t t, unsigned seconds, unsigned missed, int *slower, int *faster) {
  uint64_t end = t + seconds * SECOND;
  unsigned n = 0;
  while (t < end) {
    t += SECOND / hz(l);
    n++;
    enum wsd_lock_change c = wsd_lock_tick(l, t, !(missed && n % missed == 0), 1);
    if (c == WSD_LOCK_SLOWER && slower) (*slower)++;
    if (c == WSD_LOCK_FASTER && faster) (*faster)++;
  }
  return t;
}

int main(void) {
  /* the steady rate for each screen, at most 75 */
  struct { uint32_t refresh, want; } screens[] = {{60, 60}, {75, 75}, {100, 50}, {120, 60}, {144, 72},
                                                  {165, 55}, {180, 60}, {240, 60}, {360, 72}};
  for (unsigned i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
    wsd_frame_lock l = auto_lock(screens[i].refresh, 75);
    CHECK(hz(&l) == screens[i].want);
  }

  /* a game that keeps up stays where it is */
  wsd_frame_lock l = auto_lock(120, 75);
  int slower = 0, faster = 0;
  uint64_t t = run(&l, SECOND, 60, 0, &slower, &faster);
  CHECK(slower == 0 && faster == 0 && hz(&l) == 60);

  /* a few misses, under a tenth, change nothing */
  slower = faster = 0;
  t = run(&l, t, 30, 20, &slower, &faster);
  CHECK(slower == 0 && hz(&l) == 60);

  /* a game that misses every fifth tick steps down, to the next steady rate only */
  slower = faster = 0;
  t = run(&l, t, 7, 5, &slower, &faster);
  CHECK(slower == 1 && hz(&l) == 40);

  /* after 30 s it tries the faster rate; held for 15 s, it stays */
  faster = 0;
  t = run(&l, t, 34, 0, NULL, &faster);
  CHECK(faster == 1 && l.trying_faster == 1 && hz(&l) == 60);
  t = run(&l, t, 19, 0, NULL, NULL);
  CHECK(l.trying_faster == 0 && hz(&l) == 60);

  /* a try that fails goes back, and the next one waits twice as long */
  slower = faster = 0;
  t = run(&l, t, 7, 4, &slower, NULL);
  CHECK(hz(&l) == 40);
  t = run(&l, t, 34, 0, NULL, &faster);
  CHECK(faster == 1 && hz(&l) == 60);
  slower = 0;
  t = run(&l, t, 7, 4, &slower, NULL);
  CHECK(slower == 1 && hz(&l) == 40 && l.retry_wait_ns == 2 * WSD_LOCK_RETRY_FIRST_NS);
  faster = 0;
  t = run(&l, t, 40, 0, NULL, &faster);
  CHECK(faster == 0 && hz(&l) == 40);
  t = run(&l, t, 25, 0, NULL, &faster);
  CHECK(faster == 1 && hz(&l) == 60);

  /* a loading screen is not judged */
  l = auto_lock(120, 75);
  t = run(&l, SECOND, 4, 0, NULL, NULL);
  for (unsigned i = 0; i < 360; i++) { t += SECOND / 60; CHECK(wsd_lock_tick(&l, t, 0, 1) == WSD_LOCK_SAME); }
  t = run(&l, t, 2, 0, NULL, NULL);
  CHECK(hz(&l) == 60);

  /* a game with hitches of 400 ms that is otherwise far too slow is judged, and steps down */
  l = auto_lock(120, 75);
  t = SECOND;
  slower = 0;
  for (unsigned i = 0; i < 1200; i++) {
    t += SECOND / 60;
    int shown = i % 6 == 0 && i % 60 != 6;
    if (wsd_lock_tick(&l, t, shown, 1) == WSD_LOCK_SLOWER) slower++;
    if (slower) break;
  }
  CHECK(slower == 1 && hz(&l) == 40);

  /* a hidden window is not judged */
  l = auto_lock(120, 75);
  t = SECOND;
  for (unsigned i = 0; i < 720; i++) { t += SECOND / 60; CHECK(wsd_lock_tick(&l, t, 0, 0) == WSD_LOCK_SAME); }
  CHECK(hz(&l) == 60);

  /* it never goes below 30 */
  l = auto_lock(120, 75);
  t = SECOND;
  slower = 0;
  t = run(&l, t, 300, 2, &slower, NULL);
  CHECK(l.divisor <= 4 && l.refresh_hz / l.divisor >= 30);

  /* moving to another screen starts over from that screen's rate */
  wsd_lock_set_screen(&l, 144);
  CHECK(hz(&l) == 72 && l.divisor == l.base && !l.trying_faster);

  /* a fixed lock never changes */
  wsd_frame_lock f = {0};
  f.mode = WSD_LOCK_FIXED;
  f.divisor = 2;
  wsd_lock_set_screen(&f, 120);
  for (unsigned i = 0; i < 1000; i++) CHECK(wsd_lock_tick(&f, SECOND + i * SECOND / 60, 0, 1) == WSD_LOCK_SAME);
  CHECK(f.divisor == 2);

  printf(failed ? "RESULT FAIL (%d)\n" : "RESULT PASS\n", failed);
  return failed != 0;
}
