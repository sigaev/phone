"""Shared compiler and linker settings for native Android apps."""

ANDROID_COPTS = [
    "-std=c++23",
    "-fvisibility=hidden",
    "-fno-exceptions",
    "-fno-rtti",
]

ANDROID_LINKOPTS = [
    "-landroid",
    "-llog",
    "-lm",
    "-Wl,--no-undefined",
    # Support devices with 16 KiB memory pages.
    "-Wl,-z,max-page-size=16384",
    "-Wl,-z,common-page-size=16384",
]
