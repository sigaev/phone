"""Compile Vulkan shaders at build time, without runtime shader compilers."""

def _glslang_shader_impl(ctx):
    unoptimized = ctx.actions.declare_file(ctx.label.name + ".glslang.spv")
    binary = ctx.actions.declare_file(ctx.label.name + ".spv")
    include = ctx.actions.declare_file(ctx.label.name + ".inc")
    ctx.actions.run(
        executable = ctx.executable._compiler,
        inputs = [ctx.file.src] + ctx.files.hdrs,
        outputs = [unoptimized],
        arguments = ["-V", "--quiet", "--target-env", "vulkan1.4"] +
                    ["-D" + define for define in ctx.attr.defines] +
                    [ctx.file.src.path, "-o", unoptimized.path],
        mnemonic = "VulkanShader",
        progress_message = "Compiling Vulkan shader %{label}",
    )
    ctx.actions.run(
        executable = ctx.executable._optimizer,
        inputs = [unoptimized],
        outputs = [binary],
        arguments = ["--target-env=vulkan1.4", "-Os", unoptimized.path, "-o", binary.path],
        mnemonic = "OptimizeSpirv",
        progress_message = "Optimizing Vulkan shader %{label}",
    )

    # Write the validated words as a braced C++ array initializer. od reads them
    # in the host's byte order, in which spirv-opt wrote them.
    ctx.actions.run_shell(
        inputs = [binary],
        tools = [ctx.executable._validator],
        outputs = [include],
        arguments = [ctx.executable._validator.path, binary.path, include.path],
        command = '"$1" --target-env vulkan1.4 "$2" && ' +
                  '{ echo "{"; od -An -v -tx4 "$2" | sed "s/ *\\([0-9a-f]\\{8\\}\\)/0x\\1,/g"; echo "}"; } > "$3"',
        mnemonic = "ValidateSpirv",
    )
    return [DefaultInfo(files = depset([include])), OutputGroupInfo(spirv = depset([binary]))]

# Vulkan 1.4 shaders compiled with Khronos's glslang, then optimized for size and
# validated with SPIRV-Tools, as the NDK's glslc -Os does.
glslang_shader = rule(
    implementation = _glslang_shader_impl,
    attrs = {
        "src": attr.label(allow_single_file = [".vert", ".frag", ".comp"], mandatory = True),
        "hdrs": attr.label_list(allow_files = [".glsl"]),
        "defines": attr.string_list(),
        "_optimizer": attr.label(default = Label("@spirv_tools//:spirv-opt"), executable = True, cfg = "exec"),
        "_validator": attr.label(default = Label("@spirv_tools//:spirv-val"), executable = True, cfg = "exec"),
        "_compiler": attr.label(default = Label("@glslang//:glslang_validator"), executable = True, cfg = "exec"),
    },
)
