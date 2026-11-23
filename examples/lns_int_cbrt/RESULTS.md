# TCP CUBIC cube root on KITES: integer kernel vs LNS vs FP64

Branch `Sheshadri-Rathan-` @ `84fb209`, KITES `rvss` (single-cycle: `cycle_count ==
instructions_retired`), default LNS tables (log 11 / exp 11 bits = 32 KiB) unless stated.
Every number below comes from a run I actually did: `kites-cli` for instruction counts and
simulator outputs, `tools/lns_cbrt_accuracy` (the bit-exact C++ model, 10^6 inputs per set)
for the large-sample accuracy columns. `check.py` verified that every simulator output matches
the model bit for bit, that the exact variants are exact, and that `instructions_retired`
equals the static model for all 125 runs. Full log: `check_output.txt`. Raw data:
`results.csv` (simulator) and `model_sweep.csv` (model).

## The comparison (section 3 of the brief, filled in)

Instructions per root are on `uniform_bits` (N = 1024, log-uniform u64), *without* the
4-instruction step-count instrumentation, and *including* the 6-instruction loop/memory overhead
(load, store, 2 pointer bumps, counter, branch) that every variant pays.

| ID | Program | Arithmetic | Exact? | Role | Instr/root | Exact % (uniform) | Max abs err |
|----|---------|-----------|--------|------|-----------:|------------------:|------------:|
| B0 | `cbrt_cubic_*.s` | RV64IM port of Linux v6.12 `cubic_root()` (software `fls64`, `divu`, `mul`) | no | primary integer baseline, as shipped | **46.33** | 45.41 | 2,005 (3,934 at 10^6) |
| B1 | `cbrt_cubic_exact_*.s` | B0 + 1 integer Newton step + mul-based ±1 fix-up | yes | integer baseline, same contract as L1 | **62.31** | 100 | 0 |
| L0 | `cbrt_lns_*.s` | `fcvt.lns.lu` → `lpow(x, 1/3)` → `fcvt.lu.lns` | no | raw LNS | **9.00** | 52.73 | 3 |
| L1 | `cbrt_lns_exact_*.s` | L0 + the same fix-up as B1 | yes | **primary LNS variant** | **18.23** | 100 | 0 |
| L2 | `cbrt_lns_logdiv_*.s` | `fcvt.lns.lu`, `lsw`/`lwu`, log field ÷ 3 (reciprocal `mul`), `sw`/`llw`, `fcvt.lu.lns`, fix-up | yes | is `lpow`'s inexact 1/3 the error source? | **24.51** | 100 | 0 |
| F0 | `cbrt_fp64_*.s` | `fcvt.d.lu`, bit-trick guess, 3 FP64 Newton steps, `fcvt.lu.d`, fix-up | yes | strongest non-LNS real-number baseline | **36.51** | 100 | 0 |

The fair comparisons, one sentence each:

- **B1 vs L1 (headline, exact vs exact):** L1 needs **18.23** instructions per root against
  B1's **62.31**, i.e. **3.42× fewer** (12.23 vs 56.31 if the shared loop overhead is removed,
  4.6× fewer). Both are 100% exact on all 3,478 simulator inputs and on 6×10^6 model inputs.
- **B0 vs L0 (approx vs approx):** L0 needs **9.00** against **46.33** (5.1× fewer). The two are
  wrong in different ways. B0's error grows with magnitude (max 3,934 units at 64-bit inputs).
  L0 is within 3 units everywhere, but it rounds to nearest instead of truncating. So against
  the floor it is exact *less* often than B0 on `small` (46.9% vs 86.8%) and `near2^22`
  (67.5% vs 78.6%), and its mean relative error against the floor on `uniform_bits` is
  *worse*: 3.61e-2 vs 2.96e-2.
- **L0 vs L1 (cost of exactness in LNS):** the fix-up adds **9.23** instructions per root, which
  is **more than the whole LNS part (3 instructions)**. Half of L1's cost is integer fix-up.
