/*
 * wsd.m - Obelisk window server spike: native arm64 Cocoa process that owns NSWindows,
 * presents client frames from shared memory through Metal, and forwards input events.
 * Wire format: wsd_protocol.h, PROTOCOL.md.
 *
 * Threading: everything runs on the main queue (AppKit + socket I/O through GCD sources), except the CoreAudio
 * calls (g_audio_queue); Metal completion/presented handlers and audio errors hop back to the main queue.
 * Present path: shm fd -> mmap(MAP_SHARED) -> newBufferWithBytesNoCopy -> linear MTLTexture
 * (no CPU pixel copy) -> one render pass per display-link tick into the CAMetalLayer drawable.
 *
 * Usage: wsd --dir <private-dir> [--exit-on-disconnect] [--render-percent N]
 */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <Carbon/Carbon.h> /* kVK_* */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <AudioToolbox/AudioToolbox.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <stdatomic.h>
#include "wsd_protocol.h"

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

#define LOG(...) do { fprintf(stderr, "wsd: " __VA_ARGS__); fputc('\n', stderr); } while (0)

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static id<MTLRenderPipelineState> g_pipe;
static BOOL g_exit_on_disconnect;
static int g_render_percent = 100; /* --render-percent, 1 to 100 (PROTOCOL.md, "Render percent") */
static BOOL g_injecting;       /* set while DEBUG_INJECT feeds synthetic events */
static BOOL g_relative_global; /* CGAssociateMouseAndMouseCursorPosition(false) in effect */
static BOOL g_cursor_hide_wanted; /* the client asked for no cursor (WSD_CURSOR_VISIBLE 0) */
static int g_cursor_hidden;       /* [NSCursor hide] in effect */
static NSMutableSet *g_clients;
static BOOL g_caps_told;           /* the Caps Lock state the clients were brought to (-syncCapsLock:) */
static NSTimeInterval g_warp_time; /* uptime of the last CURSOR_WARP: the mouse moves queued before it are dropped */
static NSCursor *g_cursor;         /* the cursor over the windows (CURSOR_SET), nil for the arrow */
static BOOL g_ever_active;         /* wsd has been the active app since it started */
static BOOL g_terminate_pending;   /* a logout, restart or shutdown waits for the clients to leave */
static id<NSObject> g_app_nap_off;  /* the activity that keeps App Nap off for as long as wsd runs */

/* A window holding the pointer that shows no new frame for this long gives it back (see -checkStalled:). */
#define STALL_NS (5 * NSEC_PER_SEC)

static void restore_input_state(void) {
  if (g_relative_global) {
    CGAssociateMouseAndMouseCursorPosition(true);
    g_relative_global = NO;
  }
  g_cursor_hide_wanted = NO;
  while (g_cursor_hidden > 0) {
    [NSCursor unhide];
    g_cursor_hidden--;
  }
}

static NSString *const kShader =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VO { float4 pos [[position]]; float2 uv; };\n"
     "vertex VO vmain(uint vid [[vertex_id]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
     "  VO o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.uv = float2(p.x, 1.0 - p.y); return o; }\n"
     "fragment float4 fmain(VO in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
     "  constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
     "  return t.sample(s, in.uv); }\n";

@class WsdClient;

@interface WsdBuffer : NSObject
@property uint32_t bufferId, width, height, stride, format;
@property BOOL detached; /* BUFFER_DETACH received: never redisplayed */
@property uint8_t *cpu;   /* CPU view of the shared pixels (first pixel), for the latency probe */
@property(strong) id<MTLTexture> texture;
@end
@implementation WsdBuffer
@end

/* Input latency probe (test only, WSD_LATENCY_PROBE=1 or the first DEBUG_INPUT): a DEBUG_INPUT arms it; every overlay frame received after that
 * is compared on a coarse luminance grid with the frame received just before the input. The first frame whose
 * difference rises clearly above the frame-to-frame noise is the game's visible reaction. Times use now_ns(), which is
 * also the guest CLOCK_MONOTONIC under LKD, so submit times from nptunix are on the same clock. */
#define PROBE_GX 48
#define PROBE_GY 27
static BOOL g_probe_enabled;
static uint64_t g_probe_t0;
static int g_probe_frames, g_probe_reacted;
static float g_probe_base[PROBE_GX * PROBE_GY], g_probe_last[PROBE_GX * PROBE_GY];
static BOOL g_probe_have_last;
static double g_probe_noise;

static void probe_signature(WsdBuffer *b, float *sig) {
  for (int gy = 0; gy < PROBE_GY; gy++)
    for (int gx = 0; gx < PROBE_GX; gx++) {
      uint32_t x0 = (uint32_t)((gx + 0.5) * b.width / PROBE_GX), y0 = (uint32_t)((gy + 0.5) * b.height / PROBE_GY);
      float acc = 0;
      for (int dy = 0; dy < 4; dy++)
        for (int dx = 0; dx < 4; dx++) {
          uint32_t x = MIN(x0 + dx * 2, b.width - 1), y = MIN(y0 + dy * 2, b.height - 1);
          const uint8_t *px = b.cpu + (size_t)y * b.stride + (size_t)x * 4;
          acc += px[0] + px[1] + px[2];
        }
      sig[gy * PROBE_GX + gx] = acc / 48.0f;
    }
}

static double probe_diff(const float *a, const float *b) {
  double s = 0;
  for (int i = 0; i < PROBE_GX * PROBE_GY; i++) s += fabs(a[i] - b[i]);
  return s / (PROBE_GX * PROBE_GY);
}

static void probe_arm(const char *what) {
  if (!g_probe_enabled) return;
  g_probe_t0 = now_ns();
  g_probe_frames = 0;
  g_probe_reacted = 0;
  memcpy(g_probe_base, g_probe_last, sizeof(g_probe_base));
  fprintf(stderr, "wsd: probe: armed by %s at %llu (noise %.2f)\n", what, (unsigned long long)g_probe_t0, g_probe_noise);
}

static void probe_frame(WsdBuffer *b, uint64_t client_ns, uint64_t rx_ns) {
  if (!g_probe_enabled || !b.cpu) return;
  float sig[PROBE_GX * PROBE_GY];
  probe_signature(b, sig);
  double dprev = g_probe_have_last ? probe_diff(sig, g_probe_last) : 0;
  if (g_probe_t0 && g_probe_frames < 40) {
    double dbase = probe_diff(sig, g_probe_base);
    double thr = MAX(4 * g_probe_noise, 1.5);
    g_probe_frames++;
    BOOL react = !g_probe_reacted && dbase > thr;
    if (react) g_probe_reacted = 1;
    fprintf(stderr, "wsd: probe: frame %d submit %+.1f ms received %+.1f ms diff %.2f (prev %.2f, thr %.2f)%s\n",
            g_probe_frames, ((double)client_ns - (double)g_probe_t0) / 1e6, ((double)rx_ns - (double)g_probe_t0) / 1e6,
            dbase, dprev, thr, react ? " REACT" : "");
    if (g_probe_frames == 40) g_probe_t0 = 0;
  } else {
    g_probe_noise = g_probe_have_last ? 0.9 * g_probe_noise + 0.1 * dprev : 0;
  }
  memcpy(g_probe_last, sig, sizeof(sig));
  g_probe_have_last = YES;
}

@interface WsdView : NSView
@property(weak) id owner; /* WsdWindow */
@end

/* Wine draws the frame of most game windows itself, so they are created borderless; a borderless NSWindow refuses
 * key and main status by default and then never receives keyboard events (2026-10-03: Rocket League got no keys
 * from the real keyboard, key=0 in every window state, while injected events sent to the window directly worked). */
@interface WsdNSWindow : NSWindow
/* WSD_STYLE_NOACTIVATE (WS_EX_NOACTIVATE): a click on the window leaves the keyboard where it was */
@property BOOL noActivate;
@end
@implementation WsdNSWindow
- (BOOL)canBecomeKeyWindow { return !self.noActivate; }
- (BOOL)canBecomeMainWindow { return !self.noActivate; }
@end

@interface WsdWindow : NSObject <NSWindowDelegate>
@property uint32_t windowId;
@property(weak) WsdClient *client;
@property(strong) NSWindow *window;
@property(strong) WsdView *view;
@property(strong) CAMetalLayer *layer;
@property(strong) CADisplayLink *link;
/* pending (mailbox) frame */
@property(strong) WsdBuffer *pendingBuffer;
/* last encoded buffer, shown again (without events) when its frame was never displayed, e.g. the first frame
 * after WINDOW_SHOW of a window that is not yet on screen, while the client has nothing newer to present */
@property(strong) WsdBuffer *lastBuffer;
@property BOOL redisplay;
@property int redisplayTries;
@property uint64_t pendingSerial, pendingClientNs, pendingReceivedNs;
@property uint32_t replaced, nPresented, nZero;
/* present interval statistics (logged every 120 shown frames per plane) */
@property uint64_t statLastNs, statSumNs, statMaxNs, statMinNs;
@property uint32_t statCount;
@property BOOL relative, clip, fullscreen, transitioning;
@property NSRect clipPx;
/* when the window last showed a new frame on either plane, and whether it has held the pointer without one for
 * STALL_NS since (the pointer and the cursor are then the user's) */
@property uint64_t shownNs;
@property BOOL stalled;
/* overlay plane (PRESENT_REGION, e.g. a D3D swapchain in the client area): a WsdWindow without an NSWindow of its
 * own, whose layer is a sublayer of the view's layer */
@property(strong) WsdWindow *overlay;
@property BOOL isOverlay;
@property(weak) WsdWindow *base; /* an overlay's window */
@property NSRect overlayPx;
@property NSRect savedFrame;
@property NSWindowStyleMask savedStyle;
/* WINDOW_MINIMIZE ordered it out (no minimize button, so not in the Dock): it shows again when restored */
@property BOOL minimizedOut;
@property BOOL topmost; /* WSD_STYLE_TOPMOST (HWND_TOPMOST): above the windows that are not */
/* the mac keycodes the client was last told are down */
@property(strong) NSMutableIndexSet *heldKeys;
- (void)sendEvent:(uint32_t)type payload:(const void *)p len:(uint32_t)n extra:(NSData *)extra;
- (void)noteKey:(uint32_t)code down:(BOOL)down;
- (NSWindowLevel)levelForActive:(BOOL)active;
- (BOOL)floatsForActive:(BOOL)active;
- (void)orderInFront:(BOOL)activate;
- (void)syncCapsLock:(BOOL)on;
- (void)fitScreen;
@end

static __weak WsdWindow *g_focus_wanted; /* WINDOW_FOCUS while wsd was not active: made key once it is */

/* Audio output stream (WSD_AUDIO_*): one default-output AudioUnit per stream, whose render callback reads the
 * client's shared ring directly (no copy in between, no message per buffer). CoreAudio converts the client format
 * and rate to the device and mixes the streams. */
struct wsd_audio_rt {
  struct wsd_audio_ring *ring;
  uint8_t *data;
  uint32_t ring_frames, frame_bytes;
  uint8_t silence;
  _Atomic int running;
  /* input streams only */
  AudioComponentInstance unit; /* the callback pulls the captured frames from it */
  AudioConverterRef conv;      /* device float frames to the client's rate, channels and sample format */
  uint32_t channels, dev_channels, scratch_frames, out_frames;
  float *scratch;              /* one render of the device */
  uint8_t *out;                /* what conv made of it, in the client's format */
};

