"""Locates a system LLVM install via `llvm-config` (spec/codegen-design.md
Decision 2): there is no Bazel Central Registry module for the LLVM C++
libraries, so this repository rule shells out to `llvm-config` on the host
to find the headers/static archives already installed by the pinned
toolchain (CLAUDE.md requires Clang 22.1+; this pins to the matching LLVM
22 release for the same reason C++ ABI stability is not guaranteed across
LLVM major versions), rather than vendoring or building llvm-project from
source under Bazel.
"""

# Increment 3's scope (HIR -> llvm::Module, plus enough JIT to run the
# result for testing) only needs IR construction, the host's native target
# for JIT machine code, and ORC JIT itself -- not the full "all-targets"
# set AOT/cross-compilation will eventually want.
_COMPONENTS = ["core", "native", "orcjit", "irreader"]

_CANDIDATE_LLVM_CONFIGS = [
    "/opt/homebrew/opt/llvm/bin/llvm-config",
    "/usr/local/opt/llvm/bin/llvm-config",
    "/usr/lib/llvm-22/bin/llvm-config",
    "/usr/bin/llvm-config-22",
]

def _run(repo_ctx, llvm_config, args):
    result = repo_ctx.execute([llvm_config] + args)
    if result.return_code != 0:
        fail("`{} {}` failed (exit {}): {}".format(
            llvm_config,
            " ".join(args),
            result.return_code,
            result.stderr,
        ))
    return result.stdout.strip()

def _find_llvm_config(repo_ctx):
    env_override = repo_ctx.os.environ.get("LLVM_CONFIG")
    if env_override:
        return env_override

    found = repo_ctx.which("llvm-config")
    if found:
        return str(found)

    for candidate in _CANDIDATE_LLVM_CONFIGS:
        if repo_ctx.path(candidate).exists:
            return candidate

    fail(
        "llvm-config not found. Install LLVM 22.1+ (e.g. `brew install " +
        "llvm` on macOS, or your distro's llvm-22 package on Linux) and " +
        "ensure `llvm-config` is on PATH, or set the LLVM_CONFIG " +
        "environment variable to its full path. See spec/codegen-design.md " +
        "Decision 2.",
    )

def _split_flags(raw):
    return [flag for flag in raw.split(" ") if flag]

