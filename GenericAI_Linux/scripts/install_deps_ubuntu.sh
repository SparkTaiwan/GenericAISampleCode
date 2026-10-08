#!/usr/bin/env bash
# Build dependencies for GenericAI_Linux on Ubuntu 22.04 / 24.04 (run as root or via sudo).
# onnxruntime is not packaged by Ubuntu: run scripts/fetch_onnxruntime.sh afterwards.
set -euo pipefail

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

$SUDO apt-get update
$SUDO apt-get install -y \
    build-essential cmake ninja-build pkg-config curl \
    libzmq3-dev \
    libavcodec-dev libavutil-dev libswscale-dev \
    libturbojpeg0-dev \
    nlohmann-json3-dev libcpp-httplib-dev

# Only needed for tests/simulator.py (the recorder-side simulator):
#   apt-get install -y python3-zmq python3-numpy ffmpeg
