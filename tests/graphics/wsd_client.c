/*
 * wsd_client.c - owned native witness for the window server (wsd/).
 *
 * 1. HELLO, window create/show, three BGRA8 shm buffers (shm_open + unlink) attached by SCM_RIGHTS.
 * 2. Pacing: animates a pattern at each requested rate (default 60 and 120 Hz) for a fixed number of
 *    frames, triple-buffered on RELEASE events; reports the interval between consecutive
 *    CAMetalDrawable presentedTime values and submit->presented latency (p50/p95/p99).
 * 3. Protocol checks: synthetic input (DEBUG_INJECT), relative mouse mode, cursor clip, cursor
 *    visibility, fullscreen toggle, title, ping latency.
 * 4. --render-check: what wsd tells a client and what macOS makes of a window, for the render percent wsd runs with.
 * Single-threaded, bounded loops, ends with "RESULT PASS|FAIL".
 *
 * Usage: wsd_client <socket> [seconds_per_rate<=10] [rates e.g. 60,120] [--no-pointer]
 *   --no-pointer skips the relative-mouse step (it really disassociates the pointer for ~0.3 s).
 *   or: wsd_client <socket> --render-check <percent> [--fullscreen]
 *   runs only the render check (run-wsd-render-percent.sh). percent, 1..100, is what wsd must have applied: its
 *   --render-percent after clamping, 100 without one or with a value that is not an integer. --fullscreen also toggles
 *   fullscreen, which covers the screen for a moment.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#include "../../wsd/wsd_protocol.h"

#define WIN 1u
#define NBUF 3
static int g_sock = -1, g_fail;
static uint32_t g_w = 640, g_h = 360;

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

#define CHECK(cond, ...)                                     \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("CHECK FAILED (%s): ", #cond);                  \
      printf(__VA_ARGS__);                                   \
      printf("\n");                                          \
      g_fail = 1;                                            \
    }                                                        \
  } while (0)

static void die(const char *m) {
  printf("FATAL: %s (errno=%d)\nRESULT FAIL\n", m, errno);
  exit(1);
}

static void send_msg(uint32_t type, const void *p, uint32_t n, const void *extra, uint32_t en, int fd) {
  uint8_t buf[sizeof(struct wsd_hdr) + WSD_MAX_PAYLOAD];
  struct wsd_hdr h = {type, n + en};
  memcpy(buf, &h, sizeof(h));
  if (n) memcpy(buf + sizeof(h), p, n);
  if (en) memcpy(buf + sizeof(h) + n, extra, en);
  size_t total = sizeof(h) + n + en, off = 0;
  while (off < total) {
    struct iovec iov = {buf + off, total - off};
    struct msghdr m = {0};
    char ctl[CMSG_SPACE(sizeof(int))];
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (fd >= 0 && off == 0) {
      m.msg_control = ctl;
      m.msg_controllen = sizeof(ctl);
      struct cmsghdr *c = CMSG_FIRSTHDR(&m);
      c->cmsg_level = SOL_SOCKET;
      c->cmsg_type = SCM_RIGHTS;
      c->cmsg_len = CMSG_LEN(sizeof(int));
      memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    ssize_t r = sendmsg(g_sock, &m, 0);
    if (r <= 0) {
      if (errno == EAGAIN || errno == EINTR) { struct pollfd p = {g_sock, POLLOUT, 0}; poll(&p, 1, 100); continue; }
      die("sendmsg");
    }
    off += (size_t)r;
  }
}

/* ---- event intake ---- */
struct event { uint32_t type, len; uint8_t data[WSD_MAX_PAYLOAD]; };
static uint8_t g_in[1 << 16];
static size_t g_inlen;

