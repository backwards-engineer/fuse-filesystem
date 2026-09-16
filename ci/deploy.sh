#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/paths.sh"
cd "$source_dir"
# User-owned staging installation, identified by the tested commit.
# Windows owns this checkout; trust only this explicit workspace for this command.
revision=$(git -c safe.directory="$source_dir" rev-parse HEAD)
destination="$HOME/.local/opt/local-fusefs/releases/$revision"
mkdir -p "$destination"
install -m 0755 "$build_dir/local-fusefs" "$destination/local-fusefs"
install -m 0755 "$build_dir/fusefs-admin" "$destination/fusefs-admin"
install -m 0755 "$build_dir/fusefs-bench" "$destination/fusefs-bench"
ln -sfn "$destination" "$HOME/.local/opt/local-fusefs/current"
"$HOME/.local/opt/local-fusefs/current/local-fusefs" --version
echo "Deployed $revision to $destination"
