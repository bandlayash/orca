// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/RangeSizeSet.h"
#include "Core/PowerPC/AotWasm/AotWasmBlockCache.h"
#include "Core/PowerPC/AotWasm/AotWasmEmitter.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/PPCAnalyst.h"

namespace CoreTiming
{
class CoreTimingManager;
}
namespace CPU
{
enum class State;
}
class Interpreter;

class AotWasm : public JitBase, public AotWasmCodeBlock
{
public:
  explicit AotWasm(Core::System& system);
  AotWasm(const AotWasm&) = delete;
  AotWasm(AotWasm&&) = delete;
  AotWasm& operator=(const AotWasm&) = delete;
  AotWasm& operator=(AotWasm&&) = delete;
  ~AotWasm() override;

  void Init() override;
  void Shutdown() override;

  bool HandleFault(uintptr_t access_address, SContext* ctx) override { return false; }
  void ClearCache() override;

  void Run() override;
  void SingleStep() override;

  void Jit(u32 address) override;
  void Jit(u32 address, bool clear_cache_and_retry_on_failure);
  bool DoJit(u32 address, JitBlock* b, u32 nextPC);

  void EraseSingleBlock(const JitBlock& block) override;
  std::vector<MemoryStats> GetMemoryStats() const override;

  static std::size_t Disassemble(const JitBlock& block, std::ostream& stream);

  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const override;
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const override;

  JitBaseBlockCache* GetBlockCache() override { return &m_block_cache; }
  const char* GetName() const override { return "AOT WASM"; }
  const CommonAsmRoutinesBase* GetAsmRoutines() override { return nullptr; }

private:
  void ExecuteOneBlock();

  bool HandleFunctionHooking(u32 address);
  void WriteEndBlock();

  bool SetEmitterStateToFreeCodeRegion();

  void FreeRanges();
  void ResetFreeMemoryRanges();

  void LogGeneratedCode() const;

  // WASM block compilation
  int CompileWasmBlock(u32 block_address, u32 num_instructions, u32 downcount_amount,
                       u32 broken_block_npc);
  void BuildWasmModuleBytes(std::vector<u8>& out, u32 block_address, u32 num_instructions,
                            u32 downcount_amount, u32 broken_block_npc);
  bool TryEmitWasmInstruction(std::vector<u8>& code, const PPCAnalyst::CodeOp& op,
                              struct GPRCache& cache);
  void EmitWasmFallback(std::vector<u8>& code, u32 pc, u32 inst_hex);
  void EmitWasmExceptionCheck(std::vector<u8>& code);
  void EmitWasmEndBlock(std::vector<u8>& code, u32 downcount);
  void EmitWasmTailCallDispatch(std::vector<u8>& code);

  // Track compiled WASM blocks
  std::unordered_map<u32, int> m_wasm_func_ids;  // block_address -> JS func ID

  // Direct memory access: cached host pointers (captured in Init)
  u32 m_ram_base = 0;       // RAM buffer offset in WASM linear memory
  u32 m_exram_base = 0;     // EXRAM buffer offset
  u32 m_ram_mask = 0;       // RAM address mask (e.g., 0x01FFFFFF for 24MB)
  u32 m_exram_mask = 0;     // EXRAM address mask

  // WASM-native dispatch table (for call_indirect chain dispatch)
  u32* m_wasm_dispatch_table = nullptr;  // [pc, table_idx] pairs, 64K entries
  u32 m_wasm_dispatch_base = 0;          // WASM linear memory address of table
  bool m_wasm_tail_calls = false;        // true if browser supports tail calls

  struct StartProfiledBlockOperands;
  template <bool profiled>
  struct EndBlockOperands;
  struct InterpretOperands;
  struct InterpretAndCheckExceptionsOperands;
  struct HLEFunctionOperands;
  struct WriteBrokenBlockNPCOperands;
  struct CheckHaltOperands;
  struct CheckIdleOperands;
  struct WasmBlockOperands;