/* Reads one event if available within timeout_ms (-1: block). Returns 1 if *e filled. */
static int next_event(struct event *e, int timeout_ms) {
  for (;;) {
    if (g_inlen >= sizeof(struct wsd_hdr)) {
      struct wsd_hdr h;
      memcpy(&h, g_in, sizeof(h));
      if (h.len > WSD_MAX_PAYLOAD) die("oversized event");
      if (g_inlen >= sizeof(h) + h.len) {
        e->type = h.type;
        e->len = h.len;
        memcpy(e->data, g_in + sizeof(h), h.len);
        memmove(g_in, g_in + sizeof(h) + h.len, g_inlen - sizeof(h) - h.len);
        g_inlen -= sizeof(h) + h.len;
        return 1;
      }
    }
    struct pollfd p = {g_sock, POLLIN, 0};
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) return 0;
    ssize_t n = read(g_sock, g_in + g_inlen, sizeof(g_in) - g_inlen);
    if (n <= 0) die("server closed the connection");
    g_inlen += (size_t)n;
  }
}

/* ---- frame bookkeeping ---- */
struct buf { int fd; uint8_t *px; int busy; };
static struct buf g_buf[NBUF];
static uint64_t *g_presented, *g_latency, *g_encode;
static int g_npresented, g_nzero, g_replaced, g_errors;

static void handle_async(const struct event *e) {
  if (e->type == WSD_EV_RELEASE) {
    const struct wsd_ev_release *r = (const void *)e->data;
    if (r->buffer_id >= 1 && r->buffer_id <= NBUF) g_buf[r->buffer_id - 1].busy = 0;
  } else if (e->type == WSD_EV_PRESENTED) {
    const struct wsd_ev_presented *p = (const void *)e->data;
    g_replaced += p->replaced;
    if (!p->presented_ns) { g_nzero++; return; }
    if (g_presented && g_npresented < 4096) {
      g_presented[g_npresented] = p->presented_ns;
      g_latency[g_npresented] = p->presented_ns - p->client_time_ns;
      g_encode[g_npresented] = p->encoded_ns - p->client_time_ns;
      g_npresented++;
    }
  } else if (e->type == WSD_EV_ERROR) {
    const struct wsd_ev_error *er = (const void *)e->data;
    printf("server error: request %u code %d\n", er->request_type, er->code);
    g_errors++;
  }
}

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}

static void pct(const char *name, uint64_t *v, int n, double *p50_out) {
  if (n <= 0) { printf("  %s: no samples\n", name); return; }
  qsort(v, (size_t)n, sizeof(*v), cmp_u64);
  int i99 = (int)(n * 0.99); if (i99 >= n) i99 = n - 1;
  printf("  %s: n=%d min=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f ms\n", name, n, v[0] / 1e6, v[n / 2] / 1e6,
         v[(int)(n * 0.95)] / 1e6, v[i99] / 1e6, v[n - 1] / 1e6);
  if (p50_out) *p50_out = v[n / 2] / 1e6;
}

static void draw(uint8_t *px, uint64_t frame) {
  /* Moving vertical bar over a gradient: BGRA8. */
  uint32_t bar = (uint32_t)(frame * 8 % g_w);
  for (uint32_t y = 0; y < g_h; y++) {
    uint32_t *row = (uint32_t *)(px + y * g_w * 4);
    uint32_t base = 0xFF000000u | ((y * 255 / g_h) << 8) | 0x30;
    for (uint32_t x = 0; x < g_w; x++) row[x] = (x - bar < 24) ? 0xFFFFFFFFu : base | ((x * 255 / g_w) << 16);
  }
}

