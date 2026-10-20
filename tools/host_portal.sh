#!/bin/sh
# Builds the host portal preview (tools/host_portal.cpp) into .pio/host_portal.
set -e
cd "$(dirname "$0")/.."
SRCS=$(ls src/core/*.cpp src/storage/*.cpp src/app_core/*.cpp src/ui_core/*.cpp src/ui_core/assets/*.cpp src/net_core/*.cpp | grep -v -e sd_selftest -e arduino_fs)
mkdir -p .pio
g++ -std=gnu++17 -O1 -Isrc -Itest/common $SRCS tools/host_portal.cpp -o .pio/host_portal
echo built .pio/host_portal
