/*
 * wsd_protocol.h - wire format of the Obelisk window server (wsd). See PROTOCOL.md.
 *
 * Every message: struct wsd_hdr followed by `len` payload bytes (len <= WSD_MAX_PAYLOAD).
 * All fields little-endian, fixed width, naturally aligned; the same layout holds for a
 * Linux x86-64 client (checked by the static asserts below). At most one fd rides on a
 * message (SCM_RIGHTS); only BUFFER_ATTACH carries one.
 */
#ifndef WSD_PROTOCOL_H
#define WSD_PROTOCOL_H

#include <stdint.h>

#define WSD_PROTOCOL_VERSION 1u
#define WSD_MAX_PAYLOAD 4096u
#define WSD_MAX_WINDOWS 16u
#define WSD_MAX_BUFFERS 32u

struct wsd_hdr {
  uint32_t type;
  uint32_t len; /* payload bytes after this header */
};

/* client -> server */
enum {
  WSD_HELLO = 1,           /* wsd_hello */
  WSD_WINDOW_CREATE = 2,   /* wsd_window_create + UTF-8 title (rest of payload) */
  WSD_WINDOW_DESTROY = 3,  /* wsd_window */
  WSD_WINDOW_SHOW = 4,     /* wsd_window_flag (value: 1 show and activate, 2 show without activating, 0 hide) */
  WSD_WINDOW_FULLSCREEN = 5, /* wsd_window_flag (value: 1 on, 0 off) */
  WSD_CURSOR_VISIBLE = 6,  /* wsd_window_flag (window ignored, value: visible) */
  WSD_CURSOR_CLIP = 7,     /* wsd_cursor_clip */
  WSD_RELATIVE_MOUSE = 8,  /* wsd_window_flag (value: 1 relative, 0 absolute) */
  WSD_BUFFER_ATTACH = 9,   /* wsd_buffer_attach + 1 fd */
  WSD_BUFFER_DETACH = 10,  /* wsd_buffer */
  WSD_PRESENT = 11,        /* wsd_present */
  WSD_SET_TITLE = 12,      /* wsd_window + UTF-8 title */
  WSD_SET_FRAME_RATE = 13, /* wsd_window_flag (value: preferred Hz, 0 = display default) */
  WSD_PING = 14,           /* wsd_ping */
  WSD_DEBUG_INJECT = 15,   /* wsd_window_flag (value: scenario); synthetic NSEvents, test only */
  WSD_QUIT = 16,           /* no payload; server exits after flushing */
  WSD_WINDOW_SET_FRAME = 17, /* wsd_window_frame: content position and size in screen pixels */
  WSD_PRESENT_REGION = 18, /* wsd_present_region: present into an overlay plane at a rectangle of the window */
  WSD_DEBUG_INPUT = 19,    /* wsd_debug_input: test only, synthetic input to the largest visible window of any client */
  WSD_AUDIO_OPEN = 20,     /* wsd_audio_open + 1 fd: an output stream reading a shared ring (struct wsd_audio_ring) */
  WSD_AUDIO_START = 21,    /* wsd_audio_stream: the server starts consuming the ring */
  WSD_AUDIO_STOP = 22,     /* wsd_audio_stream: pause, unread frames stay in the ring */
  WSD_AUDIO_RESET = 23,    /* wsd_audio_stream: (stopped) drop unread frames: read position = write position */
  WSD_AUDIO_CLOSE = 24,    /* wsd_audio_stream */
  WSD_WINDOW_SET_STYLE = 25, /* wsd_window_flag (value: WSD_STYLE_*): the content keeps its place on screen */
  WSD_WINDOW_MINIMIZE = 26, /* wsd_window_flag (value: 1 into the Dock, 0 back) */
  WSD_APP_ICON = 27,       /* wsd_app_icon + 1 fd: the Dock and Cmd-Tab icon */
  WSD_OPEN_URL = 28,       /* UTF-8 http or https address: opened in the Mac's default browser */
  WSD_CURSOR_WARP = 29,    /* wsd_cursor_pos: the pointer goes to x,y screen pixels (SetCursorPos) */
  WSD_CURSOR_DEFINE = 30,  /* wsd_cursor_define + 1 fd: a cursor image the client then sets by its id */
  WSD_CURSOR_SET = 31,     /* wsd_cursor_set: the cursor shown over the server's windows */
  WSD_WINDOW_FOCUS = 32,   /* wsd_window: the client activated this window, which becomes the key window */
  WSD_SCREEN_SAVER = 33,   /* wsd_window_flag (window ignored, value: 1 the display may sleep, 0 it stays on) */
  WSD_AUDIO_OPEN_INPUT = 34, /* wsd_audio_open + 1 fd: an input stream (the default microphone) writing a shared ring */
};

