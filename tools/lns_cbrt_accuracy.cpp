// Bit-exact C++ model of the TCP CUBIC cube-root benchmark (examples/lns_int_cbrt): every variant's
// per-input output, accuracy against the exact floor cube root, and the fix-up step counts, as a
// function of the LNS converter table sizes.
//
// The LNS variants call the simulator's own code (LnsConverter, power(), LnsAlu) exactly as the
// rvss instructions do: fcvt.lns.lu = fromUInt64, lpow = power(x, y, converter),
// fcvt.lu.lns = toUInt64, ldiv = LnsAlu DIV, fcvt.lns.w = fromInt32. B0/B1 are plain C++ ports of
// Linux v6.12 cubic_root() (u32 wrap-around kept), and F0 mirrors the FP64 sequence of the
// program (fmul.d, fdiv.d, fadd.d; KITES evaluates fcvt.lu.d with std::nearbyint).
// At the default table sizes the outputs must equal the kites-cli output arrays bit for bit;
// examples/lns_int_cbrt/check.py enforces that.
//
// Build (from the repo root, one command line):
//   g++ -std=c++20 -O2 -I src/processor/lns tools/lns_cbrt_accuracy.cpp
//       src/processor/lns/lns_alu.cpp src/processor/lns/lns_conversion.cpp
//       src/processor/lns/lns_pow.cpp -o lns_cbrt_accuracy
// Run:
//   ./lns_cbrt_accuracy --eval <B0|B1|L0|L1|L2|F0> <program.s>
//       one line per input of the program's "input:" block: "<output> <fixup candidate>", using
//       the converter the simulator would build (LNS_LOG_LUT_BITS / LNS_EXP_LUT_BITS env vars).
//   ./lns_cbrt_accuracy --size <log bits> <exp bits>
//       prints the converter table bytes (log2(1+x) table + 2^f table) for one configuration.
//   ./lns_cbrt_accuracy --sweep [per-set count, default 1000000]
//       CSV on stdout: accuracy and fix-up steps for every input set x table config x variant.
//   ./lns_cbrt_accuracy --keval <BK|LK|LDK|FK|FS> <k> <program.s> <fixed> <fixed for a=0>
//       generic k-th root programs of examples/lns_int_cbrt/sweep_configs.py: per input
//       "<output> <fix-up candidate (BK: Newton iterations)> <instructions (LK/LDK)>".
//   ./lns_cbrt_accuracy --ksweep <per-set count> <fixed costs>
//       CSV: k = 2..8 x log bits x exp bits x input set for LK/LDK (sweep_configs.py model).
#include "lns_alu.h"
#include "lns_conversion.h"
#include "lns_pow.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Kites::lns;

