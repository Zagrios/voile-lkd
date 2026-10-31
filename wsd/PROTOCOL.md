# wsd protocol, version 1

`wsd` is a native arm64 Cocoa process that owns the macOS windows of one user session. Clients (the
future Wine display driver unix side, Neptune's `nptunix` WSI, test witnesses) talk to it over one
`AF_UNIX` `SOCK_STREAM` connection. The C definitions are in `wsd_protocol.h`; this file explains the
rules that the header cannot express.

## Transport and security

- Socket: `<dir>/wsd.sock`, where `<dir>` is passed with `--dir`. `wsd` creates `<dir>` with mode
  0700 if needed and refuses to start unless it is a real directory (not a symlink) owned by the
  server uid with no group/other bits. The socket file is chmod 0600. `sun_path` is limited to 103
  bytes on Darwin; `wsd` refuses longer paths.
- Peer check: every accepted connection is checked with `getpeereid`; a peer whose effective uid
  differs from the server uid is closed immediately. `LOCAL_PEERPID` is logged.
- Framing: `struct wsd_hdr { u32 type; u32 len; }` then `len` payload bytes, `len <= 4096`. All
  integers little-endian, structs naturally aligned with fixed-width fields, so a Linux x86-64
  client (Wine under FEX) uses the same layout. A larger `len` closes the connection.
- File descriptors: sent with `SCM_RIGHTS` on any byte of the stream; the server queues received fds
  in arrival order (at most 8 pending) and `BUFFER_ATTACH` consumes the oldest one. Clients send the
  fd with the message that uses it.
- Errors: a rejected request produces `EV_ERROR { request_type, -errno }`; the connection stays open.
- Backpressure: the server writes non-blocking with an output queue; a client that lets more than
  8 MiB of events pile up is disconnected.
- Lifetime: closing the connection destroys the client's windows and buffers. `--exit-on-disconnect`
  makes the server exit when its last client leaves (used by the witnesses). `QUIT` exits at once.
  On SIGTERM, SIGINT, SIGHUP and SIGALRM the server restores pointer association and cursor
  visibility before exiting.

## Requests (client to server)

| type | name | payload | effect |
| --- | --- | --- | --- |
| 1 | HELLO | `wsd_hello` | replies `EV_HELLO` with the size in pixels, max refresh and effective scale (see Render percent) of the screen with the menu bar (the origin of window coordinates, whichever screen has the key window), and its work area (without the menu bar and the Dock; zero from older servers) |
| 2 | WINDOW_CREATE | `wsd_window_create` + UTF-8 title | NSWindow + layer-backed view with an opaque black `CAMetalLayer`; size in pixels; ids are client-chosen, max 16. `WSD_STYLE_TITLED` gives the macOS title bar (with close, minimize and zoom buttons as `CLOSABLE`, `MINIATURIZABLE`, `RESIZABLE` ask), `BORDERLESS` none; `TOPMOST` floats above the windows that are not, `NOACTIVATE` never becomes the key window |
| 3 | WINDOW_DESTROY | `wsd_window` | releases a pending frame, restores relative mode, closes |
| 4 | WINDOW_SHOW | `wsd_window_flag` | 1: activate app, in front of the server's windows as the key window, then one `EV_RESIZE`; 2: in front without becoming key and without activating the app (Win32 `SWP_NOACTIVATE`); 0: `orderOut` |
| 5 | WINDOW_FULLSCREEN | `wsd_window_flag` | 1: borderless, frame = screen frame, Dock and menu bar hidden, above the menu bar while the server is the active app (see Levels); 0: restore. Exactly one `EV_RESIZE` per transition, and one when the screen changes |
| 6 | CURSOR_VISIBLE | `wsd_window_flag` (window ignored) | `[NSCursor hide]` / `unhide`, balanced |
| 7 | CURSOR_CLIP | `wsd_cursor_clip` | absolute motion outside the rectangle (window pixels) is clamped (`EV_MOTION.flags` bit 0) and the real pointer is warped back with `CGWarpMouseCursorPosition` |
| 8 | RELATIVE_MOUSE | `wsd_window_flag` | 1: `CGAssociateMouseAndMouseCursorPosition(false)`; motion is reported only as `EV_RELATIVE` raw deltas (`NSEvent.deltaX/Y`) |
| 9 | BUFFER_ATTACH | `wsd_buffer_attach` + 1 fd | `mmap(MAP_SHARED)` of the fd from the page floor of `offset`, `newBufferWithBytesNoCopy`, linear `MTLTexture` at the remaining delta with `bytesPerRow = stride`. `stride` and the delta must be multiples of `minimumLinearTextureAlignmentForPixelFormat`. The fd is closed after mapping. Formats: 1 BGRA8, 2 RGBA8, 3 RGB10A2 (DXGI R10G10B10A2_UNORM, red in the low bits). Max 32 buffers |
| 10 | BUFFER_DETACH | `wsd_buffer` | drops the server reference; the mapping is unmapped when the last in-flight GPU use ends |
| 11 | PRESENT | `wsd_present` | queues the buffer for the window (mailbox: an unshown older frame is released with `shown=0` and counted in the next `EV_PRESENTED.replaced`) |
| 12 | SET_TITLE | `wsd_window` + UTF-8 | window title |
| 13 | SET_FRAME_RATE | `wsd_window_flag` | `CADisplayLink.preferredFrameRateRange` = value Hz (0: display default) |
| 14 | PING | `wsd_ping` | `EV_PONG` with server time |
| 15 | DEBUG_INJECT | `wsd_window_flag` | test only: synthetic NSEvents fed to the window (1: key, modifiers, buttons, motion, wheel; 2: raw delta; 3: motion outside the clip rect). Every resulting event carries flag bit 31 (`WSD_KEY_SYNTHETIC`) |
| 16 | QUIT | none | server exits |
| 25 | WINDOW_SET_STYLE | `wsd_window_flag` | value = `WSD_STYLE_*`; the content keeps its place on screen. While fullscreen, the frame is applied when fullscreen ends; `TOPMOST` and `NOACTIVATE` at once |
| 26 | WINDOW_MINIMIZE | `wsd_window_flag` | 1: into the Dock (`miniaturize:`; a window without a minimize button is ordered out), 0: back (an ordered-out window is shown again without becoming key) |
| 27 | APP_ICON | `wsd_app_icon` + 1 fd | BGRA8 pixels with straight alpha, at most 1024x1024, read once (`pread`): the application icon of the server (Dock, Cmd-Tab) |
| 28 | OPEN_URL | UTF-8 address | an `http` or `https` address with a host, opened in the Mac's default browser (`NSWorkspace openURL:`); any other is rejected with `-EINVAL`. The guest's `xdg-open` sends it for the links a game opens |
| 29 | CURSOR_WARP | `wsd_cursor_pos` | the pointer goes to x,y screen pixels (`CGWarpMouseCursorPosition`, without the 0.25 s macOS holds it still afterwards); mouse moves queued before it are dropped. Ignored while the server is not the active app and in relative mode |
| 30 | CURSOR_DEFINE | `wsd_cursor_define` + 1 fd | BGRA8 pixels with straight alpha, at most 256x256, read once (`pread`): a cursor of the client, shown at the effective scale (see Render percent), set later by its id (`WSD_CURSOR_FIRST_DEFINED` and up, at most 64 per client; defining an id again replaces it) |
| 31 | CURSOR_SET | `wsd_cursor_set` | the cursor over the content of the server's windows: one of the Mac's (`WSD_CURSOR_*`) or a defined one; `-ENOENT` for an unknown id. CURSOR_VISIBLE still hides it |
| 32 | WINDOW_FOCUS | `wsd_window` | the client activated the window: it comes in front as the key window. While another app is active it becomes key once the server is active again; the server activates itself this way only before it was ever active (a game's start). Ignored for a `NOACTIVATE` window |
| 33 | SCREEN_SAVER | `wsd_window_flag` (window ignored) | 0: the display does not sleep on its own (`PreventUserIdleDisplaySleep`) until 1 or the client leaves |
| 34 | AUDIO_OPEN_INPUT | `wsd_audio_open` + 1 fd | like the audio output open (type 20, see `wsd_protocol.h`), for the default microphone: wsd fills the shared ring, the client reads it and publishes `read_frames`. The frames arrive in the open request's rate, channels and sample format (CoreAudio converts them). START, STOP and CLOSE as for output; RESET is refused with `-EINVAL`, the client resets the ring itself. macOS asks the user for microphone access when the first stream starts |
| 18 | PRESENT_REGION | `wsd_present_region` | like PRESENT, but into an overlay plane (a `CAMetalLayer` sublayer) placed at x,y width x height window pixels; used for a D3D swapchain in the client area while the base plane keeps the GDI surface (frame, title). 0x0 removes the plane. `EV_PRESENTED`/`EV_RELEASE` as for PRESENT |
| 17 | WINDOW_SET_FRAME | `wsd_window_frame` | moves (`WSD_FRAME_MOVE`: content top-left at x,y screen pixels, origin top-left of the screen with the menu bar) and/or resizes (`WSD_FRAME_SIZE`: content width x height pixels, 1..16384). A size change sends exactly one `EV_RESIZE`. When AppKit places the window elsewhere (a title bar is kept below the menu bar and on screen), an `EV_FRAME` gives the frame it got. Rejected with `-EINVAL` while fullscreen |

## Events (server to client)

| type | name | payload |
| --- | --- | --- |
| 101 | EV_HELLO | `wsd_hello_reply` |
| 102 | EV_KEY | `wsd_ev_key` + UTF-8 `NSEvent.characters` (empty for modifiers). `mac_keycode` is the `kVK_*` virtual keycode; modifier keys arrive through `flagsChanged` with `WSD_KEY_DOWN` computed from the modifier bit, except Caps Lock, a toggle: a press and a release for each change, and one more pair when a window becomes key if it changed meanwhile. `WSD_KEY_ISO`: the key comes from an ISO keyboard, where `kVK_ISO_Section` and `kVK_ANSI_Grave` swap places. No IME/text-input composition yet |
| 103 | EV_BUTTON | `wsd_ev_button`: 0 left, 1 right, 2+ `buttonNumber`; position in window pixels, top-left origin |
| 104 | EV_MOTION | `wsd_ev_motion` absolute, window pixels; bit 0 = clamped by the clip rectangle |
| 105 | EV_RELATIVE | `wsd_ev_relative` raw deltas (relative mode only) |
| 106 | EV_WHEEL | `wsd_ev_wheel`: `scrollingDeltaX/Y`, bit 0 of `precise` = trackpad-precise |
| 107 | EV_FOCUS | `wsd_window_flag`: 1 key window gained, 0 lost |
| 108 | EV_RESIZE | `wsd_ev_resize`: content size in pixels and the effective scale (see Render percent) |
| 109 | EV_CLOSE | `wsd_window`: close button pressed; the window stays open until WINDOW_DESTROY |
| 110 | EV_PRESENTED | `wsd_ev_presented`: echo of the client timestamp, server receive time, display-link encode time and `CAMetalDrawable.presentedTime` (0 when the drawable was never shown, for example while the window is occluded) |
| 111 | EV_RELEASE | `wsd_ev_release`: the GPU finished reading the buffer for that serial (`shown=1`) or it was replaced unshown (`shown=0`); the client may write it again |
| 112 | EV_PONG | `wsd_ping` |
| 113 | EV_ERROR | `wsd_ev_error` |
| 114 | EV_FRAME | `wsd_window_frame` (MOVE and SIZE set): the user moved or resized the window, or AppKit placed it (content, screen pixels). Not sent for WINDOW_SET_FRAME that lands where asked, nor while fullscreen or in the Dock |
| 115 | EV_MINIMIZE | `wsd_window_flag`: 1 the window went into the Dock, 0 it came back (also after WINDOW_MINIMIZE: the client ignores a state it already has) |
| 116 | EV_ZOOM | `wsd_window`: the zoom (green) button; the client maximizes or restores and sets the frame itself |
| 117 | EV_DISPLAY | `wsd_hello_reply`: the screen with the menu bar changed size, scale or refresh rate (not sent for its work area alone) |
| 118 | EV_APP_DEACTIVATED | no payload: another app became the active one (Cmd-Tab, a click elsewhere, a link opened in the browser); the client puts the game in the background |

All times are `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` nanoseconds, the same timebase as
`mach_absolute_time` and `CACurrentMediaTime`.

## Application

Obelisk starts wsd from a bundle named after the game (`CFBundleName`), so the menu bar, the Dock and Cmd-Tab show
the game's name; APP_ICON gives its icon. The application menu has Hide and Quit. Quit (Cmd-Q, the Dock) sends
`EV_CLOSE` to every window instead of ending the server under a running game; with no window it ends the server. A
logout, restart or shutdown also sends `EV_CLOSE` and waits (`NSTerminateLater`): it goes on once the last client is
gone, and is refused if a client is still there after 30 seconds. App Nap is off for as long as the server runs.
Coming back to the server (Cmd-Tab, the Dock) with none of its windows visible brings back the largest window in the
Dock or minimized out of sight, which its client hears as `EV_MINIMIZE` 0: a game in exclusive fullscreen minimizes
itself when it goes to the background.

## Render percent

`wsd --render-percent N` lets the server tell its clients of a smaller screen, for the whole run. N is a whole number
from 1 to 100; a value outside it is held at 1 or 100, and anything that is not made of digits only is ignored (100,
the default). Every pixel quantity on the wire is a pixel at the effective scale, the pixels per point wsd uses to
convert to AppKit's points: `min(backing, max(backing * N / 100, min_scale))`, where `backing` is the screen's backing
scale and `min_scale` is `max(640 / width, 480 / height)` for the width and height in points of the screen with the
menu bar. The reported screen is therefore never below 640x480 pixels (a game cannot run on less; a screen that is
smaller itself keeps its size), and the scale is never above the backing scale. `backing_scale_x1000` (`EV_HELLO`,
`EV_DISPLAY`, `EV_RESIZE`) is the effective scale; at 100 it is the backing scale itself.

On a 2x screen of 1512x982 points, 50 gives a scale of 1.0: the client sees a 1512x982 screen instead of 3024x1964,
renders those pixels, and macOS stretches the picture to the screen; 25 gives 0.5 (756x491), and 1 is held at 0.489
(739x480). At 50 or less on a 2x screen a window of 1280x720 pixels is 1280x720 points or more, not 640x360, so
windowed games come out larger; this suits borderless and fullscreen windows. On a 1x screen the percent lowers the
scale below 1 in the same way. The option has no protocol message: a server from before it ignores the flag and keeps
the backing scale.

## Levels

As in Wine's Mac driver, a fullscreen window is above the menu bar and the Dock only while the server is the active
app. Going back to front in the window server's order, every window on the active Space takes at least the level of
the windows behind it, so a window shown over a fullscreen one rises with it, and `TOPMOST` windows stay above all the
others. While the server is not active, a `TOPMOST` window still floats above the other apps' windows unless it is
fullscreen (a game in D3D exclusive fullscreen has its window topmost). The levels are set again when the server resigns or becomes active, when the active Space changes, and when a
window is shown, hidden, destroyed, or enters or leaves fullscreen.

## Redisplay

When a frame was encoded but never shown (`presentedTime` 0, typically the first frame after WINDOW_SHOW while the
window is not on screen yet) and the client has nothing newer queued, the server encodes the same buffer again on the
next ticks (at most 240 times), without sending events.

## Color

The `CAMetalLayer` is tagged sRGB (`colorspace = kCGColorSpaceSRGB`, BGRA8 unorm drawable): buffer values are
sRGB-encoded, as Win32 GDI and UNORM D3D swapchains produce them, and the window server color-matches them to the
display. Before this tag the raw values were shown in the display space (red 220,40,40 looked orange).

## Present path

1. The client writes pixels into a shared buffer it does not have in flight (it owns a buffer again
   after `EV_RELEASE`), then sends PRESENT.
2. On the next `CADisplayLink` tick of that window (main thread), the server takes a drawable from
   the `CAMetalLayer` (BGRA8, `framebufferOnly`, 3 drawables, `displaySyncEnabled`), sets
   `drawableSize` to the buffer size and runs one render pass that samples the buffer's linear
   texture (scaling and RGBA/BGRA swizzle in the sampler). No CPU pixel copy happens in the server.
3. `presentDrawable` then `commit`; the completed handler sends `EV_RELEASE`, the presented handler
   sends `EV_PRESENTED`.

GPU-produced buffers (DXMT shared textures) must be complete before PRESENT: today the producer
waits for its own D3D11 completion (EVENT query). A shared fence is future work (see the report).