/* server -> client */
enum {
  WSD_EV_HELLO = 101,      /* wsd_hello_reply */
  WSD_EV_KEY = 102,        /* wsd_ev_key + UTF-8 characters */
  WSD_EV_BUTTON = 103,     /* wsd_ev_button */
  WSD_EV_MOTION = 104,     /* wsd_ev_motion (absolute, window pixels, top-left origin) */
  WSD_EV_RELATIVE = 105,   /* wsd_ev_relative (raw deltas, relative mode only) */
  WSD_EV_WHEEL = 106,      /* wsd_ev_wheel */
  WSD_EV_FOCUS = 107,      /* wsd_window_flag (value: 1 in, 0 out) */
  WSD_EV_RESIZE = 108,     /* wsd_ev_resize */
  WSD_EV_CLOSE = 109,      /* wsd_window (close button pressed; window stays open) */
  WSD_EV_PRESENTED = 110,  /* wsd_ev_presented */
  WSD_EV_RELEASE = 111,    /* wsd_ev_release (server no longer reads the buffer) */
  WSD_EV_PONG = 112,       /* wsd_ping */
  WSD_EV_ERROR = 113,      /* wsd_ev_error */
  WSD_EV_FRAME = 114,      /* wsd_window_frame: the user moved or resized the window (MOVE and SIZE flags set) */
  WSD_EV_MINIMIZE = 115,   /* wsd_window_flag (value: 1 the user sent the window to the Dock, 0 it came back) */
  WSD_EV_ZOOM = 116,       /* wsd_window: the user pressed the zoom (green) button */
  WSD_EV_DISPLAY = 117,    /* wsd_hello_reply: the main screen changed (size, scale, refresh rate or work area) */
  WSD_EV_APP_DEACTIVATED = 118, /* no payload: another app became the active one (Cmd-Tab, a click, a link opened) */
};

enum { WSD_FMT_BGRA8 = 1, WSD_FMT_RGBA8 = 2, WSD_FMT_RGB10A2 = 3 };

enum {
  WSD_STYLE_TITLED = 1u << 0,
  WSD_STYLE_CLOSABLE = 1u << 1,
  WSD_STYLE_RESIZABLE = 1u << 2,
  WSD_STYLE_MINIATURIZABLE = 1u << 3,
  WSD_STYLE_BORDERLESS = 1u << 4, /* overrides the others */
  WSD_STYLE_TOPMOST = 1u << 5,    /* NSFloatingWindowLevel (HWND_TOPMOST) */
  WSD_STYLE_NOACTIVATE = 1u << 6, /* never the key window (WS_EX_NOACTIVATE) */
};

/* wsd_ev_key.flags / modifiers mirror NSEventModifierFlags bits 16..23 shifted down by 16. */
/* WSD_KEY_ISO: the key comes from an ISO keyboard, where kVK_ISO_Section is the key left of 1 and kVK_ANSI_Grave the
 * one right of the left Shift (their places are the other way round on an ANSI keyboard). */
enum { WSD_KEY_DOWN = 1u << 0, WSD_KEY_REPEAT = 1u << 1, WSD_KEY_ISO = 1u << 2, WSD_KEY_SYNTHETIC = 1u << 31 };

