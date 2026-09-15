#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/paths.sh"
cd "$source_dir"
# Keep compiler output on the Linux filesystem: DrvFS does not support all
# permission operations used by CMake and Debian packaging.
mkdir -p build artifacts "$build_dir/packages"
rm -f artifacts/local-fusefs-*.tar.gz artifacts/local-fusefs-*.deb artifacts/SHA256SUMS
rm -f "$build_dir"/packages/local-fusefs-*.tar.gz "$build_dir"/packages/local-fusefs-*.deb
cmake -S "$source_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure --output-junit "$source_dir/build/test-results.xml"
bash tests/mount_test.sh "$build_dir/local-fusefs"
(cd "$build_dir" && cpack -B "$build_dir/packages")
cp "$build_dir"/packages/local-fusefs-*.tar.gz "$build_dir"/packages/local-fusefs-*.deb artifacts/
cp "$build_dir/local-fusefs" build/local-fusefs
(cd artifacts && sha256sum *.tar.gz *.deb > SHA256SUMS)
