#!/usr/bin/env bash
# the Windows build (MSYS2 UCRT64: gcc, cmake, ninja) into build_win/, the self-test, and the processor folder
# as it goes onto a stick - dist/unzip/ with bin/windows-x86_64/unzip.exe
set -e
cd "$(dirname "$0")"
mkdir -p build_win
cmake -S . -B build_win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_win
(cd build_win && ctest --output-on-failure)
rm -rf dist/unzip
mkdir -p dist/unzip/bin/windows-x86_64
cp package/processor.ini README.md LICENSE dist/unzip/
cp third_party/miniz/LICENSE dist/unzip/LICENSE.miniz
cp third_party/libarchive/COPYING dist/unzip/LICENSE.libarchive
cp third_party/xz/COPYING.0BSD dist/unzip/LICENSE.liblzma
cp build_win/unzip.exe dist/unzip/bin/windows-x86_64/
echo "dist/unzip is ready: copy it to <stick>/System/Processors/unzip"