static OSStatus audio_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                             UInt32 frames, AudioBufferList *io) {
  struct wsd_audio_rt *rt = ref;
  uint8_t *out = io->mBuffers[0].mData;
  uint32_t want = frames, got = 0;
  (void)ts; (void)bus;
  if (atomic_load_explicit(&rt->running, memory_order_acquire)) {
    _Atomic uint64_t *wp = (_Atomic uint64_t *)&rt->ring->write_frames, *rp = (_Atomic uint64_t *)&rt->ring->read_frames;
    uint64_t w = atomic_load_explicit(wp, memory_order_acquire), r = atomic_load_explicit(rp, memory_order_relaxed);
    uint64_t avail = w > r ? w - r : 0;
    if (avail > rt->ring_frames) avail = rt->ring_frames; /* a misbehaving client cannot make us read out of range */
    got = avail < want ? (uint32_t)avail : want;
    for (uint32_t done = 0; done < got;) {
      uint32_t pos = (uint32_t)((r + done) % rt->ring_frames), n = rt->ring_frames - pos;
      if (n > got - done) n = got - done;
      memcpy(out + (size_t)done * rt->frame_bytes, rt->data + (size_t)pos * rt->frame_bytes, (size_t)n * rt->frame_bytes);
      done += n;
    }
    atomic_store_explicit(rp, r + got, memory_order_release);
    if (got < want) rt->ring->underrun_frames += want - got;
  }
  if (got < want) memset(out + (size_t)got * rt->frame_bytes, rt->silence, (size_t)(want - got) * rt->frame_bytes);
  if (got == 0 && flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
  return noErr;
}

/* Audio input stream (WSD_AUDIO_OPEN_INPUT): the default input device through an AUHAL unit at the device's own rate
 * (AUHAL's rate conversion fails to render once the rates differ). The callback renders the device's interleaved float
 * frames, an AudioConverter turns them into the client's rate, channels and sample format, and they are published in the
 * client's ring. */
struct wsd_capture_feed { struct wsd_audio_rt *rt; uint32_t frames; int given; };

static OSStatus capture_feed(AudioConverterRef c, UInt32 *packets, AudioBufferList *io,
                             AudioStreamPacketDescription **desc, void *ref) {
  struct wsd_capture_feed *f = ref;
  (void)c; (void)desc;
  if (f->given) { *packets = 0; return 1; } /* the converter returns this once it has used the render */
  f->given = 1;
  *packets = f->frames;
  io->mBuffers[0].mNumberChannels = f->rt->dev_channels;
  io->mBuffers[0].mDataByteSize = f->frames * f->rt->dev_channels * (UInt32)sizeof(float);
  io->mBuffers[0].mData = f->rt->scratch;
  return noErr;
}

static OSStatus audio_capture(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                              UInt32 frames, AudioBufferList *unused) {
  struct wsd_audio_rt *rt = ref;
  (void)unused;
  if (!atomic_load_explicit(&rt->running, memory_order_acquire) || frames > rt->scratch_frames) return noErr;
  AudioBufferList in = {1, {{rt->dev_channels, frames * rt->dev_channels * (UInt32)sizeof(float), rt->scratch}}};
  OSStatus e = AudioUnitRender(rt->unit, flags, ts, bus, frames, &in);
  if (e != noErr) return e;
  struct wsd_capture_feed feed = {rt, frames, 0};
  UInt32 got = rt->out_frames;
  AudioBufferList out = {1, {{rt->channels, got * rt->frame_bytes, rt->out}}};
  AudioConverterFillComplexBuffer(rt->conv, capture_feed, &feed, &got, &out, NULL);
  _Atomic uint64_t *wp = (_Atomic uint64_t *)&rt->ring->write_frames, *rp = (_Atomic uint64_t *)&rt->ring->read_frames;
  uint64_t w = atomic_load_explicit(wp, memory_order_relaxed), r = atomic_load_explicit(rp, memory_order_acquire);
  uint64_t used = w > r ? w - r : 0;
  uint32_t space = used >= rt->ring_frames ? 0 : (uint32_t)(rt->ring_frames - used), n = got < space ? got : space;
  for (uint32_t done = 0; done < n;) {
    uint32_t pos = (uint32_t)((w + done) % rt->ring_frames), k = rt->ring_frames - pos;
    if (k > n - done) k = n - done;
    memcpy(rt->data + (size_t)pos * rt->frame_bytes, rt->out + (size_t)done * rt->frame_bytes, (size_t)k * rt->frame_bytes);
    done += k;
  }
  atomic_store_explicit(wp, w + n, memory_order_release);
  if (n < got) rt->ring->underrun_frames += got - n; /* the client did not read: these frames are lost */
  return noErr;
}

/* CoreAudio calls can block: AudioOutputUnitStop waits out the render thread. On the main queue they held frames and
 * input (2026-10-21: Rocket League reopened a stream every 0.45 s, and each frame was held 0.4 to 0.9 s), so a
 * stream's unit is made, started, stopped and disposed in order on this queue. */
static dispatch_queue_t g_audio_queue;

@interface WsdAudio : NSObject
@property uint32_t streamId;
@property AudioComponentInstance unit; /* on g_audio_queue only */
@property void *map;
@property size_t mapLen;
@property struct wsd_audio_rt *rt;
@property BOOL input; /* the server writes the ring and the client reads it */
@end
@implementation WsdAudio
/* On g_audio_queue: a default-output unit that plays the ring in the client's format. */
- (OSStatus)setUp:(struct wsd_audio_open)o sample:(uint32_t)sample {
  AudioComponentDescription desc = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                    kAudioUnitManufacturer_Apple, 0, 0};
  AudioComponent comp = AudioComponentFindNext(NULL, &desc);
  AudioComponentInstance unit = NULL;
  OSStatus e = comp ? AudioComponentInstanceNew(comp, &unit) : -1;
  if (e == noErr) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = o.rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsPacked |
                     (o.format == WSD_AUDIO_F32 ? kAudioFormatFlagIsFloat
                      : o.format == WSD_AUDIO_U8 ? 0 : kAudioFormatFlagIsSignedInteger);
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = o.channels;
    f.mBitsPerChannel = sample * 8;
    f.mBytesPerFrame = f.mBytesPerPacket = o.frame_bytes;
    e = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f));
  }
  if (e == noErr) {
    AURenderCallbackStruct cb = {audio_render, self.rt};
    e = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
  }
  if (e == noErr) e = AudioUnitInitialize(unit);
  self.unit = unit;
  if (e != noErr) [self teardown];
  return e;
}
/* On g_audio_queue: an AUHAL unit that captures the default input device in the client's rate. */
- (OSStatus)setUpInput:(struct wsd_audio_open)o {
  AudioComponentDescription desc = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple,
                                    0, 0};
  AudioComponent comp = AudioComponentFindNext(NULL, &desc);
  AudioComponentInstance unit = NULL;
  OSStatus e = comp ? AudioComponentInstanceNew(comp, &unit) : -1;
  UInt32 on = 1, off = 0, size;
  AudioDeviceID dev = kAudioObjectUnknown;
  AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  AudioStreamBasicDescription f = {0};
  if (e == noErr) e = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &on, sizeof(on));
  if (e == noErr) e = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &off, sizeof(off));
  if (e == noErr) {
    size = sizeof(dev);
    e = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, &dev);
    if (e == noErr && dev == kAudioObjectUnknown) e = kAudioHardwareBadDeviceError; /* no microphone */
  }
  if (e == noErr) e = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof(dev));
  if (e == noErr) {
    size = sizeof(f);
    e = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &f, &size);
  }
  if (e == noErr && (!f.mChannelsPerFrame || f.mChannelsPerFrame > 64)) e = kAudioUnitErr_FormatNotSupported;
  if (e == noErr) {
    /* f is the device's format; the unit hands over its frames as interleaved float at the same rate */
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mFramesPerPacket = 1;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = f.mBytesPerPacket = f.mChannelsPerFrame * 4;
    self.rt->dev_channels = f.mChannelsPerFrame;
    e = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof(f));
  }
  if (e == noErr) {
    AudioStreamBasicDescription c = {0};
    c.mSampleRate = o.rate;
    c.mFormatID = kAudioFormatLinearPCM;
    c.mFormatFlags = kAudioFormatFlagIsPacked |
                     (o.format == WSD_AUDIO_F32 ? kAudioFormatFlagIsFloat
                      : o.format == WSD_AUDIO_U8 ? 0 : kAudioFormatFlagIsSignedInteger);
    c.mFramesPerPacket = 1;
    c.mChannelsPerFrame = o.channels;
    c.mBitsPerChannel = o.frame_bytes / o.channels * 8;
    c.mBytesPerFrame = c.mBytesPerPacket = o.frame_bytes;
    e = AudioConverterNew(&f, &c, &self.rt->conv);
    self.rt->out_frames = (uint32_t)ceil(4096.0 * o.rate / f.mSampleRate) + 64; /* a render is at most 4096 frames */
    self.rt->out = calloc(self.rt->out_frames, o.frame_bytes);
  }
  if (e == noErr) {
    size = sizeof(UInt32);
    e = AudioUnitGetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                             &self.rt->scratch_frames, &size);
  }
  if (e == noErr) {
    if (self.rt->scratch_frames > 4096) self.rt->scratch_frames = 4096;
    self.rt->scratch = calloc((size_t)self.rt->scratch_frames * self.rt->dev_channels, sizeof(float));
    self.rt->unit = unit;
    AURenderCallbackStruct cb = {audio_capture, self.rt};
    e = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof(cb));
  }
  if (e == noErr) e = AudioUnitInitialize(unit);
  self.unit = unit;
  if (e != noErr) [self teardown];
  return e;
}
/* On g_audio_queue. */
- (void)teardown {
  if (!self.unit) return;
  AudioOutputUnitStop(self.unit); /* synchronous: the render callback is not running afterwards */
  AudioUnitUninitialize(self.unit);
  AudioComponentInstanceDispose(self.unit);
  self.unit = NULL;
  if (self.rt->conv) AudioConverterDispose(self.rt->conv); /* the render callback is gone */
  self.rt->conv = NULL;
}
/* The ring outlives the unit: the client holds the stream until it closes it, and each block queued on
 * g_audio_queue holds it until it ran, the teardown last. */
- (void)dealloc {
  if (self.map) munmap(self.map, self.mapLen);
  if (self.rt) { free(self.rt->scratch); free(self.rt->out); }
  free(self.rt);
}
@end

@interface WsdClient : NSObject
@property int fd;
@property pid_t pid;
@property(strong) dispatch_source_t readSrc, writeSrc;
@property(strong) NSMutableData *inbuf, *outbuf;
@property(strong) NSMutableArray<NSNumber *> *fdQueue;
@property(strong) NSMutableDictionary<NSNumber *, WsdWindow *> *windows;
@property(strong) NSMutableDictionary<NSNumber *, WsdBuffer *> *buffers;
@property(strong) NSMutableDictionary<NSNumber *, WsdAudio *> *audio;
@property(strong) NSMutableDictionary<NSNumber *, NSCursor *> *cursors; /* CURSOR_DEFINE */
@property IOPMAssertionID displayAssertion; /* SCREEN_SAVER 0 holds the display on, kIOPMNullAssertionID otherwise */
@property BOOL dead;
- (void)send:(uint32_t)type payload:(const void *)p len:(uint32_t)n extra:(NSData *)extra;
- (void)close;
@end

/* The cursor is hidden while the client asks for it, unless one of its windows stopped showing frames. */
static void sync_cursor(void) {
  BOOL hide = g_cursor_hide_wanted;
  for (WsdClient *c in g_clients)
    for (WsdWindow *w in c.windows.allValues)
      if (w.stalled) hide = NO;
  if (hide && !g_cursor_hidden) { [NSCursor hide]; g_cursor_hidden = 1; }
  if (!hide && g_cursor_hidden) { [NSCursor unhide]; g_cursor_hidden = 0; }
}

/* The levels of wsd's windows on the active Space, set as Wine's Mac driver sets them (-[WineApplicationController
 * adjustWindowLevels:]): back to front in the window server's order, each window takes the level its own state asks
 * for (-levelForActive:) and at least the level of every window behind it, so that a window in front of a fullscreen
 * one (a dialog, a crash box) rises with it instead of hiding under it, and topmost windows stay above all others.
 * Windows on other Spaces keep theirs until that Space is the active one. */
static void adjust_levels(BOOL active) {
  NSMutableDictionary<NSNumber *, WsdWindow *> *byNumber = [NSMutableDictionary dictionary];
  for (WsdClient *c in g_clients)
    for (WsdWindow *w in c.windows.allValues)
      if (w.window) byNumber[@(w.window.windowNumber)] = w;
  NSMutableArray<WsdWindow *> *order = [NSMutableArray array]; /* front to back, the floating windows first */
  NSUInteger floating = 0;
  for (NSNumber *n in [NSWindow windowNumbersWithOptions:0]) {
    WsdWindow *w = byNumber[n];
    if (!w) continue;
    if ([w floatsForActive:active]) [order insertObject:w atIndex:floating++];
    else [order addObject:w];
  }
  NSWindowLevel maxLevel = NSIntegerMin, maxOther = NSNormalWindowLevel;
  WsdWindow *prev = nil;
  for (WsdWindow *w in order.reverseObjectEnumerator) {
    NSWindowLevel level = [w levelForActive:active], old = w.window.level;
    BOOL floats = [w floatsForActive:active];
    if (floats && level <= maxOther) level = maxOther + 1;
    if (level < maxLevel) level = maxLevel;
    else maxLevel = level;
    if (!floats && level > maxOther) maxOther = level;
    if (level != old) {
      w.window.level = level;
      /* raised: to the back of its new level, still in front of the windows set before it; lowered: to the front of
       * its new level, which going back to front keeps the order */
      if (level < old) [w.window orderFront:nil];
      else if (prev) [w.window orderWindow:NSWindowAbove relativeTo:prev.window.windowNumber];
      else [w.window orderBack:nil];
    }
    prev = w;
  }
}

/* The screen with the menu bar: the client's display, and the origin of the pixel coordinates wsd exchanges with it.
 * NSScreen.mainScreen is the screen of the key window, which moves (2026-10-04: with Obelisk on the MacBook's screen,
 * Rocket League got 3024x1964 while its window was on a 2560x1440 one, and its picture overflowed). */
static NSScreen *primary_screen(void) {
  return NSScreen.screens.firstObject ?: NSScreen.mainScreen;
}

/* The effective scale, in pixels per point: the real backing scale lowered by the render percent, never below what keeps
 * the desktop at 640x480 pixels and never above the real scale. See PROTOCOL.md, "Render percent". */
static double backing_scale(NSWindow *w) {
  NSScreen *primary = (!w || g_render_percent < 100) ? primary_screen() : nil;
  double s = w ? w.backingScaleFactor : primary.backingScaleFactor;
  s = s > 0 ? s : 1.0;
  if (g_render_percent < 100) {
    NSSize pt = primary.frame.size;
    double min_scale = MAX(640.0 / pt.width, 480.0 / pt.height);
    s = MIN(s, MAX(s * g_render_percent / 100.0, min_scale));
  }
  return s;
}

/* What HELLO and EV_DISPLAY give a client: the screen with the menu bar in pixels, and its work area. */
static struct wsd_hello_reply display_info(void) {
  NSScreen *sc = primary_screen();
  double s = backing_scale(nil);
  NSRect vf = sc.visibleFrame; /* without the menu bar and the Dock */
  struct wsd_hello_reply r = {WSD_PROTOCOL_VERSION, getpid(), (uint32_t)lround(sc.frame.size.width * s),
                              (uint32_t)lround(sc.frame.size.height * s), (uint32_t)sc.maximumFramesPerSecond,
                              (uint32_t)lround(s * 1000), (int32_t)lround((vf.origin.x - sc.frame.origin.x) * s),
                              (int32_t)lround((NSMaxY(sc.frame) - NSMaxY(vf)) * s),
                              (uint32_t)lround(vf.size.width * s), (uint32_t)lround(vf.size.height * s)};
  return r;
}
static struct wsd_hello_reply g_display; /* the screen the clients were last told of */

static NSCursor *system_cursor(uint32_t cursor) {
  switch (cursor) {
  case WSD_CURSOR_ARROW: return NSCursor.arrowCursor;
  case WSD_CURSOR_IBEAM: return NSCursor.IBeamCursor;
  case WSD_CURSOR_CROSSHAIR: return NSCursor.crosshairCursor;
  case WSD_CURSOR_RESIZE_LEFT_RIGHT: return NSCursor.resizeLeftRightCursor;
  case WSD_CURSOR_RESIZE_UP_DOWN: return NSCursor.resizeUpDownCursor;
  case WSD_CURSOR_NOT_ALLOWED: return NSCursor.operationNotAllowedCursor;
  case WSD_CURSOR_POINTING_HAND: return NSCursor.pointingHandCursor;
  case WSD_CURSOR_CLOSED_HAND: return NSCursor.closedHandCursor;
  case WSD_CURSOR_DRAG_COPY: return NSCursor.dragCopyCursor;
  case WSD_CURSOR_DRAG_LINK: return NSCursor.dragLinkCursor;
  }
  return nil;
}

/* Shows g_cursor at once when the pointer is over the content of one of wsd's windows; anywhere else it comes with
 * the next -cursorUpdate:, when the pointer enters a window. */
