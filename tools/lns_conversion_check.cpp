// Standalone checks + accuracy/size sweep for the LUT-based LNS conversion unit (no Qt needed).
//
// Build (from the repo root):
//   g++ -std=c++20 -O2 -I src/processor/lns tools/lns_conversion_check.cpp tools/lns_reference.cpp
//       src/processor/lns/lns_conversion.cpp -o lns_check   (single command line)
// Run:  ./lns_check            (add "quick" to skip the table-size sweep)
#include "lns_conversion.h"
#include "lns_reference.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

using namespace Kites::lns;
namespace ref = Kites::lns::reference;

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

static float asFloat(uint32_t b) { return std::bit_cast<float>(b); }
static uint32_t asBits(float f) { return std::bit_cast<uint32_t>(f); }

// ---------------------------------------------------------------------------
// Correctness checks (independent of table size where noted)
// ---------------------------------------------------------------------------
static void checkSpecials(const LnsConverter &c)
{
    CHECK(c.fromFloatBits(0x00000000) == kPosZero);
    CHECK(c.fromFloatBits(0x80000000) == kPosZero); // -0 -> the single zero
    CHECK(c.fromFloatBits(0x7F800000) == kPosInf);
    CHECK(c.fromFloatBits(0xFF800000) == kNegInf);
    CHECK(c.fromFloatBits(0x7FC00000) == kNaN);
    CHECK(c.fromFloatBits(0x7F800001) == kNaN);
    CHECK(c.toFloatBits(kPosZero) == 0x00000000u);
    CHECK(c.toFloatBits(kPosInf) == 0x7F800000u);
    CHECK(c.toFloatBits(kNegInf) == 0xFF800000u);
    CHECK(c.toFloatBits(kNaN) == 0x7FC00000u);
    CHECK(c.toFloatBits(0xBFC00001u) == 0x7FC00000u); // NaN: sign and payload ignored
    CHECK(c.toFloatBits(0x3FFFFFFFu) == 0x7FC00000u); // any nonzero fraction in the Inf band
    CHECK(c.toFloatBits(0xC0000000u) == 0u);          // zero field with sign set still reads as 0
    // Exact word values of the specials (these must match the ALU)
    CHECK(kPosZero == 0x40000000u && kPosInf == 0x3FC00000u);
    CHECK(kNegInf == 0xBFC00000u && kNaN == 0x3FC00001u);
    CHECK(isZero(0xC0000000u) && isNaN(0xBFC00001u) && isInf(0xBFC00000u));
    CHECK(canonicalize(0xC0000000u) == kPosZero && canonicalize(0xBFC00001u) == kNaN);
    // Finite limits: just below the Inf band is finite, the band itself is not
    CHECK(isFinite(pack(false, kLqFiniteMax)) && isFinite(pack(true, kLqFiniteMin)));
    CHECK(c.toFloatBits(pack(false, kLqFiniteMax)) == 0x7F800000u); // finite in LNS, Inf in FP32

    CHECK(c.toInt32(kNaN) == std::numeric_limits<int32_t>::max());
    CHECK(c.toInt32(kPosInf) == std::numeric_limits<int32_t>::max());
    CHECK(c.toInt32(kNegInf) == std::numeric_limits<int32_t>::min());
    CHECK(c.toUInt32(kNaN) == std::numeric_limits<uint32_t>::max());
    CHECK(c.toUInt32(kNegInf) == 0);
    CHECK(c.toUInt64(kPosInf) == std::numeric_limits<uint64_t>::max());
    CHECK(c.toInt64(kNegInf) == std::numeric_limits<int64_t>::min());
    CHECK(c.toInt64(kPosZero) == 0);
}

