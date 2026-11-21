# LNS extension

Kites can run a logarithmic-number-system (LNS) extension on the **single-cycle RVSS**
processor. The RV5S pipelines do not implement it yet.

## Number format

An LNS word is 32 bits: bit 31 is the sign, bits 30:0 are a signed Q9.22 fixed-point
`log2|x|`, so `value = (-1)^sign * 2^(log / 2^22)`. All constants live in
`src/processor/lns/lns_format.h`.

| Value | Word |
| --- | --- |
| +1.0 | `0x00000000` |
| +0 (canonical) | `0x40000000` (any word whose log field is `0x40000000` is zero) |
| +Inf / -Inf | `0x3FC00000` / `0xBFC00000` |
| NaN (canonical) | `0x3FC00001` (integer field 255 with a non-zero fraction) |

Because the all-zero word is +1.0, every L register except `l0` resets to **+0**.

## Registers

32 L registers, `l0`-`l31`, each 32 bits, separate from the x and f registers. `l0` (`lzero`)
always reads +0 and ignores writes.

| Register | ABI name | Role | Saved by |
| --- | --- | --- | --- |
| l0 | lzero | constant +0 | - |
| l1-l7 | lt0-lt6 | temporaries | caller |
| l8-l9 | ls0-ls1 | saved | callee |
| l10-l17 | la0-la7 | arguments / results | caller |
| l18-l27 | ls2-ls11 | saved | callee |
| l28-l31 | lt7-lt10 | temporaries | caller |

Both the raw names and the ABI names are accepted by the assembler. A label with one of these
names would be read as a register.

## Instructions

14 instructions (20 mnemonics, counting each integer width of the converts separately), on three
custom opcodes, mirroring how the F extension keeps `flw` / `fsw` on their own opcodes:

| Opcode | Use |
| --- | --- |
| custom-0 `0001011` (`0x0B`) | all R-type LNS instructions |
| custom-1 `0101011` (`0x2B`) | `llw` (I-type, funct3 = 010) |
| custom-2 `1011011` (`0x5B`) | `lsw` (S-type, funct3 = 010) |

On custom-0, `funct3` selects the operation and `funct7` selects the page: `funct7 = 0` holds the
main instructions and `funct7 = 1` their alternates in the same `funct3` column.

| funct3 | funct7 = 0 | funct7 = 1 (alternates) |
| --- | --- | --- |
| 000 | `ladd  lrd, lrs1, lrs2` | `laddc lrd, lrs1, lrs2` (CORDIC) |
| 001 | `lsub  lrd, lrs1, lrs2` | `lsubc lrd, lrs1, lrs2` (CORDIC) |
| 010 | `lmul  lrd, lrs1, lrs2` | - |
| 011 | `ldiv  lrd, lrs1, lrs2` | - |
| 100 | `fcvt.lns.s lrd, frs1` (rs2 = 0) | `fcvt.lns.{w,wu,l,lu} lrd, rs1` (rs2 = type) |
| 101 | `fcvt.s.lns frd, lrs1` (rs2 = 0) | `fcvt.{w,wu,l,lu}.lns rd, lrs1` (rs2 = type) |
| 110 | `lpow  lrd, lrs1, lrs2` | - |
| 111 | `lpown lrd, lrs1, rs2` | - |

| Instruction | Operation |
| --- | --- |
| `ladd` / `lsub` | `lrd = lrs1 +/- lrs2`, correction table (`LnsAlu`) |
| `laddc` / `lsubc` | same, hyperbolic CORDIC, no table (`LnsCordicAddSub`) |
| `lmul` / `ldiv` | `lrd = lrs1 * / lrs2`: add / subtract the logs, exact |
| `fcvt.lns.s` | FP32 in the low 32 bits of `frs1` -> LNS |
| `fcvt.s.lns` | LNS -> FP32, zero-extended into `frd` |
| `fcvt.lns.w` / `.wu` / `.l` / `.lu` | int32 (low 32 bits of `rs1`) / uint32 / int64 / uint64 -> LNS |
| `fcvt.w.lns` / `.wu` / `.l` / `.lu` | LNS -> int32 / uint32 / int64 / uint64 in `rd`; 32-bit results are sign-extended |
| `lpow` | `lrd = lrs1 ^ lrs2` (LNS exponent) |
| `lpown` | `lrd = lrs1 ^ x[rs2]` (64-bit integer exponent), exact |
| `llw   lrd, imm(rs1)` | `lrd = mem32[rs1 + imm]` (raw word) |
| `lsw   lrs2, imm(rs1)` | `mem32[rs1 + imm] = lrs2` (raw word) |

For the integer converts `rs2` selects the type as in RISC-V `fcvt`: 0 W, 1 WU, 2 L, 3 LU.
Offsets of `llw` / `lsw` are signed 12-bit byte offsets; they move the word unchanged (no
conversion or canonicalization).

Every other word on the three opcodes is **reserved** and executes as a no-op (disassembled as
`unknown(...)`): `funct7` values other than 0 and 1, the `-` cells above, `rs2 != 0` on the FP32
converts, `rs2 > 3` on the integer converts, and `funct3 != 010` on custom-1 / custom-2.

Not included: FP64 conversions, rounding modes and FP flags for the conversions.

