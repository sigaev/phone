# Repository instructions

- Use `.cc` for all C++ implementation files. Do not add `.cpp`, `.cxx`, or
  `.C` implementation files. Update Bazel targets and documentation when
  renaming sources.
- Always build, test, and run in optimized mode. Keep `build -c opt` in
  `.bazelrc` and rely on that default instead of repeating `-c opt` in
  commands. Do not override it with `dbg` or `fastbuild`.
- Use shorthand labels when the target name matches the package name:
  `bazel build //native_buttons`, without a redundant `:native_buttons`.

## Commit messages

- Use a seven- or eight-word title that states the change.
- Follow the title with a blank line and one short paragraph describing the
  change and its purpose. Use a second short paragraph when useful, such as
  for relevant validation details.
- Do not include `Co-authored-by` trailers.

## Required formatting

- Use `clang-format` with the repository's `.clang-format` for all C++ headers
  and implementation files, including embedded shaders. Use `buildifier` for
  all Bazel and Starlark files, including `BUILD.bazel`, `MODULE.bazel`, `.bzl`
  files, and external-repository BUILD overlays.
- Install either formatter if it is missing. Run the formatters on every changed
  applicable file before finishing work; do not approximate their output by hand.
- Verify formatting with `clang-format --dry-run --Werror` and
  `buildifier -mode=check`. Do not format downloaded dependencies or generated
  build outputs.
- Use two-space indentation without tabs and readable, normally spaced statements.
  Short functions, blocks, and control flow may share a line as permitted by
  `.clang-format`. Separate function definitions with a blank line and omit
  comments on closing namespace braces.

## C++ style and API design

- Use `CamelCase` for classes, structs, and other type names; `snake_case` for
  functions, methods, parameters, and variables; and `kConstant` for constants,
  including enumerators. Keep externally mandated entry-point names unchanged.
- Use `#pragma once` in every C++ header.
- Use `std::expected<T, Error>` for operations that can fail and propagate errors
  explicitly. Do not throw exceptions or use exceptions for control flow.
- Relentlessly minimize public headers. Expose free functions operating on opaque,
  forward-declared objects; define those objects and their implementation details
  in `.cc` files. Headers should contain only the declarations and indispensable
  value types callers need, not implementation state or method-based object APIs.
- Manage each opaque object's lifetime with a `destroy()` free function. All
  owning code must use the shared owning helper in `common/`, which invokes that
  function. Raw pointers are non-owning except at explicit C API ownership
  transfer boundaries; recover an owner immediately when ownership returns.
- Do not vendor third-party source or binaries. Declare dependencies using Bazel
  modules or repository download rules, pinned to immutable versions or commits
  with verified checksums. Bazel must fetch them as part of the build. Keep only
  integration rules and necessary patches in this repository.
