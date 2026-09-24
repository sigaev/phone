# native_buttons

A native C++23 Android app with an animated 3D pelican riding a bicycle along
a coastal causeway. The counter and its saved value remain available below
the scene. The app requests no permissions and works offline.

Rendering uses hardware Vulkan 1.1, with instanced geometry,
physically based lighting, animated water, soft shadow mapping, floating-point
HDR targets, multisample antialiasing, bloom, and compute-driven particles.
The reusable renderer lives in `//common/gpu`; the pelican, bicycle, scenery,
animation shaders, and controls live in this directory. The pelican pedals,
breathes, blinks, and flexes its neck and wings; its scarf deforms on the GPU.
There is no software rasterizer.
Text uses a GPU atlas baked from the device's Roboto font with the shared
`@stb//:stb_truetype` library fetched by Bazel. Its upstream header includes
its license; no third-party source is copied into this repository.
The custom controls do not expose accessibility nodes.

The camera circles the pelican once per minute with a gentle rise and fall and
small distance changes. Drag to adjust the view through a full circle. Pause
freezes both the animation and the automatic camera motion; dragging still works.
The feet and crank arms share the same forward-pedaling motion.
**High detail** uses native resolution,
4x MSAA, 2048-pixel shadows, and 16,384 particles. **Ultra detail** uses
130% resolution, 4096-pixel shadows, more scenery, and 65,536 particles.
The display shows measured frame rate and GPU time when timer queries are
available. The frame loop follows Android's Choreographer, requests a 60 Hz
display mode, and stops when the Activity pauses or loses its window.

## Build

After the shared [toolchain setup](../README.md#toolchain-setup), run from
the workspace root:

```sh
bazel build //native_buttons
```

Output: `bazel-bin/native_buttons/native_buttons.apk`.
`bazel build //...` also builds the app and shared libraries.

`native_buttons_lib` compiles `main.cc` with `cc_library` and links the scene
and shared renderer. `alwayslink`
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

Use `1` instead of `0` for Ultra detail. An optional final argument sets the
animation start time in seconds, useful for inspecting different camera angles.
Probe throughput is a synchronous
offscreen measurement, not a claim about the installed app's sustained frame
rate. Display composition, thermal limits, and frame pacing affect the app.

The runner copies the executable to a temporary real Android `/data` path so the
Vulkan loader can access the vendor GPU driver under PRoot, then removes it.
GPU timestamps are available on this phone and appear in the overlay.

Exercise renderer recreation, both orientations, quality transitions and readback:

```sh
bazel test //native_buttons:renderer_test --platforms=//:arm64-v8a \
    --run_under=//tools:android_test_runner
```

Enable Khronos API and synchronization validation for that same test:

```sh
bazel test //native_buttons:renderer_test --platforms=//:arm64-v8a \
    --define=vulkan_validation=true --run_under=//tools:vulkan_validation_runner
```

The optional runner downloads the pinned Android validation layer through Bazel.
Its Android layer-path bootstrap and validation callbacks are compiled only with
that flag; neither they nor the validation library are included in the normal APK.

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

## Vulkan migration measurements

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
