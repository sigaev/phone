# Repository instructions

- Use `.cc` for all C++ implementation files. Do not add `.cpp`, `.cxx`, or
  `.C` implementation files. Update Bazel targets and documentation when
  renaming sources.
- Always build, test, and run in optimized mode. Keep `build -c opt` in
  `.bazelrc` and rely on that default instead of repeating `-c opt` in
  commands. Do not override it with `dbg` or `fastbuild`.
- Use shorthand labels when the target name matches the package name:
  `bazel build //native_buttons`, without a redundant `:native_buttons`.
