#!/usr/bin/env bash
# build check
set -e
cd "$(git rev-parse --show-toplevel)"
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j
