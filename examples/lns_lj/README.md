# Lennard-Jones energy in LNS

Computes the total Lennard-Jones potential energy of 27 atoms in FP32 and in LNS on the KITES
simulator (RVSS), and compares it against an FP64 reference. For the full simulation with forces
and time integration, see `../lns_lj_md`.

## The kernel

- **System:** 27 atoms on a 3×3×3 grid with spacing 1.12, each with a small fixed offset (up to
  0.05), rounded to float32. Reduced units, σ = ε = 1.
- **Energy:** the sum over all 351 pairs i < j of 4(r⁻¹² − r⁻⁶):
  ```
  dx = xi - xj, dy, dz;  r2 = dx*dx + dy*dy + dz*dz
  s6 = r2^-3;  s12 = s6*s6;  E += 4*(s12 - s6)
  ```
  FP32 computes r2⁻³ with a division and two multiplications, LNS with one `lpown`.
- The program goes over all pairs 4 times, resetting E each time, so the final register holds the
  energy of one pass. The final energy is in `f10` (FP32) or `l10`/`la0` (LNS, also converted
  into `f11`).

| Program | Arithmetic |
| --- | --- |
| `lj_fp32.s` | FP32 (`flw`, `fsub.s`, `fmul.s`, `fadd.s`, `fdiv.s`) |
| `lj_lns.s` | LNS with table-based add/sub (`ladd`/`lsub`). The positions are converted once with `fcvt.lns.s`. |
| `lj_lns_cordic.s` | The same, with CORDIC add/sub (`laddc`/`lsubc`) |

## Files

| File | Purpose |
| --- | --- |
| `gen_lj.py` | Writes the three programs and `reference.txt` (`--n` atoms, `--repeats` passes, `--out` directory) |
| `lj_*.s` | The generated programs (27 atoms, 4 passes) |
| `reference.txt` | FP64 energy from the same float32 positions: −72.99838290052652 |
| `../../tools/lns_lj_accuracy.cpp` | C++ model of the same kernel on the LNS library. It sweeps the table sizes, and its final words match `l10` from the simulator bit for bit. |
| `sweep.sh` | Runs the programs in the simulator for every table size and writes `results.csv` |
| `results.csv` | Energy, relative error and final word per configuration (and simulator time) |

## Results (`results.csv`)

| Configuration | Relative error vs FP64 |
| --- | --- |
| FP32 | 5.3e-7 |
| LNS, add/sub table 256 → 64K entries | 7.5e-3 → 8.5e-7 |
| LNS, default (4096 entries, 11-bit conversion) | 2.7e-5 |
| LNS, conversion table 6–23 bits (4096 add/sub) | 2.3e-5 – 3.0e-5 |
| LNS CORDIC | 1.4e-7 |

The add/sub table size sets the accuracy, while the conversion table size barely matters. CORDIC
is more accurate than FP32 here.

## Running it (WSL / Linux)

1. **Build** a Release `kites-cli`, and the model (from the repo root):
   ```
   g++ -std=c++20 -O2 -I src/processor/lns tools/lns_lj_accuracy.cpp \
       src/processor/lns/lns_alu.cpp src/processor/lns/lns_conversion.cpp \
       src/processor/lns/lns_cordic_addsub.cpp src/processor/lns/lns_pow.cpp -o lns_lj_accuracy
   ```
2. **Regenerate the programs** (optional; the committed ones are the default):
   `python3 examples/lns_lj/gen_lj.py`
3. **Run one program** and read the energy:
   ```
   cd examples/lns_lj
   kites-cli lj_lns.s --vm rvss > /dev/null
   ```
   The energy is `l10` in the `lns_registers` block of `vm_state/registers_dump.json` (`f10` in
   `fp_registers` for `lj_fp32.s`). For another table size, set `LNS_LOG_LUT_BITS`,
   `LNS_EXP_LUT_BITS` or `LNS_ALU_CORRECTION_SAMPLES` for `kites-cli`.
4. **Check against the model:** `./lns_lj_accuracy` (from the repo root) prints the final word
   for every table size. Its words for the default sizes and CORDIC must equal `l10`.
5. **Run the sweep:**
   ```
   KITES_CLI=/path/to/kites-cli LJ_MODEL=./lns_lj_accuracy bash examples/lns_lj/sweep.sh
   ```

**Run time:** the times in `results.csv` are the simulator's own (host) run time, which is mostly
its bookkeeping, so they don't compare FP32 with LNS. Run time is measured in `../lns_lj_md` (an
estimated cycle count and a measured software time) and by the microbenchmarks in `tools/`.
