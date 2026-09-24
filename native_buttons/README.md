# native_buttons

A native C++23 Android app with an animated 3D pelican riding a bicycle along
a coastal causeway. The counter and its saved value remain available below
the scene. The app requests no permissions and works offline.

Rendering uses hardware OpenGL ES 3.2 through EGL, with instanced geometry,
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
up to 4x MSAA, 2048-pixel shadows, and 16,384 particles. **Ultra detail** uses
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
C++23 and 16 KiB ELF segment alignment. The `native_buttons`
`android_binary` links `libnative_buttons.so`, processes the manifest, and
packages, aligns, and signs the APK. The manifest's `android.app.lib_name`
matches the shared library. The application ID is `dev.demo.nativebuttons`;
minimum Android API is 26 and target API is 35.

The optional `gpu_probe` target renders and benchmarks the same scene on this
phone's GPU using an offscreen EGL surface. It checks driver shader compilation
and framebuffer support and can save a PPM screenshot before installation:

```sh
bazel build //native_buttons:gpu_probe --platforms=//:arm64-v8a
LD_LIBRARY_PATH=/apex/com.android.i18n/lib64:/apex/com.android.runtime/lib64/bionic:/system/lib64 \
    /system/bin/linker64 "$(pwd)/bazel-bin/native_buttons/gpu_probe" /tmp/pelican.ppm 1080 2400 0
```

Use `1` instead of `0` for Ultra detail. An optional final argument sets the
animation start time in seconds, useful for inspecting different camera angles.
Probe throughput is a synchronous
offscreen measurement, not a claim about the installed app's sustained frame
rate. Display composition, thermal limits, and frame pacing affect the app.

The initial Pixel 8 Pro / Mali-G715 check at 1080x2400 measured 43.3 FPS in
High and 31.6 FPS in Ultra, with 4x MSAA in both modes. These are short probe
runs, a baseline for the next visual and performance iteration. This driver
did not return usable GPU timer results, so the overlay shows the GPU name.

For a ten-second MP4 export, the probe can stream 300 GPU-rendered RGBA frames
at 720x1600 and 30 FPS into FFmpeg:

```sh
LD_LIBRARY_PATH=/apex/com.android.i18n/lib64:/apex/com.android.runtime/lib64/bionic:/system/lib64 \
    /system/bin/linker64 "$(pwd)/bazel-bin/native_buttons/gpu_probe" --video | \
    ffmpeg -f rawvideo -pixel_format rgba -video_size 720x1600 -framerate 30 \
        -i pipe:0 -vf vflip -an -c:v libx264 -preset veryfast -crf 20 \
        -pix_fmt yuv420p -threads 2 -movflags +faststart /tmp/native-buttons-10s.mp4
```

This exports a fresh instance of the app's scene with a zero counter on the
local GPU. It does not capture the running Activity or Android's system UI.

Signing uses the debug key bundled with `rules_android`, so no local key or
preparation script is needed. This certificate differs from the previous
local demo key: Android cannot install it as an update over that older APK.
Uninstalling the older app removes its saved count.

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
