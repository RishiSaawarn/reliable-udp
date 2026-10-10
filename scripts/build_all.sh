#!/usr/bin/env bash
# Configure and build every supported configuration.
#
#   release  -O2, -Werror, the configuration every benchmark number comes from
#   debug    -O0 -g, assertions live, no sanitizer
#   asan     RelWithDebInfo + AddressSanitizer + UndefinedBehaviorSanitizer
#   tsan     RelWithDebInfo + ThreadSanitizer
#
# ASan and TSan are mutually exclusive (CMake enforces this), hence two trees.
set -euo pipefail
cd "$(dirname "$0")/.."

JOBS="${JOBS:-$(nproc)}"

build() {
  local name="$1"; shift
  echo "=== configuring $name ==="
  cmake -S . -B "build/$name" "$@"
  echo "=== building $name ==="
  cmake --build "build/$name" -j "$JOBS"
}

build release -DCMAKE_BUILD_TYPE=Release
build debug   -DCMAKE_BUILD_TYPE=Debug
build asan    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRUDP_ASAN=ON
build tsan    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRUDP_TSAN=ON

echo
echo "all configurations built"
