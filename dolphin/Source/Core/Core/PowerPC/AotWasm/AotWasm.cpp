// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AotWasm/AotWasm.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/ostream.h>

#include "Common/CommonTypes.h"
#include "Common/GekkoDisassembler.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/Host.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

// =====================================================================
// PowerPCState field offsets (computed at compile time)
// =====================================================================
#define PPC_OFF(field) (static_cast<u32>(offsetof(PowerPC::PowerPCState, field)))

static constexpr u32 OFF_PC = PPC_OFF(pc);
static constexpr u32 OFF_NPC = PPC_OFF(npc);
static constexpr u32 OFF_GPR = PPC_OFF(gpr);
static constexpr u32 OFF_PS = PPC_OFF(ps);
static constexpr u32 OFF_CR = PPC_OFF(cr);
static constexpr u32 OFF_MSR = PPC_OFF(msr);
static constexpr u32 OFF_EXCEPTIONS = PPC_OFF(Exceptions);
static constexpr u32 OFF_DOWNCOUNT = PPC_OFF(downcount);
static constexpr u32 OFF_XER_CA = PPC_OFF(xer_ca);
static constexpr u32 OFF_XER_SO_OV = PPC_OFF(xer_so_ov);
static constexpr u32 OFF_SPR = PPC_OFF(spr);
static constexpr u32 OFF_RESERVE = PPC_OFF(reserve);
static constexpr u32 OFF_RESERVE_ADDR = PPC_OFF(reserve_address);
static constexpr u32 OFF_FPSCR = PPC_OFF(fpscr);
static constexpr u32 OFF_XER_STRINGCTRL = PPC_OFF(xer_stringctrl);

// SPR indices
static constexpr u32 SPR_LR_IDX = 8;
static constexpr u32 SPR_CTR_IDX = 9;

// Exception masks
static constexpr u32 EXCEPT_DSI_PROGRAM = 0x00000088;  // DSI=0x08 | PROGRAM=0x80

// WASM opcodes
namespace WasmOp
{
constexpr u8 Block = 0x02;
constexpr u8 Loop = 0x03;
constexpr u8 If = 0x04;
constexpr u8 Else = 0x05;
constexpr u8 End = 0x0b;
constexpr u8 Br = 0x0c;
constexpr u8 BrIf = 0x0d;
constexpr u8 Return = 0x0f;
constexpr u8 Call = 0x10;
constexpr u8 Select = 0x1b;
constexpr u8 Drop = 0x1a;
constexpr u8 LocalGet = 0x20;
constexpr u8 LocalSet = 0x21;
constexpr u8 LocalTee = 0x22;
constexpr u8 I32Load = 0x28;
constexpr u8 I32Store = 0x36;
constexpr u8 I32Const = 0x41;
constexpr u8 I32Eqz = 0x45;
constexpr u8 I32Eq = 0x46;
constexpr u8 I32Ne = 0x47;
constexpr u8 I32LtS = 0x48;
constexpr u8 I32LtU = 0x49;
constexpr u8 I32GtS = 0x4a;
constexpr u8 I32GtU = 0x4b;
constexpr u8 I32LeS = 0x4c;
constexpr u8 I32GeS = 0x4e;
constexpr u8 I32Add = 0x6a;
constexpr u8 I32Sub = 0x6b;
constexpr u8 I32Mul = 0x6c;
constexpr u8 I32And = 0x71;
constexpr u8 I32Or = 0x72;
constexpr u8 I32Xor = 0x73;
constexpr u8 I32Shl = 0x74;
constexpr u8 I32ShrS = 0x75;
constexpr u8 I32ShrU = 0x76;
constexpr u8 I32Clz = 0x67;
constexpr u8 I32Rotl = 0x77;
constexpr u8 I32DivS = 0x6d;
constexpr u8 I32DivU = 0x6e;
constexpr u8 I32RemS = 0x6f;
constexpr u8 I32Load8U = 0x2d;
constexpr u8 I32Load16U = 0x2f;
constexpr u8 I32Store8 = 0x3a;
constexpr u8 I32Store16 = 0x3b;
constexpr u8 I64Store = 0x37;
constexpr u8 I64Const = 0x42;
constexpr u8 I64Or = 0x84;
constexpr u8 I64Shl = 0x86;
constexpr u8 I64ExtendI32U = 0xad;
constexpr u8 I64ExtendI32S = 0xac;
constexpr u8 I64Load = 0x29;
constexpr u8 I64And = 0x83;
constexpr u8 I64Xor = 0x85;
constexpr u8 I64ShrU = 0x88;
constexpr u8 I64ShrS = 0x87;
constexpr u8 I64Add = 0x7c;
constexpr u8 I64Sub = 0x7d;
constexpr u8 I64Mul = 0x7e;
constexpr u8 I32WrapI64 = 0xa7;
constexpr u8 I64Eqz = 0x50;
constexpr u8 I64GtS = 0x55;
constexpr u8 I32GeU = 0x4f;
// Float opcodes
constexpr u8 F64Load = 0x2b;
constexpr u8 F64Store = 0x39;
constexpr u8 F64Add = 0xa0;
constexpr u8 F64Sub = 0xa1;
constexpr u8 F64Mul = 0xa2;
constexpr u8 F64Div = 0xa3;
constexpr u8 F64Abs = 0x99;
constexpr u8 F64Neg = 0x9a;
constexpr u8 F64Sqrt = 0x9f;
constexpr u8 F64Eq = 0x61;
constexpr u8 F64Lt = 0x63;
constexpr u8 F64Gt = 0x64;
constexpr u8 F64Ne = 0x62;
constexpr u8 F64PromoteF32 = 0xbb;
constexpr u8 F32DemoteF64 = 0xb6;
constexpr u8 F64ReinterpretI64 = 0xbf;
constexpr u8 I64ReinterpretF64 = 0xbd;
constexpr u8 F32ReinterpretI32 = 0xbe;
constexpr u8 I32ReinterpretF32 = 0xbc;
constexpr u8 I32TruncF64S = 0xaa;
constexpr u8 F64ConvertI32S = 0xb7;
constexpr u8 ReturnCallIndirect = 0x13;  // tail call via table (tail-call proposal)
constexpr u8 Void = 0x40;
constexpr u8 I32 = 0x7f;
constexpr u8 I64 = 0x7e;
constexpr u8 F64 = 0x7c;
constexpr u8 F32 = 0x7d;
constexpr u8 V128 = 0x7b;
// SIMD prefix byte: all SIMD opcodes are encoded as 0xFD + LEB128(opcode)
constexpr u8 SIMDPrefix = 0xfd;
// SIMD opcode numbers (encoded as LEB128 after the prefix)
constexpr u32 V128Load = 0x00;
constexpr u32 V128Store = 0x0b;
constexpr u32 F64x2ExtractLane = 0x1d;
constexpr u32 F64x2ReplaceLane = 0x22;
constexpr u32 F64x2Splat = 0x14;
constexpr u32 F64x2Abs = 0xec;
constexpr u32 F64x2Neg = 0xed;
constexpr u32 F64x2Sqrt = 0xef;
constexpr u32 F64x2Add = 0xf0;
constexpr u32 F64x2Sub = 0xf1;
constexpr u32 F64x2Mul = 0xf2;
constexpr u32 F64x2Div = 0xf3;
constexpr u32 F32x4DemoteF64x2Zero = 0x5e;
constexpr u32 F64x2PromoteLowF32x4 = 0x5f;
}  // namespace WasmOp

// =====================================================================
// LEB128 encoding helpers
// =====================================================================
static void EncodeU32(std::vector<u8>& out, u32 value)
{
  do
  {
    u8 byte = value & 0x7f;
    value >>= 7;
    if (value != 0)
      byte |= 0x80;
    out.push_back(byte);
  } while (value != 0);
}

static void EncodeS32(std::vector<u8>& out, s32 value)
{
  bool more = true;
  while (more)
  {
    u8 byte = value & 0x7f;
    value >>= 7;
    if ((value == 0 && (byte & 0x40) == 0) || (value == -1 && (byte & 0x40) != 0))
      more = false;
    else
      byte |= 0x80;
    out.push_back(byte);
  }
}

static void EncodeString(std::vector<u8>& out, const char* str)
{
  u32 len = static_cast<u32>(std::strlen(str));
  EncodeU32(out, len);
  out.insert(out.end(), str, str + len);
}

static void WriteSection(std::vector<u8>& module, u8 section_id, const std::vector<u8>& content)
{
  module.push_back(section_id);
  EncodeU32(module, static_cast<u32>(content.size()));
  module.insert(module.end(), content.begin(), content.end());
}

// =====================================================================
// JavaScript Bridge (Emscripten only)
// =====================================================================
#ifdef __EMSCRIPTEN__

// Interpreter fallback: called from WASM modules for unimplemented instructions
extern "C" EMSCRIPTEN_KEEPALIVE void aot_interp_fallback(u32 ppc_state_addr, u32 pc,
                                                          u32 inst_hex)
{
  // Diagnostic logging: track unique opcodes and total fallback count
  static std::unordered_set<u32> seen_opcodes;
  static u32 fallback_count = 0;

  ++fallback_count;
  const u32 opcode = (inst_hex >> 26) & 0x3F;
  if (seen_opcodes.find(opcode) == seen_opcodes.end())
  {
    seen_opcodes.insert(opcode);
    std::fprintf(stderr, "[orca-aot] fallback: pc=0x%08x inst=0x%08x opcode=%u\n",
                 pc, inst_hex, opcode);
  }
  if ((fallback_count % 10000) == 0)
  {
    std::fprintf(stderr, "[orca-aot] fallback count: %u\n", fallback_count);
  }

  auto& system = Core::System::GetInstance();
  auto& interpreter = system.GetInterpreter();
  auto& ppc_state = system.GetPPCState();

  ppc_state.pc = pc;
  ppc_state.npc = pc + 4;

  UGeckoInstruction inst;
  inst.hex = inst_hex;
  Interpreter::RunInterpreterOp(interpreter, inst);
}

// Memory read/write functions called from WASM modules
extern "C" EMSCRIPTEN_KEEPALIVE u32 aot_read_u8(u32 addr)
{
  auto& system = Core::System::GetInstance();
  auto& mmu = system.GetMMU();
  return mmu.Read<u8>(addr);
}

extern "C" EMSCRIPTEN_KEEPALIVE u32 aot_read_u16(u32 addr)
{
  auto& system = Core::System::GetInstance();
  auto& mmu = system.GetMMU();
  return mmu.Read<u16>(addr);
}

extern "C" EMSCRIPTEN_KEEPALIVE u32 aot_read_u32(u32 addr)
{
  auto& system = Core::System::GetInstance();
  auto& mmu = system.GetMMU();
  return mmu.Read<u32>(addr);
}

extern "C" EMSCRIPTEN_KEEPALIVE void aot_write_u8(u32 addr, u32 val)
{
  auto& system = Core::System::GetInstance();
  auto& mmu = system.GetMMU();
  mmu.Write<u8>(static_cast<u8>(val), addr);
}

extern "C" EMSCRIPTEN_KEEPALIVE void aot_write_u16(u32 addr, u32 val)
{
  auto& system = Core::System::GetInstance();
  auto& mmu = system.GetMMU();
  mmu.Write<u16>(static_cast<u16>(val), addr);
}

extern "C" EMSCRIPTEN_KEEPALIVE void aot_write_u32(u32 addr, u32 val)
{
  auto& system = Core::System::GetInstance();
  auto& mmu = system.GetMMU();
  mmu.Write<u32>(val, addr);
}

// JS bridge: init WASM table and detect tail call support.
// Returns 1 if return_call_indirect is supported, 0 otherwise.
EM_JS(int, aot_wasm_init_table, (), {
  // Create shared function table for call_indirect dispatch
  Module._aotTable = new WebAssembly.Table({
    initial : 4096, maximum : 65536, element : 'anyfunc'
  });
  Module._aotTableNext = 1;  // index 0 = null (reserved)

  // Detect tail call (return_call_indirect) support
  try {
    var test = new Uint8Array([
      0x00,0x61,0x73,0x6d, 0x01,0x00,0x00,0x00,
      0x01,0x06,0x01, 0x60,0x01,0x7f, 0x01,0x7f,
      0x02,0x0f,0x01, 0x03,0x65,0x6e,0x76, 0x05,0x74,0x61,0x62,0x6c,0x65,
        0x01,0x70,0x00,0x01,
      0x03,0x02,0x01,0x00,
      0x0a,0x0b,0x01,0x09,0x00, 0x20,0x00, 0x41,0x00, 0x13,0x00,0x00, 0x0b
    ]);
    new WebAssembly.Module(test);
    if (typeof console !== 'undefined')
      console.log("[AOT] return_call_indirect supported, WASM-native dispatch enabled");
    return 1;
  } catch(e) {
    if (typeof console !== 'undefined')
      console.log("[AOT] return_call_indirect not supported, using JS dispatch loop");
    return 0;
  }
});

// JS bridge: add function to WebAssembly.Table, return table index
EM_JS(int, aot_wasm_table_add, (int funcId), {
  var func = Module._aotFuncs[funcId];
  var table = Module._aotTable;
  var idx = Module._aotTableNext++;
  if (idx >= table.length)
    table.grow(Math.min(4096, 65536 - table.length));
  table.set(idx, func);
  return idx;
});

// JS bridge: compile WASM bytes into a callable function, return its ID
EM_JS(int, aot_wasm_compile, (const uint8_t* bytesPtr, int bytesLen), {
  try {
    // Cache the shared import object to avoid recreating per block
    if (!Module._aotImports) {
      Module._aotImports = {
        env : {
          memory : wasmMemory,
          table : Module._aotTable,
          fallback : Module._aot_interp_fallback,
          read_u8 : Module._aot_read_u8,
          read_u16 : Module._aot_read_u16,
          read_u32 : Module._aot_read_u32,
          write_u8 : Module._aot_write_u8,
          write_u16 : Module._aot_write_u16,
          write_u32 : Module._aot_write_u32,
        }
      };
    }
    var bytes = Module.HEAPU8.slice(bytesPtr, bytesPtr + bytesLen);
    var mod = new WebAssembly.Module(bytes.buffer);
    var instance = new WebAssembly.Instance(mod, Module._aotImports);
    if (!Module._aotFuncs)
      Module._aotFuncs = [];
    var id = Module._aotFuncs.length;
    Module._aotFuncs.push(instance.exports.run);
    return id;
  } catch (e) {
    if (typeof console !== 'undefined')
      console.error("AOT WASM compile error:", e.message || e);
    return -1;
  }
});

// JS bridge: call a compiled WASM block
EM_JS(void, aot_wasm_call, (int funcId, int ppcStatePtr), {
  Module._aotFuncs[funcId](ppcStatePtr);
});

// JS bridge: register a block in the JS dispatch tables.
EM_JS(void, aot_wasm_register, (int funcId, int blockAddress), {
  if (!Module._aotBlockMap)
    Module._aotBlockMap = new Map();
  Module._aotBlockMap.set(blockAddress, funcId);
  if (!Module._aotFastTable) {
    Module._aotFastTable = new Int32Array(524288);
    Module._aotFastTable.fill(-1);
  }
  var ftab = Module._aotFastTable;
  var h1 = ((blockAddress >>> 2) & 0x3FFFF) << 1;
  var h2 = (((blockAddress >>> 18) ^ (blockAddress >>> 2)) & 0x3FFFF) << 1;
  if (ftab[h1] < 0 || ftab[h1] === blockAddress) {
    ftab[h1] = blockAddress;
    ftab[h1 + 1] = funcId;
    return;
  }
  if (ftab[h2] < 0 || ftab[h2] === blockAddress) {
    ftab[h2] = blockAddress;
    ftab[h2 + 1] = funcId;
    return;
  }
  var evictAddr = ftab[h1];
  var evictId = ftab[h1 + 1];
  ftab[h1] = blockAddress;
  ftab[h1 + 1] = funcId;
  for (var i = 0; i < 5; i++) {
    var eh1 = ((evictAddr >>> 2) & 0x3FFFF) << 1;
    var eh2 = (((evictAddr >>> 18) ^ (evictAddr >>> 2)) & 0x3FFFF) << 1;
    var slot = (eh1 === h1) ? eh2 : eh1;
    if (ftab[slot] < 0) {
      ftab[slot] = evictAddr;
      ftab[slot + 1] = evictId;
      return;
    }
    var tmpAddr = ftab[slot];
    var tmpId = ftab[slot + 1];
    ftab[slot] = evictAddr;
    ftab[slot + 1] = evictId;
    evictAddr = tmpAddr;
    evictId = tmpId;
    h1 = slot;
  }
});

// JS bridge: call a WASM block and chain to subsequent compiled blocks.
// With tail calls enabled, blocks handle most dispatch via return_call_indirect.
// This loop only executes on dispatch misses (block returns non-zero next_pc
// that wasn't found in the WASM dispatch table).
EM_JS(void, aot_wasm_chain, (int funcId, int ppcStatePtr,
                              int offPC, int offDowncount, int offExceptions), {
  var funcs = Module._aotFuncs;
  var ftab = Module._aotFastTable;
  var map = Module._aotBlockMap;
  var result = funcs[funcId](ppcStatePtr);
  while (result !== 0) {
    var h1 = ((result >>> 2) & 0x3FFFF) << 1;
    if (ftab[h1] === result) {
      result = funcs[ftab[h1 + 1]](ppcStatePtr);
      continue;
    }
    var h2 = (((result >>> 18) ^ (result >>> 2)) & 0x3FFFF) << 1;
    if (ftab[h2] === result) {
      result = funcs[ftab[h2 + 1]](ppcStatePtr);
      continue;
    }
    var nextId = map.get(result);
    if (nextId === undefined) return;
    result = funcs[nextId](ppcStatePtr);
  }
});

// JS bridge: free a compiled WASM block
EM_JS(void, aot_wasm_free, (int funcId), {
  if (Module._aotFuncs)
    Module._aotFuncs[funcId] = null;
});

// JS bridge: unregister a block from the dispatch table
EM_JS(void, aot_wasm_unregister, (int blockAddress), {
  if (Module._aotBlockMap)
    Module._aotBlockMap.delete(blockAddress);
  if (Module._aotFastTable) {
    var ftab = Module._aotFastTable;
    var h1 = ((blockAddress >>> 2) & 0x3FFFF) << 1;
    if (ftab[h1] === blockAddress) {
      ftab[h1] = -1;
      ftab[h1 + 1] = -1;
    }
    var h2 = (((blockAddress >>> 18) ^ (blockAddress >>> 2)) & 0x3FFFF) << 1;
    if (ftab[h2] === blockAddress) {
      ftab[h2] = -1;
      ftab[h2 + 1] = -1;
    }
  }
});

#else
// Stubs for non-Emscripten builds
static int aot_wasm_init_table() { return 0; }
static int aot_wasm_table_add(int) { return 0; }
static int aot_wasm_compile(const uint8_t*, int) { return -1; }
static void aot_wasm_call(int, int) {}
static void aot_wasm_register(int, int) {}
static void aot_wasm_chain(int, int, int, int, int) {}
static void aot_wasm_free(int) {}
static void aot_wasm_unregister(int) {}
#endif

// =====================================================================
// WASM Module Builder
// =====================================================================

// Emit: local.get <index>
static void EmitLocalGet(std::vector<u8>& code, u32 idx)
{
  code.push_back(WasmOp::LocalGet);
  EncodeU32(code, idx);
}

// Emit: local.set <index>
static void EmitLocalSet(std::vector<u8>& code, u32 idx)
{
  code.push_back(WasmOp::LocalSet);
  EncodeU32(code, idx);
}

static void EmitLocalTee(std::vector<u8>& code, u32 idx)
{
  code.push_back(WasmOp::LocalTee);
  EncodeU32(code, idx);
}

// Emit: i32.const <value>
static void EmitI32Const(std::vector<u8>& code, s32 value)
{
  code.push_back(WasmOp::I32Const);
  EncodeS32(code, value);
}

// Emit: i32.load offset=<off> align=2 (4-byte aligned)
static void EmitI32Load(std::vector<u8>& code, u32 offset)
{
  code.push_back(WasmOp::I32Load);
  EncodeU32(code, 2);       // align = 2 (2^2 = 4 bytes)
  EncodeU32(code, offset);  // offset
}

// Emit: i32.store offset=<off> align=2
static void EmitI32Store(std::vector<u8>& code, u32 offset)
{
  code.push_back(WasmOp::I32Store);
  EncodeU32(code, 2);
  EncodeU32(code, offset);
}

// Emit: call <func_index>
static void EmitCall(std::vector<u8>& code, u32 func_idx)
{
  code.push_back(WasmOp::Call);
  EncodeU32(code, func_idx);
}

// Local variable indices for the WASM function
static constexpr u32 LOCAL_STATE = 0;   // ppc_state pointer (param)
static constexpr u32 LOCAL_TMP1 = 1;    // temp variable
static constexpr u32 LOCAL_TMP2 = 2;    // temp variable
static constexpr u32 LOCAL_TMP3 = 3;    // temp variable
static constexpr u32 LOCAL_EA = 4;      // effective address for fast mem
static constexpr u32 LOCAL_NPC = 5;     // next PC (avoids memory round-trip for branches)
static constexpr u32 LOCAL_CR64 = 6;    // i64 temp for CR field construction
static constexpr u32 LOCAL_FPR0 = 7;    // f64 temp for FPU operations
static constexpr u32 LOCAL_FPR1 = 8;    // f64 temp for second FPU operand
static constexpr u32 LOCAL_GPR_BASE = 9;       // locals 9-40: GPR register cache (32 i32)
static constexpr u32 LOCAL_CR_BASE = 41;       // locals 41-48: CR field cache (8 i64)
static constexpr u32 LOCAL_FPR_V128_BASE = 49;  // locals 49-80: FPR v128 cache (32 v128, FPU blocks only)
static constexpr u32 LOCAL_SIMD_TMP0 = 81;     // v128 temp for SIMD operations
static constexpr u32 LOCAL_SIMD_TMP1 = 82;     // v128 temp for SIMD operations

// Register Cache (write-back): caches GPR and FPR values in WASM locals.
// Stores only update the cache local; dirty values are flushed to memory before
// interpreter fallback calls and block exit. This saves redundant stores when
// registers are written multiple times in a block.

struct GPRCache
{
  bool loaded[32] = {};
  bool dirty[32] = {};       // write-back: needs flush to memory
  bool is_const[32] = {};    // compile-time: value is known at codegen time
  s32 const_val[32] = {};    // compile-time: the known constant value

  // FPR cache: each FPR stored as a single v128 (ps0 in lane 0, ps1 in lane 1)
  bool fpr_loaded[32] = {};
  bool fpr_dirty[32] = {};

  // CR field cache: 8 condition register fields (each stored as i64)
  bool cr_loaded[8] = {};
  bool cr_dirty[8] = {};
  bool cr_is_const[8] = {};    // compile-time: CR field value is known
  s64 cr_const_val[8] = {};    // compile-time: the known constant CR value

  // Dead CR0 elimination: if true, Rc=1 CR0 updates can be skipped
  bool skip_cr0 = false;

  void Invalidate()
  {
    // NOTE: caller must flush dirty registers BEFORE calling Invalidate
    std::memset(loaded, 0, sizeof(loaded));
    std::memset(dirty, 0, sizeof(dirty));
    std::memset(is_const, 0, sizeof(is_const));
    std::memset(fpr_loaded, 0, sizeof(fpr_loaded));
    std::memset(fpr_dirty, 0, sizeof(fpr_dirty));
    std::memset(cr_loaded, 0, sizeof(cr_loaded));
    std::memset(cr_dirty, 0, sizeof(cr_dirty));
    std::memset(cr_is_const, 0, sizeof(cr_is_const));
  }

  void InvalidateFPR()
  {
    // NOTE: caller must flush dirty FPR registers BEFORE calling this
    std::memset(fpr_loaded, 0, sizeof(fpr_loaded));
    std::memset(fpr_dirty, 0, sizeof(fpr_dirty));
  }

  void SetConst(u32 reg, s32 val)
  {
    is_const[reg] = true;
    const_val[reg] = val;
    dirty[reg] = true;  // constant value needs to be flushed eventually
    loaded[reg] = true;
  }
};

// Push GPR[reg] value onto WASM stack without requiring state_ptr.
// Useful for store instructions where the value is needed alongside an address.
static void EmitGetGPRValue(std::vector<u8>& code, GPRCache& cache, u32 reg)
{
  if (cache.is_const[reg])
  {
    EmitI32Const(code, cache.const_val[reg]);
  }
  else if (cache.loaded[reg])
  {
    EmitLocalGet(code, LOCAL_GPR_BASE + reg);
  }
  else
  {
    EmitLocalGet(code, LOCAL_STATE);
    EmitI32Load(code, OFF_GPR + reg * 4);
    EmitLocalTee(code, LOCAL_GPR_BASE + reg);
    cache.loaded[reg] = true;
  }
}

// Store WASM stack top to GPR[reg] cache (write-back, no state_ptr needed).
// Caller must have pushed (value) onto the stack.
// More efficient than EmitStoreGPR when state_ptr isn't already on the stack.
static void EmitSetGPRValue(std::vector<u8>& code, GPRCache& cache, u32 reg)
{
  EmitLocalSet(code, LOCAL_GPR_BASE + reg);
  cache.loaded[reg] = true;
  cache.dirty[reg] = true;
  cache.is_const[reg] = false;
}

// Helper: generate ROTL32(value, amount)
// Expects value on stack. Uses WASM i32.rotl which does 32-bit rotate left.
static void EmitRotl32(std::vector<u8>& code, u32 amount)
{
  EmitI32Const(code, static_cast<s32>(amount));
  code.push_back(WasmOp::I32Rotl);
}

// Helper: compute mask for rlwinm (MB to ME inclusive, wrapping)
static u32 ComputeRotMask(u32 mb, u32 me)
{
  u32 mask;
  if (mb <= me)
  {
    mask = 0;
    for (u32 i = mb; i <= me; i++)
      mask |= (1u << (31 - i));
  }
  else
  {
    mask = 0xFFFFFFFF;
    for (u32 i = me + 1; i < mb; i++)
      mask &= ~(1u << (31 - i));
  }
  return mask;
}

// Import function indices (must match import section order)
static constexpr u32 FUNC_FALLBACK = 0;
static constexpr u32 FUNC_READ_U8 = 1;
static constexpr u32 FUNC_READ_U16 = 2;
static constexpr u32 FUNC_READ_U32 = 3;
static constexpr u32 FUNC_WRITE_U8 = 4;
static constexpr u32 FUNC_WRITE_U16 = 5;
static constexpr u32 FUNC_WRITE_U32 = 6;
static constexpr u32 FUNC_BLOCK = 7;  // Our block function (after 7 imports)

// Direct memory access constants (captured at compilation time)
// These are embedded as i32.const in the generated WASM bytecode.
// RAM is stored big-endian; WASM is little-endian, so loads need byte-swap.

// Emit: i32.load8_u offset=0 align=0  (byte load, no alignment requirement)
static void EmitI32Load8U(std::vector<u8>& code, u32 offset)
{
  code.push_back(WasmOp::I32Load8U);
  EncodeU32(code, 0);       // align = 0 (byte)
  EncodeU32(code, offset);
}

// Emit: i32.load16_u offset=0 align=0
static void EmitI32Load16U(std::vector<u8>& code, u32 offset)
{
  code.push_back(WasmOp::I32Load16U);
  EncodeU32(code, 0);       // align = 0
  EncodeU32(code, offset);
}

// Emit: i32.store8 offset=0 align=0
static void EmitI32Store8(std::vector<u8>& code, u32 offset)
{
  code.push_back(WasmOp::I32Store8);
  EncodeU32(code, 0);
  EncodeU32(code, offset);
}

// Emit: i32.store16 offset=0 align=0
static void EmitI32Store16(std::vector<u8>& code, u32 offset)
{
  code.push_back(WasmOp::I32Store16);
  EncodeU32(code, 0);
  EncodeU32(code, offset);
}

// Emit byte-swap of a u32 on the WASM stack.
// Uses local LOCAL_TMP3 as scratch. Result replaces top of stack.
// Algorithm: rotl(x,8) & 0x00FF00FF | rotl(x,24) & 0xFF00FF00
static void EmitBswap32(std::vector<u8>& code)
{
  // Save value to temp
  code.push_back(WasmOp::LocalTee);
  EncodeU32(code, LOCAL_TMP3);
  // rotl(val, 8)
  EmitI32Const(code, 8);
  code.push_back(WasmOp::I32Rotl);
  // & 0x00FF00FF
  EmitI32Const(code, 0x00FF00FF);
  code.push_back(WasmOp::I32And);
  // rotl(val, 24) = rotr(val, 8)
  EmitLocalGet(code, LOCAL_TMP3);
  EmitI32Const(code, 24);
  code.push_back(WasmOp::I32Rotl);
  // & 0xFF00FF00
  EmitI32Const(code, static_cast<s32>(0xFF00FF00));
  code.push_back(WasmOp::I32And);
  // OR together
  code.push_back(WasmOp::I32Or);
}

// Emit byte-swap of a u16 on the WASM stack (zero-extended i32).
// Result: ((val >> 8) & 0xFF) | ((val & 0xFF) << 8)
static void EmitBswap16(std::vector<u8>& code)
{
  code.push_back(WasmOp::LocalTee);
  EncodeU32(code, LOCAL_TMP3);
  EmitI32Const(code, 8);
  code.push_back(WasmOp::I32ShrU);
  EmitI32Const(code, 0xFF);
  code.push_back(WasmOp::I32And);
  EmitLocalGet(code, LOCAL_TMP3);
  EmitI32Const(code, 0xFF);
  code.push_back(WasmOp::I32And);
  EmitI32Const(code, 8);
  code.push_back(WasmOp::I32Shl);
  code.push_back(WasmOp::I32Or);
}

// Emit signed LEB128 for i64
static void EncodeS64(std::vector<u8>& out, s64 value)
{
  bool more = true;
  while (more)
  {
    u8 byte = value & 0x7f;
    value >>= 7;
    if ((value == 0 && (byte & 0x40) == 0) || (value == -1 && (byte & 0x40) != 0))
      more = false;
    else
      byte |= 0x80;
    out.push_back(byte);
  }
}

static void EmitI64Const(std::vector<u8>& code, s64 value)
{
  code.push_back(WasmOp::I64Const);
  EncodeS64(code, value);
}

// Emit: i64.store offset=<off> align=3
static void EmitI64Store(std::vector<u8>& code, u32 offset)
{
  code.push_back(WasmOp::I64Store);
  EncodeU32(code, 3);       // align = 3 (8-byte alignment)
  EncodeU32(code, offset);
}

// Emit: i64.load offset=<off> align=3
static void EmitI64Load(std::vector<u8>& code, u32 offset)
{
  code.push_back(WasmOp::I64Load);
  EncodeU32(code, 3);
  EncodeU32(code, offset);
}

// Emit: f64.load offset=<off> align=3
static void EmitF64Load(std::vector<u8>& code, u32 offset)
{
  code.push_back(WasmOp::F64Load);
  EncodeU32(code, 3);
  EncodeU32(code, offset);
}

// Emit: f64.store offset=<off> align=3
static void EmitF64Store(std::vector<u8>& code, u32 offset)
{
  code.push_back(WasmOp::F64Store);
  EncodeU32(code, 3);
  EncodeU32(code, offset);
}

// --- WASM SIMD v128 emit helpers ---

// Emit a SIMD opcode: 0xFD prefix + LEB128 opcode number
static void EmitSIMDOp(std::vector<u8>& code, u32 opcode)
{
  code.push_back(WasmOp::SIMDPrefix);
  EncodeU32(code, opcode);
}

// Emit: v128.load offset=<off> align=4 (16-byte aligned)
static void EmitV128Load(std::vector<u8>& code, u32 offset)
{
  EmitSIMDOp(code, WasmOp::V128Load);
  EncodeU32(code, 4);  // align: log2(16) = 4
  EncodeU32(code, offset);
}

// Emit: v128.store offset=<off> align=4
static void EmitV128Store(std::vector<u8>& code, u32 offset)
{
  EmitSIMDOp(code, WasmOp::V128Store);
  EncodeU32(code, 4);
  EncodeU32(code, offset);
}

// Emit: f64x2.extract_lane <lane>  (v128 → f64)
static void EmitF64x2ExtractLane(std::vector<u8>& code, u8 lane)
{
  EmitSIMDOp(code, WasmOp::F64x2ExtractLane);
  code.push_back(lane);
}

// Emit: f64x2.replace_lane <lane>  (v128, f64 → v128)
static void EmitF64x2ReplaceLane(std::vector<u8>& code, u8 lane)
{
  EmitSIMDOp(code, WasmOp::F64x2ReplaceLane);
  code.push_back(lane);
}

// Emit: f32x4.demote_f64x2_zero then f64x2.promote_low_f32x4  (v128 → v128, rounds both lanes to f32)
static void EmitF64x2RoundToF32(std::vector<u8>& code)
{
  EmitSIMDOp(code, WasmOp::F32x4DemoteF64x2Zero);
  EmitSIMDOp(code, WasmOp::F64x2PromoteLowF32x4);
}

// --- FPR cache: v128 (ps0 in lane 0, ps1 in lane 1) ---

// Ensure FPR v128 is loaded into cache. Does NOT push anything onto stack.
static void EmitEnsureFPRLoaded(std::vector<u8>& code, GPRCache& cache, u32 fpr)
{
  if (!cache.fpr_loaded[fpr])
  {
    EmitLocalGet(code, LOCAL_STATE);
    EmitV128Load(code, OFF_PS + fpr * 16);
    EmitLocalSet(code, LOCAL_FPR_V128_BASE + fpr);
    cache.fpr_loaded[fpr] = true;
  }
}

// Load FPR as full v128 onto WASM stack (both ps0+ps1)
static void EmitLoadFPRv128(std::vector<u8>& code, GPRCache& cache, u32 fpr)
{
  EmitEnsureFPRLoaded(code, cache, fpr);
  EmitLocalGet(code, LOCAL_FPR_V128_BASE + fpr);
}

// Store v128 from stack to FPR cache (marks dirty)
static void EmitSetFPRv128(std::vector<u8>& code, GPRCache& cache, u32 fpr)
{
  EmitLocalSet(code, LOCAL_FPR_V128_BASE + fpr);
  cache.fpr_loaded[fpr] = true;
  cache.fpr_dirty[fpr] = true;
}

// Load ps[fpr].ps0 as f64 onto WASM stack (extracts lane 0 from v128 cache)
static void EmitLoadPS0(std::vector<u8>& code, GPRCache& cache, u32 fpr)
{
  EmitEnsureFPRLoaded(code, cache, fpr);
  EmitLocalGet(code, LOCAL_FPR_V128_BASE + fpr);
  EmitF64x2ExtractLane(code, 0);
}

// Load ps[fpr].ps1 as f64 onto WASM stack (extracts lane 1 from v128 cache)
static void EmitLoadPS1(std::vector<u8>& code, GPRCache& cache, u32 fpr)
{
  EmitEnsureFPRLoaded(code, cache, fpr);
  EmitLocalGet(code, LOCAL_FPR_V128_BASE + fpr);
  EmitF64x2ExtractLane(code, 1);
}

// Store f64 from stack into ps[fpr].ps0 (replaces lane 0 in v128 cache).
// Uses LOCAL_FPR0 as temp.
static void EmitSetPS0Value(std::vector<u8>& code, GPRCache& cache, u32 fpr)
{
  EmitLocalSet(code, LOCAL_FPR0);  // save f64 to temp
  EmitEnsureFPRLoaded(code, cache, fpr);
  EmitLocalGet(code, LOCAL_FPR_V128_BASE + fpr);  // current v128
  EmitLocalGet(code, LOCAL_FPR0);                   // f64 value
  EmitF64x2ReplaceLane(code, 0);                    // v128 with new ps0
  EmitLocalSet(code, LOCAL_FPR_V128_BASE + fpr);
  cache.fpr_dirty[fpr] = true;
}