- **L1 vs L2 (does the inexact 1/3 matter?):** yes, for accuracy. L0's error comes from the
  exponent, not the tables. Above 10 table bits, L1's worst case stays at +3 steps (on
  u64max, 2.03 steps per root on average) even with 23/22-bit tables. L2's direct log ÷ 3 is
  never more than 1 off at ≥ 10 bits. It costs more instructions, though (24.51 vs 18.23):
  there is no L↔X register move, so the log has to make a memory round trip (`lsw`, `lwu`,
  `sw`, `llw`).
- **F0 (FP64):** L1 uses **2.0× fewer** instructions than plain FP64 Newton (18.23 vs 36.51).
  But F0 already beats the integer B1 by 1.7× with *no new ISA*, so a large part of the
  "LNS vs integer" gap is really "any real-number hardware vs integer-only".

## Where the result does NOT favour LNS (read this before presenting)

1. **Small tables break L1.** With 6-bit tables (1 KiB), L1 costs **153.17** instructions per
   root (max 573 fix-up steps). That is **2.5× worse than B1**. With 7 bits (2 KiB) it is
   31.71, which still beats B1. L1 only reaches its plateau (~18.2) at **≥ 9–10 bits
   (8–16 KiB of tables)**. B0's table is 64 bytes.
2. **L0 is not "more accurate" across the board.** Against the floor it loses to B0 on `small`,
   `near2^22` and on mean relative error over `uniform_bits` (see above). On `u64max`, L0 is
   exact 0% of the time and B0 is too. L0's win is max error (3 vs 3,934) and large inputs.
3. **The LNS part is only 3 of L1's 18.23 instructions.** Fix-up is 9.23 (51%) and loop is 6
   (33%). The win comes entirely from `lpow` being *one instruction*. That is only a hardware
   win if `lpow` is cheap. Nobody has costed it yet (see the cost model below).
4. **`u64max` is L1's worst set:** 28.61 instructions per root, against 22.21 for L2 and 34.21
   for F0. That is still 2.3× better than B1 (66.18).
5. **The integer baseline has no `clz`.** Software `fls64` costs B0/B1 **21.50** instructions per
   root. *Computed, not measured:* with a Zbb `clz` (about 2 instructions: `clz` + `sub` from
   64), B0 ≈ 26.8 and B1 ≈ 42.8. L1 would still win (18.23), but by 2.3× instead of 3.4×.
6. **B1 is my design, and it is the generous one.** The brief's literal "B0 + ±1 fix-up" would
   take 55 steps per root on average (model, uniform_bits) and up to 3,934 steps. That would
   make LNS look ~10× better. I rejected that as a strawman (see NOTES.md).
7. **B0 never beats L1.** B0 never beats L1 on instruction count, at any batch size, on any
   set. I checked this because the brief asked. It is a *count*, not a speed.

## A. Instruction counts

Instructions per root (uninstrumented) on every input set, simulator, default tables:

| set | N | B0 | B1 | L0 | L1 | L2 | F0 |
|---|---:|---:|---:|---:|---:|---:|---:|
| small | 1001 | 44.19 | 60.10 | 9.00 | 17.47 | 24.46 | 36.47 |
| near2^22 | 772 | 47.81 | 63.81 | 9.00 | 17.67 | 24.67 | 36.67 |
| cubes | 609 | 49.11 | 64.93 | 9.00 | 22.18 | 24.64 | 36.66 |
| u32max | 34 | 47.50 | 63.50 | 9.00 | 18.00 | 25.00 | 37.00 |
| u64max | 38 | 52.97 | 66.18 | 9.00 | 28.61 | 22.21 | 34.21 |
| uniform_bits | 1024 | 46.33 | 62.31 | 9.00 | 18.23 | 24.51 | 36.51 |

