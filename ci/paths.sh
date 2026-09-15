#!/usr/bin/env bash
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
workspace_id=$(printf '%s' "$source_dir" | sha256sum | cut -c1-16)
build_dir="$HOME/.cache/local-fusefs/$workspace_id/build"
