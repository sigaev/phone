# Android Bazel workspace

The [native_buttons app](native_buttons/README.md) lives in `native_buttons/`.
Shared code and build infrastructure stay outside app directories:

- `common/` provides shared ownership, error handling, and Android runtime support.
- `common/gpu/` provides the shared EGL/OpenGL ES 3.2 renderer, mesh primitives,
  shadows, HDR, bloom, compute particles, and GPU text drawing.
- `BUILD.bazel` defines the shared `//:arm64-v8a` Android platform.
- `MODULE.bazel`, `.bazelrc`, and `tools/` configure the toolchains.
- `tools/android.bzl` exports `ANDROID_COPTS` and `ANDROID_LINKOPTS`
  for native apps to share C++23 compiler settings and Android linker flags.
- stb is fetched by Bazel from a pinned upstream commit, verified by SHA-256,
  and exposed as `@stb//:stb_truetype`. No third-party sources are vendored.
- libc++ sources are fetched from the LLVM revision recorded in NDK r29's
  `clang_source_info.md`, with a SHA-256 for every file. The
  `tools/libcxx.BUILD.bazel` overlay compiles the runtime components needed by
  the apps against the pinned NDK headers, with exceptions, RTTI, and unwind
  generation disabled. Add any additional compiled standard-library facilities
  to this overlay as apps need them.

Build the app from the workspace root:

```sh
bazel build //native_buttons
```

The APK is `bazel-bin/native_buttons/native_buttons.apk`. Bazel compiles, links,
packages, aligns, and signs it, including on a fresh checkout. There are no
build wrapper scripts, project `genrule` targets, or manually generated keys.

`//common:support` selects this libc++ runtime for Android. The build disables
the NDK's prebuilt C++ runtime and unwinder. `tools/no_unwind.ld` discards leftover
unwind tables from startup objects and rejects exception, unwinding, or demangler
entry points at link time. Recoverable application errors use `std::expected`;
`std::nothrow` allocations return null on failure, while ordinary allocation
failure and standard-library contract failures abort directly.

Use `clang-format` with the checked-in `.clang-format` for C++ and embedded
shaders, and `buildifier` for Bazel/Starlark files. Both are mandatory; see
[AGENTS.md](AGENTS.md) for formatting checks and the C++ API conventions.

Check the runtime on this phone with:

```sh
bazel test //common:runtime_test --platforms=//:arm64-v8a --run_under=//tools:android_test_runner
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

- Android SDK Custom release 35.0.2 (containing Build Tools 35.0.0), built for
  `aarch64-linux-musl` by HomuHomu833.
- Android NDK Custom r29, built for `aarch64-linux-musl` by the same project,
  targeting Android API 26.
- Google's Android SDK platform 35, revision 2.

The community builds replace Google's x86-64 host binaries. The patch in
`tools/patches/` adapts the upstream Bazel module's Linux host
constraints and NDK directory layout, and allows extracting platform-tools
from the combined SDK archive, whose optional `lib64` directory is absent.
NDK extraction skips bundled Python, shader tools, IDE utilities, and debugger
servers to fit the phone's storage. Extraction requires GNU `tar` with xz
support; compiler tools, headers, sysroots, and runtime libraries are retained.
Downloads remain pinned by SHA-256 and declared as Bazel toolchain inputs.
Compilation and linking use the downloaded NDK's compiler, headers, and sysroot.
Android supplies the runtime system libraries.

Bazel's Android rules also need a host C++ compiler supporting C++17 for their
build-time utilities. Their Java toolchains are downloaded by Bazel. Local
execution is enabled because PRoot cannot provide Linux namespace sandboxing.
The Java dependencies use the hermetic toolchain project's pinned Maven list
to avoid [rules_android's live-resolution issue](https://github.com/bazelbuild/rules_android/issues/485).
On this machine the host compiler is Debian GCC 14. Install it and the archive
utilities required by the Android rules with
`apt-get install g++-14 gcc-14 libc6-dev lld unzip zip`, then select it in
`.bazelrc.local`:

```text
common --repo_env=CC=/usr/bin/gcc-14
common --repo_env=CXX=/usr/bin/g++-14
```

Review the [Android SDK license](https://developer.android.com/studio/terms)
and [NDK license](https://developer.android.com/ndk/downloads). Once accepted,
put the version-specific settings required by the toolchain in the ignored
`.bazelrc.local` file:

```text
common --repo_env=ACCEPTED_ANDROID_SDK_LICENSE_VERSION=35
common --repo_env=ACCEPTED_ANDROID_NDK_LICENSE_VERSION=r29
```

The `//:arm64-v8a` platform name determines the native library's ABI directory
inside APKs. `MODULE.bazel.lock` is excluded from Git.

## References

- [Hermetic Android toolchains](https://github.com/keith/hermetic_android_toolchains/tree/0.4.0)
- [Bazel Android rules](https://github.com/bazelbuild/rules_android/tree/v0.7.3)
- [Bazel Android NDK rules](https://github.com/bazelbuild/rules_android_ndk/tree/v0.1.5)
- [Android page sizes](https://developer.android.com/guide/practices/page-sizes)
- [stb_truetype](https://github.com/nothings/stb/blob/master/stb_truetype.h)
- [ARM64 SDK archives](https://github.com/HomuHomu833/android-sdk-custom/releases/tag/35.0.2)
- [ARM64 NDK archives](https://github.com/HomuHomu833/android-ndk-custom/releases/tag/r29)
