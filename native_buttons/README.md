# native_buttons

A native C++23 Android app with an animated 3D pelican riding a bicycle along
a coastal causeway. The scene fills the window in both orientations, with
translucent controls floating over it: a bottom panel in portrait and a side
panel in landscape. The app requests no permissions and works offline.

Rendering uses hardware Vulkan 1.1, with instanced geometry,
physically based lighting, animated water, soft shadow mapping, floating-point
HDR targets, multisample antialiasing, bloom, and compute-driven particles.
The reusable renderer lives in `//common/gpu`; the pelican, bicycle, scenery,
animation shaders, and controls live in this directory. The pelican pedals,
breathes, blinks, and flexes its neck and wings; its scarf deforms on the GPU.
There is no software rasterizer.
On-screen rendering requires `VK_EXT_swapchain_maintenance1` and its instance
dependencies. Presentation fences keep swapchain images and semaphores alive
until the display has released them, including during rotation and shutdown.
Drivers without this support receive a startup error; offscreen rendering does
not require the extension.
Text uses a GPU atlas baked from the device's Roboto font with the shared
`@stb//:stb_truetype` library fetched by Bazel. Its upstream header includes
its license; no third-party source is copied into this repository.
The app remains C++-only, using Android's built-in `NativeActivity` with no
Java application sources or DEX code. Tab/Shift+Tab and arrow keys move focus;
Enter, Space, or the D-pad center activates the focused control. These GPU-drawn
controls still do not expose TalkBack accessibility nodes.

The camera circles the pelican once per minute with a gentle rise and fall and
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
**High detail** uses native resolution,
4x MSAA, 2048-pixel shadows, and 16,384 particles. **Ultra detail** uses
130% resolution, 4096-pixel shadows, more scenery, and 65,536 particles.
The display shows measured frame rate and GPU time when timer queries are
available. The frame loop follows Android's Choreographer, requests a 60 Hz
display mode, and stops when the Activity pauses or loses its window.
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
Rotation uses Vulkan's advertised extent and transform for render targets and
independently monitors native-window dimensions. Android can cache the Vulkan
extent until presentation, and either report can change after the last Activity
callback. Each size source is compared with its own previous observation, so a
cached extent still triggers the frame needed to refresh it. Checks after
presentation then rebuild stale targets. Idle checks catch later changes without
advancing paused animation, yaw, or zoom. The camera
and scene use the full window; only the overlays respect content insets.
Controls retain at least 48 dp touch targets. Short windows use a compact
translucent toolbar instead of shrinking the buttons.
Android's saved Activity state includes the count, detail setting, pause state,
camera yaw, zoom, and animation time in a validated, versioned record. Version 1
records use the default zoom; legacy count-only records remain supported too.
Animation time is accumulated and saved in double precision. CPU and shader
oscillations use a shared bounded phase, while scenery, camera orbit, road
markings, and particles retain their own cycles, so long sessions keep animating
without a visible reset. Version 1 and 2 records retain their original time and
available settings when upgraded to the double-precision format.
Counter updates replace the saved file atomically after flushing a temporary
file, so a failed write preserves the previous value. Save failures are shown
in the counter panel and in an Android toast.

## Build

