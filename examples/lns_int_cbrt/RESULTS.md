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
- Configuration sweep (section H): **871** simulator runs over **133,788** roots (k = 2..6 ×
  5 representations × 13 input sets × table sizes) and **540** independent log × exp runs, all
  verified; C++ model **11,858** configurations × 200,000 inputs.
- Section I (2026-11-24): **54** simulator runs at 14–16-bit converters, **108** add/sub-table
  runs (6 sizes, identical results), **195** constant-exponent runs (`config_sweep.csv` now
  holds 1,066 verified runs over 163,749 roots); C++ model grid widened to **21,168**
  configurations (144 table pairs × k = 2..8 × 7 sets × 3 variants) × 200,000 inputs. All
  passed.

## H. Configuration sweep: root degree, representation, input width, tables

The sections above answer one question: k = 3 against the Linux kernel's tuned `cubic_root()`.
This section tests whether that answer survives when the configuration changes, in the spirit
of the `lns_lj` sweep. It is produced by `sweep_configs.py`; raw data in `config_sweep.csv`
(simulator, committed) and `model_grid.csv` (C++ model, regenerated in about 2 minutes).

**What was swept**

| Axis | Values | Where |
|---|---|---|
| Root degree k (floor of a^(1/k), u64 input) | 2, 3, 4, 5, 6 (model also 7, 8) | simulator + model |
| Representation | integer Newton (BK), FP64 Newton (FK), FP64 `fsqrt.d` (FS, k = 2 only), LNS `lpow` (LK), LNS log-divide (LDK) | simulator |
| Input width | exactly w bits, w = 8, 16, 24, 32, 40, 48, 56, 64 (128 inputs each) | simulator (model: 16/32/48/64) |
| Input distribution | small 0..255, uniform bit length, uniform u64 value, perfect powers ±1, top of the u64 range | simulator + model |
| LNS tables | log bits × exp bits *independently*: simulator 6/8/10/11/13/23 × 6/8/10/11/13/22 for k = 3 (L0/L1/L2 in `results.csv`), diagonal for k = 2..6; model 11 × 11 pairs (6..16, 22/23) for every k | both |
| Batch size | break-even from setup + per-root cost (linear, as checked by the batch programs) | simulator |

**How it was checked.** 871 simulator runs (273 programs; LK/LDK at each table size) over
**133,788 roots**, plus 540 independent-grid runs through `check.py --rows`. Every run passed:
the output is the exact floor root, the output and fix-up candidate match the C++ model bit for
bit, `instructions_retired` equals the static model, and the recorded fix-up/iteration
histogram matches the model's. The model grid evaluates 11,858 configurations × 200,000
inputs each; its instruction counts use the fix-up cost formula that the simulator sweep
checked against every simulated root.

Variants (all exact; each "fix-up" is the cube-root fix-up loop with x^3 replaced by x^k):
BK = software `fls64`, seed 2^ceil(bits/k), integer Newton x' = ((k−1)x + a/x^(k−1))/k until
it stops decreasing (generic: no tuned lookup seed). LK = `fcvt.lns.lu` → `lpow(x, 1/k)` →
`fcvt.lu.lns` → fix-up. LDK = (log field + 1) / k by reciprocal multiply → fix-up. FK =
FP64 bit-trick seed + 3 Newton steps → fix-up. FS = `fsqrt.d` → fix-up.

**Findings, blunt**

1. **k = 3 is not a cherry-pick, for k ≥ 3.** At k = 3..6, LNS needs 3.4 to 4.4× fewer
   instructions per root than integer Newton and 1.5 to 2.2× fewer than FP64 Newton. That holds
   on every input width and distribution and at every table size from 1 KiB up (k ≥ 4) or
   from 2.5 KiB up (k = 3).
2. **k = 2 is where LNS loses, and no table size fixes it.** On 64-bit inputs LK costs
   **622** instructions per root (up to 575 fix-up steps), against **70** for integer Newton
   and **15** for `fsqrt.d`. Even with 23/22-bit tables the worst case is still 531 steps.
   The cause is the format, not the tables: Q9.22 keeps 22 fractional log bits, so a result
   with more than about 22 significant bits cannot be resolved. The square root of a 64-bit
   number has 32 bits. LK is fine up to 40-bit inputs, starts slipping at 48 (3 steps), and
   collapses at 56 (31) and 64 (491). At k = 3 the root has at most 21.3 bits, which is why
   the cube root just fits (+3 steps, the "1/3 quantisation" seen in section C).
3. **Against `fsqrt.d`, LNS never wins at k = 2.** At best it ties (15.5 vs 15.5 instructions
   for inputs up to 40 bits). If anyone asks "why not sqrt?", this is the answer: FP already
   has a one-instruction square root, and LNS's one-instruction `lpow` only pays off when
   no such instruction exists (k ≥ 3).
4. **The exp table matters, the log table barely does.** At k = 3, exp = 6 bits costs 153 to
   156 instructions per root *whatever the log size*, while log = 6 bits with exp ≥ 8 costs
   about 21. Log 8 / exp 10 (10 KiB) gets L1 to 18.4 against 18.2 at the default 11/11
   (32 KiB): **3.2× less table memory for 1% more instructions**. For k ≥ 4, 6/6 tables
   (1 KiB) are already enough.
5. **Half of every fix-up is wasted by rounding.** The LNS and FP candidates are exact only
   about 50% of the time, because the converts round to nearest. The rest are one step too
   high, so the fix-up almost always walks down once. A truncating `fcvt.lu.lns` would likely
   remove most of that. This is a guess I have not built or measured.
6. **LDK is only worth it for 64-bit cube roots.** It removes LK's +3 worst case at k = 3
   (24.6 vs 32.9 on 64-bit inputs) but costs 6 to 7 more instructions everywhere else (the
   memory round trip, because there is no L↔X move).
7. **Break-even batch size is 1 for every k.** LNS setup is 1 to 3 instructions more than
   integer setup, and that is repaid on the first root.
8. **The integer baseline is generic, so the gap is an upper bound.** BK uses a power-of-two
   seed. At k = 3 the kernel's tuned lookup-seeded version (B1, 62.3) is only 9% cheaper than
   BK (68.5), so a tuned seed for other k would probably save 10 to 20%, not 3×. Also, every
   instruction counts as 1 here, including `lpow`, `fsqrt.d`, `fdiv.d` and `divu`. Section F
   shows how latency assumptions change that.

#### H.A. Instructions per root by root degree (default 11/11 tables)

Input set `uniform_bits`:

