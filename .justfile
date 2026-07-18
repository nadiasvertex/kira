#!/usr/bin/env just --justfile

CLANG_TIDY := "/opt/homebrew/opt/llvm/bin/clang-tidy"
CLANG_FORMAT := "/opt/homebrew/opt/llvm/bin/clang-format"
DIST_DIR := "dist"

# Build the kira binary
build:
    bazelisk build --config=debug //src:kira

# Run the project test suite
test:
    bazelisk test --config=debug //...

# Build release archives for the kira CLI
package:
    #!/usr/bin/env bash
    set -euo pipefail

    version=$(grep -m1 'version = "' MODULE.bazel | cut -d '"' -f2)
    platform=$(uname -s | tr '[:upper:]' '[:lower:]')
    machine=$(uname -m)

    dist_root="{{ DIST_DIR }}"
    work_root="$dist_root/.package"
    tar_root="$work_root/kira-$version-$platform-$machine"
    tarball="$dist_root/kira-$version-$platform-$machine.tar.bz2"

    rm -rf "$dist_root"
    mkdir -p "$tar_root/bin" "$tar_root/share/kira/std/fs" "$tar_root/lib/kira"

    # `kira build` (AOT native compile) shells out to `c++` and links the
    # produced object file against these four archives (`find_bazel_archive`,
    # `src/driver/aot.cpp`); `inject_stdlib_prelude`
    # (`src/driver/driver.cpp`) auto-imports the `src/std/*.kira` sources
    # into every compile. Both lookups fall back to a path next to the
    # installed binary's own resolved location
    # (`<prefix>/lib/kira/`, `<prefix>/share/kira/std/`), which is the
    # layout built below.
    bazelisk build --config=release \
      //src:kira \
      //src/llvm_codegen:aot_runtime \
      //src/runtime:runtime \
      //src/semantic:semantic \
      //src/parser:parser
    cp -L "bazel-bin/src/kira" "$tar_root/bin/kira"
    chmod 0755 "$tar_root/bin/kira"

    rm -f "$tar_root/lib/kira"/*.lo "$tar_root/lib/kira"/*.a
    cp -L "bazel-bin/src/llvm_codegen/libaot_runtime.lo" \
          "bazel-bin/src/runtime/libruntime.lo" \
          "bazel-bin/src/semantic/libsemantic.a" \
          "bazel-bin/src/parser/libparser.a" \
          "$tar_root/lib/kira/"
    cp src/std/*.kira "$tar_root/share/kira/std/"
    cp src/std/fs/*.kira "$tar_root/share/kira/std/fs/"

    cp "README.md" "$tar_root/README.md"
    cp -R "spec" "$tar_root/spec"

    tar -C "$work_root" -cjf "$tarball" "$(basename "$tar_root")"

    printf 'Wrote %s\n' "$tarball"
    if [ "$platform" = "linux" ]; then
      case "$machine" in
        x86_64)
          deb_arch=amd64
          ;;
        aarch64|arm64)
          deb_arch=arm64
          ;;
        *)
          deb_arch="$machine"
          ;;
      esac

      deb_root="$work_root/deb"
      control_root="$work_root/control"
      deb="$dist_root/kira_${version}_${deb_arch}.deb"

      mkdir -p "$deb_root/usr/bin" "$deb_root/usr/share/doc/kira" \
        "$deb_root/usr/share/kira/std/fs" "$deb_root/usr/lib/kira"
      cp -L "bazel-bin/src/kira" "$deb_root/usr/bin/kira"
      chmod 0755 "$deb_root/usr/bin/kira"
      cp "README.md" "$deb_root/usr/share/doc/kira/README.md"
      cp -R "spec" "$deb_root/usr/share/doc/kira/spec"

      # `/usr/bin/kira` resolves its own install prefix (`/usr`) at runtime,
      # so its support files must land under `/usr/share/kira/std` and
      # `/usr/lib/kira/` — see the tarball packaging step above for why.
      cp -L "bazel-bin/src/llvm_codegen/libaot_runtime.lo" \
            "bazel-bin/src/runtime/libruntime.lo" \
            "bazel-bin/src/semantic/libsemantic.a" \
            "bazel-bin/src/parser/libparser.a" \
            "$deb_root/usr/lib/kira/"
      cp src/std/*.kira "$deb_root/usr/share/kira/std/"
      cp src/std/fs/*.kira "$deb_root/usr/share/kira/std/fs/"

      printf '%s\n' \
        "Package: kira" \
        "Version: $version" \
        "Section: devel" \
        "Priority: optional" \
        "Architecture: $deb_arch" \
        "Maintainer: Kira Contributors <maintainers@kira.invalid>" \
        "Description: Kira compiler and language tooling" \
        " Kira is an early-stage language and compiler project." \
        > "$control_root/control"
      printf '2.0\n' > "$work_root/debian-binary"
      tar -C "$control_root" -czf "$work_root/control.tar.gz" "control"
      tar -C "$deb_root" -cjf "$work_root/data.tar.bz2" "."
      ar -crS "$deb" "$work_root/debian-binary" "$work_root/control.tar.gz" "$work_root/data.tar.bz2"
      printf 'Wrote %s\n' "$deb"
    else
      printf '%s\n' "Skipped .deb packaging on $platform; build on Linux to produce a Debian package."
    fi

# Install kira into PREFIX (default: $HOME/.kira)
install prefix=(env('HOME') / '.kira'):
    #!/usr/bin/env bash
    set -euo pipefail

    prefix="{{ prefix }}"

    # Mirrors the `package` recipe's layout: `kira build` (AOT native
    # compile) and `inject_stdlib_prelude` both resolve support files
    # relative to the installed binary's own location
    # (`<prefix>/lib/kira/`, `<prefix>/share/kira/std/`).
    bazelisk build --config=release \
      //src:kira \
      //src/llvm_codegen:aot_runtime \
      //src/runtime:runtime \
      //src/semantic:semantic \
      //src/parser:parser

    mkdir -p "$prefix/bin" "$prefix/share/kira/std/fs" "$prefix/lib/kira"
    cp -L "bazel-bin/src/kira" "$prefix/bin/kira"
    chmod 0755 "$prefix/bin/kira"
    rm -f "$prefix/lib/kira"/*.lo "$prefix/lib/kira"/*.a
    cp -L "bazel-bin/src/llvm_codegen/libaot_runtime.lo" \
          "bazel-bin/src/runtime/libruntime.lo" \
          "bazel-bin/src/semantic/libsemantic.a" \
          "bazel-bin/src/parser/libparser.a" \
          "$prefix/lib/kira/"
    cp src/std/*.kira "$prefix/share/kira/std/"
    cp src/std/fs/*.kira "$prefix/share/kira/std/fs/"

    printf 'Installed kira to %s\n' "$prefix"
    printf 'Add %s/bin to your PATH to use it.\n' "$prefix"

# Bump the minor version (reset patch to 0) and refresh the release date
bump-minor:
    #!/usr/bin/env bash
    set -euo pipefail

    version_file="src/version.h"
    module_file="MODULE.bazel"
    version_test_file="src/testdata/std_test/kira_version.expected"
    major=$(grep -m1 -E 'k_version_major = [0-9]+;' "$version_file" | grep -oE '[0-9]+')
    minor=$(grep -m1 -E 'k_version_minor = [0-9]+;' "$version_file" | grep -oE '[0-9]+')
    minor=$((minor + 1))
    patch=0
    date=$(date +%Y-%m-%d)

    sed -i '' -E \
      -e "s/k_version_major = [0-9]+;/k_version_major = ${major};/" \
      -e "s/k_version_minor = [0-9]+;/k_version_minor = ${minor};/" \
      -e "s/k_version_patch = [0-9]+;/k_version_patch = ${patch};/" \
      -e "s/k_version_string = \"[0-9]+\.[0-9]+\.[0-9]+\";/k_version_string = \"${major}.${minor}.${patch}\";/" \
      -e "s/k_release_date = \"[0-9-]+\";/k_release_date = \"${date}\";/" \
      "$version_file"
    sed -i '' -E \
      -e "s/version = \"[0-9]+\.[0-9]+\.[0-9]+\",/version = \"${major}.${minor}.${patch}\",/" \
      "$module_file"
    sed -i '' -E \
      -e "s/Kira version: [0-9]+\.[0-9]+\.[0-9]+/Kira version: ${major}.${minor}.${patch}/" \
      -e "s/Kira build date: [0-9-]+/Kira build date: ${date}/" \
      "$version_test_file"

    printf 'Bumped to %s.%s.%s (%s)\n' "$major" "$minor" "$patch" "$date"

# Bump the major version (reset minor and patch to 0) and refresh the release date
bump-major:
    #!/usr/bin/env bash
    set -euo pipefail

    version_file="src/version.h"
    module_file="MODULE.bazel"
    version_test_file="src/testdata/std_test/kira_version.expected"
    major=$(grep -m1 -E 'k_version_major = [0-9]+;' "$version_file" | grep -oE '[0-9]+')
    major=$((major + 1))
    minor=0
    patch=0
    date=$(date +%Y-%m-%d)

    sed -i '' -E \
      -e "s/k_version_major = [0-9]+;/k_version_major = ${major};/" \
      -e "s/k_version_minor = [0-9]+;/k_version_minor = ${minor};/" \
      -e "s/k_version_patch = [0-9]+;/k_version_patch = ${patch};/" \
      -e "s/k_version_string = \"[0-9]+\.[0-9]+\.[0-9]+\";/k_version_string = \"${major}.${minor}.${patch}\";/" \
      -e "s/k_release_date = \"[0-9-]+\";/k_release_date = \"${date}\";/" \
      "$version_file"
    sed -i '' -E \
      -e "s/version = \"[0-9]+\.[0-9]+\.[0-9]+\",/version = \"${major}.${minor}.${patch}\",/" \
      "$module_file"
    sed -i '' -E \
      -e "s/Kira version: [0-9]+\.[0-9]+\.[0-9]+/Kira version: ${major}.${minor}.${patch}/" \
      -e "s/Kira build date: [0-9-]+/Kira build date: ${date}/" \
      "$version_test_file"

    printf 'Bumped to %s.%s.%s (%s)\n' "$major" "$minor" "$patch" "$date"

# Run the kira binary
run source_file: build
    bazel-bin/src/kira {{ source_file }}

# Generate compile_commands.json
compile-commands:
    bazelisk run @wolfd_bazel_compile_commands//:generate_compile_commands -- //src:kira

format: compile-commands
    find ./src -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) -print0 \
      | xargs -0 -P1 {{ CLANG_FORMAT }} -i

lint: compile-commands
    # Run clang-tidy
    # Auto-detect macOS SDK, then run clang-tidy with extra args.
    # clang-tidy resolves its own matching libc++ headers automatically; forcing
    # an extra -I for Homebrew LLVM's libc++ conflicts with the macOS SDK headers
    # (Apple clang vs. Homebrew clang) and corrupts parsing for every file.
    SDK="$(xcrun --show-sdk-path)"; \
    find ./src -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) -print0 \
      | xargs -0 -P1 {{ CLANG_TIDY }} -p=. --fix --fix-errors \
          --extra-arg=-isysroot --extra-arg="$SDK"