static void checkPowersOfTwo(const LnsConverter &c)
{
    CHECK(c.fromFloatBits(asBits(1.0f)) == pack(false, 0));
    for (int e = -149; e <= 127; ++e)
    {
        float f = std::ldexp(1.0f, e);
        LnsWord w = c.fromFloatBits(asBits(f));
        CHECK(fieldOf(w) == e * (1 << kFracBits)); // forward exact
        CHECK(asFloat(c.toFloatBits(w)) == f);     // reverse exact (incl. subnormal results)
        float nf = -f;
        CHECK(asFloat(c.toFloatBits(c.fromFloatBits(asBits(nf)))) == nf);
    }
    CHECK(c.fromFloatBits(0x00000001) == pack(false, -149 * (1 << kFracBits)));
}

static void checkFloatRange(const LnsConverter &c, bool checkAccuracy)
{
    CHECK(isFinite(c.fromFloatBits(0x00000001)));          // smallest subnormal never becomes 0
    CHECK(isFinite(c.fromFloatBits(0x007FFFFF)));          // largest subnormal
    CHECK(isFinite(c.fromFloatBits(0x7F7FFFFF)));          // FLT_MAX never becomes Inf
    // Subnormal round trips stay within ~1 subnormal unit (only meaningful for realistic tables;
    // very small tables such as 1 or 6 bits are deliberately inaccurate)
    if (checkAccuracy)
    for (uint32_t m : {0x00000003u, 0x00000100u, 0x00123456u, 0x00400000u, 0x007FFFFFu})
    {
        uint32_t back = c.toFloatBits(c.fromFloatBits(m));
        CHECK(std::abs(static_cast<int64_t>(back) - static_cast<int64_t>(m)) <= 1 + (m >> 21));
    }
    // LNS overflow to FP32: L = 128 and above is Inf
    CHECK(c.toFloatBits(pack(false, 128 * (1 << kFracBits))) == 0x7F800000u);
    CHECK(c.toFloatBits(pack(true, 128 * (1 << kFracBits))) == 0xFF800000u);
    // Below 2^-150 flushes to 0
    CHECK(c.toFloatBits(pack(false, -152 * (1 << kFracBits))) == 0u);
}

static void checkIntegers(const LnsConverter &c)
{
    CHECK(c.fromInt32(0) == kPosZero);
    CHECK(c.fromUInt64(0) == kPosZero);
    CHECK(c.fromInt32(1) == pack(false, 0));
    CHECK(c.fromInt32(-8) == pack(true, 3 * (1 << kFracBits)));
    CHECK(c.toInt32(c.fromInt32(-8)) == -8);
    CHECK(c.toUInt32(c.fromInt32(-5)) == 0); // negative -> unsigned is 0
    CHECK(c.toInt32(c.fromInt32(std::numeric_limits<int32_t>::min())) ==
          std::numeric_limits<int32_t>::min()); // saturates onto INT_MIN
    CHECK(isFinite(c.fromInt64(std::numeric_limits<int64_t>::min())));
    CHECK(isFinite(c.fromUInt64(std::numeric_limits<uint64_t>::max())));
    CHECK(c.toUInt64(c.fromUInt64(std::numeric_limits<uint64_t>::max())) >=
          (std::numeric_limits<uint64_t>::max() >> 1));
    for (int64_t v = 1; v < 3000000; v += 7919)
    {
        int64_t r = c.toInt64(c.fromInt64(v));
        CHECK(std::llabs(r - v) <= static_cast<int64_t>(static_cast<double>(v) * 2e-7) + 1);
        CHECK(c.toInt64(c.fromInt64(-v)) == -r);
    }
    // Large unsigned values stay within LNS relative precision
    for (int s = 0; s < 64; s += 3)
    {
        uint64_t v = (uint64_t{0xDEADBEEF} << (s > 32 ? 31 : 0)) >> (s > 32 ? 0 : s);
        if (v == 0)
        {
            continue;
        }
        uint64_t r = c.toUInt64(c.fromUInt64(v));
        double rel = std::fabs(static_cast<double>(r) - static_cast<double>(v)) /
                     static_cast<double>(v);
        CHECK(rel < 2e-7);
    }
}