static void apply_cursor(void) {
  if (!NSApp.active) return;
  NSPoint p = NSEvent.mouseLocation;
  NSWindow *w = [NSApp windowWithWindowNumber:[NSWindow windowNumberAtPoint:p belowWindowWithWindowNumber:0]];
  if (![w isKindOfClass:WsdNSWindow.class] || !NSPointInRect([w convertPointFromScreen:p], w.contentView.frame))
    return;
  [g_cursor ?: NSCursor.arrowCursor set];
}

/* WSD_STYLE_* to an AppKit style mask: a titled window gets the macOS title bar and buttons; borderless otherwise
 * (the client draws any frame itself). */
static NSWindowStyleMask style_mask(uint32_t style) {
  NSWindowStyleMask mask = 0;
  if (style & WSD_STYLE_BORDERLESS) return NSWindowStyleMaskBorderless;
  if (style & WSD_STYLE_TITLED) mask |= NSWindowStyleMaskTitled;
  if (style & WSD_STYLE_CLOSABLE) mask |= NSWindowStyleMaskClosable;
  if (style & WSD_STYLE_RESIZABLE) mask |= NSWindowStyleMaskResizable;
  if (style & WSD_STYLE_MINIATURIZABLE) mask |= NSWindowStyleMaskMiniaturizable;
  return mask;
}

/* ---------------------------------------------------------------- view and input */

static CAMetalLayer *make_metal_layer(void) {
  CAMetalLayer *l = [CAMetalLayer layer];
  l.device = g_dev;
  l.pixelFormat = MTLPixelFormatBGRA8Unorm;
  /* Clients write sRGB-encoded pixels (Win32 GDI, D3D swapchains in UNORM): tag the layer as sRGB so the window
   * server color-matches it to the display instead of showing the raw values in the display's own space. */
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  l.colorspace = srgb;
  CGColorSpaceRelease(srgb);
  /* Opaque and black: Win32 GDI never writes alpha (its pixels have alpha 0), and a window shown before its first
   * frame must not let the window's grey background through (2026-10-03: Rocket League's startup window). */
  l.opaque = YES;
  l.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
  l.framebufferOnly = YES;
  l.maximumDrawableCount = 3;
  l.displaySyncEnabled = YES;
  l.allowsNextDrawableTimeout = YES;
  l.contentsGravity = kCAGravityResize;
  return l;
}



@implementation WsdView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer {
  return make_metal_layer();
}
- (WsdWindow *)ww { return (WsdWindow *)self.owner; }
- (uint32_t)synth { return g_injecting ? WSD_KEY_SYNTHETIC : 0; }
- (uint64_t)evNs:(NSEvent *)e { return (uint64_t)(e.timestamp * 1e9); }
- (void)pixelPoint:(NSEvent *)e x:(float *)x y:(float *)y {
  NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  double s = backing_scale(self.window);
  *x = (float)(p.x * s);
  *y = (float)((self.bounds.size.height - p.y) * s);
}
/* WSD_KEY_ISO for a key of an ISO keyboard. The type comes with each key event: a MacBook's keyboard and an external
 * one may differ. */
- (uint32_t)iso:(NSEvent *)e {
  CGEventRef cg = e.CGEvent;
  int64_t type = cg ? CGEventGetIntegerValueField(cg, kCGKeyboardEventKeyboardType) : 0;
  return type && KBGetLayoutType((int16_t)type) == kKeyboardISO ? WSD_KEY_ISO : 0;
}
- (void)key:(NSEvent *)e down:(BOOL)down {
  struct wsd_ev_key k = {self.ww.windowId,
                         (down ? WSD_KEY_DOWN : 0) | (e.isARepeat ? WSD_KEY_REPEAT : 0) | [self iso:e] | [self synth],
                         e.keyCode, (uint32_t)(e.modifierFlags >> 16), [self evNs:e]};
  NSData *chars = [e.characters dataUsingEncoding:NSUTF8StringEncoding] ?: [NSData data];
  [self.ww noteKey:e.keyCode down:down];
  [self.ww sendEvent:WSD_EV_KEY payload:&k len:sizeof(k) extra:chars];
}
- (void)keyDown:(NSEvent *)e { [self key:e down:YES]; }
- (void)keyUp:(NSEvent *)e { [self key:e down:NO]; }
- (void)flagsChanged:(NSEvent *)e {
  NSEventModifierFlags bit = 0;
  switch (e.keyCode) {
  case kVK_Shift: case kVK_RightShift: bit = NSEventModifierFlagShift; break;
  case kVK_Control: case kVK_RightControl: bit = NSEventModifierFlagControl; break;
  case kVK_Option: case kVK_RightOption: bit = NSEventModifierFlagOption; break;
  case kVK_Command: case kVK_RightCommand: bit = NSEventModifierFlagCommand; break;
  case kVK_CapsLock: [self.ww syncCapsLock:(e.modifierFlags & NSEventModifierFlagCapsLock) != 0]; return;
  case kVK_Function: bit = NSEventModifierFlagFunction; break;
  default: return;
  }
  struct wsd_ev_key k = {self.ww.windowId, ((e.modifierFlags & bit) ? WSD_KEY_DOWN : 0) | [self synth], e.keyCode,
                         (uint32_t)(e.modifierFlags >> 16), [self evNs:e]};
  [self.ww noteKey:e.keyCode down:(e.modifierFlags & bit) != 0];
  [self.ww sendEvent:WSD_EV_KEY payload:&k len:sizeof(k) extra:nil];
}
- (void)button:(NSEvent *)e which:(uint32_t)b down:(BOOL)down {
  struct wsd_ev_button m = {self.ww.windowId, b, down, (uint32_t)(e.modifierFlags >> 16), 0, 0, [self evNs:e]};
  [self pixelPoint:e x:&m.x y:&m.y];
  [self.ww sendEvent:WSD_EV_BUTTON payload:&m len:sizeof(m) extra:nil];
}
- (void)mouseDown:(NSEvent *)e { [self button:e which:0 down:YES]; }
- (void)mouseUp:(NSEvent *)e { [self button:e which:0 down:NO]; }
- (void)rightMouseDown:(NSEvent *)e { [self button:e which:1 down:YES]; }
- (void)rightMouseUp:(NSEvent *)e { [self button:e which:1 down:NO]; }
- (void)otherMouseDown:(NSEvent *)e { [self button:e which:(uint32_t)e.buttonNumber down:YES]; }
- (void)otherMouseUp:(NSEvent *)e { [self button:e which:(uint32_t)e.buttonNumber down:NO]; }
- (void)motion:(NSEvent *)e {
  WsdWindow *w = self.ww;
  if (w.relative) {
    if (w.stalled) return; /* the user's pointer, not the game's camera: a stopped game would get it all at once */
    struct wsd_ev_relative r = {w.windowId, [self synth], (float)e.deltaX, (float)e.deltaY, [self evNs:e]};
    [w sendEvent:WSD_EV_RELATIVE payload:&r len:sizeof(r) extra:nil];
    return;
  }
  /* the mouse moves macOS queued before a CURSOR_WARP still carry the place the pointer left */
  if (g_warp_time) {
    if (e.timestamp <= g_warp_time) return;
    g_warp_time = 0;
  }
  struct wsd_ev_motion m = {w.windowId, [self synth], 0, 0, [self evNs:e]};
  [self pixelPoint:e x:&m.x y:&m.y];
  if (w.clip) {
    NSRect c = w.clipPx;
    float cx = fminf(fmaxf(m.x, c.origin.x), c.origin.x + c.size.width - 1);
    float cy = fminf(fmaxf(m.y, c.origin.y), c.origin.y + c.size.height - 1);
    if (cx != m.x || cy != m.y) {
      m.flags |= 1; /* clamped */
      if (!g_injecting && !w.stalled) {
        /* Real pointer left the clip rect: warp it back (global, top-left display coordinates). */
        double s = backing_scale(self.window);
        NSPoint vp = NSMakePoint(cx / s, self.bounds.size.height - cy / s);
        NSPoint sp = [self.window convertPointToScreen:[self convertPoint:vp toView:nil]];
        CGFloat top = primary_screen().frame.size.height;
        CGWarpMouseCursorPosition(CGPointMake(sp.x, top - sp.y));
        CGAssociateMouseAndMouseCursorPosition(true);
      }
      m.x = cx;
      m.y = cy;
    }
  }
  [w sendEvent:WSD_EV_MOTION payload:&m len:sizeof(m) extra:nil];
}
- (void)mouseMoved:(NSEvent *)e { [self motion:e]; }
- (void)mouseDragged:(NSEvent *)e { [self motion:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self motion:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self motion:e]; }
- (void)cursorUpdate:(NSEvent *)e { [g_cursor ?: NSCursor.arrowCursor set]; }
- (void)scrollWheel:(NSEvent *)e {
  struct wsd_ev_wheel w = {self.ww.windowId, e.hasPreciseScrollingDeltas | [self synth], (float)e.scrollingDeltaX,
                           (float)e.scrollingDeltaY, [self evNs:e]};
  [self.ww sendEvent:WSD_EV_WHEEL payload:&w len:sizeof(w) extra:nil];
}
@end

/* ---------------------------------------------------------------- windows and present */

@implementation WsdWindow
- (void)sendEvent:(uint32_t)type payload:(const void *)p len:(uint32_t)n extra:(NSData *)extra {
  [self.client send:type payload:p len:n extra:extra];
}
- (void)sendResize {
  NSSize sz = self.view.bounds.size;
  double s = backing_scale(self.window);
  struct wsd_ev_resize r = {self.windowId, (uint32_t)lround(sz.width * s), (uint32_t)lround(sz.height * s),
                            (uint32_t)lround(s * 1000)};
  [self sendEvent:WSD_EV_RESIZE payload:&r len:sizeof(r) extra:nil];
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
  struct wsd_window w = {self.windowId};
  [self sendEvent:WSD_EV_CLOSE payload:&w len:sizeof(w) extra:nil];
  return NO;
}
- (void)noteKey:(uint32_t)code down:(BOOL)down {
  if (!self.heldKeys) self.heldKeys = [NSMutableIndexSet indexSet];
  if (down)
    [self.heldKeys addIndex:code];
  else
    [self.heldKeys removeIndex:code];
}
- (void)sendKey:(uint32_t)code down:(BOOL)down {
  struct wsd_ev_key k = {self.windowId, down ? WSD_KEY_DOWN : 0, code, (uint32_t)(NSEvent.modifierFlags >> 16),
                         now_ns()};
  [self noteKey:code down:down];
  [self sendEvent:WSD_EV_KEY payload:&k len:sizeof(k) extra:nil];
}
/* Caps Lock is a toggle: macOS sends one flagsChanged for each change, and Wine toggles its state on each press
 * (server/queue.c, set_input_key_state). Each change is a press and a release, as in Wine's Mac driver
 * (-[WineWindow flagsChanged:]): a press when it came on and a release when it went off inverted Wine's state every
 * other time. A change made while another app had the keyboard is given when a window becomes key again. */
- (void)syncCapsLock:(BOOL)on {
  if (on == g_caps_told) return;
  g_caps_told = on;
  [self sendKey:kVK_CapsLock down:YES];
  [self sendKey:kVK_CapsLock down:NO];
}
/* A key released while another app is in front never reaches the window: leaving it releases every key the client
 * holds down, and coming back presses the modifiers the user still holds (2026-10-09: Obelisk's Shift+Tab overlay
 * took the focus between Shift's press and its release, and the game kept running). Command is left out: Cmd+Tab
 * back into the game would tap the Windows key. */
- (void)releaseHeldKeys {
  NSIndexSet *held = [self.heldKeys copy];
  [held enumerateIndexesUsingBlock:^(NSUInteger code, BOOL *stop) {
    [self sendKey:(uint32_t)code down:NO];
  }];
}
- (void)pressHeldModifiers {
  static const uint16_t modifiers[] = {kVK_Shift, kVK_RightShift, kVK_Control, kVK_RightControl, kVK_Option,
                                       kVK_RightOption};
  for (size_t i = 0; i < sizeof(modifiers) / sizeof(modifiers[0]); i++)
    if (CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, modifiers[i]) &&
        ![self.heldKeys containsIndex:modifiers[i]])
      [self sendKey:modifiers[i] down:YES];
}
- (void)windowDidBecomeKey:(NSNotification *)n {
  /* back in the window: a relative-mode window takes the pointer again, once it shows frames */
  if (self.relative && !g_relative_global && !self.stalled) {
    CGError e = CGAssociateMouseAndMouseCursorPosition(false);
    g_relative_global = (e == kCGErrorSuccess);
    LOG("window %u: focus back, relative mouse resumed (%d)", self.windowId, e);
  }
  struct wsd_window_flag f = {self.windowId, 1};
  [self sendEvent:WSD_EV_FOCUS payload:&f len:sizeof(f) extra:nil];
  [self pressHeldModifiers];
  [self syncCapsLock:(NSEvent.modifierFlags & NSEventModifierFlagCapsLock) != 0];
}
- (void)windowDidResignKey:(NSNotification *)n {
  /* The pointer disassociation is global: leaving the window (Cmd+Tab, another app, a notification) must give the
   * user their cursor back at once, whatever the game does with the focus event (2026-10-03 session: the cursor
   * stayed captured and the user had to kill wsd). */
  if (self.relative && g_relative_global) {
    CGAssociateMouseAndMouseCursorPosition(true);
    g_relative_global = NO;
    LOG("window %u: focus lost, relative mouse suspended", self.windowId);
  }
  [self releaseHeldKeys];
  struct wsd_window_flag f = {self.windowId, 0};
  [self sendEvent:WSD_EV_FOCUS payload:&f len:sizeof(f) extra:nil];
}
/* A window that holds the pointer (relative mode or a clip rectangle) but has shown no new frame for STALL_NS gives
 * it back and shows the cursor: a frozen game must not keep the Mac's mouse (2026-10-11: ELDEN RING froze in game
 * with the mouse captured, and the user could no longer do anything). Nothing is watched before the window's first
 * frame. The client is not told: its relative mode and clip come back with the next frame shown. */