// Store f64 from stack into ps[fpr].ps1 (replaces lane 1 in v128 cache).
// Uses LOCAL_FPR0 as temp.
static void EmitSetPS1Value(std::vector<u8>& code, GPRCache& cache, u32 fpr)
{
  EmitLocalSet(code, LOCAL_FPR0);  // save f64 to temp
  EmitEnsureFPRLoaded(code, cache, fpr);
  EmitLocalGet(code, LOCAL_FPR_V128_BASE + fpr);
  EmitLocalGet(code, LOCAL_FPR0);
  EmitF64x2ReplaceLane(code, 1);
  EmitLocalSet(code, LOCAL_FPR_V128_BASE + fpr);
  cache.fpr_dirty[fpr] = true;
}

// Forward declaration
static void EmitFlushAllCR(std::vector<u8>& code, GPRCache& cache);

// Flush all dirty GPRs from cache locals back to PowerPCState memory.
// Must be called before interpreter fallback, block exit, or cache invalidation.
static void EmitFlushAllGPRs(std::vector<u8>& code, GPRCache& cache)
{
  for (u32 i = 0; i < 32; i++)
  {
    if (!cache.dirty[i])
      continue;
    EmitLocalGet(code, LOCAL_STATE);
    EmitLocalGet(code, LOCAL_GPR_BASE + i);
    EmitI32Store(code, OFF_GPR + i * 4);
    cache.dirty[i] = false;
  }
}

// Flush all dirty FPR v128 from cache locals back to PowerPCState memory.
static void EmitFlushAllFPRs(std::vector<u8>& code, GPRCache& cache)
{
  for (u32 i = 0; i < 32; i++)
  {
    if (cache.fpr_dirty[i])
    {
      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_FPR_V128_BASE + i);
      EmitV128Store(code, OFF_PS + i * 16);
      cache.fpr_dirty[i] = false;
    }
  }
}

// Flush all dirty registers (GPR + FPR) — convenience wrapper.
static void EmitFlushAll(std::vector<u8>& code, GPRCache& cache)
{
  EmitFlushAllGPRs(code, cache);
  EmitFlushAllFPRs(code, cache);
  EmitFlushAllCR(code, cache);
}

// Invalidate FPR cache entry for a register written via raw i64 store (bypassing cache).
// Call this AFTER the direct EmitI64Store to ps[fd], to prevent stale cache flush.
static void InvalidateFPRDirect(GPRCache& cache, u32 fpr)
{
  cache.fpr_loaded[fpr] = false;
  cache.fpr_dirty[fpr] = false;
}

// Load CR field into WASM i64 local (with CR cache).
// Pushes i64 value onto WASM stack.
static void EmitLoadCR(std::vector<u8>& code, GPRCache& cache, u32 crfd)
{
  if (cache.cr_loaded[crfd])
  {
    EmitLocalGet(code, LOCAL_CR_BASE + crfd);
  }
  else
  {
    EmitLocalGet(code, LOCAL_STATE);
    EmitI64Load(code, OFF_CR + crfd * 8);
    EmitLocalTee(code, LOCAL_CR_BASE + crfd);
    cache.cr_loaded[crfd] = true;
  }
}

// Read lower 32 bits of a CR field (used for EQ check: lower32 == 0).
// Pushes i32 onto stack.
static void EmitCRReadLower32(std::vector<u8>& code, GPRCache& cache, u32 crfd)
{
  if (!cache.cr_loaded[crfd])
  {
    // Load full i64 into cache
    EmitLocalGet(code, LOCAL_STATE);
    EmitI64Load(code, OFF_CR + crfd * 8);
    EmitLocalSet(code, LOCAL_CR_BASE + crfd);
    cache.cr_loaded[crfd] = true;
  }
  EmitLocalGet(code, LOCAL_CR_BASE + crfd);
  code.push_back(WasmOp::I32WrapI64);
}

// Read upper 32 bits of a CR field (used for LT/SO bit checks).
// Pushes i32 onto stack.
static void EmitCRReadUpper32(std::vector<u8>& code, GPRCache& cache, u32 crfd)
{
  if (!cache.cr_loaded[crfd])
  {
    EmitLocalGet(code, LOCAL_STATE);
    EmitI64Load(code, OFF_CR + crfd * 8);
    EmitLocalSet(code, LOCAL_CR_BASE + crfd);
    cache.cr_loaded[crfd] = true;
  }
  EmitLocalGet(code, LOCAL_CR_BASE + crfd);
  EmitI64Const(code, 32);
  code.push_back(WasmOp::I64ShrU);
  code.push_back(WasmOp::I32WrapI64);
}

// Store i64 CR field value to cache (write-back).
// Expects i64 value on stack.
static void EmitStoreCR(std::vector<u8>& code, GPRCache& cache, u32 crfd)
{
  EmitLocalSet(code, LOCAL_CR_BASE + crfd);
  cache.cr_loaded[crfd] = true;
  cache.cr_dirty[crfd] = true;
  cache.cr_is_const[crfd] = false;  // runtime value — not constant
}

// Test a specific CR bit for branch instructions.
// Pushes i32 (0 or 1) result onto stack: 1 if the bit is set.
// bi: branch BI field (0-31), encoding cr_field*4 + bit.
static void EmitCRBitTest(std::vector<u8>& code, GPRCache& cache, u32 bi)
{
  const u32 cr_field_idx = bi >> 2;
  const u32 bit_in_field = 3 - (bi & 3);

  // CR constant propagation: if CR field is known at compile time, fold the bit test.
  // SO bit (case 0) still needs runtime check since we don't track XER.SO.
  if (cache.cr_is_const[cr_field_idx] && bit_in_field != 0)
  {
    const s64 cr_val = cache.cr_const_val[cr_field_idx];
    bool bit_set = false;
    switch (bit_in_field)
    {
    case 1:  // EQ: lower32 == 0
      bit_set = (static_cast<u32>(cr_val) == 0);
      break;
    case 2:  // GT: (s64)val > 0
      bit_set = (cr_val > 0);
      break;
    case 3:  // LT: bit 62 set
      bit_set = ((cr_val >> 62) & 1) != 0;
      break;
    }
    EmitI32Const(code, bit_set ? 1 : 0);
    return;
  }

  switch (bit_in_field)
  {
  case 0:  // SO: bit 59 in upper32
    EmitCRReadUpper32(code, cache, cr_field_idx);
    EmitI32Const(code, 1 << (59 - 32));
    code.push_back(WasmOp::I32And);
    EmitI32Const(code, 0);
    code.push_back(WasmOp::I32Ne);
    break;
  case 1:  // EQ: lower32 == 0
    EmitCRReadLower32(code, cache, cr_field_idx);
    code.push_back(WasmOp::I32Eqz);
    break;
  case 2:  // GT: (s64)val > 0 — use direct i64 comparison
    EmitLoadCR(code, cache, cr_field_idx);
    EmitI64Const(code, 0);
    code.push_back(WasmOp::I64GtS);
    break;
  case 3:  // LT: bit 62 in upper32
    EmitCRReadUpper32(code, cache, cr_field_idx);
    EmitI32Const(code, 1 << (62 - 32));
    code.push_back(WasmOp::I32And);
    EmitI32Const(code, 0);
    code.push_back(WasmOp::I32Ne);
    break;
  }
}

// Compile-time CR bit test: returns -1 if unknown, 0 if bit clear, 1 if bit set.
// Used for constant-CR branch elimination (avoiding WASM if/end blocks).
static int TryConstCRBitTest(const GPRCache& cache, u32 bi)
{
  const u32 cr_field_idx = bi >> 2;
  const u32 bit_in_field = 3 - (bi & 3);

  // SO bit (case 0) requires runtime XER.SO check — cannot fold
  if (!cache.cr_is_const[cr_field_idx] || bit_in_field == 0)
    return -1;

  const s64 cr_val = cache.cr_const_val[cr_field_idx];
  switch (bit_in_field)
  {
  case 1:  // EQ: lower32 == 0
    return (static_cast<u32>(cr_val) == 0) ? 1 : 0;
  case 2:  // GT: (s64)val > 0
    return (cr_val > 0) ? 1 : 0;
  case 3:  // LT: bit 62 set
    return ((cr_val >> 62) & 1) != 0 ? 1 : 0;
  default:
    return -1;
  }
}

// Flush all dirty CR fields from cache to memory.
static void EmitFlushAllCR(std::vector<u8>& code, GPRCache& cache)
{
  for (u32 i = 0; i < 8; i++)
  {
    if (!cache.cr_dirty[i])
      continue;
    EmitLocalGet(code, LOCAL_STATE);
    EmitLocalGet(code, LOCAL_CR_BASE + i);
    EmitI64Store(code, OFF_CR + i * 8);
    cache.cr_dirty[i] = false;
  }
}

// Emit a signed integer comparison and store result to CR field.
// Expects: a (i32) in LOCAL_TMP1, b (i32) in LOCAL_TMP2.
// crfd: which CR field (0-7).
// is_signed: true for signed comparison, false for unsigned.
// Dolphin CR optimized format (per field, stored as u64):
//   LT: bit 62 set
//   SO: bit 59 set
//   EQ: lower 32 bits == 0
//   GT: (s64)value > 0
//
// Precomputed constants for each comparison result:
//   LT only: 0xC000000000000001 → bit 63+62 set, lower bit set (negative, !EQ, LT)
//   GT only: 0x0000000000000001 → positive, lower bit set (!EQ, GT)
//   EQ only: 0x8000000000000000 → bit 63 set (negative, lower32==0, !GT, !LT)

// Emit a pre-computed CR comparison result (compile-time constant folding).
// Still reads XER.SO at runtime and ORs it into bit 59.
static void EmitConstCRResult(std::vector<u8>& code, GPRCache& cache, u32 crfd, s64 cr_val)
{
  // Compute: cr_val | (SO << 59) and store to CR cache
  EmitLocalGet(code, LOCAL_STATE);
  EmitI32Load8U(code, OFF_XER_SO_OV);
  EmitI32Const(code, 1);
  code.push_back(WasmOp::I32ShrU);
  EmitI32Const(code, 1);
  code.push_back(WasmOp::I32And);
  code.push_back(WasmOp::I64ExtendI32U);
  EmitI64Const(code, 59);
  code.push_back(WasmOp::I64Shl);
  EmitI64Const(code, cr_val);
  code.push_back(WasmOp::I64Or);
  EmitStoreCR(code, cache, crfd);
  // Track constant value (without SO — SO is rare and tested separately)
  cache.cr_is_const[crfd] = true;
  cache.cr_const_val[crfd] = cr_val;
}

static void EmitCRCompare(std::vector<u8>& code, GPRCache& cache, u32 crfd, bool is_signed)
{
  static constexpr s64 CR_LT_VAL = static_cast<s64>(0xC000000000000001ULL);
  static constexpr s64 CR_GT_VAL = static_cast<s64>(0x0000000000000001ULL);
  static constexpr s64 CR_EQ_VAL = static_cast<s64>(0x8000000000000000ULL);

  // Branchless CR comparison using WASM select.
  // Step 1: select(CR_GT, CR_EQ, a > b) → GT_or_EQ
  EmitI64Const(code, CR_GT_VAL);
  EmitI64Const(code, CR_EQ_VAL);
  EmitLocalGet(code, LOCAL_TMP1);
  EmitLocalGet(code, LOCAL_TMP2);
  code.push_back(is_signed ? WasmOp::I32GtS : WasmOp::I32GtU);
  code.push_back(WasmOp::Select);

  // Step 2: select(CR_LT, GT_or_EQ, a < b) → final CR value
  EmitLocalSet(code, LOCAL_CR64);  // save GT_or_EQ
  EmitI64Const(code, CR_LT_VAL);
  EmitLocalGet(code, LOCAL_CR64);
  EmitLocalGet(code, LOCAL_TMP1);
  EmitLocalGet(code, LOCAL_TMP2);
  code.push_back(is_signed ? WasmOp::I32LtS : WasmOp::I32LtU);
  code.push_back(WasmOp::Select);

  // OR in XER.SO at bit 59
  EmitLocalGet(code, LOCAL_STATE);
  EmitI32Load8U(code, OFF_XER_SO_OV);
  EmitI32Const(code, 1);
  code.push_back(WasmOp::I32ShrU);
  EmitI32Const(code, 1);
  code.push_back(WasmOp::I32And);
  code.push_back(WasmOp::I64ExtendI32U);
  EmitI64Const(code, 59);
  code.push_back(WasmOp::I64Shl);
  code.push_back(WasmOp::I64Or);
  EmitStoreCR(code, cache, crfd);
}

// Emit CR0 update from a 32-bit result value in LOCAL_TMP1.
// Replicates Interpreter::Helper_UpdateCR0:
//   cr_val = (s64)(s32)result
//   if result == 0: cr_val |= (1<<63) to prevent GT
//   cr_val = (cr_val & ~(1<<59)) | (SO << 59)
//   cr.fields[0] = cr_val
static void EmitUpdateCR0(std::vector<u8>& code, GPRCache& cache)
{
  static constexpr s64 CR_LT_VAL = static_cast<s64>(0xC000000000000001ULL);
  static constexpr s64 CR_GT_VAL = static_cast<s64>(0x0000000000000001ULL);
  static constexpr s64 CR_EQ_VAL = static_cast<s64>(0x8000000000000000ULL);

  // Branchless CR0 update using WASM select.
  // select(CR_GT, CR_EQ, result > 0) → GT_or_EQ
  EmitI64Const(code, CR_GT_VAL);
  EmitI64Const(code, CR_EQ_VAL);
  EmitLocalGet(code, LOCAL_TMP1);
  EmitI32Const(code, 0);
  code.push_back(WasmOp::I32GtS);
  code.push_back(WasmOp::Select);
  // select(CR_LT, GT_or_EQ, result < 0) → final
  EmitLocalSet(code, LOCAL_CR64);
  EmitI64Const(code, CR_LT_VAL);
  EmitLocalGet(code, LOCAL_CR64);
  EmitLocalGet(code, LOCAL_TMP1);
  EmitI32Const(code, 0);
  code.push_back(WasmOp::I32LtS);
  code.push_back(WasmOp::Select);

  // OR in XER.SO at bit 59
  EmitLocalGet(code, LOCAL_STATE);
  EmitI32Load8U(code, OFF_XER_SO_OV);
  EmitI32Const(code, 1);
  code.push_back(WasmOp::I32ShrU);
  EmitI32Const(code, 1);
  code.push_back(WasmOp::I32And);
  code.push_back(WasmOp::I64ExtendI32U);
  EmitI64Const(code, 59);
  code.push_back(WasmOp::I64Shl);
  code.push_back(WasmOp::I64Or);
  EmitStoreCR(code, cache, 0);
}

// Emit a fast-path direct load from shared WASM memory.
// EA must be in LOCAL_EA. Produces one i32 on the stack.
// size: 1=byte, 2=halfword, 4=word
// ram_base, exram_base, ram_mask, exram_mask are embedded as constants.
static void EmitDirectLoad(std::vector<u8>& code, u32 size,
                           u32 ram_base, u32 exram_base,
                           u32 ram_mask, u32 exram_mask,
                           u32 slow_func_idx)
{
  // Helper lambda: emit load + bswap for a given size
  auto emit_load_bswap = [&](u32 sz) {
    if (sz == 4)
    {
      EmitI32Load(code, 0);
      EmitBswap32(code);
    }
    else if (sz == 2)
    {
      EmitI32Load16U(code, 0);
      EmitBswap16(code);
    }
    else
    {
      EmitI32Load8U(code, 0);
    }
  };

  // Check if EA has bit 31 set (maps to physical memory: 0x80000000+)
  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, 31);
  code.push_back(WasmOp::I32ShrU);
  code.push_back(WasmOp::If);
  code.push_back(WasmOp::I32);  // result type: i32

  if (exram_base != 0)
  {
    // Wii mode: check bit 28 for MEM1 vs MEM2
    EmitLocalGet(code, LOCAL_EA);
    EmitI32Const(code, 28);
    code.push_back(WasmOp::I32ShrU);
    EmitI32Const(code, 1);
    code.push_back(WasmOp::I32And);
    code.push_back(WasmOp::If);
    code.push_back(WasmOp::I32);

    // MEM2 (nibble bit 0 set): exram_base + (EA & exram_mask)
    EmitI32Const(code, static_cast<s32>(exram_base));
    EmitLocalGet(code, LOCAL_EA);
    EmitI32Const(code, static_cast<s32>(exram_mask));
    code.push_back(WasmOp::I32And);
    code.push_back(WasmOp::I32Add);
    emit_load_bswap(size);

    code.push_back(WasmOp::Else);
  }

  // MEM1: ram_base + (EA & ram_mask)
  EmitI32Const(code, static_cast<s32>(ram_base));
  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, static_cast<s32>(ram_mask));
  code.push_back(WasmOp::I32And);
  code.push_back(WasmOp::I32Add);
  emit_load_bswap(size);

  if (exram_base != 0)
    code.push_back(WasmOp::End);  // end MEM1/MEM2 check

  code.push_back(WasmOp::Else);

  // Slow path: call imported function (non-physical addresses: MMIO, etc.)
  EmitLocalGet(code, LOCAL_EA);
  EmitCall(code, slow_func_idx);

  code.push_back(WasmOp::End);  // end bit31 check
}

// Emit a MEM1-only direct load (no bit31/bit28 dispatch).
// Use when the address is known to be in MEM1 (e.g., stack-relative via r1).
// EA must be in LOCAL_EA. Result pushed onto WASM stack.
static void EmitDirectLoadMEM1(std::vector<u8>& code, u32 size, u32 ram_base, u32 ram_mask)
{
  EmitI32Const(code, static_cast<s32>(ram_base));
  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, static_cast<s32>(ram_mask));
  code.push_back(WasmOp::I32And);
  code.push_back(WasmOp::I32Add);
  if (size == 4)
  {
    EmitI32Load(code, 0);
    EmitBswap32(code);
  }
  else if (size == 2)
  {
    EmitI32Load16U(code, 0);
    EmitBswap16(code);
  }
  else
  {
    EmitI32Load8U(code, 0);
  }
}

// Emit a MEM1-only direct store (no bit31/bit28 dispatch).
// EA must be in LOCAL_EA. Value must be in LOCAL_TMP1.
static void EmitDirectStoreMEM1(std::vector<u8>& code, u32 size, u32 ram_base, u32 ram_mask)
{
  EmitI32Const(code, static_cast<s32>(ram_base));
  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, static_cast<s32>(ram_mask));
  code.push_back(WasmOp::I32And);
  code.push_back(WasmOp::I32Add);
  EmitLocalGet(code, LOCAL_TMP1);
  if (size == 4)
  {
    EmitBswap32(code);
    EmitI32Store(code, 0);
  }
  else if (size == 2)
  {
    EmitBswap16(code);
    EmitI32Store16(code, 0);
  }
  else
  {
    EmitI32Store8(code, 0);
  }
}

// Emit a MEM1-only 64-bit load (no bit31/bit28 dispatch).
// EA must be in LOCAL_EA. Result (i64) pushed onto WASM stack.
// Uses LOCAL_TMP1 internally.
static void EmitDirectLoadMEM1_64(std::vector<u8>& code, u32 ram_base, u32 ram_mask)
{
  EmitI32Const(code, static_cast<s32>(ram_base));
  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, static_cast<s32>(ram_mask));
  code.push_back(WasmOp::I32And);
  code.push_back(WasmOp::I32Add);
  EmitLocalSet(code, LOCAL_TMP1);
  // High word at offset 0
  EmitLocalGet(code, LOCAL_TMP1);
  EmitI32Load(code, 0);
  EmitBswap32(code);
  code.push_back(WasmOp::I64ExtendI32U);
  EmitI64Const(code, 32);
  code.push_back(WasmOp::I64Shl);
  // Low word at offset 4
  EmitLocalGet(code, LOCAL_TMP1);
  code.push_back(WasmOp::I32Load);
  EncodeU32(code, 2);  // align
  EncodeU32(code, 4);  // offset
  EmitBswap32(code);
  code.push_back(WasmOp::I64ExtendI32U);
  code.push_back(WasmOp::I64Or);
}

// Emit a MEM1-only 64-bit store (no bit31/bit28 dispatch).
// EA must be in LOCAL_EA. i64 value must be in LOCAL_CR64.
// Uses LOCAL_TMP1, LOCAL_TMP3 internally.
static void EmitDirectStoreMEM1_64(std::vector<u8>& code, u32 ram_base, u32 ram_mask)
{
  EmitI32Const(code, static_cast<s32>(ram_base));
  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, static_cast<s32>(ram_mask));
  code.push_back(WasmOp::I32And);
  code.push_back(WasmOp::I32Add);
  EmitLocalSet(code, LOCAL_TMP1);
  // Store high 32 bits at offset 0
  EmitLocalGet(code, LOCAL_TMP1);
  EmitLocalGet(code, LOCAL_CR64);
  EmitI64Const(code, 32);
  code.push_back(WasmOp::I64ShrU);
  code.push_back(WasmOp::I32WrapI64);
  EmitBswap32(code);
  EmitI32Store(code, 0);
  // Store low 32 bits at offset 4
  EmitLocalGet(code, LOCAL_TMP1);
  EmitLocalGet(code, LOCAL_CR64);
  code.push_back(WasmOp::I32WrapI64);
  EmitBswap32(code);
  code.push_back(WasmOp::I32Store);
  EncodeU32(code, 2);  // align
  EncodeU32(code, 4);  // offset
}

// Emit a constant-address MEM1 load. wasm_addr = ram_base + (ea & ram_mask),
// pre-computed at compile time. No LOCAL_EA needed. Pushes result onto stack.
static void EmitConstAddrLoadMEM1(std::vector<u8>& code, u32 size, u32 wasm_addr)
{
  EmitI32Const(code, static_cast<s32>(wasm_addr));
  if (size == 4)
  {
    EmitI32Load(code, 0);
    EmitBswap32(code);
  }
  else if (size == 2)
  {
    EmitI32Load16U(code, 0);
    EmitBswap16(code);
  }
  else
  {
    EmitI32Load8U(code, 0);
  }
}

// Emit a constant-address MEM1 store. wasm_addr = ram_base + (ea & ram_mask),
// pre-computed at compile time. No LOCAL_EA or LOCAL_TMP1 needed.
// Value must already be on the WASM stack.
static void EmitConstAddrStoreMEM1(std::vector<u8>& code, u32 size, u32 wasm_addr)
{
  // Stack: [value]. Need [addr, value] for store. Use LOCAL_TMP1 as temp.
  EmitLocalSet(code, LOCAL_TMP1);
  EmitI32Const(code, static_cast<s32>(wasm_addr));
  EmitLocalGet(code, LOCAL_TMP1);
  if (size == 4)
  {
    EmitBswap32(code);
    EmitI32Store(code, 0);
  }
  else if (size == 2)
  {
    EmitBswap16(code);
    EmitI32Store16(code, 0);
  }
  else
  {
    EmitI32Store8(code, 0);
  }
}

// Emit a constant-address MEM1 64-bit load (for lfd).
// Loads two 32-bit words, byte-swaps each, combines into i64. Result on stack.
static void EmitConstAddrLoadMEM1_64(std::vector<u8>& code, u32 wasm_addr)
{
  // High word (at wasm_addr+0)
  EmitI32Const(code, static_cast<s32>(wasm_addr));
  EmitI32Load(code, 0);
  EmitBswap32(code);
  code.push_back(WasmOp::I64ExtendI32U);
  EmitI64Const(code, 32);
  code.push_back(WasmOp::I64Shl);
  // Low word (at wasm_addr+4)
  EmitI32Const(code, static_cast<s32>(wasm_addr));
  code.push_back(WasmOp::I32Load);
  EncodeU32(code, 2);  // align
  EncodeU32(code, 4);  // offset
  EmitBswap32(code);
  code.push_back(WasmOp::I64ExtendI32U);
  code.push_back(WasmOp::I64Or);
}

// Emit a constant-address MEM1 64-bit store (for stfd).
// Value (i64) must be in LOCAL_CR64. wasm_addr is pre-computed.
static void EmitConstAddrStoreMEM1_64(std::vector<u8>& code, u32 wasm_addr)
{
  // Store high 32 bits at wasm_addr+0
  EmitI32Const(code, static_cast<s32>(wasm_addr));
  EmitLocalGet(code, LOCAL_CR64);
  EmitI64Const(code, 32);
  code.push_back(WasmOp::I64ShrU);
  code.push_back(WasmOp::I32WrapI64);
  EmitBswap32(code);
  EmitI32Store(code, 0);
  // Store low 32 bits at wasm_addr+4
  EmitI32Const(code, static_cast<s32>(wasm_addr));
  EmitLocalGet(code, LOCAL_CR64);
  code.push_back(WasmOp::I32WrapI64);
  EmitBswap32(code);
  code.push_back(WasmOp::I32Store);
  EncodeU32(code, 2);  // align
  EncodeU32(code, 4);  // offset
}

// Emit a fast-path direct store to shared WASM memory.
// EA must be in LOCAL_EA. Value must be in LOCAL_TMP1.
static void EmitDirectStore(std::vector<u8>& code, u32 size,
                            u32 ram_base, u32 exram_base,
                            u32 ram_mask, u32 exram_mask,
                            u32 slow_func_idx)
{
  // Helper lambda: emit store with bswap for a given size
  auto emit_store_bswap = [&](u32 sz) {
    EmitLocalGet(code, LOCAL_TMP1);
    if (sz == 4)
    {
      EmitBswap32(code);
      EmitI32Store(code, 0);
    }
    else if (sz == 2)
    {
      EmitBswap16(code);
      EmitI32Store16(code, 0);
    }
    else
    {
      EmitI32Store8(code, 0);
    }
  };

  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, 31);
  code.push_back(WasmOp::I32ShrU);
  code.push_back(WasmOp::If);
  code.push_back(WasmOp::Void);

  if (exram_base != 0)
  {
    // Wii mode: check bit 28 for MEM1 vs MEM2
    EmitLocalGet(code, LOCAL_EA);
    EmitI32Const(code, 28);
    code.push_back(WasmOp::I32ShrU);
    EmitI32Const(code, 1);
    code.push_back(WasmOp::I32And);
    code.push_back(WasmOp::If);
    code.push_back(WasmOp::Void);

    // MEM2
    EmitI32Const(code, static_cast<s32>(exram_base));
    EmitLocalGet(code, LOCAL_EA);
    EmitI32Const(code, static_cast<s32>(exram_mask));
    code.push_back(WasmOp::I32And);
    code.push_back(WasmOp::I32Add);
    emit_store_bswap(size);

    code.push_back(WasmOp::Else);
  }

  // MEM1
  EmitI32Const(code, static_cast<s32>(ram_base));
  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, static_cast<s32>(ram_mask));
  code.push_back(WasmOp::I32And);
  code.push_back(WasmOp::I32Add);
  emit_store_bswap(size);

  if (exram_base != 0)
    code.push_back(WasmOp::End);  // end MEM1/MEM2

  code.push_back(WasmOp::Else);

  // Slow path: call imported function
  EmitLocalGet(code, LOCAL_EA);
  EmitLocalGet(code, LOCAL_TMP1);
  EmitCall(code, slow_func_idx);

  code.push_back(WasmOp::End);  // end bit31 check
}

// Load 64 bits from RAM (big-endian) via direct memory access.
// EA must be in LOCAL_EA. Returns i64 on the WASM stack.
// Uses LOCAL_NIBBLE, LOCAL_TMP1, LOCAL_TMP3 internally.
static void EmitDirectLoad64(std::vector<u8>& code,
                              u32 ram_base, u32 exram_base,
                              u32 ram_mask, u32 exram_mask)
{
  // Helper: emit 64-bit load from base_addr in LOCAL_TMP1 (two bswap32 + combine)
  auto emit_load64_from_tmp1 = [&]() {
    EmitLocalGet(code, LOCAL_TMP1);
    EmitI32Load(code, 0);
    EmitBswap32(code);
    code.push_back(WasmOp::I64ExtendI32U);
    EmitI64Const(code, 32);
    code.push_back(WasmOp::I64Shl);
    EmitLocalGet(code, LOCAL_TMP1);
    code.push_back(WasmOp::I32Load);
    EncodeU32(code, 2);
    EncodeU32(code, 4);
    EmitBswap32(code);
    code.push_back(WasmOp::I64ExtendI32U);
    code.push_back(WasmOp::I64Or);
  };

  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, 31);
  code.push_back(WasmOp::I32ShrU);
  code.push_back(WasmOp::If);
  code.push_back(WasmOp::I64);

  if (exram_base != 0)
  {
    // Wii mode: check bit 28 for MEM1 vs MEM2
    EmitLocalGet(code, LOCAL_EA);
    EmitI32Const(code, 28);
    code.push_back(WasmOp::I32ShrU);
    EmitI32Const(code, 1);
    code.push_back(WasmOp::I32And);
    code.push_back(WasmOp::If);
    code.push_back(WasmOp::I64);

    // MEM2
    EmitI32Const(code, static_cast<s32>(exram_base));
    EmitLocalGet(code, LOCAL_EA);
    EmitI32Const(code, static_cast<s32>(exram_mask));
    code.push_back(WasmOp::I32And);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_TMP1);
    emit_load64_from_tmp1();

    code.push_back(WasmOp::Else);
  }

  // MEM1
  EmitI32Const(code, static_cast<s32>(ram_base));
  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, static_cast<s32>(ram_mask));
  code.push_back(WasmOp::I32And);
  code.push_back(WasmOp::I32Add);
  EmitLocalSet(code, LOCAL_TMP1);
  emit_load64_from_tmp1();

  if (exram_base != 0)
    code.push_back(WasmOp::End);  // MEM1/MEM2

  code.push_back(WasmOp::Else);

  // Slow path
  EmitLocalGet(code, LOCAL_EA);
  EmitCall(code, FUNC_READ_U32);
  code.push_back(WasmOp::I64ExtendI32U);
  EmitI64Const(code, 32);
  code.push_back(WasmOp::I64Shl);
  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, 4);
  code.push_back(WasmOp::I32Add);
  EmitCall(code, FUNC_READ_U32);
  code.push_back(WasmOp::I64ExtendI32U);
  code.push_back(WasmOp::I64Or);

  code.push_back(WasmOp::End);  // bit31
}

// Store 64 bits to RAM (big-endian) via direct memory access.
// EA must be in LOCAL_EA. i64 value must be in LOCAL_CR64.
// Uses LOCAL_NIBBLE, LOCAL_TMP1, LOCAL_TMP3 internally.
static void EmitDirectStore64(std::vector<u8>& code,
                               u32 ram_base, u32 exram_base,
                               u32 ram_mask, u32 exram_mask)
{
  // Helper: store 64-bit value from LOCAL_CR64 to address in LOCAL_TMP1
  auto emit_store64_to_tmp1 = [&]() {
    EmitLocalGet(code, LOCAL_TMP1);
    EmitLocalGet(code, LOCAL_CR64);
    EmitI64Const(code, 32);
    code.push_back(WasmOp::I64ShrU);
    code.push_back(WasmOp::I32WrapI64);
    EmitBswap32(code);
    EmitI32Store(code, 0);
    EmitLocalGet(code, LOCAL_TMP1);
    EmitLocalGet(code, LOCAL_CR64);
    code.push_back(WasmOp::I32WrapI64);
    EmitBswap32(code);
    code.push_back(WasmOp::I32Store);
    EncodeU32(code, 2);
    EncodeU32(code, 4);
  };

  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, 31);
  code.push_back(WasmOp::I32ShrU);
  code.push_back(WasmOp::If);
  code.push_back(WasmOp::Void);

  if (exram_base != 0)
  {
    // Wii mode: check bit 28 for MEM1 vs MEM2
    EmitLocalGet(code, LOCAL_EA);
    EmitI32Const(code, 28);
    code.push_back(WasmOp::I32ShrU);
    EmitI32Const(code, 1);
    code.push_back(WasmOp::I32And);
    code.push_back(WasmOp::If);
    code.push_back(WasmOp::Void);

    // MEM2
    EmitI32Const(code, static_cast<s32>(exram_base));
    EmitLocalGet(code, LOCAL_EA);
    EmitI32Const(code, static_cast<s32>(exram_mask));
    code.push_back(WasmOp::I32And);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_TMP1);
    emit_store64_to_tmp1();

    code.push_back(WasmOp::Else);
  }

  // MEM1
  EmitI32Const(code, static_cast<s32>(ram_base));
  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, static_cast<s32>(ram_mask));
  code.push_back(WasmOp::I32And);
  code.push_back(WasmOp::I32Add);
  EmitLocalSet(code, LOCAL_TMP1);
  emit_store64_to_tmp1();

  if (exram_base != 0)
    code.push_back(WasmOp::End);  // MEM1/MEM2

  code.push_back(WasmOp::Else);

  // Slow path
  EmitLocalGet(code, LOCAL_EA);
  EmitLocalGet(code, LOCAL_CR64);
  EmitI64Const(code, 32);
  code.push_back(WasmOp::I64ShrU);
  code.push_back(WasmOp::I32WrapI64);
  EmitCall(code, FUNC_WRITE_U32);
  EmitLocalGet(code, LOCAL_EA);
  EmitI32Const(code, 4);
  code.push_back(WasmOp::I32Add);
  EmitLocalGet(code, LOCAL_CR64);
  code.push_back(WasmOp::I32WrapI64);
  EmitCall(code, FUNC_WRITE_U32);

  code.push_back(WasmOp::End);  // bit31
}

// Emit FPU CR comparison for fcmpu/fcmpo.
// Operands must be in LOCAL_FPR0 and LOCAL_FPR1.
// Sets cr.fields[crfd] directly (no XER.SO — float compare uses FU bit instead).
static void EmitFPCRCompare(std::vector<u8>& code, GPRCache& cache, u32 crfd)
{
  static constexpr s64 CR_LT_VAL = static_cast<s64>(0xC000000000000001ULL);
  static constexpr s64 CR_GT_VAL = static_cast<s64>(0x0000000000000001ULL);
  static constexpr s64 CR_EQ_VAL = static_cast<s64>(0x8000000000000000ULL);
  static constexpr s64 CR_FU_VAL = static_cast<s64>(0x8800000000000001ULL);

  // Branchless FP CR comparison using 3 chained selects.
  // NaN: all f64 comparisons return 0, so default is CR_FU.
  // Step 1: select(CR_GT, CR_FU, a > b) → GT_or_FU
  EmitI64Const(code, CR_GT_VAL);
  EmitI64Const(code, CR_FU_VAL);
  EmitLocalGet(code, LOCAL_FPR0);
  EmitLocalGet(code, LOCAL_FPR1);
  code.push_back(WasmOp::F64Gt);
  code.push_back(WasmOp::Select);

  // Step 2: select(CR_EQ, GT_or_FU, a == b) → EQ_or_GT_or_FU
  EmitLocalSet(code, LOCAL_CR64);
  EmitI64Const(code, CR_EQ_VAL);
  EmitLocalGet(code, LOCAL_CR64);
  EmitLocalGet(code, LOCAL_FPR0);
  EmitLocalGet(code, LOCAL_FPR1);
  code.push_back(WasmOp::F64Eq);
  code.push_back(WasmOp::Select);

  // Step 3: select(CR_LT, EQ_or_GT_or_FU, a < b) → final
  EmitLocalSet(code, LOCAL_CR64);
  EmitI64Const(code, CR_LT_VAL);
  EmitLocalGet(code, LOCAL_CR64);
  EmitLocalGet(code, LOCAL_FPR0);
  EmitLocalGet(code, LOCAL_FPR1);
  code.push_back(WasmOp::F64Lt);
  code.push_back(WasmOp::Select);

  // Store to CR cache
  EmitStoreCR(code, cache, crfd);
}

// =====================================================================
// Dead CR0 elimination: skip Rc=1 CR0 updates when CR0 is overwritten before read
// =====================================================================

