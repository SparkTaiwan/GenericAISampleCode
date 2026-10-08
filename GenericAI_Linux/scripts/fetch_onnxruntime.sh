#!/usr/bin/env bash
# Downloads a prebuilt onnxruntime release into third_party/onnxruntime
# (the default ONNXRUNTIME_ROOT in CMakeLists.txt).
#
#   scripts/fetch_onnxruntime.sh            # GPU package (CUDA 12 + cuDNN 9), falls back to CPU at runtime
#   scripts/fetch_onnxruntime.sh cpu        # CPU-only package
#   ORT_VERSION=1.20.1 scripts/fetch_onnxruntime.sh
#
# The GPU package still runs on machines without NVIDIA/CUDA: the CUDA EP
# fails to load and the Person detector falls back to CPU.
set -euo pipefail

FLAVOR="${1:-gpu}"
ORT_VERSION="${ORT_VERSION:-1.20.1}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$HERE/third_party/onnxruntime"

case "$FLAVOR" in
  gpu) PKG="onnxruntime-linux-x64-gpu-${ORT_VERSION}" ;;
  cpu) PKG="onnxruntime-linux-x64-${ORT_VERSION}" ;;
  *) echo "usage: $0 [gpu|cpu]" >&2; exit 2 ;;
esac

URL="https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${PKG}.tgz"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Downloading $URL"
curl -fL --retry 3 -o "$TMP/ort.tgz" "$URL"
tar -xzf "$TMP/ort.tgz" -C "$TMP"
rm -rf "$DEST"
mkdir -p "$(dirname "$DEST")"
mv "$TMP/$PKG" "$DEST"
echo "onnxruntime $ORT_VERSION ($FLAVOR) -> $DEST"
