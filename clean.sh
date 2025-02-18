#!/usr/bin/env bash
# Remove all build artifacts. Leaves only source + config needed to build.
set -e
cd "$(dirname "$0")"

rm -rf build
rm -rf CMakeFiles
rm -f CMakeCache.txt
rm -f cmake_install.cmake
rm -f Makefile
rm -f limonitor limonitor_integration_test
find . -name '*.o' -delete
find . -name '*.a' -delete

echo "clean"