struct wsd_hello { uint32_t version, client_pid; };
struct wsd_hello_reply {
  uint32_t version, server_pid;
  uint32_t display_width_px, display_height_px; /* main screen, pixels at the effective scale */
  uint32_t max_refresh_hz;                      /* NSScreen.maximumFramesPerSecond */
  uint32_t backing_scale_x1000;                 /* the effective scale, see PROTOCOL.md (Render percent) */
  int32_t work_x, work_y;                       /* main screen without the menu bar and the Dock, same pixels, */
  uint32_t work_width, work_height;             /* origin top-left of the main screen (0x0 from older servers) */
};
struct wsd_window_create {
  uint32_t window_id; /* chosen by the client, nonzero */
  uint32_t width_px, height_px;
  uint32_t style;
};
struct wsd_window { uint32_t window_id; };
struct wsd_window_flag { uint32_t window_id; uint32_t value; };
struct wsd_cursor_clip {
  uint32_t window_id;
  uint32_t enable;
  int32_t x, y; /* window pixels, top-left origin */
  uint32_t width, height;
};
struct wsd_buffer_attach {
  uint32_t buffer_id; /* chosen by the client, nonzero */
  uint32_t width, height;
  uint32_t stride;    /* bytes per row */
  uint32_t format;    /* WSD_FMT_* */
  uint32_t reserved;
  uint64_t offset;    /* byte offset of the first row within the fd */
  uint64_t size;      /* stride * height */
};
struct wsd_buffer { uint32_t buffer_id; };
struct wsd_window_frame {
  uint32_t window_id;
  uint32_t flags;          /* WSD_FRAME_* */
  int32_t x, y;            /* content top-left, screen pixels, origin top-left of the main screen */
  uint32_t width, height;  /* content size in pixels */
};
enum { WSD_FRAME_MOVE = 1u << 0, WSD_FRAME_SIZE = 1u << 1 };
struct wsd_present {
  uint32_t window_id, buffer_id;
  uint64_t serial;
  uint64_t client_time_ns; /* clock_gettime_nsec_np(CLOCK_UPTIME_RAW) at submit */
};
struct wsd_ping { uint64_t token; uint64_t time_ns; };
struct wsd_debug_input {
  uint32_t kind;   /* 1 key (a = mac keycode, b = 1 down / 0 up), 2 left click at window pixels (a, b),
                      3 raw motion, 4 window action as the user does it (a = WSD_DEBUG_WINDOW_*) */
  uint32_t a, b;
  uint32_t reserved; /* kind 4: second coordinate */
};
/* wsd_debug_input kind 4: MOVE puts the frame's top-left at (b, reserved) screen points, SIZE gives the content
 * b x reserved points; the others act like the title bar buttons, the application menu and the Dock */
enum { WSD_DEBUG_WINDOW_MOVE = 1, WSD_DEBUG_WINDOW_SIZE = 2, WSD_DEBUG_WINDOW_MINIMIZE = 3,
       WSD_DEBUG_WINDOW_RESTORE = 4, WSD_DEBUG_WINDOW_ZOOM = 5, WSD_DEBUG_WINDOW_CLOSE = 6,
       WSD_DEBUG_APP_QUIT = 7 /* Quit from the application menu or the Dock */ };
struct wsd_present_region {
  struct wsd_present present;
  int32_t x, y;            /* overlay rectangle in window pixels, top-left origin */
  uint32_t width, height;  /* 0x0: remove the overlay plane (present.buffer_id ignored) */
};

/* Audio output (PCM, interleaved, little-endian). The client writes frames at ring frame (write_frames % ring_frames)
 * and then publishes write_frames (release); the server's audio callback reads from read_frames and publishes it
 * (release). Both counters only grow (64-bit frame counts); unread = write_frames - read_frames <= ring_frames.
 * The ring data follows the 64-byte header at `offset` in the fd.
 * An input stream (WSD_AUDIO_OPEN_INPUT) swaps the roles: the server writes write_frames and the client publishes
 * read_frames; underrun_frames counts the frames the server dropped because the ring was full. The client resets
 * the stream itself (read_frames = write_frames), WSD_AUDIO_RESET is refused. START, STOP and CLOSE work as for output;
 * the frames are converted to the open request's format, rate and channels. */
enum { WSD_AUDIO_S16 = 1, WSD_AUDIO_S24 = 2, WSD_AUDIO_S32 = 3, WSD_AUDIO_F32 = 4, WSD_AUDIO_U8 = 5 };
struct wsd_audio_ring {
  uint64_t write_frames;  /* client */
  uint64_t read_frames;   /* server */
  uint64_t underrun_frames; /* server: silence frames played while running with an empty ring */
  uint64_t reserved[5];
};
struct wsd_audio_open {
  uint32_t stream_id;     /* chosen by the client, nonzero */
  uint32_t format;        /* WSD_AUDIO_* */
  uint32_t rate;          /* frames per second */
  uint32_t channels;
  uint32_t frame_bytes;   /* bytes per frame (channels * sample bytes) */
  uint32_t ring_frames;
  uint64_t offset;        /* byte offset of struct wsd_audio_ring within the fd */
  uint64_t size;          /* 64 + ring_frames * frame_bytes */
};
struct wsd_audio_stream { uint32_t stream_id; uint32_t reserved; };
struct wsd_cursor_pos { int32_t x, y; }; /* screen pixels, origin top-left of the main screen */
/* The cursors of the Mac a client sets by number; WSD_CURSOR_FIRST_DEFINED and up are the client's own images. */
enum { WSD_CURSOR_ARROW = 0, WSD_CURSOR_IBEAM = 1, WSD_CURSOR_CROSSHAIR = 2, WSD_CURSOR_RESIZE_LEFT_RIGHT = 3,
       WSD_CURSOR_RESIZE_UP_DOWN = 4, WSD_CURSOR_NOT_ALLOWED = 5, WSD_CURSOR_POINTING_HAND = 6,
       WSD_CURSOR_CLOSED_HAND = 7, WSD_CURSOR_DRAG_COPY = 8, WSD_CURSOR_DRAG_LINK = 9,
       WSD_CURSOR_FIRST_DEFINED = 0x100 };