- (void)checkStalled:(uint64_t)now {
  if (self.stalled || !self.shownNs || now - self.shownNs < STALL_NS || (!self.relative && !self.clip)) return;
  self.stalled = YES;
  if (self.relative && g_relative_global) {
    CGAssociateMouseAndMouseCursorPosition(true);
    g_relative_global = NO;
  }
  sync_cursor();
  LOG("window %u: no frame shown for %.1f s, pointer and cursor given back", self.windowId,
      (now - self.shownNs) / 1e9);
}
- (void)frameShownAt:(uint64_t)ns {
  uint64_t gap = ns - self.shownNs;
  self.shownNs = ns;
  if (!self.stalled) return;
  self.stalled = NO;
  CGError e = kCGErrorSuccess;
  BOOL take = self.relative && !g_relative_global && self.window.keyWindow;
  if (take) {
    e = CGAssociateMouseAndMouseCursorPosition(false);
    g_relative_global = (e == kCGErrorSuccess);
  }
  sync_cursor();
  if (take)
    LOG("window %u: frames shown again after %.1f s, relative mouse resumed (%d)", self.windowId, gap / 1e9, e);
  else
    LOG("window %u: frames shown again after %.1f s", self.windowId, gap / 1e9);
}
- (void)windowDidResize:(NSNotification *)n {
  if (self.transitioning) return; /* one RESIZE per fullscreen transition */
  [self sendResize];
  [self sendFrame];
}
/* The user (or AppKit, e.g. moving it off a removed screen) moved or resized the window: the client follows. */
- (void)sendFrame {
  if (self.transitioning || self.fullscreen || self.window.miniaturized) return;
  NSWindow *w = self.window;
  double s = backing_scale(w);
  NSRect content = [w contentRectForFrameRect:w.frame];
  double top = NSMaxY(primary_screen().frame);
  struct wsd_window_frame f = {self.windowId, WSD_FRAME_MOVE | WSD_FRAME_SIZE, (int32_t)lround(content.origin.x * s),
                               (int32_t)lround((top - NSMaxY(content)) * s), (uint32_t)lround(content.size.width * s),
                               (uint32_t)lround(content.size.height * s)};
  [self sendEvent:WSD_EV_FRAME payload:&f len:sizeof(f) extra:nil];
}
- (void)windowDidMove:(NSNotification *)n {
  [self sendFrame];
}
- (void)windowDidChangeOcclusionState:(NSNotification *)n {
  BOOL shown = (self.window.occlusionState & NSWindowOcclusionStateVisible) != 0;
  LOG("window %u %s", self.windowId, shown ? "shown by macOS" : "not shown by macOS: its frames wait");
}
/* Also sent after WINDOW_MINIMIZE (the animation ends later): the client ignores a state it already has. */
- (void)windowDidMiniaturize:(NSNotification *)n {
  struct wsd_window_flag f = {self.windowId, 1};
  [self sendEvent:WSD_EV_MINIMIZE payload:&f len:sizeof(f) extra:nil];
}
- (void)windowDidDeminiaturize:(NSNotification *)n {
  [self orderInFront:YES];
  struct wsd_window_flag f = {self.windowId, 0};
  [self sendEvent:WSD_EV_MINIMIZE payload:&f len:sizeof(f) extra:nil];
}
/* Shows the window in front of wsd's others: at the level of the front one first, which a window of its own level
 * would stay under (a fullscreen window above the menu bar), then at the level adjust_levels gives it. Without
 * activate, the key window stays where it is (Win32 SWP_NOACTIVATE, WS_EX_NOACTIVATE). */
- (void)orderInFront:(BOOL)activate {
  NSWindow *front = [NSApp windowWithWindowNumber:[NSWindow windowNumbersWithOptions:0].firstObject.integerValue];
  if (front && front != self.window && front.level > self.window.level) self.window.level = front.level;
  if (activate) [self.window makeKeyAndOrderFront:nil];
  [self.window orderFrontRegardless]; /* cooperative activation may leave the app inactive */
  adjust_levels(NSApp.active);
}
/* The client maximizes and restores the window itself (Win32 SC_MAXIMIZE) and then sets its frame. */
- (BOOL)windowShouldZoom:(NSWindow *)window toFrame:(NSRect)frame {
  struct wsd_window w = {self.windowId};
  [self sendEvent:WSD_EV_ZOOM payload:&w len:sizeof(w) extra:nil];
  return NO;
}
- (void)setStyle:(uint32_t)style {
  NSWindowStyleMask mask = style_mask(style);
  NSWindow *w = self.window;
  ((WsdNSWindow *)w).noActivate = (style & WSD_STYLE_NOACTIVATE) != 0;
  if (self.topmost != !!(style & WSD_STYLE_TOPMOST)) {
    self.topmost = !self.topmost;
    adjust_levels(NSApp.active);
  }
  if (self.fullscreen) { self.savedStyle = mask; return; } /* applied when fullscreen ends */
  if (w.styleMask == mask) return;
  NSRect content = [w contentRectForFrameRect:w.frame];
  self.transitioning = YES;
  w.styleMask = mask;
  [w setFrame:[w frameRectForContentRect:content] display:YES];
  [w makeFirstResponder:self.view];
  self.transitioning = NO;
  LOG("window %u style 0x%x", self.windowId, style);
}
- (void)minimize:(BOOL)on {
  NSWindow *w = self.window;
  if (on && !w.miniaturized && !self.minimizedOut) {
    if (w.styleMask & NSWindowStyleMaskMiniaturizable) [w miniaturize:nil];
    else {
      self.minimizedOut = YES;
      [w orderOut:nil];
      adjust_levels(NSApp.active);
    }
  } else if (!on && w.miniaturized) {
    [w deminiaturize:nil];
  } else if (!on && self.minimizedOut) {
    self.minimizedOut = NO;
    [self orderInFront:NO]; /* the client activates it if it should */
  }
}
- (void)logState:(const char *)when {
  NSWindow *w = self.window;
  NSRect f = w.frame;
  LOG("window %u [%s]: visible=%d occlusion_visible=%d on_active_space=%d key=%d app_active=%d screen='%s' "
      "frame=(%.0f,%.0f %.0fx%.0f) level=%ld presented=%u presented_zero=%u overlay_presented=%u overlay_zero=%u",
      self.windowId, when, w.visible, (w.occlusionState & NSWindowOcclusionStateVisible) != 0, w.onActiveSpace,
      w.keyWindow, NSApp.active, w.screen.localizedName.UTF8String ?: "(none)", f.origin.x, f.origin.y,
      f.size.width, f.size.height, (long)w.level, self.nPresented, self.nZero, self.overlay.nPresented, self.overlay.nZero);
}

- (void)releaseBuffer:(WsdBuffer *)b serial:(uint64_t)serial shown:(BOOL)shown {
  struct wsd_ev_release r = {b.bufferId, shown, serial};
  [self.client send:WSD_EV_RELEASE payload:&r len:sizeof(r) extra:nil];
}

/* Display-link tick: encode the latest pending frame, if any. */
- (void)tick:(CADisplayLink *)link {
  [self tickPlane];
  if (self.overlay) [self.overlay tickPlane];
}

- (void)tickPlane {
  WsdBuffer *b = self.pendingBuffer;
  BOOL redisplay = NO;
  if (!b && self.redisplay && self.lastBuffer && !self.lastBuffer.detached) { b = self.lastBuffer; redisplay = YES; }
  if (!b || !self.window.visible) return;
  /* A window macOS does not show (covered, on another Space, the screen locked) has its drawables no longer taken:
   * nextDrawable would hold the main queue up to a second, and every message and buffer release with it (2026-10-22:
   * a Space switch held Rocket League's frames, Neptune reported its swapchain occluded, and the game never presented
   * again, Obelisk issue #14). The frame stays pending until the window shows again; a newer one still replaces it. */
  if (!(self.window.occlusionState & NSWindowOcclusionStateVisible)) return;
  uint64_t tick_ns = now_ns();
  id<CAMetalDrawable> d = [self.layer nextDrawable];
  if (!d) return; /* keep it pending */
  if (redisplay) self.redisplay = NO;
  else { self.pendingBuffer = nil; self.lastBuffer = b; self.redisplayTries = 0; }
  CGSize want = CGSizeMake(b.width, b.height);
  if (!CGSizeEqualToSize(self.layer.drawableSize, want)) self.layer.drawableSize = want;

  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = d.texture;
  rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLCommandBuffer> cb = [g_queue commandBuffer];
  id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
  [enc setRenderPipelineState:g_pipe];
  [enc setFragmentTexture:b.texture atIndex:0];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [enc endEncoding];

  struct wsd_ev_presented ev = {self.windowId, b.bufferId, self.pendingSerial, self.pendingClientNs,
                                self.pendingReceivedNs, tick_ns, 0, self.replaced, 0};
  self.replaced = 0;
  uint64_t serial = self.pendingSerial;
  __weak WsdWindow *weakSelf = self;
  [d addPresentedHandler:^(id<MTLDrawable> dr) {
    struct wsd_ev_presented e2 = ev;
    e2.presented_ns = (uint64_t)(dr.presentedTime * 1e9);
    dispatch_async(dispatch_get_main_queue(), ^{
      WsdWindow *sw = weakSelf;
      if (e2.presented_ns) sw.nPresented++; else sw.nZero++;
      if (e2.presented_ns && !redisplay) {
        [sw.isOverlay ? sw.base : sw frameShownAt:now_ns()];
        if (sw.statLastNs && e2.presented_ns > sw.statLastNs) {
          uint64_t d = e2.presented_ns - sw.statLastNs;
          if (d > 50000000) { /* a hitch: wall-clock time, to match against profiles of the game */
            struct timeval tv;
            gettimeofday(&tv, NULL);
            struct tm lt;
            localtime_r(&tv.tv_sec, &lt);
            LOG("window %u %s: frame held %.1f ms, shown at %02d:%02d:%02d.%03d", sw.windowId,
                sw.isOverlay ? "overlay" : "base", d / 1e6, lt.tm_hour, lt.tm_min, lt.tm_sec, (int)(tv.tv_usec / 1000));
          }
          sw.statSumNs += d;
          if (d > sw.statMaxNs) sw.statMaxNs = d;
          if (!sw.statMinNs || d < sw.statMinNs) sw.statMinNs = d;
          if (++sw.statCount == 120) {
            LOG("window %u %s: 120 frames shown, present interval avg %.3f min %.3f max %.3f ms, latency submit->shown %.3f ms",
                sw.windowId, sw.isOverlay ? "overlay" : "base", sw.statSumNs / 120 / 1e6, sw.statMinNs / 1e6,
                sw.statMaxNs / 1e6, (e2.presented_ns > e2.client_time_ns ? e2.presented_ns - e2.client_time_ns : 0) / 1e6);
            sw.statCount = 0; sw.statSumNs = sw.statMaxNs = sw.statMinNs = 0;
          }
        }
        sw.statLastNs = e2.presented_ns;
      }
      /* never displayed and nothing newer queued: show the same buffer again on a later tick (bounded) */
      if (!e2.presented_ns && !sw.pendingBuffer && sw.lastBuffer == b && sw.redisplayTries < 240) {
        sw.redisplayTries++;
        sw.redisplay = YES;
      }
      if (!redisplay) [weakSelf sendEvent:WSD_EV_PRESENTED payload:&e2 len:sizeof(e2) extra:nil];
    });
  }];
  [cb presentDrawable:d];
  /* The drawable does not keep its layer, which Metal's queue reads when it presents (the Metal HUD of the
   * performance sharing does): a plane removed meanwhile, as when a game recreates its swapchain, would free it first
   * (2026-10-21: wsd crashed in libMTLHud on a nil layer). The command buffer keeps it until it completes. */
  CAMetalLayer *layer = self.layer;
  [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
    (void)layer;
    if (!redisplay)
      dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf releaseBuffer:b serial:serial shown:YES]; });
  }];
  [cb commit];
}

/* Moves and/or resizes the content area. Pixels are converted with the effective scale (backing_scale()); y counts
 * down from the top of the main screen (Cocoa counts up from its bottom). One EV_RESIZE follows a size change. */
- (void)setFramePx:(const struct wsd_window_frame *)f {
  NSWindow *w = self.window;
  double s = backing_scale(w);
  NSRect content = [w contentRectForFrameRect:w.frame];
  if (f->flags & WSD_FRAME_SIZE) content.size = NSMakeSize(f->width / s, f->height / s);
  if (f->flags & WSD_FRAME_MOVE) {
    double top = NSMaxY(primary_screen().frame);
    content.origin = NSMakePoint(f->x / s, top - f->y / s - content.size.height);
  } else {
    content.origin.y += [w contentRectForFrameRect:w.frame].size.height - content.size.height; /* keep the top edge */
  }
  BOOL resized = (f->flags & WSD_FRAME_SIZE) != 0;
  self.transitioning = YES;
  [w setFrame:[w frameRectForContentRect:content] display:YES];
  self.transitioning = NO;
  LOG("window %u frame px (%d,%d %ux%u) flags 0x%x", self.windowId, f->x, f->y, f->width, f->height, f->flags);
  if (resized) [self sendResize];
  /* AppKit keeps a title bar below the menu bar and inside the screen (a Win32 caption is shorter than the macOS
   * title bar, e.g. a maximized window): the client takes the frame the window really got. */
  NSRect got = [w contentRectForFrameRect:w.frame];
  if (fabs(got.origin.x - content.origin.x) > 0.5 || fabs(got.origin.y - content.origin.y) > 0.5 ||
      fabs(got.size.width - content.size.width) > 0.5 || fabs(got.size.height - content.size.height) > 0.5)
    [self sendFrame];
}

/* The level the window's own state asks for (adjust_levels may raise it). A fullscreen window rises above the menu
 * bar and the Dock only while wsd is the active app, as in Wine's Mac driver (-[WineWindow minimumLevelForActive:]).
 * A level outlives the activation: kept up after Cmd-Tab, the window covered the app the user went to, which had the
 * keyboard and the mouse under the game's picture (2026-10-23, Rocket League in fullscreen). The presentation options
 * need nothing of the kind: macOS applies them only while the app is active. */
- (NSWindowLevel)levelForActive:(BOOL)active {
  if (self.fullscreen && active) return NSMainMenuWindowLevel + 1;
  return [self floatsForActive:active] ? NSFloatingWindowLevel : NSNormalWindowLevel;
}

/* A topmost window floats above the other apps' windows, except a fullscreen one while wsd is not active, as in Wine's
 * Mac driver by default (TOPMOST_FLOAT_INACTIVE_NONFULLSCREEN). A game in D3D exclusive fullscreen has its window
 * topmost (Neptune's swapchain sets HWND_TOPMOST, as DXMT and Windows do): floating, it stayed over every app the
 * player went to (2026-10-23: the browser a game opened, then Discord, came to the front under its picture). */
- (BOOL)floatsForActive:(BOOL)active {
  return self.topmost && (active || !self.fullscreen);
}

