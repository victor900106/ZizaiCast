#!/usr/bin/env bash
# Builds bergamot.dll (Mozilla/browsermt Bergamot translation engine, MPL-2.0)
# for pm_translate: x64, static CRT, NO Intel MKL / no BLAS (Marian's
# Eigen-based ONNX sgemm + intgemm int8), so the binary is GPL-compatible.
#
#   translate/tools/build_bergamot.sh [build-dir]      (Git Bash, VS 2026 + vcpkg)
#
# Output: <build-dir>/bergamot/bin/bergamot.dll  (copy next to PhoneMirror.exe)
# Sources: BergamotTranslatorSharp (MPL-2.0; C API + Windows patches) pinned
# below, with its bergamot-translator / marian-dev submodules.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HERE/../build-translate}"
SRC="$OUT/bts-src"
BLD="$OUT/bergamot-build"
VCPKG="${VCPKG_ROOT:-$(cygpath -m "$USERPROFILE")/vcpkg}"
COMMIT=e084db279f0d4314b31c7730cfc61ab03f235604   # Freeesia/BergamotTranslatorSharp, 2026-10-02

if [ ! -d "$SRC/.git" ]; then
  git clone -q https://github.com/Freeesia/BergamotTranslatorSharp.git "$SRC"
fi
git -C "$SRC" fetch -q origin "$COMMIT" 2>/dev/null || true
git -C "$SRC" -c advice.detachedHead=false checkout -q "$COMMIT"
git -C "$SRC" submodule update --init --recursive -q

# 1) Let USE_ONNX_SGEMM be chosen outside the WASM build.
MCM="$SRC/bergamot-translator/3rd_party/marian-dev/CMakeLists.txt"
if ! grep -q "PM_ONNX_SGEMM" "$MCM"; then
  sed -i '/^CMAKE_DEPENDENT_OPTION(USE_ONNX_SGEMM/{N;s/.*/option(USE_ONNX_SGEMM "Compile with wasm compatible blas (PM_ONNX_SGEMM)" OFF)/}' "$MCM"
fi
# 2) Stubs for faiss' Fortran BLAS/LAPACK symbols.
DYN="$SRC/bergamot-translator-dynamic/CMakeLists.txt"
cp "$HERE/bergamot/lapack_stubs.cpp" "$SRC/bergamot-translator-dynamic/src/"
grep -q lapack_stubs "$DYN" || sed -i 's#add_library(bergamot_translator_dynamic SHARED src/bergamot.cpp)#add_library(bergamot_translator_dynamic SHARED src/bergamot.cpp src/lapack_stubs.cpp)#' "$DYN"

cmake -S "$SRC" -B "$BLD" -G "Visual Studio 18 2026" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_MODE=OFF \
  -DBUILD_ARCH=x86-64-v2 -DUSE_STATIC_LIBS=ON -DUSE_MKL=OFF -DUSE_ONNX_SGEMM=ON \
  -DGIT_SUBMODULE=OFF -DCOMPILE_TESTS=OFF -DCOMPILE_UNIT_TESTS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$BLD" --config Release --target bergamot_translator_dynamic -- -m
mkdir -p "$OUT/bergamot/bin"
cp "$BLD"/bergamot-translator-dynamic/Release/bergamot.dll "$OUT/bergamot/bin/"
echo "built: $OUT/bergamot/bin/bergamot.dll"