static void run_rate(uint32_t hz, double seconds) {
  struct wsd_window_flag fr = {WIN, hz};
  send_msg(WSD_SET_FRAME_RATE, &fr, sizeof(fr), NULL, 0, -1);
  int frames = (int)(hz * seconds);
  g_npresented = g_nzero = g_replaced = 0;
  int starved = 0, sent = 0;
  uint64_t period = 1000000000ull / hz, t0 = now_ns() + 50000000ull, serial = 0;
  struct event e;
  for (int f = 0; f < frames; f++) {
    uint64_t deadline = t0 + (uint64_t)f * period;
    for (;;) {
      uint64_t t = now_ns();
      if (t >= deadline) break;
      int ms = (int)((deadline - t) / 1000000);
      if (next_event(&e, ms > 0 ? ms : 0)) handle_async(&e);
      else if (ms == 0) { while (now_ns() < deadline) {} break; }
    }
    while (next_event(&e, 0)) handle_async(&e);
    int b = -1;
    for (int i = 0; i < NBUF; i++) if (!g_buf[(f + i) % NBUF].busy) { b = (f + i) % NBUF; break; }
    if (b < 0) { starved++; continue; }
    draw(g_buf[b].px, (uint64_t)f);
    g_buf[b].busy = 1;
    struct wsd_present p = {WIN, (uint32_t)b + 1, ++serial, now_ns()};
    send_msg(WSD_PRESENT, &p, sizeof(p), NULL, 0, -1);
    sent++;
  }
  uint64_t end = now_ns() + 300000000ull;
  while (now_ns() < end) if (next_event(&e, 20)) handle_async(&e);

  printf("rate %u Hz: frames_due=%d sent=%d client_starved=%d presented=%d presented_time_zero=%d mailbox_replaced=%d\n",
         hz, frames, sent, starved, g_npresented, g_nzero, g_replaced);
  static uint64_t iv[4096];
  int ni = 0;
  for (int i = 1; i < g_npresented; i++) iv[ni++] = g_presented[i] - g_presented[i - 1];
  double med = 0;
  pct("interval between presentedTime", iv, ni, &med);
  pct("latency submit->presentedTime", g_latency, g_npresented, NULL);
  pct("latency submit->display-link encode", g_encode, g_npresented, NULL);
  CHECK(g_npresented >= frames * 8 / 10, "only %d/%d frames reached the display at %u Hz", g_npresented, frames, hz);
  CHECK(med > 0 && fabs(med - 1000.0 / hz) < 0.25 * 1000.0 / hz, "median interval %.3f ms vs %.3f ms target", med,
        1000.0 / hz);
}

/* Waits up to ms for an event of the given type; other events go to handle_async. */
static int wait_type(uint32_t type, struct event *out, int ms) {
  uint64_t end = now_ns() + (uint64_t)ms * 1000000ull;
  struct event e;
  while (now_ns() < end) {
    if (!next_event(&e, 20)) continue;
    if (e.type == type) { if (out) *out = e; return 1; }
    handle_async(&e);
  }
  return 0;
}