| k | BK | FK | FS | LK | LDK | BK / best LNS | best FP / best LNS |
|---|---:|---:|---:|---:|---:|---:|---:|
| 2 | 59.7 | 27.5 | 15.5 | 35.2 | 41.2 | 1.70 | 0.44 |
| 3 | 68.5 | 36.5 | - | 18.1 | 24.6 | 3.79 | 2.02 |
| 4 | 67.3 | 41.5 | - | 19.5 | 25.5 | 3.45 | 2.13 |
| 5 | 76.4 | 46.5 | - | 21.5 | 28.5 | 3.56 | 2.16 |
| 6 | 81.4 | 51.5 | - | 23.5 | 30.5 | 3.47 | 2.19 |

Input set `uniform_u64`:

| k | BK | FK | FS | LK | LDK | BK / best LNS | best FP / best LNS |
|---|---:|---:|---:|---:|---:|---:|---:|
| 2 | 70.3 | 27.4 | 15.4 | 527.6 | 533.6 | 0.13 | 0.03 |
| 3 | 85.1 | 36.5 | - | 29.1 | 24.5 | 3.48 | 1.49 |
| 4 | 76.3 | 41.5 | - | 19.5 | 25.5 | 3.91 | 2.13 |
| 5 | 93.3 | 46.5 | - | 21.5 | 28.5 | 4.34 | 2.16 |
| 6 | 103.1 | 51.5 | - | 23.5 | 30.5 | 4.39 | 2.19 |

#### H.B. Input bit width (default tables): instructions per root

k = 2:

| width | BK | FK | FS | LK | LDK | LK max steps | LDK max steps |
|---|---:|---:|---:|---:|---:|---:|---:|
| 8 | 46.7 | 27.5 | 15.5 | 15.5 | 21.5 | 1 | 1 |
| 16 | 52.6 | 27.5 | 15.5 | 15.5 | 21.5 | 1 | 1 |
| 24 | 56.2 | 27.5 | 15.5 | 15.5 | 21.5 | 1 | 1 |
| 32 | 61.5 | 27.5 | 15.5 | 15.5 | 21.5 | 1 | 1 |
| 40 | 60.8 | 27.5 | 15.5 | 15.5 | 21.5 | 1 | 1 |
| 48 | 65.4 | 27.5 | 15.5 | 16.2 | 22.2 | 3 | 3 |
| 56 | 66.7 | 27.5 | 15.5 | 50.9 | 56.9 | 31 | 31 |
| 64 | 69.6 | 27.4 | 15.4 | 621.8 | 627.8 | 491 | 491 |

k = 3:

| width | BK | FK | LK | LDK | LK max steps | LDK max steps |
|---|---:|---:|---:|---:|---:|---:|
| 8 | 56.0 | 36.6 | 17.6 | 24.6 | 1 | 1 |
| 16 | 70.6 | 36.5 | 17.5 | 24.5 | 1 | 1 |
| 24 | 59.7 | 36.5 | 17.5 | 24.5 | 1 | 1 |
| 32 | 74.0 | 36.5 | 17.5 | 24.5 | 1 | 1 |
| 40 | 80.3 | 36.5 | 17.5 | 24.5 | 1 | 1 |
| 48 | 70.2 | 36.4 | 17.4 | 24.4 | 1 | 1 |
| 56 | 81.4 | 36.5 | 17.9 | 24.5 | 1 | 1 |
| 64 | 92.1 | 36.5 | 32.9 | 24.6 | 3 | 1 |

k = 6:

| width | BK | FK | LK | LDK | LK max steps | LDK max steps |
|---|---:|---:|---:|---:|---:|---:|
| 8 | 68.0 | 51.9 | 23.9 | 30.9 | 1 | 1 |
| 16 | 62.6 | 51.6 | 23.6 | 30.6 | 1 | 1 |
| 24 | 59.7 | 51.3 | 23.3 | 30.3 | 1 | 1 |
| 32 | 97.9 | 51.6 | 23.6 | 30.6 | 1 | 1 |
| 40 | 84.6 | 51.6 | 23.6 | 30.6 | 1 | 1 |
| 48 | 69.8 | 51.5 | 23.5 | 30.5 | 1 | 1 |
| 56 | 110.3 | 51.4 | 23.4 | 30.4 | 1 | 1 |
| 64 | 98.5 | 51.5 | 23.5 | 30.5 | 1 | 1 |

#### H.C. Fix-up / Newton-iteration distribution, all sweep sets pooled (11/11)

| k | variant | roots | candidate exact | mean abs steps | max abs steps | share with abs steps <= 1 |
|---|---|---:|---:|---:|---:|---:|
| 2 | BK (Newton iters) | 2010 | - | 2.74 | 6 | 18.1% |
| 2 | FK | 2010 | 50.0% | 0.50 | 1 | 100.0% |
| 2 | FS | 2010 | 50.0% | 0.50 | 1 | 100.0% |
| 2 | LK | 2010 | 33.6% | 35.46 | 575 | 68.6% |
| 2 | LDK | 2010 | 33.6% | 35.46 | 575 | 68.6% |
| 3 | BK (Newton iters) | 2004 | - | 3.16 | 6 | 10.9% |
| 3 | FK | 2004 | 51.6% | 0.48 | 1 | 100.0% |
| 3 | LK | 2004 | 44.6% | 0.74 | 3 | 84.1% |
| 3 | LDK | 2004 | 51.9% | 0.48 | 1 | 100.0% |
| 4 | BK (Newton iters) | 1998 | - | 2.27 | 6 | 31.5% |
| 4 | FK | 1998 | 47.7% | 0.52 | 1 | 100.0% |
| 4 | LK | 1998 | 47.7% | 0.52 | 1 | 100.0% |
| 4 | LDK | 1998 | 47.7% | 0.52 | 1 | 100.0% |
| 5 | BK (Newton iters) | 1992 | - | 3.15 | 6 | 8.1% |
| 5 | FK | 1992 | 46.4% | 0.54 | 1 | 100.0% |
| 5 | LK | 1992 | 46.3% | 0.54 | 1 | 100.0% |
| 5 | LDK | 1992 | 46.4% | 0.54 | 1 | 100.0% |
| 6 | BK (Newton iters) | 1983 | - | 3.02 | 7 | 17.8% |
| 6 | FK | 1983 | 58.5% | 0.42 | 1 | 100.0% |
| 6 | LK | 1983 | 58.5% | 0.41 | 1 | 100.0% |
| 6 | LDK | 1983 | 58.5% | 0.42 | 1 | 100.0% |

#### H.D. LNS table size (simulator, set uniform_bits): instructions per root

"(model)": k = 2 below 10 log bits was not simulated (up to 62,000 fix-up steps per root, hours per program); see H.F and `model_grid.csv`.

