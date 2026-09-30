# Android Bazel workspace

The [Native Buttons app](native_buttons/README.md) lives in `native_buttons/`,
the [Sudoku app](sudoku/README.md) in `sudoku/`, and the
[GEMM benchmark](gemm/README.md), which measures this phone's GPU with Vulkan
compute, in `gemm/`. Shared code and build infrastructure stay outside app
directories:

- `common/` provides the `Owner` helper that destroys opaque objects, `Result`
  and `Error` for `std::expected` error handling, and Android runtime support.
- `common/gpu/` provides the shared Vulkan 1.4 renderer, mesh primitives,
  shadows, HDR, bloom, compute particles, and GPU text drawing. Its overlay
  mode draws only multisampled 2D shapes and text, for flat interfaces.
- `tools/spirv.bzl` compiles every shader during the Bazel build with Khronos's
  glslang 16.6.0 (fetched with a SHA-256 and built by
  `tools/glslang.BUILD.bazel`), then optimizes the SPIR-V with `spirv-opt -Os`
  and validates it with `spirv-val`, both from SPIRV-Tools 2026.1 in the Bazel
  Central Registry. Unlike the NDK's shader tools, they support the GEMM
  benchmark's Vulkan 1.4 cooperative matrices and integer dot products. No
  shader compiler is linked into the apps.
- `BUILD.bazel` defines the shared `//:arm64-v8a` Android platform, which
  `.bazelrc` selects for every build.
- `MODULE.bazel`, `.bazelrc`, and `tools/` configure the toolchains.
- `.bazelrc` sets the compiler and linker flags for all Android code:
  optimized C++23 for ARMv9-A without exceptions, RTTI, or unwind tables,
  hidden symbols, full link-time optimization across all native libraries,
  16 KiB page alignment, stripped output, and links to libandroid, liblog, and
  libm. Targets add only their own flags, such as `-lvulkan` in `common/gpu`.
  It also runs Bazel in batch mode with a 768 MiB JVM heap and at most four
  jobs to stay within the phone's memory.
- `tools/android.bzl` defines the on-device runner that `.bazelrc` uses for
  every `bazel test` and `bazel run`. It copies each binary into Termux's
  `/data/data/com.termux/files/usr/tmp` and starts it with Android's
  `linker64`, so the Vulkan loader can reach the GPU driver.
- `--config=vulkan_validation` compiles the renderer and GPU tests with
  Khronos API and synchronization validation and runs them through
  `//tools:vulkan_validation_runner`, which supplies the Android validation
  layer from Khronos's pinned 1.4.321.0 release, verified by SHA-256. APKs
  never include the layer.
- The NDK's Vulkan headers predate 1.4, so Khronos's Vulkan-Headers come from
  the Bazel Central Registry's `vulkan_headers` module.
- stb is fetched by Bazel from a pinned upstream commit, verified by SHA-256,
  and exposed as `@stb//:stb_truetype`. No third-party sources are vendored.
- `tools/libcxx.bzl` fetches libc++ sources from the LLVM revision recorded in
  NDK r29's `clang_source_info.md`, with a SHA-256 for every file. The
  `tools/libcxx.BUILD.bazel` overlay compiles `algorithm`, `chrono`, `new`,
  `new_handler`, `new_helpers`, `string`, `system_error`, and `verbose_abort`
  against the pinned NDK headers with the `.bazelrc` flags, and adds
  `tools/no_unwind.ld` to every link that uses it. Add any additional compiled
  standard-library facilities to this overlay as apps need them.

Build the apps from the workspace root:

```sh
bazel build //native_buttons //sudoku
```

The APKs are `bazel-bin/native_buttons/native_buttons.apk` and
`bazel-bin/sudoku/sudoku.apk`. Bazel compiles, links, packages, aligns, and
signs each APK with rules_android's debug key, including on a fresh checkout
once `.bazelrc.local` holds the settings below. There are no
build wrapper scripts, project `genrule` targets, or manually generated keys.
Both apps require Android 16 (API 36), the ARMv9-A CPU of this phone, a Pixel 8
Pro, and its Vulkan 1.4 GPU; there are no code paths for older releases or
hardware. API 36 verifies APK Signature Scheme v3, so `.bazelrc` omits v1 and v2
signatures. Its `v4` signing method also writes a v4 `.idsig` beside each APK.

`//common:support` selects this libc++ runtime for Android. The build disables
the NDK's prebuilt C++ runtime and unwinder. `tools/no_unwind.ld` discards leftover
unwind tables from startup objects and rejects exception, unwinding, or demangler
entry points at link time. Recoverable application errors use `std::expected`;
`std::nothrow` allocations return null on failure, while ordinary allocation
failure and standard-library errors that would otherwise throw, such as
`std::length_error`, abort directly. libc++ hardening keeps the NDK default,
which is off.

