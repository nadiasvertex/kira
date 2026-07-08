#!/usr/bin/env just --justfile

CLANG_TIDY := "/opt/homebrew/opt/llvm/bin/clang-tidy"
CLANG_FORMAT := "/opt/homebrew/opt/llvm/bin/clang-format"
DIST_DIR := "dist"

# Build the kira binary
build:
    bazelisk build //src:kira

# Run the project test suite
test:
    bazelisk test //...

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
    mkdir -p "$tar_root/bin"

    bazelisk build --config=release //src:kira
    cp -L "bazel-bin/src/kira" "$tar_root/bin/kira"
    chmod 0755 "$tar_root/bin/kira"

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

      mkdir -p "$deb_root/usr/bin" "$deb_root/usr/share/doc/kira" "$control_root"
      cp -L "bazel-bin/src/kira" "$deb_root/usr/bin/kira"
      chmod 0755 "$deb_root/usr/bin/kira"
      cp "README.md" "$deb_root/usr/share/doc/kira/README.md"
      cp -R "spec" "$deb_root/usr/share/doc/kira/spec"

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

# Run the kira binary
run source_file:
    bazelisk run //src:kira -- {{ source_file }}

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