| k | variant | 6/6 | 8/8 | 10/10 | 11/11 | 13/13 | 23/22 | BK |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 2 | LK | (model) | (model) | 34.0 | 35.2 | 34.1 | 34.0 | 59.7 |
| 2 | LDK | (model) | (model) | 40.0 | 41.2 | 40.1 | 40.0 | 59.7 |
| 3 | LK | 113.6 | 21.0 | 18.0 | 18.1 | 18.1 | 18.1 | 68.5 |
| 3 | LDK | 27.7 | 24.5 | 24.5 | 24.6 | 24.5 | 24.5 | 68.5 |
| 4 | LK | 19.5 | 19.5 | 19.5 | 19.5 | 19.5 | 19.5 | 67.3 |
| 4 | LDK | 25.5 | 25.5 | 25.5 | 25.5 | 25.5 | 25.5 | 67.3 |
| 5 | LK | 21.4 | 21.5 | 21.5 | 21.5 | 21.5 | 21.5 | 76.4 |
| 5 | LDK | 28.5 | 28.5 | 28.5 | 28.5 | 28.5 | 28.5 | 76.4 |
| 6 | LK | 23.5 | 23.5 | 23.5 | 23.5 | 23.5 | 23.5 | 81.4 |
| 6 | LDK | 30.5 | 30.5 | 30.5 | 30.5 | 30.5 | 30.5 | 81.4 |

#### H.E. Independent log x exp tables, k = 3 (simulator, results.csv, uniform_bits)

L1 instructions per root (rows: log bits, columns: exp bits; B1 = 62.3):

| log \ exp | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 22 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 6 | 153.2 |  | 20.7 |  | 21.8 | 21.9 |  | 21.9 | 21.9 |
| 7 |  | 31.7 |  |  |  |  |  |  |  |
| 8 | 155.3 |  | 22.7 |  | 18.4 | 18.4 |  | 18.5 | 18.4 |
| 9 |  |  |  | 18.5 |  |  |  |  |  |
| 10 | 155.5 |  | 22.8 |  | 18.2 | 18.2 |  | 18.3 | 18.2 |
| 11 | 155.5 |  | 22.8 |  | 18.2 | 18.2 |  | 18.2 | 18.2 |
| 12 |  |  |  |  |  |  | 18.2 |  |  |
| 13 | 155.5 |  | 22.8 |  | 18.2 | 18.2 |  | 18.2 | 18.2 |
| 23 | 155.5 |  | 22.8 |  | 18.2 | 18.2 |  | 18.2 | 18.2 |

L2 instructions per root (rows: log bits, columns: exp bits; B1 = 62.3):

| log \ exp | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 22 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 6 | 28.6 |  | 27.2 |  | 27.7 | 27.8 |  | 27.8 | 27.8 |
| 7 |  | 25.2 |  |  |  |  |  |  |  |
| 8 | 30.0 |  | 24.5 |  | 24.5 | 24.6 |  | 24.6 | 24.6 |
| 9 |  |  |  | 24.5 |  |  |  |  |  |
| 10 | 30.1 |  | 24.6 |  | 24.5 | 24.5 |  | 24.5 | 24.5 |
| 11 | 30.1 |  | 24.6 |  | 24.5 | 24.5 |  | 24.5 | 24.5 |
| 12 |  |  |  |  |  |  | 24.5 |  |  |
| 13 | 30.1 |  | 24.6 |  | 24.5 | 24.5 |  | 24.5 | 24.5 |
| 23 | 30.1 |  | 24.6 |  | 24.5 | 24.5 |  | 24.5 | 24.5 |

#### H.F. C++ model grid: smallest tables that beat the integer baseline

Per k and input set (200,000 model inputs per set; BK is the simulated cost on the same-named 128/256-input simulator set): the lowest modelled LK cost over all 121 table pairs, the smallest table pair (bytes) at which LK is cheaper than BK, and the worst fix-up even with the largest tables.

| k | input set | BK (sim) | best LK (model) | at tables | smallest tables with LK < BK | max steps at 23/22 |
|---|---|---:|---:|---|---|---:|
| 2 | uniform_bits | 59.7 | 48.1 | 11/16 | 10/10 (16384 B) | 529 |
| 2 | uniform_u64 | 70.3 | 493.5 | 11/16 | none | 532 |
| 2 | width16 | 52.6 | 15.5 | 7/6 | 6/6 (1024 B) | 1 |
| 2 | width32 | 61.5 | 15.4 | 11/7 | 6/6 (1024 B) | 1 |
| 2 | width64 | 69.6 | 633.8 | 11/16 | none | 531 |
| 3 | uniform_bits | 68.5 | 18.1 | 7/9 | 6/7 (1536 B) | 3 |
| 3 | uniform_u64 | 85.1 | 27.7 | 7/9 | 6/8 (2560 B) | 3 |
| 3 | width16 | 70.6 | 17.5 | 6/6 | 6/6 (1024 B) | 1 |
| 3 | width32 | 74.0 | 17.3 | 8/6 | 6/6 (1024 B) | 1 |
| 3 | width64 | 92.1 | 30.5 | 7/9 | 6/8 (2560 B) | 3 |
| 4 | uniform_bits | 67.3 | 19.5 | 6/6 | 6/6 (1024 B) | 1 |
| 4 | uniform_u64 | 76.3 | 19.4 | 11/7 | 6/6 (1024 B) | 1 |
| 4 | width16 | 52.8 | 19.4 | 6/6 | 6/6 (1024 B) | 1 |
| 4 | width32 | 60.9 | 19.5 | 7/6 | 6/6 (1024 B) | 1 |
| 4 | width64 | 71.6 | 19.3 | 12/7 | 6/6 (1024 B) | 1 |
| 5 | uniform_bits | 76.4 | 21.5 | 8/7 | 6/6 (1024 B) | 1 |
| 5 | uniform_u64 | 93.3 | 21.4 | 8/7 | 6/6 (1024 B) | 1 |
| 5 | width16 | 81.0 | 21.6 | 6/6 | 6/6 (1024 B) | 1 |
| 5 | width32 | 90.9 | 21.5 | 6/6 | 6/6 (1024 B) | 1 |
| 5 | width64 | 88.4 | 21.3 | 9/7 | 6/6 (1024 B) | 1 |
| 6 | uniform_bits | 81.4 | 23.5 | 6/6 | 6/6 (1024 B) | 1 |
| 6 | uniform_u64 | 103.1 | 23.3 | 8/6 | 6/6 (1024 B) | 1 |
| 6 | width16 | 62.6 | 23.6 | 6/6 | 6/6 (1024 B) | 1 |
| 6 | width32 | 97.9 | 23.5 | 6/6 | 6/6 (1024 B) | 1 |
| 6 | width64 | 98.5 | 23.3 | 7/6 | 6/6 (1024 B) | 1 |

#### H.G. Break-even batch size per k (default tables, set uniform_bits)

Total = setup + N x per-root. Break-even N is the smallest batch at which LK's total is below BK's; 1 means LNS wins even for a single root.

| k | BK setup | BK/root | LK setup | LK/root | break-even N |
|---|---:|---:|---:|---:|---:|
| 2 | 14 | 59.7 | 17 | 35.2 | 1 |
| 3 | 14 | 68.5 | 16 | 18.1 | 1 |
| 4 | 14 | 67.3 | 16 | 19.5 | 1 |
| 5 | 14 | 76.4 | 16 | 21.5 | 1 |
| 6 | 14 | 81.4 | 15 | 23.5 | 1 |