/* A fullscreen window keeps covering its screen when that screen changes (resolution, scale). */
- (void)fitScreen {
  NSRect want = (self.window.screen ?: primary_screen()).frame;
  if (!self.fullscreen || NSEqualRects(self.window.frame, want)) return;
  self.transitioning = YES;
  [self.window setFrame:want display:YES];
  self.transitioning = NO;
  [self sendResize];
}

- (void)setFullscreenMode:(BOOL)on {
  if (on == self.fullscreen) return;
  NSWindow *w = self.window;
  self.transitioning = YES;
  if (on) {
    self.savedFrame = w.frame;
    self.savedStyle = w.styleMask;
    w.styleMask = NSWindowStyleMaskBorderless;
    [w setFrame:w.screen ? w.screen.frame : primary_screen().frame display:YES];
    @try {
      NSApp.presentationOptions = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
    } @catch (NSException *ex) {
      LOG("presentationOptions refused: %s", ex.reason.UTF8String);
    }
  } else {
    w.styleMask = self.savedStyle;
    [w setFrame:self.savedFrame display:YES];
    NSApp.presentationOptions = NSApplicationPresentationDefault;
  }
  [w makeFirstResponder:self.view];
  self.transitioning = NO;
  self.fullscreen = on;
  adjust_levels(NSApp.active);
  [self logState:on ? "fullscreen-on" : "fullscreen-off"];
  [self sendResize];
}

- (void)removeOverlay {
  WsdWindow *o = self.overlay;
  if (!o) return;
  if (o.pendingBuffer) [o releaseBuffer:o.pendingBuffer serial:o.pendingSerial shown:NO];
  o.pendingBuffer = nil;
  o.lastBuffer = nil;
  [o.layer removeFromSuperlayer];
  self.overlay = nil;
}

/* Places the overlay plane at a rectangle of the content view given in pixels (top-left origin). */
- (WsdWindow *)overlayAt:(NSRect)px {
  WsdWindow *o = self.overlay;
  double s = backing_scale(self.window);
  BOOL created = NO;
  if (!o) {
    created = YES;
    o = [WsdWindow new];
    o.isOverlay = YES;
    o.base = self;
    o.windowId = self.windowId;
    o.client = self.client;
    o.window = self.window;
    o.layer = make_metal_layer();
    o.layer.contentsScale = s;
    [self.view.layer addSublayer:o.layer];
    self.overlay = o;
    LOG("window %u: overlay plane created", self.windowId);
  }
  /* A new plane is placed even at the rectangle of the one before it: a game that recreates its swapchain removes
   * the plane and presents again to the same rectangle, and the new layer would stay 0x0 (never displayed). */
  if (created || !NSEqualRects(px, self.overlayPx) || !o.layer.superlayer) {
    NSSize vs = self.view.bounds.size;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    o.layer.frame = CGRectMake(px.origin.x / s, vs.height - (px.origin.y + px.size.height) / s, px.size.width / s,
                               px.size.height / s);
    [CATransaction commit];
    self.overlayPx = px;
    LOG("window %u: overlay at px (%.0f,%.0f %.0fx%.0f)", self.windowId, px.origin.x, px.origin.y, px.size.width, px.size.height);
  }
  return o;
}

- (void)destroy {
  [self logState:"destroy"];
  [self removeOverlay];
  if (self.pendingBuffer) [self releaseBuffer:self.pendingBuffer serial:self.pendingSerial shown:NO];
  self.pendingBuffer = nil;
  [self.link invalidate];
  self.link = nil;
  self.stalled = NO;
  if (self.relative) {
    restore_input_state();
    self.relative = NO;
  }
  sync_cursor();
  if (self.fullscreen) NSApp.presentationOptions = NSApplicationPresentationDefault;
  self.window.delegate = nil;
  [self.window orderOut:nil];
  [self.window close];
  self.window = nil;
  adjust_levels(NSApp.active);
}
@end

/* ---------------------------------------------------------------- client connection */

@implementation WsdClient
- (void)send:(uint32_t)type payload:(const void *)p len:(uint32_t)n extra:(NSData *)extra {
  if (self.dead) return;
  uint32_t total = n + (uint32_t)extra.length;
  if (total > WSD_MAX_PAYLOAD) return;
  struct wsd_hdr h = {type, total};
  [self.outbuf appendBytes:&h length:sizeof(h)];
  if (n) [self.outbuf appendBytes:p length:n];
  if (extra.length) [self.outbuf appendData:extra];
  if (self.outbuf.length > (8u << 20)) { /* client stopped reading */
    LOG("client %d output backlog over 8 MiB, dropping it", self.pid);
    [self close];
    return;
  }
  [self flush];
}
- (void)flush {
  while (self.outbuf.length) {
    ssize_t r = write(self.fd, self.outbuf.bytes, self.outbuf.length);
    if (r > 0) {
      [self.outbuf replaceBytesInRange:NSMakeRange(0, (NSUInteger)r) withBytes:NULL length:0];
      continue;
    }
    if (r < 0 && errno == EAGAIN) {
      if (!self.writeSrc) {
        self.writeSrc = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, self.fd, 0, dispatch_get_main_queue());
        __weak WsdClient *ws = self;
        dispatch_source_set_event_handler(self.writeSrc, ^{
          WsdClient *s = ws;
          [s flush];
          if (s && !s.outbuf.length && s.writeSrc) { dispatch_source_cancel(s.writeSrc); s.writeSrc = nil; }
        });
        dispatch_resume(self.writeSrc);
      }
      return;
    }
    [self close];
    return;
  }
}
- (void)error:(uint32_t)req code:(int32_t)code {
  struct wsd_ev_error e = {req, code};
  LOG("client %d: request %u failed with %d", self.pid, req, code);
  [self send:WSD_EV_ERROR payload:&e len:sizeof(e) extra:nil];
}
- (void)close {
  if (self.dead) return;
  self.dead = YES;
  for (WsdWindow *w in self.windows.allValues) [w destroy];
  [self.windows removeAllObjects];
  [self.buffers removeAllObjects]; /* textures die after in-flight command buffers release them */
  for (WsdAudio *a in self.audio.allValues) dispatch_async(g_audio_queue, ^{ [a teardown]; });
  [self.audio removeAllObjects];
  if (g_cursor && [self.cursors.allValues containsObject:g_cursor]) {
    g_cursor = nil;
    apply_cursor();
  }
  [self.cursors removeAllObjects];
  if (self.displayAssertion) {
    IOPMAssertionRelease(self.displayAssertion);
    self.displayAssertion = kIOPMNullAssertionID;
  }
  for (NSNumber *f in self.fdQueue) close(f.intValue);
  [self.fdQueue removeAllObjects];
  if (self.readSrc) dispatch_source_cancel(self.readSrc);
  if (self.writeSrc) dispatch_source_cancel(self.writeSrc);
  close(self.fd);
  LOG("client %d disconnected", self.pid);
  [g_clients removeObject:self];
  if (g_terminate_pending && g_clients.count == 0) {
    g_terminate_pending = NO;
    LOG("the game is gone, the logout, restart or shutdown goes on");
    [NSApp replyToApplicationShouldTerminate:YES];
  }
  if (g_exit_on_disconnect && g_clients.count == 0) {
    restore_input_state();
    exit(0);
  }
}

- (WsdWindow *)win:(uint32_t)wid req:(uint32_t)req {
  WsdWindow *w = self.windows[@(wid)];
  if (!w) [self error:req code:-ENOENT];
  return w;
}

- (void)createWindow:(const struct wsd_window_create *)c title:(NSString *)title {
  if (!c->window_id || self.windows[@(c->window_id)] || self.windows.count >= WSD_MAX_WINDOWS ||
      !c->width_px || !c->height_px || c->width_px > 16384 || c->height_px > 16384) {
    [self error:WSD_WINDOW_CREATE code:-EINVAL];
    return;
  }
  NSWindowStyleMask style = style_mask(c->style);
  double s = backing_scale(nil);
  NSRect r = NSMakeRect(120, 120, c->width_px / s, c->height_px / s);
  WsdWindow *w = [WsdWindow new];
  w.windowId = c->window_id;
  w.client = self;
  w.window = [[WsdNSWindow alloc] initWithContentRect:r styleMask:style backing:NSBackingStoreBuffered defer:NO];
  w.window.releasedWhenClosed = NO;
  w.window.backgroundColor = NSColor.blackColor;
  /* the zoom button maximizes through the client (WSD_EV_ZOOM); wsd's own fullscreen is WINDOW_FULLSCREEN */
  w.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
  w.window.tabbingMode = NSWindowTabbingModeDisallowed;
  w.window.title = title;
  w.window.acceptsMouseMovedEvents = YES;
  w.topmost = (c->style & WSD_STYLE_TOPMOST) != 0;
  ((WsdNSWindow *)w.window).noActivate = (c->style & WSD_STYLE_NOACTIVATE) != 0;
  w.window.level = [w levelForActive:NO];
  w.window.delegate = w;
  w.view = [[WsdView alloc] initWithFrame:NSMakeRect(0, 0, r.size.width, r.size.height)];
  w.view.owner = w;
  w.view.wantsLayer = YES;
  w.layer = (CAMetalLayer *)w.view.layer;
  w.window.contentView = w.view;
  /* the cursor the client set shows over the content (-cursorUpdate:) */
  [w.view addTrackingArea:[[NSTrackingArea alloc]
                              initWithRect:NSZeroRect
                                   options:NSTrackingCursorUpdate | NSTrackingActiveInActiveApp | NSTrackingInVisibleRect
                                     owner:w.view
                                  userInfo:nil]];
  [w.window makeFirstResponder:w.view];
  w.link = [w.view displayLinkWithTarget:w selector:@selector(tick:)];
  [w.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  self.windows[@(c->window_id)] = w;
  LOG("client %d: window %u %ux%u px style 0x%x", self.pid, c->window_id, c->width_px, c->height_px, c->style);
}

- (void)audioOpen:(const struct wsd_audio_open *)o input:(BOOL)input {
  uint32_t type = input ? WSD_AUDIO_OPEN_INPUT : WSD_AUDIO_OPEN;
  if (!self.fdQueue.count) { [self error:type code:-EBADF]; return; }
  int fd = self.fdQueue.firstObject.intValue;
  [self.fdQueue removeObjectAtIndex:0];
  uint32_t sample = o->format == WSD_AUDIO_S16 ? 2 : o->format == WSD_AUDIO_S24 ? 3 : o->format == WSD_AUDIO_U8 ? 1
                    : (o->format == WSD_AUDIO_S32 || o->format == WSD_AUDIO_F32) ? 4 : 0;
  struct stat st;
  size_t page = (size_t)getpagesize();
  uint64_t floor_off = o->offset & ~(uint64_t)(page - 1), delta = o->offset - floor_off;
  uint64_t maplen = (delta + o->size + page - 1) & ~(uint64_t)(page - 1);
  if (!o->stream_id || self.audio[@(o->stream_id)] || self.audio.count >= 32 || !sample || !o->channels ||
      o->channels > 8 || o->frame_bytes != sample * o->channels || o->rate < 4000 || o->rate > 384000 ||
      !o->ring_frames || o->ring_frames > (1u << 22) || o->size < sizeof(struct wsd_audio_ring) +
      (uint64_t)o->ring_frames * o->frame_bytes || (delta % 8) || fstat(fd, &st) ||
      (uint64_t)st.st_size < o->offset + o->size) { /* the mapping may end in a partial page past EOF: never read */
    close(fd);
    [self error:type code:-EINVAL];
    return;
  }
  void *p = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)floor_off);
  close(fd);
  if (p == MAP_FAILED) { [self error:type code:-errno]; return; }
  WsdAudio *a = [WsdAudio new];
  a.streamId = o->stream_id;
  a.map = p;
  a.mapLen = (size_t)maplen;
  struct wsd_audio_rt *rt = calloc(1, sizeof(*rt));
  rt->ring = (struct wsd_audio_ring *)((uint8_t *)p + delta);
  rt->data = (uint8_t *)rt->ring + sizeof(struct wsd_audio_ring);
  rt->ring_frames = o->ring_frames;
  rt->frame_bytes = o->frame_bytes;
  rt->silence = o->format == WSD_AUDIO_U8 ? 0x80 : 0;
  rt->channels = o->channels;
  a.rt = rt;
  a.input = input;
  self.audio[@(o->stream_id)] = a;
  LOG("client %d: audio %s stream %u: format %u, %u Hz, %u channels, ring %u frames", self.pid,
      input ? "input" : "output", o->stream_id, o->format,
      o->rate, o->channels, o->ring_frames);

  struct wsd_audio_open open = *o;
  pid_t pid = self.pid;
  __weak WsdClient *weakSelf = self;
  dispatch_async(g_audio_queue, ^{
    OSStatus e = input ? [a setUpInput:open] : [a setUp:open sample:sample];
    if (e == noErr) return;
    LOG("client %d: audio stream %u: AudioUnit setup failed (%d)", pid, open.stream_id, (int)e);
    dispatch_async(dispatch_get_main_queue(), ^{
      WsdClient *c = weakSelf;
      if (c.audio[@(open.stream_id)] != a) return; /* closed meanwhile */
      [c.audio removeObjectForKey:@(open.stream_id)];
      [c error:type code:-EIO];
    });
  });
}
/* A cursor image (SetCursor with a cursor that is not one of the Mac's): pixels are the game's, which the screen
 * shows at the effective scale (backing_scale()). At most 64 per client. */
