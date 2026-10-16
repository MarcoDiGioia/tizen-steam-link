#!/usr/bin/env bash
# Builds tsl-cli and the unit tests for this machine, then runs the tests.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build/desktop -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
cmake --build build/desktop
ctest --test-dir build/desktop --output-on-failure
echo "tsl-cli: build/desktop/src/desktop/tsl-cli"
