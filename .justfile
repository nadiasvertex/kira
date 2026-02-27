#!/usr/bin/env just --justfile

# Build the kira binary
build:
    bazelisk build //src:kira

# Format all C++ files in src/ tree with clang-format
format:
    find src -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "*.cc" -o -name "*.cxx" \) -exec clang-format -i {} +