`instr_per_root = (instructions_retired - fixed) / N`, where fixed = setup + exit. The measured
fixed costs are: B0 11, B1 16, L0 12, L1 14, L2 14, F0 15. The instrumented programs (with step
counting) are exactly 4.00 per root higher. Both are in `results.csv`.

**Break-even vs batch size** (total `instructions_retired`, uninstrumented batch programs, first
N inputs of `uniform_bits`):

| N | B0 | B1 | L0 | L1 | L2 | F0 | L1 < B1? | L1 < B0? |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1 | 55 | 76 | 21 | 32 | 39 | 52 | yes | yes |
| 4 | 198 | 267 | 48 | 84 | 112 | 161 | yes | yes |
| 16 | 748 | 1009 | 156 | 296 | 407 | 600 | yes | yes |
| 64 | 3017 | 4046 | 588 | 1154 | 1585 | 2353 | yes | yes |
| 256 | 11917 | 16015 | 2316 | 4638 | 6299 | 9370 | yes | yes |
| 1024 | 47455 | 63818 | 9228 | 18682 | 25113 | 37402 | yes | yes |

**Break-even N = 1.** There is nothing to amortise. L1's setup (14, including building 1/3 with
`fcvt.lns.w` ×2 + `ldiv`) is *smaller* than B1's (16: table address, 3 constants, constant-pool
load). "Setup amortisation" is not an argument either way for this kernel.

## B. Overhead breakdown (dynamic ablation = static model, exactly)

Each ablation program adds one phase to the previous one (same inputs, `uniform_bits`, N = 1024,
no step counting). Phase cost = difference of `instructions_retired` / 1024. The static model
(blocks × path counts from `gen_cbrt.py`) gives **identical** values for every phase. The
"fixed" and "loop" lines come from the copy-only program at N = 1 and N = 1024.

| phase (per root) | B0 | B1 | L0 | L1 | L2 | F0 |
|---|---:|---:|---:|---:|---:|---:|
| fixed (setup + exit, per program) | 11 | 16 | 12 | 14 | 14 | 15 |
| loop + memory | 6.000 | 6.000 | 6.000 | 6.000 | 6.000 | 6.000 |
| convert in | – | – | 1.000 | 1.000 | 1.000 | 1.000 (`fcvt.d.lu`) |
| compute: `fls64` | 21.500 | 21.500 | – | – | – | – |
| compute: table lookup | 12.461 | 12.461 | – | – | – | – |
| compute: Newton (kernel) | 6.371 | 6.371 | – | – | – | – |
| compute: extra Newton (B1) | – | 7.000 | – | – | – | – |
| compute: `lpow` / log ÷ 3 / FP64 Newton | – | – | 1.000 | 1.000 | 8.000 | 20.000 |
| convert out | – | – | 1.000 | 1.000 | 1.000 | 1.000 (`fcvt.lu.d`) |
| fix-up | – | 8.975 | – | 9.230 | 8.511 | 8.511 |
| **total per root** | **46.332** | **62.307** | **9.000** | **18.230** | **24.511** | **36.511** |

**Fix-up step distribution** (final − candidate; model, 10^6 `uniform_bits` inputs, default tables):

| variant | −6..−2 | −1 | 0 | +1 | +2 | +3 | mean \|steps\| |
|---|---:|---:|---:|---:|---:|---:|---:|
| B1 | 2,895 | 31,889 | 965,216 | 0 | 0 | 0 | 0.0389 |
| L1 | 0 | 403,723 | 526,205 | 46,723 | 21,088 | 2,261 | 0.4994 |
| L2 | 0 | 491,565 | 508,435 | 0 | 0 | 0 | 0.4916 |
| F0 | 0 | 490,404 | 509,596 | 0 | 0 | 0 | 0.4904 |

The simulator's per-input `steps:` arrays match the model on every input (`check.py`). The −1
steps in L1/L2/F0 are the round-to-nearest of the final convert: about half of all roots round
up, and each needs one step back. Per set (simulator, mean / max steps): L1 u64max 2.03 / 3,
cubes 0.80 / 3. L2 and F0 are never more than 1 step off.

