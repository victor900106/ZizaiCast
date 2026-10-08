#!/usr/bin/env bash
# Fetches onnxruntime.dll 1.30.0 (Microsoft ONNX Runtime, MIT, CPU execution
# provider) for PaddleOCR: the official build from the PyPI wheel
# onnxruntime-1.30.0-cp312-cp312-win_amd64.whl (SHA-256 pinned; the same DLL
# as onnxruntime-win-x64-1.30.0.zip on GitHub, a smaller download), plus its
# LICENSE and ThirdPartyNotices.txt.  Output: build-translate/ort/ (the CMake
# cache path PM_ONNXRUNTIME_DLL points there).  Imports: system DLLs + the VC++
# runtime (msvcp140, msvcp140_1, vcruntime140, vcruntime140_1), shipped
# app-local by app/CMakeLists.txt.
#   translate/tools/get_onnxruntime.sh [out-dir]
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="${1:-build-translate/ort}"
URL="https://files.pythonhosted.org/packages/a6/13/0f1699f6de549c9324bc9112a2a85b14c517904cd11b562a654643b755a1/onnxruntime-1.30.0-cp312-cp312-win_amd64.whl"
SHA="f3501472571f1b1eee50e017851e7929f5ea37312d2d8c2494a19e8fc58b4a38"
mkdir -p "$OUT"
whl="$OUT/onnxruntime-1.30.0-cp312-cp312-win_amd64.whl"
[ -f "$whl" ] || curl -fL --retry 3 -o "$whl" "$URL"
echo "$SHA *$whl" | sha256sum -c -
tmp="$OUT/whl"
rm -rf "$tmp"
mkdir -p "$tmp"
unzip -q -o "$whl" 'onnxruntime/capi/onnxruntime.dll' 'onnxruntime/LICENSE' 'onnxruntime/ThirdPartyNotices.txt' -d "$tmp"
cp "$tmp/onnxruntime/capi/onnxruntime.dll" "$OUT/onnxruntime.dll"
cp "$tmp/onnxruntime/LICENSE" "$OUT/LICENSE-onnxruntime.txt"
cp "$tmp/onnxruntime/ThirdPartyNotices.txt" "$OUT/ThirdPartyNotices-onnxruntime.txt"
rm -rf "$tmp"
ls -la "$OUT/onnxruntime.dll"