// ---------------------------------------------------------------------------
// LUT vs reference, FP32 round-trip error, table-size sweep
// ---------------------------------------------------------------------------
struct Samples
{
    std::vector<uint32_t> floats;  // FP32 normal bit patterns (strided sweep)
    std::vector<LnsWord> refFwd;   // reference forward result per float
    std::vector<double> refRtRel;  // reference round-trip relative error per float
    std::vector<LnsWord> lnsWords; // random finite LNS words in the FP32 range
    std::vector<uint32_t> refBack; // reference LNS->FP32 per word
};

static Samples buildSamples()
{
    Samples s;
    for (uint64_t b = 0; b < (1ull << 32); b += 4099)
    {
        uint32_t bits = static_cast<uint32_t>(b);
        float f = asFloat(bits);
        if (!std::isfinite(f) || std::fabs(f) < 1.1754944e-38f) // normals only
        {
            continue;
        }
        s.floats.push_back(bits);
    }
    s.refFwd.reserve(s.floats.size());
    s.refRtRel.reserve(s.floats.size());
    for (uint32_t bits : s.floats)
    {
        LnsWord w = ref::fromFloatBits(bits);
        s.refFwd.push_back(w);
        double f = asFloat(bits);
        double back = asFloat(ref::toFloatBits(w));
        s.refRtRel.push_back(std::fabs(back - f) / std::fabs(f));
    }
    uint64_t state = 0x9E3779B97F4A7C15ull;
    const int64_t lo = -149LL * (1 << kFracBits), hi = 127LL * (1 << kFracBits);
    for (int i = 0; i < 1000000; ++i)
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        int64_t lq = lo + static_cast<int64_t>((state >> 11) % static_cast<uint64_t>(hi - lo));
        bool sign = (state >> 63) & 1;
        LnsWord w = pack(sign, static_cast<int32_t>(lq));
        s.lnsWords.push_back(w);
        s.refBack.push_back(ref::toFloatBits(w));
    }
    return s;
}

struct Stats
{
    int logBits, expBits;
    size_t bytes;
    int fwdMax;           // max |dLq| vs reference, in LNS steps (2^-22)
    double fwdMean;
    double fwdMismatch;   // fraction of inputs whose word differs from reference
    int revMaxUlp;        // max FP32 ulp difference vs reference
    double revMismatch;
    double rtMax, rtMean; // FP32 -> LNS -> FP32 relative error
};

static Stats measure(const LnsConverter &c, const Samples &s)
{
    Stats st{};
    st.logBits = c.logLutBits();
    st.expBits = c.expLutBits();
    st.bytes = c.logLutBytes() + c.expLutBytes();

    double fwdSum = 0, rtSum = 0;
    size_t fwdDiff = 0;
    for (size_t i = 0; i < s.floats.size(); ++i)
    {
        LnsWord w = c.fromFloatBits(s.floats[i]);
        int d = std::abs(fieldOf(w) - fieldOf(s.refFwd[i]));
        CHECK(signOf(w) == signOf(s.refFwd[i]));
        st.fwdMax = std::max(st.fwdMax, d);
        fwdSum += d;
        fwdDiff += (d != 0);
        double f = asFloat(s.floats[i]);
        double back = asFloat(c.toFloatBits(w));
        double rel = std::fabs(back - f) / std::fabs(f);
        st.rtMax = std::max(st.rtMax, rel);
        rtSum += rel;
    }
    st.fwdMean = fwdSum / static_cast<double>(s.floats.size());
    st.fwdMismatch = static_cast<double>(fwdDiff) / static_cast<double>(s.floats.size());
    st.rtMean = rtSum / static_cast<double>(s.floats.size());

    size_t revDiff = 0;
    for (size_t i = 0; i < s.lnsWords.size(); ++i)
    {
        uint32_t g = c.toFloatBits(s.lnsWords[i]);
        uint32_t r = s.refBack[i];
        int u = std::abs(static_cast<int64_t>(g & 0x7FFFFFFF) - static_cast<int64_t>(r & 0x7FFFFFFF));
        CHECK((g >> 31) == (r >> 31));
        st.revMaxUlp = std::max(st.revMaxUlp, u);
        revDiff += (u != 0);
    }
    st.revMismatch = static_cast<double>(revDiff) / static_cast<double>(s.lnsWords.size());
    return st;
}

