#!/usr/bin/env just --justfile

# Build the kira binary
build:
    bazelisk build //src:kira

# Generate compile_commands.json and run clang-tidy (apply fixes) using it
format:
    # Generate a compilation database that clang-tidy can use
    ./tools/gen_compile_commands.sh
    # If compile_commands.json wasn't produced, warn but continue (clang-tidy will fail without it)
    if [ ! -f compile_commands.json ]; then echo "Warning: compile_commands.json not found; clang-tidy may fail"; fi
    # Run clang-tidy over C/C++ sources using the compilation database at repo root (-p=.)
    find src -type f \( -name "*.cpp" -o -name "*.cxx" -o -name "*.cc" -o -name "*.hpp" -o -name "*.h" \) -print0 | xargs -0 clang-tidy -p=. --fix