  static s32 StartProfiledBlock(PowerPC::PowerPCState& ppc_state,
                                const StartProfiledBlockOperands& operands);
  static s32 StartProfiledBlock(std::ostream& stream, const StartProfiledBlockOperands& operands);
  template <bool profiled>
  static s32 EndBlock(PowerPC::PowerPCState& ppc_state, const EndBlockOperands<profiled>& operands);
  template <bool profiled>
  static s32 EndBlock(std::ostream& stream, const EndBlockOperands<profiled>& operands);
  template <bool write_pc>
  static s32 Interpret(PowerPC::PowerPCState& ppc_state, const InterpretOperands& operands);
  template <bool write_pc>
  static s32 Interpret(std::ostream& stream, const InterpretOperands& operands);
  template <bool write_pc>
  static s32 InterpretAndCheckExceptions(PowerPC::PowerPCState& ppc_state,
                                         const InterpretAndCheckExceptionsOperands& operands);
  template <bool write_pc>
  static s32 InterpretAndCheckExceptions(std::ostream& stream,
                                         const InterpretAndCheckExceptionsOperands& operands);
  static s32 HLEFunction(PowerPC::PowerPCState& ppc_state, const HLEFunctionOperands& operands);
  static s32 HLEFunction(std::ostream& stream, const HLEFunctionOperands& operands);
  static s32 WriteBrokenBlockNPC(PowerPC::PowerPCState& ppc_state,
                                 const WriteBrokenBlockNPCOperands& operands);
  static s32 WriteBrokenBlockNPC(std::ostream& stream, const WriteBrokenBlockNPCOperands& operands);
  static s32 CheckFPU(PowerPC::PowerPCState& ppc_state, const CheckHaltOperands& operands);
  static s32 CheckFPU(std::ostream& stream, const CheckHaltOperands& operands);
  static s32 CheckBreakpoint(PowerPC::PowerPCState& ppc_state, const CheckHaltOperands& operands);
  static s32 CheckBreakpoint(std::ostream& stream, const CheckHaltOperands& operands);
  static s32 CheckIdle(PowerPC::PowerPCState& ppc_state, const CheckIdleOperands& operands);
  static s32 CheckIdle(std::ostream& stream, const CheckIdleOperands& operands);
  static s32 WasmBlockEntry(PowerPC::PowerPCState& ppc_state, const WasmBlockOperands& operands);
  static s32 WasmBlockEntry(std::ostream& stream, const WasmBlockOperands& operands);

  Common::RangeSizeSet<u8*> m_free_ranges;
  AotWasmBlockCache m_block_cache;
};

struct AotWasm::StartProfiledBlockOperands
{
  JitBlock::ProfileData* profile_data;
};

template <>
struct AotWasm::EndBlockOperands<false>
{
  u32 downcount;
  u32 num_load_stores;
  u32 num_fp_inst;
  u32 : 32;
};

template <>
struct AotWasm::EndBlockOperands<true> : AotWasm::EndBlockOperands<false>
{
  JitBlock::ProfileData* profile_data;
};

struct AotWasm::InterpretOperands
{
  Interpreter& interpreter;
  void (*func)(Interpreter&, UGeckoInstruction);
  u32 current_pc;
  UGeckoInstruction inst;
};

struct AotWasm::InterpretAndCheckExceptionsOperands : InterpretOperands
{
  PowerPC::PowerPCManager& power_pc;
  u32 downcount;
};

struct AotWasm::HLEFunctionOperands
{
  Core::System& system;
  u32 current_pc;
  u32 hook_index;
};

struct AotWasm::WriteBrokenBlockNPCOperands
{
  u32 current_pc;
  u32 : 32;
};

struct AotWasm::CheckHaltOperands
{
  PowerPC::PowerPCManager& power_pc;
  u32 current_pc;
  u32 downcount;
};

struct AotWasm::CheckIdleOperands
{
  CoreTiming::CoreTimingManager& core_timing;
  u32 idle_pc;
};

struct AotWasm::WasmBlockOperands
{
  int func_id;
  u32 downcount;
  u32 num_load_stores;
  u32 num_fp_inst;
};
