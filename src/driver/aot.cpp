#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "aot.h"
#include "src/hir/lower.h"
#include "src/llvm_codegen/aot.h"
#include "src/llvm_codegen/codegen.h"
#include "src/util/path.h"

namespace fs = std::filesystem;

namespace kira::driver {

[[nodiscard]] auto find_bazel_archive(std::string_view program_name,
                                      std::string_view bazel_package,
                                      std::string_view library_name)
    -> std::optional<fs::path> {
  auto candidates = std::vector<fs::path>{};
  for (const auto *extension : {"lo", "a"}) {
    const auto filename = std::format("lib{}.{}", library_name, extension);
    if (!program_name.empty()) {
      candidates.emplace_back(
          fs::path(std::format("{}.runfiles", program_name)) / "_main" /
          bazel_package / filename);
    }
    if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
      if (const auto *workspace = std::getenv("TEST_WORKSPACE");
          workspace != nullptr && *workspace != '\0') {
        candidates.emplace_back(fs::path(srcdir) / workspace / bazel_package /
                                filename);
      }
      candidates.emplace_back(fs::path(srcdir) / "_main" / bazel_package /
                              filename);
    }
    candidates.emplace_back(fs::path("bazel-bin") / bazel_package / filename);
    if (const auto exe_path = kira::util::resolve_self_executable()) {
      // Installed layout: `<prefix>/bin/kira`, link archives flattened
      // under `<prefix>/lib/kira/` (matching `just package`'s tar/`.deb`
      // output), independent of the AOT program's cwd.
      candidates.emplace_back(exe_path->parent_path().parent_path() / "lib" /
                              "kira" / filename);
    }
  }

  for (const auto &candidate : candidates) {
    auto ec = std::error_code{};
    if (fs::exists(candidate, ec)) {
      return candidate;
    }
  }
  return std::nullopt;
}

[[nodiscard]] auto
build_hir_module(std::span<const hir::hir_module *const> modules,
                 const semantic::type_table &types,
                 std::string_view function_name, const fs::path &output_path,
                 std::string_view program_name) -> build_outcome {
  auto compiled = llvm_codegen::compile_module(modules, types);
  if (!compiled) {
    return build_outcome{
        .succeeded = false,
        .message = std::format("failed to compile `{}` to native code (byte "
                               "offset {}): {}",
                               function_name, compiled.error().span.start,
                               compiled.error().message)};
  }

  const auto object_path =
      fs::temp_directory_path() /
      std::format("kira-build-{}.o", static_cast<long>(::getpid()));
  auto emitted = llvm_codegen::emit_object_file(
      std::move(*compiled), function_name, object_path.string());
  if (!emitted) {
    return build_outcome{.succeeded = false,
                         .message = std::format("failed to emit an object "
                                                "file for `{}`: {}",
                                                function_name,
                                                emitted.error().message)};
  }

  const auto panic_archive =
      find_bazel_archive(program_name, "src/llvm_codegen", "aot_runtime");
  if (!panic_archive) {
    return build_outcome{
        .succeeded = false,
        .message = "could not locate Kira's AOT panic runtime support "
                   "library (libaot_runtime.a) — run `kira` via `bazelisk "
                   "run //src:kira` or from `bazel-bin/src/kira` inside the "
                   "workspace that built it"};
  }
  const auto heap_archive =
      find_bazel_archive(program_name, "src/runtime", "runtime");
  if (!heap_archive) {
    return build_outcome{
        .succeeded = false,
        .message = "could not locate Kira's heap runtime support library "
                   "(libruntime.a) — run `kira` via `bazelisk run "
                   "//src:kira` or from `bazel-bin/src/kira` inside the "
                   "workspace that built it"};
  }
  // `src/runtime:runtime`'s own `layout.cpp` (`struct_field_slot` and
  // friends — compile-time-only helpers `codegen.cpp` calls to compute a
  // struct/sum type's heap layout, never called by the *generated* code
  // itself) depends on `semantic::type_table`, so its own Bazel target
  // `deps` on `//src/semantic` and (transitively) `//src/parser` — but a
  // hand-rolled archive link like this one doesn't follow `cc_library`
  // `deps` the way Bazel's own linker invocation does, so those two have to
  // be found and passed explicitly too, or any AOT program touching a
  // struct/sum type (which is to say, nearly everything past a bare
  // scalar) fails to link with `type_table::entry` undefined.
  const auto semantic_archive =
      find_bazel_archive(program_name, "src/semantic", "semantic");
  if (!semantic_archive) {
    return build_outcome{
        .succeeded = false,
        .message = "could not locate Kira's semantic-analysis support "
                   "library (libsemantic.a, needed by the heap runtime's "
                   "struct/sum layout helpers) — run `kira` via `bazelisk "
                   "run //src:kira` or from `bazel-bin/src/kira` inside the "
                   "workspace that built it"};
  }
  const auto parser_archive =
      find_bazel_archive(program_name, "src/parser", "parser");
  if (!parser_archive) {
    return build_outcome{
        .succeeded = false,
        .message = "could not locate Kira's parser support library "
                   "(libparser.a, needed by the heap runtime's struct/sum "
                   "layout helpers) — run `kira` via `bazelisk run "
                   "//src:kira` or from `bazel-bin/src/kira` inside the "
                   "workspace that built it"};
  }
  // `src/runtime:fmt.cpp` (string-interpolation formatting intrinsics,
  // `spec/string-formatting-design.md`) calls `//src/utf8`'s scalar encode/
  // decode helpers directly, same "hand-rolled link doesn't follow Bazel
  // `deps`" reasoning as `semantic_archive`/`parser_archive` above.
  const auto utf8_archive =
      find_bazel_archive(program_name, "src/utf8", "utf8");
  if (!utf8_archive) {
    return build_outcome{
        .succeeded = false,
        .message = "could not locate Kira's UTF-8 support library "
                   "(libutf8.a, needed by the heap runtime's string-"
                   "formatting intrinsics) — run `kira` via `bazelisk run "
                   "//src:kira` or from `bazel-bin/src/kira` inside the "
                   "workspace that built it"};
  }

  const auto link_command = std::format(
      R"(c++ "{}" "{}" "{}" "{}" "{}" "{}" -o "{}")", object_path.string(),
      panic_archive->string(), heap_archive->string(),
      semantic_archive->string(), parser_archive->string(),
      utf8_archive->string(), output_path.string());
  const auto link_status = std::system(link_command.c_str());
  auto ec = std::error_code{};
  fs::remove(object_path, ec);
  if (link_status != 0) {
    return build_outcome{
        .succeeded = false,
        .message = std::format("linking failed (exit status {})", link_status)};
  }

  return build_outcome{.succeeded = true, .message = output_path.string()};
}

} // namespace kira::driver
