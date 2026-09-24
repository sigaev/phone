"""Shared compiler and linker settings for native Android apps."""

ANDROID_COPTS = [
    "-std=c++23",
    "-fvisibility=hidden",
    "-D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS",
    "-fvisibility-global-new-delete-hidden",
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
]

ANDROID_LINKOPTS = [
    "-landroid",
    "-llog",
    "-lm",
    "-Wl,--no-undefined",
    # android_binary's native link does not honor --strip=always completely.
    "-Wl,--strip-all",
    # //common:support supplies libc++ built without exceptions or unwinding.
    "-nostdlib++",
    "--unwindlib=none",
    "-Wl,-z,start-stop-visibility=hidden",
    # Support devices with 16 KiB memory pages.
    "-Wl,-z,max-page-size=16384",
    "-Wl,-z,common-page-size=16384",
]

def _android_test_runner_impl(ctx):
    runner = ctx.actions.declare_file(ctx.label.name)
    layer_setup = ""
    if ctx.file.validation_layer:
        layer_relative = ctx.file.validation_layer.short_path.removeprefix("../")
        layer_setup = """
layer_relative="{layer}"
for root in "${{RUNFILES_DIR:-}}" "${{TEST_SRCDIR:-}}" "$0.runfiles"; do
    if [ -f "$root/$layer_relative" ]; then
        cp "$root/$layer_relative" "$run_directory/libVkLayer_khronos_validation.so"
        break
    fi
done
test -f "$run_directory/libVkLayer_khronos_validation.so"
export NATIVE_BUTTONS_VULKAN_LAYER_PATH="$run_directory"
""".format(layer = layer_relative)
    ctx.actions.write(
        output = runner,
        is_executable = True,
        content = """#!/bin/sh
set -eu
test_binary="$1"
shift
case "$test_binary" in
    /*) ;;
    *) test_binary="$PWD/$test_binary" ;;
esac
# Android chooses linker namespaces from the executable path. A real /data
# path is required to expose the device's Vulkan driver under PRoot.
run_directory=$(mktemp -d /data/data/com.termux/files/usr/tmp/native-buttons-test.XXXXXX)
trap 'rm -rf "$run_directory"' EXIT HUP INT TERM
cp "$test_binary" "$run_directory/test"
chmod 0700 "$run_directory/test"
export LD_LIBRARY_PATH=/apex/com.android.i18n/lib64:/apex/com.android.runtime/lib64/bionic:/system/lib64
{layer_setup}
/system/bin/linker64 "$run_directory/test" "$@"
""".replace("{layer_setup}", layer_setup),
    )
    return [DefaultInfo(executable = runner, runfiles = ctx.runfiles(files = [ctx.file.validation_layer] if ctx.file.validation_layer else []))]

android_test_runner = rule(
    implementation = _android_test_runner_impl,
    executable = True,
    attrs = {"validation_layer": attr.label(allow_single_file = [".so"])},
    doc = "Runs Android tests on this phone with an absolute executable path and system libraries.",
)