**Conversion overhead.** For L1, (convert in + convert out) / per-root cost = 2 / 18.23 =
**11.0%** (2 / 12.23 = 16.4% without the loop overhead). For Lennard-Jones (`lj_lns.s`,
**28,251** instructions retired on this build), the one-time `cvt_loop` (3 setup + 81 × 7) plus
the final `fcvt.lns.w`, `li` and `fcvt.s.lns` is 573 instructions = **2.03%**. Counting only the
83 conversion instructions themselves, it is 0.29%. The difference is structural: LJ converts
once and stays in the log domain for O(N²) work, while a cube root is convert → 1 op → convert.
Two of the three LNS instructions in L1 are conversions.

## C. Accuracy against Python's exact integer floor cube root

Simulator sets (all inputs):

| set | B0 exact % | B0 max err | B0 mean rel | L0 exact % | L0 = round(cbrt) % | L0 max err | L0 mean rel | B1 / L1 / L2 / F0 exact % |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| small | 86.81 | 1 | 3.41e-02 | 46.85 | 100.00 | 1 | 8.88e-02 | 100 / 100 / 100 / 100 |
| near2^22 | 78.63 | 1 | 1.22e-03 | 67.49 | 99.87 | 1 | 1.78e-03 | 100 / 100 / 100 / 100 |
| cubes | 17.57 | 3663 | 5.94e-03 | 42.86 | 53.69 | 3 | 5.71e-03 | 100 / 100 / 100 / 100 |
| u32max | 100.00 | 0 | 0 | 100.00 | 100.00 | 0 | 0 | 100 / 100 / 100 / 100 |
| u64max | 0.00 | 990 | 1.64e-04 | 0.00 | 0.00 | 3 | 7.80e-07 | 100 / 100 / 100 / 100 |
| uniform_bits | 45.41 | 2005 | 2.96e-02 | 52.73 | 87.79 | 3 | 3.61e-02 | 100 / 100 / 100 / 100 |

Model, 10^6 inputs per set (`model_sweep.csv`, default tables): B0 max error is 3,937 on `cubes`,
3,934 on `uniform_bits` and 990 on `u64max`. L0 max error is 3, 3 and 2. L2's raw output (before
fix-up) has max error 1 on every set and equals round(cbrt) 99.25% of the time on
`uniform_bits`. B1, L1, L2 and F0 are 100.000000% exact on all six sets (≈ 6×10^6 inputs).

**The kernel's own claim ("Avg err ~= 0.195%")**, measured as mean |B0 − cbrt(a)| / cbrt(a)
against the real cube root:

| input bit length | 1–8 | 9–16 | 17–24 | 25–32 | 33–40 | 41–48 | 49–56 | 57–64 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| mean rel err (4,000 samples each) | 9.58% | 3.31% | 0.51% | 0.080% | 0.032% | 0.027% | 0.030% | 0.029% |

On `uniform_bits` as a whole it is **1.73%** (model, 10^6), so the claim **does not hold as a
uniform average over u64**. It holds, with margin, above about 2^24 (0.03–0.08%), which is the
range TCP CUBIC actually feeds it (`cube_factor`-scaled values). Below that, any integer answer
has large relative error. The claim is best read as "for the inputs CUBIC uses".

## D. Dynamic instruction mix (per root, `uniform_bits`, uninstrumented)

Static per-iteration mix × path counts. Each program's total equals `instructions_retired`
(`mix.json`).

