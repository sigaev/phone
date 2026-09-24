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
export LD_LIBRARY_PATH=/apex/com.android.i18n/lib64:/apex/com.android.runtime/lib64/bionic:/system/lib64
exec /system/bin/linker64 "$test_binary" "$@"
""",
    )
    return [DefaultInfo(executable = runner)]

android_test_runner = rule(
    implementation = _android_test_runner_impl,
    executable = True,
    doc = "Runs Android tests on this phone with an absolute executable path and system libraries.",
)
