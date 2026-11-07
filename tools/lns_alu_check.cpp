// Standalone checks + accuracy/size sweep for the LNS ALU, using the converter for I/O (no Qt).
//
// Build (from the repo root, one command line):
//   g++ -std=c++20 -O2 -I src/processor/lns tools/lns_alu_check.cpp src/processor/lns/lns_alu.cpp
//       src/processor/lns/lns_conversion.cpp -o lns_alu_check
// Run:  ./lns_alu_check            (add "quick" to skip the table-size sweep)
#include "lns_alu.h"
#include "lns_conversion.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace Kites::lns;

static int failures = 0;
#define CHECK(c)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(c))                                                                                  \
        {                                                                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                                       \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static uint32_t asBits(float f) { return std::bit_cast<uint32_t>(f); }
static float asFloat(uint32_t b) { return std::bit_cast<float>(b); }
static LnsWord L(const LnsConverter &c, float f) { return c.fromFloatBits(asBits(f)); }

// ---------------------------------------------------------------------------
static void checkSpecials(const LnsAlu &alu, const LnsConverter &c)
{
    const LnsWord two = L(c, 2.0f), three = L(c, 3.0f), mTwo = L(c, -2.0f);
    const LnsWord zero = kPosZero, nan = kNaN, pInf = kPosInf, nInf = kNegInf;

    // NaN propagates through everything
    for (LnsAluOp op : {LnsAluOp::ADD, LnsAluOp::SUB, LnsAluOp::MUL, LnsAluOp::DIV})
    {
        CHECK(alu.execute(op, nan, two) == kNaN);
        CHECK(alu.execute(op, two, nan) == kNaN);
    }
    // add / sub
    CHECK(alu.add(pInf, pInf) == kPosInf);
    CHECK(alu.add(pInf, nInf) == kNaN);
    CHECK(alu.sub(pInf, pInf) == kNaN);
    CHECK(alu.sub(pInf, nInf) == kPosInf);
    CHECK(alu.add(pInf, two) == kPosInf);
    CHECK(alu.add(two, nInf) == kNegInf);
    CHECK(alu.sub(two, pInf) == kNegInf);
    CHECK(alu.add(zero, two) == two);
    CHECK(alu.add(two, zero) == two);
    CHECK(alu.sub(zero, two) == mTwo);
    CHECK(alu.add(zero, zero) == kPosZero);
    CHECK(alu.sub(two, two) == kPosZero);        // exact cancellation
    CHECK(alu.add(two, mTwo) == kPosZero);
    // mul
    CHECK(alu.mul(zero, pInf) == kNaN);
    CHECK(alu.mul(nInf, zero) == kNaN);
    CHECK(alu.mul(pInf, mTwo) == kNegInf);
    CHECK(alu.mul(nInf, mTwo) == kPosInf);
    CHECK(alu.mul(zero, two) == kPosZero);
    CHECK(alu.mul(mTwo, zero) == kPosZero);
    // div
    CHECK(alu.div(zero, zero) == kNaN);
    CHECK(alu.div(pInf, nInf) == kNaN);
    CHECK(alu.div(two, zero) == kPosInf);
    CHECK(alu.div(mTwo, zero) == kNegInf);
    CHECK(alu.div(pInf, two) == kPosInf);
    CHECK(alu.div(zero, two) == kPosZero);
    CHECK(alu.div(two, pInf) == kPosZero);
    // zero is read from either sign; NaN from either sign
    CHECK(alu.add(0xC0000000u, two) == two);
    CHECK(alu.mul(0xBFC00001u, two) == kNaN);
    // exact cases
    CHECK(alu.mul(two, three) == L(c, 6.0f));
    CHECK(alu.div(L(c, 8.0f), two) == L(c, 4.0f));
    CHECK(alu.mul(mTwo, mTwo) == L(c, 4.0f));
    CHECK(alu.add(two, two) == L(c, 4.0f)); // difference == 0 path: exactly one octave
    // range: results at/above the Inf threshold become Inf, below the finite limit become 0
    const LnsWord big = pack(false, kLqFiniteMax - 10), small = pack(false, kLqFiniteMin + 10);
    CHECK(alu.mul(big, big) == kPosInf);
    CHECK(alu.mul(small, small) == kPosZero);
    CHECK(alu.div(big, small) == kPosInf);
    CHECK(alu.div(small, big) == kPosZero);
}

static void checkConstructor()
{
    bool threw = false;
    try { LnsAlu bad(1); } catch (const std::invalid_argument &) { threw = true; }
    CHECK(threw);
    threw = false;
    try { LnsAlu bad(LnsAlu::kMaxCorrectionSamples + 1); } catch (const std::invalid_argument &) { threw = true; }
    CHECK(threw);
    LnsAlu a(2); // smallest allowed: endpoints only
    CHECK(a.correctionEntries() >= 2);
    CHECK(a.correctionBytes() == a.correctionEntries() * 24);
    // phi vanishes at and beyond the cutoff, for every table size
    CHECK(a.correction(LnsAlu::kCorrectionCutoff, true) == 0);
    CHECK(a.correction(LnsAlu::kCorrectionCutoff + 12345, false) == 0);
    LnsAlu def;
    CHECK(def.requestedSamples() == static_cast<size_t>(LNS_ALU_CORRECTION_SAMPLES));
}

// ---------------------------------------------------------------------------
// Op-level accuracy vs double arithmetic on FP32 operands
// ---------------------------------------------------------------------------
static uint64_t rngState = 0x2545F4914F6CDD1Dull;
static double uni() // [0,1)
{
    rngState ^= rngState << 13; rngState ^= rngState >> 7; rngState ^= rngState << 17;
    return static_cast<double>(rngState >> 11) / 9007199254740992.0;
}

struct OpErr { double mulMax = 0, divMax = 0, addMax = 0, subMax = 0; };

static OpErr measureOps(const LnsAlu &alu, const LnsConverter &c, int n)
{
    OpErr e;
    for (int i = 0; i < n; ++i)
    {
        float x = static_cast<float>(std::exp((uni() * 2 - 1) * 6.9)); // ~1e-3 .. 1e3
        float y = static_cast<float>(std::exp((uni() * 2 - 1) * 6.9));
        if (uni() < 0.5) x = -x;
        if (uni() < 0.5) y = -y;
        LnsWord wx = L(c, x), wy = L(c, y);
        double dx = x, dy = y;
        auto relErr = [&](LnsWord w, double exact, double scale) {
            double got = asFloat(c.toFloatBits(w));
            return std::fabs(got - exact) / scale;
        };
        e.mulMax = std::max(e.mulMax, relErr(alu.mul(wx, wy), dx * dy, std::fabs(dx * dy)));
        e.divMax = std::max(e.divMax, relErr(alu.div(wx, wy), dx / dy, std::fabs(dx / dy)));
        // add/sub: relative to the larger operand, so near-cancellation is judged fairly
        double scale = std::max(std::fabs(dx), std::fabs(dy));
        e.addMax = std::max(e.addMax, relErr(alu.add(wx, wy), dx + dy, scale));
        e.subMax = std::max(e.subMax, relErr(alu.sub(wx, wy), dx - dy, scale));
    }
    return e;
}

// ---------------------------------------------------------------------------
// Correction-table accuracy (in Q22 LSBs) and size
// ---------------------------------------------------------------------------
struct TabErr { double addMax = 0, addMean = 0, subMax = 0, subMean = 0; };

static std::vector<int64_t> makeTestDifferences()
{
    std::vector<int64_t> ds;
    for (int64_t d = 1; d < 2000; ++d) ds.push_back(d);
    const double lo = 0.0, hi = std::log(static_cast<double>(LnsAlu::kCorrectionCutoff));
    for (int i = 0; i < 300000; ++i)
    {
        int64_t d = static_cast<int64_t>(std::exp(lo + uni() * (hi - lo)));
        if (d >= 1 && d < LnsAlu::kCorrectionCutoff) ds.push_back(d);
    }
    return ds;
}

static TabErr measureTable(const LnsAlu &alu, const std::vector<int64_t> &ds)
{
    TabErr t;
    double sa = 0, ss = 0;
    for (int64_t d : ds)
    {
        const double x = static_cast<double>(d) / kScale;
        const double exactAdd = std::log2(1.0 + std::exp2(-x)) * kScale;
        const double exactSub = std::log2(-std::expm1(-x * std::log(2.0))) * kScale;
        const double ea = std::fabs(static_cast<double>(alu.correction(d, true)) - exactAdd);
        const double es = std::fabs(static_cast<double>(alu.correction(d, false)) - exactSub);
        t.addMax = std::max(t.addMax, ea);
        t.subMax = std::max(t.subMax, es);
        sa += ea; ss += es;
    }
    t.addMean = sa / static_cast<double>(ds.size());
    t.subMean = ss / static_cast<double>(ds.size());
    return t;
}

int main(int argc, char **argv)
{
    const bool quick = argc > 1 && std::strcmp(argv[1], "quick") == 0;
    const LnsConverter converter(11, 11);

    checkConstructor();
    checkSpecials(defaultAlu(), converter);

    std::printf("default ALU: %d requested samples -> %zu stored entries, %.1f KiB\n\n",
                static_cast<int>(LNS_ALU_CORRECTION_SAMPLES), defaultAlu().correctionEntries(),
                static_cast<double>(defaultAlu().correctionBytes()) / 1024.0);

    OpErr e = measureOps(defaultAlu(), converter, 300000);
    std::printf("op-level max relative error vs double (FP32 operands via the converter):\n"
                "  mul %.3e  div %.3e  add %.3e  sub %.3e   (add/sub relative to the larger operand)\n"
                "  FP32 round-trip floor of the format itself: ~1.2e-7\n\n",
                e.mulMax, e.divMax, e.addMax, e.subMax);
    // Regression guards. They document today's accuracy and fail if it gets worse.
    CHECK(e.mulMax < 4e-7);
    CHECK(e.divMax < 4e-7);
    CHECK(e.addMax < 3e-6);
    CHECK(e.subMax < 2e-5);

    const std::vector<int64_t> ds = makeTestDifferences();
    TabErr def = measureTable(defaultAlu(), ds);
    CHECK(def.addMax < 10.0);
    CHECK(def.subMax < 70.0);

    auto header = [] {
        std::printf("%9s %8s %9s | %9s %9s | %9s %9s\n", "requested", "entries", "table(KiB)",
                    "add max", "add mean", "sub max", "sub mean");
        std::printf("%9s %8s %9s | %9s %9s | %9s %9s\n", "", "", "", "(LSB)", "(LSB)", "(LSB)", "(LSB)");
    };
    auto row = [](const LnsAlu &a, const TabErr &t) {
        std::printf("%9zu %8zu %9.1f | %9.2f %9.3f | %9.2f %9.3f\n", a.requestedSamples(),
                    a.correctionEntries(), static_cast<double>(a.correctionBytes()) / 1024.0, t.addMax,
                    t.addMean, t.subMax, t.subMean);
    };

    std::printf("correction-table error in Q22 LSBs (1 LSB = 2.38e-7 in log2 units):\n");
    header();
    row(defaultAlu(), def);

    if (!quick)
    {
        std::printf("\nsweep (log-spaced samples in d, linear interpolation):\n");
        header();
        for (size_t n : {256u, 512u, 1024u, 2048u, 4096u, 8192u, 16384u, 32768u, 65536u})
        {
            LnsAlu a(n);
            row(a, measureTable(a, ds));
        }
    }

    std::printf(failures ? "\n%d FAILURES\n" : "\nall checks passed\n", failures);
    return failures != 0;
}
