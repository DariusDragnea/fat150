#!/bin/bash
ARCH=$(uname -m)
DIR="$(dirname "$0")"

if [ "$ARCH" = "x86_64" ]; then
    exec "$DIR/.reference/fs_ref-amd64" "$@"
elif [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    exec "$DIR/.reference/fs_ref-arm64" "$@"
else
    echo "Unsupported architecture: $ARCH"
    exit 1
fi
