# Native Buttons

A native C++23 Android app with an animated 3D pelican or flamingo riding a bicycle
along a coastal causeway. The scene fills the window in both orientations, with
translucent controls floating over it: in portrait, detail, pause, and bird
toggles under the title and a counter panel at the bottom; in landscape, a side
panel. The app requests no permissions, works offline, and keeps the screen on
while it is shown.

Rendering uses hardware Vulkan 1.4, with instanced geometry,
physically based lighting, animated water, soft shadow mapping, floating-point
HDR targets, multisample antialiasing, bloom, and compute-driven particles.
Dynamic rendering, synchronization2 barriers, and push descriptors replace render
passes, framebuffers, and descriptor pools. Per-frame values travel in 192 bytes
of push constants, within the 256 that Vulkan 1.4 guarantees, and pipelines are
built directly from SPIR-V without shader modules. Frames complete on fences: this
phone's driver wakes from timeline-semaphore waits noticeably later. Depth uses
stencil-free formats; dynamic rendering would otherwise preserve an unused
stencil aspect.
The reusable renderer lives in `//common/gpu`; the birds, bicycle, scenery,
animation shaders, and controls live in this directory. The flamingo
has soft pink plumage, a slender curved neck, and a short dark-tipped beak. It
pedals, breathes, and flexes its neck and wings; its large eyes glance around,
blink, and flutter. Its mint scarf deforms on the GPU.
There is no software rasterizer.
On-screen rendering requires `VK_EXT_swapchain_maintenance1` with its instance
dependencies, and `VK_GOOGLE_display_timing`. Presentation fences keep swapchain
images and semaphores alive until the display has released them, including
during rotation and shutdown. Drivers without this support receive a startup
error; offscreen rendering requires neither extension.
Text uses a GPU atlas baked from the device's
`/system/fonts/RobotoStatic-Regular.ttf` with the shared `@stb//:stb_truetype`
library fetched by Bazel. Printable ASCII glyphs are rasterized at 128 px into a
1024-pixel atlas with 11 mipmap levels; a missing font is a startup error. The
upstream header includes its license; no third-party source is copied into this
repository.
The app remains C++-only, using Android's built-in `NativeActivity` with no
Java application sources or DEX code. Tab/Shift+Tab and arrow keys move focus;
Enter, Space, or the D-pad center activates the focused control. These GPU-drawn
controls still do not expose TalkBack accessibility nodes.

The camera circles the selected bird once per minute with a gentle rise and fall and
small distance changes. Drag to adjust the view through a full circle. Spread
two fingers to zoom in and pinch them together to zoom out, from 0.5x to 2.5x.
Pinching cancels pending button taps and keeps the controls at their normal size.
After a pinch, lift both fingers before starting another one-finger drag or tap. Pause
freezes both the animation and the automatic camera motion and stops continuous
rendering. Controls, dragging, and window redraws still update the paused scene.
While the Activity remains visible, lightweight geometry checks run every 100 ms.
A paused scene needs GPU work only for a changed surface or pending redraw. This
also covers visible but inactive split-screen windows. Monitoring stops when the Activity
is hidden or the surface is detached, and resumes when it becomes visible.
The feet and crank arms share the same forward-pedaling motion.
Tap **Flamingo / Pelican** beside the detail and pause controls to switch birds.
The selector shows the current bird, and the scene caption follows it. Switching
works while paused and preserves the counter, detail level, animation time, yaw,
and zoom. Each bird has its own neck shape and camera framing. Both meshes are
created with the scene, so switching needs no resource rebuild.
**High detail** uses native resolution,
4x MSAA, 2048-pixel shadows, and 16,384 particles. **Ultra detail** uses
130% resolution, 4096-pixel shadows, more scenery, and 65,536 particles.
The display shows measured frame rate and GPU time, except in the compact
toolbar. The Activity reads the display's peak refresh rate, the fastest of its
current mode and that mode's alternative rates, and the window requests it as
fixed-rate content, so Android keeps the display there. Until Android reports
it, the fastest refresh Choreographer has shown stands in. Animation emulates
the peak rate divided by N, from 1 to 8, such as 120, 60, 40, 30, or 24 fps, on
an exact grid of presentation times N refreshes apart; if the display still
runs slower, the grid follows its refresh period. Android throttles an app's
Choreographer to the rate the app presents at, so Choreographer only describes
the upcoming vsyncs: their expected presentation times and how long before
presentation a frame must be ready. A timer on the rendering thread starts each
frame just in time for its slot, allowing for that latency, the average CPU and
GPU time, and a lead. Through `VK_GOOGLE_display_timing`, the frame asks the
display not to show it earlier than 2 ms before its slot. The lead starts at
2 ms and grows by 2 ms per late frame, up to one refresh, since GPU time varies
from frame to frame; it shrinks by 0.02 ms per frame on time, never below 2 ms.
Rendering is pipelined: the CPU records a frame while the GPU renders up to two
earlier ones, and at least six swapchain images, as the driver allows, hold
frames queued ahead of their presentation, so at a sustainable rate the
rendering thread does not wait on the GPU or the compositor.