## I. Converter sizing, exponent precision and a paper-based time estimate

Added 2026-11-24. Four questions: (1) how big must the converter tables be, (2) does the size of
the add/sub ALU table matter, (3) does the exponent ("power") given to `lpow` change anything,
and (4) using the research paper's hardware numbers, roughly how long would integer, FP and LNS
take? (1)–(3) are measured on the simulator and the C++ model; (4) is an **estimate**.

### I.A Converter tables (`LNS_LOG_LUT_BITS` × `LNS_EXP_LUT_BITS`)

New runs: simulator, k = 3, L0/L1/L2 on all six input sets at 14, 15 and 16 bits (54 runs, all
verified, added to `results.csv`; `sweep.sh` now sweeps 6..16 by default). C++ model: every
log × exp pair from 6..16 bits plus 23/22 (144 pairs) for k = 2..8, 7 input sets, 200,000
inputs each (`model_grid.csv`, 21,168 rows).

**Findings**

1. **Past 10 bits, bigger tables buy nothing.** L1 at 10 bits (16 KiB) costs 18.19
   instructions per root and at 16 bits (1 MiB) 18.24. The +3 worst fix-up stays from 10 bits
   to 23/22 (96 MiB).
2. **The exp table decides it, and now we know why.** `lpow(x, y)` gets the value of y from
   the exp table (`exp2Frac` of the fraction of log y), then multiplies log x by it. With a
   6-bit exp table the "1/3" it multiplies by is wrong by 1.4×10⁻⁵, whatever the log table.
   That error alone predicts the fix-up length (table I.C).
3. **For k ≥ 4 a 1 KiB converter (6/6) is within 1% of the best.** For k = 3 the best of all
   144 pairs is **log 7 / exp 9 (5 KiB)**, slightly cheaper than 23/22. That is error
   cancellation between the two tables on these inputs. Treat it as luck, not a design rule:
   log 6 / exp 10 is 2.6× worse than log 6 / exp 9. The safe recommendation for k = 3 is
   **log 8 / exp 10 (10 KiB)**, within 1% of 32 KiB on the simulator.
4. **For k = 2, no converter size helps.** The best of 144 pairs still needs 494 instructions
   per root on 64-bit inputs (section H).

#### I.A.1 Smallest converter tables within 1% / 5% of the best cost (C++ model, LK)

Over all 144 log x exp pairs (6..16 and 23 / 22 bits). Bytes = log table + exp table.

| k | input set | best instr/root | at | smallest within 1% | smallest within 5% | default 11/11 (32 KiB) |
|---|---|---:|---|---|---|---:|
| 2 | uniform_u64 | 493.5 | 11/16 | 11/12 (48 KiB, 497.3) | 10/11 (24 KiB, 500.6) | 515.1 |
| 2 | width32 | 15.4 | 11/7 | 6/6 (1 KiB, 15.4) | 6/6 (1 KiB, 15.4) | 15.5 |
| 2 | width64 | 633.8 | 11/16 | 11/12 (48 KiB, 638.7) | 10/11 (24 KiB, 643.0) | 661.5 |
| 3 | uniform_u64 | 27.7 | 7/9 | 7/9 (5 KiB, 27.7) | 7/9 (5 KiB, 27.7) | 29.2 |
| 3 | width32 | 17.3 | 8/6 | 6/6 (1 KiB, 17.3) | 6/6 (1 KiB, 17.3) | 17.5 |
| 3 | width64 | 30.5 | 7/9 | 7/9 (5 KiB, 30.5) | 7/9 (5 KiB, 30.5) | 32.5 |
| 4 | uniform_u64 | 19.4 | 11/7 | 6/6 (1 KiB, 19.5) | 6/6 (1 KiB, 19.5) | 19.5 |
| 4 | width32 | 19.5 | 7/6 | 6/6 (1 KiB, 19.5) | 6/6 (1 KiB, 19.5) | 19.5 |
| 4 | width64 | 19.3 | 12/7 | 6/7 (1 KiB, 19.5) | 6/6 (1 KiB, 19.6) | 19.5 |
| 5 | uniform_u64 | 21.4 | 8/7 | 6/7 (1 KiB, 21.4) | 6/7 (1 KiB, 21.4) | 21.5 |
| 5 | width32 | 21.5 | 6/6 | 6/6 (1 KiB, 21.5) | 6/6 (1 KiB, 21.5) | 21.5 |
| 5 | width64 | 21.3 | 9/7 | 6/7 (1 KiB, 21.3) | 6/7 (1 KiB, 21.3) | 21.5 |
| 6 | uniform_u64 | 23.3 | 8/6 | 6/6 (1 KiB, 23.3) | 6/6 (1 KiB, 23.3) | 23.5 |
| 6 | width32 | 23.5 | 6/6 | 6/6 (1 KiB, 23.5) | 6/6 (1 KiB, 23.5) | 23.5 |
| 6 | width64 | 23.3 | 7/6 | 6/6 (1 KiB, 23.3) | 6/6 (1 KiB, 23.3) | 23.5 |
| 7 | uniform_u64 | 25.4 | 8/6 | 6/6 (1 KiB, 25.4) | 6/6 (1 KiB, 25.4) | 25.5 |
| 7 | width32 | 25.5 | 6/6 | 6/6 (1 KiB, 25.5) | 6/6 (1 KiB, 25.5) | 25.5 |
| 7 | width64 | 25.4 | 6/6 | 6/6 (1 KiB, 25.4) | 6/6 (1 KiB, 25.4) | 25.4 |
| 8 | uniform_u64 | 27.3 | 7/6 | 6/6 (1 KiB, 27.3) | 6/6 (1 KiB, 27.3) | 27.3 |
| 8 | width32 | 27.4 | 6/6 | 6/6 (1 KiB, 27.4) | 6/6 (1 KiB, 27.4) | 27.4 |
| 8 | width64 | 27.0 | 7/6 | 6/6 (1 KiB, 27.0) | 6/6 (1 KiB, 27.0) | 27.0 |

#### I.A.2 Which table matters: k = 3, uniform_u64, LK instructions per root (C++ model)

