#!/usr/bin/env python3
"""Generator for the TCP CUBIC cube-root benchmark: integer kernel vs LNS vs FP64 on KITES rvss.

Writes the RV64 assembly programs (all variants x all input sets), the ablation programs used for
the dynamic overhead breakdown, the batch-size programs used for the break-even table, and
reference.json (exact floor cube root, nearest cube root and the kernel's cubic_root() for every
input). Pure Python, no numpy; every input set is deterministic (fixed LCG seeds).

Kernel source (fetched, not ported from memory):
  Linux v6.12, net/ipv4/tcp_cubic.c, static u32 cubic_root(u64 a)
  https://github.com/torvalds/linux/blob/v6.12/net/ipv4/tcp_cubic.c
  The function body is byte-identical on master as of 2026-11-23. Its header comment claims
  "Avg err ~= 0.195%"; it is an approximation by design (table lookup + ONE Newton step).

Variants (ID, program name, what it computes):
  B0 cbrt_cubic        faithful RV64IM port of cubic_root() (software fls64, divu, mul)  approx
  B1 cbrt_cubic_exact  B0 + one integer Newton step + mul-based +/-1 fix-up              exact
  L0 cbrt_lns          fcvt.lns.lu -> lpow(x, 1/3) -> fcvt.lu.lns                         approx
  L1 cbrt_lns_exact    L0 + the same mul-based fix-up as B1                               exact
  L2 cbrt_lns_logdiv   fcvt.lns.lu, log field / 3 with an integer reciprocal multiply,
                       fcvt.lu.lns, fix-up                                                exact
  F0 cbrt_fp64         fcvt.d.lu, bit-trick guess, 3 FP64 Newton steps (fmul/fdiv/fadd),
                       fcvt.lu.d, fix-up                                                  exact

Every program: N u64 inputs in .data (input:), one u64 result per input (output:), and for the
exact variants the signed fix-up step count per input (steps:, final - candidate). Exit with
li a7, 10; ecall. Data starts at 0x10000000 on KITES, so input = 0x10000000,
output = input + 8N, steps = output + 8N.

The same block lists that emit the assembly also drive static_profile(), the static instruction
model (instructions per block x how often each block runs for a given input), which check.py
compares against instructions_retired from kites-cli.

Usage:
  python3 examples/lns_int_cbrt/gen_cbrt.py [--out DIR]
"""

from __future__ import annotations

import argparse
import json
import os
from collections import Counter

KERNEL_TAG = "v6.12"
KERNEL_URL = "https://github.com/torvalds/linux/blob/v6.12/net/ipv4/tcp_cubic.c"

# v[] from cubic_root(), Linux v6.12 (verified against the fetched source).
V = [0, 54, 54, 54, 118, 118, 118, 118,
     123, 129, 134, 138, 143, 147, 151, 156,
     157, 161, 164, 168, 170, 173, 176, 179,
     181, 185, 187, 190, 192, 194, 197, 199,
     200, 202, 204, 206, 209, 211, 213, 215,
     217, 219, 221, 222, 224, 225, 227, 229,
     231, 232, 234, 236, 237, 239, 240, 242,
     244, 245, 246, 248, 250, 251, 252, 254]

CBRT_CAP = 2642245          # floor(cbrt(2^64 - 1)); CBRT_CAP^3 < 2^64 <= (CBRT_CAP + 1)^3
U32 = 0xFFFFFFFF
U64 = (1 << 64) - 1
DATA_BASE = 0x10000000

RECIP3_32 = 0xAAAAAAAB              # v / 3 == (v * RECIP3_32) >> 33 for every v < 2^32
RECIP3_64 = 0xAAAAAAAAAAAAAAAB      # v / 3 == mulhu(v, RECIP3_64) >> 1 for every u64 v
FP64_CBRT_MAGIC = 0x2AA0000000000000  # (2/3) * bits(1.0): bits(cbrt(x)) ~ bits(x)/3 + magic
FP64_THIRD = 0x3FD5555555555555
FP64_NEWTON_STEPS = 3