// Check if an instruction reads CR field 0
static bool ReadsCR0(const UGeckoInstruction inst)
{
  switch (inst.OPCD)
  {
  case 16:  // bc: reads CR if condition check is enabled
    return !(inst.BO & 0x10) && (inst.BI >> 2) == 0;
  case 19:
    switch (inst.SUBOP10)
    {
    case 16:   // bclr
    case 528:  // bcctr
      return !(inst.BO & 0x10) && (inst.BI >> 2) == 0;
    case 0:  // mcrf: reads source CR field
      return (inst.CRFS == 0);
    case 257: case 449: case 193: case 129:
    case 417: case 225: case 33: case 289:
      // CR logical ops: read CRBA and CRBB
      return ((inst.CRBA >> 2) == 0) || ((inst.CRBB >> 2) == 0);
    default:
      return false;
    }
  case 31:
    if (inst.SUBOP10 == 19)  // mfcr: reads all CR fields
      return true;
    return false;
  default:
    return false;
  }
}

// Check if an instruction unconditionally writes CR field 0
static bool WritesCR0(const UGeckoInstruction inst)
{
  switch (inst.OPCD)
  {
  case 10: case 11:  // cmpli, cmpi
    return (inst.CRFD == 0);
  case 28: case 29:  // andi., andis. — always update CR0
    return true;
  case 20: case 21: case 23:  // rlwimi., rlwinm., rlwnm.
    return inst.Rc != 0;
  case 31:
    switch (inst.SUBOP10)
    {
    case 0: case 32:  // cmp, cmpl
      return (inst.CRFD == 0);
    case 144:  // mtcrf
      return (inst.CRM & 0x80) != 0;
    case 150:  // stwcx. always writes CR0
      return true;
    default:
      return inst.Rc != 0;  // Rc=1 on opcode-31 instructions writes CR0
    }
  case 19:
    switch (inst.SUBOP10)
    {
    case 0:  // mcrf
      return (inst.CRFD == 0);
    case 257: case 449: case 193: case 129:
    case 417: case 225: case 33: case 289:
      return (inst.CRBD >> 2) == 0;
    default:
      return false;
    }
  default:
    return false;
  }
}

// Returns true if CR0 is dead after instruction at idx (overwritten before read).
static bool IsCR0DeadAfter(const PPCAnalyst::CodeOp* buffer, u32 idx, u32 num_instructions)
{
  for (u32 j = idx + 1; j < num_instructions; j++)
  {
    const UGeckoInstruction inst = buffer[j].inst;
    if (ReadsCR0(inst))
      return false;  // CR0 is read — needed
    if (WritesCR0(inst))
      return true;   // CR0 is overwritten before read — dead
  }
  return false;  // End of block — conservatively assume needed
}

// =====================================================================
// PPC Instruction Emitters
// =====================================================================

