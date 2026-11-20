/*
 * wsd_audio_output.c - native witness for wsd's audio output (WSD_AUDIO_OPEN) on a device that is slow to start.
 *
 * Plays silence the way Wine's audio driver does, 10 ms at a time with about 30 ms queued, and gives a stream up the
 * way Rocket League does: a stream whose frames nobody has read for 300 ms is closed and opened again. Checks that
 * the frames of a started stream are read at the stream's rate, by the clock while the device starts and by the
 * device afterwards and never by both, so that no stream is given up, and that a stopped stream keeps its frames.
 *
 * Usage: wsd_audio_output <socket>   (run.sh starts wsd on a slow device, then on a ready one, and calls this)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../../wsd/wsd_protocol.h"

static int g_sock = -1, g_fail;
static uint64_t g_token;

#define CHECK(cond, ...) do { if (!(cond)) { printf("CHECK FAILED (%s): ", #cond); printf(__VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

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

/* Waits until wsd has handled what was sent; returns the code of an EV_ERROR for `request` (0 if none). */
static int sync_ping(uint32_t request) {
  struct wsd_ping ping = {++g_token, 0};
  int code = 0;
  send_msg(WSD_PING, &ping, sizeof(ping), -1);
  for (;;) {
    struct wsd_hdr h;
    uint8_t payload[WSD_MAX_PAYLOAD];
    if (read_all(&h, sizeof(h)) || h.len > sizeof(payload) || read_all(payload, h.len)) { printf("FATAL: connection lost\n"); exit(1); }
    if (h.type == WSD_EV_ERROR && ((struct wsd_ev_error *)payload)->request_type == request) code = ((struct wsd_ev_error *)payload)->code;
    if (h.type == WSD_EV_PONG && ((struct wsd_ping *)payload)->token == g_token) return code;
  }
}

enum { RATE = 48000, CHANNELS = 2, RING_FRAMES = 4800, PERIOD = 480, GIVE_UP_MS = 300 };

struct stream {
  uint32_t id;
  struct wsd_audio_ring *ring;
  size_t size;
  uint64_t written;
};

static struct stream open_stream(uint32_t id) {
  struct stream s = {.id = id, .size = sizeof(struct wsd_audio_ring) + (size_t)RING_FRAMES * CHANNELS * sizeof(float)};
  char name[32];
  snprintf(name, sizeof(name), "/wao-%d-%u", getpid() % 100000, id);
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0 || shm_unlink(name) || ftruncate(fd, (off_t)((s.size + 0xffff) & ~(size_t)0xffff))) { perror("shm"); exit(1); }
  s.ring = mmap(NULL, s.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (s.ring == MAP_FAILED) { perror("mmap"); exit(1); }
  struct wsd_audio_open o = {id, WSD_AUDIO_F32, RATE, CHANNELS, CHANNELS * sizeof(float), RING_FRAMES, 0, s.size};
  send_msg(WSD_AUDIO_OPEN, &o, sizeof(o), fd);
  close(fd);
  int e = sync_ping(WSD_AUDIO_OPEN);
  CHECK(e == 0, "open stream %u: wsd answered %d", id, e);
  return s;
}

static void command(const struct stream *s, uint32_t type) {
  struct wsd_audio_stream m = {s->id, 0};
  send_msg(type, &m, sizeof(m), -1);
}

static void close_stream(struct stream *s) {
  command(s, WSD_AUDIO_CLOSE);
  munmap(s->ring, s->size);
}

static uint64_t frames_read(const struct stream *s) {
  return atomic_load_explicit((_Atomic uint64_t *)&s->ring->read_frames, memory_order_acquire);
}

/* Queues silence up to three periods ahead of what was read: the ring is zero-filled, only the count moves. */
static void feed(struct stream *s) {
  uint64_t read = frames_read(s);
  while (s->written - read < 3 * PERIOD) s->written += PERIOD;
  atomic_store_explicit((_Atomic uint64_t *)&s->ring->write_frames, s->written, memory_order_release);
}