static void input_checks(int pointer) {
  struct event e;
  /* Scenario 1: key a down/up, shift down/up, left and right click at pixel (10,20), motion, wheel. */
  struct wsd_window_flag inj = {WIN, 1};
  send_msg(WSD_DEBUG_INJECT, &inj, sizeof(inj), NULL, 0, -1);
  int keys = 0, buttons = 0, motions = 0, wheels = 0;
  uint64_t end = now_ns() + 500000000ull;
  while (now_ns() < end) {
    if (!next_event(&e, 20)) continue;
    if (e.type == WSD_EV_KEY) {
      const struct wsd_ev_key *k = (const void *)e.data;
      const char *ch = (const char *)e.data + sizeof(*k);
      int nch = (int)(e.len - sizeof(*k));
      printf("  key keycode=%u flags=0x%x mods=0x%x chars='%.*s'\n", k->mac_keycode, k->flags, k->modifiers, nch, ch);
      if (k->mac_keycode == 0 && nch == 1 && ch[0] == 'a' && (k->flags & WSD_KEY_SYNTHETIC)) keys++;
      if (k->mac_keycode == 56 && (k->flags & WSD_KEY_SYNTHETIC)) keys++;
    } else if (e.type == WSD_EV_BUTTON) {
      const struct wsd_ev_button *b = (const void *)e.data;
      printf("  button %u down=%u at (%.1f,%.1f)\n", b->button, b->down, b->x, b->y);
      if (fabsf(b->x - 10) < 1 && fabsf(b->y - 20) < 1) buttons++;
    } else if (e.type == WSD_EV_MOTION) {
      const struct wsd_ev_motion *m = (const void *)e.data;
      printf("  motion (%.1f,%.1f) flags=0x%x\n", m->x, m->y, m->flags);
      if (fabsf(m->x - 10) < 1 && fabsf(m->y - 20) < 1) motions++;
    } else if (e.type == WSD_EV_WHEEL) {
      const struct wsd_ev_wheel *w = (const void *)e.data;
      printf("  wheel dx=%.2f dy=%.2f precise=%u\n", w->dx, w->dy, w->precise & 1);
      wheels++;
    } else handle_async(&e);
  }
  printf("input scenario 1: keys=%d/4 buttons=%d/4 motion=%d/1 wheel=%d/1\n", keys, buttons, motions, wheels);
  CHECK(keys == 4 && buttons == 4 && motions == 1 && wheels == 1, "synthetic input not fully delivered");

  /* Relative mode: real CGAssociateMouseAndMouseCursorPosition(false) for ~0.3 s, then a raw delta. */
  if (pointer) {
    struct wsd_window_flag rel = {WIN, 1};
    send_msg(WSD_RELATIVE_MOUSE, &rel, sizeof(rel), NULL, 0, -1);
    struct wsd_window_flag inj2 = {WIN, 2};
    send_msg(WSD_DEBUG_INJECT, &inj2, sizeof(inj2), NULL, 0, -1);
    int ok = wait_type(WSD_EV_RELATIVE, &e, 500);
    const struct wsd_ev_relative *r = (const void *)e.data;
    if (ok) printf("relative: dx=%.1f dy=%.1f flags=0x%x\n", r->dx, r->dy, r->flags);
    CHECK(ok && r->dx == 5 && r->dy == -7, "relative delta not delivered");
    usleep(300000);
    rel.value = 0;
    send_msg(WSD_RELATIVE_MOUSE, &rel, sizeof(rel), NULL, 0, -1);
  } else {
    printf("relative: skipped (--no-pointer)\n");
  }

  /* Clip: motion at the window's far edge must come back clamped to the clip rectangle. */
  struct wsd_cursor_clip clip = {WIN, 1, 0, 0, 100, 100};
  send_msg(WSD_CURSOR_CLIP, &clip, sizeof(clip), NULL, 0, -1);
  struct wsd_window_flag inj3 = {WIN, 3};
  send_msg(WSD_DEBUG_INJECT, &inj3, sizeof(inj3), NULL, 0, -1);
  int ok = wait_type(WSD_EV_MOTION, &e, 500);
  const struct wsd_ev_motion *m = (const void *)e.data;
  if (ok) printf("clip: motion (%.1f,%.1f) flags=0x%x\n", m->x, m->y, m->flags);
  CHECK(ok && (m->flags & 1) && m->x <= 99 && m->y <= 99, "clip clamp not applied");
  clip.enable = 0;
  send_msg(WSD_CURSOR_CLIP, &clip, sizeof(clip), NULL, 0, -1);

  /* Cursor visibility round trip (visual effect not verifiable here). */
  struct wsd_window_flag cv = {WIN, 0};
  send_msg(WSD_CURSOR_VISIBLE, &cv, sizeof(cv), NULL, 0, -1);
  cv.value = 1;
  send_msg(WSD_CURSOR_VISIBLE, &cv, sizeof(cv), NULL, 0, -1);

  /* Title */
  struct wsd_window t = {WIN};
  send_msg(WSD_SET_TITLE, &t, sizeof(t), "wsd_client renamed", 18, -1);
}

