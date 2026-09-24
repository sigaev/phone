# native_buttons

A small Android ARM64 counter written in C++23 using `NativeActivity`. Two
touch buttons increment and reset a counter saved in app-private storage.
The app requests no permissions and works offline.

The interface is drawn into `ANativeWindow`. Text uses the device's Roboto
font and the shared `//common/stb:stb_truetype` library, which includes its
license. The custom buttons do not expose accessibility nodes.

## Build

After the shared [toolchain setup](../README.md#toolchain-setup), run from
the workspace root:

```sh
bazel build //native_buttons
```

Output: `bazel-bin/native_buttons/native_buttons.apk`.
`bazel build //...` also builds the app and shared libraries.

`native_buttons_lib` compiles `main.cc` with `cc_library`. `alwayslink`
preserves the dynamically discovered `ANativeActivity_onCreate` entry point.
Compiler and linker flags come from `//tools:android.bzl`, including
C++23 and 16 KiB ELF segment alignment. The `native_buttons`
`android_binary` links `libnative_buttons.so`, processes the manifest, and
packages, aligns, and signs the APK. The manifest's `android.app.lib_name`
matches the shared library. The application ID is `dev.demo.nativebuttons`;
minimum Android API is 26 and target API is 35.

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
