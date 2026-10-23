# Graphics, display, input and audio for emulated Linux x86-64 Wine on macOS: inventory of options

Living report, updated at each milestone. It separates what was READ or MEASURED in this session from what is INFERRED. The `file:line` citations point to the absolute paths given in the "Paths and abbreviations" section. Everything marked "to be completed" is not established yet.

Last update: 2026-10-02, 15:50 local time (graphics host session: milestones G1 to G4, section "Status of milestones G1 to G4").

## Current state in one sentence

On the host side, the Metal chain is qualified by owned witnesses without any Wine guest: patched Neptune vtest server built from source (G1), DXMT D3D11 rendering of a triangle verified pixel by pixel and shared between two processes through a `DMTX` shm (G2, with UTM's `dxmt-native`; building from source is stuck on the missing Metal compiler), `wsd` window server that presents without copy at 60 and 120 Hz and forwards input (G3), and design of the Neptune hookup and of the Wine driver (G4).

## Resumption of 2026-10-02 after the third interruption (state of the evidence)

The journal of the previous attempt (vtest spike) did NOT survive: the working copy lived in a temporary session directory that no longer exists (`/private/tmp/claude-501/...` contains only two empty session directories, no copy of `virglrenderer`). The spike's numbers (section 1.6) therefore remain claims without an on-disk journal until they are replayed: they are marked "MEASURED (previous attempt, journal lost)" and replayed in section 1.7 of this resumption.

Facts added in this resumption (each fact points to its journal under `EXP/logs/` or `EXP/evidence/graphics-display-audio/`):

- MEASURED `logs/graphics-voile-archs-001.log`: the Voile install contains TWO trees, `Voile/current/` (installed on 2026-09-26 19:26) and `Voile/v26.3.0-4/` (11:47). The D3DMetal binaries are byte-for-byte identical in both (same SHA-256) and all x86_64 only; Wine's Unix libraries (`winemac.so`, `winecoreaudio.so`, `winebus.so`, `ntdll.so`, `win32u.so`, `winevulkan.so`) differ between the two trees (different sizes and SHA-256) and are all x86_64 only. `runtime.json` exists only in `v26.3.0-4/`.
- MEASURED `logs/graphics-dlopen-d3dmetal-system-001.log`: on macOS 27.0.1 (26A434), `dlopen("/System/Library/Frameworks/D3DMetal.framework/Versions/A/D3DMetal")` fails in native arm64 and in x86_64 under Rosetta ("no such file, not in dyld cache"). Yet the install name of Voile's D3DMetal points to this system path: there is therefore no D3DMetal provided by the system, only Voile's or UTM's exists. Rosetta 2 is installed (`arch -x86_64 /usr/bin/true` returns 0; package `com.apple.pkg.RosettaUpdateAuto`).
- MEASURED `logs/graphics-utm-archs-001.log` and `logs/graphics-native-exports-001.log`: the numbers of section 1.2 are reconfirmed (UTM's D3DMetal x86_64 only, 5,263,904 B; `d3dmetal-native` universal with 69 exported symbols in x86_64 and 0 in arm64; `dxmt-native` universal with the same 16 exported symbols in both slices; `render-server-real` universal).
- READ: another workstream (HyperLinux and native FEX) compiled today a macOS arm64 vtest server in `RHL/probes/rocket-performance-20260930/fex-native-hyperlinux-20261001/native-neptune-vtest-runtime/` and got protocol 4, valid Neptune capset and initialized context (`owned-protocol-20261002T081849820140Z/result.json`), then a shared blob of 49,152 bytes read back (`shared-memory-qualified.json`). This workstream uses a different shm fix (`mkstemp` followed by `unlink` in `/private/tmp`) than my spike (`shm_open`). I did not modify it and I did not reuse it as evidence of my own.

### Resumption of 2026-10-02 (fourth session, 14:24 local time)

- MEASURED `logs/graphics-spike-state-001.log`: the copy `EXP/src/graphics-spike/virgl/` (577 files, created at 11:03) is byte-for-byte IDENTICAL to `NEP/virgl` (`diff -rq` empty) and to `virgl-orig/`. The patches of appendix A were therefore NOT applied to this copy (zero occurrences of `out_fence_wr` in `vtest/vtest_renderer.c`, zero of `os_create_anonymous_file` in `vtest/vtest_shm.c`). No build directory exists. The numbers of section 1.6 therefore remain "MEASURED (previous attempt, journal lost)" until the replay documented in 1.7.
- The "Status of milestones G1 to G4" section below reports the application of the patches, the build, the run of the server and of the native clients, with the journals under `EXP/logs/graphics-*`.
- Sections 3 to 7 were written in this session (milestones written to disk as they went).

## Status of milestones G1 to G4 (graphics host session, 2026-10-02 afternoon)

This section replaces the "section 1.7" announced above, which was never written. `W` = `EXP/witnesses/graphics`, `G` = `EXP/src/graphics-spike`.

### G1. Host renderer: REACHED (with borrowed `dxmt-native`)

MEASURED:

- Appendix A applied to `G/virgl` by `G/patches/apply_annex_a.py` (exact block replacement, idempotent), `G/virgl-orig` stays intact. Resulting diff: `G/patches/annex-a.diff`, 113 lines, SHA-256 `07f711fb…d27a` (`logs/graphics-annexA-apply-001.log`).
- Build: `G/build-virgl.sh` (libepoxy 1.5.10 without GLX/EGL/X11 already installed in `G/prefix`, then virglrenderer `-Dneptune=true -Dvtest=true -Dvenus=false -Drender-server-mode=process`, `ninja -j8`): 97 units, `rc=0`, produces `G/prefix/bin/virgl_test_server` and `G/prefix/libexec/virgl_render_server` arm64 (`logs/graphics-virgl-build-001.log`).
- Owned witness `W/vtest_client.c` (built by `W/build-vtest-client.sh`, launched by `W/run-vtest-client.sh`, all under `run-with-timeout.sh`, socket in a `mktemp -d` 0700 folder, 62-byte path). Result `logs/graphics-vtest-client-003.log`: protocol 4, `MAX_TIMELINE_COUNT=64`, valid capset 7 of 60 bytes, `wire_format_version=1`, `caps_flags=0x36`, `CONTEXT_INIT` followed by an answered `GET_PARAM`, server journal "loaded D3D11 library … dxmt-native" and "created Neptune context 1", `RESULT PASS`.
- `HOST3D_GUEST` blobs received through SCM_RIGHTS, `mmap(MAP_SHARED)`, coherence checked by a second independent mapping of the same fd: 64 KiB, 12,288 B (the `st_size` becomes 16,384), 1 MiB, 16 MiB. Latency of creation + `mmap` + write of one byte per 16 KiB page (n=50): median 0.030 ms / p99 0.071 ms (64 KiB), 0.069 / 0.085 ms (1 MiB), 0.614 / 0.700 ms (16 MiB).
- Empty `SUBMIT_CMD2` with `RING_IDX|OUT_FENCE_FD`: 200/200 fds readable per ring; ring 0 median 0.017 ms, p95 0.024, p99 0.056, max 0.059; ring 1 median 0.017, p95 0.035, p99 0.062, max 0.095.
- Measured trap (`logs/graphics-vtest-client-001.log`, `-002.log`, `logs/graphics-dxmt-borrow-00{1,2,3}.log`): an arm64 slice extracted by `lipo -thin`, then a bare copy of the universal Mach-O (same SHA-256 `f7444460…6dfd`), both fail at `dlopen` with "code signature invalid (errno=1)" while the file in place loads. Turing Software's Developer ID signature binds the bundle's `Info.plist`; the whole framework must be copied (`ditto`), which is done in `G/third_party/utm-5.0.5/dxmt-native.framework` (provenance in `PROVENANCE.txt`). The first pass of the witness returned PASS while the worker had loaded no backend (Neptune creates the context despite the failure): the launch script therefore requires the absence of "failed to open NPT_" in the server journal.

READ: the worker also logs "failed to load dxmt_open_existing_heap_from_fd": this UTM 5.0.5 binary does not export the heap import; no effect on this witness.

INFERRED: the fence fd becomes readable when the worker's timeline fence is retired, which does not prove the end of GPU writes (see 1.6). This witness submits no serialized Neptune command (no `D3D11CreateDevice` through the ring): DXMT rendering is qualified separately in G2.

Borrowed versus built: virglrenderer, libepoxy and the witness are built from source; `dxmt-native` is the UTM 5.0.5 binary (stepping stone, to be replaced by the G2 build).

### G2. DXMT on this Mac: step 1 (borrowed binary) REACHED, step 2 (from source) BLOCKED by the missing Metal compiler

MEASURED, step 1 (`logs/graphics-dxmt-triangle-borrowed-001.log`, UTM 5.0.5 binary `f7444460…6dfd`):

- Witness `W/dxmt_triangle.m` (Objective-C++: the MinGW D3D11 headers provided by DXMT define `__uuidof` and the COM interfaces only in C++), built by `W/build-dxmt-triangle.sh`, loads `dxmt-native` through `dlopen` like the Neptune worker.
- Shaders: precompiled DXBC SM 5.0 `vs_pass` and `ps_color`, copied from `dxmt/tests/native/shaders_dxbc.h` at commit `bcdcf6da` (compiled offline by fxc under Windows according to `tests/native/README.md`, then `xxd -i`); copy and SHA-256 in `W/dxbc/PROVENANCE.txt`. They were not regenerated here (no fxc on macOS).
- `offscreen` mode: `D3D11CreateDevice` returns level `0xb100` (11_1), vendor `0x106b`; `dlopen` 432 ms at first launch (3 ms afterwards), device creation 12 to 32 ms. Three-color triangle in a 256x256 RGBA8 target, read back through a STAGING texture and compared pixel by pixel with the analytic barycentric color (tolerance 3/255, pixels less than about 1.5 px from an edge excluded): 63,088 pixels judged, 19,602 inside, 0 mismatches, centroid 85,85,86,255. First draw up to GPU completion (EVENT query) 114 ms (pipeline compilation), then mean 0.316 ms and maximum 0.962 ms over 20 draws.
- `shared` mode: BGRA8 texture `D3D11_RESOURCE_MISC_SHARED`; `IDXGIResource::GetSharedHandle` returns a `DMTX` version 2 POD (fd 11, 256x256, format 87, `stride` 1024, `size` 262,144, `offset` 0, shm object of 262,144 B). After the draw, three reads agree without mismatch: the STAGING copy, an `mmap` mapping of the fd in the producer, and a second process launched by `posix_spawn` that receives the fd through SCM_RIGHTS on a `socketpair`, maps it and checks the pixels; identical CRC32 (`473ce849`); 7.1 ms from `posix_spawn` to the reply.

READ: DXMT's native shared texture is a linear Metal texture backed by an anonymous POSIX shm created by `shm_open(O_EXCL)` then `shm_unlink`, mapped by `mmap(MAP_SHARED)` and wrapped by `newBufferWithBytesNoCopy` (`dxmt/src/d3d11/d3d11_texture_shared_native.cpp:36-78, 140-194`); `GetSharedHandle` returns a pointer to the POD held by the texture (`d3d11_texture_device.cpp:306-330`); opening on the consumer side redoes the same mapping on the received fd (`:600-635`). `SHARED_NTHANDLE` is refused (`:415-420`).

INFERRED: waiting for GPU completion goes here through an EVENT query in the same process; a remote consumer does not have an end-of-write signal yet (see 1.6, shared fence to be qualified in G3/G4).

Step 2: `utmapp/dxmt` sources cloned at commit `bcdcf6da89af6c9fedeb2fc92b4367b23d5d6ed6` ("native: shared-texture handle version 2 with a placement offset", 2026-08-15), submodules `include/native/directx` `9df86f23` and `external/nvapi` `d08488fc` (`logs/graphics-dxmt-fetch-001.log`); LLVM 15.0.7 source downloaded (SHA-256 `8b5fcb24…eac6`, computed locally, not compared with a published checksum); minimal LLVM build launched by `G/build-llvm15.sh` (`logs/graphics-llvm15-build-001.log`).

MEASURED, step 2: minimal LLVM 15.0.7 (no target, no tool, arm64, `-j8`, 1,824 units) built and installed in `G/llvm15` with Apple clang 21 without patch (`LLVM15_DONE`, `logs/graphics-llvm15-build-001.log`). DXMT's meson configuration (`G/build-dxmt.sh`, `-Dnative_llvm_path=G/llvm15`, macOS min 13.0) succeeds, but the build stops on the 7 `.air` targets (`air_msad.metal`, `air_samplepos.metal`, `air_tessellation.metal`, `dxmt_command.metal`): "cannot execute tool 'metal' due to missing Metal Toolchain" (`logs/graphics-dxmt-build-001.log`). `xcodebuild -showComponent MetalToolchain` returns "Status: uninstalled" (`logs/graphics-metal-toolchain-001.log`). The offline Metal compiler is an Xcode component to download (`xcodebuild -downloadComponent MetalToolchain`), a global install that I did not do without agreement: REQUEST to the lead. A `ninja -k 0` pass (`logs/graphics-dxmt-build-002.log`) finds no other error, but it stops at 25 units out of 146 because almost everything depends on the `.air` files: the absence of other blockers is therefore NOT established.

### G3. Presentation and input host ("wsd"): REACHED, with reservations

Files: `EXP/src/wsd/wsd.m` (Objective-C, a single application thread: AppKit, socket I/O through GCD sources on the main queue, Metal), `wsd_protocol.h` (binary format, sizes checked by `_Static_assert` to be identical on the Linux x86-64 side), `PROTOCOL.md`, `build.sh` (ad hoc signature, no entitlement: `codesign -d --entitlements -` empty). Witnesses: `W/wsd_client.c` (`W/build-wsd-client.sh`), `W/run-wsd-client.sh` (`mktemp -d` 0700 folder, socket 0600, 69-byte path, all under `run-with-timeout.sh`).

MEASURED:

- Socket security: `wsd` refuses a folder that is not a real 0700 folder of the same uid, refuses a path of more than 103 bytes, checks each peer with `getpeereid` (uid 501 accepted, journal `client pid … uid 501 accepted`). The refusal path for another uid was NOT exercised (no second account).
- Presentation without CPU copy on the server side: shm fd received through SCM_RIGHTS, `mmap(MAP_SHARED)`, `newBufferWithBytesNoCopy`, linear texture `newTextureWithDescriptor:offset:bytesPerRow:`, one render pass per `CADisplayLink` tick to the drawable of the `CAMetalLayer` (BGRA8, 3 drawables, `displaySyncEnabled`), `EV_RELEASE` at the end of the command buffer, `EV_PRESENTED` with `presentedTime`. The CPU client writes its pixels itself (role of the producer), the server copies none.
- Measurement display: main display `XG27AQWMG` 2560x1440, scale 1.0, `maximumFramesPerSecond` 240 (external display; the internal display was not tested).
- Pacing and latency, CPU client 640x360 BGRA8 triple buffer, 4 s per pacing (`logs/graphics-wsd-client-003.log` and `-004.log`, two passes):

| Requested pacing | Frames on screen | Interval between `presentedTime` p50 / p95 / p99 (ms) | Latency submission -> `presentedTime` p50 / p95 / p99 (ms) | Replaced (mailbox) |
| --- | --- | --- | --- | --- |
| 60 Hz, pass 003 | 239/240 | 16.669 / 16.669 / 20.836 | 20.666 / 21.011 / 24.781 | 0 |
| 60 Hz, pass 004 | 231/240 | 16.669 / 20.836 / 37.505 | 9.304 / 13.495 / 17.230 | 8 |
| 120 Hz, pass 003 | 468/480 | 8.334 / 12.502 / 16.669 | 18.587 / 18.806 / 22.788 | 12 |
| 120 Hz, pass 004 | 480/480 | 8.334 / 12.502 / 12.502 | 17.861 / 18.126 / 18.171 | 0 |

- The intervals are multiples of 4.167 ms (240 Hz): the display link requested at 60 or 120 Hz falls on a 240 Hz grid and a delay of one 240 Hz tick appears at p95. The latency depends on the phase between the client's clock and the display link (the client is not locked to vertical sync): from 9 to 21 ms at p50.
- Hidden window: in passes `-001` (CPU client) and `logs/graphics-wsd-dxmt-001.log` (DXMT), almost all `presentedTime` are 0, with `occlusion_visible=0` in the `wsd` journal: the window was covered (the application is not activated by `[NSApp activate]` when launched from a terminal, macOS cooperative activation: `app_active=0`, `key=0` at display time). Fix: style `WSD_STYLE_TOPMOST` (`NSFloatingWindowLevel`, equivalent of `HWND_TOPMOST`) and `orderFrontRegardless`; afterwards 0 or 1 frame not displayed per pass.
- Input: the synthetic scenario (`DEBUG_INJECT`, `NSEvent` events passed to `-[NSWindow sendEvent:]`, marked bit 31) delivers 4/4 keys (`a` keycode 0 with the character "a", Shift keycode 56 with modifier bit 0x2), 4/4 buttons at (10,20) px, 1 move, 1 wheel (dx 1, dy -3). Relative mode really calls `CGAssociateMouseAndMouseCursorPosition(0)` (returns 0, no permission request) then restores 1 after about 0.3 s; a synthetic raw delta (5,-7) comes back as `EV_RELATIVE`. The clip returns (99,99) with the "clamped" bit for a move outside the 100x100 rectangle. Fullscreen: a single `EV_RESIZE` 2560x1440 after 2.9 ms, window level 25, back to 640x360 after 6.5 ms. Ping: p50 0.016 ms, p99 0.608 ms.
- Real input: 24 real mouse moves (bit 31 absent) received during pass `-002` when the pointer crossed the window. No real clicks, keys or wheel were measured.
- DXMT to `wsd` (`logs/graphics-wsd-dxmt-002.log`, `W/dxmt_triangle.m` mode `wsd`): three `DMTX` v2 256x256 BGRA8 shared textures rendered by DXMT (borrowed binary) whose fds are attached once to `wsd`, rotating triangle, wait for GPU completion through an EVENT query before `PRESENT`, reuse after `EV_RELEASE`. Zero copies end to end: `wsd` samples the shm pages where DXMT rendered. 60 Hz: 239/240 displayed, interval p50 16.669 / p95 20.836 / p99 20.836 ms, latency `PRESENT` -> display p50 25.7 ms; DXMT render + GPU completion per frame p50 0.536 ms. 120 Hz: 478/480, interval p50 8.334 / p95 12.502 / p99 12.502 ms, latency p50 15.8 ms, render p50 0.204 ms. 512x512 window: x2 scaling by the sampler.

READ: `CAMetalDrawable.presentedTime` is 0 if the frame was never displayed (Apple documentation of `addPresentedHandler`), which matches the measured occlusion.

INFERRED and limits:

- The synthetic input validates the encoding and the protocol, not the hardware path (HID to `NSEvent`); real clicks, keys, wheel and raw deltas, the actual hiding of the cursor and the return of the pointer through `CGWarpMouseCursorPosition` remain to be checked by a person in front of the screen. Confinement is done in software (clamping and pointer return), without `CGEventTap`: no Accessibility permission required, but the pointer can leave for an instant before the return.
- No IME nor `interpretKeyEvents`: composed text (dead accents, AZERTY) is not handled; `EV_KEY` gives the macOS keycode and `NSEvent.characters`.
- No shared GPU fence yet: the DXMT producer waits for its own EVENT query before `PRESENT`, which serializes the game's CPU on the GPU. With Neptune, the end of write will come from the fence fd of `SUBMIT_CMD2` (G1), which today only proves the retirement of the timeline fence (see 1.6).
- The client does not lock to vertical sync; an optional `EV_VBLANK` event would reduce latency by about one period.

### G4. Design (none of this is built): Neptune presentation and Wine display driver on `wsd`

`N` = `NEP/mesa-triton/src/virtio/neptune`, `VW` = `/Users/mathieu/Documents/Github/Zagrios/voile-wine`. Everything that follows is INFERRED except the citations (READ).

#### G4.1 Neptune: replace DRI3/Present with `wsd`

READ, starting point:

- `N/nptunix/npt_unix_wsi.c` (396 lines) holds a single global state `g_wsi` (`:39-59`, so a single presented swap chain per process), connects to X (`:80`), requires DRI3 and Present (`:88-120`), opens `/dev/dri/renderD128` for the syncobjs (`:104`), creates one pixmap per image with `xcb_dri3_pixmap_from_buffer_checked` assuming `size = width*height*4` and depth 24/32 (`:141-145`), presents with `ASYNC|COPY` (`:246-247`), waits for the fence by blocking the calling thread up to 30 s in path 2 (`:313-321`) and counts the `IDLE_NOTIFY` per image (`:197-215`).
- `N/nptunix/npt_unixlib.h:64-73`: `npt_unix_wsi_init_params` carries `x11_window`, `fds[4]`, `strides[4]`, `virgl_format`, `width`, `height`, without offset or size per image; `:83-101`: `present` (index, fence fd, "consumed" flag) and `drain` (release counters per image, not a mask).
- `N/npt_renderer_wine_common.c:47-85`: `unixlib_wsi_init` reads `GetPropW(hwnd, L"__wine_x11_whole_window")` (`:64`) and fails without an X11 window.
- `N/npt_swapchain_common.c:378-409`: each image is a host blob requested by `create_host_blob` with the size rounded to 4,096 (`:398-399`), the `pitch` of plane 0 serves as `stride`; the plane offset (`exp.planes[0].offset`) is available but is not passed on. On the host side, `virgl/src/neptune/npt_shared.c:195-201` exports the fd of the `DMTX` POD and a size `offset + size`: a texture placed in a shared heap starts at `offset` in the object.
- Formats: `npt_shared_texture.c:109-120` returns 1 (BGRA8), 2 (BGRX8) or 67 (RGBA8).

Proposed changes:

1. `npt_unixlib.h:64-73`: replace `uint32_t x11_window` with `uint64_t host_window` (`wsd` window identifier) and add `uint64_t offsets[NPT_WSI_MAX_IMAGES]` and `uint64_t sizes[NPT_WSI_MAX_IMAGES]`. Both sides (PE and `.so`) are built together, the structure can change.
2. `npt_swapchain_common.c:388-409`: fill `offsets[i] = exp.planes[0].offset` and `sizes[i] = exp.allocation_size`.
3. `npt_renderer_wine_common.c:58-71`: replace the read of `__wine_x11_whole_window` with `GetPropW(hwnd, L"__wine_obelisk_window")`, a property set by driver B on each HWND that has a host window (G4.2). For a child window, go up to the top-level ancestor and present in its client rectangle (first version: top-level windows only).
4. New file `N/nptunix/npt_unix_wsi_wsd.c` that keeps the four entry points (`npt_do_wsi_init/present/drain/destroy`) and the order of the `__wine_unix_call_funcs` interface:
   - `init`: get the driver's connection in the same process through `dlopen("wineobelisk.so", RTLD_NOLOAD)` then `dlsym("obelisk_wsd_submit")` and `dlsym("obelisk_wsd_set_present_listener")`; for each image, `BUFFER_ATTACH { buffer_id = 0x80000000 | (generation << 4) | i, width, height, stride, format, offset, size }` with a `dup` of the fd. Format 2 (BGRX8) requires one more `wsd` format that forces alpha to 1 in the shader.
   - `present`: version 1, wait for the fence on the client side like the current path 2 then `PRESENT`; version 2 (preferred): `PRESENT_FENCED` which passes the fence fd to `wsd`, which encodes the frame only once this fd is readable (GCD source on the fd), without blocking the game's thread; `wait_fence_consumed = 1` in both cases.
   - `drain`: the driver's reader thread calls the listener for each `EV_RELEASE` whose `buffer_id` has bit 31; the listener increments `released_count[i]` under a mutex and wakes a condition variable; `drain` waits at most `timeout_ms`.
   - `destroy`: `BUFFER_DETACH` of each image.
   - Gone: libxcb, xcb-dri3, xcb-present, libdrm, the opening of `/dev/dri/renderD128`.
5. `N/nptunix/meson.build`: option `-Dnpt_wsi=x11|wsd` that picks the file and removes the X/DRM dependencies for `wsd`.
6. DXGI outputs: replace `npt_unix_enumerate_outputs` (`nptunix/npt_display_drm.c`) with a `wsd` `GET_OUTPUTS` request (displays, size in pixels, maximum refresh rate, scale; the current `HELLO` gives only one).
7. Fullscreen: Neptune already makes a borderless `HWND_TOPMOST` window that covers the monitor (`npt_swapchain.c:520-560, 651-684`, section 2.1); driver B translates this geometry into `WINDOW_FULLSCREEN` (measured in G3) and scaling is done in the `wsd` shader.

Points to watch: the end of GPU write (the G1 fence only measures the retirement of the timeline); the `g_wsi` singleton (a game with two swap chains, or a window change, resets everything); `wsd` requires `stride` and offset to be multiples of `minimumLinearTextureAlignmentForPixelFormat` (measured: 1,024 and 0 pass).

#### G4.2 Wine display driver "wineobelisk.drv" (option B), skeleton

READ model: `winewayland.drv` fills only 21 members of `user_driver_funcs` (`VW/dlls/winewayland.drv/waylanddrv_main.c:36-58`); the full interface is `VW/include/wine/gdi_driver.h:364-446` (`WINE_GDI_DRIVER_VERSION 108` at `:221`). Input goes through `NtUserSendHardwareInput(hwnd, 0, &input, 0)` (`wayland_pointer.c:167, 272, 319, 388`, `wayland_keyboard.c:644, 814, 840`); relative motion is a `MOUSEEVENTF_MOVE` without `ABSOLUTE` (`wayland_pointer.c:377`) and absolute moves are ignored in relative mode (`:175-176`). Capture rule (`wayland_pointer.c:883-888`): a clip rectangle or a window covering the virtual screen with a hidden cursor, or forcing, gives a pointer lock (relative mode); a clip with a visible cursor gives confinement.

Caution: the guest Wine validated in QEMU is GE-Proton11-7 (section 2.0), not voile-wine. The driver must be compiled in the exact tree of the guest Wine; the interface version of GE-Proton's `gdi_driver.h` was not read.

Structure:

- PE part `wineobelisk.drv`: like `winewayland.drv/dllmain.c:30, 109`, a thread created at load calls the unixlib `read_events` in a loop.
- Unix part `wineobelisk.so` (Linux x86-64 ELF under FEX): single connection to `wsd` (path in `OBELISK_WSD_SOCKET`), `HELLO`, `user_driver_funcs` table, reader thread that decodes the events, exports `obelisk_wsd_submit` and `obelisk_wsd_set_present_listener` for `nptunix.so`.
- `HKCU\Software\Wine\Drivers\Graphics = obelisk` (`programs/explorer/desktop.c:43, 996-1030`, section 2.3).

Entry points needed for Rocket League, and their `wsd` translation (requests marked "v2" do not exist yet in `wsd_protocol.h`):

| Member | Role for Rocket League | Translation |
| --- | --- | --- |
| `pCreateWindow`, `pDestroyWindow` | game window, launcher, EAC dialog boxes | `WINDOW_CREATE`/`DESTROY` for top-level windows; property `__wine_obelisk_window` |
| `pWindowPosChanging`, `pWindowPosChanged` | size, position, visibility, switch to fullscreen | `WINDOW_SHOW`; `WINDOW_FULLSCREEN` when the rectangle covers the monitor without caption; position and size through `WINDOW_SET_FRAME` (v2) |
| `pShowWindow`, `pSysCommand` | minimize, restore (Alt+Tab) | `WINDOW_MINIMIZE` (v2) |
| `pSetWindowStyle`, `pSetWindowText`, `pSetWindowIcons` | style, title | `SET_TITLE`; style at the next `WINDOW_SET_FRAME` |
| `pActivateWindow` + `EV_FOCUS` | `WM_ACTIVATE`, game pause when out of focus | `EV_FOCUS` to `NtUserSetForegroundWindow`; activation requested from `wsd` (`WINDOW_SHOW`) |
| `pCreateWindowSurface` | GDI rendering of the launcher and of the EAC dialogs | shm surface attached through `BUFFER_ATTACH`, `PRESENT` when the surface is flushed (same presenter as Neptune) |
| `pSetCursor` | menu cursor, hidden in game | `CURSOR_VISIBLE` (measured), ARGB image + hot spot through `CURSOR_SET_IMAGE` (v2) |
| `pGetCursorPos`, `pSetCursorPos` | camera recentering by some engines | position cached from `EV_MOTION`; `CURSOR_WARP` (v2, `CGWarpMouseCursorPosition`) |
| `pClipCursor` | mouse capture in game | `winewayland` rule above: clip + hidden cursor -> `RELATIVE_MOUSE` (measured, `CGAssociateMouseAndMouseCursorPosition`), clip + visible -> `CURSOR_CLIP` (measured, software clamping) |
| mouse input | `MouseX/MouseY` camera, raw input, `dinput8` | `EV_BUTTON`/`EV_MOTION`/`EV_WHEEL` -> absolute `MOUSEINPUT`; `EV_RELATIVE` -> relative `MOUSEEVENTF_MOVE`; wineserver derives raw input from these hardware inputs (INFERRED, to be checked with `+rawinput`) |
| keyboard input, `pKbdLayerDescriptor`, `pReleaseKbdTables`, `pMapVirtualKeyEx`, `pToUnicodeEx` | per-key bindings, chat typing | macOS keycode -> scancode/VK table ported from `winemac.drv/keyboard.c`; `KEYBDINPUT` with `KEYEVENTF_SCANCODE`; `pKbdLayerDescriptor` NULL gives `kbdus` at first (section 2.1) |
| `pUpdateDisplayDevices`, `pChangeDisplaySettings` | mode list, fullscreen resolution | monitors and modes from `GET_OUTPUTS` (v2); emulated mode change (success, Metal scaling) |
| `pWindowMessage`, `pDesktopWindowProc`, `pThreadDetach`, `pBeep` | plumbing | like `winewayland` |
| `pClipboardWindowProc`, `pUpdateClipboard` | clipboard | later (not required for a match) |
| `pVulkanInit`, `pOpenGLInit` | none (D3D11 through Neptune) | NULL, the `win32u` null driver takes over |

That is about 25 members, of the same order as `winewayland` (21).

#### G4.3 Connection of the Unix side (x86-64 ELF under FEX) to the `wsd` socket

- Path: `wsd` listens on a short private host folder (measured: `$TMPDIR/wsd.XXXXXX/wsd.sock` is 69 bytes, under the limit of 103). The Linux layer exposes this folder in the guest root (for example `/run/obelisk`) and the launcher exports `OBELISK_WSD_SOCKET=/run/obelisk/wsd.sock` for Wine. The layer's network module translates `connect(AF_UNIX, guest path)` into a host `connect` on the host path; the host path, not the guest path, must fit in 103 bytes.
- Authentication: the FEX process runs under the user's uid, so `getpeereid` on the `wsd` side sees the right uid without any translation; `LOCAL_PEERPID` gives the host pid of the FEX process.
- SCM_RIGHTS: the Linux x86-64 `cmsghdr` header is 16 bytes (`size_t cmsg_len`, alignment 8) versus 12 on Darwin (`socklen_t`, alignment 4); the layer must re-encode the control messages and translate guest fd numbers into host fds through its fd table. `wsd` v1 never sends an fd to the client, only the client -> server direction is required.
- Shared memory: the attached fds (Neptune images, GDI surfaces) are host shm objects received from vtest. The guest `mmap(MAP_SHARED)` must be a real shared mapping of the host object; HyperLinux made a private copy for file-backed `MAP_SHARED` (`RHL/graphics-notes.md:7-15`): with a copy, neither rendering nor presentation works. Host mapping offsets must be multiples of 16 KiB and `ftruncate` rounds the size to 16 KiB (measured in 1.6) while the Neptune client rounds to 4 KiB (`npt_swapchain_common.c:398-399`).
- Binary format: `wsd_protocol.h` uses only fixed-size fields, naturally aligned, with `_Static_assert` on the sizes; an identical header compiled for Linux x86-64 (LP64 like Darwin arm64, little-endian) gives the same sizes. Times are in host `CLOCK_UPTIME_RAW`: the driver must not compare them with the guest clock without conversion.

### Requests to the lead and known gaps (graphics host session)

- REQUEST: allow `xcodebuild -downloadComponent MetalToolchain` (global Xcode component, roughly one Apple download, no write to `/opt` or `/usr/local`) to finish building DXMT from source; without it, `dxmt-native` stays the UTM 5.0.5 binary. Variant without install: extract the precompiled `.air` files from the UTM binary, which would no longer be a build from source.
- Gaps: refusal of a peer of another uid not exercised; real input (clicks, keys, wheel, raw deltas, return of the clamped pointer) not measured, only 24 real mouse positions were; actual hiding of the cursor not checked; internal display (ProMotion) not measured, only the external 240 Hz display was; the vtest fence does not prove GPU completion; SHA-256 checksum of the LLVM archive not compared with a published checksum; x86_64 / D3DMetal slice not exercised.

## Paths and abbreviations

- `EXP` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-wine-fex-darwin-20261002`
- `RHL` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928`
- `NEP` = `$RHL/probes/qemu-neptune-graphics-20260930`
- `DOC` = `/Users/mathieu/Documents/Github/Zagrios/sailover/docs/research/rocket-league-eac-technologies-macos-2026-10-01.md`
- `VOILE` = `/Users/mathieu/Library/Application Support/com.zagrios.obelisk/Voile/`

## Findings from existing documents (READ, not rechecked in this session)

- `$RHL/probes/gpu-paths.md:5`: no Linux-to-Metal assembly directly usable with HyperLinux is confirmed; the vtest server of UTM's virglrenderer fork (commit `71a67414…`) has two shared-memory obstacles: `vtest_shm.c:36-57` (memfd_create that returns -1) and `virglrenderer.c:1424-1447` (refusal to export an FD for `VIRGL_RESOURCE_METAL_HEAP`).
- `$RHL/graphics-notes.md:7-15`: HyperLinux's X11 path is real (Unix sockets, SCM_RIGHTS, real SysV SHM), file-backed `MAP_SHARED` is a private copy on the HyperLinux side, and the default path of hyper-linux-x11 presents a CPU buffer through NSImage.
- `$RHL/probes/hl-graphics-feasibility-20260929/README.md:13-25`: `winemac.so` and D3DMetal installed in Voile are x86_64 Mach-O; `d3d11.so` and `dxgi.so` on the Unix side are symbolic links to `libd3dshared.dylib`.
- `$DOC:150-183`: Neptune has two transports (virtio-gpu and vtest); local presentation goes through X11 DRI3/Present in `nptunix/npt_unix_wsi.c`; UTM's `macos-next` ARM backend picks `libdxmt-native.dylib` for `__APPLE__ && __aarch64__`.

## 1. Graphics

### 1.1 Process architecture constraints of D3DMetal

MEASURED (lipo, otool, nm, file, 2026-10-02, read-only):

| File | Archs | Size (bytes) | SHA-256 | Remark |
| --- | --- | --- | --- | --- |
| `VOILE/v26.3.0-4/lib/external/D3DMetal.framework/Versions/A/D3DMetal` | x86_64 only | 7,578,032 | `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad` | GPTK "4.0 beta 2" according to `runtime.json` |
| `VOILE/v26.3.0-4/lib/external/libd3dshared.dylib` | x86_64 only | 241,888 | `1582e7ceef7f495df4bebf7f06a49aef130233f8a2e9a8971e35affafeb76ec0` | CrossOver glue: links only libc++, Foundation, CoreGraphics, libSystem, libobjc (it loads D3DMetal at runtime) |
| `VOILE/.../lib/wine/x86_64-unix/{d3d10,d3d11,d3d12,dxgi,nvapi64,nvngx-on-metalfx}.so` | symbolic links to `../../external/libd3dshared.dylib` | | | `winemac.so`, `winecoreaudio.so`, `winebus.so`, `winevulkan.so`, `ntdll.so` are also x86_64 only |
| `VOILE/.../lib/wine/x86_64-windows/{d3d11,dxgi,d3d12}.dll` | PE32+ x86-64 | | | Windows side of the CrossOver chain |
| `UTM.app/Contents/Frameworks/D3DMetal.framework/Versions/A/D3DMetal` (UTM 5.0.5, `NEP/UTM.app`) | x86_64 only | 5,263,904 | `287669456c09df4732e16a76baadc535724789a96bd4e5085f9614e6de6b1049` | older version than Voile's |
| `UTM.app/.../d3dmetal-native.framework/Versions/A/d3dmetal-native` | universal x86_64 + arm64 | 510,240 | `4ff559614c5ecd53d5ad76c1e4ad893996fad9ea175bc65506ea01deddb0467a` | x86_64 slice: exports `D3D11CreateDevice`, `CreateDXGIFactory*`, `D3D12CreateDevice` and the `dmn_*` embedder API (events, shared fences, windows, memory sharing, heap import). arm64 slice: 0 exported symbols, links only libSystem. It is an empty shell. |

The `NEP`, `VOILE` paths are defined above.

Consequences, by direct deduction from the slices above and from the comment in `NEP/virgl/server/render_worker.c:507-523`:

- The D3DMetal path requires an x86_64 host process under Rosetta 2 (user process, not a VM). The arm64 slice of `d3dmetal-native` contains nothing.
- An x86_64 render process coexists without difficulty with a Linux x86-64 Wine emulated by FEX on arm64: the two talk through a Unix socket and shared memory. The Rosetta cost applies to the renderer's code (D3D11 to Metal translation, shader compilation), not to the game.
- It must be checked that Rosetta 2 is installed on the target machine (to be measured by an `arch -x86_64` witness, see section 5, witness W0).
- D3DMetal is a closed Apple binary (framework, Metal IR converter): we can modify the `d3dmetal-native` wrapper and the transport, not D3DMetal. Redistribution depends on the GPTK license (already noted in `DOC:165`).

### 1.2 Native arm64 DXMT

MEASURED (same method):

| File | Archs | Size | SHA-256 |
| --- | --- | --- | --- |
| `UTM.app/.../dxmt-native.framework/Versions/A/dxmt-native` | universal x86_64 + arm64 | 59,093,264 | `f7444460e48a8a4c8c4401385e1cd0bee9fbeceb76534cde4cda542fead76dfd` |
| `UTM.app/.../virglrenderer.1.framework/Versions/A/virglrenderer.1` | universal | 8,933,984 | `cdbe4531e7c4c4befb4316ed5a8e3a33dd27383d2f8e94c4a6a4cad4192c81c3` |
| `UTM.app/.../MoltenVK.framework/Versions/A/MoltenVK` | universal | 11,029,792 | `b96a76813884c04ebe7f13ddf4c4b305234224117a0688a56b81a398fc1641dd` |
| `UTM.app/.../vulkan_kosmickrisp.framework/Versions/A/vulkan_kosmickrisp` | universal | 23,902,384 | `3e438cca84029b162d273d98ac40ce5052ae710a2ac1e9aaae2b40b2c7ca99f6` |
| `UTM.app/.../epoxy.0.framework`, `EGL.framework`, `GLESv2.framework` (ANGLE) | universal | | |
| `NEP/render-server-real` (= `render-server-private`, same SHA-256 `d239f5926a9f0d89ae3cadcf60e3ae6610eb0dbcbe78f48e49f2240fe5d5cb39`) | universal x86_64 + arm64 | 7,947,824 | see opposite |

- `dxmt-native` arm64 links CoreFoundation, Metal, Foundation, CoreGraphics, ColorSync, MetalFX, libsqlite3, libSystem, libc++, AppKit, libobjc. It depends neither on D3DMetal nor on Rosetta. Measured exports (`nm -gU`): `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2`, `D3D10CoreCreateDevice`, `D3D10CoreRegisterLayers`, `D3D11CoreCreateDevice`, `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`, `D3D11On12CreateDevice`, `DXGIGetDebugInterface1`, `dxmt_event_{clear,close,create,dup_fd,signal,wait}`. It does not export `D3D12CreateDevice`: the D3D12 capability bit is therefore false with DXMT (`NEP/virgl/src/neptune/npt_renderer.c:41-66`).
- The universal render server picks the slice through `posix_spawnattr_setarchpref_np`: x86_64 for d3dmetal, arm64 for dxmt (`NEP/virgl/server/render_worker.c:507-523`), variable `NPT_BACKEND=dxmt`.
- Local binaries usable for a first witness without compiling anything: the arm64 slice of `render-server-real` and that of `dxmt-native`. The DXMT and d3dmetal-native sources are NOT in the local tree (section 1.5).
- DXMT `capset` (`npt_renderer.c:79-91`): TBDR, MSAA_RTV_FORCED_SC1, MAP_DEFAULT_BUFFERS, SHADER_CACHE; not EXTENDED_RESOURCE_SHARING (DXMT fails every `SHARED_NTHANDLE`). D3DMetal: DXIL bit only.

### 1.3 Neptune client chain (PE DLL, Unix library, vtest versus virtio-gpu)

READ in `NEP/mesa-triton/src/virtio/neptune/` (osy/mesa tree, neptune branch, `project mesa 26.1.0-devel` according to `NEP/guest-meson-setup.log`):

1. PE DLLs `d3d11.dll`, `dxgi.dll`, `d3d12.dll` (meson.build:122-162), compiled with MinGW 10-posix in an amd64 `ubuntu:22.04` container (`NEP/Dockerfile.guest-build`, meson 1.9.2). They replace Wine's native DLLs (`WINEDLLOVERRIDES='d3d11,dxgi=n;nptunix=b;...'`, `NEP/guest/guest-graphics.sh`). Local sizes: 20,629,950 bytes for `d3d11.dll` and 20,591,698 for `dxgi.dll` in `NEP/client-original-44d3259/` (with debug symbols). The DLLs serialize D3D11/DXGI through rings (`npt_ring.c`) and contain no rendering.
2. `nptunix.dll` (PE carrier, "Wine builtin DLL" marker patched at offset 0x40, `nptunix/meson.build:119-137`) and `nptunix.so` (native Linux x86-64 ELF, loaded by Wine's unixlib mechanism). The `__wine_unix_call_funcs` interface has 27 entries (`nptunix/npt_unix.c:217-245`, enumeration `npt_unixlib.h:222-251`): connect, read, write, close, receive_fd (recvmsg + SCM_RIGHTS), mmap, munmap, wait_fd, 4 WSI calls, 9 virtgpu (DRM) calls, enumerate_outputs, dup, 3 singleton calls, alloc_obj_id. EVERYTHING that touches the Linux kernel goes through this file, which makes it the adaptation seam.
3. Transport choice (`npt_device.c:279-292`): virtgpu (`npt_renderer_virtgpu_wine.c` plus `nptunix/npt_unix_virtgpu.c`, DRM ioctls on `/dev/dri/renderD*`) unless `NPT_DEBUG=force_vtest`, otherwise vtest (`npt_renderer_vtest_wine.c`, 297 lines). Outside a VM, virtgpu fails at open and vtest takes over; vtest can be forced.
4. vtest transport (`npt_renderer_vtest_wine.c:221-296`, `npt_renderer_vtest_protocol.c`): `AF_UNIX SOCK_STREAM` socket to `$VTEST_SOCKET_NAME` (default `/tmp/.virgl_test`, `NEP/virgl/vtest/vtest_protocol.h:28`; `sun_path` is limited to 104 bytes on Darwin), sequence `CREATE_RENDERER("neptune")`, `PING_PROTOCOL_VERSION`, protocol version >= 3 required, `GET_PARAM(MAX_TIMELINE_COUNT)`, `GET_CAPSET(NPT_CAPSET_ID)` with a check of `wire_format_version == NPT_PROTOCOL_WIRE_VERSION`, `CONTEXT_INIT`. Then: `RESOURCE_CREATE_BLOB` (type `HOST3D_GUEST` mappable for the SHMs, `HOST3D` mappable plus shareable for the host blobs), fd received by `recvmsg` SCM_RIGHTS then `mmap(MAP_SHARED, offset 0)`; `SUBMIT_CMD2` with `RING_IDX` for the commands; presentation fence through an empty `SUBMIT_CMD2` with `OUT_FENCE_FD` whose fd comes back through SCM_RIGHTS.
5. Presentation (WSI), the only point tied to X11: `npt_renderer_wine_common.c:47-85` reads the Wine window property `__wine_x11_whole_window` of the HWND and fails without it; `nptunix/npt_unix_wsi.c` creates DRI3 pixmaps from the dma-buf fds of the host blobs (`xcb_dri3_pixmap_from_buffer_checked`, :141-145), presents through `xcb_present_pixmap[_synced]` with options `ASYNC|COPY` (:246-247), reads `IDLE_NOTIFY` to release the images (:197-215) and waits for the fences through DRM syncobj (`/dev/dri/renderD128`, :104) or through `poll` 30 s (:313-321). The number of images is limited to `NPT_WSI_MAX_IMAGES = 4` (`npt_unixlib.h:17`).
6. Memory: the swap chain images are blobs created by the host (`create_host_blob`); the client copies no pixel to the display server. Size rounded to 4096 bytes on the client side (`npt_swapchain_common.c:396-399`).

INFERENCE: on macOS the only file to replace to change the presentation plane is `nptunix/npt_unix_wsi.c` (396 lines, 4 functions `wsi_init/present/drain/destroy`) plus the read of `__wine_x11_whole_window` in `npt_renderer_wine_common.c`. The rest (rings, blobs, fences, queries) is independent of X11. See sections 1.6 and 2.

### 1.4 Host renderer (neptune code of virglrenderer, d3dmetal-native, dxmt-native)

READ in `NEP/virgl` (utmapp/virglrenderer, commit `482f9d8b8c2d2288efa10a027116216909d2c226` of 2026-09-01, `NEP/virgl-pin-commit.json`; this is the `VIRGLRENDERER_COMMIT` pin in UTM 5.0.5's `NEP/build_dependencies.sh`). The upstream branch `macos-next` is more recent (head `5d26f605f50f8e22002ec6db5fb775e1992d4e96`, 2026-09-23, according to the GitHub commits page consulted; it adds at least `0316f58` "darwin shared-texture POD version 2", `6b52599` "pipe fallback for the fence eventfd (async fencing on macOS)", `8dfa5b5` out-of-band SUBMIT_FENCE channel, `a2c0dba` placement of ring threads).

Processes and roles:

1. `virgl_test_server` (`NEP/virgl/vtest/vtest_server.c`, 5,021 lines with vtest, `vtest_renderer.c` alone: 3,073 lines) accepts vtest connections. With `ENABLE_NEPTUNE` it forces `VIRGL_RENDERER_NEPTUNE | VIRGL_RENDERER_RENDER_SERVER` (`vtest_server.c:315-318`).
2. `virgl_render_server` (`NEP/virgl/server/`, 3,265 lines, plus `src/proxy/` 2,049 lines) creates one worker per context. On Darwin the worker is launched by `posix_spawn` (no `fork`, because of stale XPC connections that break Metal shader compilation, `server/render_worker.c:467-473`), with slice preference (arm64 for `NPT_BACKEND=dxmt`, x86_64 otherwise, `render_worker.c:509-524`).
3. The worker contains the host Neptune code (`NEP/virgl/src/neptune/npt_*.c`, 10,793 hand-written lines; the protocol decoder is generated, about 173 k lines of headers). It loads the backend through `dlopen`: `libdxmt-native.dylib` on `__APPLE__ && __aarch64__`, `libd3dmetal-native.dylib` on Apple x86 (`npt_library_names.h:25-32`), finds the embedder API by the prefix `dmn_` or `dxmt_` (`npt_library.c:61-115`), and variables `NPT_D3D11_LIBRARY_PATH`, `NPT_DXGI_LIBRARY_PATH`, `NPT_D3D12_LIBRARY_PATH`, `NPT_BACKEND`, `NPT_WA_FLAGS`, `NPT_CAPSET_D3D12`, `D3DMETAL_FRAMEWORK_PATH` (the scripts `NEP/run-graphics.py:26-29` and `NEP/start-utm-private.py:8` set them).
4. The Neptune context (`npt_context.c`) offers: a ring (`npt_ring.c`) for serialized commands; SHM resources created on the host side by `os_create_anonymous_file(size, "npt-shmem")` (`npt_context.c:1011`; on Apple this is `shm_open(O_CREAT|O_EXCL)` with a name `/<tag>-<pid>-<nonce>-<i>` immediately `shm_unlink`ed, then `ftruncate`, `NEP/virgl/src/mesa/util/anon_file.c:126-212`); "host" blobs from shared textures (`npt_shared.c`, POD descriptor `DMTX` version 2: `fd`, width, height, DXGI format, `stride`, `size`, `offset`); Win32 events emulated by `dmn_event_*` / `dxmt_event_*` (`npt_event.c:25-40, 117-130`) whose `*_event_dup_fd` gives a pollable fd; synchronization queues (`npt_queue.c`) that retire the fences when the GPU has finished; multiple rings with threads of different priority (`npt_ring.c`).
5. Shared textures on macOS: according to the UTM article of 2026-07-24 (read online, not rechecked in the code), a shared texture is a `shm_open` fd mapped as an `MTLBuffer` by `newBufferWithBytesNoCopy:` (Apple Silicon unified memory, linear textures only) and the shared fence is a timeline value written in shared memory by `ClearUnorderedAccessViewUint`, polled by the consumer. The README of `utmapp/dxmt` says: "Win32-style events and cross-process shared textures over POSIX shm". Locally, the structure `npt_darwin_shared_texture` (`npt_shared.c:76-90`) and `dxmt_shared_texture_handle` (retrieved in `dxmt_native.h`) have the same ABI (`'DMTX'` = `0x58544D44`, version 2).

Essential consequence for presentation: for each swap chain image the client receives a linear (mappable) shared-memory fd and not a GPU dma-buf. The client can therefore READ the pixels (copy) or pass the fd to a macOS presenter, which maps it as an MTLBuffer then MTLTexture and copies it onto a `CAMetalLayer`, without CPU copy.

What the Neptune host does NOT do: it creates no window. No `dmn_window_*` or `CAMetalLayer` call in `npt_*.c` (text search); `dxmt_native.h` (commit `70aa109c`) exports only events and the shared texture POD, no window. In the QEMU setup, the window is QEMU/UTM's (virtio-gpu scanout), fed by the guest Xorg server. There is therefore no reusable macOS presentation code in these sources today; `dmn_window_create_for_view` and `dmn_window_get_metal_layer` exist in `d3dmetal-native` (MEASURED: exported symbols of the x86_64 slice) but Neptune does not call them.

The following sections of the UTM ecosystem describe useful engineering choices: `NPT_WA_FLAGS` enables DXBC converter workarounds for D3DMetal only (`npt_library.c:240-250`), the capset announces DXIL for D3DMetal and TBDR, forced MSAA, mappable default buffers and shader cache for DXMT (`npt_renderer.c:79-95`).

### 1.5 What exists locally as source versus binary, and how it was built

| Item | Local source | Local binary | Build |
| --- | --- | --- | --- |
| Neptune client (`d3d11/dxgi/d3d12.dll`, `nptunix`) | YES: `NEP/mesa-triton/src/virtio/neptune/` (client 23,495 lines, `nptunix` 1,414 lines, Windows Triton UMD 19,233 lines useless for Wine, generated protocol 226,994 lines) | YES: `NEP/guest/client/d3d11.dll` (20,639,036 B), `dxgi.dll` (20,600,784 B), `libwinpthread-1.dll` (597,415 B), `NEP/guest/wine/x86_64-windows/nptunix.dll` (PE) and `x86_64-unix/nptunix.so` (x86-64 ELF with symbols) | MinGW 10-posix in amd64 `ubuntu:22.04` (`NEP/Dockerfile.guest-build`), meson 1.9.2, `NEP/build-win64.txt`; builddirs `NEP/build-win64`, `NEP/build-triton` (without `bin/`) |
| Host virglrenderer + Neptune + vtest + render server | YES: `NEP/virgl/` (commit `482f9d8b`); archive `NEP/virgl-source.tar.gz` | YES: `NEP/render-server-real` (= `render-server-private`, universal x86_64+arm64, 7,947,824 B, built by UTM's CI: strings `/Users/runner/actions/runner-{1,4}/_work/UTM/UTM/sysroot-macOS-{arm64,x86_64}/libexec/virgl_render_server`) and `UTM.app/Contents/Frameworks/virglrenderer.1.framework` (universal) | UTM recipe: `NEP/build_dependencies.sh:786` (`-Dtests=false -Dcheck-gl-errors=false -Dvenus=true -Dneptune=true -Dvulkan-dload=false -Drender-server-mode=process`) |
| `virgl_test_server` (vtest) for macOS | source YES (`NEP/virgl/vtest/`) | NO (no vtest executable found; UTM does not ship it) | not built locally |
| D3DMetal | NO (closed) | YES: Voile (x86_64, 7,578,032 B) and private copy in `NEP/UTM.app` (x86_64, 5,263,904 B) | Apple GPTK |
| `d3dmetal-native` | NO (only the README is copied: `hl-graphics-feasibility-20260929/d3dmetal-native-readme.md`) | YES: `NEP/UTM.app/.../d3dmetal-native.framework` (useful x86_64 slice, empty arm64 slice) | `meson_build $D3DMETAL_REPO -Dtests=disabled`, x86_64 only (`build_dependencies.sh:900-905`), otherwise empty dylib |
| Native DXMT | NO (UTM 5.0.5 pin: `bcdcf6da89af6c9fedeb2fc92b4367b23d5d6ed6`; fork `utmapp/dxmt` `70aa109c…` of 2026-09-02) | YES: `NEP/UTM.app/.../dxmt-native.framework` (universal, 59,093,264 B) | minimal LLVM 15 (`cmake -DLLVM_TARGETS_TO_BUILD=""`) then `meson_darwin_build $DXMT_REPO -Dnative_llvm_path="$PREFIX"`, SDK min 13.0 (`build_dependencies.sh:876-899`) |
| MoltenVK, KosmicKrisp, ANGLE, libepoxy, Vulkan loader | NO | YES (universal frameworks in `NEP/UTM.app`) | `build_dependencies.sh` (useless for D3D11-only rendering if epoxy is made optional, see 1.6) |
| Owned D3D11 test tools | YES: `NEP/d3d11-control.c`, `d3d11-present-control.c`, `d3d11-query-control.c`, `d3d11-view-resource-control.c`, `drm-caps-control.c`, `view-control-variants/*.c` | YES: `NEP/guest/client/d3d11-*.exe` (PE) | MinGW |
| Neptune protocol specification | generated in both trees (identical copies checked by `NEP/pinned-protocol-comparison.json`, 5 files) | | upstream `osy/neptune-protocol` (Apache-2.0, 25 commits according to the GitHub page; Python/Mako generator, doc `docs/neptune_command_serialization.txt`) |

Provenance notes: the local client is commit `44d3259f888b65369fafb33ed68f7a5ec62966b0` of 2026-08-31 of `osy/mesa` (neptune branch), `NEP/selected-guest-revision.txt` and `NEP/mesa-sep1-commit.json`; UTM 5.0.5 pins `MESA_COMMIT=79bc850d884a1307356ff61c017e58901b90c7e2` (`NEP/dependency-pins.txt`). The two commits differ: the difference between the client actually used in the validated session (`44d3259` plus the GetData fix, see `NEP/client-original-44d3259/sha256.json` and `NEP/view-resource-fix-evidence.json`) and the UTM pin was not measured here.

### 1.6 Exact steps to build a standalone macOS renderer process that accepts a vtest connection

MEASURED: a throwaway spike was done in the session scratchpad (nothing was written in `EXP`, nor in the repositories, nor in the installed runtime; neither UTM, nor QEMU, nor the game, nor EAC launched; only `dlopen` of the arm64 `dxmt-native` copied in `NEP/UTM.app`). Tools: Xcode 27.0 (27A266a), Apple clang 21.0.0 (clang-2100.3.34.2), SDK MacOSX27.0, meson 1.12.1, ninja 1.13.2, pkg-config 3.0.7, PyYAML 6.0.3 installed in `EXP/tools/venv`. The sources were a copy of `NEP/virgl` (commit `482f9d8b`).

Results, in order:

1. The vtest server, the render server and `libvirglrenderer` compile on macOS 27 arm64 with `-Dneptune=true -Dvtest=true` in about 6 s (97 units), without EGL, without GLX, without Vulkan, without ANGLE; only `libepoxy` is required by `meson.build:88` and builds in a few seconds (libepoxy 1.5.10, archive SHA-256 `a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15`, `-Dglx=no -Degl=no -Dx11=false`). It is therefore not the ANGLE/WebKit/MoltenVK chain of `build_dependencies.sh` that is needed for a Neptune-only host.
2. The UNMODIFIED server accepts the connection but is unusable by the Neptune client: `vtest_shm_check()` fails (`memfd_create` absent, `vtest_shm.c:33-63`), the negotiation replies "protocol version 0" (`vtest_renderer.c:884-926`) while the client requires >= 3 (`npt_renderer_vtest_wine.c:249-254`), and a `HOST3D_GUEST` blob fails with `vtest_new_shm called memfd_create which failed` then `VTEST_CLIENT_ERROR_COMMAND_DISPATCH`.
3. Two bounded patches (appendix A, 41 and 70 diff lines) are enough for the full handshake: `vtest_shm.c` reuses `os_create_anonymous_file` (POSIX shm) under `__APPLE__`; `vtest_renderer.c` announces `MAX_TIMELINE_COUNT = 64` and replaces the "already signaled" fence fd (`eventfd`, absent) with a `pipe` whose write end is written when the context's fence retires.
4. With these patches, a native macOS client of 150 lines (scratch, not shipped) got: protocol 4, `MAX_TIMELINE_COUNT=64`, Neptune capset `wire_format_version=1`, `caps_flags=0x36` (= TBDR, MSAA_RTV_FORCED_SC1, MAP_DEFAULT_BUFFERS, SHADER_CACHE, identical to the computation of `npt_renderer.c:79-91` for DXMT), creation of the Neptune context in an arm64 worker that loaded `dxmt-native` (log: `npt: created Neptune context 1`), 64 KiB `HOST3D_GUEST` blob received through SCM_RIGHTS then `mmap(MAP_SHARED)` and write succeeded.
5. Measured latencies (native processes, no emulation): 200 empty `SUBMIT_CMD2` requests with `RING_IDX|OUT_FENCE_FD`: 200/200 pollable fds became readable, minimum 0.017 ms, median 0.027 ms, p95 0.039 ms, maximum 0.079 ms; blob creation + SCM_RIGHTS + `mmap` + write + `munmap` + `RESOURCE_UNREF`: median 0.034 ms (64 KiB), 0.031 ms (1 MiB), 0.049 ms (16 MiB).
6. NOT measured: D3D11 device creation through the ring stream, rendering, x86_64 slice and D3DMetal, presentation, Wine client, FEX.

macOS traps measured by a small C program (scratch) and important for FEX's memory layer (16 KiB host pages, `hw.pagesize=16384`):

| Operation | Measured result |
| --- | --- |
| `shm_open` with a 40-character name | `ENAMETOOLONG` (limit 31, `PSHMNAMLEN`) |
| `ftruncate(fd, 12288)` on a shm object | accepted, `st_size` becomes 16384 |
| second `ftruncate` on the same object | `EINVAL` (single resize) |
| `mmap(len=12288, off=0)` | accepted, address aligned on 16 KiB, write at byte 12287 correct |
| `mmap(len=8192, off=4096)` | `EINVAL` (a mapping offset must be a multiple of 16 KiB) |
| SysV SHM: `kern.sysv.shmmax=4194304`, `shmall=1024`, `shmseg=8`, `shmmni=32` | maximum segment 4 MiB, total 1024 pages: a 1080p framebuffer (8.3 MB) does not fit |

Steps to follow to reproduce and harden (the "run" part was done; the rest is the recipe):

```sh
# 0. Prerequisites (already present): Xcode 27, meson, ninja, pkg-config. Python: PyYAML in the private venv.
EXP=/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-wine-fex-darwin-20261002
PFX=<private prefix>            # never /opt, never brew install

# 1. libepoxy without GL platforms
meson setup build libepoxy-1.5.10 --prefix=$PFX -Dtests=false -Dglx=no -Degl=no -Dx11=false --buildtype=release
ninja -C build && meson install -C build

# 2. pinned virglrenderer (copy of NEP/virgl; do not modify the original) then appendix A
PATH=$EXP/tools/venv/bin:$PATH meson setup build-arm64 virgl --prefix=$PFX \
  --pkg-config-path=$PFX/lib/pkgconfig -Dneptune=true -Dvenus=false -Dvtest=true \
  -Drender-server-mode=process -Drender-server-worker=process -Dtests=false \
  -Dcheck-gl-errors=false --buildtype=release
ninja -C build-arm64            # produces vtest/virgl_test_server, server/virgl_render_server, src/libvirglrenderer.1.dylib

# 3. Run (socket in a private 0700 folder, path < 104 bytes)
RENDER_SERVER_EXEC_PATH=$PWD/build-arm64/server/virgl_render_server NPT_BACKEND=dxmt \
NPT_D3D11_LIBRARY_PATH=<libdxmt-native> NPT_DXGI_LIBRARY_PATH=<libdxmt-native> NPT_D3D12_LIBRARY_PATH=<libdxmt-native> \
  build-arm64/vtest/virgl_test_server --no-virgl --no-fork --socket-path $TMPDIR/gda/s

# 4. D3DMetal slice (NOT run here; recipe from the UTM article of 2026-07-24 and from build_dependencies.sh:900-905)
#    cross-file clang -arch x86_64; static x86_64 epoxy; -Dvenus=false -Dneptune=true -Dvtest=false -Ddefault_library=static;
#    lipo -create server arm64 + x86_64 -> universal RENDER_SERVER_EXEC_PATH; NPT_BACKEND=d3dmetal;
#    D3DMETAL_FRAMEWORK_PATH=<D3DMetal.framework>; libd3dmetal-native.dylib x86_64 (meson, -Dtests=disabled).
```

For a "standalone" host process of the product, the spike shows that we can start from the patched `virgl_test_server`. What still has to be decided or written (INFERENCE, estimates in section 2.5):

- Replace `virgl_test_server` with a platform server that keeps the 9 vtest commands used by the client (CREATE_RENDERER, PING_PROTOCOL_VERSION, PROTOCOL_VERSION, GET_PARAM, GET_CAPSET, CONTEXT_INIT, RESOURCE_CREATE_BLOB, RESOURCE_UNREF, SUBMIT_CMD2) and add the presentation channel to it. The current vtest server is 5,021 lines, a large part of which is GL/virgl that is useless here.
- Authenticate the peer on the socket (`LOCAL_PEERCRED` / `getpeereid`), one thread per connection and not `fork` (inherited XPC connections break Metal, `render_worker.c:467-473`), lifecycle: stop at the end of the client, cleanup of the shm objects.
- Fences: the vtest server does not give a real GPU fence without a patch (the original path returns an already signaled fd when the context does not share a fence, `vtest_renderer.c:2378-2392`); with the appendix A patch the fd becomes readable when the timeline fence retires, which reflects the worker's progress but NOT yet the GPU completion of the writes into the shared texture. For tear-free presentation, the end of GPU write must be established by the paired D3D11 event (`npt_queue.c`, `npt_event.c`) or by a fence shared in memory (UTM article): to be qualified by witness W5 (section 5).
- Signing: ad hoc signed arm64 binaries are enough for the spike; the x86_64 D3DMetal worker runs under Rosetta without any particular entitlement according to the QEMU session of 2026-09-30 (INFERENCE, not rechecked here).

## 2. Presentation, display and input for "Linux x86-64 Wine under FEX on macOS"

### 2.0 What the validated QEMU guest used (Xorg, openbox, cursor)

READ and MEASURED in `NEP/guest/guest-graphics.sh`, `NEP/guest/xorg.conf`, `NEP/guest/openbox.xml`, `NEP/guest/guest-game.sh`, `NEP/Obelisk-EAC-Graphics.utm/config.plist` and in the non-private outputs of `NEP/game-query-fix-20260930T211549Z/` (the `*.private.log` files were not opened):

| Item | Value | Source |
| --- | --- | --- |
| Guest kernel and distribution | Debian 13, kernel `6.12.107+deb13-amd64`, evdev/usbhid/xhci/psmouse input modules prepared | `NEP/input-modules.json`, `NEP/prepare-input-modules.py` |
| X server | Xorg 21.1.16 (`xserver-xorg-core 2:21.1.16-1.3+deb13u4`), 1280x720 screen depth 24, `AutoAddDevices false`, launched as root on `:89` with `-auth` cookie | `guest-graphics.sh:51-53`, `xorg.conf` |
| Driver | `modesetting` on `/dev/dri/card0` (virtio-gpu), `AccelMethod glamor`, `DRI 3`, journal: "glamor X acceleration enabled on virgl" | `xorg.conf`, `outputs/xorg.log` |
| Cursor | `SWcursor false` (hardware cursor, maximum size 64x64); the previous default drew a white rectangle around the pointer, fixed and validated by an owned witness of 120 frames | `NEP/validation.json` ("cursor_fix"), `NEP/cursor-hardware-control-20260930T211146Z/result.json` |
| Input | explicit `evdev`: keyboard `AT Translated Set 2 keyboard` bound to `/dev/input/obelisk-keyboard` (XkbLayout `us`), pointer `QEMU QEMU USB Tablet` (absolute) bound to `/dev/input/obelisk-pointer`; a relative PS/2 mouse is also present | `xorg.conf`, `outputs/kernel-input-devices.txt`, `guest-graphics.sh:12-25` |
| Window manager | openbox 3.6.1-12 (`libobt2v5_3.6.1-12`), `focusNew yes`, `followMouse yes`, `focusDelay 0`, `raiseOnFocus no`, `decor no` for all windows; menu absent ("Unable to find a valid menu file"). The script notes "A real X11 window manager supplies focus and WM_TAKE_FOCUS delivery"; after launch, the focused window is `0x1200001` (`revert 2`) | `openbox.xml`, `guest-graphics.sh:82-92`, `outputs/x11-focus.json`, `outputs/openbox.log` |
| Mesa | 25.0.7 (`libgl1-mesa-dri`, `libegl-mesa0`, `mesa-libgallium`), only for glamor; the game's rendering does not go through Mesa | `NEP/xorg-packages/` |
| Fallback without GPU | `Xvfb :89 -screen 0 640x480x24` | `guest-graphics.sh:58` |
| Wine | GE-Proton11-7 x86_64 (`RHL/ge-proton/GE-Proton11-7-x86_64`); `WINEDLLOVERRIDES='d3d11,dxgi=n;nptunix=b;...'`; graphics checks with `WINEESYNC=0 WINEFSYNC=0 WINE_DISABLE_FAST_SYNC=1`, game with `WINEFSYNC=1` | `guest-graphics.sh:33-34`, `guest-game.sh` |
| Game window mode | `-windowed -ResX=800 -ResY=600`: neither fullscreen nor exclusive mouse capture were validated in the 91.54 s session | `guest-launch.py` (arguments added at launch) |
| Audio | no device (`Sound = []` in `config.plist`, no audio keyword in the guest scripts); yet the game's module table shows `winealsa.so`, `libasound.so.2.0.0`, `libpulse.so.0.24.3`, `mmdevapi.dll`, `xaudio2_9.dll`, `dsound.dll`, `winmm.dll` | `outputs/game-process-maps.txt` |
| Gamepad | no gamepad present; the game still loaded `xinput1_3.dll` (Wine), `dinput8.dll`, `hid.dll`, `setupapi.dll` | `outputs/game-process-maps.txt` |
| X libraries loaded by the game process | libX11, libXi, libXrandr, libXrender, libXcursor, libXfixes, libXcomposite, libXinerama, libXxf86vm, libXext, libxcb-dri3, libxcb-present | `outputs/game-process-maps.txt` |

Overall result of this guest: public matchmaking, protected session of 91.54 s, voluntary exit, declared limits "missing glyphs in the interface, hitches under QEMU TCG" (`NEP/game-query-fix-20260930T211549Z/validation.json`). The display path was therefore: D3D11 game -> Neptune client -> Metal host -> shared texture -> DRI3 pixmap in guest Xorg -> Xorg/openbox composition -> virtio-gpu scanout -> QEMU window. The only part of this chain that also exists without a VM is the beginning (up to the shared texture).

### 2.1 Rocket League needs (fullscreen, mouse capture, raw input, gamepad)

MEASURED (read-only install, file names and default configuration lines):

- `Binaries/Win64/` contains `XAudio2_7.dll`, `X3DAudio1_7.dll`, `XAPOFX1_5.dll`, `xaudio2_9redist.dll`, `xinput1_3.dll`, `d3dcompiler_47.dll`, `discord_game_sdk.dll`, `EOSSDK-Win64-Shipping.dll` (`ls` listing, no file opened).
- `TAGame/Config/DefaultSystemSettings.ini`: `Fullscreen=False` by default; `DefaultInput.ini`: keyboard/mouse bindings with the axes `MouseX`/`MouseY` (camera, free view), `MouseScrollUp/Down`, mouse buttons, `bEnableMouseSmoothing=false`, and gamepads of types `IPT_360`, `IPT_PS4`, `IPT_XBOX_ONE` with dead zones.
- In the QEMU session, the game process loaded `dinput8.dll`, `xinput1_3.dll`, `hid.dll`, `setupapi.dll`, `dsound.dll`, `xaudio2_9.dll`, `mmdevapi.dll` (see 2.0).

INFERENCE (to be confirmed by a Wine journal `+rawinput,+cursor,+keyboard,+dinput,+xinput` during a game trial):

| Need | Why | Design consequence |
| --- | --- | --- |
| Resizable windowed window, 800x600 up to native resolution | game default, validated at 800x600 | a window host that supports creation, move, resize, Z order, focus, title |
| Fullscreen | game option, on the Neptune side it is a borderless `HWND_TOPMOST` window covering the monitor rectangle (`npt_swapchain.c:520-560, 651-684`), without hardware mode change | no mode change needed for a first fullscreen; the presenter must be able to cover the screen (window level above the menu bar, Dock hidden) and scale |
| DXGI mode list | `IDXGIOutput::GetDisplayModeList` is built on the client side from DRM, otherwise a synthetic 1920x1080 output (`npt_overrides_dxgi_adapter.c:66-98`, `nptunix/npt_display_drm.c`) | replace `npt_unix_enumerate_outputs` with a call to the host that returns the real macOS modes (or a chosen list) |
| Relative mouse and capture | `MouseX/MouseY` axes for the camera; `dinput8` loaded; typically `ClipCursor` and recentering through `SetCursorPos`, or `RegisterRawInputDevices` | the host must provide unaccelerated deltas (for example `GCMouse` or `CGEventGetIntegerValueField(kCGMouseEventDeltaX)` with `CGAssociateMouseAndMouseCursorPosition(false)`) and handle the known cursor-jump trap (`winemac.drv/cocoa_cursorclipping.m:31-60, 301-370` uses a `CGEventTap`, which requires the macOS Accessibility permission, or `-[NSWindow setMouseConfinementRect:]` as a variant) |
| Keyboard input by scancode | UE3 binds keys; AZERTY/QWERTZ common | the host sends physical scancodes; the Windows layout comes from win32u's `KBDTABLES` tables (NULL return of `pKbdLayerDescriptor` gives `kbdus`, `input.c:1100`) or from a table built from the macOS layout (section 4) |
| XInput and PS4 gamepads | `xinput1_3`, `hid`, `setupapi` loaded; types IPT_PS4/360/ONE | HID bridge to `winebus.sys` (section 4) |
| XAudio2 audio | `xaudio2_9`, `mmdevapi` loaded | remote or null mmdevapi audio driver (section 3); absent from the QEMU validation |
| Cursor | system and custom cursor, hidden in game | `pSetCursor` / `pClipCursor` / `pSetCursorPos` / `pGetCursorPos` forwarded to the host |
| Focus and activation | `WM_ACTIVATE`, `WM_TAKE_FOCUS` on the X11 side; the QEMU script insists on a real window manager | `pActivateWindow` and focus events from the host |

### 2.2 Option A: X11 server on macOS

Principle: Linux x86-64 Wine under FEX keeps `winex11.drv`; the X socket is that of a native macOS server. This is the only option where the Wine side stays that of the validated QEMU setup (same `winex11.so`, same X libraries, same openbox if we keep it). Only `nptunix/npt_unix_wsi.c` has to change, because DRI3 does not exist on macOS.

Candidate servers, with what is established:

| Variant | Established | Limit |
| --- | --- | --- |
| A1 XQuartz 2.8.6 visible | official notarized package, SHA-256 `9ac35a505095bfbd3009c3b4772f0c6421e2f79c4210ab908459270d1c447909`, extracted without installer (`RHL/x11/README.md:5-12`) | MEASURED: crash at GLX initialization (`CGLQueryRendererInfo` in `OpenGL.framework`, `RHL/x11/XQuartz-visible-crash-summary.json`); with `-extension GLX` the process got past initialization but reported keyboard and `pbproxy` connection problems, then was stopped; no visible client validated |
| A2 Xvfb (same distribution, X.Org 21.1.23) | MEASURED: relocated private server, native and Linux `xdpyinfo` under HyperLinux OK, lavapipe `vkcube` captured (`RHL/x11/cube-shared-io-1.png`); 21 extensions (list below) | offscreen screen: a macOS viewer is needed that reads the framebuffer and injects input through XTEST; no DRI3, no GLX |
| A3 custom-written X server | none | `winex11.drv` calls 174 distinct Xlib/extension entry points (MEASURED by `grep` on `dlls/winex11.drv/*.c`, over-approximation); a reliable server covers all of them plus XKB, XI2, RANDR, RENDER, SHAPE, XFIXES, COMPOSITE, MIT-SHM, EWMH. Not recommended. |
| A4 X server with DRI3/Present on shm | none | variant of A2 or A1: add DRI3 (`PixmapFromBuffer` backed by the linear shm) and leave `nptunix.so` unchanged; Xorg's generic DRI3/Present code exists, the DDX must provide the callbacks |

X extensions: MEASURED on XQuartz 2.8.6's Xvfb (`RHL/x11/xdpyinfo-private-89.txt`): BIG-REQUESTS, Composite, DAMAGE, DOUBLE-BUFFER, Generic Event Extension, MIT-SCREEN-SAVER, MIT-SHM, Present, RANDR, RECORD, RENDER, SHAPE, SYNC, X-Resource, XC-MISC, XFIXES, XINERAMA, XInputExtension, XKEYBOARD, XTEST, XVideo. Absent: GLX, DRI3, XFree86-VidModeExtension, DPMS. What `winex11.drv` (Wine 11.0) does with them, READ:

| Extension | Use in `winex11.drv` | If absent |
| --- | --- | --- |
| core + BIG-REQUESTS | everything | unusable |
| XKEYBOARD | `XkbUseExtension`, `XkbKeycodeToKeysym` (`keyboard.c`, `x11drv_main.c:679, 758`) | fallback to the core mapping, degraded layouts |
| XInputExtension 2.x | raw mouse (`mouse.c:1812-1816`: `XIQueryVersion`, `xinput2_available`) | fallback to core events, no raw deltas |
| RANDR >= 1.2 / 1.4 | monitors and modes (`xrandr.c:1242-1683`) | Xinerama fallback then single screen |
| RENDER | GDI text and alpha (`xrender.c:353`) | software GDI through `XPutImage` |
| XFIXES | clipboard monitoring (`clipboard.c:2131-2137`) | no clipboard notification |
| Composite, Xinerama, XVidMode, SHAPE, MIT-SHM | options (`x11drv_main.c:553`, `xinerama.c:91`, `xvidmode.c`, `bitblt.c:1574-1657`) | local degradations |
| EWMH/ICCCM through a window manager | `_NET_ACTIVE_WINDOW`, `_NET_SUPPORTED`, `WM_TAKE_FOCUS`, `_NET_WM_STATE_FULLSCREEN` (`event.c:642, 738, 888`, `window.c:1296, 1405, 1883`) | focus through direct `XSetInputFocus`, frameless fullscreen to be handled ourselves |
| DRI3 + Present | only through `nptunix` (`npt_unix_wsi.c:88-120`) | Neptune presentation impossible without replacement |

`winex11.drv`'s MIT-SHM uses SysV (`shmget(IPC_PRIVATE, ...)`, `bitblt.c:1614`). MEASURED on this machine: `kern.sysv.shmmax=4194304`, `shmall=1024`, `shmseg=8`: a 1080p buffer (8.3 MB) fails and Wine falls back to `XPutImage`.

Neptune presentation under A: (a) A-lite: `nptunix` maps the image's shm (already accessible, it is a POSIX fd) and pushes the pixels through `xcb_put_image` (CPU copy: 1920x1080x4 x 60 fps = 498 MB/s into the X socket, plus the server's copy); usable to validate the function and at 720p, not as a final solution; (b) A4; (c) bridge to an external macOS presenter: impossible to link the X window identifier to an XQuartz `NSWindow` without a hook in the server.

Rocket League needs under A: fullscreen = `_NET_WM_STATE_FULLSCREEN` so a window manager (x86-64 openbox under FEX, already used in QEMU); mouse capture = `XGrabPointer`/`XIConfineTo` + raw XI2 deltas, whose fidelity under XQuartz is not established (not tested); the gamepad does not go through X (section 4).

Estimate (INFERENCE, one person who knows Wine and X): A-lite on XQuartz or Xvfb + viewer: 2 to 4 weeks for a visible window with input; A4: 8 to 12 weeks; A3: 12 to 20 weeks. Risks: XQuartz stability on macOS 27, isolation of preferences (the RHL test created `~/Library/Preferences/org.xquartz.X11.plist`, deleted afterwards, `RHL/x11/README.md:44`), input latency, fullscreen without a real mode change, lack of fine control over the cursor.

### 2.3 Option B: custom Wine display driver talking to a macOS host through a socket

Principle: a `wine<name>.drv` driver (selected by `HKCU\Software\Wine\Drivers\Graphics=<name>`, default list `mac,x11,wayland` in `programs/explorer/desktop.c:43`, registry read and `LoadLibraryW(L"wine%s.drv")` in `load_graphics_driver`, `desktop.c:996-1030`) whose Unix part is an x86-64 ELF under FEX that talks through a Unix socket to a macOS host process ("display host") that owns the `NSWindow`s, the cursor, the events and the clipboard. `win32u` replaces any NULL member with the null driver's function (`SET_USER_FUNC`, `dlls/win32u/driver.c:1350-1420`). Interface: `struct user_driver_funcs` with `WINE_GDI_DRIVER_VERSION 108` (`include/wine/gdi_driver.h:221, 364-446` of voile-wine, identical to `wine-mirror/wine` tag `wine-11.0`, checked online): 64 members including 2 CrossOver additions (`pSet/GetCurrentProcessExplicitAppUserModelID`), and only 21 are filled by `winewayland.drv` (`waylanddrv_main.c:36-58`).

Event model to copy: `winewayland.drv` does not use `pProcessEvents`; at PE module initialization it starts a thread (`dllmain.c:30, 109`) that calls the unixlib `read_events` (blocking loop on the socket, `waylanddrv_main.c:105-113`) and injects the input into win32u. The Option B driver follows this model.

Items to write:

| Component | Content | Size reference (MEASURED) |
| --- | --- | --- |
| Wine driver (PE + ELF unixlib) | windows (`pCreateWindow`, `pDestroyWindow`, `pWindowPosChanging/Changed`, `pShowWindow`, `pSetWindowStyle`, `pSetWindowText`, `pActivateWindow`, `pSetCapture`), GDI surface in shm (`pCreateWindowSurface`), cursor (`pSetCursor`, `pGetCursorPos`, `pSetCursorPos`, `pClipCursor`), monitors and modes (`pUpdateDisplayDevices`, `pChangeDisplaySettings`), keyboard (`pKbdLayerDescriptor`, `pReleaseKbdTables`), clipboard (`pClipboardWindowProc`, `pUpdateClipboard`, optional at first), RPC transport and reader thread | `winewayland.drv`: 8,249 lines including `wayland_surface.c` 1,315 (Wayland-specific), `wayland_pointer.c` 1,096, `wayland_keyboard.c` 992, `wayland_data_device.c` 943 (clipboard), `window.c` 876, `window_surface.c` 531, `display.c` 308, OpenGL 278 + Vulkan 121 (useless here) |
| macOS display host (Swift/Objective-C) | one `NSWindow` + `NSView` + `CAMetalLayer` per top-level HWND, Z order, focus, fullscreen level, modes (`CGDisplay*`), mouse/keyboard/wheel events converted into scancodes and deltas, cursor (`NSCursor`), confinement/capture, `NSPasteboard`, Metal presenter (section below) | `winemac.drv` Cocoa side: `cocoa_window.m` 4,209, `cocoa_app.m` 3,067, `cocoa_display.m` 1,050, `cocoa_event.m` 795, `cocoa_cursorclipping.m` 507, `cocoa_clipboard.m` 248; the C/Cocoa boundary of `winemac.drv` counts 90 `macdrv_*` functions (`macdrv_cocoa.h`, 617 lines) |
| Input/output channel | private Unix socket (0700 folder, path < 104 bytes, peer authentication), bounded binary messages, one `SCM_RIGHTS` fd per shared image | vtest transport measured in 1.6: median 27 µs per fence round trip in native |

Reuse of `winemac.drv`: the Cocoa half is designed to be called by the C half through `macdrv_cocoa.h`. Two ways to use it: (i) copy pieces of `cocoa_*.m` into the host (mouse capture, resolution of `NSEvent` traps, `CGEventTap`, mode handling); (ii) compile the C half of `winemac.drv` for Linux, replacing `macdrv_*` with RPC calls. (ii) requires removing the CoreFoundation/CoreGraphics types (`CFArrayRef`, `CFDataRef`, `CGRect`) from the C files (`window.c` 2,254, `keyboard.c` 1,958, `display.c` 1,156, `clipboard.c` 1,526, `mouse.c` 964, `event.c` 538); no measurement was made of the real effort of this port.

Neptune presentation under B (this is the point that the option fully controls):

1. `npt_renderer_wine_common.c:47-85` reads the `__wine_x11_whole_window` property today. Under B, the driver publishes an analogous property instead (host window identifier) or `nptunix` queries the driver directly.
2. `nptunix.so` and the driver's Unix part live in the same process: `nptunix.so` can resolve through `dlsym` (after `dlopen(..., RTLD_NOLOAD)`) an entry point exported by the driver's Unix library and reuse its connection, without a second socket or a second authentication, and with a consistent order between window events and frames (design INFERENCE, not tried).
3. Minimal messages: `WSI_INIT{hwnd, n, width, height, format, strides[n]}` + `n` `SCM_RIGHTS` fds; `WSI_PRESENT{hwnd, index, serial, sync_interval}` + optional fence fd; `WSI_DESTROY`; return host -> client: `RELEASE{hwnd, index}` (equivalent of `IDLE_NOTIFY`, counted per image as `npt_unixlib.h:90-101` requires), `OCCLUDED`, `DISPLAY_LINK{timestamp}` for the pacing.
4. Host: each shm fd is mapped (`mmap`), wrapped by `newBufferWithBytesNoCopy:` (address and length multiples of 16 KiB) then `newTextureWithDescriptor:offset:bytesPerRow:` (lines of `dxmt-native` confirmed by `strings`), copied by a render pass to the drawable of the `CAMetalLayer` with scaling; `RELEASE` is emitted when the command buffer is completed. This API was not exercised here.
5. The same "present a shm image on a window" path serves the driver's GDI surfaces (launcher windows, EAC dialog boxes): a single presenter for two uses.
6. Presentation mode: the game's `SyncInterval` becomes `CAMetalLayer.displaySyncEnabled`, `maximumDrawableCount` 2 or 3; the QEMU version forced `ASYNC|COPY` (`npt_unix_wsi.c:246-247`) without real vsync.

Rocket League needs under B: fullscreen through a borderless window covering the screen (already designed on the Neptune side) with Metal scaling and DXGI modes provided by the host; unaccelerated relative mouse through `GCMouse` or `CGEvent` (the Accessibility permission is required only for the confinement `CGEventTap`); keyboard through physical scancodes; gamepad through HID bridge (section 4); the whole cursor (shape, hidden, jump) goes through the same channel.

Estimate (INFERENCE; hypothesis: one person who knows Wine and AppKit): Wine driver 4,000 to 5,500 lines, host 4,000 to 6,000 lines; 4 to 6 weeks for single window + input + cursor + Metal presentation; 8 to 12 weeks with fullscreen, multi-monitor, clipboard, GDI surfaces, ergonomics. Risks: new code surface (focus/Z order synchronization bugs), dependency on AppKit APIs sensitive to the main thread (D3DMetal says so in its strings: `dmn_window_create_for_view called off the main thread`), Accessibility permission for confinement through `CGEventTap`, FEX behavior on sockets and `SCM_RIGHTS` (translation of the Linux `msghdr`/`cmsghdr` structures to Darwin, which differ).

### 2.4 Option C: winewayland.drv with a minimal Wayland compositor on macOS

Principle: `winewayland.drv` (Wine 11.0, present in `voile-wine/dlls/winewayland.drv`, 8,249 lines) is used as is in the emulated Linux Wine; the Wayland socket is served by a macOS compositor.

Dependencies of `winewayland.drv` according to `Makefile.in` of `wine-mirror/wine` tag `wine-11.0`: libwayland-client, wayland-egl, EGL, xkbcommon, xkbregistry; included protocols: cursor-shape-v1, pointer-constraints-unstable-v1, relative-pointer-unstable-v1, text-input-unstable-v3, viewporter, wlr-data-control-unstable-v1, xdg-output-unstable-v1, xdg-shell, xdg-toplevel-icon-v1. The compositor must at least offer `wl_compositor`, `wl_shm`, `wl_seat`, `wl_output`, `xdg_wm_base`, `zxdg_output_manager_v1`, `wp_viewporter`, `zwp_relative_pointer_manager_v1` and `zwp_pointer_constraints_v1` (these last two for the raw mouse and confinement); a Web search (search engine, not checked in the code) also cites `zxdg_decoration_manager_v1` for server-side decorations.

Existing macOS compositors (primary sources partially read): `J-x-Z/cocoa-way` (Rust/Smithay, Metal rendering, "damage-aware SHM uploads", GPL-3.0, mentions a CoreAudio audio relay without specifying the mechanism, protocol list not provided by the README); `jwinarske/wayland-darwin` (libwayland through epoll-shim, wlroots Cocoa backend, Metal/IOSurface rendering, according to the search result; the README could not be retrieved, 404 on `main/README.md`); `owl-compositor/owl` (Objective-C, old). None was built or tried here; the GPL-3.0 license of cocoa-way must be examined before any reuse.

Neptune presentation under C: Neptune's shared image IS already a `wl_shm` buffer (fd + offset + stride + format). `wl_buffer.release` plays the role of `IDLE_NOTIFY`; `wl_surface.attach/damage/commit` is the `present`. Gap: `winewayland.drv` keeps `wl_display` and `wl_surface` internally (`process_wayland`); `nptunix.so` must therefore go through an entry point exported from `winewayland.so` (same process, `dlsym` on `RTLD_NOLOAD`), which modifies the upstream driver (300 to 500 lines estimated). GPU-before-read synchronization is done on the client side (wait for the fence fd before `commit`, like the "path 2" of `npt_unix_wsi.c:302-321`).

Rocket League needs under C: raw mouse and confinement through the two protocols above; fullscreen through `xdg_toplevel.set_fullscreen`; mode changes emulated by the compositor (no hardware mode); gamepad outside Wayland (section 4).

Estimate (INFERENCE): minimal compositor from `libwayland-server` through epoll-shim or Smithay: 6 to 10 weeks (6,000 to 10,000 lines) before being reliable with Wine; reusing `wayland-darwin` or `cocoa-way` can reduce this number but adds a dependency whose state is not measured. Wine side: 0 lines for the driver, about 400 for the presentation point. Risks: protocol conformance (the details of `winewayland.drv`'s behaviors are not contractual, for example the required versions of `xdg_wm_base`; a public report read online reports initialization failures due to a too old version), FEX translation of libwayland's `memfd_create`, `fcntl(F_ADD_SEALS)` and `SCM_RIGHTS` calls, behavior of libwayland-client and of the pointer protocols in the emulation.

### 2.5 Comparison table

| Criterion | A: X11 server | B: custom driver + host | C: winewayland + compositor |
| --- | --- | --- | --- |
| Wine code to write | 0 (excluding WSI) | 4,000 to 5,500 lines | 0 (excluding WSI, about 400) |
| macOS code to write or adopt | existing X server (XQuartz/Xvfb) + viewer or DDX | host 4,000 to 6,000 lines | compositor 6,000 to 10,000 lines or third-party project |
| Neptune presentation | A-lite through `xcb_put_image`; A4 through DRI3 | controlled, Metal presenter | wl_shm + entry point to add |
| Available measurements | Xvfb OK headless, visible XQuartz crashes (GLX) | none for the driver; winemac's Cocoa as reference | none |
| Relative mouse / capture | uncertain under XQuartz | full control | relative/constraint protocols to provide |
| Fullscreen | WM needed, no real mode | covering window + Metal scaling | `xdg_toplevel` fullscreen + scaling |
| Main risk | unstable X server, SysV SHM 4 MiB, no DRI3 | new code surface | compositor conformance, possible GPL |
| Effort estimate (INFERENCE) | 2 to 4 weeks (A-lite) / 8 to 12 (A4) | 4 to 6 weeks (minimal) / 8 to 12 (complete) | 6 to 10 weeks |

Recommendation (INFERENCE, to be confirmed by the lead): aim for B as the product path, because it is the only option where the window, the cursor, the capture, the screen modes and the Metal presentation belong to a single channel that we control, and the point where options A and C must work around Neptune (the window identity) is trivial there. Keep A2 or A-lite as early scaffolding if we want to see windows before having driver B; keep C as an alternative if the team prefers not to maintain its own Wine driver and accepts maintaining a compositor. Driver B can start from the model of `winewayland.drv` (21 table members, event reader thread) and reuse the Cocoa logic of `winemac.drv` on the host side.

## 3. Audio

to be completed

## 4. Gamepad and keyboard layout

to be completed

## 5. Ranked plan of owned graphics witnesses

to be completed

## 6. Unknowns and open questions

to be completed

## 7. Journal of commands and external sources

to be completed

## Appendix A: macOS vtest spike patches (applied to a copy of `NEP/virgl`, commit 482f9d8b)

These diffs are reproduced here because the spike lived in the session scratchpad (outside `EXP`). They apply from the root of the `virgl/` copy (relative paths `vtest/...`).

`vtest/vtest_shm.c` (POSIX shm under `__APPLE__`):

```diff
--- a/vtest/vtest_shm.c
+++ b/vtest/vtest_shm.c
@@ -45,8 +45,33 @@
 #endif
 }
 
+#ifdef __APPLE__
+/* Darwin has no memfd_create.  Reuse the POSIX shm path of os_create_anonymous_file
+ * (shm_open + shm_unlink + ftruncate, anon_file.c).  The tag must stay short:
+ * shm names are limited to PSHMNAMLEN (31) characters on macOS. */
+#include "mesa/util/anon_file.h"
+
 int vtest_new_shm(uint32_t handle, size_t size)
 {
+   int fd = os_create_anonymous_file((off_t)size, "vt");
+   (void)handle;
+   if (fd < 0)
+      return report_failed_call("os_create_anonymous_file", -errno);
+   return fd;
+}
+
+int vtest_shm_check(void)
+{
+   int fd = os_create_anonymous_file(16384, "vt");
+   if (fd >= 0) {
+      close(fd);
+      return 1;
+   }
+   return 0;
+}
+#else
+int vtest_new_shm(uint32_t handle, size_t size)
+{
    int fd, ret;
    char name[32];
 
@@ -75,3 +100,4 @@
 
     return 0;
 }
+#endif
```

`vtest/vtest_renderer.c` (timeline count, output fence through a pipe):

```diff
--- a/vtest/vtest_renderer.c
+++ b/vtest/vtest_renderer.c
@@ -105,6 +105,10 @@
    uint32_t count;
    struct vtest_sync **syncs;
    uint64_t *values;
+
+   /* Darwin: write end of the pipe whose read end was sent as OUT_FENCE_FD.
+    * One byte is written when the context fence retires (-1 if unused). */
+   int out_fence_wr;
 };
 
 struct vtest_sync_wait {
@@ -331,6 +335,8 @@
    uint32_t i;
    for (i = 0; i < submit->count; i++)
       vtest_unref_sync(submit->syncs[i]);
+   if (submit->out_fence_wr >= 0)
+      close(submit->out_fence_wr);
    free(submit);
 }
 
@@ -961,7 +967,7 @@
    case VCMD_PARAM_MAX_TIMELINE_COUNT:
       resp[0] = true;
       /* TODO until we have a timerfd */
-#ifdef HAVE_EVENTFD_H
+#if defined(HAVE_EVENTFD_H) || defined(__APPLE__)
       if (!getenv("VIRGL_DISABLE_MT"))
          resp[1] = VTEST_MAX_TIMELINE_COUNT;
 #endif
@@ -1974,6 +1980,11 @@
       for (i = 0; i < submit->count; i++) {
          vtest_signal_sync(submit->syncs[i], submit->values[i]);
          vtest_unref_sync(submit->syncs[i]);
+      }
+      if (submit->out_fence_wr >= 0) {
+         char one = 1;
+         (void)write(submit->out_fence_wr, &one, 1);
+         close(submit->out_fence_wr);
       }
       free(submit);
 
@@ -2330,6 +2341,7 @@
       submit->count = batch->sync_count;
       submit->syncs = (void *)&submit[1];
       submit->values = (void *)&submit->syncs[batch->sync_count];
+      submit->out_fence_wr = -1;
    }
 
    for (i = 0; i < batch->sync_count; i++) {
@@ -2384,6 +2396,18 @@
             fence_fd = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
             if (fence_fd < 0)
                report_failed_call("eventfd(signalled)", -errno);
+#elif defined(__APPLE__)
+            /* No eventfd/sync_file on Darwin: hand out a pipe read end that becomes
+             * readable when the context fence retires (vtest_signal_timeline). */
+            int pfd[2];
+            if (!pipe(pfd)) {
+               fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
+               fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
+               fence_fd = pfd[0];
+               submit->out_fence_wr = pfd[1];
+            } else {
+               report_failed_call("pipe(out fence)", -errno);
+            }
 #endif
          }
          if (batch->flags & VCMD_SUBMIT_CMD2_FLAG_OUT_FENCE_FD)
```

The hunk line numbers are those of the original copy; they were applied by a Python script (exact block replacement) and not by `patch`, so a direct `patch -p1` was not tested.
