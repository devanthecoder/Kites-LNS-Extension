/**
 * @file lns_instruction_test.cpp
 * @brief The LNS extension on the single-cycle RVSS processor: ladd/lsub/lmul/ldiv,
 *        fcvt.lns.s / fcvt.s.lns, llw/lsw, the L register bank (l0 hard-wired to +0, reset
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
    auto vm = runProgram(loadFloat(1, 2.0f) + loadFloat(5, 3.0f) +
                             "fcvt.lns.s l1, f1\n"
                             "fcvt.lns.s l5, f1\n",
                         {0x0431008Bu, 0x0013428Bu, 0x0013528Bu});
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
        0x0081628B, 0x0051740B, 0xFFC1628B, 0xFE517E0B, (18u << 20) | (11u << 15) | (10u << 7) | 0x0B,
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
}