## Behaviour

- **Add / subtract.** `LnsAlu` (`src/processor/lns/lns_alu.*`) computes
  `Lmax + log2(1 +/- 2^-d)` with the correction read from a log-spaced table with linear
  interpolation (errors of a few LSBs of the log; 4096 requested samples by default).
  `laddc` / `lsubc` use `LnsCordicAddSub` (`lns_cordic_addsub.*`), which needs no table.
- **Multiply / divide** add or subtract the logs and are exact.
- **Conversions.** `LnsConverter` (`lns_conversion.*`) uses look-up tables of `2^k` entries
  (`k = 11` by default). Results round to nearest-even. The integer converts follow RISC-V
  saturation: NaN -> the largest value, out-of-range values saturate, a negative value converted
  to an unsigned type gives 0. Writes to `x0` are ignored.
- **Exponentiation** (`lns_pow.*`). `log2|x^y| = y * log2|x|`, so a power only scales the log.
  - `lpown`: the log field times the integer n; exact. A negative base keeps its sign for odd n.
  - `lpow`: y is turned back into a fixed-point number with the converter's `2^f` table, multiplied
    by the log of x and rounded to nearest. Exact when y is a power of two, so `x^0.5` (square
    root) is an exact halving of the log. A negative base gives NaN unless y is exactly 1; use
    `lpown` for integer powers.
  - Special values follow C `pow` / `pown`: `x^0 = 1` and `1^y = 1` even for NaN; otherwise NaN
    propagates; `0^y` is +0 (y > 0) or +Inf (y < 0); `Inf^y` is Inf (y > 0) or +0 (y < 0).
- **Range.** Arithmetic results above the finite range become Inf, below it +0.
- **Undo/redo** restore L registers, x registers written by the integer converts, and the memory
  written by `lsw`.
- **UI.** The register table has an "LNS" section, and the register dump JSON has an
  `lns_registers` block.

## Table sizes

The table sizes are compile-time defaults (macros) and constructor arguments of `LnsConverter` /
`LnsAlu`. The simulator's default tables can also be resized at run time through environment
variables, read once on the first LNS instruction:

| Variable | Default | Valid range | Table |
| --- | --- | --- | --- |
| `LNS_LOG_LUT_BITS` | 11 | 1-23 | converter, `log2(1+x)` |
| `LNS_EXP_LUT_BITS` | 11 | 1-22 | converter, `2^f` (also used by `lpow`) |
| `LNS_ALU_CORRECTION_SAMPLES` | 4096 | 2-2^24 | `ladd` / `lsub` correction |

A malformed or out-of-range value makes the run fail with an error message; unset variables leave
the defaults unchanged.

## Tests and tools

- `test/lns_instruction_test.cpp`: 25 GoogleTest cases (RVSS) checking every instruction against
  the library, special values, reserved encodings, undo/redo, machine code and disassembly.
- `tools/` (plain C++, no Qt; build lines in each file's header):
  - Checks: `lns_conversion_check.cpp`, `lns_alu_check.cpp` (accuracy and table-size sweeps),
    `lns_pow_check.cpp` (`lpow` accuracy vs `2^f` table size, `lpown` exactness),
    `lns_lj_accuracy.cpp` (Lennard-Jones accuracy model).
  - Microbenchmarks, which call the library directly on random inputs spread over each table:
    `lns_table_sweep.cpp` (`ladd` / `lsub` error and time vs correction samples, conversions
    and `lpow` vs LUT bits), `lns_conversion_sweep.cpp` (relative error and time of the FP32
    conversions vs LUT bits, with plain FP32 rounding as reference), `lns_cordic_bench.cpp`
    (`laddc` / `lsubc` on the same inputs as the table sweep) and `lns_op_bench.cpp` (time of
    the operations that do not depend on table size). Their times are host time of the library:
    they compare LNS configurations with each other, not LNS with FP32 hardware.

## Lennard-Jones benchmark

`examples/lns_lj/` holds an energy-only Lennard-Jones benchmark (27 atoms, 351 pairs):

- `gen_lj.py` writes `lj_fp32.s` (FP32 control), `lj_lns.s` (LNS, table add/sub),
  `lj_lns_cordic.s` (LNS, CORDIC add/sub) and `reference.txt` (FP64 energy). The LNS kernels use
  `lpown` for `r2^-3` and `fcvt.lns.w` for the constant 4.
- `tools/lns_lj_accuracy.cpp` runs the same kernel on the library and must match the final
  `l10` word from `kites-cli` bit for bit.
- `sweep.sh` (bash, WSL/Linux) times `kites-cli --vm rvss` over the table sizes and writes
  `results.csv`. It measures simulator (host) time, not hardware latency.

## Example

```asm
li t0, 0x40400000        # 3.0f
fmv.w.x f1, t0
fcvt.lns.s l1, f1        # l1 ~ 3.0 (log rounded to 2^-22)
ladd l2, l1, l1          # l2 ~ 6.0
li t1, 2
lpown l3, l1, t1         # l3 = l1^2 exactly (log doubled), ~ 9.0
fcvt.s.lns f2, l2        # f2 = 6.0000005f (6.0 + 1 ulp, from the conversion error)
fcvt.w.lns t2, l3        # t2 = 9
```
