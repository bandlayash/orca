# Orca Performance Master Plan
## Goal: Near-Native Wii Speed in the Browser

### Current State
- **Build System**: Fixed. Removed deprecated `-sDEMANGLE_SUPPORT` flag from `build.sh` and `CMakeLists.txt`. Project now compiles successfully with Emscripten 5.0.3.
- **AOT Transpiler**: Phase 1.1 fix (`slice()` instead of `subarray()`) is already present in `AotWasm.cpp`. This is intended to solve the `SharedArrayBuffer` issue when creating `WebAssembly.Module`. **Next: Verify runtime behavior.**
- **Multi-threading**: Scaffolding for `OffscreenCanvas` and dual-core (separate CPU and GPU threads) is implemented in `Core.cpp`, `EmscriptenGL.cpp`, and `PlatformWeb.cpp`.
- **Instruction Coverage**: `ps_` (Paired Single) opcodes are largely implemented. String and Cache instructions are currently missing.
- **Features**: Audio is currently disabled (`NullSoundStream`). Input handling is not yet implemented.

---

## Phase 1: AOT Verification & Stability (CRITICAL)

### 1.1 Verify AOT Runtime Stability
- Confirm that `Module.HEAPU8.slice()` successfully avoids the "first argument must be an ArrayBuffer" error in the browser.
- Monitor `aot_interp_fallback` logs to ensure blocks are actually compiling and running natively instead of falling back.

### 1.2 Verify Block Chaining
- Ensure `aot_wasm_chain` correctly dispatches between compiled blocks without returning to C++, minimizing transition overhead.

---

## Phase 2: Multi-Threading Validation

### 2.1 Verify GPU Thread Independence
- Confirm that the "Video thread" is correctly spawned in `Core.cpp` and runs the GPU loop independently.
- Use Browser DevTools to verify that CPU and GPU work are distributed across different Web Workers.

### 2.2 Fix/Verify OffscreenCanvas Transfer
- Ensure the `#canvas` element is successfully transferred to the GPU worker.
- Verify that `EMSCRIPTEN_WEBGL_CONTEXT_PROXY_DISALLOW` correctly prevents deadlocks between the GPU worker and the main thread.

---

## Phase 3: Expand AOT Instruction Coverage

### 3.1 Implement Missing "Bizarro" Instructions
- **String Instructions**: `lswi`, `stswi`, `lswx`, `stswx`. These are critical for many Wii games' library functions and currently trigger fallbacks.
- **Cache Management**: `dcbi`, `dcbt`, `dcbtst`, `dcbf`, `dcbst`, `icbi`. Required for correct JIT/AOT consistency and performance.
- **System Registers**: Refine `mfmsr`/`mtmsr` and other SPR accessors.

### 3.2 Improve Fallback Efficiency
- Implement batching for consecutive fallback instructions to reduce the cost of WASM <-> C++ transitions.

---

## Phase 4: AOT Code Quality & SIMD

### 4.1 Mega-module Compilation
- Batch multiple hot blocks into larger WASM modules to reduce per-module instantiation overhead.

### 4.2 WASM SIMD Optimizations
- Use `-msimd128` to optimize endian swaps and `ps_` (paired-single) operations.

---

## Phase 5: Memory Access Optimization

### 5.1 BAT-aware Address Translation
- Inline BAT configuration checks into WASM blocks to bypass MMU calls for common userspace memory ranges.

---

## Phase 6: GPU & Rendering Optimization

### 6.1 Fix WebGL Uniform Buffer Warning
- "Buffer for uniform block is smaller than UNIFORM_BLOCK_DATA_SIZE".
- Ensure `VideoBackends/OGL/` UBO allocations meet minimum size requirements for WebGL2.

---

## Phase 7: Audio & Input (Usability)

### 7.1 WebAudio / AudioWorklet
- Replace `NullSoundStream` with a high-performance AudioWorklet backend to enable low-latency sound.

### 7.2 Gamepad API Input
- Map browser Gamepad API and keyboard events to Dolphin's input system.

---

## Phase 8: Advanced Optimizations

### 8.1 Profile-guided Recompilation
- Track execution counts and recompile the "hottest" blocks with higher optimization levels.

### 8.2 IndexedDB Block Cache
- Cache compiled WASM modules in IndexedDB to skip recompilation on subsequent loads.
