/*
 * wsd_audio_input.c - native witness for wsd's audio input (WSD_AUDIO_OPEN_INPUT).
 *
 * Opens an input stream in several client formats, records one second of the default microphone through the shared
 * ring, and checks that about `rate` frames arrived and that the old-style RESET is refused. Prints the peak of each
 * recording: all zero means the microphone is silent or macOS has not allowed the microphone (System Settings >
 * Privacy & Security > Microphone) for the process that started wsd.
 *
 * Usage: wsd_audio_input <socket>   (run.sh starts wsd and calls this)
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../wsd/wsd_protocol.h"

static int g_sock = -1, g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { printf("CHECK FAILED (%s): ", #cond); printf(__VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)

static void send_msg(uint32_t type, const void *p, uint32_t n, int fd) {
  uint8_t buf[sizeof(struct wsd_hdr) + 64];
  struct wsd_hdr h = {type, n};
  memcpy(buf, &h, sizeof(h));
  memcpy(buf + sizeof(h), p, n);
  struct iovec iov = {buf, sizeof(h) + n};
  char ctl[CMSG_SPACE(sizeof(int))] = {0};
  struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1};
  if (fd >= 0) {
    m.msg_control = ctl;
    m.msg_controllen = sizeof(ctl);
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
  }
  if (sendmsg(g_sock, &m, 0) != (ssize_t)iov.iov_len) { perror("sendmsg"); exit(1); }
}

static int read_all(void *p, size_t n) {
  for (uint8_t *b = p; n;) {
    ssize_t r = read(g_sock, b, n);
    if (r <= 0) return -1;
    b += r;
    n -= (size_t)r;
  }
  return 0;
}

/* Reads events until the PONG of `token`; returns the code of an EV_ERROR for `request` (0 if none). */
static int sync_ping(uint32_t request, uint64_t token) {
  struct wsd_ping ping = {token, 0};
  int code = 0;
  send_msg(WSD_PING, &ping, sizeof(ping), -1);
  for (;;) {
    struct wsd_hdr h;
    uint8_t payload[WSD_MAX_PAYLOAD];
    if (read_all(&h, sizeof(h)) || h.len > sizeof(payload) || read_all(payload, h.len)) { printf("FATAL: connection lost\n"); exit(1); }
    if (h.type == WSD_EV_ERROR && ((struct wsd_ev_error *)payload)->request_type == request) code = ((struct wsd_ev_error *)payload)->code;
    if (h.type == WSD_EV_PONG && ((struct wsd_ping *)payload)->token == token) return code;
  }
}

static float sample_at(const uint8_t *p, uint32_t format) {
  switch (format) {
  case WSD_AUDIO_F32: { float v; memcpy(&v, p, 4); return v; }
  case WSD_AUDIO_S16: { int16_t v; memcpy(&v, p, 2); return v / 32768.f; }
  case WSD_AUDIO_S24: return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.f;
  case WSD_AUDIO_S32: { int32_t v; memcpy(&v, p, 4); return v / 2147483648.f; }
  default: return (p[0] - 128) / 128.f;
  }
}

static void record(uint32_t id, uint32_t format, uint32_t sample, uint32_t rate, uint32_t channels) {
  uint32_t ring_frames = rate / 2, frame_bytes = sample * channels;
  size_t size = sizeof(struct wsd_audio_ring) + (size_t)ring_frames * frame_bytes, file = (size + 0xffff) & ~(size_t)0xffff;
  char name[32];
  snprintf(name, sizeof(name), "/wai-%d-%u", getpid() % 100000, id);
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0 || shm_unlink(name) || ftruncate(fd, (off_t)file)) { perror("shm"); exit(1); }
  struct wsd_audio_ring *ring = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ring == MAP_FAILED) { perror("mmap"); exit(1); }
  const uint8_t *data = (const uint8_t *)ring + sizeof(*ring);
  struct wsd_audio_open o = {id, format, rate, channels, frame_bytes, ring_frames, 0, size};
  send_msg(WSD_AUDIO_OPEN_INPUT, &o, sizeof(o), fd);
  close(fd);
  int e = sync_ping(WSD_AUDIO_OPEN_INPUT, id * 10);
  CHECK(e == 0, "open input format %u %u Hz %u ch: wsd answered %d", format, rate, channels, e);
  if (e) return;
  struct wsd_audio_stream s = {id, 0};
  send_msg(WSD_AUDIO_START, &s, sizeof(s), -1);
  _Atomic uint64_t *wp = (_Atomic uint64_t *)&ring->write_frames, *rp = (_Atomic uint64_t *)&ring->read_frames;
  uint64_t read = 0, total = 0;
  float peak = 0;
  for (int i = 0; i < 100; i++) { /* the client reads like a WASAPI client would: 10 ms at a time */
    usleep(10000);
    uint64_t w = atomic_load_explicit(wp, memory_order_acquire);
    for (; read < w; read++) {
      for (uint32_t c = 0; c < channels; c++) {
        float v = fabsf(sample_at(data + (size_t)(read % ring_frames) * frame_bytes + c * sample, format));
        if (v > peak) peak = v;
      }
      total++;
    }
    atomic_store_explicit(rp, read, memory_order_release);
  }
  send_msg(WSD_AUDIO_STOP, &s, sizeof(s), -1);
  sync_ping(WSD_AUDIO_STOP, id * 10 + 1);
  CHECK(total > rate * 0.7 && total < rate * 1.3, "format %u %u Hz %u ch: %llu frames in one second", format, rate, channels, total);
  printf("format %u, %u Hz, %u ch: %llu frames, peak %.4f, dropped %llu\n", format, rate, channels, total, peak, ring->underrun_frames);
  send_msg(WSD_AUDIO_RESET, &s, sizeof(s), -1);
  CHECK(sync_ping(WSD_AUDIO_RESET, id * 10 + 2) == -EINVAL, "RESET of an input stream must be refused");
  send_msg(WSD_AUDIO_CLOSE, &s, sizeof(s), -1);
  sync_ping(WSD_AUDIO_CLOSE, id * 10 + 3);
  munmap(ring, size);
}

int main(int argc, char **argv) {
  if (argc < 2) { printf("usage: %s <socket>\n", argv[0]); return 2; }
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  snprintf(a.sun_path, sizeof(a.sun_path), "%s", argv[1]);
  g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (connect(g_sock, (struct sockaddr *)&a, sizeof(a))) { perror("connect"); return 1; }
  struct wsd_hello hello = {WSD_PROTOCOL_VERSION, (uint32_t)getpid()};
  send_msg(WSD_HELLO, &hello, sizeof(hello), -1);
  for (;;) {
    struct wsd_hdr h;
    uint8_t payload[WSD_MAX_PAYLOAD];
    if (read_all(&h, sizeof(h)) || read_all(payload, h.len)) return 1;
    if (h.type == WSD_EV_HELLO) break;
  }
  record(1, WSD_AUDIO_S16, 2, 48000, 1);
  record(2, WSD_AUDIO_F32, 4, 44100, 2);
  record(3, WSD_AUDIO_S24, 3, 16000, 1);
  record(4, WSD_AUDIO_U8, 1, 8000, 1);
  record(5, WSD_AUDIO_S32, 4, 48000, 2);
  printf("RESULT %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail;
}