// Returns true if the instruction was emitted natively, false for fallback.
bool AotWasm::TryEmitWasmInstruction(std::vector<u8>& code, const PPCAnalyst::CodeOp& op,
                                      GPRCache& cache)
{
  const UGeckoInstruction inst = op.inst;
  const u32 opcode = inst.OPCD;

  switch (opcode)
  {
  case 3:  // twi: trap word immediate
  {
    const u32 TO = inst.TO;
    const s32 imm = static_cast<s32>(inst.SIMM_16);

    // Flush state before potential exception
    EmitFlushAllGPRs(code, cache);
    EmitFlushAllCR(code, cache);
    EmitFlushAllFPRs(code, cache);

    EmitGetGPRValue(code, cache, inst.RA);
    EmitLocalSet(code, LOCAL_TMP1);  // a

    // Build trap condition
    EmitI32Const(code, 0);  // accumulator

    if (TO & 0x10)  // a <s imm
    {
      EmitLocalGet(code, LOCAL_TMP1);
      EmitI32Const(code, imm);
      code.push_back(WasmOp::I32LtS);
      code.push_back(WasmOp::I32Or);
    }
    if (TO & 0x08)  // a >s imm
    {
      EmitLocalGet(code, LOCAL_TMP1);
      EmitI32Const(code, imm);
      code.push_back(WasmOp::I32GtS);
      code.push_back(WasmOp::I32Or);
    }
    if (TO & 0x04)  // a == imm
    {
      EmitLocalGet(code, LOCAL_TMP1);
      EmitI32Const(code, imm);
      code.push_back(WasmOp::I32Eq);
      code.push_back(WasmOp::I32Or);
    }
    if (TO & 0x02)  // a <u imm
    {
      EmitLocalGet(code, LOCAL_TMP1);
      EmitI32Const(code, imm);
      code.push_back(WasmOp::I32LtU);
      code.push_back(WasmOp::I32Or);
    }
    if (TO & 0x01)  // a >u imm
    {
      EmitLocalGet(code, LOCAL_TMP1);
      EmitI32Const(code, imm);
      code.push_back(WasmOp::I32GtU);
      code.push_back(WasmOp::I32Or);
    }

    code.push_back(WasmOp::If);
    code.push_back(WasmOp::Void);
    {
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Const(code, static_cast<s32>(op.address));
      EmitI32Store(code, OFF_PC);
      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load(code, OFF_EXCEPTIONS);
      EmitI32Const(code, 0x80);  // EXCEPTION_PROGRAM
      code.push_back(WasmOp::I32Or);
      EmitI32Store(code, OFF_EXCEPTIONS);
      EmitI32Const(code, 0);
      code.push_back(WasmOp::Return);
    }
    code.push_back(WasmOp::End);
    return true;
  }

  case 8:  // subfic: rd = SIMM - gpr[ra], CA = carry (unsigned borrow)
  {
    if (cache.is_const[inst.RA])
    {
      const u32 a = static_cast<u32>(cache.const_val[inst.RA]);
      const u32 imm = static_cast<u32>(static_cast<s32>(inst.SIMM_16));
      const s32 result = static_cast<s32>(imm - a);
      // CA = (a == 0) || ((~a + imm) < ~a)
      const u32 not_a = ~a;
      const bool ca = (a == 0) || ((not_a + imm) < not_a);
      EmitI32Const(code, result);
      EmitSetGPRValue(code, cache, inst.RD);
      cache.SetConst(inst.RD, result);
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Const(code, ca ? 1 : 0);
      EmitI32Store8(code, OFF_XER_CA);
      return true;
    }
    EmitGetGPRValue(code, cache, inst.RA);
    EmitLocalSet(code, LOCAL_TMP1);  // a

    // result = SIMM - a (note: PowerPC subfic computes imm - ra, not ra - imm)
    EmitI32Const(code, static_cast<s32>(inst.SIMM_16));
    EmitLocalGet(code, LOCAL_TMP1);
    code.push_back(WasmOp::I32Sub);
    EmitLocalSet(code, LOCAL_TMP2);  // result

    EmitLocalGet(code, LOCAL_TMP2);
    EmitSetGPRValue(code, cache, inst.RD);

    // CA: for subfic(SIMM, ra), CA = (~ra + SIMM + 1) has no unsigned borrow
    // Equivalent to: CA = (a == 0) || (result <= SIMM - 1) ... actually:
    // PPC carry for subtract: CA = !(a > SIMM) when a != 0
    // Simpler: CA = (SIMM >= a) ... No, PPC carry semantics are: ~a + imm + 1
    // CA = (~a + imm) overflows or ~a + imm + 1 overflows
    // Equivalent to: CA = (a == 0) || (result != 0 || imm != 0)... too complex.
    // Simplification: CA = (a == 0) || (((~a) + 1) <= imm) when SIMM unsigned...
    // Actually the correct formula is: CA = (a == 0) || ((~a) < imm)
    // Because ~a + imm + 1: overflow when (~a) + imm >= 2^32, i.e., (~a) >= 2^32 - imm
    // Which is (~a) > imm - 1 for imm > 0, or always true for imm == 0 only if a == 0
    // The interpreter uses: CA = a == 0 || Helper_Carry(~a, imm) where
    // Helper_Carry(a,b) = (a+b) < a  →  (~ra + imm) < ~ra  →  imm > 0 && ~ra > (~ra + imm)
    // Simplest correct version: CA = (a == 0) | (((u32)(~a) + (u32)imm) < (u32)(~a))
    EmitLocalGet(code, LOCAL_STATE);
    // Check a == 0
    EmitLocalGet(code, LOCAL_TMP1);
    code.push_back(WasmOp::I32Eqz);
    // Check (~a + imm) < ~a (unsigned overflow of ~a + imm)
    EmitI32Const(code, ~0);
    EmitLocalGet(code, LOCAL_TMP1);
    code.push_back(WasmOp::I32Xor);  // ~a
    EmitLocalSet(code, LOCAL_TMP3);   // save ~a
    EmitLocalGet(code, LOCAL_TMP3);
    EmitI32Const(code, static_cast<s32>(inst.SIMM_16));
    code.push_back(WasmOp::I32Add);  // ~a + imm
    EmitLocalGet(code, LOCAL_TMP3);
    code.push_back(WasmOp::I32LtU);  // (~a + imm) < ~a
    code.push_back(WasmOp::I32Or);   // a==0 || carry
    EmitI32Store8(code, OFF_XER_CA);
    return true;
  }

  case 12:  // addic: rd = gpr[ra] + SIMM, CA = carry
  case 13:  // addic.: same as addic but also updates CR0
  {
    if (cache.is_const[inst.RA])
    {
      const u32 a = static_cast<u32>(cache.const_val[inst.RA]);
      const u32 imm = static_cast<u32>(static_cast<s32>(inst.SIMM_16));
      const u32 result_u = a + imm;
      const s32 result = static_cast<s32>(result_u);
      const bool ca = result_u < a;
      EmitI32Const(code, result);
      EmitSetGPRValue(code, cache, inst.RD);
      cache.SetConst(inst.RD, result);
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Const(code, ca ? 1 : 0);
      EmitI32Store8(code, OFF_XER_CA);
      if (opcode == 13 && !cache.skip_cr0)
      {
        EmitI32Const(code, result);
        EmitLocalSet(code, LOCAL_TMP1);
        EmitUpdateCR0(code, cache);
      }
      return true;
    }
    // a = gpr[ra], imm = SIMM
    EmitGetGPRValue(code, cache, inst.RA);
    EmitLocalSet(code, LOCAL_TMP1);  // a
    EmitI32Const(code, static_cast<s32>(inst.SIMM_16));
    EmitLocalSet(code, LOCAL_TMP2);  // imm

    // rd = a + imm
    EmitLocalGet(code, LOCAL_TMP1);
    EmitLocalGet(code, LOCAL_TMP2);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_TMP3);  // result

    EmitLocalGet(code, LOCAL_TMP3);
    EmitSetGPRValue(code, cache, inst.RD);

    // CA = (result < a) for unsigned overflow detection
    EmitLocalGet(code, LOCAL_STATE);
    EmitLocalGet(code, LOCAL_TMP3);
    EmitLocalGet(code, LOCAL_TMP1);
    code.push_back(WasmOp::I32LtU);
    EmitI32Store8(code, OFF_XER_CA);

    if (opcode == 13)
    {
      EmitLocalGet(code, LOCAL_TMP3);
      EmitLocalSet(code, LOCAL_TMP1);
      if (!cache.skip_cr0)
        EmitUpdateCR0(code, cache);
    }
    return true;
  }

  case 14:  // addi: rd = (ra ? gpr[ra] : 0) + SIMM
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);
    const bool base_known = (inst.RA == 0) || cache.is_const[inst.RA];
    if (base_known)
    {
      const s32 base = (inst.RA == 0) ? 0 : cache.const_val[inst.RA];
      const s32 result = base + simm;
      EmitI32Const(code, result);
      EmitSetGPRValue(code, cache, inst.RD);
      cache.SetConst(inst.RD, result);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, simm);
      code.push_back(WasmOp::I32Add);
      EmitSetGPRValue(code, cache, inst.RD);
    }
    return true;
  }

  case 15:  // addis: rd = (ra ? gpr[ra] : 0) + (SIMM << 16)
  {
    const s32 shifted = static_cast<s32>(inst.SIMM_16) << 16;
    const bool base_known = (inst.RA == 0) || cache.is_const[inst.RA];
    if (base_known)
    {
      const s32 base = (inst.RA == 0) ? 0 : cache.const_val[inst.RA];
      const s32 result = base + shifted;
      EmitI32Const(code, result);
      EmitSetGPRValue(code, cache, inst.RD);
      cache.SetConst(inst.RD, result);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, shifted);
      code.push_back(WasmOp::I32Add);
      EmitSetGPRValue(code, cache, inst.RD);
    }
    return true;
  }

  case 24:  // ori: rd = gpr[ra] | UIMM
  {
    if (inst.RA == 0 && inst.RD == 0 && inst.UIMM == 0)
    {
      // nop
      return true;
    }
    if (cache.is_const[inst.RA])
    {
      const s32 result = cache.const_val[inst.RA] | static_cast<s32>(inst.UIMM);
      EmitI32Const(code, result);
      EmitSetGPRValue(code, cache, inst.RD);
      cache.SetConst(inst.RD, result);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, static_cast<s32>(inst.UIMM));
      code.push_back(WasmOp::I32Or);
      EmitSetGPRValue(code, cache, inst.RD);
    }
    return true;
  }

  case 25:  // oris: rd = gpr[ra] | (UIMM << 16)
  {
    const s32 shifted = static_cast<s32>(inst.UIMM) << 16;
    if (cache.is_const[inst.RA])
    {
      const s32 result = cache.const_val[inst.RA] | shifted;
      EmitI32Const(code, result);
      EmitSetGPRValue(code, cache, inst.RD);
      cache.SetConst(inst.RD, result);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, shifted);
      code.push_back(WasmOp::I32Or);
      EmitSetGPRValue(code, cache, inst.RD);
    }
    return true;
  }

  case 26:  // xori: rd = gpr[ra] ^ UIMM
  {
    if (cache.is_const[inst.RA])
    {
      const s32 result = cache.const_val[inst.RA] ^ static_cast<s32>(inst.UIMM);
      EmitI32Const(code, result);
      EmitSetGPRValue(code, cache, inst.RD);
      cache.SetConst(inst.RD, result);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, static_cast<s32>(inst.UIMM));
      code.push_back(WasmOp::I32Xor);
      EmitSetGPRValue(code, cache, inst.RD);
    }
    return true;
  }

  case 27:  // xoris: rd = gpr[ra] ^ (UIMM << 16)
  {
    const s32 shifted = static_cast<s32>(inst.UIMM) << 16;
    if (cache.is_const[inst.RA])
    {
      const s32 result = cache.const_val[inst.RA] ^ shifted;
      EmitI32Const(code, result);
      EmitSetGPRValue(code, cache, inst.RD);
      cache.SetConst(inst.RD, result);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, shifted);
      code.push_back(WasmOp::I32Xor);
      EmitSetGPRValue(code, cache, inst.RD);
    }
    return true;
  }

  case 28:  // andi.: ra = gpr[rs] & UIMM (always updates CR0)
  {
    if (cache.is_const[inst.RS])
    {
      const s32 result = cache.const_val[inst.RS] & static_cast<s32>(inst.UIMM);
      EmitI32Const(code, result);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitLocalGet(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RA);
      cache.SetConst(inst.RA, result);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RS);
      EmitI32Const(code, static_cast<s32>(inst.UIMM));
      code.push_back(WasmOp::I32And);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitLocalGet(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RA);
    }
    if (!cache.skip_cr0)
      EmitUpdateCR0(code, cache);
    return true;
  }

  case 29:  // andis.: ra = gpr[rs] & (UIMM << 16) (always updates CR0)
  {
    const s32 shifted = static_cast<s32>(inst.UIMM) << 16;
    if (cache.is_const[inst.RS])
    {
      const s32 result = cache.const_val[inst.RS] & shifted;
      EmitI32Const(code, result);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitLocalGet(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RA);
      cache.SetConst(inst.RA, result);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RS);
      EmitI32Const(code, shifted);
      code.push_back(WasmOp::I32And);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitLocalGet(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RA);
    }
    if (!cache.skip_cr0)
      EmitUpdateCR0(code, cache);
    return true;
  }

  case 7:  // mulli: rd = (s32)gpr[ra] * SIMM
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);
    if (cache.is_const[inst.RA])
    {
      const s32 result = cache.const_val[inst.RA] * simm;
      EmitI32Const(code, result);
      EmitSetGPRValue(code, cache, inst.RD);
      cache.SetConst(inst.RD, result);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RA);
      // Strength reduction: multiply by power of 2 → shift left
      if (simm > 0 && (simm & (simm - 1)) == 0)
      {
        u32 shift = 0;
        u32 tmp = static_cast<u32>(simm);
        while (tmp > 1) { tmp >>= 1; shift++; }
        EmitI32Const(code, static_cast<s32>(shift));
        code.push_back(WasmOp::I32Shl);
      }
      else
      {
        EmitI32Const(code, simm);
        code.push_back(WasmOp::I32Mul);
      }
      EmitSetGPRValue(code, cache, inst.RD);
    }
    return true;
  }

  case 21:  // rlwinmx: rd = ROTL32(gpr[ra], SH) & MASK(MB, ME)
  {
    u32 mask = ComputeRotMask(inst.MB, inst.ME);
    if (cache.is_const[inst.RA])
    {
      u32 val = static_cast<u32>(cache.const_val[inst.RA]);
      if (inst.SH != 0)
        val = (val << inst.SH) | (val >> (32 - inst.SH));
      val &= mask;
      const s32 result = static_cast<s32>(val);
      EmitI32Const(code, result);
      EmitSetGPRValue(code, cache, inst.RD);
      cache.SetConst(inst.RD, result);
      if (inst.Rc && !cache.skip_cr0)
      {
        EmitI32Const(code, result);
        EmitLocalSet(code, LOCAL_TMP1);
        EmitUpdateCR0(code, cache);
      }
      return true;
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RA);
      // Strength reduction for common rlwinm patterns
      if (inst.SH != 0 && mask == (0xFFFFFFFFu << inst.SH))
      {
        // slwi rD, rS, SH: just shift left
        EmitI32Const(code, static_cast<s32>(inst.SH));
        code.push_back(WasmOp::I32Shl);
      }
      else if (inst.SH != 0 && mask == (0xFFFFFFFFu >> (32 - inst.SH)))
      {
        // srwi rD, rS, (32-SH): just shift right unsigned
        EmitI32Const(code, static_cast<s32>(32 - inst.SH));
        code.push_back(WasmOp::I32ShrU);
      }
      else
      {
        if (inst.SH != 0)
          EmitRotl32(code, inst.SH);
        if (mask != 0xFFFFFFFF)
        {
          EmitI32Const(code, static_cast<s32>(mask));
          code.push_back(WasmOp::I32And);
        }
      }
      EmitLocalTee(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RD);
    }
    if (inst.Rc && !cache.skip_cr0)
      EmitUpdateCR0(code, cache);
    return true;
  }

  case 20:  // rlwimix: rd = (ROTL32(gpr[ra], SH) & mask) | (gpr[rd] & ~mask)
  {
    u32 mask = ComputeRotMask(inst.MB, inst.ME);

    if (cache.is_const[inst.RA] && cache.is_const[inst.RD])
    {
      u32 ra_val = static_cast<u32>(cache.const_val[inst.RA]);
      if (inst.SH != 0)
        ra_val = (ra_val << inst.SH) | (ra_val >> (32 - inst.SH));
      const s32 result = static_cast<s32>((ra_val & mask) |
                         (static_cast<u32>(cache.const_val[inst.RD]) & ~mask));
      EmitI32Const(code, result);
      EmitSetGPRValue(code, cache, inst.RD);
      cache.SetConst(inst.RD, result);
      if (inst.Rc && !cache.skip_cr0)
      {
        EmitI32Const(code, result);
        EmitLocalSet(code, LOCAL_TMP1);
        EmitUpdateCR0(code, cache);
      }
      return true;
    }

    // Compute ROTL32(gpr[ra], SH) & mask
    EmitGetGPRValue(code, cache, inst.RA);
    if (inst.SH != 0)
      EmitRotl32(code, inst.SH);
    EmitI32Const(code, static_cast<s32>(mask));
    code.push_back(WasmOp::I32And);

    // Compute gpr[rd] & ~mask
    EmitGetGPRValue(code, cache, inst.RD);
    EmitI32Const(code, static_cast<s32>(~mask));
    code.push_back(WasmOp::I32And);

    // OR them together
    code.push_back(WasmOp::I32Or);
    EmitLocalTee(code, LOCAL_TMP1);
    EmitSetGPRValue(code, cache, inst.RD);
    if (inst.Rc && !cache.skip_cr0)
      EmitUpdateCR0(code, cache);
    return true;
  }

  case 23:  // rlwnmx: rd = ROTL32(gpr[ra], gpr[rb] & 0x1f) & MASK(MB, ME)
  {
    u32 mask = ComputeRotMask(inst.MB, inst.ME);
    if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
    {
      u32 val = static_cast<u32>(cache.const_val[inst.RA]);
      const u32 sh = static_cast<u32>(cache.const_val[inst.RB]) & 0x1f;
      if (sh != 0)
        val = (val << sh) | (val >> (32 - sh));
      val &= mask;
      const s32 result = static_cast<s32>(val);
      EmitI32Const(code, result);
      EmitSetGPRValue(code, cache, inst.RD);
      cache.SetConst(inst.RD, result);
      if (inst.Rc && !cache.skip_cr0)
      {
        EmitI32Const(code, result);
        EmitLocalSet(code, LOCAL_TMP1);
        EmitUpdateCR0(code, cache);
      }
      return true;
    }
    EmitGetGPRValue(code, cache, inst.RA);
    EmitGetGPRValue(code, cache, inst.RB);
    EmitI32Const(code, 0x1f);
    code.push_back(WasmOp::I32And);
    code.push_back(WasmOp::I32Rotl);
    if (mask != 0xFFFFFFFF)
    {
      EmitI32Const(code, static_cast<s32>(mask));
      code.push_back(WasmOp::I32And);
    }
    EmitLocalSet(code, LOCAL_TMP1);
    EmitLocalGet(code, LOCAL_TMP1);
    EmitSetGPRValue(code, cache, inst.RD);
    if (inst.Rc && !cache.skip_cr0)
      EmitUpdateCR0(code, cache);
    return true;
  }

  case 11:  // cmpi: signed compare rA with SIMM, store to CR[crfD]
  {
    const s32 imm = static_cast<s32>(inst.SIMM_16);
    if (cache.is_const[inst.RA])
    {
      const s32 a = cache.const_val[inst.RA];
      static constexpr s64 CR_LT = static_cast<s64>(0xC000000000000001ULL);
      static constexpr s64 CR_GT = static_cast<s64>(0x0000000000000001ULL);
      static constexpr s64 CR_EQ = static_cast<s64>(0x8000000000000000ULL);
      const s64 cr_val = (a < imm) ? CR_LT : (a > imm) ? CR_GT : CR_EQ;
      EmitConstCRResult(code, cache, inst.CRFD, cr_val);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitI32Const(code, imm);
      EmitLocalSet(code, LOCAL_TMP2);
      EmitCRCompare(code, cache, inst.CRFD, true);
    }
    return true;
  }

  case 10:  // cmpli: unsigned compare rA with UIMM, store to CR[crfD]
  {
    const u32 imm = inst.UIMM;
    if (cache.is_const[inst.RA])
    {
      const u32 a = static_cast<u32>(cache.const_val[inst.RA]);
      static constexpr s64 CR_LT = static_cast<s64>(0xC000000000000001ULL);
      static constexpr s64 CR_GT = static_cast<s64>(0x0000000000000001ULL);
      static constexpr s64 CR_EQ = static_cast<s64>(0x8000000000000000ULL);
      const s64 cr_val = (a < imm) ? CR_LT : (a > imm) ? CR_GT : CR_EQ;
      EmitConstCRResult(code, cache, inst.CRFD, cr_val);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitI32Const(code, static_cast<s32>(imm));
      EmitLocalSet(code, LOCAL_TMP2);
      EmitCRCompare(code, cache, inst.CRFD, false);
    }
    return true;
  }

  case 32:  // lwz: rd = Read_U32(EA)
  case 40:  // lhz: rd = Read_U16(EA)
  case 34:  // lbz: rd = Read_U8(EA)
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);
    const u32 size = (opcode == 32) ? 4 : (opcode == 40) ? 2 : 1;

    // Constant-address MEM1 fast path: pre-compute WASM address at compile time
    if (inst.RA != 0 && cache.is_const[inst.RA])
    {
      const u32 uea = static_cast<u32>(cache.const_val[inst.RA] + simm);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitConstAddrLoadMEM1(code, size, wasm_addr);
        EmitSetGPRValue(code, cache, inst.RD);
        return true;
      }
    }

    // Compute EA = (ra ? gpr[ra] : 0) + SIMM
    bool ea_is_mem1 = (inst.RA == 1);  // stack-relative always MEM1
    if (inst.RA != 0 && cache.is_const[inst.RA])
    {
      EmitI32Const(code, cache.const_val[inst.RA] + simm);
    }
    else if (inst.RA != 0)
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, simm);
      code.push_back(WasmOp::I32Add);
    }
    else
    {
      EmitI32Const(code, simm);
    }
    EmitLocalSet(code, LOCAL_EA);

    if (ea_is_mem1)
      EmitDirectLoadMEM1(code, size, m_ram_base, m_ram_mask);
    else
    {
      u32 slow_func = (opcode == 32) ? FUNC_READ_U32 : (opcode == 40) ? FUNC_READ_U16 : FUNC_READ_U8;
      EmitDirectLoad(code, size, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, slow_func);
    }
    EmitSetGPRValue(code, cache, inst.RD);
    return true;
  }

  case 42:  // lha: rd = sign_extend_16(Read_U16(EA))
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);

    // Constant-address MEM1 fast path
    if (inst.RA != 0 && cache.is_const[inst.RA])
    {
      const u32 uea = static_cast<u32>(cache.const_val[inst.RA] + simm);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitConstAddrLoadMEM1(code, 2, wasm_addr);
        EmitI32Const(code, 16);
        code.push_back(WasmOp::I32Shl);
        EmitI32Const(code, 16);
        code.push_back(WasmOp::I32ShrS);
        EmitSetGPRValue(code, cache, inst.RD);
        return true;
      }
    }

    bool ea_is_mem1 = (inst.RA == 1);
    if (inst.RA != 0 && cache.is_const[inst.RA])
    {
      EmitI32Const(code, cache.const_val[inst.RA] + simm);
    }
    else if (inst.RA != 0)
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, simm);
      code.push_back(WasmOp::I32Add);
    }
    else
    {
      EmitI32Const(code, simm);
    }
    EmitLocalSet(code, LOCAL_EA);

    if (ea_is_mem1)
      EmitDirectLoadMEM1(code, 2, m_ram_base, m_ram_mask);
    else
      EmitDirectLoad(code, 2, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U16);
    // Sign-extend from 16-bit
    EmitI32Const(code, 16);
    code.push_back(WasmOp::I32Shl);
    EmitI32Const(code, 16);
    code.push_back(WasmOp::I32ShrS);
    EmitSetGPRValue(code, cache, inst.RD);
    return true;
  }

  case 33:  // lwzu: rd = Read_U32(EA), ra = EA
  case 41:  // lhzu: rd = Read_U16(EA), ra = EA
  case 35:  // lbzu: rd = Read_U8(EA), ra = EA
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);
    const u32 size = (opcode == 33) ? 4 : (opcode == 41) ? 2 : 1;

    // Constant-address MEM1 fast path + constant propagation for base
    if (cache.is_const[inst.RA])
    {
      const s32 ea = cache.const_val[inst.RA] + simm;
      const u32 uea = static_cast<u32>(ea);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitConstAddrLoadMEM1(code, size, wasm_addr);
        EmitSetGPRValue(code, cache, inst.RD);
        // ra = EA: store and propagate as constant
        EmitI32Const(code, ea);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.is_const[inst.RA] = true;
        cache.const_val[inst.RA] = ea;
        return true;
      }
    }

    bool ea_is_mem1 = (inst.RA == 1);
    if (cache.is_const[inst.RA])
    {
      EmitI32Const(code, cache.const_val[inst.RA] + simm);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, simm);
      code.push_back(WasmOp::I32Add);
    }
    EmitLocalSet(code, LOCAL_EA);

    if (ea_is_mem1)
      EmitDirectLoadMEM1(code, size, m_ram_base, m_ram_mask);
    else
    {
      u32 slow_func = (opcode == 33) ? FUNC_READ_U32 : (opcode == 41) ? FUNC_READ_U16 : FUNC_READ_U8;
      EmitDirectLoad(code, size, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, slow_func);
    }
    EmitSetGPRValue(code, cache, inst.RD);
    EmitLocalGet(code, LOCAL_EA);
    EmitSetGPRValue(code, cache, inst.RA);
    return true;
  }

  case 43:  // lhau: rd = sign_extend_16(Read_U16(EA)), ra = EA
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);

    // Constant-address MEM1 fast path + constant propagation
    if (cache.is_const[inst.RA])
    {
      const s32 ea = cache.const_val[inst.RA] + simm;
      const u32 uea = static_cast<u32>(ea);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitConstAddrLoadMEM1(code, 2, wasm_addr);
        EmitI32Const(code, 16);
        code.push_back(WasmOp::I32Shl);
        EmitI32Const(code, 16);
        code.push_back(WasmOp::I32ShrS);
        EmitSetGPRValue(code, cache, inst.RD);
        // ra = EA: propagate constant
        EmitI32Const(code, ea);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.is_const[inst.RA] = true;
        cache.const_val[inst.RA] = ea;
        return true;
      }
    }

    EmitGetGPRValue(code, cache, inst.RA);
    EmitI32Const(code, simm);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_EA);

    bool ea_is_mem1 = (inst.RA == 1);
    if (ea_is_mem1)
      EmitDirectLoadMEM1(code, 2, m_ram_base, m_ram_mask);
    else
      EmitDirectLoad(code, 2, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U16);
    // Sign-extend 16→32
    EmitI32Const(code, 16);
    code.push_back(WasmOp::I32Shl);
    EmitI32Const(code, 16);
    code.push_back(WasmOp::I32ShrS);
    EmitSetGPRValue(code, cache, inst.RD);
    EmitLocalGet(code, LOCAL_EA);
    EmitSetGPRValue(code, cache, inst.RA);
    return true;
  }

  case 36:  // stw: Write_U32(EA, gpr[rs])
  case 44:  // sth: Write_U16(EA, gpr[rs])
  case 38:  // stb: Write_U8(EA, gpr[rs])
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);
    const u32 size = (opcode == 36) ? 4 : (opcode == 44) ? 2 : 1;

    // Constant-address MEM1 fast path: skip LOCAL_EA and LOCAL_TMP1
    if (inst.RA != 0 && cache.is_const[inst.RA])
    {
      const u32 uea = static_cast<u32>(cache.const_val[inst.RA] + simm);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitGetGPRValue(code, cache, inst.RS);
        EmitConstAddrStoreMEM1(code, size, wasm_addr);
        return true;
      }
    }

    bool ea_is_mem1 = (inst.RA == 1);
    if (inst.RA != 0 && cache.is_const[inst.RA])
    {
      EmitI32Const(code, cache.const_val[inst.RA] + simm);
    }
    else if (inst.RA != 0)
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, simm);
      code.push_back(WasmOp::I32Add);
    }
    else
    {
      EmitI32Const(code, simm);
    }
    EmitLocalSet(code, LOCAL_EA);

    EmitGetGPRValue(code, cache, inst.RS);
    EmitLocalSet(code, LOCAL_TMP1);

    if (ea_is_mem1)
      EmitDirectStoreMEM1(code, size, m_ram_base, m_ram_mask);
    else
    {
      u32 slow_func =
          (opcode == 36) ? FUNC_WRITE_U32 : (opcode == 44) ? FUNC_WRITE_U16 : FUNC_WRITE_U8;
      EmitDirectStore(code, size, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, slow_func);
    }
    return true;
  }

  case 37:  // stwu: Write_U32(EA, gpr[rs]), ra = EA
  case 45:  // sthu: Write_U16(EA, gpr[rs]), ra = EA
  case 39:  // stbu: Write_U8(EA, gpr[rs]), ra = EA
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);
    const u32 size = (opcode == 37) ? 4 : (opcode == 45) ? 2 : 1;

    // Constant-address MEM1 fast path + constant propagation for base
    if (cache.is_const[inst.RA])
    {
      const s32 ea = cache.const_val[inst.RA] + simm;
      const u32 uea = static_cast<u32>(ea);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitGetGPRValue(code, cache, inst.RS);
        EmitConstAddrStoreMEM1(code, size, wasm_addr);
        // ra = EA: store and propagate as constant
        EmitI32Const(code, ea);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.is_const[inst.RA] = true;
        cache.const_val[inst.RA] = ea;
        return true;
      }
    }

    bool ea_is_mem1 = (inst.RA == 1);
    if (cache.is_const[inst.RA])
    {
      EmitI32Const(code, cache.const_val[inst.RA] + simm);
    }
    else
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, simm);
      code.push_back(WasmOp::I32Add);
    }
    EmitLocalSet(code, LOCAL_EA);

    EmitGetGPRValue(code, cache, inst.RS);
    EmitLocalSet(code, LOCAL_TMP1);

    if (ea_is_mem1)
      EmitDirectStoreMEM1(code, size, m_ram_base, m_ram_mask);
    else
    {
      u32 slow_func =
          (opcode == 37) ? FUNC_WRITE_U32 : (opcode == 45) ? FUNC_WRITE_U16 : FUNC_WRITE_U8;
      EmitDirectStore(code, size, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, slow_func);
    }

    EmitLocalGet(code, LOCAL_EA);
    EmitSetGPRValue(code, cache, inst.RA);
    return true;
  }

  case 46:  // lmw: load multiple words from gpr[rd] to r31
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);

    // Constant-address MEM1 fast path: all addresses known at compile time
    if (inst.RA != 0 && cache.is_const[inst.RA])
    {
      const u32 base_ea = static_cast<u32>(cache.const_val[inst.RA] + simm);
      if ((base_ea & 0x80000000) && !(base_ea & 0x10000000))
      {
        for (u32 r = inst.RD; r < 32; r++)
        {
          const u32 ea = base_ea + (r - inst.RD) * 4;
          const u32 wasm_addr = m_ram_base + (ea & m_ram_mask);
          EmitConstAddrLoadMEM1(code, 4, wasm_addr);
          EmitSetGPRValue(code, cache, r);
        }
        return true;
      }
    }

    // General path
    if (inst.RA != 0)
    {
      EmitGetGPRValue(code, cache, inst.RA);
    }
    else
    {
      EmitI32Const(code, 0);
    }
    EmitI32Const(code, simm);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_TMP2);

    for (u32 r = inst.RD; r < 32; r++)
    {
      EmitLocalGet(code, LOCAL_TMP2);
      if (r != inst.RD)
      {
        EmitI32Const(code, static_cast<s32>((r - inst.RD) * 4));
        code.push_back(WasmOp::I32Add);
      }
      EmitLocalSet(code, LOCAL_EA);

      bool ea_is_mem1 = (inst.RA == 1);
      if (ea_is_mem1)
        EmitDirectLoadMEM1(code, 4, m_ram_base, m_ram_mask);
      else
        EmitDirectLoad(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U32);
      EmitSetGPRValue(code, cache, r);
    }
    return true;
  }

  case 47:  // stmw: store multiple words from gpr[rs] to r31
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);

    // Constant-address MEM1 fast path
    if (inst.RA != 0 && cache.is_const[inst.RA])
    {
      const u32 base_ea = static_cast<u32>(cache.const_val[inst.RA] + simm);
      if ((base_ea & 0x80000000) && !(base_ea & 0x10000000))
      {
        for (u32 r = inst.RS; r < 32; r++)
        {
          const u32 ea = base_ea + (r - inst.RS) * 4;
          const u32 wasm_addr = m_ram_base + (ea & m_ram_mask);
          EmitGetGPRValue(code, cache, r);
          EmitConstAddrStoreMEM1(code, 4, wasm_addr);
        }
        return true;
      }
    }

    // General path
    if (inst.RA != 0)
    {
      EmitGetGPRValue(code, cache, inst.RA);
    }
    else
    {
      EmitI32Const(code, 0);
    }
    EmitI32Const(code, simm);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_TMP2);

    for (u32 r = inst.RS; r < 32; r++)
    {
      EmitLocalGet(code, LOCAL_TMP2);
      if (r != inst.RS)
      {
        EmitI32Const(code, static_cast<s32>((r - inst.RS) * 4));
        code.push_back(WasmOp::I32Add);
      }
      EmitLocalSet(code, LOCAL_EA);

      EmitGetGPRValue(code, cache, r);
      EmitLocalSet(code, LOCAL_TMP1);

      bool ea_is_mem1 = (inst.RA == 1);
      if (ea_is_mem1)
        EmitDirectStoreMEM1(code, 4, m_ram_base, m_ram_mask);
      else
        EmitDirectStore(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U32);
    }
    return true;
  }

  case 31:  // Extended opcode
  {
    const u32 subop = inst.SUBOP10;
    switch (subop)
    {
    case 0:  // cmp: signed compare rA with rB, store to CR[crfD]
    {
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const s32 a = cache.const_val[inst.RA];
        const s32 b = cache.const_val[inst.RB];
        static constexpr s64 CR_LT = static_cast<s64>(0xC000000000000001ULL);
        static constexpr s64 CR_GT = static_cast<s64>(0x0000000000000001ULL);
        static constexpr s64 CR_EQ = static_cast<s64>(0x8000000000000000ULL);
        const s64 cr_val = (a < b) ? CR_LT : (a > b) ? CR_GT : CR_EQ;
        EmitConstCRResult(code, cache, inst.CRFD, cr_val);
        return true;
      }
      EmitGetGPRValue(code, cache, inst.RA);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitGetGPRValue(code, cache, inst.RB);
      EmitLocalSet(code, LOCAL_TMP2);
      EmitCRCompare(code, cache, inst.CRFD, true);
      return true;
    }

    case 32:  // cmpl: unsigned compare rA with rB, store to CR[crfD]
    {
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const u32 a = static_cast<u32>(cache.const_val[inst.RA]);
        const u32 b = static_cast<u32>(cache.const_val[inst.RB]);
        static constexpr s64 CR_LT = static_cast<s64>(0xC000000000000001ULL);
        static constexpr s64 CR_GT = static_cast<s64>(0x0000000000000001ULL);
        static constexpr s64 CR_EQ = static_cast<s64>(0x8000000000000000ULL);
        const s64 cr_val = (a < b) ? CR_LT : (a > b) ? CR_GT : CR_EQ;
        EmitConstCRResult(code, cache, inst.CRFD, cr_val);
        return true;
      }
      EmitGetGPRValue(code, cache, inst.RA);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitGetGPRValue(code, cache, inst.RB);
      EmitLocalSet(code, LOCAL_TMP2);
      EmitCRCompare(code, cache, inst.CRFD, false);
      return true;
    }

    case 266:  // add: rd = gpr[ra] + gpr[rb]
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const s32 result = cache.const_val[inst.RA] + cache.const_val[inst.RB];
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RD);
        cache.SetConst(inst.RD, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
      }
      else
      {
        EmitGetGPRValue(code, cache, inst.RA);
        EmitGetGPRValue(code, cache, inst.RB);
        code.push_back(WasmOp::I32Add);
        EmitLocalTee(code, LOCAL_TMP1);
        EmitSetGPRValue(code, cache, inst.RD);
        if (inst.Rc && !cache.skip_cr0)
          EmitUpdateCR0(code, cache);
      }
      return true;

    case 40:  // subf: rd = gpr[rb] - gpr[ra]
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const s32 result = cache.const_val[inst.RB] - cache.const_val[inst.RA];
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RD);
        cache.SetConst(inst.RD, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
      }
      else
      {
        EmitGetGPRValue(code, cache, inst.RB);
        EmitGetGPRValue(code, cache, inst.RA);
        code.push_back(WasmOp::I32Sub);
        EmitLocalTee(code, LOCAL_TMP1);
        EmitSetGPRValue(code, cache, inst.RD);
        if (inst.Rc && !cache.skip_cr0)
          EmitUpdateCR0(code, cache);
      }
      return true;

    case 83:  // mfmsr: rd = MSR
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load(code, OFF_MSR);
      EmitSetGPRValue(code, cache, inst.RD);
      return true;

    case 146:  // mtmsr: MSR = gpr[rs]
      // Updating MSR is a context-changing operation (MMU, interrupts).
      // Explicitly fallback to interpreter.
      return false;

    case 28:  // and: ra = gpr[rs] & gpr[rb]
      if (cache.is_const[inst.RS] && cache.is_const[inst.RB])
      {
        const s32 result = cache.const_val[inst.RS] & cache.const_val[inst.RB];
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
      }
      else
      {
        EmitGetGPRValue(code, cache, inst.RS);
        EmitGetGPRValue(code, cache, inst.RB);
        code.push_back(WasmOp::I32And);
        EmitLocalTee(code, LOCAL_TMP1);
        EmitSetGPRValue(code, cache, inst.RA);
        if (inst.Rc && !cache.skip_cr0)
          EmitUpdateCR0(code, cache);
      }
      return true;

    case 444:  // or: ra = gpr[rs] | gpr[rb]  (mr rA, rS when RS == RB)
      if (inst.RS == inst.RB)
      {
        // mr rA, rS — simple register move
        if (inst.RA == inst.RS && !inst.Rc)
          return true;  // nop: mr rA, rA
        if (cache.is_const[inst.RS])
        {
          // Propagate known constant
          const s32 val = cache.const_val[inst.RS];
          EmitI32Const(code, val);
          EmitSetGPRValue(code, cache, inst.RA);
          cache.SetConst(inst.RA, val);
        }
        else
        {
          // Non-const move: single load, no OR needed
          EmitGetGPRValue(code, cache, inst.RS);
          EmitSetGPRValue(code, cache, inst.RA);
        }
      }
      else if (cache.is_const[inst.RS] && cache.is_const[inst.RB])
      {
        const s32 result = cache.const_val[inst.RS] | cache.const_val[inst.RB];
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
      }
      else
      {
        EmitGetGPRValue(code, cache, inst.RS);
        EmitGetGPRValue(code, cache, inst.RB);
        code.push_back(WasmOp::I32Or);
        EmitLocalTee(code, LOCAL_TMP1);
        EmitSetGPRValue(code, cache, inst.RA);
      }
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;

    case 316:  // xor: ra = gpr[rs] ^ gpr[rb]
      if (cache.is_const[inst.RS] && cache.is_const[inst.RB])
      {
        const s32 result = cache.const_val[inst.RS] ^ cache.const_val[inst.RB];
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
      }
      else
      {
        EmitGetGPRValue(code, cache, inst.RS);
        EmitGetGPRValue(code, cache, inst.RB);
        code.push_back(WasmOp::I32Xor);
        EmitLocalTee(code, LOCAL_TMP1);
        EmitSetGPRValue(code, cache, inst.RA);
        if (inst.Rc && !cache.skip_cr0)
          EmitUpdateCR0(code, cache);
      }
      return true;

    case 24:  // slw: ra = gpr[rs] << (gpr[rb] & 0x3f), 0 if shift >= 32
    {
      if (cache.is_const[inst.RS] && cache.is_const[inst.RB])
      {
        const u32 sh = static_cast<u32>(cache.const_val[inst.RB]) & 0x3f;
        const s32 result = (sh >= 32) ? 0 : static_cast<s32>(static_cast<u32>(cache.const_val[inst.RS]) << sh);
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }

      EmitGetGPRValue(code, cache, inst.RS);
      EmitGetGPRValue(code, cache, inst.RB);
      EmitI32Const(code, 0x1f);
      code.push_back(WasmOp::I32And);
      code.push_back(WasmOp::I32Shl);
      EmitLocalSet(code, LOCAL_TMP1);

      // If bit 5 of rb is set, result = 0; else result = shifted
      EmitGetGPRValue(code, cache, inst.RB);
      EmitI32Const(code, 0x20);
      code.push_back(WasmOp::I32And);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitI32Const(code, 0);
      EmitLocalSet(code, LOCAL_TMP1);
      code.push_back(WasmOp::End);
      EmitLocalGet(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RA);
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;
    }

    case 536:  // srw: ra = gpr[rs] >> (gpr[rb] & 0x3f), unsigned, 0 if shift >= 32
    {
      if (cache.is_const[inst.RS] && cache.is_const[inst.RB])
      {
        const u32 sh = static_cast<u32>(cache.const_val[inst.RB]) & 0x3f;
        const s32 result = (sh >= 32) ? 0 :
            static_cast<s32>(static_cast<u32>(cache.const_val[inst.RS]) >> sh);
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      EmitGetGPRValue(code, cache, inst.RS);
      EmitGetGPRValue(code, cache, inst.RB);
      EmitI32Const(code, 0x1f);
      code.push_back(WasmOp::I32And);
      code.push_back(WasmOp::I32ShrU);
      EmitLocalSet(code, LOCAL_TMP1);

      EmitGetGPRValue(code, cache, inst.RB);
      EmitI32Const(code, 0x20);
      code.push_back(WasmOp::I32And);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitI32Const(code, 0);
      EmitLocalSet(code, LOCAL_TMP1);
      code.push_back(WasmOp::End);
      EmitLocalGet(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RA);
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;
    }

    case 104:  // neg: rd = -gpr[ra]
      if (cache.is_const[inst.RA])
      {
        const s32 result = -cache.const_val[inst.RA];
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RD);
        cache.SetConst(inst.RD, result);
      }
      else
      {
        EmitI32Const(code, 0);
        EmitGetGPRValue(code, cache, inst.RA);
        code.push_back(WasmOp::I32Sub);
        EmitLocalTee(code, LOCAL_TMP1);
        EmitSetGPRValue(code, cache, inst.RD);
      }
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;

    case 235:  // mullw: rd = (s32)gpr[ra] * (s32)gpr[rb]
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const s32 result = cache.const_val[inst.RA] * cache.const_val[inst.RB];
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RD);
        cache.SetConst(inst.RD, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
      }
      else
      {
        // Strength reduction: mul by const power of 2 → shl
        auto emit_mul_or_shift = [&](u32 var_reg, s32 const_val) {
          EmitGetGPRValue(code, cache, var_reg);
          if (const_val > 0 && (const_val & (const_val - 1)) == 0)
          {
            u32 shift = 0;
            u32 tmp = static_cast<u32>(const_val);
            while (tmp > 1) { tmp >>= 1; shift++; }
            EmitI32Const(code, static_cast<s32>(shift));
            code.push_back(WasmOp::I32Shl);
          }
          else
          {
            EmitI32Const(code, const_val);
            code.push_back(WasmOp::I32Mul);
          }
        };

        if (cache.is_const[inst.RA])
        {
          emit_mul_or_shift(inst.RB, cache.const_val[inst.RA]);
        }
        else if (cache.is_const[inst.RB])
        {
          emit_mul_or_shift(inst.RA, cache.const_val[inst.RB]);
        }
        else
        {
          EmitGetGPRValue(code, cache, inst.RA);
          EmitGetGPRValue(code, cache, inst.RB);
          code.push_back(WasmOp::I32Mul);
        }
        EmitLocalTee(code, LOCAL_TMP1);
        EmitSetGPRValue(code, cache, inst.RD);
        if (inst.Rc && !cache.skip_cr0)
          EmitUpdateCR0(code, cache);
      }
      return true;

    // Indexed loads
    case 23:  // lwzx: rd = Read_U32(gpr[ra] + gpr[rb])
    case 279:  // lhzx
    case 87:  // lbzx
    {
      const u32 size = (subop == 23) ? 4 : (subop == 279) ? 2 : 1;

      // Constant-address MEM1 fast path
      const bool ra_const = (inst.RA == 0) || cache.is_const[inst.RA];
      if (ra_const && cache.is_const[inst.RB])
      {
        const s32 ra_val = (inst.RA == 0) ? 0 : cache.const_val[inst.RA];
        const u32 uea = static_cast<u32>(ra_val + cache.const_val[inst.RB]);
        if ((uea & 0x80000000) && !(uea & 0x10000000))
        {
          const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
          EmitConstAddrLoadMEM1(code, size, wasm_addr);
          EmitSetGPRValue(code, cache, inst.RD);
          return true;
        }
      }

      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      bool ea_is_mem1 = (inst.RA == 1);
      if (ea_is_mem1)
      {
        EmitDirectLoadMEM1(code, size, m_ram_base, m_ram_mask);
      }
      else
      {
        u32 slow_func =
            (subop == 23) ? FUNC_READ_U32 : (subop == 279) ? FUNC_READ_U16 : FUNC_READ_U8;
        EmitDirectLoad(code, size, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, slow_func);
      }
      EmitSetGPRValue(code, cache, inst.RD);
      return true;
    }

    // Indexed stores
    case 151:  // stwx
    case 407:  // sthx
    case 215:  // stbx
    {
      const u32 size = (subop == 151) ? 4 : (subop == 407) ? 2 : 1;

      // Constant-address MEM1 fast path
      const bool ra_const = (inst.RA == 0) || cache.is_const[inst.RA];
      if (ra_const && cache.is_const[inst.RB])
      {
        const s32 ra_val = (inst.RA == 0) ? 0 : cache.const_val[inst.RA];
        const u32 uea = static_cast<u32>(ra_val + cache.const_val[inst.RB]);
        if ((uea & 0x80000000) && !(uea & 0x10000000))
        {
          const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
          EmitGetGPRValue(code, cache, inst.RS);
          EmitConstAddrStoreMEM1(code, size, wasm_addr);
          return true;
        }
      }

      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      EmitGetGPRValue(code, cache, inst.RS);
      EmitLocalSet(code, LOCAL_TMP1);

      bool ea_is_mem1 = (inst.RA == 1);
      if (ea_is_mem1)
      {
        EmitDirectStoreMEM1(code, size, m_ram_base, m_ram_mask);
      }
      else
      {
        u32 slow_func =
            (subop == 151) ? FUNC_WRITE_U32 : (subop == 407) ? FUNC_WRITE_U16 : FUNC_WRITE_U8;
        EmitDirectStore(code, size, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, slow_func);
      }
      return true;
    }

    case 60:  // andc: ra = gpr[rs] & ~gpr[rb]
      if (cache.is_const[inst.RS] && cache.is_const[inst.RB])
      {
        const s32 result = cache.const_val[inst.RS] & ~cache.const_val[inst.RB];
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      EmitGetGPRValue(code, cache, inst.RS);
      EmitGetGPRValue(code, cache, inst.RB);
      EmitI32Const(code, -1);
      code.push_back(WasmOp::I32Xor);  // NOT
      code.push_back(WasmOp::I32And);
      EmitLocalTee(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RA);
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;

    case 476:  // nand: ra = ~(gpr[rs] & gpr[rb])
      if (cache.is_const[inst.RS] && cache.is_const[inst.RB])
      {
        const s32 result = ~(cache.const_val[inst.RS] & cache.const_val[inst.RB]);
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      EmitGetGPRValue(code, cache, inst.RS);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32And);
      EmitI32Const(code, -1);
      code.push_back(WasmOp::I32Xor);
      EmitLocalTee(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RA);
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;

    case 124:  // nor: ra = ~(gpr[rs] | gpr[rb])
      if (cache.is_const[inst.RS] && cache.is_const[inst.RB])
      {
        const s32 result = ~(cache.const_val[inst.RS] | cache.const_val[inst.RB]);
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      EmitGetGPRValue(code, cache, inst.RS);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Or);
      EmitI32Const(code, -1);
      code.push_back(WasmOp::I32Xor);
      EmitLocalTee(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RA);
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;

    case 284:  // eqv: ra = ~(gpr[rs] ^ gpr[rb])
      if (cache.is_const[inst.RS] && cache.is_const[inst.RB])
      {
        const s32 result = ~(cache.const_val[inst.RS] ^ cache.const_val[inst.RB]);
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      EmitGetGPRValue(code, cache, inst.RS);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Xor);
      EmitI32Const(code, -1);
      code.push_back(WasmOp::I32Xor);
      EmitLocalTee(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RA);
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;

    case 412:  // orc: ra = gpr[rs] | ~gpr[rb]
      if (cache.is_const[inst.RS] && cache.is_const[inst.RB])
      {
        const s32 result = cache.const_val[inst.RS] | ~cache.const_val[inst.RB];
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      EmitGetGPRValue(code, cache, inst.RS);
      EmitGetGPRValue(code, cache, inst.RB);
      EmitI32Const(code, -1);
      code.push_back(WasmOp::I32Xor);
      code.push_back(WasmOp::I32Or);
      EmitLocalTee(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RA);
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;

    case 19:  // mfcr: rd = GetCR() (all 8 CR fields packed into 32-bit)
    {
      // Reconstruct 32-bit CR from optimized 64-bit fields (using CR cache)
      EmitI32Const(code, 0);
      EmitLocalSet(code, LOCAL_TMP1);

      for (u32 f = 0; f < 8; f++)
      {
        const u32 shift = (7 - f) * 4;

        // LT: bit 62 → bit 3
        EmitCRReadUpper32(code, cache, f);
        EmitI32Const(code, 1 << (62 - 32));
        code.push_back(WasmOp::I32And);
        EmitI32Const(code, 0);
        code.push_back(WasmOp::I32Ne);
        EmitI32Const(code, 3);
        code.push_back(WasmOp::I32Shl);

        // GT: (s64)val > 0 → bit 2
        EmitLoadCR(code, cache, f);
        EmitI64Const(code, 0);
        code.push_back(WasmOp::I64GtS);
        EmitI32Const(code, 2);
        code.push_back(WasmOp::I32Shl);
        code.push_back(WasmOp::I32Or);

        // EQ: lower32 == 0 → bit 1
        EmitCRReadLower32(code, cache, f);
        code.push_back(WasmOp::I32Eqz);
        EmitI32Const(code, 1);
        code.push_back(WasmOp::I32Shl);
        code.push_back(WasmOp::I32Or);

        // SO: bit 59 → bit 0
        EmitCRReadUpper32(code, cache, f);
        EmitI32Const(code, 1 << (59 - 32));
        code.push_back(WasmOp::I32And);
        EmitI32Const(code, 0);
        code.push_back(WasmOp::I32Ne);
        code.push_back(WasmOp::I32Or);

        if (shift != 0)
        {
          EmitI32Const(code, static_cast<s32>(shift));
          code.push_back(WasmOp::I32Shl);
        }
        EmitLocalGet(code, LOCAL_TMP1);
        code.push_back(WasmOp::I32Or);
        EmitLocalSet(code, LOCAL_TMP1);
      }

      EmitLocalGet(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RD);
      return true;
    }

    case 144:  // mtcrf: set CR fields from GPR based on CRM mask
    {
      // CRM is an 8-bit mask: bit 7 = field 0, bit 0 = field 7
      const u32 crm = inst.CRM;
      for (u32 f = 0; f < 8; f++)
      {
        if (!((crm >> (7 - f)) & 1))
          continue;  // skip fields not in mask

        // Extract 4-bit value for this field: (gpr[rs] >> ((7-f)*4)) & 0xF
        EmitGetGPRValue(code, cache, inst.RS);
        const u32 shift = (7 - f) * 4;
        if (shift != 0)
        {
          EmitI32Const(code, static_cast<s32>(shift));
          code.push_back(WasmOp::I32ShrU);
        }
        EmitI32Const(code, 0xF);
        code.push_back(WasmOp::I32And);
        EmitLocalSet(code, LOCAL_TMP1);

        // Convert 4-bit CR value to optimized 64-bit format using s_crTable lookup
        // Since we can't call a table lookup, inline the conversion:
        // LT (bit 3): if set, bit 62 of i64 must be set
        // GT (bit 2): if set, val must be > 0 as s64
        // EQ (bit 1): if set, lower 32 must be 0
        // SO (bit 0): if set, bit 59 must be set
        //
        // Precomputed: use if/else chain for the 16 possible values
        // Simplified: construct the i64 from the 4 bits
        //   base = 0x8000000000000001 (negative, nonzero lower → not GT, not EQ)
        //   if GT: base = 0x0000000000000001 (positive nonzero)
        //   if EQ: base = 0x8000000000000000 or 0x0000000000000000
        //   if LT: base |= (1<<62)
        //   if SO: base |= (1<<59)
        //
        // Actually, build it step by step:
        // Start with 0x8000000000000001 (default: not LT, not GT, not EQ, not SO)
        EmitI64Const(code, static_cast<s64>(0x8000000000000001ULL));
        EmitLocalSet(code, LOCAL_CR64);

        // If GT (bit 2): flip to positive (clear bit 63)
        EmitLocalGet(code, LOCAL_TMP1);
        EmitI32Const(code, 4);
        code.push_back(WasmOp::I32And);
        code.push_back(WasmOp::If);
        code.push_back(WasmOp::Void);
        EmitI64Const(code, 1);  // positive, nonzero
        EmitLocalSet(code, LOCAL_CR64);
        code.push_back(WasmOp::End);

        // If EQ (bit 1): set lower32 to 0
        EmitLocalGet(code, LOCAL_TMP1);
        EmitI32Const(code, 2);
        code.push_back(WasmOp::I32And);
        code.push_back(WasmOp::If);
        code.push_back(WasmOp::Void);
        EmitI64Const(code, static_cast<s64>(0x8000000000000000ULL));
        EmitLocalSet(code, LOCAL_CR64);
        code.push_back(WasmOp::End);

        // If LT (bit 3): set bit 62
        EmitLocalGet(code, LOCAL_TMP1);
        EmitI32Const(code, 8);
        code.push_back(WasmOp::I32And);
        code.push_back(WasmOp::If);
        code.push_back(WasmOp::Void);
        EmitLocalGet(code, LOCAL_CR64);
        EmitI64Const(code, static_cast<s64>(0x4000000000000000ULL));
        code.push_back(WasmOp::I64Or);
        EmitLocalSet(code, LOCAL_CR64);
        code.push_back(WasmOp::End);

        // If SO (bit 0): set bit 59
        EmitLocalGet(code, LOCAL_TMP1);
        EmitI32Const(code, 1);
        code.push_back(WasmOp::I32And);
        code.push_back(WasmOp::If);
        code.push_back(WasmOp::Void);
        EmitLocalGet(code, LOCAL_CR64);
        EmitI64Const(code, static_cast<s64>(0x0800000000000000ULL));
        code.push_back(WasmOp::I64Or);
        EmitLocalSet(code, LOCAL_CR64);
        code.push_back(WasmOp::End);

        // Store to CR cache
        EmitLocalGet(code, LOCAL_CR64);
        EmitStoreCR(code, cache, f);
      }
      return true;
    }

    case 954:  // extsb: ra = sign_extend_8(gpr[rs])
      if (cache.is_const[inst.RS])
      {
        const s32 result = static_cast<s32>(static_cast<s8>(cache.const_val[inst.RS]));
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
      }
      else
      {
        EmitGetGPRValue(code, cache, inst.RS);
        EmitI32Const(code, 24);
        code.push_back(WasmOp::I32Shl);
        EmitI32Const(code, 24);
        code.push_back(WasmOp::I32ShrS);
        EmitLocalTee(code, LOCAL_TMP1);
        EmitSetGPRValue(code, cache, inst.RA);
        if (inst.Rc && !cache.skip_cr0)
          EmitUpdateCR0(code, cache);
      }
      return true;

    case 922:  // extsh: ra = sign_extend_16(gpr[rs])
      if (cache.is_const[inst.RS])
      {
        const s32 result = static_cast<s32>(static_cast<s16>(cache.const_val[inst.RS]));
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
      }
      else
      {
        EmitGetGPRValue(code, cache, inst.RS);
        EmitI32Const(code, 16);
        code.push_back(WasmOp::I32Shl);
        EmitI32Const(code, 16);
        code.push_back(WasmOp::I32ShrS);
        EmitLocalTee(code, LOCAL_TMP1);
        EmitSetGPRValue(code, cache, inst.RA);
        if (inst.Rc && !cache.skip_cr0)
          EmitUpdateCR0(code, cache);
      }
      return true;

    case 26:  // cntlzw: ra = count_leading_zeros(gpr[rs])
      if (cache.is_const[inst.RS])
      {
        const u32 val = static_cast<u32>(cache.const_val[inst.RS]);
        const s32 result = (val == 0) ? 32 : __builtin_clz(val);
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
      }
      else
      {
        EmitGetGPRValue(code, cache, inst.RS);
        code.push_back(WasmOp::I32Clz);
        EmitLocalTee(code, LOCAL_TMP1);
        EmitSetGPRValue(code, cache, inst.RA);
        if (inst.Rc && !cache.skip_cr0)
          EmitUpdateCR0(code, cache);
      }
      return true;

    case 459:  // divwu: rd = (u32)gpr[ra] / (u32)gpr[rb]
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const u32 a = static_cast<u32>(cache.const_val[inst.RA]);
        const u32 b = static_cast<u32>(cache.const_val[inst.RB]);
        const s32 result = (b == 0) ? 0 : static_cast<s32>(a / b);
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RD);
        cache.SetConst(inst.RD, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      // Check divisor != 0
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Eqz);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      // Division by zero: result = 0
      EmitI32Const(code, 0);
      EmitSetGPRValue(code, cache, inst.RD);
      code.push_back(WasmOp::Else);
      EmitGetGPRValue(code, cache, inst.RA);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32DivU);
      EmitSetGPRValue(code, cache, inst.RD);
      code.push_back(WasmOp::End);
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;

    case 491:  // divw: rd = (s32)gpr[ra] / (s32)gpr[rb]
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const s32 a = cache.const_val[inst.RA];
        const s32 b = cache.const_val[inst.RB];
        s32 result;
        if (b == 0 || (a == static_cast<s32>(0x80000000) && b == -1))
          result = 0;
        else
          result = a / b;
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RD);
        cache.SetConst(inst.RD, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      // Check divisor != 0 and not INT_MIN/-1 overflow
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Eqz);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitI32Const(code, 0);
      EmitSetGPRValue(code, cache, inst.RD);
      code.push_back(WasmOp::Else);
      // Check for INT_MIN / -1
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, static_cast<s32>(0x80000000));
      code.push_back(WasmOp::I32Eq);
      EmitGetGPRValue(code, cache, inst.RB);
      EmitI32Const(code, -1);
      code.push_back(WasmOp::I32Eq);
      code.push_back(WasmOp::I32And);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitI32Const(code, 0);
      EmitSetGPRValue(code, cache, inst.RD);
      code.push_back(WasmOp::Else);
      EmitGetGPRValue(code, cache, inst.RA);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32DivS);
      EmitSetGPRValue(code, cache, inst.RD);
      code.push_back(WasmOp::End);
      code.push_back(WasmOp::End);
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;

    case 824:  // srawi: ra = (s32)gpr[rs] >> SH, CA = sign bit & shifted-out bits
    {
      const u32 sh = inst.SH;
      if (cache.is_const[inst.RS])
      {
        const s32 rs = cache.const_val[inst.RS];
        const s32 result = rs >> sh;
        const bool ca = (sh > 0) && (rs < 0) && ((static_cast<u32>(rs) & ((1u << sh) - 1)) != 0);
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Const(code, ca ? 1 : 0);
        EmitI32Store8(code, OFF_XER_CA);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      // ra = (s32)gpr[rs] >> SH
      EmitGetGPRValue(code, cache, inst.RS);
      EmitI32Const(code, static_cast<s32>(sh));
      code.push_back(WasmOp::I32ShrS);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitLocalGet(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RA);

      // CA = (gpr[rs] < 0) && ((gpr[rs] & ((1 << SH) - 1)) != 0)
      if (sh == 0)
      {
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Const(code, 0);
        EmitI32Store8(code, OFF_XER_CA);
      }
      else
      {
        EmitLocalGet(code, LOCAL_STATE);
        EmitGetGPRValue(code, cache, inst.RS);
        EmitI32Const(code, 0);
        code.push_back(WasmOp::I32LtS);
        EmitGetGPRValue(code, cache, inst.RS);
        EmitI32Const(code, static_cast<s32>((1u << sh) - 1));
        code.push_back(WasmOp::I32And);
        EmitI32Const(code, 0);
        code.push_back(WasmOp::I32Ne);
        code.push_back(WasmOp::I32And);
        EmitI32Store8(code, OFF_XER_CA);
      }
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;
    }

    case 792:  // sraw: ra = (s32)gpr[rs] >> min(gpr[rb] & 0x3f, 31), CA = sign & shifted-out
    {
      if (cache.is_const[inst.RS] && cache.is_const[inst.RB])
      {
        const s32 rs = cache.const_val[inst.RS];
        const u32 sh = static_cast<u32>(cache.const_val[inst.RB]) & 0x3f;
        s32 result;
        bool ca;
        if (sh >= 32)
        {
          result = (rs < 0) ? -1 : 0;
          ca = (rs < 0);
        }
        else if (sh == 0)
        {
          result = rs;
          ca = false;
        }
        else
        {
          result = rs >> sh;
          ca = (rs < 0) && ((static_cast<u32>(rs) & ((1u << sh) - 1)) != 0);
        }
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.SetConst(inst.RA, result);
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Const(code, ca ? 1 : 0);
        EmitI32Store8(code, OFF_XER_CA);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      // shift = gpr[rb] & 0x3f
      EmitGetGPRValue(code, cache, inst.RB);
      EmitI32Const(code, 0x3f);
      code.push_back(WasmOp::I32And);
      EmitLocalSet(code, LOCAL_TMP2);  // shift amount

      // If shift >= 32: result = (rs < 0) ? -1 : 0, CA = (rs < 0)
      // If shift < 32: result = (s32)rs >> shift, CA = (rs < 0) && ((rs & ((1<<shift)-1)) != 0)
      EmitLocalGet(code, LOCAL_TMP2);
      EmitI32Const(code, 32);
      code.push_back(WasmOp::I32GeU);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      {
        // shift >= 32: result = arithmetic shift fills with sign
        EmitGetGPRValue(code, cache, inst.RS);
        EmitI32Const(code, 31);
        code.push_back(WasmOp::I32ShrS);
        EmitSetGPRValue(code, cache, inst.RA);
        // CA = (rs < 0)
        EmitLocalGet(code, LOCAL_STATE);
        EmitGetGPRValue(code, cache, inst.RS);
        EmitI32Const(code, 0);
        code.push_back(WasmOp::I32LtS);
        EmitI32Store8(code, OFF_XER_CA);
      }
      code.push_back(WasmOp::Else);
      {
        // shift < 32: normal arithmetic shift
        EmitGetGPRValue(code, cache, inst.RS);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32ShrS);
        EmitSetGPRValue(code, cache, inst.RA);
        // CA = (rs < 0) && ((rs & ((1 << shift) - 1)) != 0)
        EmitLocalGet(code, LOCAL_STATE);
        EmitGetGPRValue(code, cache, inst.RS);
        EmitI32Const(code, 0);
        code.push_back(WasmOp::I32LtS);
        // ((1 << shift) - 1)
        EmitI32Const(code, 1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32Shl);
        EmitI32Const(code, 1);
        code.push_back(WasmOp::I32Sub);
        EmitGetGPRValue(code, cache, inst.RS);
        code.push_back(WasmOp::I32And);
        EmitI32Const(code, 0);
        code.push_back(WasmOp::I32Ne);
        code.push_back(WasmOp::I32And);
        EmitI32Store8(code, OFF_XER_CA);
      }
      code.push_back(WasmOp::End);
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;
    }

    // Indexed load with update
    case 55:  // lwzux: rd = Read_U32(gpr[ra]+gpr[rb]), ra = EA
    case 311:  // lhzux
    case 119:  // lbzux
    {
      const u32 size = (subop == 55) ? 4 : (subop == 311) ? 2 : 1;

      // Constant-address MEM1 fast path + constant propagation
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const s32 ea = cache.const_val[inst.RA] + cache.const_val[inst.RB];
        const u32 uea = static_cast<u32>(ea);
        if ((uea & 0x80000000) && !(uea & 0x10000000))
        {
          const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
          EmitConstAddrLoadMEM1(code, size, wasm_addr);
          EmitSetGPRValue(code, cache, inst.RD);
          EmitI32Const(code, ea);
          EmitSetGPRValue(code, cache, inst.RA);
          cache.is_const[inst.RA] = true;
          cache.const_val[inst.RA] = ea;
          return true;
        }
      }

      EmitGetGPRValue(code, cache, inst.RA);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      bool ea_is_mem1 = (inst.RA == 1);
      if (ea_is_mem1)
      {
        EmitDirectLoadMEM1(code, size, m_ram_base, m_ram_mask);
      }
      else
      {
        u32 slow_func =
            (subop == 55) ? FUNC_READ_U32 : (subop == 311) ? FUNC_READ_U16 : FUNC_READ_U8;
        EmitDirectLoad(code, size, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, slow_func);
      }
      EmitSetGPRValue(code, cache, inst.RD);
      EmitLocalGet(code, LOCAL_EA);
      EmitSetGPRValue(code, cache, inst.RA);
      return true;
    }

    // Indexed store with update
    case 183:  // stwux
    case 439:  // sthux
    case 247:  // stbux
    {
      const u32 size = (subop == 183) ? 4 : (subop == 439) ? 2 : 1;

      // Constant-address MEM1 fast path + constant propagation
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const s32 ea = cache.const_val[inst.RA] + cache.const_val[inst.RB];
        const u32 uea = static_cast<u32>(ea);
        if ((uea & 0x80000000) && !(uea & 0x10000000))
        {
          const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
          EmitGetGPRValue(code, cache, inst.RS);
          EmitConstAddrStoreMEM1(code, size, wasm_addr);
          EmitI32Const(code, ea);
          EmitSetGPRValue(code, cache, inst.RA);
          cache.is_const[inst.RA] = true;
          cache.const_val[inst.RA] = ea;
          return true;
        }
      }

      EmitGetGPRValue(code, cache, inst.RA);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      EmitGetGPRValue(code, cache, inst.RS);
      EmitLocalSet(code, LOCAL_TMP1);

      bool ea_is_mem1 = (inst.RA == 1);
      if (ea_is_mem1)
      {
        EmitDirectStoreMEM1(code, size, m_ram_base, m_ram_mask);
      }
      else
      {
        u32 slow_func =
            (subop == 183) ? FUNC_WRITE_U32 : (subop == 439) ? FUNC_WRITE_U16 : FUNC_WRITE_U8;
        EmitDirectStore(code, size, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, slow_func);
      }

      EmitLocalGet(code, LOCAL_EA);
      EmitSetGPRValue(code, cache, inst.RA);
      return true;
    }

    case 343:  // lhax: rd = sign_extend_16(Read_U16(EA))
    {
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      EmitDirectLoad(code, 2, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U16);
      // Sign-extend 16→32
      EmitI32Const(code, 16);
      code.push_back(WasmOp::I32Shl);
      EmitI32Const(code, 16);
      code.push_back(WasmOp::I32ShrS);
      EmitSetGPRValue(code, cache, inst.RD);
      return true;
    }

    case 375:  // lhaux: rd = sign_extend_16(Read_U16(gpr[ra]+gpr[rb])), ra = EA
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      EmitDirectLoad(code, 2, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U16);
      // Sign-extend 16→32
      EmitI32Const(code, 16);
      code.push_back(WasmOp::I32Shl);
      EmitI32Const(code, 16);
      code.push_back(WasmOp::I32ShrS);
      EmitSetGPRValue(code, cache, inst.RD);
      EmitLocalGet(code, LOCAL_EA);
      EmitSetGPRValue(code, cache, inst.RA);
      return true;
    }

    case 339:  // mfspr: rd = SPR[spr]
    {
      const u32 spr = ((inst.hex >> 16) & 0x1f) | ((inst.hex >> 6) & 0x3e0);

      if (spr == 1)  // XER: reconstruct from split fields
      {
        // xer = xer_stringctrl | (xer_ca << 29) | (xer_so_ov << 30)
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Load16U(code, OFF_XER_STRINGCTRL);

        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Load8U(code, OFF_XER_CA);
        EmitI32Const(code, 29);
        code.push_back(WasmOp::I32Shl);
        code.push_back(WasmOp::I32Or);

        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Load8U(code, OFF_XER_SO_OV);
        EmitI32Const(code, 30);
        code.push_back(WasmOp::I32Shl);
        code.push_back(WasmOp::I32Or);

        EmitSetGPRValue(code, cache, inst.RD);
        return true;
      }

      // Direct SPR reads: LR, CTR, GQR0-7, DSISR, DAR, DEC, SRR0, SRR1,
      // SPRG0-3, HID0, HID2, TBL, TBU, and others
      if (spr == SPR_LR_IDX || spr == SPR_CTR_IDX ||
          (spr >= 912 && spr <= 919) ||  // GQR0-7
          spr == 18 || spr == 19 ||       // DSISR, DAR
          spr == 22 ||                    // DEC
          spr == 25 ||                    // SDR1
          spr == 26 || spr == 27 ||       // SRR0, SRR1
          (spr >= 272 && spr <= 275) ||   // SPRG0-3
          spr == 1008 || spr == 1009 ||   // HID0, HID1
          spr == 920 ||                   // HID2
          spr == 921 ||                   // WPAR
          spr == 268 || spr == 269 ||     // TBL, TBU
          spr == 952 || spr == 956 ||     // MMCR0, MMCR1
          spr == 953 || spr == 954 ||     // PMC1, PMC2
          spr == 957 || spr == 958)       // PMC3, PMC4
      {
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Load(code, OFF_SPR + spr * 4);
        EmitSetGPRValue(code, cache, inst.RD);
        return true;
      }
      return false;
    }

    case 371:  // mftb: move from time base (same encoding as mfspr)
    {
      const u32 spr = ((inst.hex >> 16) & 0x1f) | ((inst.hex >> 6) & 0x3e0);
      if (spr != 268 && spr != 269)
        return false;
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load(code, OFF_SPR + spr * 4);
      EmitSetGPRValue(code, cache, inst.RD);
      return true;
    }

    case 467:  // mtspr: SPR[spr] = gpr[rs]
    {
      const u32 spr = ((inst.hex >> 16) & 0x1f) | ((inst.hex >> 6) & 0x3e0);

      if (spr == 1)  // XER: split into component fields
      {
        // xer_stringctrl = val & 0xFFFF
        EmitLocalGet(code, LOCAL_STATE);
        EmitGetGPRValue(code, cache, inst.RS);
        EmitI32Const(code, 0xFFFF);
        code.push_back(WasmOp::I32And);
        EmitI32Store16(code, OFF_XER_STRINGCTRL);
        // xer_ca = (val >> 29) & 1
        EmitLocalGet(code, LOCAL_STATE);
        EmitGetGPRValue(code, cache, inst.RS);
        EmitI32Const(code, 29);
        code.push_back(WasmOp::I32ShrU);
        EmitI32Const(code, 1);
        code.push_back(WasmOp::I32And);
        EmitI32Store8(code, OFF_XER_CA);
        // xer_so_ov = (val >> 30) & 3
        EmitLocalGet(code, LOCAL_STATE);
        EmitGetGPRValue(code, cache, inst.RS);
        EmitI32Const(code, 30);
        code.push_back(WasmOp::I32ShrU);
        EmitI32Const(code, 3);
        code.push_back(WasmOp::I32And);
        EmitI32Store8(code, OFF_XER_SO_OV);
        return true;
      }

      // Direct SPR writes
      if (spr == SPR_LR_IDX || spr == SPR_CTR_IDX ||
          (spr >= 912 && spr <= 919) ||  // GQR0-7
          spr == 18 || spr == 19 ||       // DSISR, DAR
          spr == 22 ||                    // DEC
          spr == 25 ||                    // SDR1
          spr == 26 || spr == 27 ||       // SRR0, SRR1
          (spr >= 272 && spr <= 275) ||   // SPRG0-3
          spr == 1008 || spr == 1009 ||   // HID0, HID1
          spr == 920 ||                   // HID2
          spr == 921 ||                   // WPAR
          spr == 952 || spr == 956 ||     // MMCR0, MMCR1
          spr == 953 || spr == 954 ||     // PMC1, PMC2
          spr == 957 || spr == 958)       // PMC3, PMC4
      {
        EmitLocalGet(code, LOCAL_STATE);
        EmitGetGPRValue(code, cache, inst.RS);
        EmitI32Store(code, OFF_SPR + spr * 4);
        return true;
      }
      return false;
    }

    case 10:  // addc: rd = gpr[ra] + gpr[rb], CA = carry
    {
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const u32 a = static_cast<u32>(cache.const_val[inst.RA]);
        const u32 b = static_cast<u32>(cache.const_val[inst.RB]);
        const u32 result_u = a + b;
        const s32 result = static_cast<s32>(result_u);
        const bool ca = result_u < a;
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RD);
        cache.SetConst(inst.RD, result);
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Const(code, ca ? 1 : 0);
        EmitI32Store8(code, OFF_XER_CA);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      EmitGetGPRValue(code, cache, inst.RA);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitGetGPRValue(code, cache, inst.RB);
      EmitLocalGet(code, LOCAL_TMP1);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_TMP2);

      EmitLocalGet(code, LOCAL_TMP2);
      EmitSetGPRValue(code, cache, inst.RD);

      // CA = result < ra (unsigned carry)
      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_TMP2);
      EmitLocalGet(code, LOCAL_TMP1);
      code.push_back(WasmOp::I32LtU);
      EmitI32Store8(code, OFF_XER_CA);
      if (inst.Rc && !cache.skip_cr0)
      {
        EmitLocalGet(code, LOCAL_TMP2);
        EmitLocalSet(code, LOCAL_TMP1);
        EmitUpdateCR0(code, cache);
      }
      return true;
    }

    case 202:  // addze: rd = gpr[ra] + CA
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitLocalSet(code, LOCAL_TMP1);

      // result = ra + CA
      EmitLocalGet(code, LOCAL_TMP1);
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load8U(code, OFF_XER_CA);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_TMP2);

      EmitLocalGet(code, LOCAL_TMP2);
      EmitSetGPRValue(code, cache, inst.RD);

      // CA = result < ra (unsigned overflow from adding CA)
      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_TMP2);
      EmitLocalGet(code, LOCAL_TMP1);
      code.push_back(WasmOp::I32LtU);
      EmitI32Store8(code, OFF_XER_CA);
      if (inst.Rc && !cache.skip_cr0)
      {
        EmitLocalGet(code, LOCAL_TMP2);
        EmitLocalSet(code, LOCAL_TMP1);
        EmitUpdateCR0(code, cache);
      }
      return true;
    }

    case 234:  // addme: rd = gpr[ra] + CA - 1 (= ra + CA + 0xFFFFFFFF)
    {
      // Use i64 arithmetic for correct carry detection
      EmitGetGPRValue(code, cache, inst.RA);
      code.push_back(WasmOp::I64ExtendI32U);
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load8U(code, OFF_XER_CA);
      code.push_back(WasmOp::I64ExtendI32U);
      code.push_back(WasmOp::I64Add);
      EmitI64Const(code, 0xFFFFFFFF);
      code.push_back(WasmOp::I64Add);
      EmitLocalSet(code, LOCAL_CR64);

      EmitLocalGet(code, LOCAL_CR64);
      code.push_back(WasmOp::I32WrapI64);
      EmitLocalTee(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RD);

      // CA = (result >> 32) & 1
      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_CR64);
      EmitI64Const(code, 32);
      code.push_back(WasmOp::I64ShrU);
      code.push_back(WasmOp::I32WrapI64);
      EmitI32Store8(code, OFF_XER_CA);

      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;
    }

    case 75:  // mulhw: rd = high32((s32)gpr[ra] * (s32)gpr[rb])
    {
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const s64 a = static_cast<s64>(cache.const_val[inst.RA]);
        const s64 b = static_cast<s64>(cache.const_val[inst.RB]);
        const s32 result = static_cast<s32>((a * b) >> 32);
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RD);
        cache.SetConst(inst.RD, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      EmitGetGPRValue(code, cache, inst.RA);
      code.push_back(WasmOp::I64ExtendI32S);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I64ExtendI32S);
      code.push_back(WasmOp::I64Mul);
      EmitI64Const(code, 32);
      code.push_back(WasmOp::I64ShrS);
      code.push_back(WasmOp::I32WrapI64);
      EmitLocalSet(code, LOCAL_TMP1);

      EmitLocalGet(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RD);
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;
    }

    case 11:  // mulhwu: rd = high32((u32)gpr[ra] * (u32)gpr[rb])
    {
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const u64 a = static_cast<u64>(static_cast<u32>(cache.const_val[inst.RA]));
        const u64 b = static_cast<u64>(static_cast<u32>(cache.const_val[inst.RB]));
        const s32 result = static_cast<s32>(static_cast<u32>((a * b) >> 32));
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RD);
        cache.SetConst(inst.RD, result);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      EmitGetGPRValue(code, cache, inst.RA);
      code.push_back(WasmOp::I64ExtendI32U);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I64ExtendI32U);
      code.push_back(WasmOp::I64Mul);
      EmitI64Const(code, 32);
      code.push_back(WasmOp::I64ShrU);
      code.push_back(WasmOp::I32WrapI64);
      EmitLocalSet(code, LOCAL_TMP1);

      EmitLocalGet(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RD);
      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;
    }

    case 8:  // subfc: rd = gpr[rb] - gpr[ra], CA = carry
    {
      if (cache.is_const[inst.RA] && cache.is_const[inst.RB])
      {
        const u32 a = static_cast<u32>(cache.const_val[inst.RA]);
        const u32 b = static_cast<u32>(cache.const_val[inst.RB]);
        const s32 result = static_cast<s32>(b - a);
        const bool ca = b >= a;  // no borrow
        EmitI32Const(code, result);
        EmitSetGPRValue(code, cache, inst.RD);
        cache.SetConst(inst.RD, result);
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Const(code, ca ? 1 : 0);
        EmitI32Store8(code, OFF_XER_CA);
        if (inst.Rc && !cache.skip_cr0)
        {
          EmitI32Const(code, result);
          EmitLocalSet(code, LOCAL_TMP1);
          EmitUpdateCR0(code, cache);
        }
        return true;
      }
      EmitGetGPRValue(code, cache, inst.RA);
      EmitLocalSet(code, LOCAL_TMP1);  // a
      EmitGetGPRValue(code, cache, inst.RB);
      EmitLocalSet(code, LOCAL_TMP2);  // b

      // result = ~a + b + 1 = b - a
      EmitLocalGet(code, LOCAL_TMP2);
      EmitLocalGet(code, LOCAL_TMP1);
      code.push_back(WasmOp::I32Sub);
      EmitLocalSet(code, LOCAL_TMP3);  // result

      EmitLocalGet(code, LOCAL_TMP3);
      EmitSetGPRValue(code, cache, inst.RD);

      // CA = (a == 0) || (result <= b - 1 when b > 0) ...
      // Correct: CA = ~a + b overflows (before +1), or ~a + b + 1 overflows
      // Simplification: CA = (a == 0) || (result < b) ... no
      // PPC subfc carry: CA = !(borrow out) = !(a > b)
      // Actually: CA = (b >= a) unsigned
      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_TMP2);
      EmitLocalGet(code, LOCAL_TMP1);
      code.push_back(WasmOp::I32GeU);  // b >= a (no borrow)
      EmitI32Store8(code, OFF_XER_CA);

      if (inst.Rc && !cache.skip_cr0)
      {
        EmitLocalGet(code, LOCAL_TMP3);
        EmitLocalSet(code, LOCAL_TMP1);
        EmitUpdateCR0(code, cache);
      }
      return true;
    }

    case 136:  // subfe: rd = ~gpr[ra] + gpr[rb] + CA
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, -1);
      code.push_back(WasmOp::I32Xor);  // ~a
      EmitLocalSet(code, LOCAL_TMP1);

      EmitGetGPRValue(code, cache, inst.RB);
      EmitLocalSet(code, LOCAL_TMP2);  // b

      // result = ~a + b + CA
      EmitLocalGet(code, LOCAL_TMP1);
      EmitLocalGet(code, LOCAL_TMP2);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_TMP3);  // partial = ~a + b

      // CA_new = (~a + b) < ~a (carry from first add)
      EmitLocalGet(code, LOCAL_TMP3);
      EmitLocalGet(code, LOCAL_TMP1);
      code.push_back(WasmOp::I32LtU);
      EmitLocalSet(code, LOCAL_EA);  // carry1

      // Add old CA
      EmitLocalGet(code, LOCAL_TMP3);
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load8U(code, OFF_XER_CA);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_TMP3);  // final result

      EmitLocalGet(code, LOCAL_TMP3);
      EmitSetGPRValue(code, cache, inst.RD);

      // CA = carry1 || (partial + old_CA) < partial
      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_EA);
      EmitLocalGet(code, LOCAL_TMP3);
      EmitLocalGet(code, LOCAL_TMP1);
      EmitLocalGet(code, LOCAL_TMP2);
      code.push_back(WasmOp::I32Add);  // recompute partial for comparison
      code.push_back(WasmOp::I32LtU);
      code.push_back(WasmOp::I32Or);
      EmitI32Store8(code, OFF_XER_CA);

      if (inst.Rc && !cache.skip_cr0)
      {
        EmitLocalGet(code, LOCAL_TMP3);
        EmitLocalSet(code, LOCAL_TMP1);
        EmitUpdateCR0(code, cache);
      }
      return true;
    }

    case 138:  // adde: rd = gpr[ra] + gpr[rb] + CA
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitLocalSet(code, LOCAL_TMP1);

      // partial = ra + rb
      EmitLocalGet(code, LOCAL_TMP1);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_TMP2);  // partial

      // carry1 = partial < ra
      EmitLocalGet(code, LOCAL_TMP2);
      EmitLocalGet(code, LOCAL_TMP1);
      code.push_back(WasmOp::I32LtU);
      EmitLocalSet(code, LOCAL_EA);

      // result = partial + old_CA
      EmitLocalGet(code, LOCAL_TMP2);
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load8U(code, OFF_XER_CA);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_TMP3);

      EmitLocalGet(code, LOCAL_TMP3);
      EmitSetGPRValue(code, cache, inst.RD);

      // CA = carry1 || (result < partial)
      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_EA);
      EmitLocalGet(code, LOCAL_TMP3);
      EmitLocalGet(code, LOCAL_TMP2);
      code.push_back(WasmOp::I32LtU);
      code.push_back(WasmOp::I32Or);
      EmitI32Store8(code, OFF_XER_CA);

      if (inst.Rc && !cache.skip_cr0)
      {
        EmitLocalGet(code, LOCAL_TMP3);
        EmitLocalSet(code, LOCAL_TMP1);
        EmitUpdateCR0(code, cache);
      }
      return true;
    }

    // Memory barriers and cache ops: NOPs in emulation
    case 598:  // sync
    case 854:  // eieio
      return true;

    case 54:   // dcbst
    case 86:   // dcbf
    case 278:  // dcbt (prefetch hint)
    case 246:  // dcbtst (prefetch hint)
    case 982:  // icbi
      return true;  // NOPs in emulation

    case 1014:  // dcbz: zero a 32-byte cache line
    {
      // EA = (ra ? gpr[ra] : 0) + gpr[rb], aligned down to 32 bytes
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitI32Const(code, static_cast<s32>(~31u));  // align down to 32
      code.push_back(WasmOp::I32And);
      EmitLocalSet(code, LOCAL_TMP2);  // aligned base EA

      // Write 8 zero words (32 bytes)
      for (u32 i = 0; i < 8; i++)
      {
        EmitLocalGet(code, LOCAL_TMP2);
        if (i != 0)
        {
          EmitI32Const(code, static_cast<s32>(i * 4));
          code.push_back(WasmOp::I32Add);
        }
        EmitLocalSet(code, LOCAL_EA);
        EmitI32Const(code, 0);
        EmitLocalSet(code, LOCAL_TMP1);
        EmitDirectStore(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U32);
      }
      return true;
    }

    // lwarx/stwcx. (load-link/store-conditional for atomics)
    case 20:  // lwarx: rd = Read_U32(EA), set reservation
    {
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      // Load the value
      EmitDirectLoad(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U32);
      EmitSetGPRValue(code, cache, inst.RD);

      // Set reservation: reserve = true, reserve_address = EA
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Const(code, 1);
      EmitI32Store8(code, OFF_RESERVE);
      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_EA);
      EmitI32Store(code, OFF_RESERVE_ADDR);
      return true;
    }

    case 150:  // stwcx.: store conditional (always sets CR0)
    {
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      // if (reserve && reserve_address == EA): store, set CR0 = EQ
      // else: set CR0 = not-EQ
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load8U(code, OFF_RESERVE);
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load(code, OFF_RESERVE_ADDR);
      EmitLocalGet(code, LOCAL_EA);
      code.push_back(WasmOp::I32Eq);
      code.push_back(WasmOp::I32And);  // reserve && addr match

      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);

      // Store the value
      EmitGetGPRValue(code, cache, inst.RS);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitDirectStore(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U32);

      // CR0 = EQ (0x8000000000000000) | SO from XER
      EmitI64Const(code, static_cast<s64>(0x8000000000000000ULL));
      EmitLocalSet(code, LOCAL_CR64);

      code.push_back(WasmOp::Else);

      // CR0 = not-EQ (GT: 0x0000000000000001) | SO from XER
      EmitI64Const(code, static_cast<s64>(0x0000000000000001ULL));
      EmitLocalSet(code, LOCAL_CR64);

      code.push_back(WasmOp::End);

      // Clear reservation
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Const(code, 0);
      EmitI32Store8(code, OFF_RESERVE);

      // OR in XER.SO
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load8U(code, OFF_XER_SO_OV);
      EmitI32Const(code, 1);
      code.push_back(WasmOp::I32ShrU);
      EmitI32Const(code, 1);
      code.push_back(WasmOp::I32And);
      code.push_back(WasmOp::I64ExtendI32U);
      EmitI64Const(code, 59);
      code.push_back(WasmOp::I64Shl);
      EmitLocalGet(code, LOCAL_CR64);
      code.push_back(WasmOp::I64Or);
      EmitStoreCR(code, cache, 0);
      return true;
    }

    // Indexed FPU loads
    case 599:  // lfdx: ps[fd].ps0 = Read_U64(gpr[ra]+gpr[rb])
    {
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      if (inst.RA == 1)
        EmitDirectLoadMEM1_64(code, m_ram_base, m_ram_mask);
      else
        EmitDirectLoad64(code, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask);
      EmitLocalSet(code, LOCAL_CR64);

      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_CR64);
      EmitI64Store(code, OFF_PS + inst.FD * 16);
      InvalidateFPRDirect(cache, inst.FD);
      return true;
    }

    case 535:  // lfsx: load single indexed
    {
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      if (inst.RA == 1)
        EmitDirectLoadMEM1(code, 4, m_ram_base, m_ram_mask);
      else
        EmitDirectLoad(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U32);
      code.push_back(WasmOp::F32ReinterpretI32);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR0);

      EmitLocalGet(code, LOCAL_FPR0);
      EmitSetPS0Value(code, cache, inst.FD);
      EmitLocalGet(code, LOCAL_FPR0);
      EmitSetPS1Value(code, cache, inst.FD);
      return true;
    }

    // Indexed FPU stores
    case 727:  // stfdx: Write_U64(EA, ps[fs].ps0)
    {
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      EmitLocalGet(code, LOCAL_STATE);
      EmitI64Load(code, OFF_PS + inst.FS * 16);
      EmitLocalSet(code, LOCAL_CR64);

      if (inst.RA == 1)
        EmitDirectStoreMEM1_64(code, m_ram_base, m_ram_mask);
      else
        EmitDirectStore64(code, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask);
      return true;
    }

    case 663:  // stfsx: Write_U32(EA, ConvertToSingle(ps[fs].ps0))
    {
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      EmitLoadPS0(code, cache, inst.FS);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::I32ReinterpretF32);
      EmitLocalSet(code, LOCAL_TMP1);

      if (inst.RA == 1)
        EmitDirectStoreMEM1(code, 4, m_ram_base, m_ram_mask);
      else
        EmitDirectStore(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U32);
      return true;
    }

    // Indexed FPU loads with update
    case 631:  // lfdux
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      if (inst.RA == 1)
        EmitDirectLoadMEM1_64(code, m_ram_base, m_ram_mask);
      else
        EmitDirectLoad64(code, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask);
      EmitLocalSet(code, LOCAL_CR64);

      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_CR64);
      EmitI64Store(code, OFF_PS + inst.FD * 16);
      InvalidateFPRDirect(cache, inst.FD);
      EmitLocalGet(code, LOCAL_EA);
      EmitSetGPRValue(code, cache, inst.RA);
      return true;
    }

    case 567:  // lfsux
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      if (inst.RA == 1)
        EmitDirectLoadMEM1(code, 4, m_ram_base, m_ram_mask);
      else
        EmitDirectLoad(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U32);
      code.push_back(WasmOp::F32ReinterpretI32);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR0);

      EmitLocalGet(code, LOCAL_FPR0);
      EmitSetPS0Value(code, cache, inst.FD);
      EmitLocalGet(code, LOCAL_FPR0);
      EmitSetPS1Value(code, cache, inst.FD);
      EmitLocalGet(code, LOCAL_EA);
      EmitSetGPRValue(code, cache, inst.RA);
      return true;
    }

    // Indexed FPU stores with update
    case 759:  // stfdux
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      EmitLocalGet(code, LOCAL_STATE);
      EmitI64Load(code, OFF_PS + inst.FS * 16);
      EmitLocalSet(code, LOCAL_CR64);

      if (inst.RA == 1)
        EmitDirectStoreMEM1_64(code, m_ram_base, m_ram_mask);
      else
        EmitDirectStore64(code, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask);
      EmitLocalGet(code, LOCAL_EA);
      EmitSetGPRValue(code, cache, inst.RA);
      return true;
    }

    case 695:  // stfsux
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      EmitLoadPS0(code, cache, inst.FS);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::I32ReinterpretF32);
      EmitLocalSet(code, LOCAL_TMP1);

      if (inst.RA == 1)
        EmitDirectStoreMEM1(code, 4, m_ram_base, m_ram_mask);
      else
        EmitDirectStore(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U32);
      EmitLocalGet(code, LOCAL_EA);
      EmitSetGPRValue(code, cache, inst.RA);
      return true;
    }

    case 534:  // lwbrx: load word byte-reversed (= raw LE load, no bswap needed)
    {
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);
      EmitDirectLoad(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U32);
      // DirectLoad did bswap (gave us big-endian value), bswap again to get LE = byte-reversed
      EmitBswap32(code);
      EmitSetGPRValue(code, cache, inst.RD);
      return true;
    }

    case 790:  // lhbrx: load halfword byte-reversed
    {
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);
      EmitDirectLoad(code, 2, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U16);
      // DirectLoad did bswap16, undo it for byte-reversed result
      EmitBswap16(code);
      EmitSetGPRValue(code, cache, inst.RD);
      return true;
    }

    case 662:  // stwbrx: store word byte-reversed
    {
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);
      EmitGetGPRValue(code, cache, inst.RS);
      EmitBswap32(code);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitDirectStore(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U32);
      return true;
    }

    case 918:  // sthbrx: store halfword byte-reversed
    {
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);
      EmitGetGPRValue(code, cache, inst.RS);
      EmitBswap16(code);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitDirectStore(code, 2, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U16);
      return true;
    }

    // =================================================================
    // Additional cache/sync NOPs not already covered above
    // =================================================================
    case 470:  // dcbi: data cache block invalidate — NOP
    case 758:  // dcba: data cache block allocate — NOP
      return true;

    // =================================================================
    // subfze, subfme: carry-dependent arithmetic not covered above
    // =================================================================
    case 232:  // subfze: rd = ~gpr[ra] + CA, CA = carry
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, -1);
      code.push_back(WasmOp::I32Xor);  // ~ra
      code.push_back(WasmOp::I64ExtendI32U);
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load8U(code, OFF_XER_CA);
      code.push_back(WasmOp::I64ExtendI32U);
      code.push_back(WasmOp::I64Add);
      EmitLocalSet(code, LOCAL_CR64);

      EmitLocalGet(code, LOCAL_CR64);
      code.push_back(WasmOp::I32WrapI64);
      EmitLocalTee(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RD);

      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_CR64);
      EmitI64Const(code, 32);
      code.push_back(WasmOp::I64ShrU);
      code.push_back(WasmOp::I32WrapI64);
      EmitI32Store8(code, OFF_XER_CA);

      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;
    }

    case 200:  // subfme: rd = ~gpr[ra] + CA - 1, CA = carry
    {
      EmitGetGPRValue(code, cache, inst.RA);
      EmitI32Const(code, -1);
      code.push_back(WasmOp::I32Xor);  // ~ra
      code.push_back(WasmOp::I64ExtendI32U);
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load8U(code, OFF_XER_CA);
      code.push_back(WasmOp::I64ExtendI32U);
      code.push_back(WasmOp::I64Add);
      EmitI64Const(code, 0xFFFFFFFF);
      code.push_back(WasmOp::I64Add);
      EmitLocalSet(code, LOCAL_CR64);

      EmitLocalGet(code, LOCAL_CR64);
      code.push_back(WasmOp::I32WrapI64);
      EmitLocalTee(code, LOCAL_TMP1);
      EmitSetGPRValue(code, cache, inst.RD);

      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_CR64);
      EmitI64Const(code, 32);
      code.push_back(WasmOp::I64ShrU);
      code.push_back(WasmOp::I32WrapI64);
      EmitI32Store8(code, OFF_XER_CA);

      if (inst.Rc && !cache.skip_cr0)
        EmitUpdateCR0(code, cache);
      return true;
    }

    // =================================================================
    // Trap instruction (tw): conditional program exception
    // =================================================================
    case 4:  // tw: trap word
    {
      // Compare gpr[ra] and gpr[rb] using TO conditions
      const u32 TO = inst.TO;

      // Flush state before potential exception
      EmitFlushAllGPRs(code, cache);
      EmitFlushAllCR(code, cache);
      EmitFlushAllFPRs(code, cache);

      EmitGetGPRValue(code, cache, inst.RA);
      EmitLocalSet(code, LOCAL_TMP1);  // a
      EmitGetGPRValue(code, cache, inst.RB);
      EmitLocalSet(code, LOCAL_TMP2);  // b

      // Build trap condition: any matching TO bit triggers trap
      EmitI32Const(code, 0);  // accumulator

      if (TO & 0x10)  // a <s b
      {
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32LtS);
        code.push_back(WasmOp::I32Or);
      }
      if (TO & 0x08)  // a >s b
      {
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32GtS);
        code.push_back(WasmOp::I32Or);
      }
      if (TO & 0x04)  // a == b
      {
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32Eq);
        code.push_back(WasmOp::I32Or);
      }
      if (TO & 0x02)  // a <u b
      {
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32LtU);
        code.push_back(WasmOp::I32Or);
      }
      if (TO & 0x01)  // a >u b
      {
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32GtU);
        code.push_back(WasmOp::I32Or);
      }

      // If trap condition met, set EXCEPTION_PROGRAM and stop block
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      {
        // Set PC
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Const(code, static_cast<s32>(op.address));
        EmitI32Store(code, OFF_PC);
        // Set Exceptions |= PROGRAM (0x80)
        EmitLocalGet(code, LOCAL_STATE);
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Load(code, OFF_EXCEPTIONS);
        EmitI32Const(code, 0x80);
        code.push_back(WasmOp::I32Or);
        EmitI32Store(code, OFF_EXCEPTIONS);
        // Return 0 to stop chain
        EmitI32Const(code, 0);
        code.push_back(WasmOp::Return);
      }
      code.push_back(WasmOp::End);
      return true;
    }

    // =================================================================
    // String load/store instructions
    // =================================================================
    case 597:  // lswi: load string word immediate
    {
      // EA = (RA == 0) ? 0 : gpr[RA]
      // NB = (inst.NB == 0) ? 32 : inst.NB
      // Load NB bytes starting from EA into registers starting at RD
      const u32 nb = (inst.NB == 0) ? 32 : inst.NB;

      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitLocalSet(code, LOCAL_TMP2);  // base EA (preserved)

      u32 r = inst.RD;
      u32 bytes_loaded = 0;
      while (bytes_loaded < nb)
      {
        // Start of a new register: zero it
        EmitI32Const(code, 0);
        EmitSetGPRValue(code, cache, r);

        // Load up to 4 bytes into this register (MSB first = big-endian)
        for (u32 byte_in_reg = 0; byte_in_reg < 4 && bytes_loaded < nb; byte_in_reg++, bytes_loaded++)
        {
          // Read byte from memory at base_EA + bytes_loaded
          EmitLocalGet(code, LOCAL_TMP2);
          if (bytes_loaded != 0)
          {
            EmitI32Const(code, static_cast<s32>(bytes_loaded));
            code.push_back(WasmOp::I32Add);
          }
          EmitCall(code, FUNC_READ_U8);
          // Shift into position: byte 0 goes to bits 24-31, byte 1 to 16-23, etc.
          EmitI32Const(code, static_cast<s32>((3 - byte_in_reg) * 8));
          code.push_back(WasmOp::I32Shl);
          // OR into register
          EmitGetGPRValue(code, cache, r);
          code.push_back(WasmOp::I32Or);
          EmitSetGPRValue(code, cache, r);
        }
        r = (r + 1) & 31;
      }
      return true;
    }

    case 725:  // stswi: store string word immediate
    {
      const u32 nb = (inst.NB == 0) ? 32 : inst.NB;

      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitLocalSet(code, LOCAL_EA);

      u32 r = inst.RS;
      u32 bytes_stored = 0;
      while (bytes_stored < nb)
      {
        for (u32 byte_in_reg = 0; byte_in_reg < 4 && bytes_stored < nb; byte_in_reg++, bytes_stored++)
        {
          // Compute target address
          EmitLocalGet(code, LOCAL_EA);
          if (bytes_stored != 0)
          {
            EmitI32Const(code, static_cast<s32>(bytes_stored));
            code.push_back(WasmOp::I32Add);
          }
          // Extract byte from register: (gpr[r] >> ((3 - byte_in_reg) * 8)) & 0xFF
          EmitGetGPRValue(code, cache, r);
          EmitI32Const(code, static_cast<s32>((3 - byte_in_reg) * 8));
          code.push_back(WasmOp::I32ShrU);
          EmitI32Const(code, 0xFF);
          code.push_back(WasmOp::I32And);
          // Call write_u8(addr, value)
          EmitCall(code, FUNC_WRITE_U8);
        }
        r = (r + 1) & 31;
      }
      return true;
    }

    default:
      return false;
    }
  }

  case 19:  // Branch extended (blr, bctr, etc.)
  {
    const u32 subop = inst.SUBOP10;
    switch (subop)
    {
    case 16:  // bclr: branch to LR (conditional)
    {
      const u32 bo = inst.BO;
      const bool ctr_dec = (bo & 0x04) == 0;
      const bool skip_cond = (bo & 0x10) != 0;

      if (inst.LK)
      {
        // blrl: save pc+4 to LR, then branch to old LR
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Load(code, OFF_SPR + SPR_LR_IDX * 4);
        EmitLocalSet(code, LOCAL_TMP1);  // save old LR
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Const(code, static_cast<s32>(op.address + 4));
        EmitI32Store(code, OFF_SPR + SPR_LR_IDX * 4);
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalSet(code, LOCAL_NPC);
        return true;
      }

      // Save LR target
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load(code, OFF_SPR + SPR_LR_IDX * 4);
      EmitLocalSet(code, LOCAL_TMP1);

      if (ctr_dec)
      {
        // Decrement CTR
        EmitLocalGet(code, LOCAL_STATE);
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Load(code, OFF_SPR + SPR_CTR_IDX * 4);
        EmitI32Const(code, 1);
        code.push_back(WasmOp::I32Sub);
        EmitLocalTee(code, LOCAL_TMP2);
        EmitI32Store(code, OFF_SPR + SPR_CTR_IDX * 4);

        const bool branch_ctr_zero = (bo & 0x02) != 0;
        EmitLocalGet(code, LOCAL_TMP2);
        if (branch_ctr_zero)
          code.push_back(WasmOp::I32Eqz);
        // else: nonzero is truthy

        if (!skip_cond)
        {
          const bool branch_if_true = (bo & 0x08) != 0;
          EmitCRBitTest(code, cache, inst.BI);
          if (!branch_if_true)
            code.push_back(WasmOp::I32Eqz);
          code.push_back(WasmOp::I32And);
        }

        code.push_back(WasmOp::If);
        code.push_back(WasmOp::Void);
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalSet(code, LOCAL_NPC);
        code.push_back(WasmOp::End);
        return true;
      }

      // No CTR decrement
      if (skip_cond)
      {
        // Unconditional blr
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalSet(code, LOCAL_NPC);
      }
      else
      {
        const bool branch_if_true = (bo & 0x08) != 0;

        // Constant-CR branch elimination for bclr
        const int cr_bit = TryConstCRBitTest(cache, inst.BI);
        if (cr_bit >= 0)
        {
          const bool taken = branch_if_true ? (cr_bit != 0) : (cr_bit == 0);
          if (taken)
          {
            EmitLocalGet(code, LOCAL_TMP1);
            EmitLocalSet(code, LOCAL_NPC);
          }
          // else: not taken, NPC already in LOCAL_NPC as pc+4
        }
        else
        {
          EmitCRBitTest(code, cache, inst.BI);
          if (!branch_if_true)
            code.push_back(WasmOp::I32Eqz);
          code.push_back(WasmOp::If);
          code.push_back(WasmOp::Void);
          EmitLocalGet(code, LOCAL_TMP1);
          EmitLocalSet(code, LOCAL_NPC);
          code.push_back(WasmOp::End);
        }
      }
      return true;
    }

    case 528:  // bcctr: branch to CTR (conditional)
    {
      const u32 bo = inst.BO;
      if (inst.LK)
      {
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Const(code, static_cast<s32>(op.address + 4));
        EmitI32Store(code, OFF_SPR + SPR_LR_IDX * 4);
      }

      if (bo & 0x10)
      {
        // Unconditional bctr: npc = CTR
        EmitLocalGet(code, LOCAL_STATE);
        EmitI32Load(code, OFF_SPR + SPR_CTR_IDX * 4);
        EmitLocalSet(code, LOCAL_NPC);
      }
      else
      {
        const bool branch_if_true = (bo & 0x08) != 0;

        // Constant-CR branch elimination for bcctr
        const int cr_bit = TryConstCRBitTest(cache, inst.BI);
        if (cr_bit >= 0)
        {
          const bool taken = branch_if_true ? (cr_bit != 0) : (cr_bit == 0);
          if (taken)
          {
            EmitLocalGet(code, LOCAL_STATE);
            EmitI32Load(code, OFF_SPR + SPR_CTR_IDX * 4);
            EmitLocalSet(code, LOCAL_NPC);
          }
          // else: not taken, NPC already in LOCAL_NPC as pc+4
        }
        else
        {
          EmitCRBitTest(code, cache, inst.BI);
          if (!branch_if_true)
            code.push_back(WasmOp::I32Eqz);

          code.push_back(WasmOp::If);
          code.push_back(WasmOp::Void);
          EmitLocalGet(code, LOCAL_STATE);
          EmitI32Load(code, OFF_SPR + SPR_CTR_IDX * 4);
          EmitLocalSet(code, LOCAL_NPC);
          code.push_back(WasmOp::End);
        }
      }
      return true;
    }

    case 150:  // isync: instruction synchronize (NOP for us)
      return true;

    case 0:  // mcrf: copy one CR field to another
    {
      const u32 crfD = inst.CRFD;
      const u32 crfS = inst.CRFS;
      if (crfD == crfS)
        return true;  // nop
      EmitLoadCR(code, cache, crfS);
      EmitStoreCR(code, cache, crfD);
      return true;
    }

    // CR logical operations (crand, cror, crxor, crandc, crorc, crnand, crnor, creqv)
    // These operate on individual bits of the CR (addressed by crbD, crbA, crbB)
    case 257:  // crand
    case 449:  // cror
    case 193:  // crxor
    case 129:  // crandc
    case 417:  // crorc
    case 225:  // crnand
    case 33:   // crnor
    case 289:  // creqv
    {
      // Read CR bit A
      const u32 crbA = inst.RA;  // actually crbA
      const u32 crbB = inst.RB;  // actually crbB
      const u32 crbD = inst.RD;  // actually crbD

      // Read both CR bits using cached CR
      EmitCRBitTest(code, cache, crbA);
      EmitLocalSet(code, LOCAL_TMP1);
      EmitCRBitTest(code, cache, crbB);
      EmitLocalSet(code, LOCAL_TMP2);

      // Apply logical operation
      switch (subop)
      {
      case 257:  // crand
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32And);
        break;
      case 449:  // cror
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32Or);
        break;
      case 193:  // crxor
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32Xor);
        break;
      case 129:  // crandc: A & ~B
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32Eqz);
        code.push_back(WasmOp::I32And);
        break;
      case 417:  // crorc: A | ~B
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32Eqz);
        code.push_back(WasmOp::I32Or);
        break;
      case 225:  // crnand: ~(A & B)
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32And);
        code.push_back(WasmOp::I32Eqz);
        break;
      case 33:   // crnor: ~(A | B)
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32Or);
        code.push_back(WasmOp::I32Eqz);
        break;
      case 289:  // creqv: ~(A ^ B) = A == B
        EmitLocalGet(code, LOCAL_TMP1);
        EmitLocalGet(code, LOCAL_TMP2);
        code.push_back(WasmOp::I32Eq);
        break;
      default:
        return false;
      }
      EmitLocalSet(code, LOCAL_TMP1);  // result bit (0 or 1)

      // Write result CR bit: read target field, modify bit, write back
      const u32 dstField = crbD >> 2;
      const u32 dstSub = 3 - (crbD & 3);

      // Extract current 4-bit CR value for dstField (using CR cache)
      // LT
      EmitCRReadUpper32(code, cache, dstField);
      EmitI32Const(code, 1 << (62 - 32));
      code.push_back(WasmOp::I32And);
      EmitI32Const(code, 0);
      code.push_back(WasmOp::I32Ne);
      EmitI32Const(code, 3);
      code.push_back(WasmOp::I32Shl);
      // GT
      EmitLoadCR(code, cache, dstField);
      EmitI64Const(code, 0);
      code.push_back(WasmOp::I64GtS);
      EmitI32Const(code, 2);
      code.push_back(WasmOp::I32Shl);
      code.push_back(WasmOp::I32Or);
      // EQ
      EmitCRReadLower32(code, cache, dstField);
      code.push_back(WasmOp::I32Eqz);
      EmitI32Const(code, 1);
      code.push_back(WasmOp::I32Shl);
      code.push_back(WasmOp::I32Or);
      // SO
      EmitCRReadUpper32(code, cache, dstField);
      EmitI32Const(code, 1 << (59 - 32));
      code.push_back(WasmOp::I32And);
      EmitI32Const(code, 0);
      code.push_back(WasmOp::I32Ne);
      code.push_back(WasmOp::I32Or);
      EmitLocalSet(code, LOCAL_TMP2);  // current 4-bit value

      // Modify the target bit
      // Clear bit: val &= ~(1 << dstSub)... wait, bit numbering:
      // bit 3 = LT, bit 2 = GT, bit 1 = EQ, bit 0 = SO
      // dstSub: 0=SO, 1=EQ, 2=GT, 3=LT
      EmitLocalGet(code, LOCAL_TMP2);
      EmitI32Const(code, ~(1 << dstSub));
      code.push_back(WasmOp::I32And);
      EmitLocalGet(code, LOCAL_TMP1);
      EmitI32Const(code, static_cast<s32>(dstSub));
      code.push_back(WasmOp::I32Shl);
      code.push_back(WasmOp::I32Or);
      EmitLocalSet(code, LOCAL_TMP1);

      // Now convert 4-bit value back to optimized i64 format (same as mtcrf)
      EmitI64Const(code, static_cast<s64>(0x8000000000000001ULL));
      EmitLocalSet(code, LOCAL_CR64);

      EmitLocalGet(code, LOCAL_TMP1);
      EmitI32Const(code, 4);  // GT bit
      code.push_back(WasmOp::I32And);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitI64Const(code, 1);
      EmitLocalSet(code, LOCAL_CR64);
      code.push_back(WasmOp::End);

      EmitLocalGet(code, LOCAL_TMP1);
      EmitI32Const(code, 2);  // EQ bit
      code.push_back(WasmOp::I32And);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitI64Const(code, static_cast<s64>(0x8000000000000000ULL));
      EmitLocalSet(code, LOCAL_CR64);
      code.push_back(WasmOp::End);

      EmitLocalGet(code, LOCAL_TMP1);
      EmitI32Const(code, 8);  // LT bit
      code.push_back(WasmOp::I32And);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitLocalGet(code, LOCAL_CR64);
      EmitI64Const(code, static_cast<s64>(0x4000000000000000ULL));
      code.push_back(WasmOp::I64Or);
      EmitLocalSet(code, LOCAL_CR64);
      code.push_back(WasmOp::End);

      EmitLocalGet(code, LOCAL_TMP1);
      EmitI32Const(code, 1);  // SO bit
      code.push_back(WasmOp::I32And);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitLocalGet(code, LOCAL_CR64);
      EmitI64Const(code, static_cast<s64>(0x0800000000000000ULL));
      code.push_back(WasmOp::I64Or);
      EmitLocalSet(code, LOCAL_CR64);
      code.push_back(WasmOp::End);

      EmitLocalGet(code, LOCAL_CR64);
      EmitStoreCR(code, cache, dstField);
      return true;
    }

    default:
      return false;
    }
  }

  case 18:  // b/bl: unconditional branch
  {
    // Target address: sign-extend 24-bit LI field, shift left 2
    s32 li = static_cast<s32>(inst.LI << 8) >> 6;  // sign-extend 24-bit, shift left 2
    u32 target;
    if (inst.AA)
      target = static_cast<u32>(li);  // absolute
    else
      target = static_cast<u32>(static_cast<s32>(op.address) + li);  // relative

    // If link bit set: LR = pc + 4
    if (inst.LK)
    {
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Const(code, static_cast<s32>(op.address + 4));
      EmitI32Store(code, OFF_SPR + SPR_LR_IDX * 4);
    }

    // npc = target (stored in LOCAL_NPC, not memory)
    EmitI32Const(code, static_cast<s32>(target));
    EmitLocalSet(code, LOCAL_NPC);
    return true;
  }

  case 16:  // bc: conditional branch
  {
    const u32 bo = inst.BO;
    const u32 bi = inst.BI;

    s32 bd = static_cast<s32>(static_cast<s16>(inst.BD << 2));
    u32 target;
    if (inst.AA)
      target = static_cast<u32>(bd);
    else
      target = static_cast<u32>(static_cast<s32>(op.address) + bd);

    // If link bit set: LR = pc + 4
    if (inst.LK)
    {
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Const(code, static_cast<s32>(op.address + 4));
      EmitI32Store(code, OFF_SPR + SPR_LR_IDX * 4);
    }

    const bool ctr_decrement = (bo & 0x04) == 0;
    const bool skip_cond_check = (bo & 0x10) != 0;

    if (ctr_decrement && skip_cond_check)
    {
      // bdnz / bdz: decrement CTR, branch based on CTR==0 or CTR!=0
      // BO[1] (bit 1): 0 = branch if CTR != 0 (bdnz), 1 = branch if CTR == 0 (bdz)
      const bool branch_if_ctr_zero = (bo & 0x02) != 0;

      // CTR = CTR - 1
      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load(code, OFF_SPR + SPR_CTR_IDX * 4);
      EmitI32Const(code, 1);
      code.push_back(WasmOp::I32Sub);
      EmitLocalTee(code, LOCAL_TMP1);
      EmitI32Store(code, OFF_SPR + SPR_CTR_IDX * 4);

      // Check CTR
      EmitLocalGet(code, LOCAL_TMP1);
      if (branch_if_ctr_zero)
        code.push_back(WasmOp::I32Eqz);  // bdz: branch if CTR == 0
      // else bdnz: branch if CTR != 0 (nonzero is truthy for if)

      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitI32Const(code, static_cast<s32>(target));
      EmitLocalSet(code, LOCAL_NPC);
      code.push_back(WasmOp::End);
      return true;
    }

    if (ctr_decrement && !skip_cond_check)
    {
      // CTR decrement + CR condition check
      const bool branch_if_ctr_zero = (bo & 0x02) != 0;
      const bool branch_if_true = (bo & 0x08) != 0;

      // CTR = CTR - 1 (always executed regardless of CR)
      EmitLocalGet(code, LOCAL_STATE);
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load(code, OFF_SPR + SPR_CTR_IDX * 4);
      EmitI32Const(code, 1);
      code.push_back(WasmOp::I32Sub);
      EmitLocalTee(code, LOCAL_TMP1);
      EmitI32Store(code, OFF_SPR + SPR_CTR_IDX * 4);

      // If CR condition is known at compile time, simplify
      const int cr_bit = TryConstCRBitTest(cache, bi);
      if (cr_bit >= 0)
      {
        const bool cr_cond = branch_if_true ? (cr_bit != 0) : (cr_bit == 0);
        if (!cr_cond)
        {
          // CR condition is always false → branch never taken (CTR still decremented)
          return true;
        }
        // CR condition is always true → only CTR condition matters
        EmitLocalGet(code, LOCAL_TMP1);
        if (branch_if_ctr_zero)
          code.push_back(WasmOp::I32Eqz);
      }
      else
      {
        // CTR condition
        EmitLocalGet(code, LOCAL_TMP1);
        if (branch_if_ctr_zero)
          code.push_back(WasmOp::I32Eqz);

        // CR condition (runtime)
        EmitCRBitTest(code, cache, bi);
        if (!branch_if_true)
          code.push_back(WasmOp::I32Eqz);

        // Both conditions must be true
        code.push_back(WasmOp::I32And);
      }

      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitI32Const(code, static_cast<s32>(target));
      EmitLocalSet(code, LOCAL_NPC);
      code.push_back(WasmOp::End);
      return true;
    }

    // No CTR decrement cases (BO bit 2 == 1)
    if (skip_cond_check)
    {
      // Unconditional (from BC perspective)
      EmitI32Const(code, static_cast<s32>(target));
      EmitLocalSet(code, LOCAL_NPC);
      return true;
    }

    // Conditional branch: check CR bit BI using cached CR
    {
      const bool branch_if_true = (bo & 0x08) != 0;

      // Constant-CR branch elimination: resolve at compile time if CR bit is known
      const int cr_bit = TryConstCRBitTest(cache, bi);
      if (cr_bit >= 0)
      {
        const bool taken = branch_if_true ? (cr_bit != 0) : (cr_bit == 0);
        if (taken)
        {
          EmitI32Const(code, static_cast<s32>(target));
          EmitLocalSet(code, LOCAL_NPC);
        }
        // else: not taken, NPC already set to pc+4 in LOCAL_NPC
        return true;
      }

      EmitCRBitTest(code, cache, bi);
      if (!branch_if_true)
        code.push_back(WasmOp::I32Eqz);

      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitI32Const(code, static_cast<s32>(target));
      EmitLocalSet(code, LOCAL_NPC);
      code.push_back(WasmOp::End);

      return true;
    }
  }

  // =====================================================================
  // FPU Load/Store Instructions
  // =====================================================================
  case 50:  // lfd: ps[fd].ps0 = Read_U64(EA)
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);

    // Constant-address MEM1 fast path
    if (inst.RA != 0 && cache.is_const[inst.RA])
    {
      const u32 uea = static_cast<u32>(cache.const_val[inst.RA] + simm);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitLocalGet(code, LOCAL_STATE);
        EmitConstAddrLoadMEM1_64(code, wasm_addr);
        EmitI64Store(code, OFF_PS + inst.FD * 16);
        InvalidateFPRDirect(cache, inst.FD);
        return true;
      }
    }

    if (inst.RA != 0)
    {
      EmitGetGPRValue(code, cache, inst.RA);
    }
    else
    {
      EmitI32Const(code, 0);
    }
    EmitI32Const(code, simm);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_EA);

    const bool lfd_mem1 = (inst.RA == 1);
    if (lfd_mem1)
      EmitDirectLoadMEM1_64(code, m_ram_base, m_ram_mask);
    else
      EmitDirectLoad64(code, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask);
    EmitLocalSet(code, LOCAL_CR64);

    EmitLocalGet(code, LOCAL_STATE);
    EmitLocalGet(code, LOCAL_CR64);
    EmitI64Store(code, OFF_PS + inst.FD * 16);
    InvalidateFPRDirect(cache, inst.FD);
    return true;
  }

  case 51:  // lfdu: lfd + ra = EA
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);

    // Constant-address MEM1 fast path + constant propagation
    if (cache.is_const[inst.RA])
    {
      const s32 ea = cache.const_val[inst.RA] + simm;
      const u32 uea = static_cast<u32>(ea);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitLocalGet(code, LOCAL_STATE);
        EmitConstAddrLoadMEM1_64(code, wasm_addr);
        EmitI64Store(code, OFF_PS + inst.FD * 16);
        InvalidateFPRDirect(cache, inst.FD);
        EmitI32Const(code, ea);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.is_const[inst.RA] = true;
        cache.const_val[inst.RA] = ea;
        return true;
      }
    }

    EmitGetGPRValue(code, cache, inst.RA);
    EmitI32Const(code, simm);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_EA);

    const bool lfdu_mem1 = (inst.RA == 1);
    if (lfdu_mem1)
      EmitDirectLoadMEM1_64(code, m_ram_base, m_ram_mask);
    else
      EmitDirectLoad64(code, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask);
    EmitLocalSet(code, LOCAL_CR64);

    EmitLocalGet(code, LOCAL_STATE);
    EmitLocalGet(code, LOCAL_CR64);
    EmitI64Store(code, OFF_PS + inst.FD * 16);
    InvalidateFPRDirect(cache, inst.FD);
    EmitLocalGet(code, LOCAL_EA);
    EmitSetGPRValue(code, cache, inst.RA);
    return true;
  }

  case 48:  // lfs: load single, convert to double, store to both ps0 and ps1
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);

    // Constant-address MEM1 fast path
    if (inst.RA != 0 && cache.is_const[inst.RA])
    {
      const u32 uea = static_cast<u32>(cache.const_val[inst.RA] + simm);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitConstAddrLoadMEM1(code, 4, wasm_addr);
        code.push_back(WasmOp::F32ReinterpretI32);
        code.push_back(WasmOp::F64PromoteF32);
        EmitLocalSet(code, LOCAL_FPR0);
        EmitLocalGet(code, LOCAL_FPR0);
        EmitSetPS0Value(code, cache, inst.FD);
        EmitLocalGet(code, LOCAL_FPR0);
        EmitSetPS1Value(code, cache, inst.FD);
        return true;
      }
    }

    if (inst.RA != 0)
    {
      EmitGetGPRValue(code, cache, inst.RA);
    }
    else
    {
      EmitI32Const(code, 0);
    }
    EmitI32Const(code, simm);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_EA);

    bool ea_is_mem1 = (inst.RA == 1);
    if (ea_is_mem1)
      EmitDirectLoadMEM1(code, 4, m_ram_base, m_ram_mask);
    else
      EmitDirectLoad(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U32);
    code.push_back(WasmOp::F32ReinterpretI32);
    code.push_back(WasmOp::F64PromoteF32);
    EmitLocalSet(code, LOCAL_FPR0);

    EmitLocalGet(code, LOCAL_FPR0);
    EmitSetPS0Value(code, cache, inst.FD);
    EmitLocalGet(code, LOCAL_FPR0);
    EmitSetPS1Value(code, cache, inst.FD);
    return true;
  }

  case 49:  // lfsu: lfs + ra = EA
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);

    // Constant-address MEM1 fast path + constant propagation
    if (cache.is_const[inst.RA])
    {
      const s32 ea = cache.const_val[inst.RA] + simm;
      const u32 uea = static_cast<u32>(ea);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitConstAddrLoadMEM1(code, 4, wasm_addr);
        code.push_back(WasmOp::F32ReinterpretI32);
        code.push_back(WasmOp::F64PromoteF32);
        EmitLocalSet(code, LOCAL_FPR0);
        EmitLocalGet(code, LOCAL_FPR0);
        EmitSetPS0Value(code, cache, inst.FD);
        EmitLocalGet(code, LOCAL_FPR0);
        EmitSetPS1Value(code, cache, inst.FD);
        EmitI32Const(code, ea);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.is_const[inst.RA] = true;
        cache.const_val[inst.RA] = ea;
        return true;
      }
    }

    EmitGetGPRValue(code, cache, inst.RA);
    EmitI32Const(code, simm);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_EA);

    if (inst.RA == 1)
      EmitDirectLoadMEM1(code, 4, m_ram_base, m_ram_mask);
    else
      EmitDirectLoad(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U32);
    code.push_back(WasmOp::F32ReinterpretI32);
    code.push_back(WasmOp::F64PromoteF32);
    EmitLocalSet(code, LOCAL_FPR0);

    EmitLocalGet(code, LOCAL_FPR0);
    EmitSetPS0Value(code, cache, inst.FD);
    EmitLocalGet(code, LOCAL_FPR0);
    EmitSetPS1Value(code, cache, inst.FD);
    EmitLocalGet(code, LOCAL_EA);
    EmitSetGPRValue(code, cache, inst.RA);
    return true;
  }

  case 54:  // stfd: Write_U64(EA, ps[fs].ps0)
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);

    // Constant-address MEM1 fast path
    if (inst.RA != 0 && cache.is_const[inst.RA])
    {
      const u32 uea = static_cast<u32>(cache.const_val[inst.RA] + simm);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitLocalGet(code, LOCAL_STATE);
        EmitI64Load(code, OFF_PS + inst.FS * 16);
        EmitLocalSet(code, LOCAL_CR64);
        EmitConstAddrStoreMEM1_64(code, wasm_addr);
        return true;
      }
    }

    if (inst.RA != 0)
    {
      EmitGetGPRValue(code, cache, inst.RA);
    }
    else
    {
      EmitI32Const(code, 0);
    }
    EmitI32Const(code, simm);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_EA);

    EmitLocalGet(code, LOCAL_STATE);
    EmitI64Load(code, OFF_PS + inst.FS * 16);
    EmitLocalSet(code, LOCAL_CR64);

    const bool stfd_mem1 = (inst.RA == 1);
    if (stfd_mem1)
      EmitDirectStoreMEM1_64(code, m_ram_base, m_ram_mask);
    else
      EmitDirectStore64(code, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask);
    return true;
  }

  case 55:  // stfdu: stfd + ra = EA
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);

    // Constant-address MEM1 fast path + constant propagation
    if (cache.is_const[inst.RA])
    {
      const s32 ea = cache.const_val[inst.RA] + simm;
      const u32 uea = static_cast<u32>(ea);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitLocalGet(code, LOCAL_STATE);
        EmitI64Load(code, OFF_PS + inst.FS * 16);
        EmitLocalSet(code, LOCAL_CR64);
        EmitConstAddrStoreMEM1_64(code, wasm_addr);
        EmitI32Const(code, ea);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.is_const[inst.RA] = true;
        cache.const_val[inst.RA] = ea;
        return true;
      }
    }

    EmitGetGPRValue(code, cache, inst.RA);
    EmitI32Const(code, simm);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_EA);

    EmitLocalGet(code, LOCAL_STATE);
    EmitI64Load(code, OFF_PS + inst.FS * 16);
    EmitLocalSet(code, LOCAL_CR64);

    const bool stfdu_mem1 = (inst.RA == 1);
    if (stfdu_mem1)
      EmitDirectStoreMEM1_64(code, m_ram_base, m_ram_mask);
    else
      EmitDirectStore64(code, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask);
    EmitLocalGet(code, LOCAL_EA);
    EmitSetGPRValue(code, cache, inst.RA);
    return true;
  }

  case 52:  // stfs: Write_U32(EA, ConvertToSingle(ps[fs].ps0))
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);

    // Constant-address MEM1 fast path
    if (inst.RA != 0 && cache.is_const[inst.RA])
    {
      const u32 uea = static_cast<u32>(cache.const_val[inst.RA] + simm);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitLoadPS0(code, cache, inst.FS);
        code.push_back(WasmOp::F32DemoteF64);
        code.push_back(WasmOp::I32ReinterpretF32);
        EmitConstAddrStoreMEM1(code, 4, wasm_addr);
        return true;
      }
    }

    if (inst.RA != 0)
    {
      EmitGetGPRValue(code, cache, inst.RA);
    }
    else
    {
      EmitI32Const(code, 0);
    }
    EmitI32Const(code, simm);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_EA);

    EmitLoadPS0(code, cache, inst.FS);
    code.push_back(WasmOp::F32DemoteF64);
    code.push_back(WasmOp::I32ReinterpretF32);
    EmitLocalSet(code, LOCAL_TMP1);

    bool ea_is_mem1 = (inst.RA == 1);
    if (ea_is_mem1)
      EmitDirectStoreMEM1(code, 4, m_ram_base, m_ram_mask);
    else
      EmitDirectStore(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U32);
    return true;
  }

  case 53:  // stfsu: stfs + ra = EA
  {
    const s32 simm = static_cast<s32>(inst.SIMM_16);

    // Constant-address MEM1 fast path + constant propagation
    if (cache.is_const[inst.RA])
    {
      const s32 ea = cache.const_val[inst.RA] + simm;
      const u32 uea = static_cast<u32>(ea);
      if ((uea & 0x80000000) && !(uea & 0x10000000))
      {
        const u32 wasm_addr = m_ram_base + (uea & m_ram_mask);
        EmitLoadPS0(code, cache, inst.FS);
        code.push_back(WasmOp::F32DemoteF64);
        code.push_back(WasmOp::I32ReinterpretF32);
        EmitConstAddrStoreMEM1(code, 4, wasm_addr);
        EmitI32Const(code, ea);
        EmitSetGPRValue(code, cache, inst.RA);
        cache.is_const[inst.RA] = true;
        cache.const_val[inst.RA] = ea;
        return true;
      }
    }

    EmitGetGPRValue(code, cache, inst.RA);
    EmitI32Const(code, simm);
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_EA);

    EmitLoadPS0(code, cache, inst.FS);
    code.push_back(WasmOp::F32DemoteF64);
    code.push_back(WasmOp::I32ReinterpretF32);
    EmitLocalSet(code, LOCAL_TMP1);

    if (inst.RA == 1)
      EmitDirectStoreMEM1(code, 4, m_ram_base, m_ram_mask);
    else
      EmitDirectStore(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U32);
    EmitLocalGet(code, LOCAL_EA);
    EmitSetGPRValue(code, cache, inst.RA);
    return true;
  }

  // =====================================================================
  // Opcode 63: Double-precision FPU
  // =====================================================================
  case 63:
  {
    const u32 subop10 = inst.SUBOP10;
    switch (subop10)
    {
    case 72:  // fmr: ps0[fd] = ps0[fb]
      EmitLoadPS0(code, cache, inst.FB);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 264:  // fabs: ps0[fd] = |ps0[fb]|
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Abs);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 40:  // fneg: ps0[fd] = -ps0[fb]
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Neg);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 136:  // fnabs: ps0[fd] = -|ps0[fb]|
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Abs);
      code.push_back(WasmOp::F64Neg);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 0:  // fcmpu: compare ps0[fa] vs ps0[fb], update CR[crfd]
    {
      EmitLoadPS0(code, cache, inst.FA);
      EmitLocalSet(code, LOCAL_FPR0);
      EmitLoadPS0(code, cache, inst.FB);
      EmitLocalSet(code, LOCAL_FPR1);
      EmitFPCRCompare(code, cache, inst.CRFD);
      return true;
    }

    case 32:  // fcmpo: same as fcmpu but raises exception on QNaN
    {
      // Skip FPSCR exception handling; just do the comparison
      EmitLoadPS0(code, cache, inst.FA);
      EmitLocalSet(code, LOCAL_FPR0);
      EmitLoadPS0(code, cache, inst.FB);
      EmitLocalSet(code, LOCAL_FPR1);
      EmitFPCRCompare(code, cache, inst.CRFD);
      return true;
    }

    case 12:  // frsp: ps0[fd] = (float)ps0[fb] (round to single precision)
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 15:  // fctiwz: convert ps0[fb] to integer (truncate), store as i64 bits in ps0[fd]
    {
      // Use i32.trunc_sat_f64_s (saturating): handles NaN→0, overflow→INT_MIN/MAX
      EmitLocalGet(code, LOCAL_STATE);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(0xFC);  // prefix for saturating trunc
      EncodeU32(code, 0x02); // i32.trunc_sat_f64_s
      code.push_back(WasmOp::I64ExtendI32S);
      EmitI64Store(code, OFF_PS + inst.FD * 16);
      InvalidateFPRDirect(cache, inst.FD);  // stored raw bits, not a normal f64
      return true;
    }

    case 583:  // mffs: fd.ps0 = NaN-box(fpscr)
    {
      // ps0[fd] = reinterpret_f64(0xFFF8000000000000 | fpscr.Hex)
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load(code, OFF_FPSCR);
      code.push_back(WasmOp::I64ExtendI32U);
      EmitI64Const(code, static_cast<s64>(0xFFF8000000000000ULL));
      code.push_back(WasmOp::I64Or);
      code.push_back(WasmOp::F64ReinterpretI64);
      EmitSetPS0Value(code, cache, inst.FD);
      // Ignore Rc bit (CR1 update) — extremely rare
      return true;
    }

    default:
      break;  // fall through to SUBOP5 check
    }

    // Check SUBOP5 for arithmetic FPU instructions
    const u32 subop5 = (inst.hex >> 1) & 0x1f;
    switch (subop5)
    {
    case 21:  // fadd: ps0[fd] = ps0[fa] + ps0[fb]
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Add);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 20:  // fsub: ps0[fd] = ps0[fa] - ps0[fb]
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Sub);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 25:  // fmul: ps0[fd] = ps0[fa] * ps0[fc]
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FC);
      code.push_back(WasmOp::F64Mul);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 18:  // fdiv: ps0[fd] = ps0[fa] / ps0[fb]
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Div);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 29:  // fmadd: ps0[fd] = ps0[fa] * ps0[fc] + ps0[fb]
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FC);
      code.push_back(WasmOp::F64Mul);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Add);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 28:  // fmsub: ps0[fd] = ps0[fa] * ps0[fc] - ps0[fb]
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FC);
      code.push_back(WasmOp::F64Mul);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Sub);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 31:  // fnmadd: ps0[fd] = -(ps0[fa] * ps0[fc] + ps0[fb])
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FC);
      code.push_back(WasmOp::F64Mul);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Add);
      code.push_back(WasmOp::F64Neg);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 30:  // fnmsub: ps0[fd] = -(ps0[fa] * ps0[fc] - ps0[fb])
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FC);
      code.push_back(WasmOp::F64Mul);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Sub);
      code.push_back(WasmOp::F64Neg);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 26:  // frsqrte: ps0[fd] = 1.0 / sqrt(ps0[fb])
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Sqrt);
      EmitLocalSet(code, LOCAL_FPR0);
      EmitI64Const(code, static_cast<s64>(0x3FF0000000000000ULL));  // 1.0 in IEEE 754
      code.push_back(WasmOp::F64ReinterpretI64);
      EmitLocalGet(code, LOCAL_FPR0);
      code.push_back(WasmOp::F64Div);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 24:  // fres: ps0[fd] = 1.0 / ps0[fb]
      EmitI64Const(code, static_cast<s64>(0x3FF0000000000000ULL));
      code.push_back(WasmOp::F64ReinterpretI64);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Div);
      EmitSetPS0Value(code, cache, inst.FD);
      return true;

    case 23:  // fsel: fd = (fa >= 0.0) ? fc : fb
    {
      EmitLoadPS0(code, cache, inst.FA);
      EmitI64Const(code, 0);  // 0.0
      code.push_back(WasmOp::F64ReinterpretI64);
      code.push_back(WasmOp::F64Lt);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      // fa < 0: fd = fb
      EmitLoadPS0(code, cache, inst.FB);
      EmitSetPS0Value(code, cache, inst.FD);
      code.push_back(WasmOp::Else);
      // fa >= 0: fd = fc
      EmitLoadPS0(code, cache, inst.FC);
      EmitSetPS0Value(code, cache, inst.FD);
      code.push_back(WasmOp::End);
      return true;
    }

    default:
      return false;
    }
  }

  // =====================================================================
  // Opcode 59: Single-precision FPU (results rounded to single, stored to both ps0/ps1)
  // =====================================================================
  case 59:
  {
    const u32 subop5 = (inst.hex >> 1) & 0x1f;

    // Emit the operation, then round to single and store to both ps0 and ps1
    switch (subop5)
    {
    case 21:  // fadds
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Add);
      break;
    case 20:  // fsubs
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Sub);
      break;
    case 25:  // fmuls
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FC);
      code.push_back(WasmOp::F64Mul);
      break;
    case 18:  // fdivs
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Div);
      break;
    case 29:  // fmadds
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FC);
      code.push_back(WasmOp::F64Mul);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Add);
      break;
    case 28:  // fmsubs
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FC);
      code.push_back(WasmOp::F64Mul);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Sub);
      break;
    case 31:  // fnmadds
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FC);
      code.push_back(WasmOp::F64Mul);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Add);
      code.push_back(WasmOp::F64Neg);
      break;
    case 30:  // fnmsubs
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS0(code, cache, inst.FC);
      code.push_back(WasmOp::F64Mul);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Sub);
      code.push_back(WasmOp::F64Neg);
      break;
    case 24:  // fres (single)
      EmitI64Const(code, static_cast<s64>(0x3FF0000000000000ULL));
      code.push_back(WasmOp::F64ReinterpretI64);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Div);
      break;
    default:
      return false;
    }

    // Round to single precision
    code.push_back(WasmOp::F32DemoteF64);
    code.push_back(WasmOp::F64PromoteF32);
    EmitLocalSet(code, LOCAL_FPR0);

    // Store to both ps0 and ps1
    EmitLocalGet(code, LOCAL_FPR0);
    EmitSetPS0Value(code, cache, inst.FD);
    EmitLocalGet(code, LOCAL_FPR0);
    EmitSetPS1Value(code, cache, inst.FD);
    return true;
  }

  // =====================================================================
  // Opcode 4: Paired Singles
  // =====================================================================
  case 4:
  {
    // Check SUBOP6 first for psq_lx/psq_stx (bits 30:25)
    const u32 subop6 = inst.SUBOP6;
    if (subop6 == 6 || subop6 == 38)  // psq_lx (6) / psq_lux (38)
    {
      const u32 gqr_idx = inst.Ix;
      const bool w_bit = inst.Wx != 0;
      const bool update = (subop6 == 38);

      if (update && inst.RA == 0)
        return false;

      // EA = (RA ? gpr[RA] : 0) + gpr[RB]
      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      // Check GQR load type
      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load(code, OFF_SPR + (912 + gqr_idx) * 4);
      EmitI32Const(code, 16);
      code.push_back(WasmOp::I32ShrU);
      EmitI32Const(code, 7);
      code.push_back(WasmOp::I32And);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      {
        EmitLocalGet(code, 0);
        EmitI32Const(code, static_cast<s32>(op.address));
        EmitI32Store(code, OFF_PC);
        EmitLocalGet(code, 0);
        EmitI32Const(code, static_cast<s32>(op.address + 4));
        EmitI32Store(code, OFF_NPC);
        EmitLocalGet(code, 0);
        EmitI32Const(code, static_cast<s32>(op.address));
        EmitI32Const(code, static_cast<s32>(op.inst.hex));
        EmitCall(code, FUNC_FALLBACK);
      }
      code.push_back(WasmOp::Else);
      {
        const bool psqlx_mem1 = (inst.RA == 1);
        if (psqlx_mem1)
          EmitDirectLoadMEM1(code, 4, m_ram_base, m_ram_mask);
        else
          EmitDirectLoad(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U32);
        code.push_back(WasmOp::F32ReinterpretI32);
        code.push_back(WasmOp::F64PromoteF32);
        EmitLocalSet(code, LOCAL_FPR0);

        if (!w_bit)
        {
          EmitLocalGet(code, LOCAL_EA);
          EmitI32Const(code, 4);
          code.push_back(WasmOp::I32Add);
          EmitLocalSet(code, LOCAL_EA);
          if (psqlx_mem1)
            EmitDirectLoadMEM1(code, 4, m_ram_base, m_ram_mask);
          else
            EmitDirectLoad(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U32);
          code.push_back(WasmOp::F32ReinterpretI32);
          code.push_back(WasmOp::F64PromoteF32);
          EmitLocalSet(code, LOCAL_FPR1);
        }

        EmitLocalGet(code, LOCAL_FPR0);
        EmitSetPS0Value(code, cache, inst.RD);

        if (w_bit)
        {
          EmitI64Const(code, static_cast<s64>(0x3FF0000000000000ULL));
          code.push_back(WasmOp::F64ReinterpretI64);
          EmitSetPS1Value(code, cache, inst.RD);
        }
        else
        {
          EmitLocalGet(code, LOCAL_FPR1);
          EmitSetPS1Value(code, cache, inst.RD);
        }
      }
      code.push_back(WasmOp::End);

      if (update)
      {
        if (!w_bit)
        {
          EmitLocalGet(code, LOCAL_EA);
          EmitI32Const(code, 4);
          code.push_back(WasmOp::I32Sub);
          EmitLocalSet(code, LOCAL_EA);
        }
        EmitLocalGet(code, LOCAL_EA);
        EmitSetGPRValue(code, cache, inst.RA);
      }
      return true;
    }
    if (subop6 == 7 || subop6 == 39)  // psq_stx (7) / psq_stux (39)
    {
      const u32 gqr_idx = inst.Ix;
      const bool w_bit = inst.Wx != 0;
      const bool update = (subop6 == 39);

      if (update && inst.RA == 0)
        return false;

      if (inst.RA != 0)
      {
        EmitGetGPRValue(code, cache, inst.RA);
      }
      else
      {
        EmitI32Const(code, 0);
      }
      EmitGetGPRValue(code, cache, inst.RB);
      code.push_back(WasmOp::I32Add);
      EmitLocalSet(code, LOCAL_EA);

      EmitLocalGet(code, LOCAL_STATE);
      EmitI32Load(code, OFF_SPR + (912 + gqr_idx) * 4);
      EmitI32Const(code, 7);
      code.push_back(WasmOp::I32And);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      {
        EmitLocalGet(code, 0);
        EmitI32Const(code, static_cast<s32>(op.address));
        EmitI32Store(code, OFF_PC);
        EmitLocalGet(code, 0);
        EmitI32Const(code, static_cast<s32>(op.address + 4));
        EmitI32Store(code, OFF_NPC);
        EmitLocalGet(code, 0);
        EmitI32Const(code, static_cast<s32>(op.address));
        EmitI32Const(code, static_cast<s32>(op.inst.hex));
        EmitCall(code, FUNC_FALLBACK);
      }
      code.push_back(WasmOp::Else);
      {
        const bool psqsx_mem1 = (inst.RA == 1);
        EmitLoadPS0(code, cache, inst.RS);
        code.push_back(WasmOp::F32DemoteF64);
        code.push_back(WasmOp::I32ReinterpretF32);
        EmitLocalSet(code, LOCAL_TMP1);
        if (psqsx_mem1)
          EmitDirectStoreMEM1(code, 4, m_ram_base, m_ram_mask);
        else
          EmitDirectStore(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U32);

        if (!w_bit)
        {
          EmitLocalGet(code, LOCAL_EA);
          EmitI32Const(code, 4);
          code.push_back(WasmOp::I32Add);
          EmitLocalSet(code, LOCAL_EA);
          EmitLoadPS1(code, cache, inst.RS);
          code.push_back(WasmOp::F32DemoteF64);
          code.push_back(WasmOp::I32ReinterpretF32);
          EmitLocalSet(code, LOCAL_TMP1);
          if (psqsx_mem1)
            EmitDirectStoreMEM1(code, 4, m_ram_base, m_ram_mask);
          else
            EmitDirectStore(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U32);
        }
      }
      code.push_back(WasmOp::End);

      if (update)
      {
        if (!w_bit)
        {
          EmitLocalGet(code, LOCAL_EA);
          EmitI32Const(code, 4);
          code.push_back(WasmOp::I32Sub);
          EmitLocalSet(code, LOCAL_EA);
        }
        EmitLocalGet(code, LOCAL_EA);
        EmitSetGPRValue(code, cache, inst.RA);
      }
      return true;
    }

    const u32 subop10 = inst.SUBOP10;
    switch (subop10)
    {
    case 72:  // ps_mr: fd = fb (copy full v128)
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;

    case 40:  // ps_neg: fd = -fb (SIMD f64x2.neg)
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, WasmOp::F64x2Neg);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;

    case 264:  // ps_abs: fd = |fb| (SIMD f64x2.abs)
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, WasmOp::F64x2Abs);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;

    case 136:  // ps_nabs: fd = -|fb| (SIMD f64x2.abs + f64x2.neg)
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, WasmOp::F64x2Abs);
      EmitSIMDOp(code, WasmOp::F64x2Neg);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;

    case 528:  // ps_merge00: fd.ps0 = fa.ps0, fd.ps1 = fb.ps0
    {
      // i8x16.shuffle: take bytes 0-7 from fa (ps0), bytes 0-7 from fb (ps0 → lane 1)
      EmitLoadFPRv128(code, cache, inst.FA);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, 0x0d);  // i8x16.shuffle
      // Indices: fa[0..7], fb[0..7] (fb is at offset 16 in the concatenated view)
      const u8 shuffle00[] = {0,1,2,3,4,5,6,7, 16,17,18,19,20,21,22,23};
      code.insert(code.end(), shuffle00, shuffle00 + 16);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;
    }

    case 560:  // ps_merge01: fd.ps0 = fa.ps0, fd.ps1 = fb.ps1
    {
      EmitLoadFPRv128(code, cache, inst.FA);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, 0x0d);  // i8x16.shuffle
      // fa[0..7] (ps0), fb[8..15] (ps1, at offset 16+8=24)
      const u8 shuffle01[] = {0,1,2,3,4,5,6,7, 24,25,26,27,28,29,30,31};
      code.insert(code.end(), shuffle01, shuffle01 + 16);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;
    }

    case 592:  // ps_merge10: fd.ps0 = fa.ps1, fd.ps1 = fb.ps0
    {
      EmitLoadFPRv128(code, cache, inst.FA);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, 0x0d);  // i8x16.shuffle
      // fa[8..15] (ps1), fb[0..7] (ps0, at offset 16)
      const u8 shuffle10[] = {8,9,10,11,12,13,14,15, 16,17,18,19,20,21,22,23};
      code.insert(code.end(), shuffle10, shuffle10 + 16);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;
    }

    case 624:  // ps_merge11: fd.ps0 = fa.ps1, fd.ps1 = fb.ps1
    {
      EmitLoadFPRv128(code, cache, inst.FA);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, 0x0d);  // i8x16.shuffle
      // fa[8..15] (ps1), fb[8..15] (ps1, at offset 16+8=24)
      const u8 shuffle11[] = {8,9,10,11,12,13,14,15, 24,25,26,27,28,29,30,31};
      code.insert(code.end(), shuffle11, shuffle11 + 16);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;
    }

    default:
      break;  // fall through to SUBOP5 check
    }

    // Check SUBOP5 for PS arithmetic
    const u32 subop5 = (inst.hex >> 1) & 0x1f;

    // PS arithmetic: operate on both ps0 and ps1, round to single
    switch (subop5)
    {
    // --- SIMD v128 paired-single arithmetic (symmetric: both lanes identical) ---
    case 21:  // ps_add: fd = fa + fb (SIMD)
      EmitLoadFPRv128(code, cache, inst.FA);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, WasmOp::F64x2Add);
      EmitF64x2RoundToF32(code);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;

    case 20:  // ps_sub: fd = fa - fb (SIMD)
      EmitLoadFPRv128(code, cache, inst.FA);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, WasmOp::F64x2Sub);
      EmitF64x2RoundToF32(code);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;

    case 25:  // ps_mul: fd = fa * fc (SIMD)
      EmitLoadFPRv128(code, cache, inst.FA);
      EmitLoadFPRv128(code, cache, inst.FC);
      EmitSIMDOp(code, WasmOp::F64x2Mul);
      EmitF64x2RoundToF32(code);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;

    case 18:  // ps_div: fd = fa / fb (SIMD)
      EmitLoadFPRv128(code, cache, inst.FA);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, WasmOp::F64x2Div);
      EmitF64x2RoundToF32(code);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;

    case 29:  // ps_madd: fd = fa * fc + fb (SIMD)
      EmitLoadFPRv128(code, cache, inst.FA);
      EmitLoadFPRv128(code, cache, inst.FC);
      EmitSIMDOp(code, WasmOp::F64x2Mul);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, WasmOp::F64x2Add);
      EmitF64x2RoundToF32(code);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;

    case 28:  // ps_msub: fd = fa * fc - fb (SIMD)
      EmitLoadFPRv128(code, cache, inst.FA);
      EmitLoadFPRv128(code, cache, inst.FC);
      EmitSIMDOp(code, WasmOp::F64x2Mul);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, WasmOp::F64x2Sub);
      EmitF64x2RoundToF32(code);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;

    case 31:  // ps_nmadd: fd = -(fa * fc + fb) (SIMD)
      EmitLoadFPRv128(code, cache, inst.FA);
      EmitLoadFPRv128(code, cache, inst.FC);
      EmitSIMDOp(code, WasmOp::F64x2Mul);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, WasmOp::F64x2Add);
      EmitSIMDOp(code, WasmOp::F64x2Neg);
      EmitF64x2RoundToF32(code);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;

    case 30:  // ps_nmsub: fd = -(fa * fc - fb) (SIMD)
      EmitLoadFPRv128(code, cache, inst.FA);
      EmitLoadFPRv128(code, cache, inst.FC);
      EmitSIMDOp(code, WasmOp::F64x2Mul);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, WasmOp::F64x2Sub);
      EmitSIMDOp(code, WasmOp::F64x2Neg);
      EmitF64x2RoundToF32(code);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;

    case 24:  // ps_res: fd = 1.0 / fb (SIMD: splat 1.0, div)
    {
      // Build v128 of [1.0, 1.0] via f64x2.splat
      EmitI64Const(code, static_cast<s64>(0x3FF0000000000000ULL));
      code.push_back(WasmOp::F64ReinterpretI64);
      EmitSIMDOp(code, WasmOp::F64x2Splat);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, WasmOp::F64x2Div);
      EmitF64x2RoundToF32(code);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;
    }

    case 26:  // ps_rsqrte: fd = 1.0 / sqrt(fb) (SIMD)
    {
      EmitI64Const(code, static_cast<s64>(0x3FF0000000000000ULL));
      code.push_back(WasmOp::F64ReinterpretI64);
      EmitSIMDOp(code, WasmOp::F64x2Splat);
      EmitLoadFPRv128(code, cache, inst.FB);
      EmitSIMDOp(code, WasmOp::F64x2Sqrt);
      EmitSIMDOp(code, WasmOp::F64x2Div);
      EmitF64x2RoundToF32(code);
      EmitSetFPRv128(code, cache, inst.FD);
      return true;
    }

    case 10:  // ps_sum0: fd.ps0 = fa.ps0 + fb.ps1, fd.ps1 = fc.ps1
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS1(code, cache, inst.FB);
      code.push_back(WasmOp::F64Add);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR0);
      EmitLoadPS1(code, cache, inst.FC);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR1);
      break;

    case 11:  // ps_sum1: fd.ps0 = fc.ps0, fd.ps1 = fa.ps0 + fb.ps1
      EmitLoadPS0(code, cache, inst.FC);  // read fc.ps0 first (before fd write clobbers it)
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR0);
      EmitLoadPS0(code, cache, inst.FA);
      EmitLoadPS1(code, cache, inst.FB);
      code.push_back(WasmOp::F64Add);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR1);
      break;

    case 12:  // ps_muls0: fd.ps0 = fa.ps0 * fc.ps0, fd.ps1 = fa.ps1 * fc.ps0
    {
      EmitLoadPS0(code, cache, inst.FC);
      EmitLocalSet(code, LOCAL_FPR1);  // cache fc.ps0
      EmitLoadPS0(code, cache, inst.FA);
      EmitLocalGet(code, LOCAL_FPR1);
      code.push_back(WasmOp::F64Mul);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR0);
      EmitLoadPS1(code, cache, inst.FA);
      EmitLocalGet(code, LOCAL_FPR1);
      code.push_back(WasmOp::F64Mul);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR1);
      break;
    }

    case 13:  // ps_muls1: fd.ps0 = fa.ps0 * fc.ps1, fd.ps1 = fa.ps1 * fc.ps1
    {
      EmitLoadPS1(code, cache, inst.FC);
      EmitLocalSet(code, LOCAL_FPR1);  // cache fc.ps1
      EmitLoadPS0(code, cache, inst.FA);
      EmitLocalGet(code, LOCAL_FPR1);
      code.push_back(WasmOp::F64Mul);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR0);
      EmitLoadPS1(code, cache, inst.FA);
      EmitLocalGet(code, LOCAL_FPR1);
      code.push_back(WasmOp::F64Mul);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR1);
      break;
    }

    case 14:  // ps_madds0: fd = fa * fc.ps0 + fb (both slots)
    {
      EmitLoadPS0(code, cache, inst.FC);
      EmitLocalSet(code, LOCAL_FPR1);  // cache fc.ps0
      EmitLoadPS0(code, cache, inst.FA);
      EmitLocalGet(code, LOCAL_FPR1);
      code.push_back(WasmOp::F64Mul);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Add);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR0);
      EmitLoadPS1(code, cache, inst.FA);
      EmitLocalGet(code, LOCAL_FPR1);
      code.push_back(WasmOp::F64Mul);
      EmitLoadPS1(code, cache, inst.FB);
      code.push_back(WasmOp::F64Add);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR1);
      break;
    }

    case 15:  // ps_madds1: fd = fa * fc.ps1 + fb (both slots)
    {
      EmitLoadPS1(code, cache, inst.FC);
      EmitLocalSet(code, LOCAL_FPR1);  // cache fc.ps1
      EmitLoadPS0(code, cache, inst.FA);
      EmitLocalGet(code, LOCAL_FPR1);
      code.push_back(WasmOp::F64Mul);
      EmitLoadPS0(code, cache, inst.FB);
      code.push_back(WasmOp::F64Add);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR0);
      EmitLoadPS1(code, cache, inst.FA);
      EmitLocalGet(code, LOCAL_FPR1);
      code.push_back(WasmOp::F64Mul);
      EmitLoadPS1(code, cache, inst.FB);
      code.push_back(WasmOp::F64Add);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR1);
      break;
    }

    case 23:  // ps_sel: fd = (fa >= 0) ? fc : fb (both slots)
    {
      // ps0: if fa.ps0 >= 0.0 then fc.ps0 else fb.ps0
      EmitLoadPS0(code, cache, inst.FA);
      EmitI64Const(code, 0);
      code.push_back(WasmOp::F64ReinterpretI64);
      code.push_back(WasmOp::F64Lt);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitLoadPS0(code, cache, inst.FB);
      EmitLocalSet(code, LOCAL_FPR0);
      code.push_back(WasmOp::Else);
      EmitLoadPS0(code, cache, inst.FC);
      EmitLocalSet(code, LOCAL_FPR0);
      code.push_back(WasmOp::End);
      // ps1: if fa.ps1 >= 0.0 then fc.ps1 else fb.ps1
      EmitLoadPS1(code, cache, inst.FA);
      EmitI64Const(code, 0);
      code.push_back(WasmOp::F64ReinterpretI64);
      code.push_back(WasmOp::F64Lt);
      code.push_back(WasmOp::If);
      code.push_back(WasmOp::Void);
      EmitLoadPS1(code, cache, inst.FB);
      EmitLocalSet(code, LOCAL_FPR1);
      code.push_back(WasmOp::Else);
      EmitLoadPS1(code, cache, inst.FC);
      EmitLocalSet(code, LOCAL_FPR1);
      code.push_back(WasmOp::End);
      break;
    }

    default:
      return false;
    }

    // Store both results (cache-direct, no state_ptr needed)
    EmitLocalGet(code, LOCAL_FPR0);
    EmitSetPS0Value(code, cache, inst.FD);
    EmitLocalGet(code, LOCAL_FPR1);
    EmitSetPS1Value(code, cache, inst.FD);
    return true;
  }

  // =====================================================================
  // Paired Single Quantized Load/Store (psq_l, psq_lu, psq_st, psq_stu)
  // =====================================================================
  case 56:  // psq_l: quantized PS load
  case 57:  // psq_lu: quantized PS load with update
  {
    const u32 gqr_idx = inst.I;
    const bool w_bit = inst.W != 0;
    const bool update = (opcode == 57);

    if (update && inst.RA == 0)
      return false;

    // Compute EA
    if (inst.RA != 0)
    {
      EmitGetGPRValue(code, cache, inst.RA);
    }
    else
    {
      EmitI32Const(code, 0);
    }
    EmitI32Const(code, static_cast<s32>(inst.SIMM_12));
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_EA);

    // Check GQR load type: if type != 0 (QUANTIZE_FLOAT), use fallback
    EmitLocalGet(code, LOCAL_STATE);
    EmitI32Load(code, OFF_SPR + (912 + gqr_idx) * 4);
    EmitI32Const(code, 16);
    code.push_back(WasmOp::I32ShrU);
    EmitI32Const(code, 7);
    code.push_back(WasmOp::I32And);
    code.push_back(WasmOp::If);
    code.push_back(WasmOp::Void);
    {
      // Non-float quantization type — use interpreter fallback
      EmitLocalGet(code, 0);
      EmitI32Const(code, static_cast<s32>(op.address));
      EmitI32Store(code, OFF_PC);
      EmitLocalGet(code, 0);
      EmitI32Const(code, static_cast<s32>(op.address + 4));
      EmitI32Store(code, OFF_NPC);
      EmitLocalGet(code, 0);
      EmitI32Const(code, static_cast<s32>(op.address));
      EmitI32Const(code, static_cast<s32>(op.inst.hex));
      EmitCall(code, FUNC_FALLBACK);
    }
    code.push_back(WasmOp::Else);
    {
      // QUANTIZE_FLOAT fast path: load f32(s), promote to f64, store to PS
      // Load ps0: read f32 (big-endian) from EA
      const bool psql_mem1 = (inst.RA == 1);
      if (psql_mem1)
        EmitDirectLoadMEM1(code, 4, m_ram_base, m_ram_mask);
      else
        EmitDirectLoad(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U32);
      code.push_back(WasmOp::F32ReinterpretI32);
      code.push_back(WasmOp::F64PromoteF32);
      EmitLocalSet(code, LOCAL_FPR0);

      if (!w_bit)
      {
        // Paired mode: also load ps1 from EA+4
        EmitLocalGet(code, LOCAL_EA);
        EmitI32Const(code, 4);
        code.push_back(WasmOp::I32Add);
        EmitLocalSet(code, LOCAL_EA);
        if (psql_mem1)
          EmitDirectLoadMEM1(code, 4, m_ram_base, m_ram_mask);
        else
          EmitDirectLoad(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_READ_U32);
        code.push_back(WasmOp::F32ReinterpretI32);
        code.push_back(WasmOp::F64PromoteF32);
        EmitLocalSet(code, LOCAL_FPR1);
      }

      EmitLocalGet(code, LOCAL_FPR0);
      EmitSetPS0Value(code, cache, inst.RD);

      if (w_bit)
      {
        // Scalar mode: ps1 = 1.0
        EmitI64Const(code, static_cast<s64>(0x3FF0000000000000ULL));
        code.push_back(WasmOp::F64ReinterpretI64);
        EmitSetPS1Value(code, cache, inst.RD);
      }
      else
      {
        EmitLocalGet(code, LOCAL_FPR1);
        EmitSetPS1Value(code, cache, inst.RD);
      }
    }
    code.push_back(WasmOp::End);

    if (update)
    {
      // Restore EA (it may have been modified for ps1 load)
      if (!w_bit)
      {
        EmitLocalGet(code, LOCAL_EA);
        EmitI32Const(code, 4);
        code.push_back(WasmOp::I32Sub);
        EmitLocalSet(code, LOCAL_EA);
      }
      EmitLocalGet(code, LOCAL_EA);
      EmitSetGPRValue(code, cache, inst.RA);
    }
    return true;
  }

  case 60:  // psq_st: quantized PS store
  case 61:  // psq_stu: quantized PS store with update
  {
    const u32 gqr_idx = inst.I;
    const bool w_bit = inst.W != 0;
    const bool update = (opcode == 61);

    if (update && inst.RA == 0)
      return false;

    // Compute EA
    if (inst.RA != 0)
    {
      EmitGetGPRValue(code, cache, inst.RA);
    }
    else
    {
      EmitI32Const(code, 0);
    }
    EmitI32Const(code, static_cast<s32>(inst.SIMM_12));
    code.push_back(WasmOp::I32Add);
    EmitLocalSet(code, LOCAL_EA);

    // Check GQR store type: if type != 0, use fallback
    EmitLocalGet(code, LOCAL_STATE);
    EmitI32Load(code, OFF_SPR + (912 + gqr_idx) * 4);
    EmitI32Const(code, 7);
    code.push_back(WasmOp::I32And);
    code.push_back(WasmOp::If);
    code.push_back(WasmOp::Void);
    {
      // Non-float: fallback
      EmitLocalGet(code, 0);
      EmitI32Const(code, static_cast<s32>(op.address));
      EmitI32Store(code, OFF_PC);
      EmitLocalGet(code, 0);
      EmitI32Const(code, static_cast<s32>(op.address + 4));
      EmitI32Store(code, OFF_NPC);
      EmitLocalGet(code, 0);
      EmitI32Const(code, static_cast<s32>(op.address));
      EmitI32Const(code, static_cast<s32>(op.inst.hex));
      EmitCall(code, FUNC_FALLBACK);
    }
    code.push_back(WasmOp::Else);
    {
      // QUANTIZE_FLOAT fast path: demote f64 to f32, store as big-endian
      // Store ps0
      const bool psqs_mem1 = (inst.RA == 1);
      EmitLoadPS0(code, cache, inst.RS);
      code.push_back(WasmOp::F32DemoteF64);
      code.push_back(WasmOp::I32ReinterpretF32);
      EmitLocalSet(code, LOCAL_TMP1);
      if (psqs_mem1)
        EmitDirectStoreMEM1(code, 4, m_ram_base, m_ram_mask);
      else
        EmitDirectStore(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U32);

      if (!w_bit)
      {
        // Paired mode: also store ps1 at EA+4
        EmitLocalGet(code, LOCAL_EA);
        EmitI32Const(code, 4);
        code.push_back(WasmOp::I32Add);
        EmitLocalSet(code, LOCAL_EA);
        EmitLoadPS1(code, cache, inst.RS);
        code.push_back(WasmOp::F32DemoteF64);
        code.push_back(WasmOp::I32ReinterpretF32);
        EmitLocalSet(code, LOCAL_TMP1);
        if (psqs_mem1)
          EmitDirectStoreMEM1(code, 4, m_ram_base, m_ram_mask);
        else
          EmitDirectStore(code, 4, m_ram_base, m_exram_base, m_ram_mask, m_exram_mask, FUNC_WRITE_U32);
      }
    }
    code.push_back(WasmOp::End);

    if (update)
    {
      if (!w_bit)
      {
        EmitLocalGet(code, LOCAL_EA);
        EmitI32Const(code, 4);
        code.push_back(WasmOp::I32Sub);
        EmitLocalSet(code, LOCAL_EA);
      }
      EmitLocalGet(code, LOCAL_EA);
      EmitSetGPRValue(code, cache, inst.RA);
    }
    return true;
  }

  default:
    return false;
  }
}