def _llvm_repository_impl(repo_ctx):
    llvm_config = _find_llvm_config(repo_ctx)

    version = _run(repo_ctx, llvm_config, ["--version"])
    if not version.startswith("22."):
        fail((
            "kira's llvm_codegen requires LLVM 22.x, matching CLAUDE.md's " +
            "Clang 22.1+ toolchain pin (spec/codegen-design.md Decision 2 " +
            "explains why this project pins rather than floats across LLVM " +
            "major versions) -- llvm-config at {} reports version {}."
        ).format(llvm_config, version))

    includedir = _run(repo_ctx, llvm_config, ["--includedir"])
    libdir = _run(repo_ctx, llvm_config, ["--libdir"])

    # -I/-std are dropped: headers are reached through this repo's own
    # `include` symlink + `includes` attribute below, and this project
    # pins its own C++ standard (CLAUDE.md) rather than inheriting
    # whatever LLVM itself was built with. `-fno-exceptions` is dropped too
    # -- that describes how LLVM's *own* sources were compiled, not a
    # constraint on consumers, and kira's codegen code needs exceptions
    # (panic_error, matching the bytecode VM's own panic propagation).
    cxxflags = [
        flag
        for flag in _split_flags(_run(repo_ctx, llvm_config, ["--cxxflags"]))
        if not flag.startswith("-I") and not flag.startswith("-std=") and
           flag != "-fno-exceptions"
    ]

    ldflags = [
        flag
        for flag in _split_flags(_run(repo_ctx, llvm_config, ["--ldflags"]))
        if not flag.startswith("-L")
    ]

    # The static archives themselves are declared as `srcs` below (named
    # explicitly via `--libnames`, from this repo's own `lib` symlink)
    # rather than passed as `-lLLVMxxx` linkopts -- `srcs` gets them linked
    # directly with no dependence on a `-L` search path pointing back out at
    # the host filesystem, and named explicitly (not a `lib/*.a` glob) so
    # this doesn't drag in every other LLVM-project archive (clang, lld,
    # ...) the host install happens to also ship.
    libnames = _split_flags(_run(
        repo_ctx,
        llvm_config,
        ["--link-static", "--libnames"] + _COMPONENTS,
    ))
    system_libs = _split_flags(_run(
        repo_ctx,
        llvm_config,
        ["--link-static", "--system-libs"] + _COMPONENTS,
    ))

    repo_ctx.symlink(includedir, "include")
    repo_ctx.symlink(libdir, "lib")

    # `--system-libs` mixes ordinary `-lfoo` flags (resolved through the
    # default linker search path) with bare absolute paths for libraries
    # `llvm-config` couldn't name a `-l` flag for. Either way, a raw linkopt
    # referencing a path outside this repo (or a `-lfoo` the default linker
    # search path doesn't actually contain, e.g. a keg-only Homebrew
    # dependency) would need the sandboxed link action to have host
    # filesystem access it doesn't have by default, so every such library
    # this rule can locate gets symlinked into this repo (an ordinary
    # declared `srcs` input) instead of passed through as a raw flag.
    extra_lib_srcs = []
    system_lib_opts = []
    for flag in system_libs:
        if flag.startswith("/"):
            name = flag.rsplit("/", 1)[-1]
            repo_ctx.symlink(flag, "extra_libs/" + name)
            extra_lib_srcs.append("extra_libs/" + name)
            continue

        if flag.startswith("-l"):
            located = None
            for libdir_candidate in (libdir, "/opt/homebrew/lib", "/usr/local/lib"):
                for ext in (".dylib", ".so", ".a"):
                    candidate = "{}/lib{}{}".format(
                        libdir_candidate, flag[len("-l"):], ext)
                    if repo_ctx.path(candidate).exists:
                        located = candidate
                        break
                if located:
                    break
            if located:
                name = located.rsplit("/", 1)[-1]
                repo_ctx.symlink(located, "extra_libs/" + name)
                extra_lib_srcs.append("extra_libs/" + name)
                continue

        system_lib_opts.append(flag)

    build_file = """load("@rules_cc//cc:defs.bzl", "cc_library")

# Generated by third_party/llvm/repo.bzl from `{llvm_config}` (LLVM {version}).
# This target intentionally runs unsandboxed (`no-sandbox`): its static
# archives and a handful of Homebrew-installed system libraries (see
# `extra_libs/`) are reached by symlink from outside this repository, and
# `llvm-config`'s own system-library flags reference the host's default
# library search path directly -- see repo.bzl's top comment for why this
# is an accepted, deliberately non-hermetic integration (spec/codegen-design.md
# Decision 2), not an oversight.
cc_library(
    name = "llvm",
    hdrs = glob(["include/**"]),
    includes = ["include"],
    copts = {cxxflags},
    srcs = {lib_srcs} + {extra_lib_srcs},
    linkopts = {linkopts},
    tags = ["no-sandbox"],
    visibility = ["//visibility:public"],
)
""".format(
        llvm_config = llvm_config,
        version = version,
        cxxflags = repr(cxxflags),
        lib_srcs = repr(["lib/" + name for name in libnames]),
        extra_lib_srcs = repr(extra_lib_srcs),
        linkopts = repr(ldflags + system_lib_opts),
    )

    repo_ctx.file("BUILD.bazel", build_file)
    repo_ctx.file("WORKSPACE", "workspace(name = \"llvm\")\n")

llvm_repository = repository_rule(
    implementation = _llvm_repository_impl,
    local = True,
    environ = ["LLVM_CONFIG", "PATH"],
)

def _llvm_extension_impl(_module_ctx):
    llvm_repository(name = "llvm")

llvm_extension = module_extension(implementation = _llvm_extension_impl)
