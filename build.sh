#!/usr/bin/env bash
set -euo pipefail

if [ -z "${EMSDK:-}" ]; then
  echo "Error: EMSDK is not set. Source emsdk_env.sh first." >&2
  exit 1
fi

source "$EMSDK/emsdk_env.sh"

emcmake cmake -S dolphin -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_GENERIC=ON \
  -DENABLE_NOGUI=ON \
  -DENABLE_QT=OFF \
  -DENABLE_HEADLESS=OFF \
  -DUSE_SYSTEM_LIBS=OFF \
  -DCMAKE_C_FLAGS="-pthread" \
  -DCMAKE_CXX_FLAGS="-pthread" \
  -DCMAKE_EXE_LINKER_FLAGS="-sASSERTIONS=2 -sDEMANGLE_SUPPORT=1 -g2 -sINITIAL_MEMORY=536870912 -sALLOW_MEMORY_GROWTH=0"

emmake ninja -C build-wasm dolphin-nogui -j$(nproc)

mkdir -p dist
cp build-wasm/Binaries/dolphin.js   dist/
cp build-wasm/Binaries/dolphin.wasm dist/
cp build-wasm/Binaries/dolphin.data dist/ 2>/dev/null || true
cp build-wasm/Binaries/dolphin.worker.js dist/ 2>/dev/null || true
cp web/index.html web/main.js web/style.css web/_headers web/netlify.toml dist/

echo "Done. Run: python3 serve.py"