Use `clang-format` with the checked-in `.clang-format` for C++ and embedded
shaders, and `buildifier` for Bazel/Starlark files. Both are mandatory; see
[AGENTS.md](AGENTS.md) for formatting checks and the C++ API conventions.

Check the runtime on this phone with:

```sh
bazel test //common:runtime_test
```

The test checks allocation failure, allocator handlers, alignment, standard-library
operations, and direct aborts for unrecoverable errors. Bazel generates the Android
test runner; no preparation script is needed.

## Toolchain setup

Use Bazel 9.2.0 (pinned in `.bazelversion`) or Bazelisk in this phone's
Debian/PRoot environment. Both the build host and the APK target are ARM64.
Allow several GiB of free space for the toolchains and first build. Subsequent
builds reuse Bazel's cache.

`MODULE.bazel` uses `hermetic_android_toolchains` 0.4.0 from Bazel Central
Registry with pinned, checksum-verified ARM64 archives:

- Android SDK Custom release 37.0.0 (containing Build Tools 37.0.0), built for
  `aarch64-linux-musl` by HomuHomu833.
- Android NDK Custom r29, built for `aarch64-linux-musl` by the same project,
  targeting its newest sysroot, Android API 35. Google's r30 has no ARM64 Linux
  host build, and this project offers only an r30 beta.
- Google's Android SDK platform 37.2, revision 1. The apps compile against it
  and target API 36.

The community builds replace Google's x86-64 host binaries. The patch in
`tools/patches/` adapts the upstream Bazel module's Linux host and Java
toolchain constraints and NDK directory layout. It also adds a platform-tools
strip prefix for the combined SDK archive, whose Build Tools have no optional
`lib64` directory. NDK extraction skips the shader tools, bundled Python,
clangd, clang-tidy, BOLT, other unused LLVM analysis and debug-info
tools, and `lldb-server` to fit the phone's storage. Extraction requires GNU
`tar` and `xz`; compiler tools, headers, sysroots, and runtime libraries are
retained.
Downloads remain pinned by SHA-256 and declared as Bazel toolchain inputs.
Compilation and linking use the downloaded NDK's compiler, headers, and sysroot.
Android supplies the runtime system libraries.

Bazel's Android rules also need a host C++ compiler supporting C++17 for their
build-time utilities. Their Java toolchains are downloaded by Bazel. Local
execution is enabled because PRoot cannot provide Linux namespace sandboxing.
The Java dependencies use the hermetic toolchain project's pinned Maven list
to avoid [rules_android's live-resolution issue](https://github.com/bazelbuild/rules_android/issues/485).
On this machine the host compiler is Debian GCC 14. Install it, the lld linker,
and the archive utilities with
`apt-get install g++-14 gcc-14 libc6-dev lld unzip xz-utils zip`, then select
it in `.bazelrc.local`:

```text
common --repo_env=CC=/usr/bin/gcc-14
common --repo_env=CXX=/usr/bin/g++-14
```

Review the [Android SDK license](https://developer.android.com/studio/terms)
and [NDK license](https://developer.android.com/ndk/downloads). Once accepted,
put the version-specific settings required by the toolchain in the ignored
`.bazelrc.local` file:

```text
common --repo_env=ACCEPTED_ANDROID_SDK_LICENSE_VERSION=37.2
common --repo_env=ACCEPTED_ANDROID_NDK_LICENSE_VERSION=r29
```

The `//:arm64-v8a` platform name determines the native library's ABI directory
inside APKs. `MODULE.bazel.lock` is excluded from Git.

## References

- [Hermetic Android toolchains](https://github.com/keith/hermetic_android_toolchains/tree/0.4.0)
- [Bazel Android rules](https://github.com/bazelbuild/rules_android/tree/v0.7.3)
- [Bazel Android NDK rules](https://github.com/bazelbuild/rules_android_ndk/tree/v0.1.5)
- [Android page sizes](https://developer.android.com/guide/practices/page-sizes)
- [stb_truetype](https://github.com/nothings/stb/blob/6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760/stb_truetype.h)
- [Vulkan validation layers](https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/tag/vulkan-sdk-1.4.321.0)
- [ARM64 SDK archives](https://github.com/HomuHomu833/android-sdk-custom/releases/tag/37.0.0)
- [ARM64 NDK archives](https://github.com/HomuHomu833/android-ndk-custom/releases/tag/r29)