- (void)defineCursor:(const struct wsd_cursor_define *)c {
  if (!self.fdQueue.count) { [self error:WSD_CURSOR_DEFINE code:-EBADF]; return; }
  int fd = self.fdQueue.firstObject.intValue;
  [self.fdQueue removeObjectAtIndex:0];
  struct stat st;
  if (c->cursor_id < WSD_CURSOR_FIRST_DEFINED || (!self.cursors[@(c->cursor_id)] && self.cursors.count >= 64) ||
      !c->width || !c->height || c->width > 256 || c->height > 256 || c->stride < c->width * 4 ||
      c->size < (uint64_t)c->stride * c->height || c->hot_x < 0 || c->hot_y < 0 || (uint32_t)c->hot_x >= c->width ||
      (uint32_t)c->hot_y >= c->height || fstat(fd, &st) || (uint64_t)st.st_size < c->offset + c->size) {
    close(fd);
    [self error:WSD_CURSOR_DEFINE code:-EINVAL];
    return;
  }
  NSMutableData *pixels = [NSMutableData dataWithLength:(NSUInteger)c->size];
  ssize_t got = pread(fd, pixels.mutableBytes, (size_t)c->size, (off_t)c->offset);
  close(fd);
  if (got != (ssize_t)c->size) { [self error:WSD_CURSOR_DEFINE code:-EIO]; return; }
  CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGImageRef image = CGImageCreate(c->width, c->height, 8, 32, c->stride, srgb,
                                   kCGBitmapByteOrder32Little | kCGImageAlphaFirst, provider, NULL, false,
                                   kCGRenderingIntentDefault);
  CGColorSpaceRelease(srgb);
  CGDataProviderRelease(provider);
  if (!image) { [self error:WSD_CURSOR_DEFINE code:-EINVAL]; return; }
  double s = backing_scale(nil);
  NSImage *picture = [[NSImage alloc] initWithCGImage:image size:NSMakeSize(c->width / s, c->height / s)];
  CGImageRelease(image);
  NSCursor *cursor = [[NSCursor alloc] initWithImage:picture hotSpot:NSMakePoint(c->hot_x / s, c->hot_y / s)];
  NSCursor *old = self.cursors[@(c->cursor_id)];
  self.cursors[@(c->cursor_id)] = cursor;
  if (old && g_cursor == old) {
    g_cursor = cursor;
    apply_cursor();
  }
}

/* The Dock and Cmd-Tab icon of the whole server (one game per wsd): the client's program icon. */
- (void)appIcon:(const struct wsd_app_icon *)a {
  if (!self.fdQueue.count) { [self error:WSD_APP_ICON code:-EBADF]; return; }
  int fd = self.fdQueue.firstObject.intValue;
  [self.fdQueue removeObjectAtIndex:0];
  struct stat st;
  if (!a->width || !a->height || a->width > 1024 || a->height > 1024 || a->stride < a->width * 4 ||
      a->size < (uint64_t)a->stride * a->height || fstat(fd, &st) || (uint64_t)st.st_size < a->offset + a->size) {
    close(fd);
    [self error:WSD_APP_ICON code:-EINVAL];
    return;
  }
  NSMutableData *pixels = [NSMutableData dataWithLength:(NSUInteger)a->size];
  ssize_t got = pread(fd, pixels.mutableBytes, (size_t)a->size, (off_t)a->offset);
  close(fd);
  if (got != (ssize_t)a->size) { [self error:WSD_APP_ICON code:-EIO]; return; }
  CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGImageRef image = CGImageCreate(a->width, a->height, 8, 32, a->stride, srgb,
                                   kCGBitmapByteOrder32Little | kCGImageAlphaFirst, provider, NULL, false,
                                   kCGRenderingIntentDefault);
  CGColorSpaceRelease(srgb);
  CGDataProviderRelease(provider);
  if (!image) { [self error:WSD_APP_ICON code:-EINVAL]; return; }
  NSApp.applicationIconImage = [[NSImage alloc] initWithCGImage:image size:NSZeroSize];
  CGImageRelease(image);
  LOG("client %d: application icon %ux%u", self.pid, a->width, a->height);
}

- (void)attachBuffer:(const struct wsd_buffer_attach *)a {
  if (!self.fdQueue.count) { [self error:WSD_BUFFER_ATTACH code:-EBADF]; return; }
  int fd = self.fdQueue.firstObject.intValue;
  [self.fdQueue removeObjectAtIndex:0];
  /* All 4 bytes per pixel; sampling converts each to the BGRA8 drawable. */
  MTLPixelFormat pf = a->format == WSD_FMT_BGRA8 ? MTLPixelFormatBGRA8Unorm
                      : a->format == WSD_FMT_RGBA8 ? MTLPixelFormatRGBA8Unorm
                      : a->format == WSD_FMT_RGB10A2 ? MTLPixelFormatRGB10A2Unorm : MTLPixelFormatInvalid;
  NSUInteger align = pf != MTLPixelFormatInvalid ? [g_dev minimumLinearTextureAlignmentForPixelFormat:pf] : 1;
  struct stat st;
  size_t page = (size_t)getpagesize();
  uint64_t floor_off = a->offset & ~(uint64_t)(page - 1), delta = a->offset - floor_off;
  uint64_t maplen = (delta + a->size + page - 1) & ~(uint64_t)(page - 1);
  if (!a->buffer_id || self.buffers[@(a->buffer_id)] || self.buffers.count >= WSD_MAX_BUFFERS ||
      pf == MTLPixelFormatInvalid || !a->width || !a->height || a->width > 16384 || a->height > 16384 ||
      a->stride < (uint64_t)a->width * 4 || a->size < (uint64_t)a->stride * a->height || (a->stride % align) ||
      (delta % align) || fstat(fd, &st) || (uint64_t)st.st_size < floor_off + maplen) {
    close(fd);
    [self error:WSD_BUFFER_ATTACH code:-EINVAL];
    return;
  }
  void *p = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)floor_off);
  close(fd); /* the mapping keeps the object alive */
  if (p == MAP_FAILED) { [self error:WSD_BUFFER_ATTACH code:-errno]; return; }
  size_t len = (size_t)maplen;
  id<MTLBuffer> mb = [g_dev newBufferWithBytesNoCopy:p length:len options:MTLResourceStorageModeShared
                                         deallocator:^(void *ptr, NSUInteger l) { munmap(ptr, l); }];
  if (!mb) { munmap(p, len); [self error:WSD_BUFFER_ATTACH code:-ENOMEM]; return; }
  MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:a->width
                                                                               height:a->height mipmapped:NO];
  td.usage = MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModeShared;
  id<MTLTexture> tex = [mb newTextureWithDescriptor:td offset:(NSUInteger)delta bytesPerRow:a->stride];
  if (!tex) { [self error:WSD_BUFFER_ATTACH code:-EINVAL]; return; }
  WsdBuffer *b = [WsdBuffer new];
  b.bufferId = a->buffer_id; b.width = a->width; b.height = a->height; b.stride = a->stride; b.format = a->format;
  b.texture = tex;
  b.cpu = (uint8_t *)p + delta;
  self.buffers[@(a->buffer_id)] = b;
  LOG("client %d: buffer %u %ux%u stride %u fmt %u mapped %zu bytes (no copy)", self.pid, a->buffer_id, a->width,
      a->height, a->stride, a->format, len);
}

- (void)debugInject:(WsdWindow *)w scenario:(uint32_t)sc {
  NSWindow *nw = w.window;
  double s = backing_scale(nw);
  NSInteger wn = nw.windowNumber;
  NSTimeInterval t = NSProcessInfo.processInfo.systemUptime;
  NSPoint p = NSMakePoint(10 / s, w.view.bounds.size.height - 20 / s); /* pixel (10,20) */
  g_injecting = YES;
  if (sc == 1) {
    NSEvent *kd = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:t
                               windowNumber:wn context:nil characters:@"a" charactersIgnoringModifiers:@"a"
                                  isARepeat:NO keyCode:kVK_ANSI_A];
    NSEvent *ku = [NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0 timestamp:t
                               windowNumber:wn context:nil characters:@"a" charactersIgnoringModifiers:@"a"
                                  isARepeat:NO keyCode:kVK_ANSI_A];
    NSEvent *sd = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint
                              modifierFlags:NSEventModifierFlagShift timestamp:t windowNumber:wn context:nil
                                 characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:kVK_Shift];
    NSEvent *su = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:0 timestamp:t
                               windowNumber:wn context:nil characters:@"" charactersIgnoringModifiers:@""
                                  isARepeat:NO keyCode:kVK_Shift];
    NSEvent *md = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:p modifierFlags:0 timestamp:t
                                 windowNumber:wn context:nil eventNumber:1 clickCount:1 pressure:1];
    NSEvent *mu = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:p modifierFlags:0 timestamp:t
                                 windowNumber:wn context:nil eventNumber:2 clickCount:1 pressure:0];
    NSEvent *rd = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown location:p modifierFlags:0 timestamp:t
                                 windowNumber:wn context:nil eventNumber:3 clickCount:1 pressure:1];
    NSEvent *ru = [NSEvent mouseEventWithType:NSEventTypeRightMouseUp location:p modifierFlags:0 timestamp:t
                                 windowNumber:wn context:nil eventNumber:4 clickCount:1 pressure:0];
    NSEvent *mm = [NSEvent mouseEventWithType:NSEventTypeMouseMoved location:p modifierFlags:0 timestamp:t
                                 windowNumber:wn context:nil eventNumber:5 clickCount:0 pressure:0];
    for (NSEvent *e in @[ kd, ku, sd, su, md, mu, rd, ru, mm ]) [nw sendEvent:e];
    CGEventRef sw = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 2, -3, 1);
    [w.view scrollWheel:[NSEvent eventWithCGEvent:sw]];
    CFRelease(sw);
  } else if (sc == 2) {
    /* Raw delta path: CGEvent mouse-moved carrying deltas, handed to the view. */
    CGEventRef mv = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, CGPointMake(100, 100), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(mv, kCGMouseEventDeltaX, 5);
    CGEventSetIntegerValueField(mv, kCGMouseEventDeltaY, -7);
    [w.view mouseMoved:[NSEvent eventWithCGEvent:mv]];
    CFRelease(mv);
  } else if (sc == 3) {
    /* Motion outside the clip rect: expect clamped coordinates (flag bit 0). */
    NSPoint far = NSMakePoint(w.view.bounds.size.width - 1, 1);
    NSEvent *mm = [NSEvent mouseEventWithType:NSEventTypeMouseMoved location:far modifierFlags:0 timestamp:t
                                 windowNumber:wn context:nil eventNumber:6 clickCount:0 pressure:0];
    [nw sendEvent:mm];
  }
  g_injecting = NO;
}

