// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_WASM_BASELINE_MIPS_LIFTOFF_ASSEMBLER_MIPS_H_
#define V8_WASM_BASELINE_MIPS_LIFTOFF_ASSEMBLER_MIPS_H_

#include "src/wasm/baseline/liftoff-assembler.h"

#define BAILOUT(reason) bailout("mips " reason)

namespace v8 {
namespace internal {
namespace wasm {

namespace liftoff {

// fp-4 holds the stack marker, fp-8 is the instance parameter, first stack
// slot is located at fp-16.
constexpr int32_t kConstantStackSpace = 8;
constexpr int32_t kFirstStackSlotOffset =
    kConstantStackSpace + LiftoffAssembler::kStackSlotSize;

inline MemOperand GetStackSlot(uint32_t index) {
  int32_t offset = index * LiftoffAssembler::kStackSlotSize;
  return MemOperand(fp, -kFirstStackSlotOffset - offset);
}

inline MemOperand GetHalfStackSlot(uint32_t half_index) {
  int32_t offset = half_index * (LiftoffAssembler::kStackSlotSize / 2);
  return MemOperand(fp, -kFirstStackSlotOffset - offset);
}

inline MemOperand GetInstanceOperand() { return MemOperand(fp, -8); }

// Use this register to store the address of the last argument pushed on the
// stack for a call to C. This register must be callee saved according to the c
// calling convention.
static constexpr Register kCCallLastArgAddrReg = s1;

inline void Load(LiftoffAssembler* assm, LiftoffRegister dst, MemOperand src,
                 ValueType type) {
  switch (type) {
    case kWasmI32:
      assm->lw(dst.gp(), src);
      break;
    case kWasmF32:
      assm->lwc1(dst.fp(), src);
      break;
    case kWasmF64:
      assm->Ldc1(dst.fp(), src);
      break;
    default:
      UNREACHABLE();
  }
}

inline void push(LiftoffAssembler* assm, LiftoffRegister reg, ValueType type) {
  switch (type) {
    case kWasmI32:
      assm->push(reg.gp());
      break;
    case kWasmI64:
      assm->Push(reg.high_gp(), reg.low_gp());
      break;
    case kWasmF32:
      assm->addiu(sp, sp, -sizeof(float));
      assm->swc1(reg.fp(), MemOperand(sp, 0));
      break;
    case kWasmF64:
      assm->addiu(sp, sp, -sizeof(double));
      assm->Sdc1(reg.fp(), MemOperand(sp, 0));
      break;
    default:
      UNREACHABLE();
  }
}

}  // namespace liftoff

uint32_t LiftoffAssembler::PrepareStackFrame() {
  uint32_t offset = static_cast<uint32_t>(pc_offset());
  // When constant that represents size of stack frame can't be represented
  // as 16bit we need three instructions to add it to sp, so we reserve space
  // for this case.
  addiu(sp, sp, 0);
  nop();
  nop();
  return offset;
}

void LiftoffAssembler::PatchPrepareStackFrame(uint32_t offset,
                                              uint32_t stack_slots) {
  uint32_t bytes = liftoff::kConstantStackSpace + kStackSlotSize * stack_slots;
  DCHECK_LE(bytes, kMaxInt);
  // We can't run out of space, just pass anything big enough to not cause the
  // assembler to try to grow the buffer.
  constexpr int kAvailableSpace = 256;
  TurboAssembler patching_assembler(isolate(), buffer_ + offset,
                                    kAvailableSpace, CodeObjectRequired::kNo);
  // If bytes can be represented as 16bit, addiu will be generated and two
  // nops will stay untouched. Otherwise, lui-ori sequence will load it to
  // register and, as third instruction, addu will be generated.
  patching_assembler.Addu(sp, sp, Operand(-bytes));
}

void LiftoffAssembler::FinishCode() {}

void LiftoffAssembler::LoadConstant(LiftoffRegister reg, WasmValue value,
                                    RelocInfo::Mode rmode) {
  switch (value.type()) {
    case kWasmI32:
      TurboAssembler::li(reg.gp(), Operand(value.to_i32(), rmode));
      break;
    case kWasmI64: {
      DCHECK(RelocInfo::IsNone(rmode));
      int32_t low_word = value.to_i64();
      int32_t high_word = value.to_i64() >> 32;
      TurboAssembler::li(reg.low_gp(), Operand(low_word));
      TurboAssembler::li(reg.high_gp(), Operand(high_word));
      break;
    }
    case kWasmF32:
      TurboAssembler::Move(reg.fp(), value.to_f32_boxed().get_bits());
      break;
    case kWasmF64:
      TurboAssembler::Move(reg.fp(), value.to_f64_boxed().get_bits());
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::LoadFromInstance(Register dst, uint32_t offset,
                                        int size) {
  DCHECK_LE(offset, kMaxInt);
  lw(dst, liftoff::GetInstanceOperand());
  DCHECK_EQ(4, size);
  lw(dst, MemOperand(dst, offset));
}

void LiftoffAssembler::SpillInstance(Register instance) {
  sw(instance, liftoff::GetInstanceOperand());
}

void LiftoffAssembler::FillInstanceInto(Register dst) {
  lw(dst, liftoff::GetInstanceOperand());
}

void LiftoffAssembler::Load(LiftoffRegister dst, Register src_addr,
                            Register offset_reg, uint32_t offset_imm,
                            LoadType type, LiftoffRegList pinned,
                            uint32_t* protected_load_pc, bool is_load_mem) {
  // TODO(ksreten): Add check if unaligned memory access
  Register src = no_reg;
  if (offset_reg != no_reg) {
    src = GetUnusedRegister(kGpReg, pinned).gp();
    emit_ptrsize_add(src, src_addr, offset_reg);
  }
  MemOperand src_op = (offset_reg != no_reg) ? MemOperand(src, offset_imm)
                                             : MemOperand(src_addr, offset_imm);

  if (protected_load_pc) *protected_load_pc = pc_offset();
  switch (type.value()) {
    case LoadType::kI32Load8U:
      lbu(dst.gp(), src_op);
      break;
    case LoadType::kI64Load8U:
      lbu(dst.low_gp(), src_op);
      xor_(dst.high_gp(), dst.high_gp(), dst.high_gp());
      break;
    case LoadType::kI32Load8S:
      lb(dst.gp(), src_op);
      break;
    case LoadType::kI64Load8S:
      lb(dst.low_gp(), src_op);
      TurboAssembler::Move(dst.high_gp(), dst.low_gp());
      sra(dst.high_gp(), dst.high_gp(), 31);
      break;
    case LoadType::kI32Load16U:
      TurboAssembler::Ulhu(dst.gp(), src_op);
      break;
    case LoadType::kI64Load16U:
      TurboAssembler::Ulhu(dst.low_gp(), src_op);
      xor_(dst.high_gp(), dst.high_gp(), dst.high_gp());
      break;
    case LoadType::kI32Load16S:
      TurboAssembler::Ulh(dst.gp(), src_op);
      break;
    case LoadType::kI64Load16S:
      TurboAssembler::Ulh(dst.low_gp(), src_op);
      TurboAssembler::Move(dst.high_gp(), dst.low_gp());
      sra(dst.high_gp(), dst.high_gp(), 31);
      break;
    case LoadType::kI32Load:
      TurboAssembler::Ulw(dst.gp(), src_op);
      break;
    case LoadType::kI64Load32U:
      TurboAssembler::Ulw(dst.low_gp(), src_op);
      xor_(dst.high_gp(), dst.high_gp(), dst.high_gp());
      break;
    case LoadType::kI64Load32S:
      TurboAssembler::Ulw(dst.low_gp(), src_op);
      TurboAssembler::Move(dst.high_gp(), dst.low_gp());
      sra(dst.high_gp(), dst.high_gp(), 31);
      break;
    case LoadType::kI64Load: {
      MemOperand src_op_upper = (offset_reg != no_reg)
                                    ? MemOperand(src, offset_imm + 4)
                                    : MemOperand(src_addr, offset_imm + 4);
      TurboAssembler::Ulw(dst.high_gp(), src_op_upper);
      TurboAssembler::Ulw(dst.low_gp(), src_op);
      break;
    }
    case LoadType::kF32Load:
      TurboAssembler::Ulwc1(dst.fp(), src_op, t8);
      break;
    case LoadType::kF64Load:
      TurboAssembler::Uldc1(dst.fp(), src_op, t8);
      break;
    default:
      UNREACHABLE();
  }

#if defined(V8_TARGET_BIG_ENDIAN)
  if (is_load_mem) {
    ChangeEndiannessLoad(dst, type, pinned);
  }
#endif
}

void LiftoffAssembler::Store(Register dst_addr, Register offset_reg,
                             uint32_t offset_imm, LiftoffRegister src,
                             StoreType type, LiftoffRegList pinned,
                             uint32_t* protected_store_pc, bool is_store_mem) {
  // TODO(ksreten): Add check if unaligned memory access
  Register dst = no_reg;
  if (offset_reg != no_reg) {
    dst = GetUnusedRegister(kGpReg, pinned).gp();
    emit_ptrsize_add(dst, dst_addr, offset_reg);
  }
  MemOperand dst_op = (offset_reg != no_reg) ? MemOperand(dst, offset_imm)
                                             : MemOperand(dst_addr, offset_imm);

#if defined(V8_TARGET_BIG_ENDIAN)
  if (is_store_mem) {
    LiftoffRegister tmp = GetUnusedRegister(src.reg_class(), pinned);
    // Save original value.
    Move(tmp, src, type.value_type());

    src = tmp;
    pinned.set(tmp);
    ChangeEndiannessStore(src, type, pinned);
  }
#endif

  if (protected_store_pc) *protected_store_pc = pc_offset();
  switch (type.value()) {
    case StoreType::kI64Store8:
      src = src.low();
      V8_FALLTHROUGH;
    case StoreType::kI32Store8:
      sb(src.gp(), dst_op);
      break;
    case StoreType::kI64Store16:
      src = src.low();
      V8_FALLTHROUGH;
    case StoreType::kI32Store16:
      TurboAssembler::Ush(src.gp(), dst_op, t8);
      break;
    case StoreType::kI64Store32:
      src = src.low();
      V8_FALLTHROUGH;
    case StoreType::kI32Store:
      TurboAssembler::Usw(src.gp(), dst_op);
      break;
    case StoreType::kI64Store: {
      MemOperand dst_op_upper = (offset_reg != no_reg)
                                    ? MemOperand(dst, offset_imm + 4)
                                    : MemOperand(dst_addr, offset_imm + 4);
      TurboAssembler::Usw(src.high_gp(), dst_op_upper);
      TurboAssembler::Usw(src.low_gp(), dst_op);
      break;
    }
    case StoreType::kF32Store:
      TurboAssembler::Uswc1(src.fp(), dst_op, t8);
      break;
    case StoreType::kF64Store:
      TurboAssembler::Usdc1(src.fp(), dst_op, t8);
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::ChangeEndiannessLoad(LiftoffRegister dst, LoadType type,
                                            LiftoffRegList pinned) {
  bool is_float = false;
  LiftoffRegister tmp = dst;
  switch (type.value()) {
    case LoadType::kI64Load8U:
    case LoadType::kI64Load8S:
      // Swap low and high registers.
      TurboAssembler::Move(at, tmp.low_gp());
      TurboAssembler::Move(tmp.low_gp(), tmp.high_gp());
      TurboAssembler::Move(tmp.high_gp(), at);
      V8_FALLTHROUGH;
    case LoadType::kI32Load8U:
    case LoadType::kI32Load8S:
      // No need to change endianness for byte size.
      return;
    case LoadType::kF32Load:
      is_float = true;
      tmp = GetUnusedRegister(kGpReg, pinned);
      emit_type_conversion(kExprI32ReinterpretF32, tmp, dst);
      V8_FALLTHROUGH;
    case LoadType::kI32Load:
      TurboAssembler::ByteSwapSigned(tmp.gp(), tmp.gp(), 4);
      break;
    case LoadType::kI32Load16S:
      TurboAssembler::ByteSwapSigned(tmp.gp(), tmp.gp(), 2);
      break;
    case LoadType::kI32Load16U:
      TurboAssembler::ByteSwapUnsigned(tmp.gp(), tmp.gp(), 2);
      break;
    case LoadType::kF64Load:
      is_float = true;
      tmp = GetUnusedRegister(kGpRegPair, pinned);
      emit_type_conversion(kExprI64ReinterpretF64, tmp, dst);
      V8_FALLTHROUGH;
    case LoadType::kI64Load:
      TurboAssembler::Move(at, tmp.low_gp());
      TurboAssembler::ByteSwapSigned(tmp.low_gp(), tmp.high_gp(), 4);
      TurboAssembler::ByteSwapSigned(tmp.high_gp(), at, 4);
      break;
    case LoadType::kI64Load16U:
      TurboAssembler::ByteSwapUnsigned(tmp.low_gp(), tmp.high_gp(), 2);
      TurboAssembler::Move(tmp.high_gp(), zero_reg);
      break;
    case LoadType::kI64Load16S:
      TurboAssembler::ByteSwapSigned(tmp.low_gp(), tmp.high_gp(), 2);
      sra(tmp.high_gp(), tmp.high_gp(), 31);
      break;
    case LoadType::kI64Load32U:
      TurboAssembler::ByteSwapSigned(tmp.low_gp(), tmp.high_gp(), 4);
      TurboAssembler::Move(tmp.high_gp(), zero_reg);
      break;
    case LoadType::kI64Load32S:
      TurboAssembler::ByteSwapSigned(tmp.low_gp(), tmp.high_gp(), 4);
      sra(tmp.high_gp(), tmp.high_gp(), 31);
      break;
    default:
      UNREACHABLE();
  }

  if (is_float) {
    switch (type.value()) {
      case LoadType::kF32Load:
        emit_type_conversion(kExprF32ReinterpretI32, dst, tmp);
        break;
      case LoadType::kF64Load:
        emit_type_conversion(kExprF64ReinterpretI64, dst, tmp);
        break;
      default:
        UNREACHABLE();
    }
  }
}

void LiftoffAssembler::ChangeEndiannessStore(LiftoffRegister src,
                                             StoreType type,
                                             LiftoffRegList pinned) {
  bool is_float = false;
  LiftoffRegister tmp = src;
  switch (type.value()) {
    case StoreType::kI64Store8:
      // Swap low and high registers.
      TurboAssembler::Move(at, tmp.low_gp());
      TurboAssembler::Move(tmp.low_gp(), tmp.high_gp());
      TurboAssembler::Move(tmp.high_gp(), at);
      V8_FALLTHROUGH;
    case StoreType::kI32Store8:
      // No need to change endianness for byte size.
      return;
    case StoreType::kF32Store:
      is_float = true;
      tmp = GetUnusedRegister(kGpReg, pinned);
      emit_type_conversion(kExprI32ReinterpretF32, tmp, src);
      V8_FALLTHROUGH;
    case StoreType::kI32Store:
    case StoreType::kI32Store16:
      TurboAssembler::ByteSwapSigned(tmp.gp(), tmp.gp(), 4);
      break;
    case StoreType::kF64Store:
      is_float = true;
      tmp = GetUnusedRegister(kGpRegPair, pinned);
      emit_type_conversion(kExprI64ReinterpretF64, tmp, src);
      V8_FALLTHROUGH;
    case StoreType::kI64Store:
    case StoreType::kI64Store32:
    case StoreType::kI64Store16:
      TurboAssembler::Move(at, tmp.low_gp());
      TurboAssembler::ByteSwapSigned(tmp.low_gp(), tmp.high_gp(), 4);
      TurboAssembler::ByteSwapSigned(tmp.high_gp(), at, 4);
      break;
    default:
      UNREACHABLE();
  }

  if (is_float) {
    switch (type.value()) {
      case StoreType::kF32Store:
        emit_type_conversion(kExprF32ReinterpretI32, src, tmp);
        break;
      case StoreType::kF64Store:
        emit_type_conversion(kExprF64ReinterpretI64, src, tmp);
        break;
      default:
        UNREACHABLE();
    }
  }
}

void LiftoffAssembler::LoadCallerFrameSlot(LiftoffRegister dst,
                                           uint32_t caller_slot_idx,
                                           ValueType type) {
  MemOperand src(fp, kPointerSize * (caller_slot_idx + 1));
  liftoff::Load(this, dst, src, type);
}

void LiftoffAssembler::MoveStackValue(uint32_t dst_index, uint32_t src_index,
                                      ValueType type) {
  DCHECK_NE(dst_index, src_index);
  LiftoffRegister reg = GetUnusedRegister(reg_class_for(type));
  Fill(reg, src_index, type);
  Spill(dst_index, reg, type);
}

void LiftoffAssembler::MoveToReturnRegister(LiftoffRegister reg,
                                            ValueType type) {
  // TODO(wasm): Extract the destination register from the CallDescriptor.
  // TODO(wasm): Add multi-return support.
  LiftoffRegister dst =
      reg.is_pair() ? LiftoffRegister::ForPair(v0, v1)
                    : reg.is_gp() ? LiftoffRegister(v0) : LiftoffRegister(f2);
  if (reg != dst) Move(dst, reg, type);
}

void LiftoffAssembler::Move(Register dst, Register src, ValueType type) {
  DCHECK_NE(dst, src);
  TurboAssembler::mov(dst, src);
}

void LiftoffAssembler::Move(DoubleRegister dst, DoubleRegister src,
                            ValueType type) {
  DCHECK_NE(dst, src);
  TurboAssembler::Move(dst, src);
}

void LiftoffAssembler::Spill(uint32_t index, LiftoffRegister reg,
                             ValueType type) {
  RecordUsedSpillSlot(index);
  MemOperand dst = liftoff::GetStackSlot(index);
  switch (type) {
    case kWasmI32:
      sw(reg.gp(), dst);
      break;
    case kWasmI64:
      sw(reg.low_gp(), dst);
      sw(reg.high_gp(), liftoff::GetHalfStackSlot(2 * index + 1));
      break;
    case kWasmF32:
      swc1(reg.fp(), dst);
      break;
    case kWasmF64:
      TurboAssembler::Sdc1(reg.fp(), dst);
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::Spill(uint32_t index, WasmValue value) {
  RecordUsedSpillSlot(index);
  MemOperand dst = liftoff::GetStackSlot(index);
  switch (value.type()) {
    case kWasmI32: {
      LiftoffRegister tmp = GetUnusedRegister(kGpReg);
      TurboAssembler::li(tmp.gp(), Operand(value.to_i32()));
      sw(tmp.gp(), dst);
      break;
    }
    case kWasmI64: {
      LiftoffRegister tmp = GetUnusedRegister(kGpRegPair);

      int32_t low_word = value.to_i64();
      int32_t high_word = value.to_i64() >> 32;
      TurboAssembler::li(tmp.low_gp(), Operand(low_word));
      TurboAssembler::li(tmp.high_gp(), Operand(high_word));

      sw(tmp.low_gp(), dst);
      sw(tmp.high_gp(), liftoff::GetHalfStackSlot(2 * index + 1));
      break;
    }
    default:
      // kWasmF32 and kWasmF64 are unreachable, since those
      // constants are not tracked.
      UNREACHABLE();
  }
}

void LiftoffAssembler::Fill(LiftoffRegister reg, uint32_t index,
                            ValueType type) {
  MemOperand src = liftoff::GetStackSlot(index);
  switch (type) {
    case kWasmI32:
      lw(reg.gp(), src);
      break;
    case kWasmI64:
      lw(reg.low_gp(), src);
      lw(reg.high_gp(), liftoff::GetHalfStackSlot(2 * index + 1));
      break;
    case kWasmF32:
      lwc1(reg.fp(), src);
      break;
    case kWasmF64:
      TurboAssembler::Ldc1(reg.fp(), src);
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::FillI64Half(Register reg, uint32_t half_index) {
  lw(reg, liftoff::GetHalfStackSlot(half_index));
}

void LiftoffAssembler::emit_i32_mul(Register dst, Register lhs, Register rhs) {
  TurboAssembler::Mul(dst, lhs, rhs);
}

#define I32_BINOP(name, instruction)                                 \
  void LiftoffAssembler::emit_i32_##name(Register dst, Register lhs, \
                                         Register rhs) {             \
    instruction(dst, lhs, rhs);                                      \
  }

// clang-format off
I32_BINOP(add, addu)
I32_BINOP(sub, subu)
I32_BINOP(and, and_)
I32_BINOP(or, or_)
I32_BINOP(xor, xor_)
// clang-format on

#undef I32_BINOP

bool LiftoffAssembler::emit_i32_clz(Register dst, Register src) {
  TurboAssembler::Clz(dst, src);
  return true;
}

bool LiftoffAssembler::emit_i32_ctz(Register dst, Register src) {
  TurboAssembler::Ctz(dst, src);
  return true;
}

bool LiftoffAssembler::emit_i32_popcnt(Register dst, Register src) {
  TurboAssembler::Popcnt(dst, src);
  return true;
}

#define I32_SHIFTOP(name, instruction)                                      \
  void LiftoffAssembler::emit_i32_##name(                                   \
      Register dst, Register src, Register amount, LiftoffRegList pinned) { \
    instruction(dst, src, amount);                                          \
  }

I32_SHIFTOP(shl, sllv)
I32_SHIFTOP(sar, srav)
I32_SHIFTOP(shr, srlv)

#undef I32_SHIFTOP

void LiftoffAssembler::emit_i64_mul(LiftoffRegister dst, LiftoffRegister lhs,
                                    LiftoffRegister rhs) {
  LiftoffRegister org_dst = dst;
  // Allocate registers for dst, to prevent overwriting source.
  if (dst.overlaps(lhs) || dst.overlaps(rhs)) {
    dst = GetUnusedRegister(kGpRegPair, LiftoffRegList::ForRegs(lhs, rhs));
  }

  // Multiply.
  TurboAssembler::Mulu(dst.high_gp(), dst.low_gp(), lhs.low_gp(), rhs.low_gp());
  LiftoffRegister tmp =
      GetUnusedRegister(kGpReg, LiftoffRegList::ForRegs(dst, lhs, rhs));
  TurboAssembler::Mul(tmp.gp(), lhs.low_gp(), rhs.high_gp());
  TurboAssembler::Addu(dst.high_gp(), dst.high_gp(), tmp.gp());

  TurboAssembler::Mul(tmp.gp(), lhs.high_gp(), rhs.low_gp());
  TurboAssembler::Addu(dst.high_gp(), dst.high_gp(), tmp.gp());

  // Move result to original dst if needed.
  if (dst != org_dst) {
    Move(org_dst, dst, kWasmI64);
  }
}

void LiftoffAssembler::emit_i64_add(LiftoffRegister dst, LiftoffRegister lhs,
                                    LiftoffRegister rhs) {
  LiftoffRegister org_dst = dst;
  // Allocate registers for dst, to prevent overwriting source.
  if (dst.overlaps(lhs) || dst.overlaps(rhs)) {
    dst = GetUnusedRegister(kGpRegPair, LiftoffRegList::ForRegs(lhs, rhs));
  }

  TurboAssembler::AddPair(dst.low_gp(), dst.high_gp(), lhs.low_gp(),
                          lhs.high_gp(), rhs.low_gp(), rhs.high_gp());

  // Move result to original dst if needed.
  if (dst != org_dst) {
    Move(org_dst, dst, kWasmI64);
  }
}

void LiftoffAssembler::emit_i64_sub(LiftoffRegister dst, LiftoffRegister lhs,
                                    LiftoffRegister rhs) {
  LiftoffRegister org_dst = dst;
  // Allocate registers for dst, to prevent overwriting source.
  if (dst.overlaps(lhs) || dst.overlaps(rhs)) {
    dst = GetUnusedRegister(kGpRegPair, LiftoffRegList::ForRegs(lhs, rhs));
  }

  TurboAssembler::SubPair(dst.low_gp(), dst.high_gp(), lhs.low_gp(),
                          lhs.high_gp(), rhs.low_gp(), rhs.high_gp());

  // Move result to original dst if needed.
  if (dst != org_dst) {
    Move(org_dst, dst, kWasmI64);
  }
}

namespace liftoff {

inline bool IsRegInRegPair(LiftoffRegister pair, Register reg) {
  DCHECK(pair.is_pair());
  return pair.low_gp() == reg || pair.high_gp() == reg;
}

inline void Emit64BitShiftOperation(
    LiftoffAssembler* assm, LiftoffRegister dst, LiftoffRegister src,
    Register amount,
    void (TurboAssembler::*emit_shift)(Register, Register, Register, Register,
                                       Register, Register, Register),
    LiftoffRegList pinned) {
  Label move, done;
  pinned.set(dst);
  pinned.set(src);
  pinned.set(amount);

  // If shift amount is 0, don't do the shifting.
  assm->TurboAssembler::Branch(&move, eq, amount, Operand(zero_reg));

  if (liftoff::IsRegInRegPair(dst, amount) || dst.overlaps(src)) {
    // If some of destination registers are in use, get another, unused pair.
    // That way we prevent overwriting some input registers while shifting.
    LiftoffRegister tmp = assm->GetUnusedRegister(kGpRegPair, pinned);

    // Do the actual shift.
    (assm->*emit_shift)(tmp.low_gp(), tmp.high_gp(), src.low_gp(),
                        src.high_gp(), amount, kScratchReg, kScratchReg2);

    // Place result in destination register.
    assm->TurboAssembler::Move(dst.high_gp(), tmp.high_gp());
    assm->TurboAssembler::Move(dst.low_gp(), tmp.low_gp());
  } else {
    (assm->*emit_shift)(dst.low_gp(), dst.high_gp(), src.low_gp(),
                        src.high_gp(), amount, kScratchReg, kScratchReg2);
  }
  assm->TurboAssembler::Branch(&done);

  // If shift amount is 0, move src to dst.
  assm->bind(&move);
  assm->TurboAssembler::Move(dst.high_gp(), src.high_gp());
  assm->TurboAssembler::Move(dst.low_gp(), src.low_gp());

  assm->bind(&done);
}
}  // namespace liftoff

void LiftoffAssembler::emit_i64_shl(LiftoffRegister dst, LiftoffRegister src,
                                    Register amount, LiftoffRegList pinned) {
  liftoff::Emit64BitShiftOperation(this, dst, src, amount,
                                   &TurboAssembler::ShlPair, pinned);
}

void LiftoffAssembler::emit_i64_sar(LiftoffRegister dst, LiftoffRegister src,
                                    Register amount, LiftoffRegList pinned) {
  liftoff::Emit64BitShiftOperation(this, dst, src, amount,
                                   &TurboAssembler::SarPair, pinned);
}

void LiftoffAssembler::emit_i64_shr(LiftoffRegister dst, LiftoffRegister src,
                                    Register amount, LiftoffRegList pinned) {
  liftoff::Emit64BitShiftOperation(this, dst, src, amount,
                                   &TurboAssembler::ShrPair, pinned);
}

#define FP_BINOP(name, instruction)                                          \
  void LiftoffAssembler::emit_##name(DoubleRegister dst, DoubleRegister lhs, \
                                     DoubleRegister rhs) {                   \
    instruction(dst, lhs, rhs);                                              \
  }
#define FP_UNOP(name, instruction)                                             \
  void LiftoffAssembler::emit_##name(DoubleRegister dst, DoubleRegister src) { \
    instruction(dst, src);                                                     \
  }

FP_BINOP(f32_add, add_s)
FP_BINOP(f32_sub, sub_s)
FP_BINOP(f32_mul, mul_s)
FP_BINOP(f32_div, div_s)
FP_UNOP(f32_abs, abs_s)
FP_UNOP(f32_neg, neg_s)
FP_UNOP(f32_ceil, Ceil_s_s)
FP_UNOP(f32_floor, Floor_s_s)
FP_UNOP(f32_trunc, Trunc_s_s)
FP_UNOP(f32_nearest_int, Round_s_s)
FP_UNOP(f32_sqrt, sqrt_s)
FP_BINOP(f64_add, add_d)
FP_BINOP(f64_sub, sub_d)
FP_BINOP(f64_mul, mul_d)
FP_BINOP(f64_div, div_d)
FP_UNOP(f64_abs, abs_d)
FP_UNOP(f64_neg, neg_d)
FP_UNOP(f64_sqrt, sqrt_d)

#undef FP_BINOP
#undef FP_UNOP

void LiftoffAssembler::emit_f64_ceil(DoubleRegister dst, DoubleRegister src) {
  if ((IsMipsArchVariant(kMips32r2) || IsMipsArchVariant(kMips32r6)) &&
      IsFp64Mode()) {
    Ceil_d_d(dst, src);
  } else {
    BAILOUT("emit_f64_ceil");
  }
}

void LiftoffAssembler::emit_f64_floor(DoubleRegister dst, DoubleRegister src) {
  if ((IsMipsArchVariant(kMips32r2) || IsMipsArchVariant(kMips32r6)) &&
      IsFp64Mode()) {
    Floor_d_d(dst, src);
  } else {
    BAILOUT("emit_f64_floor");
  }
}

void LiftoffAssembler::emit_f64_trunc(DoubleRegister dst, DoubleRegister src) {
  if ((IsMipsArchVariant(kMips32r2) || IsMipsArchVariant(kMips32r6)) &&
      IsFp64Mode()) {
    Trunc_d_d(dst, src);
  } else {
    BAILOUT("emit_f64_trunc");
  }
}

void LiftoffAssembler::emit_f64_nearest_int(DoubleRegister dst,
                                            DoubleRegister src) {
  if ((IsMipsArchVariant(kMips32r2) || IsMipsArchVariant(kMips32r6)) &&
      IsFp64Mode()) {
    Round_d_d(dst, src);
  } else {
    BAILOUT("emit_f64_nearest_int");
  }
}

bool LiftoffAssembler::emit_type_conversion(WasmOpcode opcode,
                                            LiftoffRegister dst,
                                            LiftoffRegister src) {
  switch (opcode) {
    case kExprI32ConvertI64:
      TurboAssembler::Move(dst.gp(), src.low_gp());
      return true;
    case kExprI32ReinterpretF32:
      mfc1(dst.gp(), src.fp());
      return true;
    case kExprI64SConvertI32:
      TurboAssembler::Move(dst.low_gp(), src.gp());
      TurboAssembler::Move(dst.high_gp(), src.gp());
      sra(dst.high_gp(), dst.high_gp(), 31);
      return true;
    case kExprI64UConvertI32:
      TurboAssembler::Move(dst.low_gp(), src.gp());
      TurboAssembler::Move(dst.high_gp(), zero_reg);
      return true;
    case kExprI64ReinterpretF64:
      mfc1(dst.low_gp(), src.fp());
      TurboAssembler::Mfhc1(dst.high_gp(), src.fp());
      return true;
    case kExprF32SConvertI32: {
      LiftoffRegister scratch =
          GetUnusedRegister(kFpReg, LiftoffRegList::ForRegs(dst));
      mtc1(src.gp(), scratch.fp());
      cvt_s_w(dst.fp(), scratch.fp());
      return true;
    }
    case kExprF32UConvertI32: {
      LiftoffRegister scratch =
          GetUnusedRegister(kFpReg, LiftoffRegList::ForRegs(dst));
      TurboAssembler::Cvt_d_uw(dst.fp(), src.gp(), scratch.fp());
      cvt_s_d(dst.fp(), dst.fp());
      return true;
    }
    case kExprF32ConvertF64:
      cvt_s_d(dst.fp(), src.fp());
      return true;
    case kExprF32ReinterpretI32:
      TurboAssembler::FmoveLow(dst.fp(), src.gp());
      return true;
    case kExprF64SConvertI32: {
      LiftoffRegister scratch =
          GetUnusedRegister(kFpReg, LiftoffRegList::ForRegs(dst));
      mtc1(src.gp(), scratch.fp());
      cvt_d_w(dst.fp(), scratch.fp());
      return true;
    }
    case kExprF64UConvertI32: {
      LiftoffRegister scratch =
          GetUnusedRegister(kFpReg, LiftoffRegList::ForRegs(dst));
      TurboAssembler::Cvt_d_uw(dst.fp(), src.gp(), scratch.fp());
      return true;
    }
    case kExprF64ConvertF32:
      cvt_d_s(dst.fp(), src.fp());
      return true;
    case kExprF64ReinterpretI64:
      mtc1(src.low_gp(), dst.fp());
      TurboAssembler::Mthc1(src.high_gp(), dst.fp());
      return true;
    default:
      return false;
  }
}

void LiftoffAssembler::emit_jump(Label* label) {
  TurboAssembler::Branch(label);
}

void LiftoffAssembler::emit_jump(Register target) { BAILOUT("emit_jump"); }

void LiftoffAssembler::emit_cond_jump(Condition cond, Label* label,
                                      ValueType type, Register lhs,
                                      Register rhs) {
  if (rhs != no_reg) {
    TurboAssembler::Branch(label, cond, lhs, Operand(rhs));
  } else {
    TurboAssembler::Branch(label, cond, lhs, Operand(zero_reg));
  }
}

void LiftoffAssembler::emit_i32_eqz(Register dst, Register src) {
  sltiu(dst, src, 1);
}

void LiftoffAssembler::emit_i32_set_cond(Condition cond, Register dst,
                                         Register lhs, Register rhs) {
  Register tmp = dst;
  if (dst == lhs || dst == rhs) {
    tmp = GetUnusedRegister(kGpReg, LiftoffRegList::ForRegs(lhs, rhs)).gp();
  }
  // Write 1 as result.
  TurboAssembler::li(tmp, 1);

  // If negative condition is true, write 0 as result.
  Condition neg_cond = NegateCondition(cond);
  TurboAssembler::LoadZeroOnCondition(tmp, lhs, Operand(rhs), neg_cond);

  // If tmp != dst, result will be moved.
  TurboAssembler::Move(dst, tmp);
}

void LiftoffAssembler::emit_i64_eqz(Register dst, LiftoffRegister src) {
  Register tmp =
      GetUnusedRegister(kGpReg, LiftoffRegList::ForRegs(src, dst)).gp();
  sltiu(tmp, src.low_gp(), 1);
  sltiu(dst, src.high_gp(), 1);
  and_(dst, dst, tmp);
}

namespace liftoff {
inline Condition cond_make_unsigned(Condition cond) {
  switch (cond) {
    case kSignedLessThan:
      return kUnsignedLessThan;
    case kSignedLessEqual:
      return kUnsignedLessEqual;
    case kSignedGreaterThan:
      return kUnsignedGreaterThan;
    case kSignedGreaterEqual:
      return kUnsignedGreaterEqual;
    default:
      return cond;
  }
}
}  // namespace liftoff

void LiftoffAssembler::emit_i64_set_cond(Condition cond, Register dst,
                                         LiftoffRegister lhs,
                                         LiftoffRegister rhs) {
  Label low, cont;

  // For signed i64 comparisons, we still need to use unsigned comparison for
  // the low word (the only bit carrying signedness information is the MSB in
  // the high word).
  Condition unsigned_cond = liftoff::cond_make_unsigned(cond);

  Register tmp = dst;
  if (liftoff::IsRegInRegPair(lhs, dst) || liftoff::IsRegInRegPair(rhs, dst)) {
    tmp =
        GetUnusedRegister(kGpReg, LiftoffRegList::ForRegs(dst, lhs, rhs)).gp();
  }

  // Write 1 initially in tmp register.
  TurboAssembler::li(tmp, 1);

  // If high words are equal, then compare low words, else compare high.
  Branch(&low, eq, lhs.high_gp(), Operand(rhs.high_gp()));

  TurboAssembler::LoadZeroOnCondition(
      tmp, lhs.high_gp(), Operand(rhs.high_gp()), NegateCondition(cond));
  Branch(&cont);

  bind(&low);
  TurboAssembler::LoadZeroOnCondition(tmp, lhs.low_gp(), Operand(rhs.low_gp()),
                                      NegateCondition(unsigned_cond));

  bind(&cont);
  // Move result to dst register if needed.
  TurboAssembler::Move(dst, tmp);
}

namespace liftoff {

inline FPUCondition ConditionToConditionCmpFPU(bool& predicate,
                                               Condition condition) {
  switch (condition) {
    case kEqual:
      predicate = true;
      return EQ;
    case kUnequal:
      predicate = false;
      return EQ;
    case kUnsignedLessThan:
      predicate = true;
      return OLT;
    case kUnsignedGreaterEqual:
      predicate = false;
      return OLT;
    case kUnsignedLessEqual:
      predicate = true;
      return OLE;
    case kUnsignedGreaterThan:
      predicate = false;
      return OLE;
    default:
      predicate = true;
      break;
  }
  UNREACHABLE();
}

};  // namespace liftoff

void LiftoffAssembler::emit_f32_set_cond(Condition cond, Register dst,
                                         DoubleRegister lhs,
                                         DoubleRegister rhs) {
  Label not_nan, cont;
  TurboAssembler::CompareIsNanF32(lhs, rhs);
  TurboAssembler::BranchFalseF(&not_nan);
  // If one of the operands is NaN, return 1 for f32.ne, else 0.
  if (cond == ne) {
    TurboAssembler::li(dst, 1);
  } else {
    TurboAssembler::Move(dst, zero_reg);
  }
  TurboAssembler::Branch(&cont);

  bind(&not_nan);

  TurboAssembler::li(dst, 1);
  bool predicate;
  FPUCondition fcond = liftoff::ConditionToConditionCmpFPU(predicate, cond);
  TurboAssembler::CompareF32(fcond, lhs, rhs);
  if (predicate) {
    TurboAssembler::Movf(dst, zero_reg);
  } else {
    TurboAssembler::Movt(dst, zero_reg);
  }

  bind(&cont);
}

void LiftoffAssembler::emit_f64_set_cond(Condition cond, Register dst,
                                         DoubleRegister lhs,
                                         DoubleRegister rhs) {
  Label not_nan, cont;
  TurboAssembler::CompareIsNanF64(lhs, rhs);
  TurboAssembler::BranchFalseF(&not_nan);
  // If one of the operands is NaN, return 1 for f64.ne, else 0.
  if (cond == ne) {
    TurboAssembler::li(dst, 1);
  } else {
    TurboAssembler::Move(dst, zero_reg);
  }
  TurboAssembler::Branch(&cont);

  bind(&not_nan);

  TurboAssembler::li(dst, 1);
  bool predicate;
  FPUCondition fcond = liftoff::ConditionToConditionCmpFPU(predicate, cond);
  TurboAssembler::CompareF64(fcond, lhs, rhs);
  if (predicate) {
    TurboAssembler::Movf(dst, zero_reg);
  } else {
    TurboAssembler::Movt(dst, zero_reg);
  }

  bind(&cont);
}

void LiftoffAssembler::StackCheck(Label* ool_code) {
  LiftoffRegister tmp = GetUnusedRegister(kGpReg);
  TurboAssembler::li(
      tmp.gp(), Operand(ExternalReference::address_of_stack_limit(isolate())));
  TurboAssembler::Ulw(tmp.gp(), MemOperand(tmp.gp()));
  TurboAssembler::Branch(ool_code, ule, sp, Operand(tmp.gp()));
}

void LiftoffAssembler::CallTrapCallbackForTesting() {
  PrepareCallCFunction(0, GetUnusedRegister(kGpReg).gp());
  CallCFunction(
      ExternalReference::wasm_call_trap_callback_for_testing(isolate()), 0);
}

void LiftoffAssembler::AssertUnreachable(AbortReason reason) {
  if (emit_debug_code()) Abort(reason);
}

void LiftoffAssembler::PushCallerFrameSlot(const VarState& src,
                                           uint32_t src_index,
                                           RegPairHalf half) {
  switch (src.loc()) {
    case VarState::kStack: {
      if (src.type() == kWasmF64) {
        DCHECK_EQ(kLowWord, half);
        lw(at, liftoff::GetHalfStackSlot(2 * src_index - 1));
        push(at);
      }
      lw(at,
         liftoff::GetHalfStackSlot(2 * src_index + (half == kLowWord ? 0 : 1)));
      push(at);
      break;
    }
    case VarState::kRegister:
      if (src.type() == kWasmI64) {
        PushCallerFrameSlot(
            half == kLowWord ? src.reg().low() : src.reg().high(), kWasmI32);
      } else {
        PushCallerFrameSlot(src.reg(), src.type());
      }
      break;
    case VarState::KIntConst: {
      // The high word is the sign extension of the low word.
      li(at,
         Operand(half == kLowWord ? src.i32_const() : src.i32_const() >> 31));
      push(at);
      break;
    }
  }
}

void LiftoffAssembler::PushCallerFrameSlot(LiftoffRegister reg,
                                           ValueType type) {
  liftoff::push(this, reg, type);
}

void LiftoffAssembler::PushRegisters(LiftoffRegList regs) {
  LiftoffRegList gp_regs = regs & kGpCacheRegList;
  unsigned num_gp_regs = gp_regs.GetNumRegsSet();
  if (num_gp_regs) {
    unsigned offset = num_gp_regs * kPointerSize;
    addiu(sp, sp, -offset);
    while (!gp_regs.is_empty()) {
      LiftoffRegister reg = gp_regs.GetFirstRegSet();
      offset -= kPointerSize;
      sw(reg.gp(), MemOperand(sp, offset));
      gp_regs.clear(reg);
    }
    DCHECK_EQ(offset, 0);
  }
  LiftoffRegList fp_regs = regs & kFpCacheRegList;
  unsigned num_fp_regs = fp_regs.GetNumRegsSet();
  if (num_fp_regs) {
    addiu(sp, sp, -(num_fp_regs * kStackSlotSize));
    unsigned offset = 0;
    while (!fp_regs.is_empty()) {
      LiftoffRegister reg = fp_regs.GetFirstRegSet();
      TurboAssembler::Sdc1(reg.fp(), MemOperand(sp, offset));
      fp_regs.clear(reg);
      offset += sizeof(double);
    }
    DCHECK_EQ(offset, num_fp_regs * sizeof(double));
  }
}

void LiftoffAssembler::PopRegisters(LiftoffRegList regs) {
  LiftoffRegList fp_regs = regs & kFpCacheRegList;
  unsigned fp_offset = 0;
  while (!fp_regs.is_empty()) {
    LiftoffRegister reg = fp_regs.GetFirstRegSet();
    TurboAssembler::Ldc1(reg.fp(), MemOperand(sp, fp_offset));
    fp_regs.clear(reg);
    fp_offset += sizeof(double);
  }
  if (fp_offset) addiu(sp, sp, fp_offset);
  LiftoffRegList gp_regs = regs & kGpCacheRegList;
  unsigned gp_offset = 0;
  while (!gp_regs.is_empty()) {
    LiftoffRegister reg = gp_regs.GetLastRegSet();
    lw(reg.gp(), MemOperand(sp, gp_offset));
    gp_regs.clear(reg);
    gp_offset += kPointerSize;
  }
  addiu(sp, sp, gp_offset);
}

void LiftoffAssembler::DropStackSlotsAndRet(uint32_t num_stack_slots) {
  DCHECK_LT(num_stack_slots, (1 << 16) / kPointerSize);  // 16 bit immediate
  TurboAssembler::DropAndRet(static_cast<int>(num_stack_slots));
}

void LiftoffAssembler::PrepareCCall(wasm::FunctionSig* sig,
                                    const LiftoffRegister* args,
                                    ValueType out_argument_type) {
  int pushed_bytes = 0;
  for (ValueType param_type : sig->parameters()) {
    pushed_bytes += RoundUp<kPointerSize>(WasmOpcodes::MemSize(param_type));
    liftoff::push(this, *args++, param_type);
  }
  if (out_argument_type != kWasmStmt) {
    int size = RoundUp<kPointerSize>(WasmOpcodes::MemSize(out_argument_type));
    addiu(sp, sp, -size);
    pushed_bytes += size;
  }
  // Save the original sp (before the first push), such that we can later
  // compute pointers to the pushed values. Do this only *after* pushing the
  // values, because {kCCallLastArgAddrReg} might collide with an arg register.
  addiu(liftoff::kCCallLastArgAddrReg, sp, pushed_bytes);
  constexpr Register kScratch = at;
  static_assert(kScratch != liftoff::kCCallLastArgAddrReg, "collision");
  int num_c_call_arguments = static_cast<int>(sig->parameter_count()) +
                             (out_argument_type != kWasmStmt);
  PrepareCallCFunction(num_c_call_arguments, kScratch);
}

void LiftoffAssembler::SetCCallRegParamAddr(Register dst, int param_byte_offset,
                                            ValueType type) {
  // Check that we don't accidentally override kCCallLastArgAddrReg.
  DCHECK_NE(liftoff::kCCallLastArgAddrReg, dst);
  addiu(dst, liftoff::kCCallLastArgAddrReg, -param_byte_offset);
}

void LiftoffAssembler::SetCCallStackParamAddr(int stack_param_idx,
                                              int param_byte_offset,
                                              ValueType type) {
  static constexpr Register kScratch = at;
  SetCCallRegParamAddr(kScratch, param_byte_offset, type);
  sw(kScratch, MemOperand(sp, stack_param_idx * kPointerSize));
}

void LiftoffAssembler::LoadCCallOutArgument(LiftoffRegister dst, ValueType type,
                                            int param_byte_offset) {
  // Check that we don't accidentally override kCCallLastArgAddrReg.
  DCHECK_NE(LiftoffRegister(liftoff::kCCallLastArgAddrReg), dst);
  MemOperand src(liftoff::kCCallLastArgAddrReg, -param_byte_offset);
  liftoff::Load(this, dst, src, type);
}

void LiftoffAssembler::CallC(ExternalReference ext_ref, uint32_t num_params) {
  CallCFunction(ext_ref, static_cast<int>(num_params));
}

void LiftoffAssembler::FinishCCall() {
  TurboAssembler::Move(sp, liftoff::kCCallLastArgAddrReg);
}

void LiftoffAssembler::CallNativeWasmCode(Address addr) {
  Call(addr, RelocInfo::WASM_CALL);
}

void LiftoffAssembler::CallRuntime(Zone* zone, Runtime::FunctionId fid) {
  // Set instance to zero.
  TurboAssembler::Move(cp, zero_reg);
  CallRuntimeDelayed(zone, fid);
}

void LiftoffAssembler::CallIndirect(wasm::FunctionSig* sig,
                                    compiler::CallDescriptor* call_descriptor,
                                    Register target) {
  if (target == no_reg) {
    pop(at);
    Call(at);
  } else {
    Call(target);
  }
}

void LiftoffAssembler::AllocateStackSlot(Register addr, uint32_t size) {
  addiu(sp, sp, -size);
  TurboAssembler::Move(addr, sp);
}

void LiftoffAssembler::DeallocateStackSlot(uint32_t size) {
  addiu(sp, sp, size);
}

}  // namespace wasm
}  // namespace internal
}  // namespace v8

#undef BAILOUT

#endif  // V8_WASM_BASELINE_MIPS_LIFTOFF_ASSEMBLER_MIPS_H_
