/**
 * @file lns_instruction_test.cpp
 * @brief The LNS extension on the single-cycle RVSS processor: ladd/lsub/lmul/ldiv,
 *        fcvt.lns.s / fcvt.s.lns, the integer converts, llw/lsw, the L register bank (l0 hard-wired to +0, reset
 *        value +0), reserved encodings, and undo/redo.
 *
 * The results are checked against the LnsAlu / LnsConverter library directly (the instructions
 * must be a thin wrapper around them) and, for a few values, against the real number.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "common/instructions.h"
#include "processor/lns/lns_alu.h"
#include "processor/lns/lns_cordic_addsub.h"
#include "processor/lns/lns_conversion.h"
#include "processor/lns/lns_format.h"
#include "processor/registers.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

namespace
{
namespace lns = Kites::lns;

constexpr uint64_t kDataBase = 0x10000000;

uint32_t floatBits(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

float bitsToFloat(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

/// "li t0, <bits>; fmv.w.x f<reg>, t0" : put an FP32 value in an FPR without touching memory.
std::string loadFloat(int fpr, float value)
{
    return "li t0, " + std::to_string(floatBits(value)) + "\nfmv.w.x f" + std::to_string(fpr) +
           ", t0\n";
}

std::unique_ptr<Kites::RVSSProcessor> loadProgram(const std::string &source,
                                                  const std::vector<uint32_t> &extraWords = {})
{
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<Kites::RVSSProcessor>();
    std::istringstream stream(source);
    Kites::AssembledProgram program = Kites::assemble(stream);
    for (uint32_t word : extraWords)
    {
        program.text_buffer.push_back(word);
    }
    vm->LoadProgram(program);
    return vm;
}

std::unique_ptr<Kites::RVSSProcessor> runProgram(const std::string &source,
                                                 const std::vector<uint32_t> &extraWords = {})
{
    auto vm = loadProgram(source, extraWords);
    vm->DebugRun();
    return vm;
}

float fprAsFloat(const Kites::RegisterFile &r, size_t index)
{
    return bitsToFloat(static_cast<uint32_t>(r.ReadFpr(index)));
}

} // namespace

TEST(LnsInstructions, ResetValueIsPositiveZero)
{
    auto vm = runProgram("addi x0, x0, 0\n");
    for (size_t i = 0; i < 32; ++i)
    {
        EXPECT_EQ(vm->registers_.ReadLns(i), lns::kPosZero) << "l" << i;
    }
}

TEST(LnsInstructions, ConvertFromAndToFloat)
{
    auto vm = runProgram(loadFloat(1, 2.0f) + loadFloat(2, 1.0f) + loadFloat(3, 0.5f) +
                         loadFloat(4, -2.0f) +
                         "fcvt.lns.s l1, f1\n"
                         "fcvt.lns.s l2, f2\n"
                         "fcvt.lns.s l3, f3\n"
                         "fcvt.lns.s l4, f4\n"
                         "fcvt.s.lns f11, l1\n"
                         "fcvt.s.lns f12, l2\n"
                         "fcvt.s.lns f13, l3\n"
                         "fcvt.s.lns f14, l4\n");
    auto &r = vm->registers_;

    // Powers of two are exact in both directions.
    EXPECT_EQ(r.ReadLns(1), lns::pack(false, 1 << lns::kFracBits));
    EXPECT_EQ(r.ReadLns(2), lns::pack(false, 0));
    EXPECT_EQ(r.ReadLns(3), lns::pack(false, -(1 << lns::kFracBits)));
    EXPECT_EQ(r.ReadLns(4), lns::pack(true, 1 << lns::kFracBits));

    // The FP32 result sits zero-extended in the low 32 bits of the FPR.
    EXPECT_EQ(r.ReadFpr(11), uint64_t{floatBits(2.0f)});
    EXPECT_EQ(r.ReadFpr(12), uint64_t{floatBits(1.0f)});
    EXPECT_EQ(r.ReadFpr(13), uint64_t{floatBits(0.5f)});
    EXPECT_EQ(r.ReadFpr(14), uint64_t{floatBits(-2.0f)});
}

TEST(LnsInstructions, ConvertMatchesConverterLibrary)
{
    const float values[] = {3.0f, 0.1f, 1234.5f, -7.25f, 1.0e-20f, 3.0e30f};
    std::string source;
    for (size_t i = 0; i < 6; ++i)
    {
        source += loadFloat(static_cast<int>(i + 1), values[i]);
    }
    for (size_t i = 0; i < 6; ++i)
    {
        source += "fcvt.lns.s l" + std::to_string(i + 1) + ", f" + std::to_string(i + 1) + "\n";
        source += "fcvt.s.lns f" + std::to_string(i + 11) + ", l" + std::to_string(i + 1) + "\n";
    }
    auto vm = runProgram(source);
    for (size_t i = 0; i < 6; ++i)
    {
        const lns::LnsWord expected = lns::defaultConverter().fromFloatBits(floatBits(values[i]));
        EXPECT_EQ(vm->registers_.ReadLns(i + 1), expected) << values[i];
        EXPECT_EQ(vm->registers_.ReadFpr(i + 11),
                  uint64_t{lns::defaultConverter().toFloatBits(expected)})
            << values[i];
    }
}

TEST(LnsInstructions, ArithmeticMatchesAluLibrary)
{
    auto vm = runProgram(loadFloat(1, 3.0f) + loadFloat(2, 0.5f) +
                         "fcvt.lns.s l1, f1\n"
                         "fcvt.lns.s l2, f2\n"
                         "ladd l3, l1, l2\n"
                         "lsub l4, l1, l2\n"
                         "lmul l5, l1, l2\n"
                         "ldiv l6, l1, l2\n"
                         "fcvt.s.lns f13, l3\n"
                         "fcvt.s.lns f14, l4\n"
                         "fcvt.s.lns f15, l5\n"
                         "fcvt.s.lns f16, l6\n");
    auto &r = vm->registers_;
    const lns::LnsWord a = lns::defaultConverter().fromFloatBits(floatBits(3.0f));
    const lns::LnsWord b = lns::defaultConverter().fromFloatBits(floatBits(0.5f));
    ASSERT_EQ(r.ReadLns(1), a);
    ASSERT_EQ(r.ReadLns(2), b);

    EXPECT_EQ(r.ReadLns(3), lns::defaultAlu().execute(lns::LnsAluOp::ADD, a, b));
    EXPECT_EQ(r.ReadLns(4), lns::defaultAlu().execute(lns::LnsAluOp::SUB, a, b));
    EXPECT_EQ(r.ReadLns(5), lns::defaultAlu().execute(lns::LnsAluOp::MUL, a, b));
    EXPECT_EQ(r.ReadLns(6), lns::defaultAlu().execute(lns::LnsAluOp::DIV, a, b));

    // ...and the numbers come out right (the LNS add/sub tables are approximate, hence 1e-4).
    EXPECT_NEAR(fprAsFloat(r, 13), 3.5f, 3.5f * 1e-4f);
    EXPECT_NEAR(fprAsFloat(r, 14), 2.5f, 2.5f * 1e-4f);
    EXPECT_NEAR(fprAsFloat(r, 15), 1.5f, 1.5f * 1e-4f);
    EXPECT_NEAR(fprAsFloat(r, 16), 6.0f, 6.0f * 1e-4f);
}

TEST(LnsInstructions, MultiplyAndDivideByPowersOfTwoAreExact)
{
    auto vm = runProgram(loadFloat(1, 2.0f) + loadFloat(2, 4.0f) +
                         "fcvt.lns.s l1, f1\n"
                         "fcvt.lns.s l2, f2\n"
                         "lmul l3, l1, l2\n"
                         "ldiv l4, l2, l1\n"
                         "fcvt.s.lns f13, l3\n"
                         "fcvt.s.lns f14, l4\n");
    EXPECT_EQ(vm->registers_.ReadFpr(13), uint64_t{floatBits(8.0f)});
    EXPECT_EQ(vm->registers_.ReadFpr(14), uint64_t{floatBits(2.0f)});
}

TEST(LnsInstructions, SpecialValuesFollowTheAlu)
{
    // l5 is untouched, so it is +0.
    auto vm = runProgram(loadFloat(1, 2.0f) +
                         "fcvt.lns.s l1, f1\n"
                         "ldiv l2, l1, l5\n"  // 2 / 0 = +inf
                         "lmul l3, l1, l5\n"  // 2 * 0 = +0
                         "ldiv l4, l5, l5\n"  // 0 / 0 = NaN
                         "ladd l6, l1, l5\n"  // 2 + 0 = 2
                         "lsub l7, l1, l1\n"); // 2 - 2 = 0
    auto &r = vm->registers_;
    const lns::LnsWord two = r.ReadLns(1);
    const lns::LnsWord zero = lns::kPosZero;
    const auto &alu = lns::defaultAlu();
    EXPECT_EQ(r.ReadLns(2), alu.execute(lns::LnsAluOp::DIV, two, zero));
    EXPECT_TRUE(lns::isInf(r.ReadLns(2)));
    EXPECT_EQ(r.ReadLns(3), alu.execute(lns::LnsAluOp::MUL, two, zero));
    EXPECT_TRUE(lns::isZero(r.ReadLns(3)));
    EXPECT_EQ(r.ReadLns(4), alu.execute(lns::LnsAluOp::DIV, zero, zero));
    EXPECT_TRUE(lns::isNaN(r.ReadLns(4)));
    EXPECT_EQ(r.ReadLns(6), alu.execute(lns::LnsAluOp::ADD, two, zero));
    EXPECT_EQ(r.ReadLns(7), alu.execute(lns::LnsAluOp::SUB, two, two));
    EXPECT_TRUE(lns::isZero(r.ReadLns(7)));
}

TEST(LnsInstructions, CordicAddSubInstructionsUseTheCordicHelper)
{
    auto vm = loadProgram("laddc l3, l1, l2\n"
                          "lsubc l4, l1, l2\n"
                          "laddc l0, l1, l1\n"
                          "lsubc l5, l1, l1\n"
                          "laddc l6, l1, l7\n"
                          "lsubc l8, l9, l10\n");
    vm->registers_.WriteLns(1, lns::pack(false, 0));
    vm->registers_.WriteLns(2, lns::pack(false, -(1 << lns::kFracBits)));
    vm->registers_.WriteLns(7, lns::kNaN);
    vm->registers_.WriteLns(9, lns::kPosInf);
    vm->registers_.WriteLns(10, lns::kPosInf);
    vm->DebugRun();

    EXPECT_EQ(vm->registers_.ReadLns(3),
              lns::LnsCordicAddSub::add(vm->registers_.ReadLns(1), vm->registers_.ReadLns(2)));
    EXPECT_EQ(vm->registers_.ReadLns(4),
              lns::LnsCordicAddSub::sub(vm->registers_.ReadLns(1), vm->registers_.ReadLns(2)));
    EXPECT_EQ(vm->registers_.ReadLns(0), lns::kPosZero);
    EXPECT_EQ(vm->registers_.ReadLns(5), lns::kPosZero); // exact cancellation
    EXPECT_TRUE(lns::isNaN(vm->registers_.ReadLns(6)));
    EXPECT_TRUE(lns::isNaN(vm->registers_.ReadLns(8))); // +inf - +inf
}

TEST(LnsInstructions, IntegerConvertsMatchConverterLibrary)
{
    // x5..x9 hold the inputs; li loads full 64-bit values.
    auto vm = runProgram("li x5, -7\n"
                         "li x6, -1\n"          // W: -1, WU: 4294967295, LU: 2^64 - 1
                         "li x7, -123456789012\n"
                         "li x8, 4294967301\n"  // 2^32 + 5: the W/WU forms read the low 32 bits
                         "li x9, 1000\n"
                         "fcvt.lns.w  l1, x5\n"
                         "fcvt.lns.wu l2, x6\n"
                         "fcvt.lns.l  l3, x7\n"
                         "fcvt.lns.lu l4, x6\n"
                         "fcvt.lns.w  l5, x8\n"
                         "fcvt.lns.w  l6, x6\n"
                         "fcvt.lns.l  l7, x9\n"
                         "fcvt.w.lns  x10, l1\n"
                         "fcvt.wu.lns x11, l2\n"
                         "fcvt.l.lns  x12, l3\n"
                         "fcvt.lu.lns x13, l4\n"
                         "fcvt.w.lns  x14, l5\n"
                         "fcvt.l.lns  x15, l7\n");
    auto &r = vm->registers_;
    const lns::LnsConverter &c = lns::defaultConverter();

    EXPECT_EQ(r.ReadLns(1), c.fromInt32(-7));
    EXPECT_EQ(r.ReadLns(2), c.fromUInt32(4294967295u));
    EXPECT_EQ(r.ReadLns(3), c.fromInt64(-123456789012LL));
    EXPECT_EQ(r.ReadLns(4), c.fromUInt64(UINT64_MAX));
    EXPECT_EQ(r.ReadLns(5), c.fromInt32(5));
    EXPECT_EQ(r.ReadLns(6), c.fromInt32(-1));
    EXPECT_EQ(r.ReadLns(6), lns::pack(true, 0)); // -1 is exact: sign set, log 0
    EXPECT_EQ(r.ReadLns(7), c.fromInt64(1000));

    // 32-bit results are sign-extended into the 64-bit register, as RISC-V fcvt.w(u) does.
    EXPECT_EQ(r.ReadGpr(10), static_cast<uint64_t>(static_cast<int64_t>(c.toInt32(r.ReadLns(1)))));
    EXPECT_EQ(r.ReadGpr(11), static_cast<uint64_t>(static_cast<int64_t>(
                                 static_cast<int32_t>(c.toUInt32(r.ReadLns(2))))));
    EXPECT_EQ(r.ReadGpr(12), static_cast<uint64_t>(c.toInt64(r.ReadLns(3))));
    EXPECT_EQ(r.ReadGpr(13), c.toUInt64(r.ReadLns(4)));
    EXPECT_EQ(r.ReadGpr(14), static_cast<uint64_t>(static_cast<int64_t>(c.toInt32(r.ReadLns(5)))));

    // Small integers survive the round trip exactly.
    EXPECT_EQ(static_cast<int64_t>(r.ReadGpr(10)), -7);
    EXPECT_EQ(r.ReadGpr(14), 5u);
    EXPECT_EQ(r.ReadGpr(15), 1000u);
}

TEST(LnsInstructions, IntegerConvertsSaturateLikeRiscV)
{
    auto vm = runProgram(loadFloat(1, 3.0e30f) + loadFloat(2, -2.0f) + loadFloat(3, 3.0e9f) +
                         "li t0, 0x7FC00000\n"
                         "fmv.w.x f4, t0\n" // FP32 NaN
                         "fcvt.lns.s l1, f1\n"
                         "fcvt.lns.s l2, f2\n"
                         "fcvt.lns.s l3, f3\n"
                         "fcvt.lns.s l4, f4\n"
                         "li x20, 99\n"
                         "fcvt.w.lns  x10, l1\n"  // too large -> INT32_MAX
                         "fcvt.w.lns  x11, l2\n"  // -2
                         "fcvt.wu.lns x12, l2\n"  // negative -> unsigned: 0
                         "fcvt.wu.lns x13, l3\n"  // 3e9 fits in 32 bits unsigned
                         "fcvt.w.lns  x14, l4\n"  // NaN -> INT32_MAX
                         "fcvt.l.lns  x15, l4\n"  // NaN -> INT64_MAX
                         "fcvt.l.lns  x16, l1\n"  // 3e30 -> INT64_MAX
                         "fcvt.lu.lns x17, l2\n"  // negative -> 0
                         "fcvt.w.lns  x0, l1\n"   // x0 stays 0
                         "fcvt.w.lns  x20, lzero\n"); // +0 -> 0
    auto &r = vm->registers_;
    EXPECT_EQ(r.ReadGpr(10), 0x7FFFFFFFu);
    EXPECT_EQ(static_cast<int64_t>(r.ReadGpr(11)), -2);
    EXPECT_EQ(r.ReadGpr(12), 0u);
    EXPECT_EQ(r.ReadGpr(13), static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(
                                 lns::defaultConverter().toUInt32(r.ReadLns(3))))));
    EXPECT_EQ(r.ReadGpr(13) >> 32, 0xFFFFFFFFu); // bit 31 set, so sign-extended
    EXPECT_EQ(r.ReadGpr(14), 0x7FFFFFFFu);
    EXPECT_EQ(r.ReadGpr(15), 0x7FFFFFFFFFFFFFFFull);
    EXPECT_EQ(r.ReadGpr(16), 0x7FFFFFFFFFFFFFFFull);
    EXPECT_EQ(r.ReadGpr(17), 0u);
    EXPECT_EQ(r.ReadGpr(0), 0u);
    EXPECT_EQ(r.ReadGpr(20), 0u);
}

TEST(LnsInstructions, UndoRestoresIntegerConvertResults)
{
    auto vm = loadProgram("li x5, 12\n"
                          "fcvt.lns.w l1, x5\n"
                          "fcvt.w.lns x6, l1\n");
    while (vm->program_counter_ < vm->program_size_)
    {
        vm->Step();
    }
    auto &r = vm->registers_;
    ASSERT_EQ(r.ReadGpr(6), 12u);
    ASSERT_EQ(r.ReadLns(1), lns::defaultConverter().fromInt32(12));

    vm->Undo(); // fcvt.w.lns x6
    EXPECT_EQ(r.ReadGpr(6), 0u);
    vm->Undo(); // fcvt.lns.w l1
    EXPECT_EQ(r.ReadLns(1), lns::kPosZero);
    vm->Redo();
    vm->Redo();
    EXPECT_EQ(r.ReadGpr(6), 12u);
}

TEST(LnsInstructions, IntegerConvertMachineCodeAndDisassembly)
{
    using Kites::instruction_set::disassemble;
    using Kites::instruction_set::isWellFormedLnsInstruction;

    Kites::setupVmStateDirectory();
    std::istringstream stream("fcvt.lns.w l5, x6\n"
                              "fcvt.lns.wu l5, x6\n"
                              "fcvt.lns.l l5, x6\n"
                              "fcvt.lns.lu l5, x6\n"
                              "fcvt.w.lns x5, l6\n"
                              "fcvt.wu.lns x5, l6\n"
                              "fcvt.l.lns x5, l6\n"
                              "fcvt.lu.lns x5, l6\n"
                              "fcvt.lns.w la0, a1\n");
    const Kites::AssembledProgram program = Kites::assemble(stream);
    // funct7 = 0000001, rs2 = integer type (0 W, 1 WU, 2 L, 3 LU), funct3 = 100 / 101
    const std::vector<uint32_t> expected = {0x0203428B, 0x0213428B, 0x0223428B,
                                            0x0233428B, 0x0203528B, 0x0213528B,
                                            0x0223528B, 0x0233528B, 0x0205C50B};
    const std::vector<std::string> disassembly = {
        "fcvt.lns.w l5, x6", "fcvt.lns.wu l5, x6", "fcvt.lns.l l5, x6",
        "fcvt.lns.lu l5, x6", "fcvt.w.lns x5, l6", "fcvt.wu.lns x5, l6",
        "fcvt.l.lns x5, l6", "fcvt.lu.lns x5, l6", "fcvt.lns.w l10, x11",
    };
    ASSERT_EQ(program.text_buffer.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(program.text_buffer[i], expected[i]) << i;
        EXPECT_TRUE(isWellFormedLnsInstruction(program.text_buffer[i])) << i;
        EXPECT_EQ(disassemble(program.text_buffer[i]), disassembly[i]) << i;
    }
    EXPECT_FALSE(isWellFormedLnsInstruction(0x0243428Bu)); // funct7 = 1 with rs2 = 4
    EXPECT_FALSE(isWellFormedLnsInstruction(0x0203228Bu)); // funct7 = 1 with funct3 = 010
    EXPECT_FALSE(isWellFormedLnsInstruction(0x0403428Bu)); // funct7 = 2 is reserved
}

TEST(LnsInstructions, L0IsHardWiredToPositiveZero)
{
    auto vm = runProgram(loadFloat(1, 2.0f) +
                         "fcvt.lns.s l1, f1\n"
                         "fcvt.lns.s l0, f1\n" // ignored
                         "ladd l0, l1, l1\n"   // ignored
                         "lmul l2, l1, l0\n"   // 2 * (+0) = +0
                         "ladd l3, l1, l0\n"   // 2 + 0 = 2
                         "lmul lzero, l1, l1\n"); // ABI name of l0, also ignored
    auto &r = vm->registers_;
    EXPECT_EQ(r.ReadLns(0), lns::kPosZero);
    EXPECT_TRUE(lns::isZero(r.ReadLns(2)));
    EXPECT_EQ(r.ReadLns(3), r.ReadLns(1));
}

TEST(LnsInstructions, AbiNamesAddressTheSameRegisters)
{
    auto vm = runProgram(loadFloat(1, 2.0f) +
                         "fcvt.lns.s la0, f1\n"   // l10
                         "fcvt.lns.s ls0, f1\n"   // l8
                         "fcvt.lns.s lt10, f1\n"  // l31
                         "fcvt.lns.s ls11, f1\n"  // l27
                         "ladd lt0, la0, ls0\n"); // l1
    auto &r = vm->registers_;
    const lns::LnsWord two = lns::pack(false, 1 << lns::kFracBits);
    EXPECT_EQ(r.ReadLns(10), two);
    EXPECT_EQ(r.ReadLns(8), two);
    EXPECT_EQ(r.ReadLns(31), two);
    EXPECT_EQ(r.ReadLns(27), two);
    EXPECT_EQ(r.ReadLns(1), lns::defaultAlu().execute(lns::LnsAluOp::ADD, two, two));
}

TEST(LnsInstructions, StoreAndLoadRawWords)
{
    auto vm = runProgram(".data\n"
                         "buf: .word 0, 0, 0, 0\n"
                         ".text\n"
                         "lui s0, 0x10000\n" +
                         loadFloat(1, -2.0f) +
                         "fcvt.lns.s l1, f1\n"
                         "lsw l1, 4(s0)\n"
                         "llw l2, 4(s0)\n"
                         "addi s1, s0, 12\n"
                         "lsw l1, -4(s1)\n" // negative offset: 12 - 4 = 8
                         "lwu a0, 4(s0)\n"
                         "lwu a1, 8(s0)\n"
                         "lw a2, 0(s0)\n" // untouched neighbour
                         "lw a3, 12(s0)\n");
    auto &r = vm->registers_;
    const lns::LnsWord minusTwo = lns::pack(true, 1 << lns::kFracBits); // 0x80400000
    EXPECT_EQ(r.ReadLns(1), minusTwo);
    EXPECT_EQ(r.ReadLns(2), minusTwo);
    EXPECT_EQ(r.ReadGpr(10), uint64_t{minusTwo});
    EXPECT_EQ(r.ReadGpr(11), uint64_t{minusTwo});
    EXPECT_EQ(r.ReadGpr(12), 0u);
    EXPECT_EQ(r.ReadGpr(13), 0u);
    EXPECT_EQ(vm->memory_controller_.readWord(kDataBase + 4), minusTwo);
}

TEST(LnsInstructions, LoadsAndStoresMoveWordsWithoutCanonicalizing)
{
    // 0x12345678 is not a canonical LNS word; llw/lsw must carry it through untouched.
    auto vm = runProgram(".data\n"
                         "buf: .word 0, 0, 0, 0\n"
                         ".text\n"
                         "lui s0, 0x10000\n"
                         "lui t1, 0x12345\n"
                         "addi t1, t1, 0x678\n"
                         "sw t1, 0(s0)\n"
                         "llw l3, 0(s0)\n"
                         "lsw l3, 8(s0)\n"
                         "lwu a0, 8(s0)\n");
    EXPECT_EQ(vm->registers_.ReadLns(3), 0x12345678u);
    EXPECT_EQ(vm->registers_.ReadGpr(10), 0x12345678u);
}

TEST(LnsInstructions, LoadIntoL0IsIgnored)
{
    auto vm = runProgram(".data\n"
                         "buf: .word 0, 0, 0, 0\n"
                         ".text\n"
                         "lui s0, 0x10000\n"
                         "addi t1, x0, 77\n"
                         "sw t1, 0(s0)\n"
                         "llw l0, 0(s0)\n");
    EXPECT_EQ(vm->registers_.ReadLns(0), lns::kPosZero);
}

TEST(LnsInstructions, ReservedEncodingsAreNoOps)
{
    // Hand-built custom-0 words appended after a normal program. Each would overwrite a register
    // that the program has already set if it were executed:
    //   0x0431008B : ladd l1, l2, l3 with funct7 = 2       (would set l1 to +0)
    //   0x0013428B : fcvt.lns.s l5, f6 with rs2 = 1        (would set l5 to +0, f6 is 0.0)
    //   0x0013528B : fcvt.s.lns f5, l6 with rs2 = 1        (would set f5 to 0.0, l6 is +0)
    //   0x0081628B : old custom-0 llw l5, 8(x2) slot      (would load a word into l5)
    //   0x008132AB : custom-1 (llw opcode) with funct3 = 011
    auto vm = runProgram(loadFloat(1, 2.0f) + loadFloat(5, 3.0f) +
                             "fcvt.lns.s l1, f1\n"
                             "fcvt.lns.s l5, f1\n",
                         {0x0431008Bu, 0x0013428Bu, 0x0013528Bu, 0x0081628Bu, 0x008132ABu});
    auto &r = vm->registers_;
    const lns::LnsWord two = lns::pack(false, 1 << lns::kFracBits);
    EXPECT_EQ(r.ReadLns(1), two);
    EXPECT_EQ(r.ReadLns(5), two);
    EXPECT_EQ(r.ReadFpr(5), uint64_t{floatBits(3.0f)});
}

TEST(LnsInstructions, UndoAndRedoRestoreLnsRegisters)
{
    auto vm = loadProgram(loadFloat(1, 2.0f) +
                          "fcvt.lns.s l1, f1\n"
                          "fcvt.s.lns f2, l1\n"
                          "ladd l3, l1, l1\n");
    while (vm->program_counter_ < vm->program_size_)
    {
        vm->Step();
    }
    auto &r = vm->registers_;
    const lns::LnsWord two = lns::pack(false, 1 << lns::kFracBits);
    const lns::LnsWord four = r.ReadLns(3);
    ASSERT_EQ(r.ReadLns(1), two);
    ASSERT_EQ(r.ReadFpr(2), uint64_t{floatBits(2.0f)});
    ASSERT_NE(four, lns::kPosZero);

    vm->Undo(); // ladd l3
    EXPECT_EQ(r.ReadLns(3), lns::kPosZero);
    EXPECT_EQ(r.ReadLns(1), two);

    vm->Undo(); // fcvt.s.lns f2
    EXPECT_EQ(r.ReadFpr(2), 0u);
    EXPECT_EQ(r.ReadLns(1), two);

    vm->Undo(); // fcvt.lns.s l1
    EXPECT_EQ(r.ReadLns(1), lns::kPosZero);

    vm->Redo();
    EXPECT_EQ(r.ReadLns(1), two);
    vm->Redo();
    EXPECT_EQ(r.ReadFpr(2), uint64_t{floatBits(2.0f)});
    vm->Redo();
    EXPECT_EQ(r.ReadLns(3), four);
}

TEST(LnsInstructions, UndoRestoresStoredMemory)
{
    auto vm = loadProgram(".data\n"
                          "buf: .word 0, 0, 0, 0\n"
                          ".text\n"
                          "lui s0, 0x10000\n" +
                          loadFloat(1, 2.0f) +
                          "fcvt.lns.s l1, f1\n"
                          "lsw l1, 0(s0)\n");
    while (vm->program_counter_ < vm->program_size_)
    {
        vm->Step();
    }
    const lns::LnsWord two = lns::pack(false, 1 << lns::kFracBits);
    EXPECT_EQ(vm->memory_controller_.readWord(kDataBase), two);
    vm->Undo();
    EXPECT_EQ(vm->memory_controller_.readWord(kDataBase), 0u);
    vm->Redo();
    EXPECT_EQ(vm->memory_controller_.readWord(kDataBase), two);
}

TEST(LnsInstructions, RegisterFileBank)
{
    Kites::RegisterFile regs;
    regs.Reset();
    regs.WriteLns(0, 0x12345678);
    EXPECT_EQ(regs.ReadLns(0), lns::kPosZero);
    regs.WriteLns(31, 0xDEADBEEF);
    EXPECT_EQ(regs.ReadLns(31), 0xDEADBEEFu);
    EXPECT_THROW((void)regs.ReadLns(32), std::out_of_range);
    EXPECT_THROW(regs.WriteLns(32, 0), std::out_of_range);

    EXPECT_EQ(Kites::LnsRegisterIndex("la0"), 10);
    EXPECT_EQ(Kites::LnsRegisterIndex("lt10"), 31);
    EXPECT_EQ(Kites::LnsRegisterIndex("lzero"), 0);
    EXPECT_EQ(Kites::LnsRegisterIndex("l32"), -1);
    EXPECT_EQ(Kites::LnsRegisterIndex("x5"), -1);
    EXPECT_EQ(Kites::LnsRegisterAbiName(10), "la0");
    EXPECT_EQ(Kites::LnsRegisterName(10), "l10");

    regs.Reset();
    EXPECT_EQ(regs.ReadLns(31), lns::kPosZero);
}

TEST(LnsInstructions, MachineCodeAndDisassembly)
{
    using Kites::instruction_set::disassemble;
    using Kites::instruction_set::isLnsInstruction;

    Kites::setupVmStateDirectory();
    std::istringstream stream("ladd l1, l2, l3\n"
                              "lsub l1, l2, l3\n"
                              "lmul l1, l2, l3\n"
                              "ldiv l1, l2, l3\n"
                              "laddc l1, l2, l3\n"
                              "lsubc l1, l2, l3\n"
                              "fcvt.lns.s l5, f6\n"
                              "fcvt.s.lns f5, l6\n"
                              "llw l5, 8(x2)\n"
                              "lsw l5, 8(x2)\n"
                              "llw l5, -4(x2)\n"
                              "lsw l5, -4(x2)\n"
                              "ladd la0, la1, ls2\n");
    const Kites::AssembledProgram program = Kites::assemble(stream);
    const std::vector<uint32_t> expected = {
        0x0031008B, 0x0031108B, 0x0031208B, 0x0031308B, 0x0231008B, 0x0231108B,
        0x0003428B, 0x0003528B,
        0x008122AB, 0x0051245B, 0xFFC122AB, 0xFE512E5B, (18u << 20) | (11u << 15) | (10u << 7) | 0x0B,
    };
    const std::vector<std::string> disassembly = {
        "ladd l1, l2, l3",    "lsub l1, l2, l3",    "lmul l1, l2, l3",
        "ldiv l1, l2, l3",    "laddc l1, l2, l3",   "lsubc l1, l2, l3",
        "fcvt.lns.s l5, f6",  "fcvt.s.lns f5, l6",  "llw l5, 8(x2)",
        "lsw l5, 8(x2)",      "llw l5, -4(x2)",     "lsw l5, -4(x2)",
        "ladd l10, l11, l18",
    };
    ASSERT_EQ(program.text_buffer.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(program.text_buffer[i], expected[i]) << i;
        EXPECT_TRUE(isLnsInstruction(program.text_buffer[i])) << i;
        EXPECT_EQ(disassemble(program.text_buffer[i]), disassembly[i]) << i;
    }
    EXPECT_TRUE(Kites::instruction_set::isWellFormedLnsInstruction(0x0231008Bu));
    EXPECT_TRUE(Kites::instruction_set::isWellFormedLnsInstruction(0x0231108Bu));
    EXPECT_FALSE(Kites::instruction_set::isWellFormedLnsInstruction(0x0431008Bu));
    // llw / lsw live on custom-1 / custom-2 with funct3 = 010; the old custom-0 funct3 110 / 111
    // slots and other funct3 values on the new opcodes are reserved.
    EXPECT_FALSE(Kites::instruction_set::isWellFormedLnsInstruction(0x0081628Bu));
    EXPECT_FALSE(Kites::instruction_set::isWellFormedLnsInstruction(0x0051740Bu));
    EXPECT_FALSE(Kites::instruction_set::isWellFormedLnsInstruction(0x008132ABu));
    EXPECT_FALSE(Kites::instruction_set::isWellFormedLnsInstruction(0x0051345Bu));
}