- (void)handle:(uint32_t)type data:(const uint8_t *)d len:(uint32_t)n {
#define NEED(T) if (n < sizeof(struct T)) { [self error:type code:-EMSGSIZE]; return; } const struct T *m = (const void *)d
  switch (type) {
  case WSD_HELLO: {
    NEED(wsd_hello);
    struct wsd_hello_reply r = display_info();
    g_display = r;
    LOG("client %d hello version %u", self.pid, m->version);
    [self send:WSD_EV_HELLO payload:&r len:sizeof(r) extra:nil];
    break;
  }
  case WSD_WINDOW_CREATE: {
    NEED(wsd_window_create);
    NSString *title = [[NSString alloc] initWithBytes:d + sizeof(*m) length:n - sizeof(*m)
                                             encoding:NSUTF8StringEncoding] ?: @"";
    [self createWindow:m title:title];
    break;
  }
  case WSD_WINDOW_DESTROY: {
    NEED(wsd_window);
    WsdWindow *w = [self win:m->window_id req:type];
    if (w) { [w destroy]; [self.windows removeObjectForKey:@(m->window_id)]; }
    break;
  }
  case WSD_WINDOW_SHOW: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (!w) break;
    w.minimizedOut = NO;
    if (m->value) {
      if (m->value == 1) [NSApp activate];
      [w orderInFront:m->value == 1];
      [w sendResize];
      [w logState:"show"];
      __weak WsdWindow *ww = w;
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1000000000), dispatch_get_main_queue(), ^{ [ww logState:"show+1s"]; });
    } else {
      [w.window orderOut:nil];
      adjust_levels(NSApp.active);
    }
    break;
  }
  case WSD_WINDOW_FULLSCREEN: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (w) [w setFullscreenMode:m->value != 0];
    break;
  }
  case WSD_CURSOR_VISIBLE: {
    NEED(wsd_window_flag);
    g_cursor_hide_wanted = !m->value;
    sync_cursor();
    break;
  }
  case WSD_CURSOR_CLIP: {
    NEED(wsd_cursor_clip);
    WsdWindow *w = [self win:m->window_id req:type];
    if (!w) break;
    w.clip = m->enable && m->width && m->height;
    w.clipPx = NSMakeRect(m->x, m->y, m->width, m->height);
    break;
  }
  case WSD_RELATIVE_MOUSE: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (!w) break;
    BOOL on = m->value != 0;
    if (on && w.stalled) { /* the next frame shown takes the pointer */
      w.relative = YES;
      LOG("client %d: relative mouse on, waiting for a frame", self.pid);
      break;
    }
    if (on != w.relative) {
      CGError e = CGAssociateMouseAndMouseCursorPosition(!on);
      LOG("client %d: relative mouse %s -> CGAssociateMouseAndMouseCursorPosition(%d) = %d", self.pid,
          on ? "on" : "off", !on, e);
      if (e != kCGErrorSuccess) { [self error:type code:e]; break; }
      w.relative = on;
      g_relative_global = on;
    }
    break;
  }
  case WSD_BUFFER_ATTACH: {
    NEED(wsd_buffer_attach);
    [self attachBuffer:m];
    break;
  }
  case WSD_AUDIO_OPEN:
  case WSD_AUDIO_OPEN_INPUT: {
    NEED(wsd_audio_open);
    [self audioOpen:m input:type == WSD_AUDIO_OPEN_INPUT];
    break;
  }
  case WSD_AUDIO_START:
  case WSD_AUDIO_STOP:
  case WSD_AUDIO_RESET:
  case WSD_AUDIO_CLOSE: {
    NEED(wsd_audio_stream);
    WsdAudio *a = self.audio[@(m->stream_id)];
    if (!a) { [self error:type code:-ENOENT]; break; }
    if (type == WSD_AUDIO_START) {
      atomic_store_explicit(&a.rt->running, 1, memory_order_release);
      uint32_t stream = m->stream_id;
      pid_t pid = self.pid;
      __weak WsdClient *weakSelf = self;
      dispatch_async(g_audio_queue, ^{
        if (!a.unit) return; /* its setup failed: the client gets that error */
        OSStatus e = AudioOutputUnitStart(a.unit);
        if (e == noErr) return;
        LOG("client %d: audio stream %u: start failed (%d)", pid, stream, (int)e);
        dispatch_async(dispatch_get_main_queue(), ^{
          WsdClient *c = weakSelf;
          if (c && !c.dead) [c error:WSD_AUDIO_START code:-EIO];
        });
      });
    } else if (type == WSD_AUDIO_STOP) {
      atomic_store_explicit(&a.rt->running, 0, memory_order_release); /* silence from the next render on */
      dispatch_async(g_audio_queue, ^{
        if (a.unit) AudioOutputUnitStop(a.unit);
      });
    } else if (type == WSD_AUDIO_RESET) {
      if (a.input) { [self error:type code:-EINVAL]; break; } /* the client reads the ring: it resets it itself */
      _Atomic uint64_t *wp = (_Atomic uint64_t *)&a.rt->ring->write_frames;
      atomic_store_explicit((_Atomic uint64_t *)&a.rt->ring->read_frames,
                            atomic_load_explicit(wp, memory_order_acquire), memory_order_release);
    } else {
      [self.audio removeObjectForKey:@(m->stream_id)];
      dispatch_async(g_audio_queue, ^{ [a teardown]; });
    }
    break;
  }
  case WSD_BUFFER_DETACH: {
    NEED(wsd_buffer);
    ((WsdBuffer *)self.buffers[@(m->buffer_id)]).detached = YES;
    [self.buffers removeObjectForKey:@(m->buffer_id)];
    break;
  }
  case WSD_PRESENT: {
    NEED(wsd_present);
    uint64_t rx = now_ns();
    WsdWindow *w = [self win:m->window_id req:type];
    WsdBuffer *b = self.buffers[@(m->buffer_id)];
    if (!w) break;
    if (!b) { [self error:type code:-ENOENT]; break; }
    if (w.pendingBuffer) { /* mailbox: the newer frame replaces the unshown one */
      [w releaseBuffer:w.pendingBuffer serial:w.pendingSerial shown:NO];
      w.replaced++;
    }
    w.pendingBuffer = b;
    w.pendingSerial = m->serial;
    w.pendingClientNs = m->client_time_ns;
    w.pendingReceivedNs = rx;
    break;
  }
  case WSD_DEBUG_INPUT: {
    NEED(wsd_debug_input);
    /* test only (driving a game from a host-side script): the target is the largest visible window of any client */
    WsdWindow *best = nil;
    double area = 0;
    for (WsdClient *c in g_clients)
      for (WsdWindow *w in c.windows.allValues) {
        NSSize sz = w.window.frame.size;
        if ((w.window.visible || (m->kind == 4 && w.window.miniaturized)) && sz.width * sz.height > area) {
          area = sz.width * sz.height;
          best = w;
        }
      }
    if (!best) { [self error:type code:-ENOENT]; break; }
    NSWindow *nw = best.window;
    if (m->kind == 4) {
      /* the AppKit calls behind the title bar, its buttons and the Dock: the delegate sees what a user does */
      double top = NSMaxY(primary_screen().frame);
      NSRect f = nw.frame;
      switch (m->a) {
      case WSD_DEBUG_WINDOW_MOVE: [nw setFrameTopLeftPoint:NSMakePoint(m->b, top - m->reserved)]; break;
      case WSD_DEBUG_WINDOW_SIZE: {
        NSRect c = [nw contentRectForFrameRect:f];
        c.origin.y += c.size.height - m->reserved;
        c.size = NSMakeSize(m->b, m->reserved);
        [nw setFrame:[nw frameRectForContentRect:c] display:YES];
        break;
      }
      case WSD_DEBUG_WINDOW_MINIMIZE: [nw performMiniaturize:nil]; break;
      case WSD_DEBUG_WINDOW_RESTORE: [nw deminiaturize:nil]; break;
      case WSD_DEBUG_WINDOW_ZOOM: [nw performZoom:nil]; break;
      case WSD_DEBUG_WINDOW_CLOSE: [nw performClose:nil]; break;
      case WSD_DEBUG_APP_QUIT: [NSApp terminate:nil]; break;
      }
      LOG("debug window action %u (%u, %u) -> window %u", m->a, m->b, m->reserved, best.windowId);
      break;
    }
    NSTimeInterval t = NSProcessInfo.processInfo.systemUptime;
    /* the first test input turns the probe on when WSD_LATENCY_PROBE was not set (the launcher passes a fixed
     * environment); it arms nothing itself: the probe needs earlier frames for its base and noise */
    if (!g_probe_enabled) {
      g_probe_enabled = YES;
      LOG("latency probe on (first test input)");
    } else if (m->kind != 3 && (m->kind == 2 || m->b)) probe_arm(m->kind == 1 ? "key" : "click");
    g_injecting = YES;
    if (m->kind == 3) {
      /* raw mouse motion (relative mode test): delta x = a - 10000, delta y = b - 10000, through the view as a real
       * mouse-moved event carrying deltas */
      CGEventRef mv = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, CGPointMake(100, 100), kCGMouseButtonLeft);
      CGEventSetIntegerValueField(mv, kCGMouseEventDeltaX, (int64_t)m->a - 10000);
      CGEventSetIntegerValueField(mv, kCGMouseEventDeltaY, (int64_t)m->b - 10000);
      [best.view mouseMoved:[NSEvent eventWithCGEvent:mv]];
      CFRelease(mv);
      g_injecting = NO;
      break;
    }
    if (m->kind == 1) {
      NSEvent *e = [NSEvent keyEventWithType:m->b ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint
                               modifierFlags:0 timestamp:t windowNumber:nw.windowNumber context:nil characters:@""
                 charactersIgnoringModifiers:@"" isARepeat:NO keyCode:(unsigned short)m->a];
      [nw sendEvent:e];
    } else if (m->kind == 2) {
      double sc = backing_scale(nw);
      NSPoint p = NSMakePoint(m->a / sc, best.view.bounds.size.height - m->b / sc);
      for (int i = 0; i < 2; i++) {
        NSEvent *e = [NSEvent mouseEventWithType:i ? NSEventTypeLeftMouseUp : NSEventTypeLeftMouseDown location:p
                                   modifierFlags:0 timestamp:t windowNumber:nw.windowNumber context:nil
                                     eventNumber:100 + i clickCount:1 pressure:i ? 0 : 1];
        [nw sendEvent:e];
      }
    }
    g_injecting = NO;
    LOG("debug input kind %u a %u b %u -> window %u", m->kind, m->a, m->b, best.windowId);
    break;
  }
  case WSD_PRESENT_REGION: {
    NEED(wsd_present_region);
    uint64_t rx = now_ns();
    WsdWindow *w = [self win:m->present.window_id req:type];
    if (!w) break;
    if (!m->width || !m->height) { [w removeOverlay]; break; }
    WsdBuffer *b = self.buffers[@(m->present.buffer_id)];
    if (!b) { [self error:type code:-ENOENT]; break; }
    WsdWindow *o = [w overlayAt:NSMakeRect(m->x, m->y, m->width, m->height)];
    if (o.pendingBuffer) {
      [o releaseBuffer:o.pendingBuffer serial:o.pendingSerial shown:NO];
      o.replaced++;
    }
    probe_frame(b, m->present.client_time_ns, rx);
    o.pendingBuffer = b;
    o.pendingSerial = m->present.serial;
    o.pendingClientNs = m->present.client_time_ns;
    o.pendingReceivedNs = rx;
    break;
  }
  case WSD_SET_TITLE: {
    NEED(wsd_window);
    WsdWindow *w = [self win:m->window_id req:type];
    if (w)
      w.window.title = [[NSString alloc] initWithBytes:d + sizeof(*m) length:n - sizeof(*m)
                                              encoding:NSUTF8StringEncoding] ?: @"";
    break;
  }
  case WSD_SET_FRAME_RATE: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (!w) break;
    float hz = (float)m->value;
    w.link.preferredFrameRateRange = hz > 0 ? CAFrameRateRangeMake(hz, hz, hz) : CAFrameRateRangeDefault;
    LOG("client %d: window %u preferred frame rate %u Hz", self.pid, m->window_id, m->value);
    break;
  }
  case WSD_PING: {
    NEED(wsd_ping);
    struct wsd_ping r = *m;
    r.time_ns = now_ns();
    [self send:WSD_EV_PONG payload:&r len:sizeof(r) extra:nil];
    break;
  }
  case WSD_DEBUG_INJECT: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (w) [self debugInject:w scenario:m->value];
    break;
  }
  case WSD_WINDOW_SET_FRAME: {
    NEED(wsd_window_frame);
    WsdWindow *w = [self win:m->window_id req:type];
    if (!w) break;
    if (((m->flags & WSD_FRAME_SIZE) && (!m->width || !m->height || m->width > 16384 || m->height > 16384)) ||
        w.fullscreen) {
      [self error:type code:-EINVAL];
      break;
    }
    [w setFramePx:m];
    break;
  }
  case WSD_WINDOW_SET_STYLE: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (w) [w setStyle:m->value];
    break;
  }
  case WSD_WINDOW_MINIMIZE: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (w) [w minimize:m->value != 0];
    break;
  }
  case WSD_APP_ICON: {
    NEED(wsd_app_icon);
    [self appIcon:m];
    break;
  }
  case WSD_OPEN_URL: {
    /* A link the game opens (the guest's xdg-open sends it): web addresses only, never a file or another app's
     * scheme. The log names the host alone: the rest may hold a sign-in code. */
    NSString *text = [[NSString alloc] initWithBytes:d length:n encoding:NSUTF8StringEncoding];
    NSURL *url = text ? [NSURL URLWithString:text] : nil;
    NSString *scheme = url.scheme.lowercaseString;
    if (!url.host.length || !([scheme isEqualToString:@"https"] || [scheme isEqualToString:@"http"])) {
      [self error:type code:-EINVAL];
      break;
    }
    LOG("client %d: opening a %s link to %s", self.pid, scheme.UTF8String, url.host.UTF8String);
    if (![NSWorkspace.sharedWorkspace openURL:url]) [self error:type code:-EIO];
    break;
  }
  case WSD_CURSOR_WARP: {
    NEED(wsd_cursor_pos);
    /* SetCursorPos, only while wsd is the active app (a game behind another app must not move the user's pointer) and
     * outside relative mode (the pointer stands still there and the game reads deltas). As in Wine's Mac driver
     * (-[WineApplicationController setCursorPosition:]), without the 0.25 s after a warp during which macOS keeps the
     * pointer still, then the mouse moves already queued are dropped (-motion:). */
    if (!NSApp.active || g_relative_global) break;
    double s = backing_scale(nil);
    CGSetLocalEventsSuppressionInterval(0);
    CGError e = CGWarpMouseCursorPosition(CGPointMake(m->x / s, m->y / s));
    CGSetLocalEventsSuppressionInterval(0.25);
    if (e != kCGErrorSuccess) { [self error:type code:-EIO]; break; }
    g_warp_time = NSProcessInfo.processInfo.systemUptime;
    CGAssociateMouseAndMouseCursorPosition(true);
    break;
  }
  case WSD_CURSOR_DEFINE: {
    NEED(wsd_cursor_define);
    [self defineCursor:m];
    break;
  }
  case WSD_CURSOR_SET: {
    NEED(wsd_cursor_set);
    NSCursor *c = m->cursor >= WSD_CURSOR_FIRST_DEFINED ? self.cursors[@(m->cursor)] : system_cursor(m->cursor);
    if (!c) { [self error:type code:-ENOENT]; break; }
    g_cursor = c;
    apply_cursor();
    break;
  }
  case WSD_WINDOW_FOCUS: {
    NEED(wsd_window);
    WsdWindow *w = [self win:m->window_id req:type];
    if (!w || !w.window.visible || ((WsdNSWindow *)w.window).noActivate) break;
    /* Wine activated the window (SetForegroundWindow, a dialog taking the focus): the key window follows, as Wine's
     * Mac driver gives it in ActivateWindow. wsd itself becomes active this way only at the game's start; later, a
     * window activated while another app is in front becomes key once wsd is active again. */
    if (NSApp.active) [w orderInFront:YES];
    else if (!g_ever_active) {
      [NSApp activate];
      [w orderInFront:YES];
    } else
      g_focus_wanted = w;
    break;
  }
  case WSD_SCREEN_SAVER: {
    NEED(wsd_window_flag);
    /* SystemParametersInfo(SPI_SETSCREENSAVEACTIVE): while the client asks, the display does not sleep on its own,
     * as Wine's Mac driver does it (macdrv_SystemParametersInfo); the assertion goes with the client */
    if (!m->value && !self.displayAssertion) {
      IOPMAssertionID a = kIOPMNullAssertionID;
      if (IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
                                      CFSTR("A Windows game asked to keep the display on"), &a) != kIOReturnSuccess) {
        [self error:type code:-EIO];
        break;
      }
      self.displayAssertion = a;
      LOG("client %d: the display stays on", self.pid);
    } else if (m->value && self.displayAssertion) {
      IOPMAssertionRelease(self.displayAssertion);
      self.displayAssertion = kIOPMNullAssertionID;
      LOG("client %d: the display may sleep again", self.pid);
    }
    break;
  }
  case WSD_QUIT:
    LOG("client %d asked the server to quit", self.pid);
    restore_input_state();
    [self flush];
    exit(0);
  default:
    [self error:type code:-ENOSYS];
  }
#undef NEED
}

- (void)readable {
  uint8_t buf[16384];
  char ctl[CMSG_SPACE(sizeof(int) * 4)];
  struct iovec iov = {buf, sizeof(buf)};
  struct msghdr mh = {0};
  mh.msg_iov = &iov; mh.msg_iovlen = 1;
  mh.msg_control = ctl; mh.msg_controllen = sizeof(ctl);
  ssize_t r = recvmsg(self.fd, &mh, 0);
  if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { [self close]; return; }
  if (r < 0) return;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int nfd = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      for (int i = 0; i < nfd; i++) {
        int f;
        memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
        if (self.fdQueue.count >= 8) close(f); else [self.fdQueue addObject:@(f)];
      }
    }
  [self.inbuf appendBytes:buf length:(NSUInteger)r];
  NSUInteger off = 0;
  while (!self.dead && self.inbuf.length - off >= sizeof(struct wsd_hdr)) {
    struct wsd_hdr h;
    memcpy(&h, (const uint8_t *)self.inbuf.bytes + off, sizeof(h));
    if (h.len > WSD_MAX_PAYLOAD) { LOG("client %d: oversized message", self.pid); [self close]; return; }
    if (self.inbuf.length - off < sizeof(h) + h.len) break;
    uint8_t payload[WSD_MAX_PAYLOAD];
    memcpy(payload, (const uint8_t *)self.inbuf.bytes + off + sizeof(h), h.len);
    off += sizeof(h) + h.len;
    [self handle:h.type data:payload len:h.len];
  }
  if (!self.dead) [self.inbuf replaceBytesInRange:NSMakeRange(0, off) withBytes:NULL length:0];
}
@end

/* ---------------------------------------------------------------- main */

