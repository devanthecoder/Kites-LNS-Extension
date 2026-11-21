// Accuracy and host time of the CORDIC add/sub (laddc/lsubc) on random inputs (no Qt).
//
// Uses the same inputs as the ALU part of lns_table_sweep, so the results compare directly with
// the table-based ladd/lsub. Errors are in LSB of the log (x 1.6526e-7 for relative error);
// lsubc skips differences below 1/64 (near-cancellation). Times are host ns per call.
//
// Build (from the repo root, one command line):
//   g++ -std=c++20 -O2 -I src/processor/lns tools/lns_cordic_bench.cpp
//       src/processor/lns/lns_cordic_addsub.cpp -o lns_cordic_bench
// Run:  taskset -c 2 ./lns_cordic_bench
#include "lns_cordic_addsub.h"
#include "lns_format.h"
#include <algorithm>
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
  std::mt19937_64 rng(12345); std::uniform_real_distribution<double> U(0.0, 1.0);
  std::vector<LnsWord> A(M), B(M);
  for (int i = 0; i < M; ++i) { const double la = -20 + 40 * U(rng), d = 24 * U(rng);
    A[i] = pack(false, (int32_t)std::llround(la * kScale)); B[i] = pack(false, (int32_t)std::llround((la - d) * kScale)); }
  double amax = 0, asum = 0, smax = 0, ssum = 0; int sc = 0;
  for (int i = 0; i < M; ++i) {
    const double la = logOf(A[i]), lb = logOf(B[i]);
    const double e = std::fabs(logOf(LnsCordicAddSub::add(A[i], B[i])) - std::log2(std::exp2(la) + std::exp2(lb))) * kScale;
    amax = std::max(amax, e); asum += e;
    if (la - lb > 1.0 / 64) { const double es = la + std::log2(1 - std::exp2(lb - la));
      const double f = std::fabs(logOf(LnsCordicAddSub::sub(A[i], B[i])) - es) * kScale; smax = std::max(smax, f); ssum += f; ++sc; } }
  const int N = 250000;
  const double ta = ns([&](int i){ return LnsCordicAddSub::add(A[i & (M-1)], B[(i*7+13) & (M-1)]); }, N);
  const double ts = ns([&](int i){ return LnsCordicAddSub::sub(A[i & (M-1)], B[(i*7+13) & (M-1)]); }, N);
  std::printf("laddc max %.3f mean %.3f | lsubc max %.3f mean %.3f | ns add %.1f sub %.1f avg %.1f\n", amax, asum / M, smax, ssum / sc, ta, ts, (ta + ts) / 2);
}
