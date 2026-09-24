"""Compile Vulkan shaders with the pinned NDK, without runtime shader compilers."""

def _spirv_shader_impl(ctx):
    binary = ctx.actions.declare_file(ctx.label.name + ".spv")
    raw_include = ctx.actions.declare_file(ctx.label.name + ".raw.inc")
    include = ctx.actions.declare_file(ctx.label.name + ".inc")
    for output, extra in [(binary, []), (raw_include, ["-mfmt=c"])]:
        ctx.actions.run(
            executable = ctx.executable._compiler,
            inputs = [ctx.file.src],
            outputs = [output],
            arguments = ["--target-env=vulkan1.1", "-Os"] + extra + [ctx.file.src.path, "-o", output.path],
            mnemonic = "VulkanShader",
            progress_message = "Compiling Vulkan shader %{input}",
        )
    ctx.actions.run_shell(
        inputs = [binary, raw_include],
        tools = [ctx.executable._validator],
        outputs = [include],
        arguments = [ctx.executable._validator.path, binary.path, raw_include.path, include.path],
        command = '"$1" --target-env vulkan1.1 "$2" && cp "$3" "$4"',
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
