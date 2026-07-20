#include "driver.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <system_error>

#include "lowering_stage.h"
#include "parse_stage.h"
#include "run_build_stage.h"
#include "src/semantic/analysis.h"
#include "src/util/path.h"
#include "src/util/str.h"
#include "src/version.h"
#include "static_if_stage.h"

using kira::source_manager;
using kira::util::append_text;
using kira::util::normalize_path;

namespace kira::driver {

namespace {

/// The `@architecture` variant constant matching this compiler binary's own
/// build host CPU. Kira has no `--target` flag yet (no cross-compilation),
/// so "target" and "build host" are the same thing for now — `TARGET_ARCH`
/// et al. (spec/std-reference.md) describe whatever this binary was itself
/// compiled for.
[[nodiscard]] constexpr auto detect_target_arch() -> std::string_view {
#if defined(__x86_64__) || defined(_M_X64)
  return "@x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "@aarch64";
#elif defined(__arm__) || defined(_M_ARM)
  return "@arm";
#elif defined(__riscv) && __riscv_xlen == 64
  return "@riscv64";
#elif defined(__riscv) && __riscv_xlen == 32
  return "@riscv32";
#elif defined(__wasm64__)
  return "@wasm64";
#elif defined(__wasm32__)
  return "@wasm32";
#else
  return "@other(\"unknown\")";
#endif
}

struct target_os_info {
  std::string_view os;
  std::string_view family;
  std::string_view vendor;
  std::string_view env;
};

[[nodiscard]] constexpr auto detect_target_os() -> target_os_info {
#if defined(_WIN32)
  return {"windows", "windows", "pc", "msvc"};
#elif defined(__APPLE__)
  return {"macos", "macos", "apple", "none"};
#elif defined(__linux__)
#if defined(__GLIBC__)
  return {"linux", "unix", "unknown", "gnu"};
#else
  return {"linux", "unix", "unknown", "musl"};
#endif
#elif defined(__FreeBSD__)
  return {"freebsd", "unix", "unknown", "none"};
#elif defined(__OpenBSD__)
  return {"openbsd", "unix", "unknown", "none"};
#elif defined(__NetBSD__)
  return {"netbsd", "unix", "unknown", "none"};
#else
  return {"unknown", "unknown", "unknown", "none"};
#endif
}

[[nodiscard]] constexpr auto detect_target_endian() -> std::string_view {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return "@big";
#else
  return "@little";
#endif
}

/// Generates `module std.platform`'s target/build-info accessor functions
/// (spec/std-reference.md "Compile-Time Target Constants") as Kira source
/// text, with no `module` line of its own. This is the one piece of
/// `std.platform` that cannot be a checked-in `.kira` file: the values
/// describe the compiler binary's own build host, so they are computed here
/// from C++ preprocessor macros each time the driver runs and spliced into
/// the checked-in `platform.kira` body (see `assemble_platform_module_source`).
///
/// Each value is embedded directly as the accessor function's own return
/// literal (`pub pure def target_os() -> str: "macos"`) rather than routed
/// through a `pub static` the function body then reads by name — a bare
/// reference to a non-scalar (`str`, sum-type, struct, ...) top-level
/// `static` lowers to an `hir_local_ref` the bytecode compiler can't
/// resolve ("reference to `X` is not a local binding"), a pre-existing gap
/// confirmed with a two-line repro (`type t = @a | @b`, `static X: t = @a`,
/// `def f() -> t: X`, called from `main`) — only genuinely scalar statics
/// (e.g. plain `int32`) survive that path. Embedding the literal in the
/// function body itself sidesteps the bug entirely, since it never becomes
/// a reference to a binding at all.
///
/// `target_os_family()` in particular is generated here rather than
/// classified from `target_os()`'s string at runtime, because neither
/// backend implements `str` `==` yet ("type `str` has no scalar
/// bytecode/llvm_codegen representation yet ... until spec/codegen-design.md
/// increment 6 (heap types) lands", confirmed with a two-line repro) — the
/// classification the checked-in file would otherwise compute at runtime is
/// done here in C++ instead, from the exact same `target_os_info` used for
/// `target_os()` itself, so the two can't disagree.
[[nodiscard]] auto generate_platform_target_accessors() -> std::string {
  const auto os = detect_target_os();
  const auto os_family_variant =
      os.family == "unix"      ? std::string("@unix")
      : os.family == "windows" ? std::string("@windows")
      : os.family == "macos"   ? std::string("@macos")
                               : std::format("@other(\"{}\")", os.family);
  return std::format("pub pure def target_arch() -> architecture: {}\n"
                     "pub pure def target_os() -> str: \"{}\"\n"
                     "pub pure def target_os_family() -> os_family: {}\n"
                     "pub pure def target_vendor() -> str: \"{}\"\n"
                     "pub pure def target_env() -> str: \"{}\"\n"
                     "pub pure def target_endianness() -> endianness: {}\n"
                     "pub pure def target_pointer_width() -> usize: {}\n"
                     "\n"
                     "pub pure def kira_version() -> str: \"{}\"\n"
                     "pub pure def kira_implementation() -> str: \"kira\"\n"
                     "pub pure def kira_compiler() -> str: \"llvm\"\n"
                     "pub pure def kira_build_date() -> str: \"{}\"\n"
                     "\n",
                     detect_target_arch(), os.os, os_family_variant, os.vendor,
                     os.env, detect_target_endian(), sizeof(void *),
                     k_version_string, k_release_date);
}

/// Reads the checked-in `platform_source_path` (`src/std/platform.kira`),
/// strips its leading `module std.platform` line, and splices the
/// driver-generated target/build-info accessors (see
/// `generate_platform_target_accessors`'s doc comment for why they can't be
/// a separate module or a `static` the rest of the file reads by name) in
/// right after a single `module std.platform` line. Returns `nullopt` if
/// the file can't be read or doesn't start with the expected module
/// declaration.
[[nodiscard]] auto assemble_platform_module_source(
    const std::filesystem::path &platform_source_path)
    -> std::optional<std::string> {
  auto file = std::ifstream(platform_source_path);
  if (!file) {
    return std::nullopt;
  }
  auto body = std::string(std::istreambuf_iterator<char>(file),
                          std::istreambuf_iterator<char>());
  constexpr std::string_view k_expected_first_line = "module std.platform";
  if (!body.starts_with(k_expected_first_line)) {
    return std::nullopt;
  }
  const auto first_newline = body.find('\n');
  const auto rest = first_newline == std::string::npos
                        ? std::string_view{}
                        : std::string_view(body).substr(first_newline + 1);
  return std::format("module std.platform\n\n{}{}",
                     generate_platform_target_accessors(), rest);
}

} // namespace

/// Locates `filename` under the `src/std` Bazel package, trying every
/// invocation shape the binary might run under (`bazelisk run`'s runfiles
/// tree, `bazel test`'s `TEST_SRCDIR`, a plain `bazel-bin` invocation from
/// the workspace root, or — for a binary installed from a `just package`
/// archive/`.deb`, run from any working directory — `share/kira/std` next
/// to the running executable's own resolved install prefix) — the same set
/// of candidates `find_bazel_archive` (`src/driver/aot.cpp`) tries for the
/// AOT runtime archives.
[[nodiscard]] static auto find_stdlib_source_file(std::string_view program_name,
                                                  std::string_view filename)
    -> std::optional<std::filesystem::path> {
  auto candidates = std::vector<std::filesystem::path>{};
  if (!program_name.empty()) {
    candidates.emplace_back(
        std::filesystem::path(std::format("{}.runfiles", program_name)) /
        "_main" / "src" / "std" / filename);
  }
  if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
    if (const auto *workspace = std::getenv("TEST_WORKSPACE");
        workspace != nullptr && *workspace != '\0') {
      candidates.emplace_back(std::filesystem::path(srcdir) / workspace /
                              "src" / "std" / filename);
    }
    candidates.emplace_back(std::filesystem::path(srcdir) / "_main" / "src" /
                            "std" / filename);
  }
  candidates.emplace_back(std::filesystem::path("src") / "std" / filename);
  if (const auto exe_path = util::resolve_self_executable()) {
    // Installed layout: `<prefix>/bin/kira`, stdlib sources under
    // `<prefix>/share/kira/std/`.
    candidates.emplace_back(exe_path->parent_path().parent_path() / "share" /
                            "kira" / "std" / filename);
  }