N is the smallest interval whose budget fits the frame cost, where the cost is
the larger of the CPU and GPU time, since the two overlap. The GPU also overlaps
consecutive frames: on this phone, High sustains 120 fps with a measured GPU
time of about 9.4 ms against an 8.3 ms budget, so the budget allows 1.2 times
its length. A new surface, detail level, or peak-rate report restarts
measurement at the peak rate, where GPU clocks run highest; Android reports the
peak rate on every configuration change, including rotation. Switching birds
keeps N. The cost after 30 frames, or up to twice as long while it is still
falling as the GPU warms up, sets N, and any cheaper frame afterwards lowers
it. A frame is late when display timing shows it reached the screen more than
half a refresh later, relative to its requested time, than the previous frame,
or when its timer fired too late to record it before its deadline; that frame
takes the next reachable slot. Late frames first lengthen the lead. Once the
lead is a full refresh, four late frames within a 120-frame window set a floor
one above the current N, so a few misses are tolerated. The floor stays until a
hold has passed since N rose to it; then, while the current window has no late
frames, it drops one step per frame until the cost sets N again. The GPU clock
falls at slower rates, inflating the cost, so after a hold without late frames
or a floor holding N up, the next faster rate is measured again if the cost
exceeds its budget by at most 25%, and kept if it fits. The hold starts at one
second and doubles, up to 32 seconds, when N slows within five seconds of
speeding up; any other slowdown resets it to one second. Late signals are
ignored for eight frames after a rate or display mode change. The loop stops
when the Activity pauses or loses its window.
Rendering and counter persistence run on a dedicated native thread. Required
window-redraw callbacks wait for a completed frame, including while paused;
window-destruction callbacks wait until the worker releases the surface.
Temporary surface unavailability and out-of-date swapchains defer frames for a
later retry, with a timed fallback when vsync callbacks are unavailable.
Redraws, state snapshots, surface detachment, and shutdown have
three-second monotonic deadlines, including when Choreographer stops delivering
callbacks. A redraw or snapshot timeout reports an error and allows one further
second for cleanup. If the worker cannot release its window or stop by the
deadline, the process terminates rather than returning with live window users
or blocking the main thread indefinitely. GPU fence waits are also bounded.
Each frame prepares its targets once; a subsequent window-size change defers the
frame so camera, overlay, and touch coordinates stay aligned. Input uses the
controls from the last successfully presented frame. Rebuilt render targets
become ready only after all resources are created; offscreen capture requires a
new frame after target replacement. Tap cancellation measures displacement from
the initial touch using Android's density-aware `ViewConfiguration` tolerance,
refreshed when the device configuration changes.
Render targets follow Vulkan's advertised extent, and a transform change also
rebuilds them; swapchains use the identity pre-transform and leave rotation to
Android's compositor. The runtime independently monitors native-window
dimensions. Android can cache the Vulkan
extent until presentation, and either report can change after the last Activity
callback. Each size source is compared with its own previous observation, so a
cached extent still triggers the frame needed to refresh it. Checks after
presentation then rebuild stale targets. Idle checks catch later changes without
advancing paused animation, yaw, or zoom. The camera
and scene use the full window; only the overlays respect content insets.
Controls retain at least 48 dp touch targets in windows at least 160 dp wide.
Windows narrower than 300 dp or shorter than 420 dp (280 dp in landscape) use a
compact translucent toolbar without the title or frame statistics instead of
shrinking the buttons.
Android's saved Activity state includes the count, selected bird, detail setting,
pause state, camera yaw, zoom, and animation time in a validated, versioned record.
Only the current version 4 record is accepted; other data starts a new session
with default settings, the count from `count.txt`, and the flamingo. A count
restored from the Activity overrides `count.txt` and is written back; counts
stay between 0 and 999,999.
Animation time advances by the spacing of presentation slots, at most 0.1 s per
frame, and is accumulated and saved in double precision. CPU and shader
oscillations use a shared bounded phase, while scenery, camera orbit, road
markings, and particles retain their own cycles, so long sessions keep animating
without a visible reset.
Counter updates replace the saved file atomically after flushing a temporary
file, so a failed write preserves the previous value. Save failures are shown
in the counter panel and in an Android toast.