static void fullscreen_check(uint32_t disp_w, uint32_t disp_h) {
  struct event e;
  struct wsd_window_flag fs = {WIN, 1};
  uint64_t t0 = now_ns();
  send_msg(WSD_WINDOW_FULLSCREEN, &fs, sizeof(fs), NULL, 0, -1);
  int ok = wait_type(WSD_EV_RESIZE, &e, 1000);
  const struct wsd_ev_resize *r = (const void *)e.data;
  if (ok) printf("fullscreen on: %ux%u px after %.1f ms (display %ux%u)\n", r->width_px, r->height_px,
                 (now_ns() - t0) / 1e6, disp_w, disp_h);
  CHECK(ok && r->width_px == disp_w && r->height_px == disp_h, "fullscreen size mismatch");
  /* Present a few frames while fullscreen (scaled by the layer). */
  for (int i = 0; i < 10; i++) {
    struct event ev;
    while (next_event(&ev, 0)) handle_async(&ev);
    int b = -1;
    for (int k = 0; k < NBUF; k++) if (!g_buf[k].busy) { b = k; break; }
    if (b >= 0) {
      draw(g_buf[b].px, (uint64_t)i * 4);
      g_buf[b].busy = 1;
      struct wsd_present p = {WIN, (uint32_t)b + 1, 100000u + i, now_ns()};
      send_msg(WSD_PRESENT, &p, sizeof(p), NULL, 0, -1);
    }
    usleep(16000);
  }
  fs.value = 0;
  t0 = now_ns();
  send_msg(WSD_WINDOW_FULLSCREEN, &fs, sizeof(fs), NULL, 0, -1);
  ok = wait_type(WSD_EV_RESIZE, &e, 1000);
  r = (const void *)e.data;
  if (ok) printf("fullscreen off: %ux%u px after %.1f ms\n", r->width_px, r->height_px, (now_ns() - t0) / 1e6);
  CHECK(ok && r->width_px == g_w && r->height_px == g_h, "windowed size not restored");
}

static void ping_check(void) {
  static uint64_t rt[200];
  struct event e;
  for (int i = 0; i < 200; i++) {
    struct wsd_ping p = {(uint64_t)i, now_ns()};
    send_msg(WSD_PING, &p, sizeof(p), NULL, 0, -1);
    if (!wait_type(WSD_EV_PONG, &e, 500)) { CHECK(0, "pong %d missing", i); return; }
    rt[i] = now_ns() - p.time_ns;
  }
  pct("ping round trip (main-thread server)", rt, 200, NULL);
}

/* ---- render percent ---- */

static int near_to(double a, double b, double tol) { return fabs(a - b) <= tol; }

/* The bounds in points of the first window of process pid that macOS lists on screen. Window geometry needs no screen
 * recording permission. */
static int window_points(pid_t pid, CGRect *out) {
  CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                                               kCGNullWindowID);
  if (!list) return 0;
  int found = 0;
  for (CFIndex i = 0; i < CFArrayGetCount(list) && !found; i++) {
    CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
    CFNumberRef owner = CFDictionaryGetValue(d, kCGWindowOwnerPID);
    CFDictionaryRef b = CFDictionaryGetValue(d, kCGWindowBounds);
    int p = 0;
    CGRect r;
    if (owner && b && CFNumberGetValue(owner, kCFNumberIntType, &p) && p == pid &&
        CGRectMakeWithDictionaryRepresentation(b, &r) && r.size.width > 1) {
      *out = r;
      found = 1;
    }
  }
  CFRelease(list);
  return found;
}

/* Waits up to 1.5 s for the window of pid to be listed with the given width in points (+-1 pt); *r has the last bounds. */
static int window_width_points(pid_t pid, double width, CGRect *r) {
  uint64_t end = now_ns() + 1500000000ull;
  while (now_ns() < end) {
    if (window_points(pid, r) && near_to(r->size.width, width, 1)) return 1;
    usleep(50000);
  }
  return 0;
}

/* Prints the EV_RESIZE e under `what`, checks its size against the size asked for (within tol pixels) and notes a size
 * that is not exact. Returns the payload of e. */
static const struct wsd_ev_resize *check_resize(const struct event *e, const char *what, uint32_t want_w, uint32_t want_h,
                                                double tol) {
  const struct wsd_ev_resize *r = (const void *)e->data;
  printf("%s: %ux%u px scale %.3f (asked %ux%u)\n", what, r->width_px, r->height_px, r->backing_scale_x1000 / 1000.0, want_w,
         want_h);
  CHECK(near_to(r->width_px, want_w, tol) && near_to(r->height_px, want_h, tol), "%s gave %ux%u px, asked %ux%u", what,
        r->width_px, r->height_px, want_w, want_h);
  if (r->width_px != want_w || r->height_px != want_h)
    printf("note: %s did not round trip exactly (%ux%u for %ux%u)\n", what, r->width_px, r->height_px, want_w, want_h);
  return r;
}