static void printHeader()
{
    std::printf("%-5s %-5s %10s | %7s %9s %9s | %7s %9s | %11s %11s\n", "logK", "expK", "table(KiB)",
                "fwdMax", "fwdMean", "fwd!=ref", "revMax", "rev!=ref", "RT max rel", "RT mean rel");
    std::printf("%-5s %-5s %10s | %7s %9s %9s | %7s %9s | %11s %11s\n", "", "", "", "(steps)",
                "(steps)", "", "(ulp)", "", "", "");
}

static void printRow(const Stats &st)
{
    std::printf("%-5d %-5d %10.1f | %7d %9.4f %8.3f%% | %7d %8.3f%% | %11.3e %11.3e\n", st.logBits,
                st.expBits, static_cast<double>(st.bytes) / 1024.0, st.fwdMax, st.fwdMean,
                st.fwdMismatch * 100.0, st.revMaxUlp, st.revMismatch * 100.0, st.rtMax, st.rtMean);
}

int main(int argc, char **argv)
{
    const bool quick = argc > 1 && std::strcmp(argv[1], "quick") == 0;

    // 1. Correctness with the default table size, and with tiny/huge tables.
    {
        const LnsConverter &c = defaultConverter();
        std::printf("default converter: logK=%d expK=%d\n", c.logLutBits(), c.expLutBits());
        checkSpecials(c);
        checkPowersOfTwo(c);
        checkFloatRange(c, true);
        checkIntegers(c);
    }
    for (auto [lk, ek] : {std::pair{1, 1}, std::pair{6, 6}, std::pair{23, 22}})
    {
        LnsConverter c(lk, ek);
        checkSpecials(c);
        checkPowersOfTwo(c);
        checkFloatRange(c, lk >= 11);
    }
    bool threw = false;
    try { LnsConverter bad(0, 11); } catch (const std::invalid_argument &) { threw = true; }
    CHECK(threw);
    threw = false;
    try { LnsConverter bad(11, 23); } catch (const std::invalid_argument &) { threw = true; }
    CHECK(threw);
    CHECK(defaultConverter().toFloatBits(defaultConverter().fromFloatBits(asBits(10.0f))) != 0);

    // 2. LUT vs reference, with pass criteria on the default configuration.
    std::printf("building samples...\n");
    Samples samples = buildSamples();
    std::printf("%zu FP32 normals, %zu random LNS words\n\n", samples.floats.size(),
                samples.lnsWords.size());

    double refMax = 0, refSum = 0;
    for (double r : samples.refRtRel)
    {
        refMax = std::max(refMax, r);
        refSum += r;
    }
    std::printf("reference (host log2/exp2) FP32->LNS->FP32 round trip: max rel %.3e, mean rel %.3e\n"
                "(this is the floor set by the Q9.22 format itself; the LUT can only add to it)\n\n",
                refMax, refSum / static_cast<double>(samples.refRtRel.size()));

    Stats def = measure(defaultConverter(), samples);
    printHeader();
    printRow(def);
    CHECK(def.fwdMax <= 1);
    CHECK(def.revMaxUlp <= 1);
    CHECK(def.rtMax < 1.7e-7);

    // 3. Table-size sweep.
    if (!quick)
    {
        std::printf("\nsweep (uniform segments, linear interpolation):\n");
        printHeader();
        for (int k : {6, 7, 8, 9, 10, 11, 12, 13})
        {
            LnsConverter c(k, k);
            printRow(measure(c, samples));
        }
        LnsConverter full(23, 22); // no interpolation for FP32 inputs: the "ideal" point
        printRow(measure(full, samples));
    }

    std::printf(failures ? "\n%d FAILURES\n" : "\nall checks passed\n", failures);
    return failures != 0;
}
