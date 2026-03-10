#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[ERROR] This helper is for macOS (Darwin)." >&2
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

cd "$ROOT_DIR"

echo "[INFO] Configuring preset macos-hklmreg-release ..."
cmake --preset macos-hklmreg-release

echo "[INFO] Building preset macos-hklmreg-release ..."
cmake --build --preset macos-hklmreg-release

echo "[OK] Build complete: build/macos-hklmreg-release/hklmreg"
