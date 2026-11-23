# TCP CUBIC cube-root benchmark: notes

## Where the numbers come from

- **Code base:** branch `Sheshadri-Rathan-` at commit `84fb2092c3c43d8e34d4a61e781084e08b8eaf1e`
  ("TCP Benchmark", 2026-11-23). That commit is `main` (`c3dc0a8`) plus Codex's partial
  `examples/lns_int_cbrt/` work. The LNS code (`src/processor/lns/`, `rvss_processor.cpp`) is the
  one on `main`. If the branch gains commits that touch the LNS converter, `lpow` or the
  assembler, regenerate the programs and re-run `check.py` and `sweep.sh`: every number here is
  tied to this commit.
- **Kernel:** Linux **v6.12**, `net/ipv4/tcp_cubic.c`, `static u32 cubic_root(u64 a)`,
  <https://github.com/torvalds/linux/blob/v6.12/net/ipv4/tcp_cubic.c>. I fetched it
  (I didn't port it from memory). The function body is byte-identical on `master` (checked
  2026-11-23). The table `v[]`, the `b < 7` direct path, `b = ((b * 84) >> 8) - 1`,
  `shift = a >> (b * 3)`, `x = ((u32)(((u32)v[shift] + 10) << b)) >> 6` and the single Newton
  step `x = 2 * x + (u32)div64_u64(a, (u64)x * (u64)(x - 1)); x = ((x * 341) >> 10)` all match
  the fetched source. Its comment claims "Avg err ~= 0.195%".
- **Build:** host Ubuntu 24.04, g++ 13.3, Qt 6.4.2 (the distro package). Two build-only edits
  were needed in a *scratch copy*, never in the delivered tree:
  1. `CMakeLists.txt` asks for `Qt6 6.9.2`. The sandbox could not download 6.9.2 (download.qt.io
     is blocked), so the scratch copy uses `find_package(Qt6 REQUIRED ...)`.
  2. `src/ui/editor_tab/editortab.cpp:183` (`qWarning() << std::string`) does not compile
     against Qt 6.4. The scratch copy wraps it in `QString::fromStdString`. This is GUI code and
     doesn't affect the simulator.
  3. Tests only: `test/test_main.cpp` includes `<windows.h>` unconditionally, so the GoogleTest
     suite cannot build on Linux/WSL as committed. The scratch copy guarded it with
     `#ifdef _WIN32`, and the new test was built and run that way (21/21 pass). On the team's
     Windows/MinGW setup it should build unchanged with `cmake --build --preset test-only`.
     `test/CMakeLists.txt` auto-globs `*.cpp`, so no CMake edit is needed.
- **Simulator speed:** kites-cli opens and closes two files per retired instruction (`strace`:
  ~136k `openat` + `close` for 68k instructions). On a normal disk that is about 2k
  instructions/s. `check.py` therefore runs each program in a temp dir under `/dev/shm`
  (about 9k instr/s) and runs 4 programs in parallel. The whole `check.py` takes about 75 s
  and `sweep.sh` a few minutes. This only affects host time, not any reported metric.

## Folder layout

- Everything lives in `examples/lns_int_cbrt/` (the integer-application folder, next to
  `examples/lns_lj/`), as the team asked. Codex's partial commit `84fb209` put its files in
  `examples/lns_cbrt/`. So `git status` on the branch shows those 7 files as **deleted** there
  and `examples/lns_int_cbrt/` as new. That is the move, not lost work: `gen_cbrt.py` and
  `reference.json` replace Codex's versions, and the `cbrt_*_small.s` programs are regenerated.
  The tools keep their names (`tools/lns_cbrt_accuracy.cpp`, `tools/cbrt_cost_model.py`,
  `tools/cbrt_cost_latencies.json`, `test/lns_cbrt_kernel_test.cpp`).

## Deviations from the brief, and why

1. **B1 is "B0 + one integer Newton step + ±1 fix-up", not "B0 + fix-up".** The kernel's
   single Newton step leaves errors up to **3,934** units at 64-bit inputs (measured, 10^6
   inputs per set). A ±1 fix-up loop straight after B0 would take a mean of 55 steps per root on
   `uniform_bits`, and 466 steps on `u64max` (model rows `B0+fixup_only`). No engineer would ship
   that. It would turn B1 into a strawman and inflate the LNS win. One more integer Newton step
   (7 instructions, 1 `divu`) cuts the residual to at most 6 steps. This choice *helps the
   integer side*.
2. **Fix-up is mul-based, not `divu`-based.** Codex's partial version tested `a / x < x*x` with
   `divu`. The delivered fix-up uses `x*x*x <= a` (2 `mul`) with a clamp to 2,642,245 (the largest
   cube root of a u64), so `(x+1)^3` can never overflow. B1, L1, L2 and F0 share the identical
   fix-up blocks.