After the shared [toolchain setup](../README.md#toolchain-setup), run from
the workspace root:

```sh
bazel build //native_buttons
```

Output: `bazel-bin/native_buttons/native_buttons.apk`.
`bazel build //...` also builds the app and shared libraries.

`native_buttons_lib` compiles the lifecycle/input adapter in `main.cc` and links
the worker runtime, storage, scene, and shared renderer. `alwayslink`
preserves the dynamically discovered `ANativeActivity_onCreate` entry point.
Compiler and linker flags come from `//tools:android.bzl`, including
C++23, full symbol stripping, and 16 KiB ELF segment alignment. Vulkan GLSL lives
in `shaders/` and `//common/gpu/shaders`; Bazel compiles and validates SPIR-V with
the pinned NDK shader tools and embeds it in the native library.
`//common:support` links libc++ runtime sources built by Bazel without exceptions,
RTTI, or unwind tables. The prebuilt NDK C++ runtime, libc++abi, libunwind, and
demangler are excluded. `std::nothrow` allocation still returns null on failure;
ordinary allocation failure and standard-library contract failures abort.
Application errors continue to propagate through `std::expected`.
The `native_buttons`
`android_binary` links `libnative_buttons.so`, processes the manifest, and
packages, aligns, and signs the APK. The manifest's `android.app.lib_name`
matches the shared library. The application ID is `dev.demo.nativebuttons`;
minimum Android API is 26 and target API is 35.

The optional `gpu_probe` target renders and benchmarks the same scene on this
phone's GPU using offscreen Vulkan images. It checks pipeline creation, render
targets, synchronization and readback, and can save a PPM screenshot before installation:

```sh
bazel run //native_buttons:gpu_probe --platforms=//:arm64-v8a \
    --run_under=//tools:android_test_runner -- /tmp/pelican.ppm 1080 2400 0
```

Use `1` instead of `0` for Ultra detail. Optional arguments after the quality
flag set the animation start time in seconds, pixels per dp, and zoom. The default
density gives the shorter image dimension a width of 360 dp. For example,
`/tmp/landscape.ppm 960 432 0 12 1.2 1.5` renders an 800-by-360 dp landscape view at 1.5x zoom.
Probe throughput is a synchronous
offscreen measurement, not a claim about the installed app's sustained frame
rate. Display composition, thermal limits, and frame pacing affect the app.

The runner copies the executable to a temporary real Android `/data` path so the
Vulkan loader can access the vendor GPU driver under PRoot, then removes it.
GPU timestamps are available on this phone and appear in the overlay.

Run the native regression suite on the phone:

```sh
bazel test //common:runtime_test //native_buttons:application_test \
    //native_buttons:runtime_fault_test //native_buttons:scene_test \
    //native_buttons:state_test //native_buttons:gestures_test //native_buttons:renderer_test \
    //native_buttons:presentation_test \
    //native_buttons:depth_fallback_test --platforms=//:arm64-v8a \
    --run_under=//tools:android_test_runner
```

The application test exercises atomic persistence with an injected failed write,
worker startup/shutdown, touch jitter and scaled cancellation, keyboard navigation, lifecycle
snapshots, paused redraw, insets, and surface recreation. The scene test checks
actual scenery positions through a year of animation, phase-wrap continuity, and
control hit testing, including retaining the previous layout when presentation is
deferred. The
runtime fault test runs the real worker and scene with a simulated GPU boundary:
it checks delayed rotation and resizing with no subsequent callback or input,
zero-sized surface recovery, visible/hidden Activity transitions, surface
recreation, idle query errors, and visible button hit targets. It also checks
transient retries and redraw timeout/cleanup when no vsync callbacks arrive.
Subprocess checks verify bounded snapshots, detachment and shutdown with a stalled worker or GPU
destructor. State tests cover round trips, legacy data, and malformed records.
Gesture tests exercise real motion-event handling with synthetic Android pointers,
including reordered indices, extra fingers, near-zero spans, cancellation,
single-finger taps after pinching, and chronological processing of batched motion.
The real-worker tests check batched excursions that return inside a Reset button,
valid batched jitter, and 60 Hz clock updates after days of saved animation time.
Application tests cover zoom limits, paused zooming, accidental-tap prevention,
and zoom restoration across recreation.
GPU tests exercise portrait and landscape offscreen targets, preparation boundaries, quality changes,
readback, camera zoom at both limits, and fixed-quality animation with
the changing UI excluded. Fixed-camera captures also check shader-driven motion
at large timestamps and across phase wraps. The depth-fallback test simulates
unsupported D24S8 and renders with another format on the real GPU.
The presentation test uses real GPU commands with a simulated display boundary
to exercise delayed window-size reports, capabilities cached until presentation,
all four rotations, transform-only changes,
resizes during acquisition/presentation, zero extents, idle geometry queries,
transient swapchain failures, and presentation-fence retirement.

Enable Khronos API and synchronization validation for the GPU tests:

```sh
bazel test //native_buttons:renderer_test //native_buttons:depth_fallback_test \
    //native_buttons:presentation_test \
    --platforms=//:arm64-v8a --define=vulkan_validation=true \
    --run_under=//tools:vulkan_validation_runner
```

The optional runner downloads the pinned Android validation layer through Bazel.
Its Android layer-path bootstrap and validation callbacks are compiled only with
that flag; neither they nor the validation library are included in the normal APK.

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
at 720x1600 and 30 FPS into FFmpeg:

```sh
bazel run //native_buttons:gpu_probe --platforms=//:arm64-v8a \
    --run_under=//tools:android_test_runner -- --video | \
    ffmpeg -f rawvideo -pixel_format rgba -video_size 720x1600 -framerate 30 \
        -i pipe:0 -an -c:v libx264 -preset veryfast -crf 20 \
        -pix_fmt yuv420p -threads 2 -movflags +faststart /tmp/native-buttons-10s.mp4
```

This exports a fresh instance of the app's scene with a zero counter on the
local GPU. It does not capture the running Activity or Android's system UI.

Signing uses the debug key bundled with `rules_android`, so no local key or
preparation script is needed. This certificate differs from the previous
local demo key: Android cannot install it as an update over that older APK.
Uninstalling the older app removes its saved count.

## Historical Vulkan migration measurements

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

Termux needs `allow-external-apps = true` in `~/.termux/termux.properties`
while sharing the APK; reload Termux settings after changing it and restore
the original setting after installation. Android may also require allowing
Termux to install unknown apps and tapping Install. The `chmod` handles
Android 14+'s read-only requirement for dynamically loaded DEX files under
PRoot.
