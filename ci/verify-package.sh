#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/paths.sh"
cd "$source_dir"
extracted=$(mktemp -d /tmp/local-fusefs-package-XXXXXX)
trap 'rm -rf -- "$extracted"' EXIT
packages=(artifacts/local-fusefs-*.deb)
[[ ${#packages[@]} == 1 ]] || { echo 'Expected one Debian package' >&2; exit 1; }
dpkg-deb -x "${packages[0]}" "$extracted"
binary=$(find "$extracted" -type f -name local-fusefs -print -quit)
bindir=$(dirname "$binary")
"$binary" --version
"$bindir/fusefs-admin" --version
python3 tests/integration.py "$binary" "$bindir/fusefs-admin" FuseTests.test_remount_persistence FuseTests.test_cli_history_restore_prune_gc
(cd artifacts && sha256sum -c SHA256SUMS)