| class | B0 | B1 | L0 | L1 | L2 | F0 |
|---|---:|---:|---:|---:|---:|---:|
| integer ALU | 30.602 | 34.603 | 3.000 | 4.182 | 6.000 | 6.000 |
| `mul` / `mulw` / `mulhu` | 2.730 | 8.732 | 0 | 4.182 | 5.000 | 5.000 |
| `divu` | 0.910 | 1.910 | 0 | 0 | 0 | 0 |
| branches + `j` | 9.090 | 14.062 | 1.000 | 4.867 | 5.511 | 4.511 |
| loads / stores | 3.000 | 3.000 | 2.000 | 2.000 | 4.000 | 2.000 |
| LNS converts | 0 | 0 | 2.000 | 2.000 | 2.000 | 0 |
| `lpow` | 0 | 0 | 1.000 | 1.000 | 0 | 0 |
| `llw` / `lsw` | 0 | 0 | 0 | 0 | 2.000 | 0 |
| FP (cvt 2, move 2, add 6, mul 6, div 3) | 0 | 0 | 0 | 0 | 0 | 19.000 |
| **total** | 46.332 | 62.307 | 9.000 | 18.230 | 24.511 | 36.511 |

(`divu` in B0 is 0.91 rather than 1 because inputs below 64 take the direct table path.)

## E. Memory

- **LNS converter tables** (log2(1+x) + 2^f, value + slope per entry): 6 bits 1 KiB, 7 bits
  2 KiB, 8 bits 4 KiB, 9 bits 8 KiB, 10 bits 16 KiB, **11 bits 32 KiB (default)**, 12 bits
  64 KiB, 13 bits 128 KiB, 23/22 bits 96 MiB (`lns_cbrt_accuracy --size`).
- **Add/sub ALU table: never read by this kernel.** `ldiv` (setup only) is an exact log
  subtraction. All 18 LNS main programs give identical outputs and instruction counts with
  `LNS_ALU_CORRECTION_SAMPLES=2` (the smallest legal value; 0 is rejected by `LnsAlu`).
- **B0/B1 table:** 64 bytes (`v[]`).
- **Code size** (instruction words from kites-cli, main / uninstrumented): B0 69 / 69,
  B1 103 / 97, L0 21 / 21, L1 45 / 39, L2 52 / 46, F0 65 / 59.

Table-size sweep (simulator, `uniform_bits`, N = 1024, uninstrumented per root):

| bits (log/exp) | table bytes | L0 exact % | L0 max err | L1 instr/root | L1 max steps | L2 instr/root | L2 max steps |
|---|---:|---:|---:|---:|---:|---:|---:|
| 6/6 | 1,024 | 25.59 | 573 | **153.17** | 573 | 28.58 | 30 |
| 7/7 | 2,048 | 33.01 | 65 | 31.71 | 65 | 25.19 | 8 |
| 8/8 | 4,096 | 35.25 | 25 | 22.66 | 25 | 24.54 | 3 |
| 9/9 | 8,192 | 39.06 | 7 | 18.48 | 7 | 24.50 | 1 |
| 10/10 | 16,384 | 52.64 | 3 | 18.19 | 3 | 24.50 | 1 |
| 11/11 | 32,768 | 52.73 | 3 | 18.23 | 3 | 24.51 | 1 |
| 12/12 | 65,536 | 52.64 | 3 | 18.25 | 3 | 24.51 | 1 |
| 13/13 | 131,072 | 52.73 | 3 | 18.24 | 3 | 24.51 | 1 |
| 23/22 | 100,663,296 | 52.83 | 3 | 18.23 | 3 | 24.51 | 1 |

The 10^6-input model sweep (6..16 bits plus 22 and 23/22, all six sets) is in `model_sweep.csv`.
It shows the same picture: L1's worst case is stuck at +3 from 10 bits up to 23/22. That is the
Q9.22 quantisation of the 1/3 exponent itself (−6,647,815 vs the ideal −6,647,814.56), times a
log of up to 64, and no table size removes it.

## F. Hardware-cost sensitivity (MODEL, not a measurement)