## Build

After the shared [toolchain setup](../README.md#toolchain-setup), run from
the workspace root:

```sh
bazel build //native_buttons
```

Output: `bazel-bin/native_buttons/native_buttons.apk`, 82,203 bytes, with a v4
`native_buttons.apk.idsig` beside it. `bazel build //...` builds both apps, the
tests, and the tools.

`native_buttons_lib` compiles the lifecycle/input adapter in `main.cc` and links
the gesture handling, worker runtime, state and storage, scene, and shared
renderer. `alwayslink` preserves the dynamically discovered
`ANativeActivity_onCreate` entry point. Shared compiler and linker flags come
from `.bazelrc`, including C++23 for ARMv9-A, hidden symbols, link-time
optimization across all native libraries, full symbol stripping, and 16 KiB ELF
segment alignment; targets add only their own libraries, such as
`-lnativewindow`, and the tests' wrapped functions. Vulkan GLSL lives
in `shaders/` and `//common/gpu/shaders`; Bazel compiles and validates SPIR-V 1.6 with
the pinned NDK shader tools and embeds it in the native library. NDK r29's API 35
stub library predates Vulkan 1.4, so the renderer loads its one Vulkan 1.4
command, `vkCmdPushDescriptorSet`, and the display-timing commands from the
driver.
`//common:support` links libc++ runtime sources built by Bazel without exceptions,
RTTI, or unwind tables. The prebuilt NDK C++ runtime, libc++abi, libunwind, and
demangler are excluded. `std::nothrow` allocation still returns null on failure;
ordinary allocation failure and standard-library errors that would otherwise
throw abort.
Application errors continue to propagate through `std::expected`.
The `native_buttons`
`android_binary` links `libnative_buttons.so`, processes the manifest, and
packages, aligns, and signs the APK. The manifest's `android.app.lib_name`
matches the shared library. The application ID is `dev.demo.nativebuttons`;
minimum and target Android API are 36, and the launcher label is
**Native Buttons**. The manifest requires Vulkan 1.4 and hardware level 1,
matching the renderer's device check. It disables backups, so the count is not
in Android backups, keeps the native library compressed in the APK for
extraction at installation, and handles orientation and size changes without
recreating the Activity.

The optional `gpu_probe` target renders and benchmarks the same scene on this
phone's GPU using offscreen Vulkan images. It renders 90 frames, times the last
60, and, when given a path, reads back the last frame into a PPM screenshot
before installation:

```sh
bazel run //native_buttons:gpu_probe -- /tmp/flamingo.ppm 1080 2400 0
```

All arguments are optional: the path, the size (1080x2400 by default, each side
1 to 4096), the quality (`0` for High, any other number for Ultra), the
animation start time in seconds (2 by default), pixels per dp, zoom (1 by
default, 0.5 to 2.5), and `pelican` or `flamingo` (the default). The default
density gives the shorter image dimension a width of 360 dp, and the overlay
shows a count of 7. For example,
`/tmp/landscape.ppm 960 432 0 12 1.2 1.5 pelican` renders the pelican in an
800-by-360 dp landscape view at 1.5x zoom.
Probe throughput is a synchronous
offscreen measurement, not a claim about the installed app's sustained frame
rate. Display composition, thermal limits, and frame pacing affect the app.

`.bazelrc` runs every `bazel run` and `bazel test` under
`//tools:android_test_runner`. It copies the executable to a temporary
directory under Termux's real `/data` path so the Vulkan loader can access the
vendor GPU driver under PRoot, runs it with Android's system linker, and removes
the directory afterwards. GPU timestamps are available on this phone and appear
in the overlay.

Run the native regression suite on the phone:

```sh
bazel test //common:runtime_test //native_buttons:application_test \
    //native_buttons:runtime_fault_test //native_buttons:scene_test \
    //native_buttons:state_test //native_buttons:gestures_test //native_buttons:renderer_test \
    //native_buttons:presentation_test
```

`//common:runtime_test` checks the libc++ runtime, as described in the
[workspace README](../README.md). The application test exercises atomic
persistence with an injected failed write, the 0 to 999,999 count range, worker
startup/shutdown, touch jitter and scaled cancellation, keyboard navigation,
lifecycle snapshots, paused redraw, insets, a detached runtime drawing nothing,
Reset saved during the pause handshake, and surface recreation. It also covers
zoom limits, paused zooming, accidental-tap prevention, and zoom restoration
across recreation, and touch and keyboard bird selection while paused and its
restoration. The scene test checks actual scenery positions through a year of
animation, phase-wrap continuity, and control hit testing, including retaining
the previous layout when presentation is deferred. It checks that switching
birds reuses the prebuilt meshes and shows only the selected bird's name and
caption, and that all five controls are at least 48 dp, disjoint, inside the
safe area, and hit-tested correctly in compact, portrait, and landscape layouts
at densities from 1 to 4.

The runtime fault test runs the real worker and scene with a simulated GPU
boundary: it checks delayed rotation and resizing with no subsequent callback or
input, zero-sized surface recovery, visible/hidden Activity transitions, surface
recreation, idle query errors, and visible button hit targets. It also checks
transient retries, deferred frames that recover with one wait and one frame, a
resize during frame layout, and redraw timeout/cleanup when no vsync callbacks
arrive. Subprocess checks verify that, with a stalled worker or GPU destructor,
a snapshot fails with a timeout and detachment and shutdown abort the process,
each within 2.5 to 5 seconds. The test also checks batched excursions that
return inside a Reset button, valid batched jitter, and the animation clock in
eleven ten-second runs of simulated vsync and display timing: at 60 Hz after
days of saved animation time; drawing every refresh with a fast GPU; keeping
that while Android runs a 120 Hz display at 60 Hz for two seconds; settling on
every other refresh when the GPU is too slow; returning to every refresh once it
is fast enough; settling on every third refresh for a slower GPU while
Choreographer calls back on only every other vsync; returning to and keeping
every refresh for a GPU that starts slowly and whose clock falls further at
lower rates; keeping 60 fps when occasional frames cost more than average; and
keeping 120 fps while GPU time settles from a slow start. Each run checks that
requested presentation times end on an exact grid of refreshes, and that
animation time advances ten seconds within 70 ms, keeps sub-frame precision when
saved, and stays still while paused.

State tests cover round trips and rejected or malformed records. Gesture tests
exercise real motion-event handling with synthetic Android pointers, including
reordered indices, extra fingers, near-zero spans, cancellation, single-finger
taps after pinching, and chronological processing of batched motion.

The renderer test runs `gpu_probe` with `--exercise` on portrait and landscape
offscreen targets: preparation boundaries, High/Ultra/High changes, readback,
opaque output under the UI, the exact MSAA, particle, and triangle workload of
both birds, switching birds at a frozen timestamp, camera zoom at both limits,
identical images for equal timestamps, and fixed-quality animation with the
changing UI excluded. Fixed-camera captures also check shader-driven motion at
large timestamps and across phase wraps. The presentation test uses real GPU
commands with a simulated display boundary to exercise delayed window-size
reports, capabilities cached until presentation, all four rotations,
transform-only changes, stable suboptimal results that keep the swapchain,
resizes during acquisition/presentation, zero extents, idle geometry queries,
transient swapchain failures, reported presentation allocation failures,
requested presentation times, late-frame reports that ignore a steady delay, a
deadline on every GPU wait, presentation-fence retirement, and rejection of
drivers without surface maintenance.

Enable Khronos API and synchronization validation for the GPU tests:

```sh
bazel test //native_buttons:renderer_test //native_buttons:presentation_test \
    --config=vulkan_validation
```

Any validation error fails the test; warnings are printed.
`--config=vulkan_validation` defines `vulkan_validation=true` and runs the tests
under `//tools:vulkan_validation_runner`, which copies the pinned Khronos
Android validation layer, fetched by Bazel, beside the executable. The tests'
layer-path bootstrap and the renderer's validation callbacks are compiled only
with that config; the normal APK includes neither them nor the layer.

These tests do not verify Android's compositor or the real Activity window.
After installation, check portrait, landscape and reverse landscape, touch and
keyboard controls, rotation while paused, background/resume, and Activity
recreation. The counter and controls should remain upright and aligned with
their touch targets. ADB (Android Debug Bridge) can install the APK, send input,
and collect screenshots and logs from a connected Android device:

```sh
adb install -r bazel-bin/native_buttons/native_buttons.apk
adb shell am start -n dev.demo.nativebuttons/android.app.NativeActivity
```

For a ten-second MP4 export, the probe can stream 300 GPU-rendered RGBA frames
of the High-detail flamingo at 720x1600, 2 px per dp, from an animation time of
2 s, at 30 FPS into FFmpeg:

```sh
bazel run //native_buttons:gpu_probe -- --video | \
    ffmpeg -f rawvideo -pixel_format rgba -video_size 720x1600 -framerate 30 \
        -i pipe:0 -an -c:v libx264 -preset veryfast -crf 20 \
        -pix_fmt yuv420p -threads 2 -movflags +faststart /tmp/native-buttons-10s.mp4
```

This exports a fresh instance of the app's scene with a zero counter on the
local GPU; its FPS readout shows the export's render rate. It does not capture
the running Activity or Android's system UI.

Signing uses the debug key bundled with `rules_android`, so no local key or
preparation script is needed. The APK carries an APK Signature Scheme v3
signature, with the v4 `.idsig` beside it.

## Historical Vulkan migration measurements

These measurements describe the pelican scene. The flamingo option
has a different mesh workload: 739,398 triangles in High and 862,150 in Ultra.

Measured on this Pixel 8 Pro / Mali-G715, comparing the saved release from commit
`62d0917` with the Vulkan release. Both executables ran locally from Android's
`/data` path, with the foreground app paused and no build running. For each
quality mode, five runs per API alternated order, with three seconds between
runs. Each run warmed up for 30 frames and timed the next 60 frames, using the
same fixed animation times and 1080x2400 output.

| Metric | Previous renderer | Vulkan | Change |
|---|---:|---:|---:|
| High, median offscreen FPS | 42.13 | 45.45 | +7.9% |
| Ultra, median offscreen FPS | 28.90 | 36.50 | +26.3% |
| Signed APK, bytes | 53,722 | 66,010 | +12,288 (22.9%) |
| Uncompressed native library, bytes | 85,760 | 118,504 | +32,744 (38.2%) |

High run ranges were 41.09–45.27 FPS before and 43.28–71.79 FPS with Vulkan;
Ultra ranges were 27.51–29.73 and 34.43–37.26 FPS. The early 71.79 FPS result
shows the clock/temperature sensitivity of short phone benchmarks. These are
synchronous offscreen measurements, not sustained on-screen frame rates.

Geometry and rendering workload are preserved: 747,078 triangles in High and
869,830 in Ultra, 4x MSAA, the same shadow resolutions, six bloom passes, and
16,384/65,536 compute particles. Ultra renders at 1404x3120. Vulkan adds explicit
resource management and command recording; optimized SPIR-V occupies 25,068
uncompressed bytes versus 8,777 bytes of previous shader source. APK growth is
12 KiB. No validation library, shader compiler, exception runtime, unwinder,
demangler, debug information, or static symbol table is bundled.

Validation covered SPIR-V, Khronos API/synchronization checks, repeated renderer
creation/destruction, portrait/landscape offscreen targets, High/Ultra/High
switching, asynchronous frames, and image readback. Release and validated tests
passed, as did `bazel build //native_buttons` and APK signature verification.

## Install on this phone

After building, copy the APK to Termux's home and open Android's package
installer:

```sh
mkdir -p /data/data/com.termux/files/home/native_buttons
cp bazel-bin/native_buttons/native_buttons.apk \
    /data/data/com.termux/files/home/native_buttons/native_buttons.apk
chmod 0400 /data/data/com.termux/files/usr/libexec/termux-am/am.apk
am start --user 0 -W -a android.intent.action.INSTALL_PACKAGE \
    -d content://com.termux.files/data/data/com.termux/files/home/native_buttons/native_buttons.apk \
    -t application/vnd.android.package-archive -f 0x10000001 \
    -p com.google.android.packageinstaller
```

Termux needs `allow-external-apps = true` in
`/data/data/com.termux/files/home/.termux/termux.properties` while sharing the
APK; run `termux-reload-settings` after changing it and restore the original
setting after installation. Android may also require allowing
Termux to install unknown apps and tapping Install. The `chmod` handles
Android 14+'s read-only requirement for dynamically loaded DEX files under
PRoot.
