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
    find src -type f \( -name "*.cpp" -o -name "*.cxx" -o -name "*.cc" -o -name "*.hpp" -o -name "*.h" \) -print0 | xargs -0 -P1 {{ CLANG_TIDY }} -p=. --fix --fix-errors
