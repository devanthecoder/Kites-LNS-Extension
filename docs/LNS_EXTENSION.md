# LNS extension

Kites can run a small logarithmic-number-system (LNS) extension on the **single-cycle RVSS**
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

All use the custom-0 major opcode `0001011` (`0x0B`).

| Instruction | Format | funct3 | Operation |
| --- | --- | --- | --- |
| `ladd  lrd, lrs1, lrs2` | R | 000 | `lrd = lrs1 + lrs2` |
| `lsub  lrd, lrs1, lrs2` | R | 001 | `lrd = lrs1 - lrs2` |
| `lmul  lrd, lrs1, lrs2` | R | 010 | `lrd = lrs1 * lrs2` |
| `ldiv  lrd, lrs1, lrs2` | R | 011 | `lrd = lrs1 / lrs2` |
| `fcvt.lns.s lrd, frs1` | R, rs2 = 0 | 100 | FP32 in the low 32 bits of `frs1` -> LNS |
| `fcvt.s.lns frd, lrs1` | R, rs2 = 0 | 101 | LNS -> FP32, zero-extended into `frd` |
| `llw   lrd, imm(rs1)` | I | 110 | `lrd = mem32[rs1 + imm]` (raw word) |
| `lsw   lrs2, imm(rs1)` | S | 111 | `mem32[rs1 + imm] = lrs2` (raw word) |

`funct7` is 0 for the R-type instructions (the top bits of `llw`/`lsw` hold the immediate). Offsets are signed 12-bit byte offsets. `llw`/`lsw` move the
word unchanged: no conversion or canonicalization.

Reserved encodings (a custom-0 word with `funct7 != 0` for funct3 000-101, or `rs2 != 0` for the
two converts) execute as no-ops and disassemble as `unknown(...)`.

Not included: conversions to and from integers, FP64 conversions, rounding modes and FP flags for
the conversions.

## Behaviour

- Arithmetic is done by `Kites::lns::LnsAlu` (`src/processor/lns/lns_alu.*`): multiply and divide
  add and subtract the logs; add and subtract use a correction table (errors of a few LSBs of the
  log; the table size is a constructor parameter).
- Conversions are done by `Kites::lns::LnsConverter` (`src/processor/lns/lns_conversion.*`), which
  uses look-up tables of `2^k` entries (`k = 11` by default, overridable with the
  `LNS_LOG_LUT_BITS` / `LNS_EXP_LUT_BITS` macros or the constructor).
- Undo/redo restore L registers and the memory written by `lsw`.
- The UI register table has an "LNS" section, and the register dump JSON has an
  `lns_registers` block.

## Example

```asm
li t0, 0x40400000        # 3.0f
fmv.w.x f1, t0
fcvt.lns.s l1, f1        # l1 = 3.0 (LNS)
ladd l2, l1, l1          # l2 = 6.0
fcvt.s.lns f2, l2        # f2 = 6.0f
```