| log \ exp | 6 | 7 | 8 | 9 | 10 | 11 | 13 | 16 | 22 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 6 | 1718.6 | 184.5 | 64.4 | 35.8 | 74.4 | 74.8 | 75.1 | 75.1 | 75.0 |
| 8 | 1743.0 | 209.0 | 88.8 | 32.5 | 31.6 | 32.0 | 32.3 | 32.3 | 32.1 |
| 10 | 1744.6 | 210.5 | 90.4 | 34.0 | 28.9 | 29.3 | 29.6 | 29.6 | 29.5 |
| 11 | 1744.6 | 210.6 | 90.4 | 34.1 | 28.8 | 29.2 | 29.4 | 29.4 | 29.3 |
| 13 | 1744.7 | 210.6 | 90.5 | 34.1 | 28.8 | 29.2 | 29.4 | 29.4 | 29.3 |
| 16 | 1744.7 | 210.6 | 90.5 | 34.1 | 28.8 | 29.1 | 29.4 | 29.4 | 29.3 |
| 23 | 1744.7 | 210.6 | 90.5 | 34.1 | 28.8 | 29.1 | 29.4 | 29.4 | 29.3 |

#### I.A.3 Simulator: converter bits 6..16 (k = 3, uniform_bits, log = exp bits)

| bits | table bytes | L1 instr/root | L1 max steps | L2 instr/root | L2 max steps |
|---:|---:|---:|---:|---:|---:|
| 6 | 1,024 | 153.17 | 573 | 28.58 | 30 |
| 7 | 2,048 | 31.71 | 65 | 25.19 | 8 |
| 8 | 4,096 | 22.66 | 25 | 24.54 | 3 |
| 9 | 8,192 | 18.48 | 7 | 24.50 | 1 |
| 10 | 16,384 | 18.19 | 3 | 24.50 | 1 |
| 11 | 32,768 | 18.23 | 3 | 24.51 | 1 |
| 12 | 65,536 | 18.25 | 3 | 24.51 | 1 |
| 13 | 131,072 | 18.24 | 3 | 24.51 | 1 |
| 14 | 262,144 | 18.24 | 3 | 24.51 | 1 |
| 15 | 524,288 | 18.24 | 3 | 24.51 | 1 |
| 16 | 1,048,576 | 18.24 | 3 | 24.51 | 1 |
| 23/22 | 100,663,296 | 18.23 | 3 | 24.51 | 1 |


### I.B The add/sub ALU table (`LNS_ALU_CORRECTION_SAMPLES`)

Simulator, k = 3, L0/L1/L2 on all six input sets with 2, 16, 256, 4,096 (default), 65,536 and
16,777,216 (the maximum) samples: **all 108 runs give identical outputs and identical
instruction counts.** The root kernel never adds or subtracts in LNS (`ldiv` in setup is an
exact log subtraction). So the ALU table can be any size for this application; its cost
belongs to Lennard-Jones, not here.

### I.C The exponent ("power")

Two experiments on the exponent 1/k given to `lpow`:

- **How 1/k is built does not matter.** Variant LKc loads a correctly rounded constant
  (round(−log2(k)·2²²)) instead of building 1/k with `fcvt.lns.w` ×2 + `ldiv`. For every k
  from 2 to 8 the two words are bit-identical, so LKc equals LK everywhere: 195 more
  simulator runs (k = 2..6, 13 input sets, tables 10/10, 11/11, 23/22, all verified, rows in
  `config_sweep.csv`) and all 7,056 model configurations.
- **The exponent's precision sets the fix-up length, and a one-line formula predicts it.**
  `./lns_cbrt_accuracy --yerr k e` prints the effective exponent `lpow` actually uses with an
  e-bit exp table. The worst fix-up is about cap_k × ln 2 × 64 × |y_eff − 1/k|. For k = 3:

| exp bits | error of effective 1/3 (relative) | predicted worst fix-up | measured (model, all sets) |
|---:|---:|---:|---:|
| 6 | 1.44e-05 | 561 | 597 |
| 7 | 1.53e-06 | 60 | 71 |
| 8 | 6.13e-07 | 24 | 28 |
| 9 | 1.56e-07 | 6 | 8 |
| 10 | 7.32e-08 | 2.9 | 3 |
| 11..22 | 7.3e-08 | 2.9 | 3 |

  The floor of 7.3×10⁻⁸ is the Q9.22 rounding of log2(3), and it shows up as the +3. For
  k = 2, 4 and 8 the exponent is exact (−1, −2, −3 in the log), so their fix-up comes from
  the output precision (section H), not from the exponent. For k = 5, 6 and 7 the exponent
  error times their small cap is below one step.
- **The root degree k itself** is in section H. LNS wins for k ≥ 3 and loses for square
  roots of inputs wider than about 44 bits.

### I.D Estimated time from the paper (ESTIMATE, not a measurement)