`tools/cbrt_cost_model.py` multiplies the mix in D by a latency table
(`tools/cbrt_cost_latencies.json`) and sums it serially (no pipelining, no overlap).
**These latencies are assumptions.** Only integer `mul` = 3 cycles and `div` = 6–68 cycles
(operand-dependent) come from a datasheet: the SiFive U74 Core Complex Manual, Table 6
(<https://www.scs.stanford.edu/~zyedidia/docs/sifive/sifive-u74.pdf>). ALU, branch, load/store
= 1, FP add/mul = 4, `fdiv.d` = 20, FP convert = 3 and **every LNS latency** are round-number
guesses. Swept: `div` ∈ {6, 20, 40, 68}, LNS convert ∈ {1, 3, 6}, `lpow` ∈ {3, 10, 30}.

- L1/B1 weighted cost ranges from **0.14** (div 68, convert 1, lpow 3) to **0.73**
  (div 6, convert 6, lpow 30). B1 never wins inside the swept range.
- Break-even: with everything else at default, B1 wins only if one `lpow` costs more than
  **≈ 62 cycles** (div = 6), 89 (div = 20), 127 (div = 40) or 180 (div = 68).
- F0 is the worst under this model (143.5 "cycles" per root, from 3 `fdiv.d` at an assumed
  20). This flips the instruction-count ranking of F0 vs B0/B1, which shows how much the FP
  verdict depends on unknown latencies.

## G. Test vectors and test cases

- Simulator: **3,478** distinct inputs (small 1,001, near2^22 772, cubes 609, u32max 34,
  u64max 38, uniform_bits 1,024) × 6 variants = **20,868** per-input output checks on the main
  programs. Plus 45 ablation and 36 batch programs and 18 ALU-table re-runs: **125 kites-cli
  runs**, each checked for exact output, model match and `instructions_retired == static
  model`. All passed (`check_output.txt`).
- C++ model: ≈ **6×10^6** inputs per configuration (10^6 per set; cubes 999,999) × 13 table
  configurations, each input evaluated for L0/L1 and L2 (plus B0/B1/F0 once), all checked
  against the exact floor.
- GoogleTest `test/lns_cbrt_kernel_test.cpp`: **21** edge-input cases (0, 1, 2, 7, 8, 9, 26,
  27, 63, 64, 2^21, 2^22 − 1, 2^22, 2^22 + 1, 2^32 − 1, 2^32, 2642245³ − 1, 2642245³,
  2642245³ + 1, 2^63, 2^64 − 1). All pass, built on Linux with the `test_main.cpp` guard
  described in NOTES.md.

## What must not be claimed

- **Simulator wall-clock time is not an LNS-vs-integer comparison.** KITES runs integer/FP ops
  natively and emulates every LNS instruction in C++. No wall time is tabulated here.
- **Fewer instructions is not faster hardware.** Instruction count is the measured metric.
  Section F is a labelled model with assumed latencies.
- **L0 is approximate.** Never quote L0's 9.00 instructions per root as "the LNS win" without
  its error next to it. The exact comparison is **B1 (62.31) vs L1 (18.23)**.
- B0 (approximate) does **not** beat L1 (exact) on instruction count on any set or batch size.
  If the tables are shrunk to 6 bits, L1 (153.17) loses to both B0 and B1.

## Reproduce

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release . && cmake --build build --target kites_cli
g++ -std=c++20 -O2 -I src/processor/lns tools/lns_cbrt_accuracy.cpp src/processor/lns/lns_alu.cpp \
    src/processor/lns/lns_conversion.cpp src/processor/lns/lns_pow.cpp -o lns_cbrt_accuracy
python3 examples/lns_int_cbrt/gen_cbrt.py
KITES_CLI=build/kites-cli CBRT_MODEL=./lns_cbrt_accuracy python3 examples/lns_int_cbrt/check.py
KITES_CLI=build/kites-cli CBRT_MODEL=./lns_cbrt_accuracy examples/lns_int_cbrt/sweep.sh
./lns_cbrt_accuracy --sweep > examples/lns_int_cbrt/model_sweep.csv
python3 tools/cbrt_cost_model.py
```