3. **`fls64` is a branchy 6-step binary search.** It mirrors the generic kernel `__fls`, used
   when there is no `clz`. KITES has no Zbb. This costs B0/B1 **21.5 instructions per root**,
   reported as its own line. RESULTS.md also gives a *computed, not measured* "with Zbb clz"
   figure so nobody can say the baseline was handicapped.
4. **F0 uses `fadd.d`, not `fmadd.d`.** On this branch, **`fmadd.d` returns wrong results on
   rvss.** Repro: `f2 = 3.0, f10 = 2.0, f3 = 100.0; fmadd.d f4, f2, f10, f3` gives
   `f4 = 0.0` instead of 106.0. With rd == rs3 (`fmadd.d f3, f2, f10, f3`), f3 stays 100.0.
   `fmul.d`, `fadd.d` and `fdiv.d` behave. I didn't touch the simulator (brief rule). F0 forms
   `2y` with `fadd.d`, which costs one extra instruction per Newton step. **This is a real KITES
   bug the team should log** (it predates the LNS work). The `alu.cpp` FMADD_D case itself
   calls `std::fma`, so the fault is in decode or operand routing. I didn't investigate further.
5. **Exact variants count fix-up steps** (`mv s6, s5` before, `sub s6, s5, s6` after, plus
   `sd`/`addi` into `steps:`). That is 4 instrumentation instructions per root. `results.csv`
   reports both `instr_per_root` and `instr_per_root_uninstrumented`. The batch programs (used
   for break-even and `mix.json`) carry no instrumentation, so they are the production cost.
6. **1/3 for `lpow` is built as the brief says:** `fcvt.lns.w` of 1 and 3, then `ldiv`. That
   goes through the log table, so the exponent is `-6647815` in Q22, against an ideal
   `-6647814.56`. `ldiv` constructs the add/sub ALU object once in setup (it never reads its
   correction table). `check.py` confirms that outputs and counts are unchanged with
   `LNS_ALU_CORRECTION_SAMPLES=2`. Zero samples is rejected by `LnsAlu`, so 2 is the smallest
   legal value.
7. **L2 needs a zero guard.** `a = 0` converts to the reserved LNS zero (field `0x40000000`).
   Dividing that word by 3 would make a huge finite value, which saturates to `UINT64_MAX`, and
   the fix-up would then crawl down 2.6 million steps. L2 skips the divide for `a = 0`
   (1 `beqz` per root). The divide is `(w + 1) * 0xAAAAAAAB >> 33`, i.e. round(w / 3) with a
   reciprocal multiply, exact for every w < 2^31.
8. **Codex's earlier `cbrt_*_small.s` files (8 inputs each) were overwritten** by the generator's
   1,001-input `small` programs. `gen_cbrt.py` replaces Codex's version (same CLI shape and file
   naming). Its kernel port did not wrap `x * 341` to 32 bits, and it lacked the ablation, batch
   and static model.

## Things I could not do or did not do

- **No hardware timing.** There is no LNS hardware. `tools/cbrt_cost_model.py` is a labelled
  model. Only `mul` = 3 and `div` = 6..68 cycles come from a datasheet (SiFive U74 manual,
  Table 6). Everything LNS is a guess.
- **No wall-clock comparison.** By design (KITES emulates LNS ops in C++).
- **Lennard-Jones count:** `docs/LNS_PPT_CONTEXT.md` quotes 28,252 instructions for `lj_lns.s`.
  This build retires **28,251** (re-run 2026-11-23). It's an off-by-one in the doc or in its
  run. The shares in RESULTS.md use 28,251.
- **Assembler `li` expansion** sizes (1 / 2 words, and 4 / 8 for wide constants) were measured
  with kites-cli and hard-coded in `gen_cbrt.li_words`. Wide constants live in a `.dword` pool
  instead. If the assembler's `li` expansion changes, the static model will disagree and
  `check.py` will fail loudly. That is intended.

## Open questions for the team

1. Do you want the headline per-root numbers with or without the 6-instruction loop overhead?
   RESULTS.md shows both. With loop: L1 18.2 vs B1 62.3. Without: 12.2 vs 56.3.
2. Is an `lpow` latency of a few cycles defensible for the ISA story? The cost model says L1
   stays ahead of B1 until one `lpow` costs about 62 cycles (div = 6) to 180 (div = 68). A
   reviewer will ask what `lpow` costs in gates (an exp2 table lookup plus a 31x32 multiply).
3. Should `fmadd.d` be fixed and F0 regenerated with it? It would save 3 instructions per root
   in F0. It does not change any conclusion.
