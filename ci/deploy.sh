#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/paths.sh"
cd "$source_dir"
# User-owned staging installation, identified by the tested commit.
revision=$(git rev-parse HEAD)
destination="$HOME/.local/opt/local-fusefs/releases/$revision"
mkdir -p "$destination"
install -m 0755 "$build_dir/local-fusefs" "$destination/local-fusefs"
ln -sfn "$destination" "$HOME/.local/opt/local-fusefs/current"
"$HOME/.local/opt/local-fusefs/current/local-fusefs" --version
echo "Deployed $revision to $destination"