/* The screen with the menu bar changed (resolution, scale, a screen plugged in or out): fullscreen windows take the
 * new frame and the clients hear of it (EV_DISPLAY), as Wine's Mac driver tells Wine
 * (-applicationDidChangeScreenParameters:). A work area alone (the Dock shown or hidden) is not news: the client
 * gives the game the whole screen. */
static void screens_changed(void) {
  for (WsdClient *c in g_clients)
    for (WsdWindow *w in c.windows.allValues) [w fitScreen];
  struct wsd_hello_reply r = display_info();
  if (r.display_width_px == g_display.display_width_px && r.display_height_px == g_display.display_height_px &&
      r.max_refresh_hz == g_display.max_refresh_hz && r.backing_scale_x1000 == g_display.backing_scale_x1000)
    return;
  g_display = r;
  LOG("main screen now %ux%u px at %u Hz, scale %.2f", r.display_width_px, r.display_height_px, r.max_refresh_hz,
      r.backing_scale_x1000 / 1000.0);
  for (WsdClient *c in g_clients) [c send:WSD_EV_DISPLAY payload:&r len:sizeof(r) extra:nil];
}

/* Back in wsd (Cmd-Tab, the Dock) with none of its windows visible: the largest of those in the Dock or minimized out
 * of sight comes back, as Wine's Mac driver brings one back (-unminimizeWindowIfNoneVisible). A game in exclusive
 * fullscreen minimizes itself when it goes to the background (Wine's D3D9, Unreal), and nothing was left to come back
 * to. The client hears it as the user's restore (EV_MINIMIZE 0). */
static void unminimize_if_none_visible(void) {
  WsdWindow *best = nil;
  double area = 0;
  for (WsdClient *c in g_clients)
    for (WsdWindow *w in c.windows.allValues) {
      if (w.window.visible) return;
      NSSize sz = w.window.frame.size;
      if ((w.window.miniaturized || w.minimizedOut) && sz.width * sz.height > area) {
        area = sz.width * sz.height;
        best = w;
      }
    }
  if (!best) return;
  LOG("window %u comes back: no window was visible", best.windowId);
  if (best.window.miniaturized) {
    [best.window deminiaturize:nil]; /* -windowDidDeminiaturize: tells the client */
    return;
  }
  best.minimizedOut = NO;
  [best orderInFront:YES];
  struct wsd_window_flag f = {best.windowId, 0};
  [best sendEvent:WSD_EV_MINIMIZE payload:&f len:sizeof(f) extra:nil];
}

/* Whether the quit is a logout, a restart or a shutdown: the reason the Apple event gives, as Wine's Mac driver reads
 * it (-applicationShouldTerminate:). */
static BOOL session_ending(void) {
  NSAppleEventDescriptor *e = NSAppleEventManager.sharedAppleEventManager.currentAppleEvent;
  switch ([e attributeDescriptorForKeyword:kAEQuitReason].typeCodeValue) {
  case kAELogOut:
  case kAEReallyLogOut:
  case kAERestart:
  case kAEShowRestartDialog:
  case kAEShutDown:
  case kAEShowShutdownDialog:
    return YES;
  }
  return NO;
}

/* A logout, restart or shutdown leaves the game the time to close */
#define SESSION_END_WAIT (30 * NSEC_PER_SEC)

/* Quit (Cmd-Q, the Dock) asks the game to close its windows, as the close button does, instead of ending the server
 * under a running game (which then lost its windows and input but kept running); whoever started the game ends the
 * server after it. Without any window, Quit ends the server. */
@interface WsdAppDelegate : NSObject <NSApplicationDelegate>
@end
@implementation WsdAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
  NSUInteger asked = 0;
  for (WsdClient *c in g_clients)
    for (WsdWindow *w in c.windows.allValues) {
      struct wsd_window m = {w.windowId};
      [w sendEvent:WSD_EV_CLOSE payload:&m len:sizeof(m) extra:nil];
      asked++;
    }
  if (!asked) return NSTerminateNow;
  /* The Mac waits for the game to close during a logout, restart or shutdown (Wine's Mac driver answers
   * NSTerminateLater the same way): cancelling stopped the logout as long as a game ran. The answer comes once the
   * last client is gone, or a refusal after SESSION_END_WAIT if the game is still there (it may be asking the player
   * something). */
  if (session_ending()) {
    if (!g_terminate_pending) {
      g_terminate_pending = YES;
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, SESSION_END_WAIT), dispatch_get_main_queue(), ^{
        if (!g_terminate_pending) return;
        g_terminate_pending = NO;
        LOG("the game is still there, the logout, restart or shutdown is refused");
        [NSApp replyToApplicationShouldTerminate:NO];
      });
    }
    LOG("logout, restart or shutdown: asked %lu windows to close", (unsigned long)asked);
    return NSTerminateLater;
  }
  LOG("quit requested: asked %lu windows to close", (unsigned long)asked);
  return NSTerminateCancel;
}
/* The windows above the menu bar (fullscreen, and those in front of one) come down as wsd resigns, before the other
 * app's windows come to the front (Wine's Mac driver also does it in -applicationWillResignActive:), and go back up
 * once wsd is active again. */
- (void)applicationWillResignActive:(NSNotification *)n {
  adjust_levels(NO);
}
/* The game goes to the background as it does on Windows: its client puts the desktop in the foreground, as Wine's Mac
 * driver does on the same notification (APP_DEACTIVATED, macdrv_app_deactivated), and the game gets WM_ACTIVATEAPP
 * (it may pause, mute or minimize itself). Wine kept the game in the foreground until then, whatever app the user
 * was in. */
- (void)applicationDidResignActive:(NSNotification *)n {
  for (WsdClient *c in g_clients) [c send:WSD_EV_APP_DEACTIVATED payload:NULL len:0 extra:nil];
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)flag {
  unminimize_if_none_visible();
  return YES;
}
- (void)applicationDidBecomeActive:(NSNotification *)n {
  if (g_ever_active) unminimize_if_none_visible();
  g_ever_active = YES;
  WsdWindow *w = g_focus_wanted;
  g_focus_wanted = nil;
  if (w.window.visible && !((WsdNSWindow *)w.window).noActivate) [w orderInFront:YES];
  adjust_levels(YES);
}
@end

/* The application menu: the bundle's name (Obelisk starts wsd from a bundle named after the game), Hide and Quit. */
static void make_menu(void) {
  NSString *name = NSBundle.mainBundle.infoDictionary[@"CFBundleName"] ?: NSProcessInfo.processInfo.processName;
  BOOL fr = [NSLocale.preferredLanguages.firstObject hasPrefix:@"fr"];
  NSMenu *bar = [NSMenu new], *menu = [[NSMenu alloc] initWithTitle:name];
  NSMenuItem *item = [NSMenuItem new];
  [menu addItemWithTitle:[(fr ? @"Masquer " : @"Hide ") stringByAppendingString:name] action:@selector(hide:)
           keyEquivalent:@"h"];
  [menu addItem:NSMenuItem.separatorItem];
  [menu addItemWithTitle:[(fr ? @"Quitter " : @"Quit ") stringByAppendingString:name] action:@selector(terminate:)
           keyEquivalent:@"q"];
  item.submenu = menu;
  [bar addItem:item];
  NSApp.mainMenu = bar;
}

static int make_listener(const char *dir, char *path_out, size_t path_cap) {
  if (mkdir(dir, 0700) && errno != EEXIST) { LOG("mkdir %s: %s", dir, strerror(errno)); return -1; }
  struct stat st;
  if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077)) {
    LOG("refusing %s: must be a directory owned by uid %d with mode 0700", dir, getuid());
    return -1;
  }
  struct sockaddr_un sa = {.sun_family = AF_UNIX};
  int n = snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/wsd.sock", dir);
  if (n < 0 || (size_t)n >= sizeof(sa.sun_path)) { LOG("socket path too long (limit %zu)", sizeof(sa.sun_path) - 1); return -1; }
  unlink(sa.sun_path);
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof(sa)) || chmod(sa.sun_path, 0600) || listen(s, 8)) {
    LOG("listen %s: %s", sa.sun_path, strerror(errno));
    return -1;
  }
  fcntl(s, F_SETFL, O_NONBLOCK);
  snprintf(path_out, path_cap, "%s", sa.sun_path);
  return s;
}

static void accept_one(int ls) {
  int fd = accept(ls, NULL, NULL);
  if (fd < 0) return;
  uid_t uid; gid_t gid;
  pid_t pid = -1;
  socklen_t pl = sizeof(pid);
  getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &pl);
  if (getpeereid(fd, &uid, &gid) || uid != getuid()) {
    LOG("rejecting peer pid %d uid %d (server uid %d)", pid, (int)uid, (int)getuid());
    close(fd);
    return;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  fcntl(fd, F_SETFL, O_NONBLOCK);
  WsdClient *c = [WsdClient new];
  c.fd = fd; c.pid = pid;
  c.inbuf = [NSMutableData data]; c.outbuf = [NSMutableData data];
  c.fdQueue = [NSMutableArray array];
  c.windows = [NSMutableDictionary dictionary]; c.buffers = [NSMutableDictionary dictionary];
  c.audio = [NSMutableDictionary dictionary];
  c.cursors = [NSMutableDictionary dictionary];
  c.readSrc = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, 0, dispatch_get_main_queue());
  __weak WsdClient *wc = c;
  dispatch_source_set_event_handler(c.readSrc, ^{ [wc readable]; });
  dispatch_resume(c.readSrc);
  [g_clients addObject:c];
  LOG("client pid %d uid %d accepted", pid, (int)uid);
}

/* --render-percent N: a whole number kept between 1 and 100; anything not made of digits only is ignored (100). */
static int parse_render_percent(const char *arg) {
  char *end = NULL;
  long v = strtol(arg, &end, 10);
  if (*arg < '0' || *arg > '9' || *end) {
    LOG("--render-percent \"%s\" is not an integer, ignored", arg);
    return 100;
  }
  return (int)MIN(MAX(v, 1L), 100L);
}

int main(int argc, char **argv) {
  @autoreleasepool {
    const char *dir = NULL, *render_percent = NULL;
    for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
      else if (!strcmp(argv[i], "--exit-on-disconnect")) g_exit_on_disconnect = YES;
      else if (!strcmp(argv[i], "--render-percent")) /* a following option is not its value */
        render_percent = i + 1 < argc && strncmp(argv[i + 1], "--", 2) ? argv[++i] : "(no value)";
    }
    if (!dir) {
      fprintf(stderr, "usage: wsd --dir <private-dir> [--exit-on-disconnect] [--render-percent N]\n");
      return 2;
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (render_percent) g_render_percent = parse_render_percent(render_percent);
    g_probe_enabled = getenv("WSD_LATENCY_PROBE") != NULL;

    g_dev = MTLCreateSystemDefaultDevice();
    g_queue = [g_dev newCommandQueue];
    g_audio_queue = dispatch_queue_create("wsd.audio", DISPATCH_QUEUE_SERIAL);
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:kShader options:nil error:&err];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_pipe = lib ? [g_dev newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
    if (!g_pipe) { LOG("Metal setup failed: %s", err.localizedDescription.UTF8String); return 1; }

    static char path[128];
    int ls = make_listener(dir, path, sizeof(path));
    if (ls < 0) return 1;
    g_clients = [NSMutableSet set];

    /* Termination (timeout wrapper sends SIGALRM): restore pointer state, then exit. */
    static dispatch_source_t sigsrc[4];
    int sigs[4] = {SIGTERM, SIGINT, SIGALRM, SIGHUP};
    for (int i = 0; i < 4; i++) {
      signal(sigs[i], SIG_IGN);
      sigsrc[i] = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sigs[i], 0, dispatch_get_main_queue());
      int sn = sigs[i];
      dispatch_source_set_event_handler(sigsrc[i], ^{
        LOG("signal %d, restoring input state and exiting", sn);
        restore_input_state();
        unlink(path);
        exit(0);
      });
      dispatch_resume(sigsrc[i]);
    }
    atexit(restore_input_state);
    /* a Quit from the Dock or the app menu ends the process through -terminate: say so in the log */
    [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationWillTerminateNotification object:nil
                                                       queue:nil usingBlock:^(NSNotification *note) {
      LOG("application quit requested (Dock or menu), restoring input state");
      restore_input_state();
    }];

    /* windows on the Space just shown may have kept a level of when wsd was active (adjust_levels) */
    [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceActiveSpaceDidChangeNotification
                                                                object:nil queue:NSOperationQueue.mainQueue
                                                            usingBlock:^(NSNotification *note) {
      adjust_levels(NSApp.active);
    }];

    [NSApplication sharedApplication];
    static WsdAppDelegate *delegate;
    delegate = [WsdAppDelegate new];
    NSApp.delegate = delegate;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    make_menu();
    [NSApp finishLaunching];
    /* App Nap would slow wsd down while its windows are covered and nothing plays, with the game waiting on it (its
     * frames, its input, a match being found meanwhile); Wine's Mac driver turns it off the same way
     * (-[WineApplicationController transformProcessToForeground:]). Idle system sleep stays allowed. */
    g_app_nap_off = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                                                                 reason:@"Running a Windows game"];
    [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                                      object:nil queue:NSOperationQueue.mainQueue
                                                  usingBlock:^(NSNotification *note) {
      screens_changed();
    }];

    static dispatch_source_t acc;
    acc = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)ls, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(acc, ^{ accept_one(ls); });
    dispatch_resume(acc);

    /* every second, not per frame: windows holding the pointer without new frames give it back */
    static dispatch_source_t stall_timer;
    stall_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(stall_timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC,
                              NSEC_PER_SEC / 20);
    dispatch_source_set_event_handler(stall_timer, ^{
      uint64_t now = now_ns();
      for (WsdClient *c in g_clients)
        for (WsdWindow *w in c.windows.allValues) [w checkStalled:now];
    });
    dispatch_resume(stall_timer);

    LOG("listening on %s (pid %d, device %s, max %ld Hz)", path, getpid(), g_dev.name.UTF8String,
        (long)primary_screen().maximumFramesPerSecond);
    struct wsd_hello_reply shown = display_info();
    LOG("render percent: requested %s, applied %d; the screen's scale %.3f reaches the clients as %.3f (desktop %ux%u px)",
        render_percent ?: "none", g_render_percent, primary_screen().backingScaleFactor, backing_scale(nil),
        shown.display_width_px, shown.display_height_px);
    [NSApp run];
  }
  return 0;
}
