#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/paths.sh"
sanitizer_dir="${build_dir}-sanitizers"
cmake -S "$source_dir" -B "$sanitizer_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DENABLE_SANITIZERS=ON
cmake --build "$sanitizer_dir" --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir "$sanitizer_dir" --output-on-failure --output-junit "$source_dir/build/sanitizer-results.xml"