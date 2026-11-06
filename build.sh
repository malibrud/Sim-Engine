#!/bin/sh
# ---------------------------------------------------------------------------
#  Reference build for `sec`, the sim-engine compiler driver.
#
#  A full, from-scratch compile into a single translation unit: the build model
#  SPECIFICATION.md section 12.4 prescribes for generated models, applied to
#  the compiler itself. No object files, so "incremental" is not a feature
#  that was skipped but a category that does not exist.
#
#  The POSIX twin of build.bat. Uses $CXX if set, else c++ (g++ or clang++).
#      ./build.sh           release
#      ./build.sh debug     -O0 -g
#
#  LF only: sh chokes on CRLF line endings.
# ---------------------------------------------------------------------------
set -e

cd "$(dirname "$0")"

CXX=${CXX:-c++}
if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "build: \`$CXX\` not found. Install g++ or clang++, or set CXX." >&2
    exit 2
fi

mkdir -p build

FLAGS="-std=c++17 -Wall -Wextra"
if [ "$1" = "debug" ]; then
    FLAGS="$FLAGS -O0 -g"
else
    FLAGS="$FLAGS -O2 -DNDEBUG"
fi

echo "$CXX $FLAGS"
if ! $CXX $FLAGS -o build/sec src/unity.cpp; then
    echo "build: FAILED" >&2
    exit 1
fi

echo "build: build/sec"
