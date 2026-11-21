// Relative error and host time of the FP32 <-> LNS conversions vs converter LUT size (no Qt).
//
// For 4 .. 22 index bits (log and exp LUTs the same size): worst and average relative error of
// fcvt.lns.s (random floats over 2^-60 .. 2^60) and fcvt.s.lns (random LNS words over the same
// range), and host ns per call (best of 7 runs of 2M calls). The last line is the relative error
// of plain FP32 rounding of the same values, the best any conversion to FP32 can do.
//
// Build (from the repo root, one command line):
//   g++ -std=c++20 -O2 -I src/processor/lns tools/lns_conversion_sweep.cpp
//       src/processor/lns/lns_conversion.cpp -o lns_conversion_sweep
// Run:  taskset -c 2 ./lns_conversion_sweep      (CSV on stdout)
#include "lns_conversion.h"
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
static double logOf(LnsWord w) { return fieldOf(w) / kScale; }
int main() {
  std::mt19937_64 rng(777); std::uniform_real_distribution<double> U(0, 1);
  std::vector<uint32_t> F(M); std::vector<LnsWord> W(M);
  for (int i = 0; i < M; ++i) { F[i] = std::bit_cast<uint32_t>((float)std::exp2(-60 + 120 * U(rng)));
    W[i] = pack(false, (int32_t)std::llround((-60 + 120 * U(rng)) * kScale)); }
  std::printf("bits,tolns_max,tolns_mean,tofloat_max,tofloat_mean,tolns_ns,tofloat_ns\n");
  for (int k = 4; k <= 22; k += (k < 14 ? 1 : 2)) {
    const LnsConverter c(k, k); double a = 0, as = 0, b = 0, bs = 0;
    for (int i = 0; i < M; ++i) {
      const double x = std::bit_cast<float>(F[i]);
      const double e1 = std::fabs(std::exp2(logOf(c.fromFloatBits(F[i])) - std::log2(x)) - 1); a = std::max(a, e1); as += e1;
      const double ex = std::exp2(logOf(W[i])); const double e2 = std::fabs(std::bit_cast<float>(c.toFloatBits(W[i])) - ex) / ex;
      b = std::max(b, e2); bs += e2; }
    const int N = 2000000;
    std::printf("%d,%.3e,%.3e,%.3e,%.3e,%.1f,%.1f\n", k, a, as / M, b, bs / M,
      ns([&](int i){ return c.fromFloatBits(F[i & (M-1)]); }, N), ns([&](int i){ return c.toFloatBits(W[i & (M-1)]); }, N)); }
  // FP32 rounding of the same exact values, for reference
  double r = 0, rs = 0; for (int i = 0; i < M; ++i) { const double ex = std::exp2(logOf(W[i])); const double e = std::fabs((float)ex - ex) / ex; r = std::max(r, e); rs += e; }
  std::printf("fp32 rounding,%.3e,%.3e\n", r, rs / M);
}
