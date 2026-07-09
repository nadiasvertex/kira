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
        .message = std::format("failed to compile `{}` to native code: {}",
                               function_name, compiled.error().message)};
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

  const auto link_command = std::format(
      R"(c++ "{}" "{}" "{}" -o "{}")", object_path.string(),
      panic_archive->string(), heap_archive->string(), output_path.string());
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
