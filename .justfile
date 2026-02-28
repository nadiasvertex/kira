#!/usr/bin/env just --justfile

CLANG_TIDY := "/opt/homebrew/opt/llvm/bin/clang-tidy"

# Build the kira binary
build:
    bazelisk build //src:kira

# Run the kira binary
run:
    bazelisk run //src:kira

# Generate compile_commands.json and run clang-tidy (apply fixes) using it
format:
    bazelisk run @wolfd_bazel_compile_commands//:generate_compile_commands -- //src:kira
    # Auto-detect macOS SDK and optional Homebrew LLVM libc++ include, then run clang-tidy with extra args
    SDK="$(xcrun --show-sdk-path)"; \
    if [ -d "/opt/homebrew/opt/llvm/include/c++/v1" ]; then \
      LIBCPP_INC="/opt/homebrew/opt/llvm/include/c++/v1"; \
    else \
      LIBCPP_INC=""; \
    fi; \
    find ./src -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) -print0 \
      | xargs -0 -P1 {{ CLANG_TIDY }} -p=. --fix --fix-errors \
          --extra-arg=-isysroot --extra-arg="$SDK" $( [ -n "$LIBCPP_INC" ] && printf -- '--extra-arg=-I%s' "$LIBCPP_INC" )
