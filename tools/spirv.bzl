"""Compile Vulkan shaders at build time, without runtime shader compilers."""

def _spirv_shader_impl(ctx):
    binary = ctx.actions.declare_file(ctx.label.name + ".spv")
    raw_include = ctx.actions.declare_file(ctx.label.name + ".raw.inc")
    include = ctx.actions.declare_file(ctx.label.name + ".inc")

    # The NDK shader tools stop at Vulkan 1.3, whose SPIR-V 1.6 is also Vulkan 1.4's.
    for output, extra in [(binary, []), (raw_include, ["-mfmt=c"])]:
        ctx.actions.run(
            executable = ctx.executable._compiler,
            inputs = [ctx.file.src],
            outputs = [output],
            arguments = ["--target-env=vulkan1.3", "-Os"] + extra + [ctx.file.src.path, "-o", output.path],
            mnemonic = "VulkanShader",
            progress_message = "Compiling Vulkan shader %{input}",
        )
    ctx.actions.run_shell(
        inputs = [binary, raw_include],
        tools = [ctx.executable._validator],
        outputs = [include],
        arguments = [ctx.executable._validator.path, binary.path, raw_include.path, include.path],
        command = '"$1" --target-env vulkan1.3 "$2" && cp "$3" "$4"',
        mnemonic = "ValidateSpirv",
    )
    return [DefaultInfo(files = depset([include])), OutputGroupInfo(spirv = depset([binary]))]

spirv_shader = rule(
    implementation = _spirv_shader_impl,
    attrs = {
        "src": attr.label(allow_single_file = [".vert", ".frag", ".comp"], mandatory = True),
        "_validator": attr.label(default = Label("@androidndk//:spirv_val"), executable = True, cfg = "exec", allow_single_file = True),
        "_compiler": attr.label(default = Label("@androidndk//:glslc"), executable = True, cfg = "exec", allow_single_file = True),
    },
)

def _glslang_shader_impl(ctx):
    binary = ctx.actions.declare_file(ctx.label.name + ".spv")
    raw_include = ctx.actions.declare_file(ctx.label.name + ".raw.inc")
    include = ctx.actions.declare_file(ctx.label.name + ".inc")
    arguments = ["-V", "--quiet", "--target-env", "vulkan1.4"] + ["-D" + define for define in ctx.attr.defines]
    for output, extra in [(binary, []), (raw_include, ["-x"])]:
        ctx.actions.run(
            executable = ctx.executable._compiler,
            inputs = [ctx.file.src] + ctx.files.hdrs,
            outputs = [output],
            arguments = arguments + extra + [ctx.file.src.path, "-o", output.path],
            mnemonic = "VulkanShader",
            progress_message = "Compiling Vulkan shader %{label}",
        )

    # The hexadecimal output lacks the braces that glslc's C format includes.
    ctx.actions.run_shell(
        inputs = [binary, raw_include],
        tools = [ctx.executable._validator],
        outputs = [include],
        arguments = [ctx.executable._validator.path, binary.path, raw_include.path, include.path],
        command = '"$1" --target-env vulkan1.4 "$2" && { echo "{"; cat "$3"; echo "}"; } > "$4"',
        mnemonic = "ValidateSpirv",
    )
    return [DefaultInfo(files = depset([include])), OutputGroupInfo(spirv = depset([binary]))]

# Vulkan 1.4 shaders compiled with Khronos's glslang and validated with SPIRV-Tools,
# for features newer than the NDK's shader tools, such as cooperative matrices.
glslang_shader = rule(
    implementation = _glslang_shader_impl,
    attrs = {
        "src": attr.label(allow_single_file = [".vert", ".frag", ".comp"], mandatory = True),
        "hdrs": attr.label_list(allow_files = [".glsl"]),
        "defines": attr.string_list(),
        "_validator": attr.label(default = Label("@spirv_tools//:spirv-val"), executable = True, cfg = "exec"),
        "_compiler": attr.label(default = Label("@glslang//:glslang_validator"), executable = True, cfg = "exec"),
    },
)