`scripts/latency_model.py` multiplies the measured per-root instruction mix by **one fixed
latency per instruction** (table I.E) and sums them serially. The fixed values come from
**B. Parhami, "Computing with logarithmic number system arithmetic", Computers and Electrical
Engineering 87 (2020) 106800** (the team's research paper) where it has them, and match the
Lennard-Jones table where it does not (LNS convert 4, multiply 4). Raw numbers:
`results/latency_estimates.csv` (the `A_cycles_fixed` column; `low`/`high` are the sensitivity
cases in I.F). A second, gate-delay version (scenario B, ns from the paper's Table 1) is in the
same file as a cross-check.

**k = 3, the TCP CUBIC variants (uniform_bits), fixed latencies:**

| variant | instructions per root | estimated cycles per root | gate-delay cross-check (ns) |
|---|---:|---:|---:|
| B0 kernel (approx) | 46.3 | 69.1 | 343 |
| B1 integer exact | 62.3 | 116.1 | 651 |
| L0 LNS (approx) | 9.0 | 22.0 | 160 |
| **L1 LNS exact** | 18.2 | 39.6 | 314 |
| L2 LNS log-divide | 24.5 | 40.5 | 318 |
| F0 FP64 Newton | 36.5 | 175.5 | 747 |

**Roots of degree k = 2..6 (cycles per root, fixed latencies):**

| k | input | integer BK | FP64 FK | `fsqrt` FS | LNS LK | LNS LDK |
|---|---|---:|---:|---:|---:|---:|
| 2 | 32-bit | 133 | 145 | 65 | 33 | 32 |
| 2 | 64-bit uniform | 163 | 144 | 64 | 867 | 866 |
| 3 | 64-bit uniform | 210 | 175 | – | 56 | 40 |
| 4 | 64-bit uniform | 188 | 194 | – | 45 | 44 |
| 5 | 64-bit uniform | 233 | 211 | – | 50 | 52 |
| 6 | 64-bit uniform | 275 | 230 | – | 56 | 58 |

**What the estimate says, bluntly**

1. **The instruction-count win survives.** L1 takes about **39.6 cycles** per root against
   **116.1** for the integer kernel (**2.9× faster**) and **175.5** for FP64
   (**4.4× faster**). By instruction count FP was the runner-up; by time it is the
   slowest, because each of its 3 divides costs about 30 cycles on the paper's FP chip.
2. **`lpow` is no longer the whole story.** At 8 cycles, L1 (one `lpow`) and L2 (no `lpow`, divide
   the log with `ldiv` instead) cost almost the same: 39.6 vs 40.5
   cycles. On 64-bit k = 3 inputs LDK is clearly cheaper (40 vs 56); for k = 4..6 the two are
   within a few cycles. If `lpow` turns out slow in hardware, the log-divide path is the fallback,
   and it needs no `lpow` at all.
3. **Square roots flip for narrow inputs.** By instruction count LNS only tied `fsqrt.d`
   (15.5 each). By time it wins for 32-bit inputs (33 vs
   65 cycles), because the paper's FP square root takes about 40 cycles.
   On 64-bit inputs it still loses by more than 10×.
4. **The gate-delay cross-check agrees in direction:** L1 314 ns vs B1
   651 ns vs F0 747 ns.

**Where the paper does not match KITES (say these before anyone else does)**

- **The paper has no converter or `lpow` latency.** It describes a converter (Kim et al.:
  leading-zero counter, shifter, 3 carry-save levels, a 26-bit adder) but gives no timing.
  Those are the two operations the LNS kernel depends on, so their values here are fixed guesses
  (convert 4, `lpow` 8) and section I.F shows what happens if they are wrong.
- **Different LNS format.** ELM uses 8 integer + 23 fraction bits; KITES uses 9 + 22. One
  more fraction bit would roughly halve the k = 3 and k = 2 fix-ups (inferred, not run). It
  would not rescue k = 2 on 64-bit inputs (about 265 steps instead of 530).
- **Different instruction set.** ELM has no integer↔LNS convert and no `lpow`. Its square
  root is a shift of the log, i.e. our LDK for k = 2 in one instruction. KITES needs a
  memory round trip for that because there is no L↔X register move.
- **The paper's FP is 32-bit; our F0 is FP64.** FP64 on the same chip would be slower, so
  the FP numbers here flatter F0.
- **Two machines, two technologies.** Scenario A mixes ELM cycles (125 MHz) for LNS with TMS
  cycles (150 MHz) for FP, as if one chip had both units. Scenario B uses 0.7 µm gate delays
  (Table 1) with 0.18 µm memory times (Table 3).
- **Serial sum, no pipelining.** The TMS is a pipelined VLIW, so independent roots could
  overlap and FP throughput would look better than this latency sum. The paper itself notes
  TMS wins on long vectors.
- **Integer latencies are not from the paper** (SiFive U74 datasheet in scenario A).
- **Power consumption cannot be modelled from the paper.** It only gives a qualitative
  "factor of about 2" saving for LNS multiply/divide-type circuits and 50% less switching
  activity. Nothing here estimates energy.

### I.E Every latency used, one fixed value per instruction

Raw: `results/latency_assumptions.csv`. Same format as the Lennard-Jones table. The estimate is a
serial sum over the single-cycle KITES core, so there is no "overlapped" (pipelined) column:
LNS only runs on that core and there is no pipeline to model (the LJ MD benchmark dropped it
for the same reason). LNS convert and `lpow` are fixed at 4 and 8 cycles for the default 11/11-bit
tables and scale with table size as described in I.G.

| Instruction | Used by | Latency (cycles) | Where the number comes from | Guess? |
|---|---|---:|---|---|
| Integer ALU (addi, add, sub, shifts, mv, sltiu, li, la) | all | 1 | Same core for every format (single-cycle integer units on ELM and TMS) | Assumption |
| Branches, jumps (beqz, bnez, bgeu, bltu, beq, j) | all | 1 | No branch penalty | Assumption |
| ld, lbu, lwu | all | 1 | Table 3, ELM load (cache hit) | From paper |
| sd, sw | all | 1 | Same core for every format (as in the LJ table) | Assumption |
| mul, mulw, mulhu | B0, B1; fix-up of L1, L2, F0 | 3 | SiFive U74 Core Complex Manual, Table 6 | Datasheet, not paper |
| divu | B0, B1 (Newton step) | 20 | SiFive U74 manual: 6-68 cycles depending on operands; 20 picked inside that range | Guess within a datasheet range |
| fcvt.lns.lu, fcvt.lu.lns, fcvt.lns.w | all LNS variants (2 per root) | 4 at 11/11 bits | 4 cycles at 11/11 bits (assumed: the paper gives no converter cycle count; as in the LJ table), scaled with table size by CACTI 7: only the table-read share 5/28 (Parhami 2020 section 4.2) scales. fcvt.lns.* reads the log table, fcvt.lu.lns the exp table | Guess, scaled by CACTI |
| lpow | L0, L1, LK (1 per root) | 8 at 11/11 bits | 8 cycles at 11/11 bits (derived): exp-table stage 4 (same work as an LNS->int convert) + log(x)*y multiply 4 (= LJ's lpown). Scaled with exp-table size by CACTI 7: only the exp-table stage's read share 5/28 (Parhami 2020 section 4.2) scales, the multiply does not | Derived, scaled by CACTI |
| ldiv | L2, LDK; setup of L1/LK | 1 | Table 3, ELM divide | From paper |
| llw, lsw | setup only (building 1/k) | 1 | Same core for every format (ELM: load 1, store 2) | Assumption |
| fadd.d | F0, FK | 4 | Table 3, TMS320C6711 add (an FP32 chip, used for FP64) | From paper |
| fmul.d | F0, FK | 4 | Table 3, TMS320C6711 multiply | From paper |
| fdiv.d | F0, FK (3 per root) | 30 | Table 3, TMS320C6711 divide (~30) | From paper |
| fsqrt.d | FS (k = 2 only) | 40 | Table 3, TMS320C6711 square root (~40) | From paper |
| fcvt.d.lu, fcvt.lu.d | F0, FK | 4 | Taken equal to the TMS add pipeline | Guess |
| fmv.x.d, fmv.d.x | F0 | 1 | Register move | Assumption |

**Why `lpow` is 8 and not 4.** LJ's `lpown` multiplies the log by an integer: one multiply, 4
cycles. KITES' `lpow` (`src/processor/lns/lns_pow.cpp`) first has to turn the exponent y back
into a number: it looks up the exp table *and* interpolates (`exp2Frac`: base + slope × remainder,
a second multiply), which is exactly the work an LNS→int convert does (4 cycles here), and only
then multiplies log(x) by it (4, as `lpown`). 4 + 4 = 8. A bare lookup with no interpolation
would give 5 (1 + 4); that is the low end of the sensitivity range.

**Differences from the LJ table, all deliberate:** integer `mul` (3) and `divu` (20) are not in
the LJ table, because the LJ kernel has none; they come from the SiFive U74 manual, not the
paper. `fsqrt.d` (40) and `fcvt.d.lu` / `fcvt.lu.d` (4) are likewise cbrt-only. The TMS numbers
are FP32 figures used for FP64, which flatters F0 (same caveat as LJ). The older U74-based model
(`tools/cbrt_cost_model.py`, section F) uses different guesses and is superseded by this table.

### I.F Sensitivity to the assumptions (kept out of the headline on purpose)

The headline uses the fixed values above. The guessed ones move it as follows (k = 3, cycles
per root, everything else fixed):

| What changes | L1 LNS | B1 integer | F0 FP64 |
|---|---:|---:|---:|
| Fixed set (headline) | 39.6 | 116.1 | 175.5 |
| All guesses optimistic (convert 1, `lpow` 5, `divu` 6) | 30.6 | 89.3 | 175.5 |
| All guesses pessimistic (convert 8, `lpow` 16, `divu` 68) | 55.6 | 207.8 | 175.5 |

- **`lpow` is probably slower than `lpown`**, because it adds the exp-table stage before the
  multiply. That is already in the fixed 8. **L1 still beats the integer kernel until one `lpow`
  costs about 84 cycles** (and FP64 until about 144), with everything else fixed.
- **Converters:** L1 beats integer until one convert costs about 42 cycles (FP64: about 72).
- **`divu`** only moves the integer baseline: 6–68 cycles puts B1 between 89 and
  208. L1 wins at every point of that range.
- Plot `plots/7b_sensitivity.png` shows both break-evens.

### I.G Converter table size: estimated time vs accuracy (CACTI-scaled, ESTIMATE)

**Question.** A bigger converter table gives a more accurate result (fewer fix-up steps, section
I.A) but is slower to read. Where is the best trade-off for the integer cube root? This is the
integer counterpart of the Lennard-Jones MD accuracy-vs-speed study.

**Method** (`scripts/latency_model.py`, the same method as `examples/lns_lj_md/table_latency.py`
on branch Devadath):

1. **CACTI 7** (<https://github.com/HewlettPackard/cacti>, commit `1ffd8df`) gives the access time
   of one 2^k-entry table (8-byte entries, single-ported RAM, 45 nm, the MD script's
   configuration) for k = 6..16: `results/cacti_table_latency.csv`, rows `conversion`. Check:
   2,048 entries (11 bits) = 0.3352 ns and 4,096 entries (12 bits) = 0.4185 ns, as in the MD run.
2. **Only the table-read share scales.** Parhami 2020 (section 4.2 / Table 1, citing Coleman &
   Chester 1999) puts a 5 ns ROM access inside the 28 ns LNS add, so the read is 5/28 of the
   operation: s(k) = 23/28 + 5/28 × access(k) / access(11).
3. fcvt.lns.* reads the **log** table and fcvt.lu.lns and `lpow` read the **exp** table:
   to_lns = 4 s(log), from_lns = 4 s(exp), lns_convert = (to_lns + from_lns) / 2 (one in and one
   out per root), lpow = 4 s(exp) + 4 (the exp-table stage scales; the multiply does not).
4. Each table size uses **its own measured instruction mix**: the static model of the N = 1024
   uniform_bits program with the C++ model's fix-up candidates at that table size. For every
   row it matches `results/results.csv` (simulator) exactly. No simulator run was repeated.
5. 23/22-bit tables (96 MiB) have no CACTI value and are marked "not modelled" (not
   extrapolated).

**Check:** at 11/11 bits the scaled latencies are exactly 4.0 and 8.0, so every default-table
number above is unchanged (L1 39.59, B1 116.06, F0 175.51 cycles per root).

**k = 3, uniform_bits (log = exp bits). B1 = 116.1 and F0 = 175.5 cycles do not depend on the
tables.**

| bits | both tables (bytes) | CACTI access (ns) | convert (cycles) | `lpow` (cycles) | L1 instr/root | **L1 cycles/root** | L1 worst fix-up | L2 cycles/root | L0 cycles/root (approx) | L0 % exact |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 6 | 1,024 | 0.1757 | 3.66 | 7.66 | 153.17 | **309.1** | 573 | 47.9 | 21.0 | 25.6 |
| 7 | 2,048 | 0.1968 | 3.71 | 7.71 | 31.70 | **66.2** | 65 | 41.3 | 21.1 | 33.0 |
| 8 | 4,096 | 0.2181 | 3.75 | 7.75 | 22.66 | **48.2** | 25 | 40.1 | 21.2 | 35.3 |
| 9 | 8,192 | 0.2391 | 3.79 | 7.79 | 18.48 | **40.0** | 7 | 40.1 | 21.4 | 39.1 |
| 10 | 16,384 | 0.2824 | 3.89 | 7.89 | 18.19 | **39.2** | 3 | 40.3 | 21.7 | 52.6 |
| 11 | 32,768 | 0.3352 | 4.00 | 8.00 | 18.23 | **39.6** | 3 | 40.5 | 22.0 | 52.7 |
| 12 | 65,536 | 0.4185 | 4.18 | 8.18 | 18.25 | **40.1** | 3 | 40.9 | 22.5 | 52.6 |
| 13 | 131,072 | 0.5344 | 4.42 | 8.42 | 18.24 | **40.9** | 3 | 41.4 | 23.3 | 52.7 |
| 14 | 262,144 | 0.7318 | 4.84 | 8.85 | 18.24 | **42.1** | 3 | 42.2 | 24.5 | 52.7 |
| 15 | 524,288 | 1.0306 | 5.48 | 9.48 | 18.24 | **44.0** | 3 | 43.5 | 26.4 | 52.7 |
| 16 | 1,048,576 | 1.3956 | 6.26 | 10.26 | 18.24 | **46.4** | 3 | 45.0 | 28.8 | 52.7 |
| 23/22 | 100,663,296 | – | not modelled | not modelled | 18.23 | not modelled | 3 | not modelled | not modelled | 52.8 |

Raw: `results/latency_estimates.csv` (rows `kind = table_sweep`) and
`plots/data/9_table_size_time_vs_accuracy.csv`; plot `plots/9_table_size_time_vs_accuracy.png`.

**What it says**

1. **The fastest exact LNS configuration is 10 bits (16 KiB): 39.2 cycles per root**, slightly
   faster than the 11-bit default (39.6) and with the same worst fix-up (3 steps). Below 9 bits
   the extra fix-up steps cost far more than the faster table saves (6 bits: 309 cycles, worse
   than integer). Above 11 bits the table only gets slower: 46.4 cycles at 16 bits (1 MiB) for
   no accuracy gain.
2. **The table-read effect is small next to the fix-up effect.** From 6 to 16 bits the convert
   only goes from 3.66 to 6.26 cycles. The fix-up steps move L1 by a factor of 8.
3. **L2 (no `lpow`) is the more robust LNS option:** it stays between 40 and 48 cycles at every
   size, and it beats L1 at 8 bits or fewer and at 15 bits or more.
4. **From 9 to 16 bits L1 is 2.5× to 3.0× faster than integer and 3.8× to 4.5× faster than FP64.**
   At 7–8 bits the lead shrinks to 1.8–2.4× over integer, and at 6 bits L1 loses (309 vs 116).

**Honest limits of this estimate**

- **The 4-cycle convert is an assumption.** The paper gives no converter latency, so the
  absolute numbers stand on that guess. CACTI only changes how it scales.
- **The 5/28 read share comes from the LNS adder, not from a converter or `lpow`.** A converter
  is little more than a table read plus an interpolation, so its read share is probably larger
  than 5/28. In that case big tables would cost more than shown here and the case for 9–10-bit
  tables would get stronger. For `lpow` the share is applied to the exp-table stage only, which
  is the same assumption one level down.
- **CACTI models a generic 45 nm SRAM with 8-byte entries**, not the KITES table format, and
  0.7 µm ROM in the paper. Only the *ratio* access(k) / access(11) is used, which is the most
  transferable part.
- Serial sum, no pipelining, as everywhere in section I.

**Sources:** B. Parhami, "Computing with logarithmic number system arithmetic: Implementation
methods and performance benefits", Computers and Electrical Engineering 87 (2020) 106800
(`docs/LNSResearchPaper.pdf`). J. N. Coleman, E. I. Chester, "A 32-Bit Logarithmic Arithmetic
Unit and Its Performance Compared to Floating-Point", Proc. 14th IEEE Symposium on Computer
Arithmetic (ARITH-14), 1999 (`docs/Coleman1999_32bit_LNS_unit_ARITH14.pdf` on branch Devadath).
CACTI 7, <https://github.com/HewlettPackard/cacti>, commit `1ffd8df`.

### I.H Other roots: converter table size (CACTI-scaled, ESTIMATE)

The same CACTI scaling as I.G, applied to the k-th-root variants of section H (LK = `lpow`, LDK = log ÷ k), set `uniform_bits`, converter tables 6..16 bits (log = exp). Instruction counts come from the static model with the C++ model's fix-up candidates at each table size, checked against the simulator rows of `config_sweep.csv` wherever the simulator ran that size (6, 8, 10, 11 and 13 bits). Rows `kind = kroot_table_sweep` in `results/latency_estimates.csv`. Estimated cycles per root (scenario A); the integer baseline BK does not use the tables:

| k | BK (integer) | LK at 6 bits | LK at 8 | LK at 10 | LK at 11 (default) | LK at 16 | best LNS (variant, bits) |
|---|---:|---:|---:|---:|---:|---:|---|
| 2 | 136.5 | 4363 | 287.5 | 62.8 | 64.8 | 69.3 | 61.9 (LDK, 10) |
| 3 | 162.7 | 229.9 | 44.9 | 38.9 | 39.3 | 46.1 | 38.9 (LK, 10) |
| 4 | 166.8 | 43.5 | 43.8 | 44.2 | 44.5 | 51.3 | 42.9 (LDK, 6) |
| 5 | 186.3 | 49.5 | 49.7 | 50.1 | 50.5 | 57.3 | 49.5 (LK, 6) |
| 6 | 209.8 | 55.5 | 55.7 | 56.1 | 56.5 | 63.3 | 55.5 (LK, 6) |

- **k ≥ 4: the smallest tables are the fastest.** Instruction counts are the same at every table size (at most 1 fix-up step even at 6 bits), so the only effect of a bigger table is a slower `lpow` / convert. 6-bit tables (1 KiB) are both exact and cheapest.
- **k = 3 is the in-between case:** below about 9 bits the fix-up steps explode (6 bits: 229.9 cycles, worse than integer); from 9–10 bits the cost is flat and then rises slowly with table size. Best: 10 bits.
- **k = 2 needs at least 9–10 bits** and even then keeps a worst case of about 240 fix-up steps on 64-bit inputs (the 22-bit log fraction cannot resolve a 32-bit root, section H). On `uniform_bits` it still beats integer Newton on average from 9 bits up; on `uniform_u64` it does not (section H).
- Tables above 16 bits are not modelled (no CACTI value).

## What must not be claimed

- **Simulator wall-clock time is not an LNS-vs-integer comparison.** KITES runs integer/FP ops
  natively and emulates every LNS instruction in C++. No wall time is tabulated here.
- **Fewer instructions is not faster hardware.** Instruction count is the measured metric.
  Section F is a labelled model with assumed latencies.
- **L0 is approximate.** Never quote L0's 9.00 instructions per root as "the LNS win" without
  its error next to it. The exact comparison is **B1 (62.31) vs L1 (18.23)**.
- B0 (approximate) does **not** beat L1 (exact) on instruction count on any set or batch size.
  If the tables are shrunk to 6 bits, L1 (153.17) loses to both B0 and B1.
- **Section I.D is an estimate, not a time.** Two of its key inputs (convert and `lpow`
  latency) are guesses; the deck should keep presenting instruction counts.
- **"LNS is better for integer roots" is false as a general claim.** It holds for k ≥ 3. For
  square roots of inputs wider than about 44 bits it is 9× worse than integer Newton and 40×
  worse than `fsqrt.d` (section H).

## Reproduce

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release . && cmake --build build --target kites_cli
g++ -std=c++20 -O2 -I src/processor/lns tools/lns_cbrt_accuracy.cpp src/processor/lns/lns_alu.cpp \
    src/processor/lns/lns_conversion.cpp src/processor/lns/lns_pow.cpp -o lns_cbrt_accuracy
python3 examples/lns_int_cbrt/scripts/gen_cbrt.py
KITES_CLI=build/kites-cli CBRT_MODEL=./lns_cbrt_accuracy python3 examples/lns_int_cbrt/scripts/check.py
KITES_CLI=build/kites-cli CBRT_MODEL=./lns_cbrt_accuracy examples/lns_int_cbrt/scripts/sweep.sh
./lns_cbrt_accuracy --sweep > examples/lns_int_cbrt/results/model_sweep.csv
python3 tools/cbrt_cost_model.py
# Section H (configuration sweep): about 25 min simulator + 2 min model on 4 cores
KITES_CLI=build/kites-cli CBRT_MODEL=./lns_cbrt_accuracy python3 examples/lns_int_cbrt/scripts/sweep_configs.py sim
CBRT_MODEL=./lns_cbrt_accuracy python3 examples/lns_int_cbrt/scripts/sweep_configs.py model
python3 examples/lns_int_cbrt/scripts/sweep_configs.py summary
# Section I
python3 examples/lns_int_cbrt/scripts/sweep_configs.py extra
./lns_cbrt_accuracy --yerr 3 6           # effective exponent and predicted worst fix-up
CBRT_MODEL=./lns_cbrt_accuracy python3 examples/lns_int_cbrt/scripts/latency_model.py
# Plots and their CSVs (plots/, plots/data/); needs matplotlib
CBRT_MODEL=./lns_cbrt_accuracy python3 examples/lns_int_cbrt/scripts/make_plots.py
```

Folder layout: scripts in `scripts/`, programs in `programs/`, CSV/JSON results in `results/`,
plots in `plots/` (see NOTES.md).