/* Plays for `ms`. Returns 0, or 1 when the stream was given up: nothing was read for GIVE_UP_MS. */
static int play(struct stream *s, double ms, double *first_read_ms, double *longest_wait_ms) {
  double start = now_ms(), last_progress = start;
  uint64_t last = frames_read(s);
  for (;;) {
    feed(s);
    usleep(10000);
    double t = now_ms();
    uint64_t read = frames_read(s);
    if (read != last) {
      if (first_read_ms && *first_read_ms < 0) *first_read_ms = t - start;
      last = read;
      last_progress = t;
    }
    if (longest_wait_ms && t - last_progress > *longest_wait_ms) *longest_wait_ms = t - last_progress;
    if (t - last_progress >= GIVE_UP_MS) return 1;
    if (t - start >= ms) return 0;
  }
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

  /* The game: five seconds of sound, on as many streams as it takes. */
  uint32_t id = 0, given_up = 0;
  double first = -1, longest = 0, begin = now_ms();
  while (now_ms() - begin < 5000) {
    struct stream s = open_stream(++id);
    command(&s, WSD_AUDIO_START);
    double first_here = -1;
    given_up += (uint32_t)play(&s, 5000 - (now_ms() - begin), &first_here, &longest);
    if (id == 1) first = first_here;
    close_stream(&s);
  }
  printf("game: %u stream(s), %u given up, first frames read after %.0f ms, longest wait %.0f ms\n", id, given_up, first, longest);
  CHECK(given_up == 0, "%u streams went unread for %d ms and were opened again", given_up, GIVE_UP_MS);
  CHECK(first >= 0 && first < 200, "the first stream's frames were first read after %.0f ms", first);

  /* A started stream is read at its rate: before a slow device plays (0.2 s to 0.6 s after the start) as after. */
  struct stream s = open_stream(++id);
  command(&s, WSD_AUDIO_START);
  CHECK(play(&s, 200, NULL, NULL) == 0, "the stream went unread");
  uint64_t f0 = frames_read(&s);
  double t0 = now_ms();
  CHECK(play(&s, 400, NULL, NULL) == 0, "the stream went unread");
  uint64_t f1 = frames_read(&s);
  double t1 = now_ms();
  CHECK(play(&s, 600, NULL, NULL) == 0, "the stream went unread");
  uint64_t f2 = frames_read(&s);
  double t2 = now_ms();
  CHECK(play(&s, 1000, NULL, NULL) == 0, "the stream went unread");
  uint64_t f3 = frames_read(&s);
  double t3 = now_ms(), early = (f1 - f0) / ((t1 - t0) / 1000), late = (f3 - f2) / ((t3 - t2) / 1000);
  printf("rate: %.0f frames/s from 0.2 s to 0.6 s, %.0f frames/s from 1.2 s to 2.2 s\n", early, late);
  CHECK(early > RATE * 0.85 && early < RATE * 1.15, "%.0f frames/s read from 0.2 s to 0.6 s after the start", early);
  CHECK(late > RATE * 0.9 && late < RATE * 1.1, "%.0f frames/s read once the device played", late);

  /* A stopped stream keeps its frames, and plays them when it starts again. */
  command(&s, WSD_AUDIO_STOP);
  sync_ping(WSD_AUDIO_STOP);
  usleep(50000); /* a render under way ends */
  feed(&s);
  uint64_t stopped = frames_read(&s);
  usleep(400000);
  CHECK(frames_read(&s) == stopped, "%llu frames were read from a stopped stream", frames_read(&s) - stopped);
  command(&s, WSD_AUDIO_START);
  CHECK(play(&s, 500, NULL, NULL) == 0, "the stream went unread after its second start");
  close_stream(&s);

  /* Streams opened and closed faster than the device starts leave the next one readable at once. */
  for (int i = 0; i < 8; i++) {
    struct stream quick = open_stream(++id);
    command(&quick, WSD_AUDIO_START);
    feed(&quick);
    usleep(20000);
    close_stream(&quick);
  }
  s = open_stream(++id);
  command(&s, WSD_AUDIO_START);
  first = -1;
  CHECK(play(&s, 1000, &first, NULL) == 0, "the stream after eight short ones went unread");
  printf("after eight short streams: first frames read after %.0f ms\n", first);
  CHECK(first >= 0 && first < 200, "the frames were first read after %.0f ms", first);
  close_stream(&s);
  sync_ping(WSD_AUDIO_CLOSE);

  printf("RESULT %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail;
}