namespace
{
using u32 = uint32_t;
using u64 = uint64_t;
using u128 = unsigned __int128;

constexpr u64 kCap = 2642245; // floor(cbrt(2^64 - 1))
constexpr u64 kRecip3_32 = 0xAAAAAAABull;
constexpr u64 kRecip3_64 = 0xAAAAAAAAAAAAAAABull;
constexpr u64 kFpMagic = 0x2AA0000000000000ull;
constexpr int kFpNewtonSteps = 3;

constexpr std::array<uint8_t, 64> kV = {
    0,   54,  54,  54,  118, 118, 118, 118, 123, 129, 134, 138, 143, 147, 151, 156,
    157, 161, 164, 168, 170, 173, 176, 179, 181, 185, 187, 190, 192, 194, 197, 199,
    200, 202, 204, 206, 209, 211, 213, 215, 217, 219, 221, 222, 224, 225, 227, 229,
    231, 232, 234, 236, 237, 239, 240, 242, 244, 245, 246, 248, 250, 251, 252, 254};

u64 cube(u64 x) { return x * x * x; } // callers keep x <= kCap, so no overflow

/// floor(cbrt(a)), exact (FP estimate, then integer correction in 128 bits).
u64 exactCbrt(u64 a)
{
    if (a < 2)
    {
        return a;
    }
    u64 x = static_cast<u64>(std::cbrt(static_cast<long double>(a)));
    if (x > kCap)
    {
        x = kCap;
    }
    while (x > 0 && static_cast<u128>(x) * x * x > a)
    {
        --x;
    }
    while (x < kCap && static_cast<u128>(x + 1) * (x + 1) * (x + 1) <= a)
    {
        ++x;
    }
    return x;
}

/// round(cbrt(a)): r + 1 when (2r + 1)^3 <= 8a.
u64 nearestCbrt(u64 a)
{
    const u64 r = exactCbrt(a);
    const u128 t = 2 * static_cast<u128>(r) + 1;
    return (t * t * t <= 8 * static_cast<u128>(a)) ? r + 1 : r;
}

/// Linux v6.12 cubic_root(), line by line.
u32 kernelCubicRoot(u64 a)
{
    u32 b = static_cast<u32>(64 - std::countl_zero(a)); // fls64
    if (b < 7)
    {
        return (static_cast<u32>(kV[static_cast<u32>(a)]) + 35) >> 6;
    }
    b = ((b * 84) >> 8) - 1;
    const u32 shift = static_cast<u32>(a >> (b * 3));
    u32 x = static_cast<u32>((static_cast<u32>(kV[shift]) + 10) << b) >> 6;
    x = 2 * x + static_cast<u32>(a / (static_cast<u64>(x) * static_cast<u64>(x - 1)));
    x = (x * 341) >> 10;
    return x;
}

u64 refine(u64 a, u64 x)
{
    if (x == 0)
    {
        return 0;
    }
    return ((2 * x + a / (x * x)) * kRecip3_32) >> 33;
}

/// The mul-based fix-up of the exact programs: clamp to kCap, step down, step up.
u64 fixup(u64 a, u64 x)
{
    if (x > kCap)
    {
        x = kCap;
    }
    while (cube(x) > a)
    {
        --x;
    }
    while (x < kCap && cube(x + 1) <= a)
    {
        ++x;
    }
    return x;
}

struct Lns
{
    const LnsConverter &conv;
    LnsWord third;
    explicit Lns(const LnsConverter &c)
        : conv(c), third(defaultAlu().execute(LnsAluOp::DIV, c.fromInt32(1), c.fromInt32(3)))
    {
    }
    u64 l0(u64 a) const { return conv.toUInt64(power(conv.fromUInt64(a), third, conv)); }
    u64 l2raw(u64 a) const
    {
        LnsWord w = conv.fromUInt64(a);
        if (a != 0)
        { // lsw / lwu / addi 1 / mul recip / srli 33 / sw / llw
            w = static_cast<LnsWord>(((static_cast<u64>(w) + 1) * kRecip3_32) >> 33);
        }
        return conv.toUInt64(w);
    }
};

u64 f0raw(u64 a)
{
    const double d = static_cast<double>(a); // fcvt.d.lu (round to nearest)
    u64 bits = std::bit_cast<u64>(d);
    bits = static_cast<u64>((static_cast<u128>(bits) * kRecip3_64) >> 64) >> 1; // mulhu, srli
    double y = std::bit_cast<double>(bits + kFpMagic);
    for (int i = 0; i < kFpNewtonSteps; ++i)
    {
        double q = y * y;
        q = d / q;
        const double twoY = y + y;
        q = q + twoY;
        y = q * (1.0 / 3.0); // the program loads 0x3FD5555555555555 == (double)(1.0/3.0)
    }
    // fcvt.lu.d as KITES implements it (current rounding mode = nearest).
    if (!std::isfinite(y) || y > static_cast<double>(UINT64_MAX) || y < 0.0)
    {
        return y < 0.0 ? 0 : UINT64_MAX;
    }
    return static_cast<u64>(std::nearbyint(y));
}

/// (final output, value entering the fix-up) of one variant for input a.
std::pair<u64, u64> evalVariant(const std::string &v, const Lns &lns, u64 a)
{
    if (v == "B0")
    {
        const u64 x = kernelCubicRoot(a);
        return {x, x};
    }
    if (v == "B1")
    {
        const u64 c = refine(a, kernelCubicRoot(a));
        return {fixup(a, c), c};
    }
    if (v == "L0")
    {
        const u64 x = lns.l0(a);
        return {x, x};
    }
    if (v == "L1")
    {
        const u64 c = lns.l0(a);
        return {fixup(a, c), c};
    }
    if (v == "L2")
    {
        const u64 c = lns.l2raw(a);
        return {fixup(a, c), c};
    }
    if (v == "F0")
    {
        const u64 c = f0raw(a);
        return {fixup(a, c), c};
    }
    throw std::runtime_error("unknown variant " + v);
}

/// The .dword values between "input:" and "output:" in a generated program.
std::vector<u64> readInputs(const std::string &path)
{
    std::ifstream in(path);
    if (!in)
    {
        throw std::runtime_error("cannot open " + path);
    }
    std::vector<u64> values;
    std::string line;
    bool inInput = false;
    while (std::getline(in, line))
    {
        if (line.rfind("input:", 0) == 0)
        {
            inInput = true;
            continue;
        }
        if (line.rfind("output:", 0) == 0)
        {
            break;
        }
        const auto at = line.find(".dword");
        if (!inInput || at == std::string::npos)
        {
            continue;
        }
        std::string list = line.substr(at + 6);
        for (char &ch : list)
        {
            if (ch == ',')
            {
                ch = ' ';
            }
        }
        std::istringstream fields(list);
        std::string token;
        while (fields >> token)
        {
            values.push_back(std::stoull(token, nullptr, 0));
        }
    }
    return values;
}

// ---- input sets for the sweep (large versions of the generator's sets) ----------------------
struct Lcg
{
    u64 state;
    u32 next32()
    {
        state = 6364136223846793005ull * state + 1442695040888963407ull;
        return static_cast<u32>(state >> 32);
    }
    u64 next64()
    {
        const u64 hi = next32();
        return (hi << 32) | next32();
    }
};

std::map<std::string, std::vector<u64>> sweepSets(size_t n)
{
    std::map<std::string, std::vector<u64>> s;
    auto &small = s["small"];
    for (u64 a = 0; a < n; ++a)
    {
        small.push_back(a);
    }
    auto &near = s["near2^22"];
    for (int p : {21, 22, 23, 24})
    {
        for (int64_t d = -4096; d <= 4096; ++d)
        {
            near.push_back(static_cast<u64>((int64_t{1} << p) + d));
        }
    }
    Lcg r{0xC0B1C};
    while (near.size() < n)
    {
        near.push_back((u64{1} << 21) + r.next64() % ((u64{1} << 24) - (u64{1} << 21) + 1));
    }
    auto &cubes = s["cubes"];
    const size_t ncount = n / 3;
    for (size_t i = 0; i < ncount; ++i)
    {
        const u64 m = 1 + static_cast<u64>(i) * (kCap - 1) / (ncount - 1);
        const u64 c = cube(m);
        cubes.push_back(c - 1);
        cubes.push_back(c);
        if (m < kCap || c + 1 != 0)
        {
            cubes.push_back(c + 1);
        }
    }
    auto &u32max = s["u32max"];
    for (u64 a = (u64{1} << 32) - n / 2; a < (u64{1} << 32) + n / 2; ++a)
    {
        u32max.push_back(a);
    }
    auto &u64max = s["u64max"];
    for (u64 d = 0; d < n / 2; ++d)
    {
        u64max.push_back(UINT64_MAX - d);
    }
    for (u64 a = (u64{1} << 63) - n / 4; a < (u64{1} << 63) + n / 4; ++a)
    {
        u64max.push_back(a);
    }
    auto &uni = s["uniform_bits"]; // same recipe and seed as gen_cbrt.py (first 1024 identical)
    Lcg u{0x5EEDC0DE};
    while (uni.size() < n)
    {
        const u32 bits = 1 + u.next32() % 64;
        const u64 draw = u.next64(); // always drawn, as in the generator
        const u64 low = bits == 1 ? 0 : (draw & ((u64{1} << (bits - 1)) - 1));
        uni.push_back((u64{1} << (bits - 1)) | low);
    }
    return s;
}

struct Stats
{
    size_t n = 0, exact = 0, nearest = 0;
    u64 maxAbs = 0;
    long double sumAbs = 0, sumRel = 0, sumRelReal = 0;
    std::map<int64_t, size_t> steps;
    void add(u64 a, u64 out, u64 candidate, bool fix)
    {
        const u64 e = exactCbrt(a);
        ++n;
        exact += out == e;
        nearest += out == nearestCbrt(a);
        const u64 d = out > e ? out - e : e - out;
        maxAbs = std::max(maxAbs, d);
        sumAbs += d;
        if (e > 0)
        {
            sumRel += static_cast<long double>(d) / e;
        }
        if (a > 0)
        {
            const long double real = std::cbrt(static_cast<long double>(a));
            sumRelReal += std::fabs(static_cast<long double>(out) - real) / real;
        }
        if (fix)
        {
            ++steps[static_cast<int64_t>(out - candidate)];
        }
    }
};

void printHeader()
{
    std::printf("variant,input_set,n,log_bits,exp_bits,table_bytes,pct_exact,pct_nearest,"
                "max_abs_err,mean_abs_err,mean_rel_err,mean_rel_err_vs_real_cbrt,"
                "fixup_steps_mean_abs,fixup_steps_max_abs,fixup_steps_hist\n");
}

void printRow(const char *variant, const std::string &set, int lb, int eb, size_t bytes,
              const Stats &s)
{
    long double meanSteps = 0;
    int64_t maxSteps = 0;
    std::string hist;
    for (const auto &[k, c] : s.steps)
    {
        meanSteps += static_cast<long double>(std::llabs(k)) * c;
        maxSteps = std::max<int64_t>(maxSteps, std::llabs(k));
        hist += (hist.empty() ? "" : ";") + std::to_string(k) + ":" + std::to_string(c);
    }
    if (!s.steps.empty())
    {
        meanSteps /= s.n;
    }
    std::printf("%s,%s,%zu,%d,%d,%zu,%.6Lf,%.6Lf,%llu,%.6Lf,%.4Le,%.4Le,%s,%s,%s\n", variant,
                set.c_str(), s.n, lb, eb, bytes, 100.0L * s.exact / s.n, 100.0L * s.nearest / s.n,
                static_cast<unsigned long long>(s.maxAbs), s.sumAbs / s.n, s.sumRel / s.n,
                s.sumRelReal / s.n,
                s.steps.empty() ? "" : std::to_string(static_cast<double>(meanSteps)).c_str(),
                s.steps.empty() ? "" : std::to_string(maxSteps).c_str(), hist.c_str());
}

void sweep(size_t n)
{
    const auto sets = sweepSets(n);
    printHeader();
    // Table-independent variants once. "B0+fixup" is the literal "B0 plus a +/-1 fix-up loop"
    // (no extra Newton step), kept to show why B1 adds one Newton step first.
    for (const auto &[name, values] : sets)
    {
        Stats b0, b0fix, b1, f0raw_, f0;
        for (u64 a : values)
        {
            const u64 k = kernelCubicRoot(a);
            b0.add(a, k, k, false);
            b0fix.add(a, fixup(a, k), k, true);
            const u64 c = refine(a, k);
            b1.add(a, fixup(a, c), c, true);
            const u64 fc = f0raw(a);
            f0raw_.add(a, fc, fc, false);
            f0.add(a, fixup(a, fc), fc, true);
        }
        printRow("B0", name, 0, 0, 64, b0);
        printRow("B0+fixup_only", name, 0, 0, 64, b0fix);
        printRow("B1", name, 0, 0, 64, b1);
        printRow("F0raw", name, 0, 0, 0, f0raw_);
        printRow("F0", name, 0, 0, 0, f0);
    }
    std::vector<std::pair<int, int>> configs;
    for (int b = 6; b <= 16; ++b)
    {
        configs.emplace_back(b, b);
    }
    configs.emplace_back(22, 22);
    configs.emplace_back(23, 22);
    for (const auto &[lb, eb] : configs)
    {
        const LnsConverter conv(lb, eb);
        const Lns lns(conv);
        const size_t bytes = conv.logLutBytes() + conv.expLutBytes();
        for (const auto &[name, values] : sets)
        {
            Stats l0, l1, l2raw, l2;
            for (u64 a : values)
            {
                const u64 c = lns.l0(a);
                l0.add(a, c, c, false);
                l1.add(a, fixup(a, c), c, true);
                const u64 c2 = lns.l2raw(a);
                l2raw.add(a, c2, c2, false);
                l2.add(a, fixup(a, c2), c2, true);
            }
            printRow("L0", name, lb, eb, bytes, l0);
            printRow("L1", name, lb, eb, bytes, l1);
            printRow("L2raw", name, lb, eb, bytes, l2raw);
            printRow("L2", name, lb, eb, bytes, l2);
        }
        std::fflush(stdout);
    }
}
// ---- generic k-th root (examples/lns_int_cbrt/sweep_configs.py) ------------------------------
/// x^k in 128 bits, saturated at 2^65 (enough to compare against any u64).
u128 powSat(u64 x, int k)
{
    u128 r = 1;
    for (int i = 0; i < k; ++i)
    {
        r *= x;
        if (r > (static_cast<u128>(1) << 65))
        {
            return static_cast<u128>(1) << 65;
        }
    }
    return r;
}

/// floor(a^(1/k)), exact.
u64 iroot(u64 a, int k)
{
    if (a < 2)
    {
        return a;
    }
    u64 x = static_cast<u64>(std::pow(static_cast<long double>(a), 1.0L / k));
    while (x > 0 && powSat(x, k) > a)
    {
        --x;
    }
    while (powSat(x + 1, k) <= a)
    {
        ++x;
    }
    return x;
}

u64 capK(int k) { return iroot(UINT64_MAX, k); }

/// The mul-based fix-up of the sweep programs (x^k instead of x^3).
u64 fixupK(u64 a, u64 x, int k, u64 cap)
{
    if (x > cap)
    {
        x = cap;
    }
    while (powSat(x, k) > a)
    {
        --x;
    }
    while (x < cap && powSat(x + 1, k) <= a)
    {
        ++x;
    }
    return x;
}

/// Instructions of one fix-up (uninstrumented); same formula as sweep_configs.fixup_cost(),
/// which sweep_configs.py checks against the block walk of every simulated input.
u64 fixupCostK(int k, u64 cand, u64 fin, u64 cap)
{
    const u64 cl = cand > cap ? 1 : 0;
    const u64 x = cand > cap ? cap : cand;
    const u64 n = 1 + cl + k;
    if (fin < x)
    {
        return n + (x - fin) * (k + 1);
    }
    return n + (fin - x) * (1 + (k + 1) + 2) + (fin == cap ? 1 : 1 + (k + 1));
}

/// fcvt.lu.d as KITES implements it (round to nearest, saturating).
u64 fcvtLuD(double y)
{
    if (!std::isfinite(y) || y > static_cast<double>(UINT64_MAX) || y < 0.0)
    {
        return y < 0.0 ? 0 : UINT64_MAX;
    }
    return static_cast<u64>(std::nearbyint(y));
}

struct KRoot
{
    int k;
    u64 cap;
    const LnsConverter &conv;
    LnsWord inv;  // 1/k in LNS: ldiv(fromInt32(1), fromInt32(k))
    LnsWord invc; // 1/k with a correctly rounded log: round(-log2(k) * 2^22) (variant LKc)
    u64 fpR;      // seed: bits/k ~= mulhu(bits, fpR) >> fpS
    int fpS;
    u64 fpMagic;  // (k-1)/k * bits(1.0)
    KRoot(int k_, const LnsConverter &c)
        : k(k_), cap(capK(k_)), conv(c),
          inv(defaultAlu().execute(LnsAluOp::DIV, c.fromInt32(1), c.fromInt32(k_))),
          invc(pack(false, static_cast<int32_t>(
                               -std::llround(std::log2(static_cast<long double>(k_)) * 4194304.0L)))),
          fpR(0),
          fpS(0), fpMagic(static_cast<u64>((static_cast<u128>(k_ - 1) * 0x3FF0000000000000ull) / k_))
    {
        for (int s = 0; s < 8; ++s)
        {
            const u128 num = static_cast<u128>(1) << (64 + s);
            const u128 r = (num + k_ - 1) / k_;
            if (r < (static_cast<u128>(1) << 64))
            {
                fpR = static_cast<u64>(r);
                fpS = s;
            }
        }
    }
    /// BK: integer Newton from 2^ceil(bits/k); returns (root, iterations).
    std::pair<u64, u64> bk(u64 a) const
    {
        if (a == 0)
        {
            return {0, 0};
        }
        const int bits = 64 - std::countl_zero(a);
        u64 x = u64{1} << ((bits + k - 1) / k);
        u64 it = 0;
        for (;;)
        {
            u64 p = 1;
            for (int i = 0; i < k - 1; ++i)
            {
                p *= x;
            }
            const u64 y = ((k - 1) * x + a / p) / k;
            if (y >= x)
            {
                return {x, it};
            }
            x = y;
            ++it;
        }
    }
    u64 lk(u64 a) const { return conv.toUInt64(power(conv.fromUInt64(a), inv, conv)); }
    u64 lkc(u64 a) const { return conv.toUInt64(power(conv.fromUInt64(a), invc, conv)); }
    u64 ldk(u64 a) const
    {
        LnsWord w = conv.fromUInt64(a);
        if (a != 0)
        {
            w = static_cast<LnsWord>((static_cast<u64>(w) + 1) / k);
        }
        return conv.toUInt64(w);
    }
    u64 fk(u64 a) const
    {
        const double d = static_cast<double>(a);
        const u64 bits = std::bit_cast<u64>(d);
        const u64 g = static_cast<u64>((static_cast<u128>(bits) * fpR) >> 64) >> fpS;
        double y = std::bit_cast<double>(g + fpMagic);
        const double km1 = static_cast<double>(k - 1);
        const double invk = 1.0 / k;
        for (int i = 0; i < 3; ++i)
        {
            double q;
            if (k == 2)
            {
                q = d / y;
                q = q + y;
            }
            else
            {
                double p = y * y;
                for (int j = 0; j < k - 3; ++j)
                {
                    p = p * y;
                }
                q = d / p;
                const double t = y * km1;
                q = q + t;
            }
            y = q * invk;
        }
        return fcvtLuD(y);
    }
    u64 fs(u64 a) const { return fcvtLuD(std::sqrt(static_cast<double>(a))); }
    /// (output, fix-up candidate or Newton iterations for BK)
    std::pair<u64, u64> eval(const std::string &v, u64 a) const
    {
        if (v == "BK")
        {
            return bk(a);
        }
        u64 c;
        if (v == "LK")
        {
            c = lk(a);
        }
        else if (v == "LKc")
        {
            c = lkc(a);
        }
        else if (v == "LDK")
        {
            c = ldk(a);
        }
        else if (v == "FK")
        {
            c = fk(a);
        }
        else if (v == "FS")
        {
            c = fs(a);
        }
        else
        {
            throw std::runtime_error("unknown k-root variant " + v);
        }
        return {fixupK(a, c, k, cap), c};
    }
};

std::map<std::string, std::vector<u64>> kSweepSets(size_t n)
{
    std::map<std::string, std::vector<u64>> s;
    auto &small = s["small"];
    for (u64 a = 0; a < n; ++a)
    {
        small.push_back(a);
    }
    Lcg r{0x5EEDC0DE};
    auto &uni = s["uniform_bits"];
    while (uni.size() < n)
    {
        const u32 bits = 1 + r.next32() % 64;
        const u64 draw = r.next64();
        const u64 low = bits == 1 ? 0 : (draw & ((u64{1} << (bits - 1)) - 1));
        uni.push_back((u64{1} << (bits - 1)) | low);
    }
    Lcg q{0xFADE};
    auto &u64v = s["uniform_u64"];
    while (u64v.size() < n)
    {
        u64v.push_back(q.next64());
    }
    for (int w : {16, 32, 48, 64})
    {
        Lcg t{0x51DE00ull + static_cast<u64>(w)};
        auto &v = s["width" + std::to_string(w)];
        while (v.size() < n)
        {
            const u64 low = t.next64() & ((u64{1} << (w - 1)) - 1);
            v.push_back((u64{1} << (w - 1)) | low);
        }
    }
    return s;
}

/// costs: "LK2=9/9,LDK2=15/9,..." = per-root instructions outside the fix-up (a != 0 / a == 0).
void kSweep(size_t n, const std::string &costs)
{
    std::map<std::string, std::pair<u64, u64>> fixed;
    std::stringstream cs(costs);
    std::string item;
    while (std::getline(cs, item, ','))
    {
        const auto eq = item.find('='), sl = item.find('/');
        fixed[item.substr(0, eq)] = {std::stoull(item.substr(eq + 1, sl - eq - 1)),
                                     std::stoull(item.substr(sl + 1))};
    }
    const auto sets = kSweepSets(n);
    std::printf("k,variant,input_set,n,log_bits,exp_bits,table_bytes,pct_exact,"
                "candidate_pct_exact,candidate_max_abs_err,candidate_mean_rel_err,"
                "steps_mean_abs,steps_max_abs,pct_steps_le1,instr_per_root_model,"
                "steps_abs_buckets_0;1;2-3;4-15;16-255;256-65535;65536+\n");
    const std::vector<int> logs = {6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 23};
    const std::vector<int> exps = {6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 22};
    std::map<std::pair<int, std::string>, std::vector<u64>> exact;
    for (int k = 2; k <= 8; ++k)
    {
        for (const auto &[name, values] : sets)
        {
            auto &e = exact[{k, name}];
            for (u64 a : values)
            {
                e.push_back(iroot(a, k));
            }
        }
    }
    for (int lb : logs)
    {
        for (int eb : exps)
        {
            const LnsConverter conv(lb, eb);
            const size_t bytes = conv.logLutBytes() + conv.expLutBytes();
            for (int k = 2; k <= 8; ++k)
            {
                const KRoot kr(k, conv);
                for (const auto &[name, values] : sets)
                {
                    const auto &ex = exact[{k, name}];
                    for (const char *v : {"LK", "LKc", "LDK"})
                    {
                        const auto [fa, fz] = fixed.at(std::string(v) + std::to_string(k));
                        size_t candExact = 0, le1 = 0, ok = 0;
                        u64 maxAbs = 0, maxSteps = 0;
                        long double sumRel = 0, sumSteps = 0, sumCost = 0;
                        // |steps| buckets: 0, 1, 2-3, 4-15, 16-255, 256-65535, >= 65536
                        std::array<size_t, 7> hist{};
                        for (size_t i = 0; i < values.size(); ++i)
                        {
                            const u64 a = values[i];
                            const std::string vs(v);
                            const u64 c = vs == "LK" ? kr.lk(a) : vs == "LKc" ? kr.lkc(a) : kr.ldk(a);
                            const u64 f = ex[i]; // == fixupK(a, c, k, cap) for every c

                            ok += f == ex[i];
                            candExact += c == ex[i];
                            const u64 d = c > ex[i] ? c - ex[i] : ex[i] - c;
                            maxAbs = std::max(maxAbs, d);
                            if (ex[i] > 0)
                            {
                                sumRel += static_cast<long double>(d) / ex[i];
                            }
                            const int64_t st = static_cast<int64_t>(f - c);
                            const u64 ast = static_cast<u64>(std::llabs(st));
                            maxSteps = std::max(maxSteps, ast);
                            sumSteps += ast;
                            le1 += ast <= 1;
                            ++hist[ast == 0 ? 0 : ast == 1 ? 1 : ast < 4 ? 2 : ast < 16 ? 3
                                   : ast < 256 ? 4 : ast < 65536 ? 5 : 6];
                            sumCost += (a == 0 ? fz : fa) + fixupCostK(k, c, f, kr.cap);
                        }
                        std::string h;
                        for (size_t b = 0; b < hist.size(); ++b)
                        {
                            h += (b ? ";" : "") + std::to_string(hist[b]);
                        }
                        const long double nn = static_cast<long double>(values.size());
                        std::printf("%d,%s,%s,%zu,%d,%d,%zu,%.4Lf,%.4Lf,%llu,%.4Le,%.4Lf,%llu,"
                                    "%.4Lf,%.3Lf,%s\n",
                                    k, v, name.c_str(), values.size(), lb, eb, bytes,
                                    100.0L * ok / nn, 100.0L * candExact / nn,
                                    static_cast<unsigned long long>(maxAbs), sumRel / nn,
                                    sumSteps / nn, static_cast<unsigned long long>(maxSteps),
                                    100.0L * le1 / nn, sumCost / nn, h.c_str());
                    }
                }
            }
            std::fflush(stdout);
        }
    }
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "--size" && argc == 4)
        {
            const LnsConverter conv(std::stoi(argv[2]), std::stoi(argv[3]));
            std::printf("%zu\n", conv.logLutBytes() + conv.expLutBytes());
            return 0;
        }
        if (mode == "--eval" && argc == 4)
        {
            const Lns lns(defaultConverter());
            for (u64 a : readInputs(argv[3]))
            {
                const auto [out, cand] = evalVariant(argv[2], lns, a);
                std::printf("%llu %llu\n", static_cast<unsigned long long>(out),
                            static_cast<unsigned long long>(cand));
            }
            return 0;
        }
        if (mode == "--keval" && argc == 7)
        {
            // one line per input: "<output> <candidate | BK iterations> <model instructions>"
            const int k = std::stoi(argv[3]);
            const KRoot kr(k, defaultConverter());
            const std::string v = argv[2];
            const u64 fa = std::stoull(argv[5]), fz = std::stoull(argv[6]);
            for (u64 a : readInputs(argv[4]))
            {
                const auto [out, cand] = kr.eval(v, a);
                const u64 cost = (v == "LK" || v == "LKc" || v == "LDK")
                                     ? (a == 0 ? fz : fa) + fixupCostK(k, cand, out, kr.cap)
                                     : 0;
                std::printf("%llu %llu %llu\n", static_cast<unsigned long long>(out),
                            static_cast<unsigned long long>(cand),
                            static_cast<unsigned long long>(cost));
            }
            return 0;
        }
        if (mode == "--yerr" && argc == 4)
        {
            // effective exponent of lpow(x, 1/k): y_eff = exp2Frac(frac(L_y)) * 2^(I - 31), for
            // the ldiv-built 1/k (LK) and the correctly rounded constant (LKc), and the
            // predicted worst fix-up: cap * ln2 * 64 * |y_eff - 1/k| (+1 for output rounding)
            const int k = std::stoi(argv[2]);
            const int eb = std::stoi(argv[3]);
            const LnsConverter conv(11, eb);
            const KRoot kr(k, conv);
            for (const LnsWord w : {kr.inv, kr.invc})
            {
                const int32_t ly = fieldOf(w);
                const int32_t ip = ly >> kFracBits;
                const uint32_t fr = static_cast<uint32_t>(ly) & kFractionMask;
                const long double m = static_cast<long double>(conv.exp2Frac(fr));
                const long double y = std::ldexp(m, ip - 31);
                const long double dy = y - 1.0L / k;
                std::printf("%d %d %d %.6Le %.3Lf\n", k, eb, static_cast<int>(ly),
                            dy * k, static_cast<long double>(kr.cap) * std::log(2.0L) * 64.0L *
                                        std::fabs(dy));
            }
            return 0;
        }
        if (mode == "--ksweep" && argc == 4)
        {
            kSweep(std::stoull(argv[2]), argv[3]);
            return 0;
        }
        if (mode == "--sweep")
        {
            sweep(argc > 2 ? std::stoull(argv[2]) : 1000000);
            return 0;
        }
        std::fprintf(stderr, "usage: %s --eval <variant> <program.s> | --size <log> <exp> | "
                             "--sweep [n] | --keval <BK|LK|LKc|LDK|FK|FS> <k> <program.s> <fixed> <fixed a=0> | "
                             "--ksweep <n> <fixed costs>\n",
                     argv[0]);
        return 2;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
