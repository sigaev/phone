"""Build Chromecast tests with the TLS server needed by their fake devices."""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

def _test_config_impl(_settings, _attr):
    return {"@mbedtls//:mbedtls_config": "//chromecast:mbedtls_test_config"}

_test_config = transition(
    implementation = _test_config_impl,
    inputs = [],
    outputs = ["@mbedtls//:mbedtls_config"],
)

def _mbedtls_test_impl(ctx):
    binary = ctx.attr.binary[DefaultInfo]
    executable = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(
        output = executable,
        target_file = ctx.executable.binary,
        is_executable = True,
    )
    return [DefaultInfo(executable = executable, runfiles = binary.default_runfiles)]

_mbedtls_test = rule(
    implementation = _mbedtls_test_impl,
    test = True,
    cfg = _test_config,
    # The Android runner from .bazelrc executes the binary from the Linux host.
    exec_groups = {"test": exec_group()},
    attrs = {
        "binary": attr.label(executable = True, cfg = "target", mandatory = True),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)

def mbedtls_cc_test(name, size, **kwargs):
    """A native test whose entire dependency graph uses the test TLS config."""
    cc_binary(
        name = name + "_binary",
        testonly = True,
        tags = ["manual"],
        **kwargs
    )
    _mbedtls_test(
        name = name,
        size = size,
        binary = ":" + name + "_binary",
        target_compatible_with = kwargs.get("target_compatible_with", []),
    )