struct wsd_cursor_define {
  uint32_t cursor_id;      /* WSD_CURSOR_FIRST_DEFINED or more, chosen by the client; defining it again replaces it */
  uint32_t width, height;  /* at most 256x256 */
  uint32_t stride;         /* bytes per row */
  int32_t hot_x, hot_y;    /* hot spot, pixels from the top-left corner */
  uint64_t offset;         /* byte offset of the first row within the fd */
  uint64_t size;           /* stride * height; pixels are BGRA8 with straight (not premultiplied) alpha */
};
struct wsd_cursor_set { uint32_t cursor; uint32_t reserved; }; /* WSD_CURSOR_* or a defined id */
struct wsd_app_icon {
  uint32_t width, height;  /* at most 1024x1024 */
  uint32_t stride;         /* bytes per row */
  uint32_t reserved;
  uint64_t offset;         /* byte offset of the first row within the fd */
  uint64_t size;           /* stride * height; pixels are BGRA8 with straight (not premultiplied) alpha */
};

struct wsd_ev_key {
  uint32_t window_id;
  uint32_t flags;      /* WSD_KEY_* */
  uint32_t mac_keycode; /* kVK_* virtual keycode */
  uint32_t modifiers;
  uint64_t time_ns;
};
struct wsd_ev_button {
  uint32_t window_id;
  uint32_t button; /* 0 left, 1 right, 2 middle, 3.. others */
  uint32_t down;
  uint32_t modifiers;
  float x, y;
  uint64_t time_ns;
};
struct wsd_ev_motion { uint32_t window_id; uint32_t flags; float x, y; uint64_t time_ns; };
struct wsd_ev_relative { uint32_t window_id; uint32_t flags; float dx, dy; uint64_t time_ns; };
struct wsd_ev_wheel { uint32_t window_id; uint32_t precise; float dx, dy; uint64_t time_ns; };
/* backing_scale_x1000: the effective scale, see PROTOCOL.md (Render percent) */
struct wsd_ev_resize { uint32_t window_id; uint32_t width_px, height_px; uint32_t backing_scale_x1000; };
struct wsd_ev_presented {
  uint32_t window_id, buffer_id;
  uint64_t serial;
  uint64_t client_time_ns;   /* echoed */
  uint64_t received_ns;      /* server got the PRESENT */
  uint64_t encoded_ns;       /* display-link tick that encoded it */
  uint64_t presented_ns;     /* CAMetalDrawable.presentedTime, 0 if never shown */
  uint32_t replaced;         /* frames released unshown since the previous present (mailbox) */
  uint32_t reserved;
};
struct wsd_ev_release { uint32_t buffer_id; uint32_t shown; uint64_t serial; };
struct wsd_ev_error { uint32_t request_type; int32_t code; };

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define WSD_ASSERT_SIZE(t, n) _Static_assert(sizeof(struct t) == (n), #t " layout")
#elif defined(__cplusplus)
#define WSD_ASSERT_SIZE(t, n) static_assert(sizeof(struct t) == (n), #t " layout")
#else
#define WSD_ASSERT_SIZE(t, n)
#endif
WSD_ASSERT_SIZE(wsd_hdr, 8);
WSD_ASSERT_SIZE(wsd_hello_reply, 40);
WSD_ASSERT_SIZE(wsd_window_create, 16);
WSD_ASSERT_SIZE(wsd_cursor_clip, 24);
WSD_ASSERT_SIZE(wsd_buffer_attach, 40);
WSD_ASSERT_SIZE(wsd_present, 24);
WSD_ASSERT_SIZE(wsd_window_frame, 24);
WSD_ASSERT_SIZE(wsd_present_region, 40);
WSD_ASSERT_SIZE(wsd_debug_input, 16);
WSD_ASSERT_SIZE(wsd_audio_ring, 64);
WSD_ASSERT_SIZE(wsd_audio_open, 40);
WSD_ASSERT_SIZE(wsd_app_icon, 32);
WSD_ASSERT_SIZE(wsd_ev_key, 24);
WSD_ASSERT_SIZE(wsd_ev_button, 32);
WSD_ASSERT_SIZE(wsd_ev_motion, 24);
WSD_ASSERT_SIZE(wsd_ev_presented, 56);
WSD_ASSERT_SIZE(wsd_ev_release, 16);

#endif
