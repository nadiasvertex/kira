#!/usr/bin/env just --justfile

CLANG_TIDY := "/opt/homebrew/opt/llvm/bin/clang-tidy"
CLANG_FORMAT := "/opt/homebrew/opt/llvm/bin/clang-format"
DIST_DIR := "dist"

# Build the cinder binary
build:
    bazelisk build --config=debug //src:cinder

# Run the project test suite
test:
    bazelisk test --config=debug //...

# Wipe all Bazel state (cache + external repos) and rebuild from scratch
clean:
    # Run this after a toolchain change (e.g. an Xcode update) leaves stale
    # builtin-include paths behind and builds fail with
    # "absolute path inclusion(s) found".
    bazelisk clean --expunge
    bazelisk build --config=debug //src:cinder

# Build release archives for the cinder CLI
package:
    #!/usr/bin/env bash
    set -euo pipefail

    version=$(grep -m1 'version = "' MODULE.bazel | cut -d '"' -f2)
    platform=$(uname -s | tr '[:upper:]' '[:lower:]')
    machine=$(uname -m)

    dist_root="{{ DIST_DIR }}"
    work_root="$dist_root/.package"
    tar_root="$work_root/cinder-$version-$platform-$machine"
    tarball="$dist_root/cinder-$version-$platform-$machine.tar.bz2"

    rm -rf "$dist_root"
    mkdir -p "$tar_root/bin" "$tar_root/share/cinder/std/fs" "$tar_root/lib/cinder"

    # `cinder build` (AOT native compile) shells out to `c++` and links the
    # produced object file against these four archives (`find_bazel_archive`,
    # `src/driver/aot.cpp`); `inject_stdlib_prelude`
    # (`src/driver/driver.cpp`) auto-imports the `src/std/*.cn` sources
    # into every compile. Both lookups fall back to a path next to the
    # installed binary's own resolved location
    # (`<prefix>/lib/cinder/`, `<prefix>/share/cinder/std/`), which is the
    # layout built below.
    bazelisk build --config=release \
      //src:cinder \
      //src/llvm_codegen:aot_runtime \
      //src/runtime:runtime \
      //src/semantic:semantic \
      //src/parser:parser
    cp -L "bazel-bin/src/cinder" "$tar_root/bin/cinder"
    chmod 0755 "$tar_root/bin/cinder"

    rm -f "$tar_root/lib/cinder"/*.lo "$tar_root/lib/cinder"/*.a
    cp -L "bazel-bin/src/llvm_codegen/libaot_runtime.lo" \
          "bazel-bin/src/runtime/libruntime.lo" \
          "bazel-bin/src/semantic/libsemantic.a" \
          "bazel-bin/src/parser/libparser.a" \
          "$tar_root/lib/cinder/"
    cp src/std/*.cn "$tar_root/share/cinder/std/"
    cp src/std/fs/*.cn "$tar_root/share/cinder/std/fs/"

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
      deb="$dist_root/cinder_${version}_${deb_arch}.deb"

      mkdir -p "$deb_root/usr/bin" "$deb_root/usr/share/doc/cinder" \
        "$deb_root/usr/share/cinder/std/fs" "$deb_root/usr/lib/cinder"
      cp -L "bazel-bin/src/cinder" "$deb_root/usr/bin/cinder"
      chmod 0755 "$deb_root/usr/bin/cinder"
      cp "README.md" "$deb_root/usr/share/doc/cinder/README.md"
      cp -R "spec" "$deb_root/usr/share/doc/cinder/spec"

      # `/usr/bin/cinder` resolves its own install prefix (`/usr`) at runtime,
      # so its support files must land under `/usr/share/cinder/std` and
      # `/usr/lib/cinder/` — see the tarball packaging step above for why.
      cp -L "bazel-bin/src/llvm_codegen/libaot_runtime.lo" \
            "bazel-bin/src/runtime/libruntime.lo" \
            "bazel-bin/src/semantic/libsemantic.a" \
            "bazel-bin/src/parser/libparser.a" \
            "$deb_root/usr/lib/cinder/"
      cp src/std/*.cn "$deb_root/usr/share/cinder/std/"
      cp src/std/fs/*.cn "$deb_root/usr/share/cinder/std/fs/"

      printf '%s\n' \
        "Package: cinder" \
        "Version: $version" \
        "Section: devel" \
        "Priority: optional" \
        "Architecture: $deb_arch" \
        "Maintainer: Cinder Contributors <maintainers@cinder.invalid>" \
        "Description: Cinder compiler and language tooling" \
        " Cinder is an early-stage language and compiler project." \
        > "$control_root/control"
      printf '2.0\n' > "$work_root/debian-binary"
      tar -C "$control_root" -czf "$work_root/control.tar.gz" "control"
      tar -C "$deb_root" -cjf "$work_root/data.tar.bz2" "."
      ar -crS "$deb" "$work_root/debian-binary" "$work_root/control.tar.gz" "$work_root/data.tar.bz2"
      printf 'Wrote %s\n' "$deb"
    else
      printf '%s\n' "Skipped .deb packaging on $platform; build on Linux to produce a Debian package."
    fi

# Install cinder into PREFIX (default: $HOME/.cinder)
install prefix=(env('HOME') / '.cinder'):
    #!/usr/bin/env bash
    set -euo pipefail

    prefix="{{ prefix }}"

    # Mirrors the `package` recipe's layout: `cinder build` (AOT native
    # compile) and `inject_stdlib_prelude` both resolve support files
    # relative to the installed binary's own location
    # (`<prefix>/lib/cinder/`, `<prefix>/share/cinder/std/`).
    bazelisk build --config=release \
      //src:cinder \
      //src/llvm_codegen:aot_runtime \
      //src/runtime:runtime \
      //src/semantic:semantic \
      //src/parser:parser

    mkdir -p "$prefix/bin" "$prefix/share/cinder/std/fs" "$prefix/lib/cinder"
    cp -L "bazel-bin/src/cinder" "$prefix/bin/cinder"
    chmod 0755 "$prefix/bin/cinder"
    rm -f "$prefix/lib/cinder"/*.lo "$prefix/lib/cinder"/*.a
    cp -L "bazel-bin/src/llvm_codegen/libaot_runtime.lo" \
          "bazel-bin/src/runtime/libruntime.lo" \
          "bazel-bin/src/semantic/libsemantic.a" \
          "bazel-bin/src/parser/libparser.a" \
          "$prefix/lib/cinder/"
    cp src/std/*.cn "$prefix/share/cinder/std/"
    cp src/std/fs/*.cn "$prefix/share/cinder/std/fs/"

    printf 'Installed cinder to %s\n' "$prefix"
    printf 'Add %s/bin to your PATH to use it.\n' "$prefix"

# Bump the minor version (reset patch to 0) and refresh the release date
bump-minor:
    #!/usr/bin/env bash
    set -euo pipefail

    version_file="src/version.h"
    module_file="MODULE.bazel"
    version_test_file="src/testdata/std_test/cinder_version.expected"
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
      -e "s/Cinder version: [0-9]+\.[0-9]+\.[0-9]+/Cinder version: ${major}.${minor}.${patch}/" \
      -e "s/Cinder build date: [0-9-]+/Cinder build date: ${date}/" \
      "$version_test_file"

    printf 'Bumped to %s.%s.%s (%s)\n' "$major" "$minor" "$patch" "$date"

# Bump the major version (reset minor and patch to 0) and refresh the release date
bump-major:
    #!/usr/bin/env bash
    set -euo pipefail

    version_file="src/version.h"
    module_file="MODULE.bazel"
    version_test_file="src/testdata/std_test/cinder_version.expected"
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
      -e "s/Cinder version: [0-9]+\.[0-9]+\.[0-9]+/Cinder version: ${major}.${minor}.${patch}/" \
      -e "s/Cinder build date: [0-9-]+/Cinder build date: ${date}/" \
      "$version_test_file"

    printf 'Bumped to %s.%s.%s (%s)\n' "$major" "$minor" "$patch" "$date"

# Run the cinder binary
run source_file: build
    bazel-bin/src/cinder {{ source_file }}

# Run every demo/*.cn program and fail if any of them error out
demo: build
    #!/usr/bin/env bash
    set -uo pipefail

    fail=0
    for f in demo/*.cn; do
      if ! out=$(bazel-bin/src/cinder "$f" 2>&1); then
        echo "=== FAIL: $f ==="
        echo "$out"
        fail=1
      fi
    done

    if [ "$fail" -ne 0 ]; then
      echo "One or more demo programs failed" >&2
      exit 1
    fi
    echo "All demo programs ran successfully"

# Generate compile_commands.json
compile-commands:
    bazelisk run @wolfd_bazel_compile_commands//:generate_compile_commands -- //src:cinder

format: compile-commands
    find ./src -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) -print0 \
      | xargs -0 -P1 {{ CLANG_FORMAT }} -i

# Build the specification (spec/specification/) as an epub, a pdf, and a
# static website, via pandoc. Output lands under dist/docs/.
docs:
    python3 tools/docs/build_docs.py all

# Render the specification (spec/specification/) to HTML via pandoc, into
# website/spec/, for publishing alongside the marketing site (website/).
website-spec:
    #!/usr/bin/env bash
    set -euo pipefail
    rm -rf website/spec website/.spec-build
    python3 tools/docs/build_docs.py site website/.spec-build
    mv website/.spec-build/site website/spec
    rm -rf website/.spec-build

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
