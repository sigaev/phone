"""Download only the libc++ sources needed by the exception-free Android runtime."""

# Match NDK r29's clang r563880c headers; update these pins with the NDK.
_LLVM_REVISION = "386af4a5c64ab75eaee2448dc38f2e34a40bfed0"
_SOURCES = {
    "LICENSE.TXT": "539dd7aed86e8a4f12cbdd0e6c50c189c7d74847e4fecc64ce2c6ee3a01da38b",
    "src/chrono.cpp": "9f5acabff11905fd20100c6fb7afa3450b1e9dc5b2279302a5a544e55defe361",
    "src/new.cpp": "6c32fe75863232c72b69de45bf397a9af2a7b624812e060e7036270421fff881",
    "src/new_handler.cpp": "0b17697b569d4a4cc9b3c2f97ef69fdf8d6cac72de9871489456c5d9235b1bbf",
    "src/new_helpers.cpp": "82b6b57506edc48dcbb13eaa5bf533e680167f983a2a2cf85064c003cb8cdb38",
    "src/string.cpp": "e00d2ba81df72a1070a5aa7571efc9b2f3779859685280358a806f3c1a7a9ce5",
    "src/system_error.cpp": "82b3bf2367d76f7a5fde7824b53289fd55115794fd01ad1976a02247e50187e2",
    "src/verbose_abort.cpp": "30c556a4192eac151be428caeda401fcbe0ee211a2fedbabb56a70e8b3775da9",
    "src/include/apple_availability.h": "5e4920e7d642b44c64c487c54e3ad71628cc5c59be5ab43043ba20c279ce6d97",
    "src/include/atomic_support.h": "c01d71b5a8fe48773f3670166030284b1050413e22f24bf21bb8c64637036f9e",
    "src/include/config_elast.h": "066c960d0c2741d5bb164816d5583bf92a40dd6f690fa567e3cc8a7ebd4e269c",
    "src/include/overridable_function.h": "d570d29380c99a63610e41874534e589910dfbac1792e681e897b0c3cf2d577b",
}

def _libcxx_runtime_impl(rctx):
    build_file = rctx.read(rctx.attr.build_file)
    for path, sha256 in _SOURCES.items():
        rctx.download(
            url = "https://raw.githubusercontent.com/llvm/llvm-project/{}/libcxx/{}".format(_LLVM_REVISION, path),
            output = path,
            sha256 = sha256,
        )
    rctx.file("BUILD.bazel", build_file)
    rctx.file("REPO.bazel", "")

libcxx_runtime = repository_rule(
    implementation = _libcxx_runtime_impl,
    attrs = {
        "build_file": attr.label(
            default = Label("//tools:libcxx.BUILD.bazel"),
            allow_single_file = True,
        ),
    },
)