  for (const auto &candidate : candidates) {
    auto ec = std::error_code{};
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
  }
  return std::nullopt;
}

auto inject_stdlib_prelude(cli_config &cfg) -> void {
  auto already_present =
      [&cfg](const std::filesystem::path &candidate) -> bool {
    const auto normalized = normalize_path(candidate);
    return std::ranges::any_of(cfg.sources, [&](const auto &source) -> bool {
      return normalize_path(std::filesystem::path(source)) == normalized;
    });
  };

  // `traits.kira` first: it declares the traits `prelude.kira` depends on;
  // `io.kira` before `console.kira`, matching `console.kira`'s own
  // `use std.io` — though declaration order across session files has no
  // effect on resolution — the whole session's module index is built
  // before any file is checked. `fmt.kira` backs string-interpolation
  // formatting (`spec/string-formatting-design.md`) and is always needed
  // once any source file contains a `"{expr}"` interpolation. `deriving.kira`
  // provides the real `static def derive_show[T]()` that `deriving show`
  // sugar splices in (`semantic::checker::resolve_deriving_show`) — always
  // injected, exactly like the others, so it's available to every session
  // even though most sessions never actually reference it by name.
  for (const auto *filename :
       {"traits.kira", "iter.kira", "prelude.kira", "panic.kira", "option.kira",
        "result.kira", "list.kira", "io.kira", "console.kira", "algo.kira",
        "fmt.kira", "string.kira", "deriving.kira", "fs/path.kira"}) {
    const auto found = find_stdlib_source_file(cfg.program_name, filename);
    if (found && !already_present(*found)) {
      cfg.sources.push_back(found->string());
    }
  }

  // `std.platform` is assembled rather than injected verbatim: its checked-in
  // `platform.kira` body is spliced together with the driver-generated
  // `TARGET_*`/`KIRA_*` constants block (`assemble_platform_module_source`'s
  // doc comment explains why the two can't be separate modules) and the
  // result written to a fixed path under the system temp directory. The
  // content is a pure function of this compiler binary's own build host, so
  // a stale or concurrently-written copy from another `kira` invocation is
  // always byte-identical — nothing to race on.
  if (const auto platform_source =
          find_stdlib_source_file(cfg.program_name, "platform.kira")) {
    auto ec = std::error_code{};
    const auto assembled_path =
        std::filesystem::temp_directory_path(ec) / "kira-platform.kira";
    if (!ec && !already_present(assembled_path)) {
      if (const auto assembled =
              assemble_platform_module_source(*platform_source)) {
        auto out = std::ofstream(assembled_path, std::ios::trunc);
        if (out) {
          out << *assembled;
          out.close();
          if (!out.fail()) {
            cfg.sources.push_back(assembled_path.string());
          }
        }
      }
    }
  }
}