void AotWasm::EmitWasmFallback(std::vector<u8>& code, u32 pc, u32 inst_hex)
{
  // Set pc and npc before calling fallback
  EmitLocalGet(code, 0);
  EmitI32Const(code, static_cast<s32>(pc));
  EmitI32Store(code, OFF_PC);

  EmitLocalGet(code, 0);
  EmitI32Const(code, static_cast<s32>(pc + 4));
  EmitI32Store(code, OFF_NPC);

  // Call fallback(state_ptr, pc, inst_hex)
  EmitLocalGet(code, 0);
  EmitI32Const(code, static_cast<s32>(pc));
  EmitI32Const(code, static_cast<s32>(inst_hex));
  EmitCall(code, FUNC_FALLBACK);
}

void AotWasm::EmitWasmExceptionCheck(std::vector<u8>& code)
{
  EmitLocalGet(code, 0);
  EmitI32Load(code, OFF_EXCEPTIONS);
  EmitI32Const(code, static_cast<s32>(EXCEPT_DSI_PROGRAM));
  code.push_back(WasmOp::I32And);
  code.push_back(WasmOp::If);
  code.push_back(WasmOp::Void);
  EmitI32Const(code, 0);  // return 0 = stop chain
  code.push_back(WasmOp::Return);
  code.push_back(WasmOp::End);
}

