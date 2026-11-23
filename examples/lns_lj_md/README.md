# Lennard-Jones molecular dynamics in LNS

A full molecular-dynamics simulation, with forces and time integration, run in FP32 and in LNS on
the KITES simulator (RVSS). Each variant is compared against an FP64 reference computed in C++.

## The simulation

- **System:** 27 atoms in a free cluster. They start on a 3×3×3 grid with spacing 1.12 and small
  offsets (the same positions as `examples/lns_lj`), with small random velocities (temperature 0.1,
  zero total momentum).
- **Units:** reduced Lennard-Jones units, σ = ε = mass = 1.
- **Method:** velocity Verlet. Each step is a half kick v += (dt/2)·F, a move x += dt·v, a force
  update, and a second half kick. The default is dt = 0.005 for 200 steps.
- **Logging:** every 5 steps the program stores the kinetic, potential and total energy and all
  atom positions in memory.

### Variants

| Program | Arithmetic |
| --- | --- |
| `md_fp32.s` | FP32 (`flw`/`fsw`, `fadd.s`, `fsub.s`, `fmul.s`, `fdiv.s`) |
| `md_lns.s` | LNS with table-based add/sub (`ladd`/`lsub`) |
| `md_lns_cordic.s` | LNS with CORDIC add/sub (`laddc`/`lsubc`) |

FP32 computes r⁻² and r⁻⁶ with a division and two multiplications; LNS uses `lpown`. Every
variant starts from the same float32 inputs. The FP64 reference has no assembly program: it is
computed in C++ (`tools/lns_lj_md_model.cpp`), from the same inputs, with the exact time step.

## Files

| File | Purpose |
| --- | --- |
| `gen_md.py` | Writes the three programs for any time step and step count |
| `md_*.s` | The programs at the default setting (dt = 0.005, 200 steps) |
| `../../tools/lns_lj_md_model.cpp` | C++ model: the FP64 reference, plus FP32 and LNS runs that match the simulator bit for bit |
| `analyze_md.py` | Bit-exact check, error metrics, and instruction counts from the simulator trace |
| `sweep_md.sh` | Runs every configuration in the simulator and collects the results |
| `results/` | `md_runs.csv` (one row per run), `md_samples.csv` (every logged sample), `md_counts.csv` (instruction counts) |

## Measures (all against FP64)

- **Energy error:** |E(t) − E_FP64(t)| / |E_FP64(0)|. Comparing with FP64 at the same time step
  cancels the method's own energy fluctuation, so only the arithmetic error is left.
- **Energy drift:** (E(t) − E(0)) / |E(0)|, each variant's own conservation, given for context.
- **Position deviation:** the root-mean-square distance between each atom's position and its
  FP64 position.

The sweep covers:
- **Table sizes:** FP32; LNS with the table at 256–64K add/sub entries and 6–13 (and 23/22)
  conversion-table bits; LNS with CORDIC at 6–13 conversion-table bits.
- **Time steps:** dt = 0.001, 0.002, 0.005, 0.01 and 0.02, each run both for 200 steps (fixed
  step count) and for 1.0 time unit (fixed simulated time).

## Running it (WSL / Linux)

1. **Build** a Release `kites-cli` and the model (from the repo root):
   ```
   g++ -std=c++20 -O2 -ffp-contract=off -I src/processor/lns tools/lns_lj_md_model.cpp \
       src/processor/lns/lns_alu.cpp src/processor/lns/lns_conversion.cpp \
       src/processor/lns/lns_cordic_addsub.cpp src/processor/lns/lns_pow.cpp -o lns_lj_md_model
   ```
2. **Run one program** and check it against the model:
   ```
   cd examples/lns_lj_md
   kites-cli md_lns.s --vm rvss --mem-dump 100003D8 1722 > /dev/null 2>&1
   ../../lns_lj_md_model md_fp32.s lns > model.txt
   python3 analyze_md.py check vm_state/memory_dump.json model.txt
   ```
   - The log starts at `0x100003D8`.
   - 1722 rows = 41 samples × 84 words / 2 words per row.
   - For a different table size, set `LNS_LOG_LUT_BITS`, `LNS_EXP_LUT_BITS` and
     `LNS_ALU_CORRECTION_SAMPLES` for `kites-cli`, and pass the same three sizes to the model.
   - On WSL, run `kites-cli` from a folder under `/tmp` (RAM-backed), not from the repo on
     `/mnt/c`: it rewrites its `vm_state` files throughout the run, and on a disk folder that
     makes it several times slower.
3. **Run the full sweep:**
   ```
   KITES_CLI=/path/to/kites-cli bash examples/lns_lj_md/sweep_md.sh
   ```
   - It makes 234 runs, 8 at a time, and takes about 3–4 hours.
   - `QUICK=1` does a 3-run smoke test.
   - The sweep resumes if interrupted.

**Run time:** the simulator spends most of its time on its own bookkeeping and on rewriting its
state files, so its run time says nothing about FP32 against LNS. It is recorded, but only
instruction counts are a fair comparison.