/* wsd runs with --render-percent (or without), `percent` being what it must have applied (1..100). The expected values
 * come from CoreGraphics and the formula of PROTOCOL.md (Render percent), not from wsd. */
static void render_check(const struct wsd_hello_reply *hr, int percent, int fullscreen) {
  struct event e;
  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(CGMainDisplayID());
  if (!mode) die("no display mode for the main display");
  double pts_w = (double)CGDisplayModeGetWidth(mode), pts_h = (double)CGDisplayModeGetHeight(mode);
  double native_w = (double)CGDisplayModeGetPixelWidth(mode), native_h = (double)CGDisplayModeGetPixelHeight(mode);
  CGDisplayModeRelease(mode);
  double real = native_w / pts_w;
  double min_scale = fmax(640.0 / pts_w, 480.0 / pts_h);
  double eff = percent < 100 ? fmin(real, fmax(real * percent / 100.0, min_scale)) : real;
  int guarded = percent < 100 && real * percent / 100.0 < min_scale && min_scale < real;
  printf("render check: percent %d, main display %.0fx%.0f pt = %.0fx%.0f px (scale %.3f), min scale %.3f, effective scale "
         "%.3f%s\n",
         percent, pts_w, pts_h, native_w, native_h, real, min_scale, eff, guarded ? " (held by the 640x480 guard)" : "");

  CHECK(hr->backing_scale_x1000 == (uint32_t)lround(eff * 1000), "HELLO scale %u, expected %ld", hr->backing_scale_x1000,
        lround(eff * 1000));
  CHECK(near_to(hr->display_width_px, pts_w * eff, 1) && near_to(hr->display_height_px, pts_h * eff, 1),
        "HELLO display %ux%u px, expected %.0fx%.0f", hr->display_width_px, hr->display_height_px, pts_w * eff,
        pts_h * eff);
  if (native_w >= 640 && native_h >= 480)
    CHECK(hr->display_width_px + 1 >= 640 && hr->display_height_px + 1 >= 480,
          "HELLO display %ux%u px is smaller than 640x480", hr->display_width_px, hr->display_height_px);
  CHECK(hr->work_width > 0 && hr->work_width <= hr->display_width_px && hr->work_height > 0 &&
            hr->work_height <= hr->display_height_px,
        "HELLO work area %ux%u in a display of %ux%u", hr->work_width, hr->work_height, hr->display_width_px,
        hr->display_height_px);

  /* A window of win_w x win_h px, then moved and resized with SET_FRAME. Its size in pixels must come back with the
   * effective scale, and macOS must give it width / eff points. macOS rounds a window to whole points, and a point is
   * eff pixels, so the size is right within ceil(eff) pixels. Below 1 pixel per point a pixel is more than a point, so
   * the windows are small enough to fit the screen in points. */
  double size_tol = ceil(eff);
  uint32_t win_w = eff < 1 ? 200 : g_w, win_h = eff < 1 ? 150 : g_h;
  struct wsd_window_create wc = {WIN, win_w, win_h, WSD_STYLE_TITLED | WSD_STYLE_CLOSABLE | WSD_STYLE_RESIZABLE};
  send_msg(WSD_WINDOW_CREATE, &wc, sizeof(wc), "wsd_client", 10, -1);
  struct wsd_window_flag show = {WIN, 1};
  send_msg(WSD_WINDOW_SHOW, &show, sizeof(show), NULL, 0, -1);
  if (!wait_type(WSD_EV_RESIZE, &e, 2000)) die("no RESIZE after show");
  const struct wsd_ev_resize *rs = check_resize(&e, "window shown", win_w, win_h, size_tol);
  CHECK(rs->backing_scale_x1000 == hr->backing_scale_x1000, "RESIZE scale %u, HELLO scale %u", rs->backing_scale_x1000,
        hr->backing_scale_x1000);

  struct wsd_window_frame fr = {WIN, WSD_FRAME_MOVE | WSD_FRAME_SIZE, 200, 200, 800, 450};
  if (eff < 1) fr = (struct wsd_window_frame){WIN, WSD_FRAME_MOVE | WSD_FRAME_SIZE, 100, 100, 300, 200};
  send_msg(WSD_WINDOW_SET_FRAME, &fr, sizeof(fr), NULL, 0, -1);
  if (!wait_type(WSD_EV_RESIZE, &e, 2000)) die("no RESIZE after SET_FRAME");
  check_resize(&e, "set frame", fr.width, fr.height, size_tol);

  CGRect b = {{0, 0}, {0, 0}};
  int listed = window_width_points((pid_t)hr->server_pid, fr.width / eff, &b);
  printf("window on screen: %.1f pt wide at x %.1f (expected %.1f pt wide at x %.1f)\n", b.size.width, b.origin.x,
         fr.width / eff, fr.x / eff);
  CHECK(listed, "macOS lists no window of wsd %.1f pt wide", fr.width / eff);
  CHECK(near_to(b.origin.x, fr.x / eff, 1), "window at x %.1f pt, expected %.1f", b.origin.x, fr.x / eff);

  if (fullscreen) {
    struct wsd_window_flag fs = {WIN, 1};
    send_msg(WSD_WINDOW_FULLSCREEN, &fs, sizeof(fs), NULL, 0, -1);
    int ok = wait_type(WSD_EV_RESIZE, &e, 1500);
    CHECK(ok, "no RESIZE after fullscreen");
    if (ok) check_resize(&e, "fullscreen on", hr->display_width_px, hr->display_height_px, 0);
    /* Leaving fullscreen is not checked: here (wsd not the active app) it gives the window back the size of the screen's
     * visible frame instead of its own, as the wsd built before the render percent does. */
    fs.value = 0;
    send_msg(WSD_WINDOW_FULLSCREEN, &fs, sizeof(fs), NULL, 0, -1);
    if (wait_type(WSD_EV_RESIZE, &e, 1500)) {
      const struct wsd_ev_resize *off = (const void *)e.data;
      printf("fullscreen off: %ux%u px (not checked)\n", off->width_px, off->height_px);
    }
  }

  /* The server handles messages in order: an error for any of them arrives before the PONG. */
  struct wsd_window wd = {WIN};
  send_msg(WSD_WINDOW_DESTROY, &wd, sizeof(wd), NULL, 0, -1);
  struct wsd_ping p = {0, now_ns()};
  send_msg(WSD_PING, &p, sizeof(p), NULL, 0, -1);
  CHECK(wait_type(WSD_EV_PONG, &e, 500), "no PONG after WINDOW_DESTROY");
  CHECK(g_errors == 0, "%d server errors", g_errors);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <socket> [seconds] [rates] [--no-pointer]\n", argv[0]); return 2; }
  int render_percent = -1, render_fullscreen = 0; /* render_percent: the --render-check value, -1 without the option */
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--render-check")) render_percent = i + 1 < argc ? atoi(argv[++i]) : 0;
    else if (!strcmp(argv[i], "--fullscreen")) render_fullscreen = 1;
  }
  if (render_percent >= 0 && (render_percent < 1 || render_percent > 100)) {
    fprintf(stderr, "usage: %s <socket> --render-check <percent 1..100> [--fullscreen]\n", argv[0]);
    return 2;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  double seconds = argc > 2 ? atof(argv[2]) : 4.0;
  if (seconds <= 0 || seconds > 10) seconds = 4.0;
  const char *rates = argc > 3 && argv[3][0] != '-' ? argv[3] : "60,120";
  int pointer = 1;
  for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--no-pointer")) pointer = 0;

  struct sockaddr_un sa = {.sun_family = AF_UNIX};
  if (strlen(argv[1]) >= sizeof(sa.sun_path)) die("socket path too long");
  strcpy(sa.sun_path, argv[1]);
  g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (g_sock < 0 || connect(g_sock, (struct sockaddr *)&sa, sizeof(sa))) die("connect");
  int one = 1;
  setsockopt(g_sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));

  struct event e;
  struct wsd_hello hello = {WSD_PROTOCOL_VERSION, (uint32_t)getpid()};
  send_msg(WSD_HELLO, &hello, sizeof(hello), NULL, 0, -1);
  if (!wait_type(WSD_EV_HELLO, &e, 2000)) die("no HELLO reply");
  struct wsd_hello_reply hr;
  memcpy(&hr, e.data, sizeof(hr));
  printf("hello: version=%u server_pid=%u display=%ux%u px max_refresh=%u Hz scale=%.3f\n", hr.version, hr.server_pid,
         hr.display_width_px, hr.display_height_px, hr.max_refresh_hz, hr.backing_scale_x1000 / 1000.0);

  if (render_percent >= 0) {
    render_check(&hr, render_percent, render_fullscreen);
    close(g_sock);
    printf("RESULT %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
  }

  struct wsd_window_create wc = {WIN, g_w, g_h, WSD_STYLE_TITLED | WSD_STYLE_CLOSABLE | WSD_STYLE_RESIZABLE | WSD_STYLE_TOPMOST};
  send_msg(WSD_WINDOW_CREATE, &wc, sizeof(wc), "wsd_client", 10, -1);
  struct wsd_window_flag show = {WIN, 1};
  send_msg(WSD_WINDOW_SHOW, &show, sizeof(show), NULL, 0, -1);
  if (!wait_type(WSD_EV_RESIZE, &e, 2000)) die("no RESIZE after show");
  const struct wsd_ev_resize *rs = (const void *)e.data;
  printf("window shown: %ux%u px scale=%.3f\n", rs->width_px, rs->height_px, rs->backing_scale_x1000 / 1000.0);
  CHECK(rs->width_px == g_w && rs->height_px == g_h, "window size %ux%u", rs->width_px, rs->height_px);

  size_t sz = (size_t)g_w * g_h * 4;
  for (int i = 0; i < NBUF; i++) {
    char name[32];
    snprintf(name, sizeof(name), "/wsdc-%d-%d", getpid(), i);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) die("shm_open");
    shm_unlink(name);
    if (ftruncate(fd, (off_t)sz)) die("ftruncate");
    g_buf[i].fd = fd;
    g_buf[i].px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_buf[i].px == MAP_FAILED) die("mmap");
    struct wsd_buffer_attach a = {(uint32_t)i + 1, g_w, g_h, g_w * 4, WSD_FMT_BGRA8, 0, 0, sz};
    send_msg(WSD_BUFFER_ATTACH, &a, sizeof(a), NULL, 0, fd);
  }

  static uint64_t pres[4096], lat[4096], enc[4096];
  g_presented = pres; g_latency = lat; g_encode = enc;
  char rbuf[64];
  snprintf(rbuf, sizeof(rbuf), "%s", rates);
  for (char *tok = strtok(rbuf, ","); tok; tok = strtok(NULL, ",")) {
    uint32_t hz = (uint32_t)atoi(tok);
    if (hz >= 1 && hz <= 240) run_rate(hz, seconds);
  }
  g_presented = NULL;

  input_checks(pointer);
  fullscreen_check(hr.display_width_px, hr.display_height_px);
  ping_check();

  /* Drain, destroy, disconnect. */
  uint64_t end = now_ns() + 200000000ull;
  while (now_ns() < end) if (next_event(&e, 20)) handle_async(&e);
  struct wsd_window wd = {WIN};
  send_msg(WSD_WINDOW_DESTROY, &wd, sizeof(wd), NULL, 0, -1);
  CHECK(g_errors == 0, "%d server errors", g_errors);
  close(g_sock);
  printf("RESULT %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail;
}