void AotWasm::EmitWasmEndBlock(std::vector<u8>& code, u32 downcount)
{
  // NPC is already in LOCAL_NPC (set by branch handler or default).
  // Copy to LOCAL_TMP1 for use by self-loop check and tail-call dispatch.
  EmitLocalGet(code, LOCAL_NPC);
  EmitLocalSet(code, LOCAL_TMP1);  // tmp1 = npc (next PC)

  // downcount -= cycles
  EmitLocalGet(code, 0);
  EmitLocalGet(code, 0);
  EmitI32Load(code, OFF_DOWNCOUNT);
  EmitI32Const(code, static_cast<s32>(downcount));
  code.push_back(WasmOp::I32Sub);
  EmitLocalTee(code, LOCAL_TMP2);  // tmp2 = new downcount
  EmitI32Store(code, OFF_DOWNCOUNT);

  // Check stop condition: downcount <= 0 OR exceptions != 0
  EmitLocalGet(code, LOCAL_TMP2);
  EmitI32Const(code, 0);
  code.push_back(WasmOp::I32LeS);
  EmitLocalGet(code, 0);
  EmitI32Load(code, OFF_EXCEPTIONS);
  code.push_back(WasmOp::I32Or);
  code.push_back(WasmOp::If);
  code.push_back(WasmOp::Void);
  // Only write pc = npc on stop (deferred from normal path to save a store)
  EmitLocalGet(code, 0);
  EmitLocalGet(code, LOCAL_TMP1);
  EmitI32Store(code, OFF_PC);
  EmitI32Const(code, 0);
  code.push_back(WasmOp::Return);
  code.push_back(WasmOp::End);

  // Can continue chaining. Push npc as the return value.
  // pc write is deferred — the chain target or JS dispatcher will set pc when needed.
  EmitLocalGet(code, LOCAL_TMP1);
}

