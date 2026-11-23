/**
 * @file lns_cbrt_kernel_test.cpp
 * @brief The L1 cube-root kernel of examples/lns_int_cbrt (fcvt.lns.lu -> lpow(x, 1/3) ->
 *        fcvt.lu.lns -> integer fix-up) on RVSS, for hand-picked edge inputs.
 *
 * Each input is assembled into its own small program (same instruction sequence as
 * cbrt_lns_exact.s, minus the loop) and run in-process. Checks: the result is the exact floor
 * cube root, and the pre-fix-up candidate equals the LnsConverter / power() library result.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

#include "assembler/assembler.h"
#include "processor/lns/lns_alu.h"
#include "processor/lns/lns_conversion.h"
#include "processor/lns/lns_pow.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

namespace
{
namespace lns = Kites::lns;
using u64 = uint64_t;
using u128 = unsigned __int128;

constexpr u64 kCap = 2642245; // floor(cbrt(2^64 - 1))

u64 floorCbrt(u64 a)
{
    u64 lo = 0, hi = kCap;
    while (lo < hi)
    { // largest x with x^3 <= a
        const u64 mid = (lo + hi + 1) / 2;
        if (static_cast<u128>(mid) * mid * mid <= a)
        {
            lo = mid;
        }
        else
        {
            hi = mid - 1;
        }
    }
    return lo;
}

std::string hex(u64 v)
{
    std::ostringstream s;
    s << "0x" << std::hex << v;
    return s.str();
}

/// L1 kernel for one input: a0 = exact floor cube root, a1 = raw LNS candidate.
std::string l1Program(u64 a)
{
    return "li s4, " + hex(a) + "\n" + "li s7, " + std::to_string(kCap) + "\n" +
           "li t0, 1\n"
           "fcvt.lns.w l10, t0\n"
           "li t0, 3\n"
           "fcvt.lns.w l11, t0\n"
           "ldiv l12, l10, l11\n"
           "fcvt.lns.lu l1, s4\n"
           "lpow l2, l1, l12\n"
           "fcvt.lu.lns s5, l2\n"
           "mv a1, s5\n"
           "bgeu s7, s5, fx_down\n"
           "mv s5, s7\n"
           "fx_down:\n"
           "mul t0, s5, s5\n"
           "mul t0, t0, s5\n"
           "bltu s4, t0, fx_dec\n"
           "fx_up:\n"
           "beq s5, s7, fx_done\n"
           "addi t1, s5, 1\n"
           "mul t0, t1, t1\n"
           "mul t0, t0, t1\n"
           "bltu s4, t0, fx_done\n"
           "mv s5, t1\n"
           "j fx_up\n"
           "fx_dec:\n"
           "addi s5, s5, -1\n"
           "mul t0, s5, s5\n"
           "mul t0, t0, s5\n"
           "bltu s4, t0, fx_dec\n"
           "fx_done:\n"
           "mv a0, s5\n";
}

std::unique_ptr<Kites::RVSSProcessor> run(const std::string &source)
{
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<Kites::RVSSProcessor>();
    std::istringstream stream(source);
    Kites::AssembledProgram program = Kites::assemble(stream);
    vm->LoadProgram(program);
    vm->DebugRun();
    return vm;
}

class LnsCbrtKernel : public ::testing::TestWithParam<u64>
{
};

TEST_P(LnsCbrtKernel, ExactFloorAndLibraryCandidate)
{
    const u64 a = GetParam();
    auto vm = run(l1Program(a));
    const auto &r = vm->registers_;
    EXPECT_EQ(r.ReadGpr(10), floorCbrt(a)) << "a = " << a;

    const lns::LnsConverter &c = lns::defaultConverter();
    const lns::LnsWord third =
        lns::defaultAlu().execute(lns::LnsAluOp::DIV, c.fromInt32(1), c.fromInt32(3));
    EXPECT_EQ(r.ReadGpr(11), c.toUInt64(lns::power(c.fromUInt64(a), third, c))) << "a = " << a;
}

constexpr u64 kCapCube = kCap * kCap * kCap;

INSTANTIATE_TEST_SUITE_P(EdgeInputs, LnsCbrtKernel,
                         ::testing::Values(u64{0}, u64{1}, u64{2}, u64{7}, u64{8}, u64{9},
                                           u64{26}, u64{27}, u64{63}, u64{64}, u64{1} << 21,
                                           (u64{1} << 22) - 1, u64{1} << 22,
                                           (u64{1} << 22) + 1, u64{0xFFFFFFFF},
                                           u64{1} << 32, kCapCube - 1, kCapCube, kCapCube + 1,
                                           u64{1} << 63, ~u64{0}));
} // namespace