SETS = ("small", "near2^22", "cubes", "u32max", "u64max", "uniform_bits")
VARIANTS = {  # id -> (program stem, phases in order, exact?)
    "B0": ("cbrt_cubic", ["fls64", "lookup", "newton"], False),
    "B1": ("cbrt_cubic_exact", ["fls64", "lookup", "newton", "refine", "fixup"], True),
    "L0": ("cbrt_lns", ["convert_in", "lpow", "convert_out"], False),
    "L1": ("cbrt_lns_exact", ["convert_in", "lpow", "convert_out", "fixup"], True),
    "L2": ("cbrt_lns_logdiv", ["convert_in", "logdiv", "convert_out", "fixup"], True),
    "F0": ("cbrt_fp64", ["convert_in", "fp_newton", "convert_out", "fixup"], True),
}
BATCH_SIZES = (1, 4, 16, 64, 256, 1024)
ABLATION_SET = "uniform_bits"


# ---------------------------------------------------------------------------------------------
# Reference arithmetic (Python integers only, never floating point)
# ---------------------------------------------------------------------------------------------
def exact_cbrt(a: int) -> int:
    """floor(a^(1/3)) by integer Newton from above, then an integer fix-up."""
    if a < 2:
        return a
    x = 1 << ((a.bit_length() + 2) // 3)  # >= cbrt(a)
    while True:
        y = (2 * x + a // (x * x)) // 3
        if y >= x:
            break
        x = y
    while (x + 1) ** 3 <= a:
        x += 1
    while x ** 3 > a:
        x -= 1
    return x


def nearest_cbrt(a: int) -> int:
    """round(a^(1/3)) (ties cannot occur for integers): r or r+1 with (2r+1)^3 <= 8a."""
    r = exact_cbrt(a)
    return r + 1 if (2 * r + 1) ** 3 <= 8 * a else r


def fls64(a: int) -> int:
    return a.bit_length()


def kernel_cubic_root(a: int) -> int:
    """Line-by-line port of Linux v6.12 cubic_root(u64 a), including its u32 wrap-around."""
    b = fls64(a)
    if b < 7:
        return ((V[a] + 35) >> 6) & U32
    b = (((b * 84) & U32) >> 8) - 1
    shift = (a >> (b * 3)) & U32
    x = (((V[shift] + 10) << b) & U32) >> 6
    q = (a // ((x * (x - 1)) & U64)) & U32       # (u32)div64_u64(a, (u64)x * (u64)(x - 1))
    x = (2 * x + q) & U32
    return ((x * 341) & U32) >> 10


def refine(a: int, x: int) -> int:
    """One integer Newton step used by B1: x' = (2x + a / x^2) / 3 (x = 0 stays 0)."""
    if x == 0:
        return 0
    v = 2 * x + a // (x * x)
    assert v < (1 << 31), "reciprocal-multiply divide by 3 needs v < 2^31"
    return (v * RECIP3_32) >> 33


def fixup(a: int, x: int) -> int:
    """The B1/L1/L2/F0 fix-up exactly as the assembly does it: clamp, step down, step up."""
    x = min(x & U64, CBRT_CAP)
    while x ** 3 > a:
        x -= 1
    while x < CBRT_CAP and (x + 1) ** 3 <= a:
        x += 1
    return x


# ---------------------------------------------------------------------------------------------
# Input sets
# ---------------------------------------------------------------------------------------------
class Lcg:
    """64-bit LCG (Knuth MMIX constants); the top 32 bits of two steps make one 64-bit draw."""

    def __init__(self, seed: int):
        self.state = seed & U64

    def next32(self) -> int:
        self.state = (6364136223846793005 * self.state + 1442695040888963407) & U64
        return self.state >> 32

    def next64(self) -> int:
        return (self.next32() << 32) | self.next32()


def input_sets() -> dict[str, list[int]]:
    near = set()
    for p in (21, 22, 23, 24):
        near.update((1 << p) + d for d in range(-64, 65))
    rng = Lcg(0xC0B1C)
    while len(near) < 516 + 256:
        near.add((1 << 21) + rng.next64() % ((1 << 24) - (1 << 21) + 1))

    ns = {1, 2, 3, 4, 7, 8, 63, 64, 65, 127, 128, 1023, 1024, 1 << 15, 1 << 20, 1 << 21,
          (1 << 21) + 1, 2642244, CBRT_CAP}
    ns.update(1 + (i * (CBRT_CAP - 1)) // 99 for i in range(100))            # linear spread
    ns.update(int(round(CBRT_CAP ** (i / 99))) for i in range(100))         # log spread
    cubes = sorted({n ** 3 + d for n in ns for d in (-1, 0, 1) if 0 <= n ** 3 + d <= U64})

    rng = Lcg(0x5EEDC0DE)
    uniform = []
    while len(uniform) < 1024:
        bits = 1 + rng.next32() % 64                    # bit length uniform in 1..64
        v = (1 << (bits - 1)) | (rng.next64() & ((1 << (bits - 1)) - 1))
        uniform.append(v)                               # keep order (batch prefixes)

    u64max = {U64 - d for d in range(32)} | {(1 << 63) + d for d in (-1, 0, 1)}
    u64max |= {CBRT_CAP ** 3 + d for d in (-1, 0, 1)}
    return {
        "small": list(range(1001)),
        "near2^22": sorted(near),
        "cubes": cubes,
        "u32max": sorted({(1 << 32) + d for d in range(-17, 17)}),
        "u64max": sorted(u64max),
        "uniform_bits": uniform,
    }


def set_file_tag(name: str) -> str:
    return name.replace("^", "")


# ---------------------------------------------------------------------------------------------
# Code blocks. A block is (label or None, [instructions], phase). Control flow between blocks is
# explicit in the instructions; static_profile() walks the same blocks.
# ---------------------------------------------------------------------------------------------
CLASS_OF = {
    "addi": "alu", "add": "alu", "sub": "alu", "slli": "alu", "srli": "alu", "srl": "alu",
    "sll": "alu", "sltiu": "alu", "mv": "alu", "addw": "alu", "srliw": "alu",
    "mul": "mul", "mulw": "mul", "mulhu": "mul",
    "divu": "div",
    "beqz": "branch", "bnez": "branch", "bgeu": "branch", "bltu": "branch", "beq": "branch",
    "j": "branch",
    "ld": "load", "lbu": "load", "lwu": "load",
    "sd": "store", "sw": "store",
    "fcvt.lns.lu": "lns_convert", "fcvt.lu.lns": "lns_convert", "fcvt.lns.w": "lns_convert",
    "lpow": "lns_pow", "ldiv": "lns_alu", "llw": "lns_mem", "lsw": "lns_mem",
    "fcvt.d.lu": "fp_convert", "fcvt.lu.d": "fp_convert", "fmv.x.d": "fp_move",
    "fmv.d.x": "fp_move", "fmul.d": "fp_mul", "fadd.d": "fp_add", "fdiv.d": "fp_div",
    "ecall": "system",
}
CLASSES = ("alu", "mul", "div", "branch", "load", "store", "lns_convert", "lns_pow", "lns_alu",
           "lns_mem", "fp_convert", "fp_move", "fp_add", "fp_mul", "fp_div", "system")


def li_words(value: int) -> int:
    """Instruction words KITES emits for li (measured with kites-cli on this branch)."""
    if -2048 <= value < 2048:
        return 1
    if -(1 << 31) <= value < (1 << 31):
        return 2
    raise ValueError(f"li {value:#x}: wide constant, load it from the constant pool instead")


def words_and_class(instr: str) -> tuple[int, str]:
    op = instr.split()[0]
    if op == "la":
        return 2, "alu"                       # auipc + addi
    if op == "li":
        return li_words(int(instr.split(",")[1], 0)), "alu"
    return 1, CLASS_OF[op]


def blk(label, instrs, phase):
    return (label, instrs, phase)


def fls64_blocks():
    """fls64(a) in t0 by a branchy binary search (the generic kernel __fls); a in s4."""
    bl = [blk(None, ["li t0, 0", "beqz s4, fls_done"], "fls64"),
          blk("fls_init", ["li t0, 1", "mv t1, s4"], "fls64")]
    for k, nxt in ((32, 16), (16, 8), (8, 4), (4, 2), (2, 1)):
        bl.append(blk(f"fls_{k}_test", [f"srli t2, t1, {k}", f"beqz t2, fls_{nxt}_test"], "fls64"))
        bl.append(blk(f"fls_{k}_take", ["mv t1, t2", f"addi t0, t0, {k}"], "fls64"))
    bl.append(blk("fls_1_test", ["srli t2, t1, 1", "beqz t2, fls_done"], "fls64"))
    bl.append(blk("fls_1_take", ["addi t0, t0, 1"], "fls64"))
    bl.append(blk("fls_done", [], "fls64"))
    return bl


def fls64_walk(a):
    if a == 0:
        return [0]
    path = [0, 1]
    t = a
    idx = 2
    for k in (32, 16, 8, 4, 2):
        path.append(idx)
        if t >> k:
            path.append(idx + 1)
            t >>= k
        idx += 2
    path.append(idx)
    if t >> 1:
        path.append(idx + 1)
    path.append(idx + 2)
    return path


def lookup_blocks():
    return [
        blk(None, ["sltiu t1, t0, 7", "beqz t1, k_table"], "lookup"),
        blk("k_direct", ["add t1, s8, s4", "lbu t2, 0(t1)", "addi t2, t2, 35", "srli s5, t2, 6",
                         "j k_done"], "lookup"),
        blk("k_table", ["mul t0, t0, s9", "srli t0, t0, 8", "addi t0, t0, -1", "slli t1, t0, 1",
                        "add t1, t1, t0", "srl t1, s4, t1", "add t1, s8, t1", "lbu t2, 0(t1)",
                        "addi t2, t2, 10", "sll t2, t2, t0", "srli s5, t2, 6"], "lookup"),
    ]


def newton_blocks():
    return [
        blk(None, ["addi t1, s5, -1", "mul t1, s5, t1", "divu t1, s4, t1", "slli t2, s5, 1",
                   "addw t2, t2, t1", "mulw t2, t2, s10", "srliw s5, t2, 10"], "newton"),
    ]


def k_done_block():
    return [blk("k_done", [], "lookup")]


def refine_blocks():
    return [
        blk(None, ["beqz s5, rf_done"], "refine"),
        blk(None, ["mul t0, s5, s5", "divu t0, s4, t0", "slli t1, s5, 1", "add t0, t0, t1",
                   "mul t0, t0, s11", "srli s5, t0, 33"], "refine"),
        blk("rf_done", [], "refine"),
    ]


def fixup_blocks(count):
    pre = [blk(None, ["mv s6, s5"], "count")] if count else []
    post = [blk(None, ["sub s6, s5, s6"], "count")] if count else []
    return pre + [
        blk(None, ["bgeu s7, s5, fx_down"], "fixup"),                         # 0 clamp test
        blk(None, ["mv s5, s7"], "fixup"),                                    # 1 clamp
        blk("fx_down", ["mul t0, s5, s5", "mul t0, t0, s5", "bltu s4, t0, fx_dec"], "fixup"),  # 2
        blk("fx_up", ["beq s5, s7, fx_done"], "fixup"),                       # 3
        blk(None, ["addi t1, s5, 1", "mul t0, t1, t1", "mul t0, t0, t1",
                   "bltu s4, t0, fx_done"], "fixup"),                         # 4
        blk(None, ["mv s5, t1", "j fx_up"], "fixup"),                         # 5
        blk("fx_dec", ["addi s5, s5, -1", "mul t0, s5, s5", "mul t0, t0, s5",
                       "bltu s4, t0, fx_dec"], "fixup"),                      # 6
        blk("fx_done", [], "fixup"),                                          # 7
    ] + post


def fixup_walk(a, x):
    """Block indices (relative to the fix-up blocks without the count blocks)."""
    path = [0]
    x &= U64
    if x > CBRT_CAP:
        path.append(1)
        x = CBRT_CAP
    path.append(2)
    if x ** 3 > a:
        while True:
            path.append(6)
            x -= 1
            if not x ** 3 > a:
                break
    else:
        while True:
            path.append(3)
            if x == CBRT_CAP:
                break
            path.append(4)
            if (x + 1) ** 3 > a:
                break
            path.append(5)
            x += 1
    path.append(7)
    return path


def lns_cin():
    return [blk(None, ["fcvt.lns.lu l1, s4"], "convert_in")]


def lns_lpow():
    return [blk(None, ["lpow l2, l1, l12"], "lpow")]


def lns_cout(src):
    return [blk(None, [f"fcvt.lu.lns s5, {src}"], "convert_out")]


def logdiv_blocks():
    # a = 0 encodes as the LNS zero (field 0x40000000), which must not be divided: skip it, and
    # l1 already holds zero. For a >= 1 the word is the non-negative Q9.22 log itself.
    return [
        blk(None, ["beqz s4, ld_done"], "logdiv"),
        blk(None, ["lsw l1, 0(s8)", "lwu t0, 0(s8)", "addi t0, t0, 1", "mul t0, t0, s11",
                   "srli t0, t0, 33", "sw t0, 0(s8)", "llw l1, 0(s8)"], "logdiv"),
        blk("ld_done", [], "logdiv"),
    ]


def fp_cin():
    return [blk(None, ["fcvt.d.lu f1, s4"], "convert_in")]


def fp_newton_blocks():
    body = ["fmv.x.d t0, f1", "mulhu t0, t0, s11", "srli t0, t0, 1", "add t0, t0, s9",
            "fmv.d.x f2, t0"]
    for _ in range(FP64_NEWTON_STEPS):
        # y = (a / y^2 + 2y) / 3. Not fmadd.d: on this branch fmadd.d returns a wrong result on
        # rvss (see NOTES.md), so 2y is formed with fadd.d.
        body += ["fmul.d f3, f2, f2", "fdiv.d f3, f1, f3", "fadd.d f4, f2, f2",
                 "fadd.d f3, f3, f4", "fmul.d f2, f3, f11"]
    return [blk(None, body, "fp_newton")]


def fp_cout():
    return [blk(None, ["fcvt.lu.d s5, f2"], "convert_out")]


def phase_blocks(variant, phase, count):
    """Blocks of one phase, and a walk function (a, state) -> [block indices]."""
    single = lambda a, st: list(range(len(phase_blocks(variant, phase, count)[0])))  # noqa: E731
    if phase == "fls64":
        return fls64_blocks(), lambda a, st: fls64_walk(a)
    if phase == "lookup":
        return lookup_blocks(), lambda a, st: [0, 1] if a.bit_length() < 7 else [0, 2]
    if phase == "newton":
        # k_direct jumps straight to k_done, past the Newton step.
        return newton_blocks() + k_done_block(), \
            lambda a, st: [1] if a.bit_length() < 7 else [0, 1]
    if phase == "refine":
        return refine_blocks(), lambda a, st: [0, 2] if st["x_kernel"] == 0 else [0, 1, 2]
    if phase == "fixup":
        off = 1 if count else 0

        def walk(a, st):
            p = [off + i for i in fixup_walk(a, st["candidate"])]
            return ([0] + p + [off + 8]) if count else p
        return fixup_blocks(count), walk
    if phase == "convert_in":
        return (fp_cin() if variant == "F0" else lns_cin()), single
    if phase == "lpow":
        return lns_lpow(), single
    if phase == "logdiv":
        return logdiv_blocks(), lambda a, st: [0, 2] if a == 0 else [0, 1, 2]
    if phase == "convert_out":
        if variant == "F0":
            return fp_cout(), single
        return lns_cout("l1" if variant == "L2" else "l2"), single
    if phase == "fp_newton":
        return fp_newton_blocks(), single
    raise ValueError(phase)


# ---------------------------------------------------------------------------------------------
# Programs
# ---------------------------------------------------------------------------------------------
def setup_lines(variant, n, count, exact):
    lines = ["la s0, input", "la s1, output", f"li s3, {n}"]
    if exact and count:
        lines.insert(2, "la s2, steps")
    if exact:
        lines.append(f"li s7, {CBRT_CAP}")
    if variant in ("B0", "B1"):
        lines += ["la s8, vtab", "li s9, 84", "li s10, 341"]
        if variant == "B1":
            lines += ["la t0, consts", "ld s11, 0(t0)"]
    elif variant in ("L0", "L1"):
        # 1/3 in LNS, built once: log2(1) - log2(3); both converts go through the log table.
        lines += ["li t0, 1", "fcvt.lns.w l10, t0", "li t0, 3", "fcvt.lns.w l11, t0",
                  "ldiv l12, l10, l11"]
    elif variant == "L2":
        lines += ["la s8, scratch", "la t0, consts", "ld s11, 0(t0)"]
    elif variant == "F0":
        lines += ["la t0, consts", "ld s11, 0(t0)", "ld s9, 8(t0)", "ld t1, 16(t0)",
                  "fmv.d.x f11, t1"]
    return lines


def consts_for(variant):
    if variant in ("B1", "L2"):
        return [RECIP3_32]
    if variant == "F0":
        return [RECIP3_64, FP64_CBRT_MAGIC, FP64_THIRD]
    return []


LOOP_HEAD = ["ld s4, 0(s0)"]


def loop_tail(store_reg, exact, count):
    t = [f"sd {store_reg}, 0(s1)"]
    if exact and count:
        t += ["sd s6, 0(s2)", "addi s2, s2, 8"]
    return t + ["addi s0, s0, 8", "addi s1, s1, 8", "addi s3, s3, -1", "bnez s3, root_loop"]


EXIT = ["li a7, 10", "ecall"]


class Program:
    """One generated program: variant, the first `k` phases, input values, count flag."""

    def __init__(self, variant, values, set_name, phases=None, count=True, title=""):
        self.variant = variant
        stem, all_phases, exact = VARIANTS[variant]
        self.phases = list(all_phases if phases is None else phases)
        self.exact = exact                      # layout and setup follow the full variant
        self.count = count and exact and "fixup" in self.phases
        self.values = list(values)
        self.set_name = set_name
        self.title = title or stem
        self.parts = [(ph,) + phase_blocks(variant, ph, self.count) for ph in self.phases]
        if "lookup" in self.phases and "newton" not in self.phases:
            self.parts.append(("lookup", k_done_block(), lambda a, st: [0]))
        writes_s5 = any(ph in ("lookup", "newton", "convert_out") for ph in self.phases)
        self.store_reg = "s5" if writes_s5 else "s4"

    @property
    def n(self):
        return len(self.values)

    @property
    def addresses(self):
        n = self.n
        return {"input": DATA_BASE, "output": DATA_BASE + 8 * n,
                "steps": DATA_BASE + 16 * n if self.exact else None}

    def setup(self):
        return setup_lines(self.variant, self.n, self.count, self.exact)

    def text(self):
        n = self.n
        out = [f"# TCP CUBIC cube-root benchmark ({self.title}).",
               f"# Generated by examples/lns_int_cbrt/gen_cbrt.py; do not edit by hand.",
               f"# variant={self.variant} phases={','.join(self.phases)} "
               f"count_steps={int(self.count)} set={self.set_name} N={n}",
               f"# Kernel: Linux {KERNEL_TAG} cubic_root() ({KERNEL_URL})",
               f"# input @ {DATA_BASE:#x}, output @ {self.addresses['output']:#x}"
               + (f", steps @ {self.addresses['steps']:#x}" if self.exact else ""),
               ".data", "input:"]
        for i in range(0, n, 4):
            out.append("    .dword " + ", ".join(str(v) for v in self.values[i:i + 4]))
        out.append(f"output: .zero {8 * n}")
        if self.exact:
            out.append(f"steps: .zero {8 * n}")
        consts = consts_for(self.variant)
        if consts:
            out.append("consts: .dword " + ", ".join(f"{c:#x}" for c in consts))
        if self.variant == "L2":
            out.append("scratch: .zero 8")
        if self.variant in ("B0", "B1"):
            out.append("vtab: .byte " + ", ".join(map(str, V)))
        out += [".text", "main:"]
        out += ["    " + s for s in self.setup()]
        out.append("root_loop:")
        out += ["    " + s for s in LOOP_HEAD]
        for ph, blocks, _walk in self.parts:
            out.append(f"    # --- {ph}")
            for label, instrs, _phase in blocks:
                if label:
                    out.append(f"{label}:")
                out += ["    " + s for s in instrs]
        out += ["    " + s for s in loop_tail(self.store_reg, self.exact, self.count)]
        out += ["    " + s for s in EXIT]
        return "\n".join(out) + "\n"

    # -- static model ---------------------------------------------------------------------
    def static_profile(self, candidates=None):
        """Predicted dynamic counts. candidates[i] is the value entering the fix-up for
        input i (needed only for variants whose candidate is not computable here: L*, F0)."""
        phase_tot = Counter()
        class_tot = Counter()
        class_setup = Counter()

        def add(instrs, phase):
            for ins in instrs:
                w, c = words_and_class(ins)
                phase_tot[phase] += w
                class_tot[c] += w
                if phase == "setup":
                    class_setup[c] += w

        add(self.setup(), "setup")
        add(EXIT, "setup")
        tail = loop_tail(self.store_reg, self.exact, self.count)
        steps_hist = Counter()
        for i, a in enumerate(self.values):
            add(LOOP_HEAD, "loop")
            add([s for s in tail if "s6" not in s and "s2" not in s], "loop")
            add([s for s in tail if "s6" in s or "s2" in s], "count")
            st = {}
            if self.variant in ("B0", "B1"):
                st["x_kernel"] = kernel_cubic_root(a)
                st["candidate"] = refine(a, st["x_kernel"]) if "refine" in self.phases \
                    else st["x_kernel"]
            elif "fixup" in self.phases:
                st["candidate"] = candidates[i]
            for ph, blocks, walk in self.parts:
                for idx in walk(a, st):
                    _label, instrs, phase = blocks[idx]
                    add(instrs, phase)
            if "fixup" in self.phases:
                steps_hist[fixup(a, st["candidate"]) - (st["candidate"] & U64)] += 1
        total = sum(phase_tot.values())
        return {"total": total, "phases": dict(phase_tot), "classes": dict(class_tot),
                "classes_setup": dict(class_setup),
                "steps_hist": {str(k): v for k, v in sorted(steps_hist.items())}}


def all_programs():
    """(relative path, Program) for every program the benchmark uses."""
    sets = input_sets()
    progs = []
    for vid, (stem, phases, _exact) in VARIANTS.items():
        for name, values in sets.items():
            progs.append((f"{stem}_{set_file_tag(name)}.s",
                          Program(vid, values, name, title=f"{vid} {stem}, set {name}")))
    uni = sets[ABLATION_SET]
    for vid, (stem, phases, exact) in VARIANTS.items():
        # 0 = copy loop only (with this variant's setup), then one more phase per program.
        for k in range(0, len(phases) + 1):
            tag = "empty" if k == 0 else phases[k - 1]
            progs.append((f"ablation/{vid}_{k}_{tag}_nocount.s",
                          Program(vid, uni, ABLATION_SET, phases[:k], count=False,
                                  title=f"{vid} ablation, first {k} phase(s), no step count")))
        progs.append((f"ablation/{vid}_empty_n1.s",
                      Program(vid, uni[:1], ABLATION_SET, [], count=False,
                              title=f"{vid} ablation, copy loop, N=1")))
        for n in BATCH_SIZES:
            progs.append((f"batch/{stem}_n{n}.s",
                          Program(vid, uni[:n], ABLATION_SET, count=False,
                                  title=f"{vid} batch N={n}, no step count")))
    return progs


def references(sets):
    return {name: [{"input": str(a), "exact": exact_cbrt(a), "nearest": nearest_cbrt(a),
                    "kernel": kernel_cubic_root(a)} for a in values]
            for name, values in sets.items()}


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=here, help="output directory (default: this directory)")
    args = ap.parse_args()
    for sub in ("", "ablation", "batch"):
        os.makedirs(os.path.join(args.out, sub), exist_ok=True)
    progs = all_programs()
    for rel, prog in progs:
        with open(os.path.join(args.out, rel), "w", encoding="utf-8", newline="\n") as f:
            f.write(prog.text())
    sets = input_sets()
    with open(os.path.join(args.out, "reference.json"), "w", encoding="utf-8") as f:
        json.dump({"kernel_tag": KERNEL_TAG, "kernel_url": KERNEL_URL,
                   "note": "exact = floor(cbrt(a)), nearest = round(cbrt(a)), kernel = Python "
                           "port of cubic_root(); all integer arithmetic",
                   "sets": references(sets)}, f, indent=1)
        f.write("\n")
    counts = ", ".join(f"{k}={len(v)}" for k, v in sets.items())
    print(f"Generated {len(progs)} programs ({counts}) in {args.out}")


if __name__ == "__main__":
    main()