// Emit WASM-native dispatch via return_call_indirect.
// Called after EndBlock + self-loop check. Expects npc in LOCAL_TMP1.
// On dispatch hit: tail-calls next block (never returns).
// On miss: falls through (caller should return npc).
void AotWasm::EmitWasmTailCallDispatch(std::vector<u8>& code)
{
  if (!m_wasm_tail_calls)
    return;

  // hash_addr = dispatch_base + ((npc >> 2) & 0x3FFFF) * 8
  EmitI32Const(code, static_cast<s32>(m_wasm_dispatch_base));
  EmitLocalGet(code, LOCAL_TMP1);
  EmitI32Const(code, 2);
  code.push_back(WasmOp::I32ShrU);
  EmitI32Const(code, 0x3FFFF);
  code.push_back(WasmOp::I32And);
  EmitI32Const(code, 3);
  code.push_back(WasmOp::I32Shl);  // * 8
  code.push_back(WasmOp::I32Add);
  EmitLocalSet(code, LOCAL_TMP2);   // tmp2 = hash_addr

  // Check dispatch table: if memory[hash_addr] == npc
  EmitLocalGet(code, LOCAL_TMP2);
  EmitI32Load(code, 0);            // memory[hash_addr] (stored PC)
  EmitLocalGet(code, LOCAL_TMP1);  // npc
  code.push_back(WasmOp::I32Eq);
  code.push_back(WasmOp::If);
  code.push_back(WasmOp::Void);
  // Hit: tail call to next block via table
  EmitLocalGet(code, LOCAL_STATE);           // state_ptr arg
  EmitLocalGet(code, LOCAL_TMP2);
  EmitI32Load(code, 4);                      // table_idx from dispatch table
  code.push_back(WasmOp::ReturnCallIndirect);
  EncodeU32(code, 0);  // type index 0: (i32) -> (i32)
  EncodeU32(code, 0);  // table index 0
  code.push_back(WasmOp::End);
}

