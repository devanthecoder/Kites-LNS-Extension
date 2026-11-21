// Host time per call of the LNS operations that do not depend on table size (no Qt).
//
// FP32 add/div (host hardware, for scale), lmul/ldiv, lpown, laddc/lsubc and the int32
// conversions, on random inputs (best of 7 runs). Host time of the library, not hardware latency.
//
// Build (from the repo root, one command line):
//   g++ -std=c++20 -O2 -I src/processor/lns tools/lns_op_bench.cpp src/processor/lns/lns_alu.cpp
//       src/processor/lns/lns_conversion.cpp src/processor/lns/lns_pow.cpp
//       src/processor/lns/lns_cordic_addsub.cpp -o lns_op_bench
// Run:  taskset -c 2 ./lns_op_bench
#include "lns_alu.h"
#include "lns_conversion.h"
#include "lns_cordic_addsub.h"
#include "lns_pow.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace Kites::lns;
using Clk = std::chrono::steady_clock;
static volatile uint32_t sink;
constexpr int M = 1 << 16;
template <class F> double ns(F f, int reps) { double best = 1e30;
  for (int t = 0; t < 7; ++t) { auto a = Clk::now(); uint32_t acc = 0; for (int i = 0; i < reps; ++i) acc += f(i); sink = acc;
    best = std::min(best, std::chrono::duration<double, std::nano>(Clk::now() - a).count() / reps); } return best; }
int main() {
  std::mt19937_64 rng(12345); std::uniform_real_distribution<double> U(0, 1);
  std::vector<LnsWord> A(M), B(M); std::vector<uint32_t> FA(M), FB(M); std::vector<int32_t> I(M);
  for (int i = 0; i < M; ++i) { double la = -20 + 40 * U(rng), d = 24 * U(rng);
    A[i] = pack(false, (int32_t)std::llround(la * kScale)); B[i] = pack(false, (int32_t)std::llround((la - d) * kScale));
    FA[i] = std::bit_cast<uint32_t>((float)std::exp2(la)); FB[i] = std::bit_cast<uint32_t>((float)std::exp2(la - d));
    I[i] = (int32_t)(rng() >> 33) - (1 << 30); }
  const LnsConverter c; const int N = 2000000;
  auto a = [&](int i) { return A[i & (M - 1)]; }; auto b = [&](int i) { return B[(i * 7 + 13) & (M - 1)]; };
  std::printf("fp32 add,%.1f\n", ns([&](int i){ return std::bit_cast<uint32_t>(std::bit_cast<float>(FA[i&(M-1)]) + std::bit_cast<float>(FB[(i*7+13)&(M-1)])); }, N));
  std::printf("fp32 div,%.1f\n", ns([&](int i){ return std::bit_cast<uint32_t>(std::bit_cast<float>(FA[i&(M-1)]) / std::bit_cast<float>(FB[(i*7+13)&(M-1)])); }, N));
  std::printf("lmul/ldiv,%.1f\n", (ns([&](int i){ return LnsAlu::mul(a(i), b(i)); }, N) + ns([&](int i){ return LnsAlu::div(a(i), b(i)); }, N)) / 2);
  std::printf("lpown n=-3,%.1f\n", ns([&](int i){ return powerN(a(i), -3); }, N));
  std::printf("laddc/lsubc,%.1f\n", (ns([&](int i){ return LnsCordicAddSub::add(a(i), b(i)); }, N/8) + ns([&](int i){ return LnsCordicAddSub::sub(a(i), b(i)); }, N/8)) / 2);
  std::printf("fcvt.lns.w / fcvt.w.lns,%.1f / %.1f\n", ns([&](int i){ return c.fromInt32(I[i&(M-1)]); }, N), ns([&](int i){ return (uint32_t)c.toInt32(a(i)); }, N));
}