auto compile_sources(const cli_config &cfg, bool use_color)
    -> std::expected<compile_report, std::string> {
  if (cfg.sources.empty()) {
    return std::unexpected{"no source files provided"};
  }

  auto report = compile_report{};
  const auto metadata_root = std::filesystem::path(cfg.metadata_dir);
  auto sources = source_manager{};
  auto session_diagnostics = diagnostic_bag{};
  auto file_has_errors = std::vector<bool>{};

  auto parsed_inputs = parse_sources(cfg, sources, session_diagnostics,
                                     file_has_errors, report.diagnostics);

  // Fold import-gating `static if` blocks before the module graph is built, so
  // the selected `use`s (and any other items in the taken branch) become real
  // top-level items every later phase sees. See `fold_static_if_imports`.
  fold_static_if_imports(parsed_inputs, session_diagnostics);

  auto semantic_inputs = std::vector<semantic::parsed_module>{};
  semantic_inputs.reserve(parsed_inputs.size());
  for (const auto &input : parsed_inputs) {
    semantic_inputs.push_back(semantic::parsed_module{
        .file_id = input.file_id,
        .ast_file = input.ast_file.get(),
    });
  }

  const auto checked = semantic::validate_semantics(
      semantic_inputs, session_diagnostics, file_has_errors,
      semantic::semantic_options{.check_names_and_types = !cfg.parse_only});

  report.error_count += session_diagnostics.error_count();
  append_text(
      report.diagnostics,
      diagnostic_renderer(sources, use_color).render_all(session_diagnostics));

  const auto lowering_renderer = diagnostic_renderer(sources, use_color);
  const auto lowered_modules =
      lower_and_emit_modules(cfg, parsed_inputs, file_has_errors, checked,
                             metadata_root, lowering_renderer, report);

  run_requested_function(cfg, lowered_modules, checked, report);
  build_requested_function(cfg, lowered_modules, checked, report);

  return report;
}

auto render_compile_summary(const compile_report &report,
                            bool show_compile_details) -> std::string {
  if (report.modules.empty()) {
    return std::format("Compilation failed with {} error(s).",
                       report.error_count);
  }

  std::string out;
  auto append_line = [&out](std::string_view line) -> void {
    if (!out.empty()) {
      out += "\n";
    }
    out += line;
  };

  if (show_compile_details) {
    std::string modules_section =
        std::format("Compiled {} module(s):", report.modules.size());
    for (size_t i = 0; i < report.modules.size(); ++i) {
      modules_section += std::format("\n  [{}] {} -> {}", i,
                                     module_display_name(report.modules[i]),
                                     report.modules[i].metadata_path);
    }
    append_line(modules_section);

    if (!report.hir_modules.empty()) {
      const auto lowered_count = static_cast<size_t>(std::count_if(
          report.hir_modules.begin(), report.hir_modules.end(),
          [](const auto &result) -> auto { return result.lowered; }));
      std::string hir_section =
          std::format("Lowered {}/{} module(s) to HIR.", lowered_count,
                      report.hir_modules.size());
      for (const auto &result : report.hir_modules) {
        if (!result.lowered) {
          hir_section +=
              std::format("\n  [{}] {}", result.module_path, result.error);
        }
      }
      append_line(hir_section);
    }
  }

  if (report.run.has_value()) {
    if (report.run->succeeded) {
      if (show_compile_details) {
        append_line(report.run->message);
      }
    } else {
      append_line(std::format("run failed: {}", report.run->message));
    }
  }
  if (report.build.has_value()) {
    append_line(report.build->succeeded
                    ? std::format("Built executable: {}", report.build->message)
                    : std::format("build failed: {}", report.build->message));
  }
  if (report.error_count > 0) {
    append_line(std::format("Encountered {} error(s).", report.error_count));
  }
  return out;
}

} // namespace kira::driver
