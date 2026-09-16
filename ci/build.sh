#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/paths.sh"
cd "$source_dir"
mkdir -p build artifacts "$build_dir/packages"
configure() {
  rm -f build/test-results.xml build/sanitizer-results.xml
  cmake -S "$source_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DENABLE_SANITIZERS=OFF
  cmake --build "$build_dir" --parallel
}
tests() {
  ctest --test-dir "$build_dir" --output-on-failure --output-junit "$source_dir/build/test-results.xml"
}
benchmarks() {
  for size in 4096 65536 1048576; do
    "$build_dir/fusefs-bench" --bytes 8388608 --chunk-size "$size" > "artifacts/benchmark-$size.json"
  done
  python3 benchmarks/mount_bench.py "$build_dir/local-fusefs" "$build_dir/fusefs-admin" > artifacts/benchmark-fuse.json
}
package() {
  rm -f artifacts/local-fusefs-*.tar.gz artifacts/local-fusefs-*.deb artifacts/SHA256SUMS
  rm -f "$build_dir"/packages/local-fusefs-*.tar.gz "$build_dir"/packages/local-fusefs-*.deb
  (cd "$build_dir" && cpack -B "$build_dir/packages")
  cp "$build_dir"/packages/local-fusefs-*.tar.gz "$build_dir"/packages/local-fusefs-*.deb artifacts/
  cp "$build_dir/local-fusefs" "$build_dir/fusefs-admin" "$build_dir/fusefs-bench" build/
  (cd artifacts && sha256sum *.tar.gz *.deb > SHA256SUMS)
  bash ci/verify-package.sh
}
case "${1:-all}" in
  configure) configure;;
  test) tests;;
  benchmark) benchmarks;;
  package) package;;
  all) configure; tests; benchmarks; package;;
  *) echo "Usage: $0 [configure|test|benchmark|package|all]" >&2; exit 2;;
esac