// =====================================================================
// LEB128 decoding helpers (for peephole optimizer)
// =====================================================================
static u32 DecodeU32(const u8* data, size_t& pos)
{
  u32 result = 0;
  u32 shift = 0;
  u8 byte;
  do
  {
    byte = data[pos++];
    result |= static_cast<u32>(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  return result;
}

static s32 DecodeS32(const u8* data, size_t& pos)
{
  s32 result = 0;
  u32 shift = 0;
  u8 byte;
  do
  {
    byte = data[pos++];
    result |= static_cast<s32>(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  // Sign-extend if the sign bit of the last byte is set
  if (shift < 32 && (byte & 0x40))
    result |= -(1 << shift);
  return result;
}

// Return the number of bytes a LEB128 value occupies starting at data[pos].
// Does NOT advance pos.
static size_t LEB128Size(const u8* data, size_t pos)
{
  size_t start = pos;
  while (data[pos] & 0x80)
    pos++;
  pos++;  // include the final byte (high bit clear)
  return pos - start;
}

// Skip the local declarations section at the start of a WASM function body.
// Returns the byte offset where actual code begins.
static size_t SkipLocalDecls(const std::vector<u8>& code)
{
  size_t pos = 0;
  u32 num_groups = DecodeU32(code.data(), pos);
  for (u32 i = 0; i < num_groups; i++)
  {
    DecodeU32(code.data(), pos);  // count
    pos++;                         // type byte
  }
  return pos;
}

// Determine how many bytes to skip for the operand(s) of a given WASM opcode.
// Returns the total operand size in bytes starting at data[operand_pos].
static size_t OpcodeOperandSize(u8 opcode, const u8* data, size_t operand_pos)
{
  switch (opcode)
  {
  // Opcodes followed by one LEB128 operand
  case WasmOp::LocalGet:
  case WasmOp::LocalSet:
  case WasmOp::LocalTee:
  case WasmOp::Br:
  case WasmOp::BrIf:
  case WasmOp::Call:
  case WasmOp::I32Const:
  case WasmOp::I64Const:
    return LEB128Size(data, operand_pos);

  // Block-type instructions: followed by a block type byte
  case WasmOp::Block:
  case WasmOp::Loop:
  case WasmOp::If:
    return 1;  // block type byte (e.g., 0x40 for void)

  // Memory instructions: two LEB128 operands (align, offset)
  case WasmOp::I32Load:
  case WasmOp::I32Store:
  case WasmOp::I32Load8U:
  case WasmOp::I32Load16U:
  case WasmOp::I32Store8:
  case WasmOp::I32Store16:
  case WasmOp::I64Store:
  case WasmOp::I64Load:
  case WasmOp::F64Load:
  case WasmOp::F64Store:
  {
    size_t s1 = LEB128Size(data, operand_pos);
    size_t s2 = LEB128Size(data, operand_pos + s1);
    return s1 + s2;
  }

  // return_call_indirect: two LEB128 operands (type_idx, table_idx)
  case WasmOp::ReturnCallIndirect:
  {
    size_t s1 = LEB128Size(data, operand_pos);
    size_t s2 = LEB128Size(data, operand_pos + s1);
    return s1 + s2;
  }

  // All other opcodes have no operands
  default:
    return 0;
  }
}

// =====================================================================
// Peephole optimizer for WASM bytecode
// =====================================================================
// Processes the function body (after local declarations) in a single
// forward pass, building an optimized output vector, then swaps it in.
//
// Patterns:
//   1. local.set X; local.get X  -> local.tee X
//   2. i32.const 0; i32.add      -> (removed)
//   3. i32.const 0; i32.or       -> (removed)
//   4. i32.const 0; i32.xor      -> (removed)
//   5. i32.const -1; i32.and     -> (removed)
//   6. i32.const C1; i32.const C2; <binop>  -> i32.const (C1 op C2)
//   7. local.get X; drop         -> (removed)
static void PeepholeOptimize(std::vector<u8>& code)
{
  size_t code_start = SkipLocalDecls(code);
  size_t size = code.size();

  // Build new output: copy local declarations verbatim, then optimize the rest
  std::vector<u8> out;
  out.reserve(size);
  out.insert(out.end(), code.begin(), code.begin() + code_start);

  size_t i = code_start;
  const u8* d = code.data();

  while (i < size)
  {
    u8 op = d[i];

    // Pattern 1: local.set X; local.get X -> local.tee X
    if (op == WasmOp::LocalSet)
    {
      size_t set_operand_start = i + 1;
      size_t set_operand_len = LEB128Size(d, set_operand_start);
      size_t after_set = set_operand_start + set_operand_len;

      if (after_set < size && d[after_set] == WasmOp::LocalGet)
      {
        size_t get_operand_start = after_set + 1;
        size_t get_operand_len = LEB128Size(d, get_operand_start);

        // Compare raw LEB128 bytes of the two operands
        if (set_operand_len == get_operand_len &&
            std::memcmp(d + set_operand_start, d + get_operand_start, set_operand_len) == 0)
        {
          // Emit local.tee X instead
          out.push_back(WasmOp::LocalTee);
          out.insert(out.end(), d + set_operand_start,
                     d + set_operand_start + set_operand_len);
          i = get_operand_start + get_operand_len;
          continue;
        }
      }
    }

    // Pattern 7: local.get X; drop -> remove both
    if (op == WasmOp::LocalGet)
    {
      size_t operand_start = i + 1;
      size_t operand_len = LEB128Size(d, operand_start);
      size_t after_get = operand_start + operand_len;

      if (after_get < size && d[after_get] == WasmOp::Drop)
      {
        // Skip both local.get X and drop
        i = after_get + 1;
        continue;
      }
    }

    // Pattern 6: i32.const C1; i32.const C2; <binop> -> i32.const (C1 op C2)
    // Also covers patterns 2-5 as special cases of constant folding.
    if (op == WasmOp::I32Const)
    {
      size_t c1_operand_start = i + 1;
      size_t c1_pos = c1_operand_start;
      s32 c1 = DecodeS32(d, c1_pos);
      size_t c1_end = c1_pos;  // position after first const's operand

      if (c1_end < size && d[c1_end] == WasmOp::I32Const)
      {
        // Two consecutive i32.const - check for binary op after
        size_t c2_pos = c1_end + 1;
        s32 c2 = DecodeS32(d, c2_pos);
        size_t c2_end = c2_pos;

        if (c2_end < size)
        {
          u8 binop = d[c2_end];
          s32 result;
          bool can_fold = true;
          switch (binop)
          {
          case WasmOp::I32Add:
            result = c1 + c2;
            break;
          case WasmOp::I32Sub:
            result = c1 - c2;
            break;
          case WasmOp::I32Mul:
            result = c1 * c2;
            break;
          case WasmOp::I32And:
            result = c1 & c2;
            break;
          case WasmOp::I32Or:
            result = c1 | c2;
            break;
          case WasmOp::I32Xor:
            result = c1 ^ c2;
            break;
          case WasmOp::I32Shl:
            result = c1 << (c2 & 31);
            break;
          case WasmOp::I32ShrS:
            result = c1 >> (c2 & 31);
            break;
          case WasmOp::I32ShrU:
            result = static_cast<s32>(static_cast<u32>(c1) >> (c2 & 31));
            break;
          default:
            can_fold = false;
            result = 0;
            break;
          }

          if (can_fold)
          {
            out.push_back(WasmOp::I32Const);
            EncodeS32(out, result);
            i = c2_end + 1;  // skip past the binop
            continue;
          }
        }
      }

      // Patterns 2-5: i32.const <identity>; <binop> -> remove both
      // The other operand is already on the stack.
      if (c1_end < size)
      {
        u8 next_op = d[c1_end];

        // i32.const 0; i32.add -> identity
        // i32.const 0; i32.or  -> identity
        // i32.const 0; i32.xor -> identity
        if (c1 == 0 && (next_op == WasmOp::I32Add || next_op == WasmOp::I32Or ||
                        next_op == WasmOp::I32Xor))
        {
          i = c1_end + 1;  // skip const and binop
          continue;
        }

        // i32.const -1; i32.and -> identity
        if (c1 == -1 && next_op == WasmOp::I32And)
        {
          i = c1_end + 1;
          continue;
        }
      }
    }

    // No pattern matched: copy this instruction verbatim
    size_t operand_len = OpcodeOperandSize(op, d, i + 1);
    size_t instr_len = 1 + operand_len;
    out.insert(out.end(), d + i, d + i + instr_len);
    i += instr_len;
  }

  code.swap(out);
}

// Build complete WASM module bytes for a PPC block
void AotWasm::BuildWasmModuleBytes(std::vector<u8>& out, u32 block_address, u32 num_instructions,
                                    u32 downcount_amount, u32 broken_block_npc)
{
  // === WASM Header ===
  out.push_back(0x00);
  out.push_back(0x61);
  out.push_back(0x73);
  out.push_back(0x6d);  // magic
  out.push_back(0x01);
  out.push_back(0x00);
  out.push_back(0x00);
  out.push_back(0x00);  // version

  // === Type Section (id=1) ===
  {
    std::vector<u8> types;
    EncodeU32(types, 4);  // 4 types

    // Type 0: (i32) -> (i32)  [block function: returns next PC, or 0 to stop chain]
    types.push_back(0x60);
    EncodeU32(types, 1);
    types.push_back(WasmOp::I32);
    EncodeU32(types, 1);
    types.push_back(WasmOp::I32);

    // Type 1: (i32, i32, i32) -> ()  [fallback]
    types.push_back(0x60);
    EncodeU32(types, 3);
    types.push_back(WasmOp::I32);
    types.push_back(WasmOp::I32);
    types.push_back(WasmOp::I32);
    EncodeU32(types, 0);

    // Type 2: (i32) -> (i32)  [read]
    types.push_back(0x60);
    EncodeU32(types, 1);
    types.push_back(WasmOp::I32);
    EncodeU32(types, 1);
    types.push_back(WasmOp::I32);

    // Type 3: (i32, i32) -> ()  [write]
    types.push_back(0x60);
    EncodeU32(types, 2);
    types.push_back(WasmOp::I32);
    types.push_back(WasmOp::I32);
    EncodeU32(types, 0);

    WriteSection(out, 1, types);
  }

  // === Import Section (id=2) ===
  {
    std::vector<u8> imports;
    EncodeU32(imports, 9);  // 9 imports

    // Import 0: env.memory (shared memory)
    EncodeString(imports, "env");
    EncodeString(imports, "memory");
    imports.push_back(0x02);  // memory
    imports.push_back(0x03);  // limits: has_max + shared
    EncodeU32(imports, 0);    // min = 0
    EncodeU32(imports, 65536);  // max = 65536 (4GB)

    // Import 1: env.table (funcref table for call_indirect dispatch)
    EncodeString(imports, "env");
    EncodeString(imports, "table");
    imports.push_back(0x01);  // table
    imports.push_back(0x70);  // funcref
    imports.push_back(0x01);  // limits: has_max
    EncodeU32(imports, 0);    // min = 0
    EncodeU32(imports, 65536);  // max = 65536

    // Import 2 (func idx 0): env.fallback (func type 1)
    EncodeString(imports, "env");
    EncodeString(imports, "fallback");
    imports.push_back(0x00);  // func
    EncodeU32(imports, 1);    // type index

    // Import 3-5 (func idx 1-3): read functions (func type 2)
    const char* read_names[] = {"read_u8", "read_u16", "read_u32"};
    for (int i = 0; i < 3; i++)
    {
      EncodeString(imports, "env");
      EncodeString(imports, read_names[i]);
      imports.push_back(0x00);
      EncodeU32(imports, 2);
    }

    // Import 6-8 (func idx 4-6): write functions (func type 3)
    const char* write_names[] = {"write_u8", "write_u16", "write_u32"};
    for (int i = 0; i < 3; i++)
    {
      EncodeString(imports, "env");
      EncodeString(imports, write_names[i]);
      imports.push_back(0x00);
      EncodeU32(imports, 3);
    }

    WriteSection(out, 2, imports);
  }

  // === Function Section (id=3) ===
  {
    std::vector<u8> funcs;
    EncodeU32(funcs, 1);  // 1 function
    EncodeU32(funcs, 0);  // type index 0: (i32) -> ()
    WriteSection(out, 3, funcs);
  }

  // === Export Section (id=7) ===
  {
    std::vector<u8> exports;
    EncodeU32(exports, 1);
    EncodeString(exports, "run");
    exports.push_back(0x00);          // func export
    EncodeU32(exports, FUNC_BLOCK);   // func index (after imports)
    WriteSection(out, 7, exports);
  }

  // === Code Section (id=10) ===
  {
    // Build function body
    std::vector<u8> body;

    // Pre-scan: check if block has any FPU instructions (needed for local layout and MSR check)
    bool has_fpu = false;
    for (u32 i = 0; i < num_instructions && !has_fpu; i++)
    {
      if (m_code_buffer[i].opinfo->flags & FL_USE_FPU)
        has_fpu = true;
    }

    // Local declarations layout (CR before FPR so FPR can be omitted for integer-only blocks):
    //   1-5: i32 (tmp1, tmp2, tmp3, ea, nibble)
    //   6: i64 (cr64)
    //   7-8: f64 (fpr0, fpr1)  -- scalar FPU temps
    //   9-40: i32 (32 GPR cache)
    //   41-48: i64 (8 CR field cache)
    //   49-80: v128 (32 FPR v128 cache) -- only for FPU blocks
    //   81-82: v128 (2 SIMD temp locals) -- only for FPU blocks
    if (has_fpu)
    {
      EncodeU32(body, 7);   // 7 local declaration groups
      EncodeU32(body, 5);   body.push_back(WasmOp::I32);   // 5 i32 temps
      EncodeU32(body, 1);   body.push_back(WasmOp::I64);   // 1 i64 cr64
      EncodeU32(body, 2);   body.push_back(WasmOp::F64);   // 2 f64 scalar FPU temps
      EncodeU32(body, 32);  body.push_back(WasmOp::I32);   // 32 i32 GPR cache
      EncodeU32(body, 8);   body.push_back(WasmOp::I64);   // 8 i64 CR cache
      EncodeU32(body, 32);  body.push_back(WasmOp::V128);  // 32 v128 FPR cache
      EncodeU32(body, 2);   body.push_back(WasmOp::V128);  // 2 v128 SIMD temps
    }
    else
    {
      EncodeU32(body, 5);   // 5 local declaration groups (no FPR)
      EncodeU32(body, 5);   body.push_back(WasmOp::I32);   // 5 i32 temps
      EncodeU32(body, 1);   body.push_back(WasmOp::I64);   // 1 i64 cr64
      EncodeU32(body, 2);   body.push_back(WasmOp::F64);   // 2 f64 placeholders (index stability)
      EncodeU32(body, 32);  body.push_back(WasmOp::I32);   // 32 i32 GPR cache
      // CR cache follows GPR at index 41 (same as FPU path)
      EncodeU32(body, 8);   body.push_back(WasmOp::I64);   // 8 i64 CR cache
      // No FPR/SIMD locals: saves space for integer-only blocks
    }

    // If block has FPU instructions, emit inline MSR.FP check at the start.
    // If FP is disabled, set EXCEPTION_FPU_UNAVAILABLE and return immediately.
    if (has_fpu)
    {
      // if (!(msr & (1 << 13))) { Exceptions |= 0x40; pc = block_address; return; }
      EmitLocalGet(body, LOCAL_STATE);
      EmitI32Load(body, OFF_MSR);
      EmitI32Const(body, 1 << 13);  // MSR_FP bit
      body.push_back(WasmOp::I32And);
      body.push_back(WasmOp::I32Eqz);
      body.push_back(WasmOp::If);
      body.push_back(WasmOp::Void);
      // Set exception
      EmitLocalGet(body, LOCAL_STATE);
      EmitLocalGet(body, LOCAL_STATE);
      EmitI32Load(body, OFF_EXCEPTIONS);
      EmitI32Const(body, 0x40);  // EXCEPTION_FPU_UNAVAILABLE
      body.push_back(WasmOp::I32Or);
      EmitI32Store(body, OFF_EXCEPTIONS);
      // Set pc = block_address
      EmitLocalGet(body, LOCAL_STATE);
      EmitI32Const(body, static_cast<s32>(block_address));
      EmitI32Store(body, OFF_PC);
      // Adjust downcount
      EmitLocalGet(body, LOCAL_STATE);
      EmitLocalGet(body, LOCAL_STATE);
      EmitI32Load(body, OFF_DOWNCOUNT);
      EmitI32Const(body, static_cast<s32>(downcount_amount));
      body.push_back(WasmOp::I32Sub);
      EmitI32Store(body, OFF_DOWNCOUNT);
      EmitI32Const(body, 0);  // return 0 = stop chain (exception set)
      body.push_back(WasmOp::Return);
      body.push_back(WasmOp::End);
    }

    // GPR register cache: avoids redundant memory loads/stores within a block
    GPRCache gpr_cache;

    // Self-loop detection: check if the block ends with a branch back to block_address.
    // If so, wrap in a WASM loop to avoid WASM→JS→WASM overhead per iteration.
    bool is_self_loop = false;
    for (u32 i = num_instructions; i > 0; i--)
    {
      const PPCAnalyst::CodeOp& scan_op = m_code_buffer[i - 1];
      if (!scan_op.canEndBlock)
        continue;
      const UGeckoInstruction scan_inst = scan_op.inst;
      u32 target = 0;
      bool has_target = false;
      if (scan_inst.OPCD == 16)  // bc: conditional branch
      {
        s32 bd = static_cast<s32>(static_cast<s16>(scan_inst.BD << 2));
        target = scan_inst.AA ? static_cast<u32>(bd) :
                                static_cast<u32>(static_cast<s32>(scan_op.address) + bd);
        has_target = true;
      }
      else if (scan_inst.OPCD == 18)  // b: unconditional branch
      {
        s32 li = static_cast<s32>(scan_inst.LI << 8) >> 6;
        target = scan_inst.AA ? static_cast<u32>(li) :
                                static_cast<u32>(static_cast<s32>(scan_op.address) + li);
        has_target = true;
      }
      if (has_target && target == block_address && !scan_inst.LK)
        is_self_loop = true;
      break;
    }

    if (is_self_loop)
    {
      // Pre-load all 32 GPRs into WASM locals before the loop.
      // This allows the loop body to use local.get (fast) instead of memory loads.
      // Cost: 32 loads once; savings: eliminates redundant memory loads on each iteration.
      for (u32 r = 0; r < 32; r++)
      {
        EmitLocalGet(body, LOCAL_STATE);
        EmitI32Load(body, OFF_GPR + r * 4);
        EmitLocalSet(body, LOCAL_GPR_BASE + r);
        gpr_cache.loaded[r] = true;
      }
      // Also pre-load FPRs as v128 if the block uses FPU instructions
      if (has_fpu)
      {
        for (u32 r = 0; r < 32; r++)
        {
          EmitLocalGet(body, LOCAL_STATE);
          EmitV128Load(body, OFF_PS + r * 16);
          EmitLocalSet(body, LOCAL_FPR_V128_BASE + r);
          gpr_cache.fpr_loaded[r] = true;
        }
      }
      body.push_back(WasmOp::Loop);
      body.push_back(WasmOp::Void);
    }

    // Emit code for each instruction
    for (u32 i = 0; i < num_instructions; i++)
    {
      const PPCAnalyst::CodeOp& op = m_code_buffer[i];

      if (op.skip)
        continue;

      bool is_can_end = op.canEndBlock;

      // For canEndBlock instructions, set default npc (needed for conditional branch not-taken path).
      // NPC is tracked in LOCAL_NPC to avoid memory round-trip; only written to state.pc on stop.
      if (is_can_end)
      {
        EmitI32Const(body, static_cast<s32>(op.address + 4));
        EmitLocalSet(body, LOCAL_NPC);
      }

      // Dead CR0 elimination: compute whether CR0 update can be skipped for Rc=1 instructions
      gpr_cache.skip_cr0 = IsCR0DeadAfter(m_code_buffer.data(), i, num_instructions);

      // Try to emit native WASM for both canEndBlock and normal instructions
      bool emitted = TryEmitWasmInstruction(body, op, gpr_cache);

      if (!emitted)
      {
        // Fallback batching: if this is NOT a canEndBlock instruction, look ahead
        // and collect consecutive fallback instructions to batch them under a single
        // register flush, reducing the overhead of WASM↔C++ transitions.
        u32 batch_end = i;
        bool batch_has_fpu_flag = (op.opinfo->flags & FL_USE_FPU) != 0;
        bool batch_has_loadstore = (op.opinfo->flags & FL_LOADSTORE) != 0;

        if (!is_can_end)
        {
          // Look ahead for more consecutive non-canEndBlock fallbacks
          for (u32 j = i + 1; j < num_instructions; j++)
          {
            const PPCAnalyst::CodeOp& next_op = m_code_buffer[j];
            if (next_op.skip)
              continue;
            if (next_op.canEndBlock)
              break;
            // Check if it would also fail to emit natively (peek: don't modify state)
            // We can't easily peek TryEmitWasmInstruction without side effects,
            // so just batch up to 8 consecutive instructions at most
            if (j - i >= 8)
              break;
            // Create a temporary cache copy to test emission
            GPRCache test_cache = gpr_cache;
            std::vector<u8> test_code;
            if (TryEmitWasmInstruction(test_code, next_op, test_cache))
              break;  // Next instruction can be natively emitted, stop batch
            // Include this instruction in the batch
            batch_end = j;
            if (next_op.opinfo->flags & FL_USE_FPU)
              batch_has_fpu_flag = true;
            if (next_op.opinfo->flags & FL_LOADSTORE)
              batch_has_loadstore = true;
          }
        }

        // Single flush for the entire batch
        EmitFlushAllGPRs(body, gpr_cache);
        EmitFlushAllCR(body, gpr_cache);
        if (batch_has_fpu_flag)
          EmitFlushAllFPRs(body, gpr_cache);
        gpr_cache.Invalidate();

        // Emit fallback calls for all batched instructions
        for (u32 k = i; k <= batch_end; k++)
        {
          const PPCAnalyst::CodeOp& batch_op = m_code_buffer[k];
          if (batch_op.skip)
            continue;
          EmitWasmFallback(body, batch_op.address, batch_op.inst.hex);
        }

        // Single exception check at end of batch (covers all load/stores)
        if (batch_has_loadstore)
        {
          EmitWasmExceptionCheck(body);
        }

        // Advance loop past batched instructions
        if (batch_end > i)
          i = batch_end;
      }

      // For canEndBlock fallback: interpreter wrote npc to state memory, reload into LOCAL_NPC
      if (!emitted && is_can_end)
      {
        EmitLocalGet(body, LOCAL_STATE);
        EmitI32Load(body, OFF_NPC);
        EmitLocalSet(body, LOCAL_NPC);
      }

      // End block after canEndBlock instructions
      if (is_can_end)
      {
        // Flush dirty cached registers before block exit
        EmitFlushAll(body, gpr_cache);
        EmitWasmEndBlock(body, downcount_amount);
        // EmitWasmEndBlock leaves i32 on stack: next PC (or returned 0 if stop)

        if (is_self_loop)
        {
          // Save EndBlock result, check if we should loop back
          EmitLocalSet(body, LOCAL_TMP3);  // save npc from stack

          // Check if we can loop back: pc == block_address (branch taken back to start)
          // AND the chain result is non-zero (downcount > 0 and no exceptions)
          EmitLocalGet(body, LOCAL_TMP1);  // tmp1 = npc (set by EndBlock)
          EmitI32Const(body, static_cast<s32>(block_address));
          body.push_back(WasmOp::I32Eq);
          EmitLocalGet(body, LOCAL_TMP3);
          body.push_back(WasmOp::I32And);  // non-zero tmp3 AND npc == block_address
          // Invalidate GPR/FPR cache for next iteration
          gpr_cache.Invalidate();
          body.push_back(WasmOp::BrIf);
          EncodeU32(body, 0);  // br_if 0 → loop top

          // Not looping — try tail-call dispatch to next block
          EmitWasmTailCallDispatch(body);
          EmitLocalGet(body, LOCAL_TMP3);
        }
        else
        {
          // No self-loop: try tail-call dispatch before returning
          // Stack has npc. Save it, try dispatch, restore if miss.
          EmitLocalSet(body, LOCAL_TMP3);
          EmitWasmTailCallDispatch(body);
          EmitLocalGet(body, LOCAL_TMP3);
        }
        // Stack has i32 (next PC for JS fallback, or 0 if stopped)
        body.push_back(WasmOp::Return);
      }
    }

    if (is_self_loop)
    {
      body.push_back(WasmOp::End);  // end loop
    }

    // Handle broken blocks (no branch at end)
    if (broken_block_npc != 0)
    {
      EmitI32Const(body, static_cast<s32>(broken_block_npc));
      EmitLocalSet(body, LOCAL_NPC);
    }
    // Flush dirty cached registers before final block exit
    EmitFlushAll(body, gpr_cache);
    EmitWasmEndBlock(body, downcount_amount);
    // Stack has npc. Try tail-call dispatch before falling through to function end.
    EmitLocalSet(body, LOCAL_TMP3);
    EmitWasmTailCallDispatch(body);
    EmitLocalGet(body, LOCAL_TMP3);
    // Function returns npc (for JS dispatch fallback)
    body.push_back(WasmOp::End);  // end of function

    // Run peephole optimizer on the function body
    PeepholeOptimize(body);

    // Wrap in code section
    std::vector<u8> code_section;
    EncodeU32(code_section, 1);  // 1 function
    EncodeU32(code_section, static_cast<u32>(body.size()));
    code_section.insert(code_section.end(), body.begin(), body.end());

    WriteSection(out, 10, code_section);
  }
}

int AotWasm::CompileWasmBlock(u32 block_address, u32 num_instructions, u32 downcount_amount,
                               u32 broken_block_npc)
{
  std::vector<u8> module_bytes;
  module_bytes.reserve(1024);

  BuildWasmModuleBytes(module_bytes, block_address, num_instructions, downcount_amount,
                       broken_block_npc);

  int func_id = aot_wasm_compile(module_bytes.data(), static_cast<int>(module_bytes.size()));
  return func_id;
}

// =====================================================================
// WasmBlockEntry callback - dispatches to compiled WASM blocks
// =====================================================================

s32 AotWasm::WasmBlockEntry(PowerPC::PowerPCState& ppc_state, const WasmBlockOperands& operands)
{
  // Chain execution: runs this block and continues to subsequent compiled blocks
  // without returning to C++ between blocks. Only returns when downcount <= 0,
  // exceptions are set, or the next block isn't compiled yet.
  aot_wasm_chain(operands.func_id,
                 static_cast<int>(reinterpret_cast<uintptr_t>(&ppc_state)),
                 static_cast<int>(OFF_PC),
                 static_cast<int>(OFF_DOWNCOUNT),
                 static_cast<int>(OFF_EXCEPTIONS));

  // Performance monitoring is approximate — we only count the first block's stats.
  // More accurate counting would require per-block tracking in the chain.
  PowerPC::UpdatePerformanceMonitor(operands.downcount, operands.num_load_stores,
                                    operands.num_fp_inst, ppc_state);
  return 0;  // exit callback loop
}

// =====================================================================
// AotWasm core methods
// =====================================================================

AotWasm::AotWasm(Core::System& system) : JitBase(system), m_block_cache(*this)
{
}

AotWasm::~AotWasm() = default;

void AotWasm::Init()
{
  RefreshConfig();

  AllocCodeSpace(CODE_SIZE);
  ResetFreeMemoryRanges();

  jo.enableBlocklink = false;

  m_block_cache.Init();

  code_block.m_stats = &js.st;
  code_block.m_gpa = &js.gpa;
  code_block.m_fpa = &js.fpa;

  // Capture RAM pointers for direct memory access from AOT WASM modules.
  // These pointers are offsets into the shared WASM linear memory.
  auto& memory = m_system.GetMemory();
  m_ram_base = reinterpret_cast<uintptr_t>(memory.GetRAM());
  m_ram_mask = memory.GetRamMask();
  if (memory.GetEXRAM())
  {
    m_exram_base = reinterpret_cast<uintptr_t>(memory.GetEXRAM());
    m_exram_mask = memory.GetExRamMask();
  }

  std::fprintf(stderr, "[orca-aot] Init: ram_base=0x%08x exram_base=0x%08x ppc_state=%p\n",
               static_cast<u32>(m_ram_base), static_cast<u32>(m_exram_base),
               static_cast<void*>(&m_system.GetPPCState()));

  // Initialize WASM dispatch table for call_indirect chain dispatch.
  // This table lives in WASM linear memory so blocks can read it directly.
  m_wasm_dispatch_table = new u32[262144 * 2];
  std::memset(m_wasm_dispatch_table, 0xFF, 262144 * 8);  // 0xFFFFFFFF = no entry
  m_wasm_dispatch_base = static_cast<u32>(reinterpret_cast<uintptr_t>(m_wasm_dispatch_table));

  // Init WebAssembly.Table and detect tail call support
  m_wasm_tail_calls = (aot_wasm_init_table() != 0);

  INFO_LOG_FMT(DYNA_REC, "AOT WASM transpiler initialized (offsets: pc={}, npc={}, gpr={}, "
               "exceptions={}, downcount={}, spr={}, ram_base=0x{:08x}, exram_base=0x{:08x}, "
               "dispatch_base=0x{:08x}, tail_calls={})",
               OFF_PC, OFF_NPC, OFF_GPR, OFF_EXCEPTIONS, OFF_DOWNCOUNT, OFF_SPR,
               m_ram_base, m_exram_base, m_wasm_dispatch_base, m_wasm_tail_calls);
}

void AotWasm::Shutdown()
{
  // Free all compiled WASM blocks and unregister from dispatch table
  for (auto& [addr, func_id] : m_wasm_func_ids)
  {
    if (func_id >= 0)
      aot_wasm_free(func_id);
    aot_wasm_unregister(static_cast<int>(addr));
  }
  m_wasm_func_ids.clear();
  delete[] m_wasm_dispatch_table;
  m_wasm_dispatch_table = nullptr;
  m_block_cache.Shutdown();
}

void AotWasm::ExecuteOneBlock()
{
  const u8* normal_entry = m_block_cache.Dispatch();
  if (!normal_entry)
  {
    Jit(m_ppc_state.pc);
    return;
  }

  auto& ppc_state = m_ppc_state;
  while (true)
  {
    const auto callback = *reinterpret_cast<const AnyCallback*>(normal_entry);
    const u8* payload = normal_entry + sizeof(callback);
    // Direct dispatch to the most commonly used callbacks for better performance
    if (callback == AnyCallbackCast(Interpret<false>)) [[likely]]
    {
      Interpret<false>(ppc_state, *reinterpret_cast<const InterpretOperands*>(payload));
      normal_entry = payload + sizeof(InterpretOperands);
    }
    else if (callback == AnyCallbackCast(Interpret<true>))
    {
      Interpret<true>(ppc_state, *reinterpret_cast<const InterpretOperands*>(payload));
      normal_entry = payload + sizeof(InterpretOperands);
    }
    else
    {
      if (const auto distance = callback(ppc_state, payload))
        normal_entry += distance;
      else
        break;
    }
  }
}

void AotWasm::Run()
{
  auto& core_timing = m_system.GetCoreTiming();

  const CPU::State* state_ptr = m_system.GetCPU().GetStatePtr();
  while (*state_ptr == CPU::State::Running)
  {
    core_timing.Advance();

    do
    {
      ExecuteOneBlock();
    } while (m_ppc_state.downcount > 0 && *state_ptr == CPU::State::Running);
  }
}

void AotWasm::SingleStep()
{
  m_system.GetCoreTiming().Advance();
  ExecuteOneBlock();
}

s32 AotWasm::StartProfiledBlock(PowerPC::PowerPCState& ppc_state,
                                const StartProfiledBlockOperands& operands)
{
  JitBlock::ProfileData::BeginProfiling(operands.profile_data);
  return sizeof(AnyCallback) + sizeof(operands);
}

template <bool profiled>
s32 AotWasm::EndBlock(PowerPC::PowerPCState& ppc_state,
                      const EndBlockOperands<profiled>& operands)
{
  ppc_state.pc = ppc_state.npc;
  ppc_state.downcount -= operands.downcount;
  PowerPC::UpdatePerformanceMonitor(operands.downcount, operands.num_load_stores,
                                    operands.num_fp_inst, ppc_state);
  if constexpr (profiled)
    JitBlock::ProfileData::EndProfiling(operands.profile_data, operands.downcount);
  return 0;
}

template <bool write_pc>
s32 AotWasm::Interpret(PowerPC::PowerPCState& ppc_state,
                       const InterpretOperands& operands)
{
  if constexpr (write_pc)
  {
    ppc_state.pc = operands.current_pc;
    ppc_state.npc = operands.current_pc + 4;
  }
  operands.func(operands.interpreter, operands.inst);
  return sizeof(AnyCallback) + sizeof(operands);
}

template <bool write_pc>
s32 AotWasm::InterpretAndCheckExceptions(
    PowerPC::PowerPCState& ppc_state, const InterpretAndCheckExceptionsOperands& operands)
{
  if constexpr (write_pc)
  {
    ppc_state.pc = operands.current_pc;
    ppc_state.npc = operands.current_pc + 4;
  }
  operands.func(operands.interpreter, operands.inst);

  if ((ppc_state.Exceptions & (EXCEPTION_DSI | EXCEPTION_PROGRAM)) != 0)
  {
    ppc_state.pc = operands.current_pc;
    ppc_state.downcount -= operands.downcount;
    operands.power_pc.CheckExceptions();
    return 0;
  }
  return sizeof(AnyCallback) + sizeof(operands);
}

s32 AotWasm::HLEFunction(PowerPC::PowerPCState& ppc_state,
                          const HLEFunctionOperands& operands)
{
  const auto& [system, current_pc, hook_index] = operands;
  ppc_state.pc = current_pc;
  HLE::Execute(Core::CPUThreadGuard{system}, current_pc, hook_index);
  return sizeof(AnyCallback) + sizeof(operands);
}

s32 AotWasm::WriteBrokenBlockNPC(PowerPC::PowerPCState& ppc_state,
                                 const WriteBrokenBlockNPCOperands& operands)
{
  const auto& [current_pc] = operands;
  ppc_state.npc = current_pc;
  return sizeof(AnyCallback) + sizeof(operands);
}

s32 AotWasm::CheckFPU(PowerPC::PowerPCState& ppc_state, const CheckHaltOperands& operands)
{
  const auto& [power_pc, current_pc, downcount] = operands;
  if (!ppc_state.msr.FP)
  {
    ppc_state.pc = current_pc;
    ppc_state.downcount -= downcount;
    ppc_state.Exceptions |= EXCEPTION_FPU_UNAVAILABLE;
    power_pc.CheckExceptions();
    return 0;
  }
  return sizeof(AnyCallback) + sizeof(operands);
}

s32 AotWasm::CheckBreakpoint(PowerPC::PowerPCState& ppc_state,
                              const CheckHaltOperands& operands)
{
  const auto& [power_pc, current_pc, downcount] = operands;
  ppc_state.pc = current_pc;
  if (power_pc.CheckAndHandleBreakPoints())
  {
    power_pc.GetPPCState().downcount -= downcount;
    return 0;
  }
  return sizeof(AnyCallback) + sizeof(operands);
}

s32 AotWasm::CheckIdle(PowerPC::PowerPCState& ppc_state,
                        const CheckIdleOperands& operands)
{
  const auto& [core_timing, idle_pc] = operands;
  if (ppc_state.npc == idle_pc)
    core_timing.Idle();
  return sizeof(AnyCallback) + sizeof(operands);
}

bool AotWasm::HandleFunctionHooking(u32 address)
{
  const auto result = HLE::TryReplaceFunction(m_ppc_symbol_db, address, PowerPC::CoreMode::JIT);
  if (!result)
    return false;

  Write(HLEFunction, {m_system, address, result.hook_index});

  if (result.type != HLE::HookType::Replace)
    return false;

  js.downcountAmount += js.st.numCycles;
  WriteEndBlock();
  return true;
}

void AotWasm::WriteEndBlock()
{
  if (IsProfilingEnabled())
  {
    Write(EndBlock<true>, {{js.downcountAmount, js.numLoadStoreInst, js.numFloatingPointInst},
                           js.curBlock->profile_data.get()});
  }
  else
  {
    Write(EndBlock<false>, {js.downcountAmount, js.numLoadStoreInst, js.numFloatingPointInst});
  }
}

bool AotWasm::SetEmitterStateToFreeCodeRegion()
{
  const auto free = m_free_ranges.by_size_begin();
  if (free == m_free_ranges.by_size_end())
  {
    WARN_LOG_FMT(DYNA_REC, "Failed to find free memory region in code region.");
    return false;
  }
  SetCodePtr(free.from(), free.to());
  return true;
}

void AotWasm::FreeRanges()
{
  for (const auto& [from, to] : m_block_cache.GetRangesToFree())
    m_free_ranges.insert(from, to);
  m_block_cache.ClearRangesToFree();
}

void AotWasm::ResetFreeMemoryRanges()
{
  m_free_ranges.clear();
  m_free_ranges.insert(region, region + region_size);
}

void AotWasm::Jit(u32 em_address)
{
  Jit(em_address, true);
}

void AotWasm::Jit(u32 em_address, bool clear_cache_and_retry_on_failure)
{
  if (IsAlmostFull() || SConfig::GetInstance().bJITNoBlockCache)
  {
    ClearCache();
  }
  FreeRanges();

  const u32 nextPC =
      analyzer.Analyze(em_address, &code_block, &m_code_buffer, m_code_buffer.size());
  if (code_block.m_memory_exception)
  {
    m_ppc_state.npc = nextPC;
    m_ppc_state.Exceptions |= EXCEPTION_ISI;
    m_system.GetPowerPC().CheckExceptions();
    WARN_LOG_FMT(POWERPC, "ISI exception at {:#010x}", nextPC);
    return;
  }

  if (SetEmitterStateToFreeCodeRegion())
  {
    JitBlock* b = m_block_cache.AllocateBlock(em_address);
    b->normalEntry = b->near_begin = GetWritableCodePtr();

    if (DoJit(em_address, b, nextPC))
    {
      b->near_end = GetWritableCodePtr();
      b->far_begin = b->far_end = nullptr;

      if (b->near_begin != b->near_end)
        m_free_ranges.erase(b->near_begin, b->near_end);

      m_block_cache.FinalizeBlock(*b, jo.enableBlocklink, code_block, m_code_buffer);

#ifdef JIT_LOG_GENERATED_CODE
      LogGeneratedCode();
#endif
      return;
    }
  }

  if (clear_cache_and_retry_on_failure)
  {
    WARN_LOG_FMT(DYNA_REC, "flushing code caches, please report if this happens a lot");
    ClearCache();
    Jit(em_address, false);
    return;
  }

  PanicAlertFmtT("JIT failed to find code space after a cache clear. This should never happen. "
                  "Please report this incident on the bug tracker. Dolphin will now exit.");
  std::exit(-1);
}

bool AotWasm::DoJit(u32 em_address, JitBlock* b, u32 nextPC)
{
  js.blockStart = em_address;
  js.firstFPInstructionFound = false;
  js.fifoBytesSinceCheck = 0;
  js.downcountAmount = 0;
  js.numLoadStoreInst = 0;
  js.numFloatingPointInst = 0;
  js.curBlock = b;

  auto& interpreter = m_system.GetInterpreter();
  auto& power_pc = m_system.GetPowerPC();
  auto& cpu = m_system.GetCPU();
  auto& breakpoints = power_pc.GetBreakPoints();

  // Count cycles and stats
  for (u32 i = 0; i < code_block.m_num_instructions; i++)
  {
    PPCAnalyst::CodeOp& op = m_code_buffer[i];
    js.downcountAmount += op.opinfo->num_cycles;
    if (op.opinfo->flags & FL_LOADSTORE)
      ++js.numLoadStoreInst;
    if (op.opinfo->flags & FL_USE_FPU)
      ++js.numFloatingPointInst;
  }

  // Try WASM compilation first (no debugging/profiling/HLE for WASM path)
  bool use_wasm = !IsDebuggingEnabled() && !IsProfilingEnabled();

#ifdef __EMSCRIPTEN__
  if (use_wasm)
  {
    // Check for HLE hooks in the block - if any, fall back to interpreter path
    for (u32 i = 0; i < code_block.m_num_instructions && use_wasm; i++)
    {
      const auto result = HLE::TryReplaceFunction(m_ppc_symbol_db, m_code_buffer[i].address,
                                                   PowerPC::CoreMode::JIT);
      if (result)
        use_wasm = false;
    }

    // FPU instructions are allowed — they fall back to interpreter via
    // EmitWasmFallback. We emit an inline MSR.FP check at the start of the
    // WASM block if any FPU instruction is present (see BuildWasmModuleBytes).
  }

  if (use_wasm)
  {
    u32 broken_npc = code_block.m_broken ? nextPC : 0;
    int func_id = CompileWasmBlock(em_address, code_block.m_num_instructions,
                                    js.downcountAmount, broken_npc);
    if (func_id >= 0)
    {
      m_wasm_func_ids[em_address] = func_id;

      // Register in JS fast dispatch table for block chaining
      aot_wasm_register(func_id, static_cast<int>(em_address));

      // Add to WebAssembly.Table for call_indirect dispatch
      int table_idx = aot_wasm_table_add(func_id);

      // Update WASM-memory dispatch table for intra-WASM lookup
      if (m_wasm_dispatch_table)
      {
        u32 hash = (em_address >> 2) & 0x3FFFF;
        m_wasm_dispatch_table[hash * 2] = em_address;
        m_wasm_dispatch_table[hash * 2 + 1] = static_cast<u32>(table_idx);
      }

      // Write a single WasmBlockEntry callback
      Write(WasmBlockEntry,
            {func_id, js.downcountAmount, js.numLoadStoreInst, js.numFloatingPointInst});

      if (HasWriteFailed())
      {
        WARN_LOG_FMT(DYNA_REC, "JIT ran out of space in code region during code generation.");
        return false;
      }
      return true;
    }
    // WASM compilation failed - fall through to interpreter path
  }
#endif

  // Fallback: CachedInterpreter-style callback array
  // Reset counters since we re-process instructions
  js.downcountAmount = 0;
  js.numLoadStoreInst = 0;
  js.numFloatingPointInst = 0;
  js.firstFPInstructionFound = false;

  if (IsProfilingEnabled())
    Write(StartProfiledBlock, {js.curBlock->profile_data.get()});

  for (u32 i = 0; i < code_block.m_num_instructions; i++)
  {
    PPCAnalyst::CodeOp& op = m_code_buffer[i];
    js.op = &op;

    js.compilerPC = op.address;
    js.instructionsLeft = (code_block.m_num_instructions - 1) - i;
    js.downcountAmount += op.opinfo->num_cycles;
    if (op.opinfo->flags & FL_LOADSTORE)
      ++js.numLoadStoreInst;
    if (op.opinfo->flags & FL_USE_FPU)
      ++js.numFloatingPointInst;

    if (HandleFunctionHooking(js.compilerPC))
      break;

    if (!op.skip)
    {
      if (IsDebuggingEnabled() && !cpu.IsStepping() &&
          breakpoints.IsAddressBreakPoint(js.compilerPC))
      {
        Write(CheckBreakpoint, {power_pc, js.compilerPC, js.downcountAmount});
      }
      if (!js.firstFPInstructionFound && (op.opinfo->flags & FL_USE_FPU) != 0)
      {
        Write(CheckFPU, {power_pc, js.compilerPC, js.downcountAmount});
        js.firstFPInstructionFound = true;
      }

      if ((jo.memcheck && (op.opinfo->flags & FL_LOADSTORE) != 0) ||
          (!op.canEndBlock && ShouldHandleFPExceptionForInstruction(&op)))
      {
        const InterpretAndCheckExceptionsOperands operands = {
            {interpreter, Interpreter::GetInterpreterOp(op.inst), js.compilerPC, op.inst},
            power_pc,
            js.downcountAmount};
        Write(op.canEndBlock ? CallbackCast(InterpretAndCheckExceptions<true>) :
                               CallbackCast(InterpretAndCheckExceptions<false>),
              operands);
      }
      else
      {
        const InterpretOperands operands = {interpreter, Interpreter::GetInterpreterOp(op.inst),
                                            js.compilerPC, op.inst};
        Write(op.canEndBlock ? CallbackCast(Interpret<true>) : CallbackCast(Interpret<false>),
              operands);
      }

      if (op.branchIsIdleLoop)
        Write(CheckIdle, {m_system.GetCoreTiming(), js.blockStart});
      if (op.canEndBlock)
        WriteEndBlock();
    }
  }
  if (code_block.m_broken)
  {
    Write(WriteBrokenBlockNPC, {nextPC});
    WriteEndBlock();
  }

  if (HasWriteFailed())
  {
    WARN_LOG_FMT(DYNA_REC, "JIT ran out of space in code region during code generation.");
    return false;
  }
  return true;
}

void AotWasm::EraseSingleBlock(const JitBlock& block)
{
  // Free the WASM block if it exists
  auto it = m_wasm_func_ids.find(block.effectiveAddress);
  if (it != m_wasm_func_ids.end())
  {
    if (it->second >= 0)
      aot_wasm_free(it->second);
    aot_wasm_unregister(static_cast<int>(block.effectiveAddress));
    m_wasm_func_ids.erase(it);
  }
  m_block_cache.EraseSingleBlock(block);
  FreeRanges();
}

std::vector<JitBase::MemoryStats> AotWasm::GetMemoryStats() const
{
  return {{"free", m_free_ranges.get_stats()}};
}

std::size_t AotWasm::DisassembleNearCode(const JitBlock& block,
                                         std::ostream& stream) const
{
  return Disassemble(block, stream);
}

std::size_t AotWasm::DisassembleFarCode(const JitBlock& block, std::ostream& stream) const
{
  stream << "N/A\n";
  return 0;
}

void AotWasm::ClearCache()
{
  // Free all WASM blocks and unregister from dispatch table
  for (auto& [addr, func_id] : m_wasm_func_ids)
  {
    if (func_id >= 0)
      aot_wasm_free(func_id);
    aot_wasm_unregister(static_cast<int>(addr));
  }
  m_wasm_func_ids.clear();

  m_block_cache.Clear();
  m_block_cache.ClearRangesToFree();
  ClearCodeSpace();
  ResetFreeMemoryRanges();
  RefreshConfig();
  Host_JitCacheInvalidation();
}

void AotWasm::LogGeneratedCode() const
{
  std::ostringstream stream;

  stream << "\nPPC Code Buffer:\n";
  for (const PPCAnalyst::CodeOp& op :
       std::span{m_code_buffer.data(), code_block.m_num_instructions})
  {
    fmt::print(stream, "0x{:08x}\t\t{}\n", op.address,
               Common::GekkoDisassembler::Disassemble(op.inst.hex, op.address));
  }

  stream << "\nHost Code:\n";
  Disassemble(*js.curBlock, stream);

  DEBUG_LOG_FMT(DYNA_REC, "{}", std::move(stream).str());
}
