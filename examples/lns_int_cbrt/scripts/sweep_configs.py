#!/usr/bin/env python3
"""Configuration sweep for the integer-root benchmark: root degree x number representation x
input width / distribution x LNS table sizes, on KITES rvss, every run verified.

The TCP CUBIC benchmark (gen_cbrt.py / check.py / sweep.sh) answers one question: k = 3 against
the Linux kernel's tuned cubic_root(). This script asks how that answer moves when the
configuration moves, in the spirit of the lns_lj sweep:

  root degree      k = 2, 3, 4, 5, 6  (floor(a^(1/k)) of an unsigned 64-bit a)
  representation   integer (BK), FP64 (FK, and FS = fsqrt.d for k = 2), LNS lpow (LK),
                   LNS log-divide (LDK)
  input width      uniform values of exactly w bits, w = 8, 16, 24, 32, 40, 48, 56, 64
  distribution     small (0..255), uniform bit length, uniform u64 value, perfect powers +-1,
                   the top of the u64 range
  LNS tables       LNS_LOG_LUT_BITS x LNS_EXP_LUT_BITS swept independently (simulator: k = 3
                   with the existing L0/L1/L2 programs through check.py --rows; model: every k)

Variants (all exact except where noted; "fix-up" = clamp to the k-th-root cap, then +/-1 steps
with a mul-based x^k test, the same loop as the cube-root programs with x^3 replaced by x^k):
  BK   software fls64 -> seed 2^ceil(bits/k) (>= root) -> integer Newton
       x' = ((k-1)x + a / x^(k-1)) / k until x' >= x. No fix-up needed (Newton from above).
  LK   fcvt.lns.lu -> lpow(x, 1/k) -> fcvt.lu.lns -> fix-up.
  LDK  fcvt.lns.lu -> (log field + 1) / k by reciprocal multiply (shift for k = 2, 4)
       -> fcvt.lu.lns -> fix-up.
  FK   fcvt.d.lu -> bit-trick seed bits/k + (1 - 1/k) * bits(1.0) -> 3 FP64 Newton steps
       (fmul/fdiv/fadd, no fmadd.d: broken on rvss) -> fcvt.lu.d -> fix-up.
  FS   k = 2 only: fcvt.d.lu -> fsqrt.d -> fcvt.lu.d -> fix-up (what real FP hardware would do).

Checks for every simulator run (any failure exits non-zero):
  * output == exact floor k-th root (Python integers) on every input;
  * output and fix-up candidate == the C++ model (tools/lns_cbrt_accuracy --keval), bit for bit;
  * instructions_retired == this script's static model (blocks x path counts);
  * the recorded fix-up step / Newton iteration histogram == the static model's.

Usage (repo root; needs kites-cli and the model binary, see RESULTS.md "Reproduce"):
  KITES_CLI=build/kites-cli CBRT_MODEL=./lns_cbrt_accuracy \
      python3 examples/lns_int_cbrt/scripts/sweep_configs.py sim      # -> config_sweep.csv
  CBRT_MODEL=./lns_cbrt_accuracy python3 examples/lns_int_cbrt/scripts/sweep_configs.py model
                                                              # -> model_grid.csv (C++ grid)
  python3 examples/lns_int_cbrt/scripts/sweep_configs.py summary      # tables for RESULTS.md
Programs are written to examples/lns_int_cbrt/programs/configs/ (generated, gitignored).
"""

from __future__ import annotations

import argparse
import csv
import io
import os
import struct
import subprocess
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))          # scripts/
ROOT = os.path.dirname(HERE)                                  # examples/lns_int_cbrt/
PROG = os.path.join(ROOT, "programs")                         # .s programs, reference.json
RESD = os.path.join(ROOT, "results")                          # CSV / JSON results
sys.path.insert(0, HERE)
import gen_cbrt as g  # noqa: E402
import check as chk  # noqa: E402

U64 = g.U64
KS = (2, 3, 4, 5, 6)
MODEL_KS = (2, 3, 4, 5, 6, 7, 8)
WIDTHS = (8, 16, 24, 32, 40, 48, 56, 64)
PER_SET = 128
FP_STEPS = 3
OUT_DIR = os.path.join(PROG, "configs")
# k = 2 with log tables below 10 bits needs up to ~4,400 (8 bits) or ~62,000 (6 bits) fix-up
# steps per root on 64-bit inputs: hours of simulation per program. Those configurations are
# covered by the C++ model grid (model mode) only.
K2_MIN_LOG_BITS = 10
SIM_CSV = os.path.join(RESD, "config_sweep.csv")
GRID_CSV = os.path.join(RESD, "model_grid.csv")


# ---------------------------------------------------------------------------------------------
# Integer reference and constants
# ---------------------------------------------------------------------------------------------
def iroot(a: int, k: int) -> int:
    """floor(a^(1/k)) with Python integers."""
    if a < 2:
        return a
    x = 1 << -(-a.bit_length() // k)
    while True:
        y = ((k - 1) * x + a // x ** (k - 1)) // k
        if y >= x:
            break
        x = y
    while (x + 1) ** k <= a:
        x += 1
    while x ** k > a:
        x -= 1
    return x


def cap(k: int) -> int:
    return iroot(U64, k)


def lkc_word(k: int) -> int:
    """LNS word of 1/k with a correctly rounded Q9.22 log: round(-log2(k) * 2^22), sign 0
    (60-digit decimal logarithm, so the rounding is exact)."""
    import decimal
    decimal.getcontext().prec = 60
    lg = decimal.Decimal(k).ln() / decimal.Decimal(2).ln()
    field = -int((lg * (1 << 22)).to_integral_value(rounding=decimal.ROUND_HALF_EVEN))
    return field & 0x7FFFFFFF                      # 31-bit two's-complement field, sign bit 0


def is_pow2(v: int) -> bool:
    return v & (v - 1) == 0


def recip(k: int, vmax: int) -> tuple[int, int]:
    """(M, S) with floor(v / k) == (v * M) >> S for every 0 <= v <= vmax and v * M < 2^64."""
    for s in range(1, 64):
        m = -(-(1 << s) // k)
        e = m * k - (1 << s)
        if vmax * e < (1 << s) and vmax * m < (1 << 64):
            return m, s
    raise ValueError(f"no reciprocal for k={k}, vmax={vmax}")


def seed_bits(k):
    return -(-64 // k)                       # ceil(64 / k): largest seed exponent


def bk_vmax(k):
    """Bound on (k-1)x + a / x^(k-1) inside the Newton loop (x >= root, x <= 2^ceil(64/k))."""
    return (k - 1 + (1 << k)) << seed_bits(k)


def fp_mulhu(k):
    """(R, s): bits/k ~= mulhu(bits, R) >> s, with R = ceil(2^(64+s)/k) < 2^64, s largest."""
    best = None
    for s in range(0, 8):
        r = -(-(1 << (64 + s)) // k)
        if r < (1 << 64):
            best = (r, s)
    return best


def fp_magic(k):
    return ((k - 1) * 0x3FF0000000000000) // k


def dbits(x: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", x))[0]


# ---------------------------------------------------------------------------------------------
# Input sets (deterministic)
# ---------------------------------------------------------------------------------------------
def sweep_sets(k: int) -> dict[str, list[int]]:
    sets = {}
    rng = g.Lcg(0x51DE + k)
    for w in WIDTHS:
        vals = []
        for _ in range(PER_SET):
            low = rng.next64() & ((1 << (w - 1)) - 1)
            vals.append((1 << (w - 1)) | low)
        sets[f"width{w}"] = vals
    sets["small"] = list(range(256))
    sets["uniform_bits"] = g.input_sets()["uniform_bits"][:256]
    rng = g.Lcg(0xFADE + k)
    sets["uniform_u64"] = [rng.next64() for _ in range(256)]
    c = cap(k)
    rs = sorted({int(round(c ** (i / 59))) for i in range(60)} | {2, 3, c - 1, c})
    sets["powers"] = sorted({r ** k + d for r in rs for d in (-1, 0, 1) if 0 <= r ** k + d <= U64})
    top = {U64 - d for d in range(32)} | {(1 << 63) + d for d in (-1, 0, 1)}
    top |= {c ** k + d for d in (-1, 0, 1) if c ** k + d <= U64}
    sets["u64_top"] = sorted(top)
    return sets


# ---------------------------------------------------------------------------------------------
# Code blocks: (label, [instrs], phase); a "walk" yields block indices per input
# ---------------------------------------------------------------------------------------------
CLASS_EXTRA = {"fsqrt.d": "fp_sqrt", "fdiv.d": "fp_div"}


def words_and_class(ins):
    op = ins.split()[0]
    if op in CLASS_EXTRA:
        return 1, CLASS_EXTRA[op]
    return g.words_and_class(ins)


def pow_seq(dst, src, k):
    """dst = src^k (k >= 2) with k - 1 muls."""
    return [f"mul {dst}, {src}, {src}"] + [f"mul {dst}, {dst}, {src}"] * (k - 2)


def divk_seq(reg, k, mreg, s):
    if is_pow2(k):
        return [f"srli {reg}, {reg}, {k.bit_length() - 1}"]
    return [f"mul {reg}, {reg}, {mreg}", f"srli {reg}, {reg}, {s}"]


def fixup_blocks(k, count):
    pre = [("", ["mv s6, s5"], "count")] if count else []
    post = [("", ["sub s6, s5, s6"], "count")] if count else []
    return pre + [
        ("", ["bgeu s7, s5, fx_down"], "fixup"),
        ("", ["mv s5, s7"], "fixup"),
        ("fx_down", pow_seq("t0", "s5", k) + ["bltu s4, t0, fx_dec"], "fixup"),
        ("fx_up", ["beq s5, s7, fx_done"], "fixup"),
        ("", ["addi t1, s5, 1"] + pow_seq("t0", "t1", k) + ["bltu s4, t0, fx_done"], "fixup"),
        ("", ["mv s5, t1", "j fx_up"], "fixup"),
        ("fx_dec", ["addi s5, s5, -1"] + pow_seq("t0", "s5", k) + ["bltu s4, t0, fx_dec"],
         "fixup"),
        ("fx_done", [], "fixup"),
    ] + post


def fixup_walk(a, x, k, count):
    c = cap(k)
    off = 1 if count else 0
    path = [0]
    x &= U64
    if x > c:
        path.append(1)
        x = c
    path.append(2)
    if x ** k > a:
        while True:
            path.append(6)
            x -= 1
            if not x ** k > a:
                break
    else:
        while True:
            path.append(3)
            if x == c:
                break
            path.append(4)
            if (x + 1) ** k > a:
                break
            path.append(5)
            x += 1
    path.append(7)
    path = [off + i for i in path]
    return ([0] + path + [off + 8]) if count else path


def fixup_cost(k, cand, final):
    """Instructions of one fix-up (uninstrumented), as a function of (candidate, result) only.
    The model grid uses this; sim() asserts it equals the block walk on every simulated input."""
    c = cap(k)
    cl = 1 if (cand & U64) > c else 0
    x = min(cand & U64, c)
    n = 1 + cl + k                                     # clamp test (+ clamp) + fx_down
    if final < x:
        return n + (x - final) * (k + 1)
    up = final - x
    return n + up * (1 + (k + 1) + 2) + (1 if final == c else 1 + (k + 1))


def bk_blocks(k, count, m2, s2, m1, s1):
    head = [("", ["mv s5, s4"] + (["li s6, 0"] if count else []) + ["beqz s4, bk_done"],
             "newton")]
    fls = [(lab or "", ins, ph) for lab, ins, ph in g.fls64_blocks()]
    seed = [("bk_seed", [f"addi t0, t0, {k - 1}"] + divk_seq("t0", k, "s10", s1) +
             ["li t1, 1", "sll s5, t1, t0"], "newton")]
    # x^(k-1) into t0 (k = 2: x itself), q = a / x^(k-1), t0 = (k-1)x + q, t0 /= k
    xp = ["mv t0, s5"] if k == 2 else pow_seq("t0", "s5", k - 1)
    if k == 2:
        km1 = []
        add = ["add t0, t0, s5"]
    elif is_pow2(k - 1):
        km1 = [f"slli t1, s5, {(k - 1).bit_length() - 1}"]
        add = ["add t0, t0, t1"]
    else:
        km1 = ["mul t1, s5, s9"]
        add = ["add t0, t0, t1"]
    loop = [("bk_loop", xp + ["divu t0, s4, t0"] + km1 + add + divk_seq("t0", k, "s11", s2) +
             ["bgeu t0, s5, bk_done"], "newton"),
            ("", ["mv s5, t0"] + (["addi s6, s6, 1"] if count else []) + ["j bk_loop"],
             "newton"),
            ("bk_done", [], "newton")]
    return head + fls + seed + loop


def bk_run(a, k):
    """(result, iterations) of the BK assembly, with its exact integer semantics."""
    if a == 0:
        return 0, 0
    e = (a.bit_length() + k - 1) // k
    x = 1 << e
    it = 0
    while True:
        assert x ** (k - 1) < (1 << 64)
        v = (k - 1) * x + a // x ** (k - 1)
        assert v <= bk_vmax(k)
        y = v // k
        if y >= x:
            return x, it
        x = y
        it += 1


def bk_walk(a, k, nfls):
    """Block indices for BK: head, fls64 path, seed, loop iterations, done."""
    if a == 0:
        return [0, nfls + 4]
    path = [0] + [1 + i for i in g.fls64_walk(a)] + [nfls + 1]
    _x, it = bk_run(a, k)
    path += [nfls + 2, nfls + 3] * it + [nfls + 2, nfls + 4]
    return path


def count_split(instrs):
    """Instrumentation lines inside BK blocks (s6 iteration counter) go to the count phase."""
    return [i for i in instrs if "s6" not in i], [i for i in instrs if "s6" in i]


# ---------------------------------------------------------------------------------------------
# Programs
# ---------------------------------------------------------------------------------------------
class KProg:
    def __init__(self, variant, k, set_name, values, count=True):
        self.variant, self.k, self.set_name = variant, k, set_name
        self.values = list(values)
        self.count = count
        self.consts = []
        k_ = k
        if variant == "BK":
            self.m2, self.s2 = recip(k_, bk_vmax(k_)) if not is_pow2(k_) else (0, 0)
            self.m1, self.s1 = recip(k_, 64 + k_) if not is_pow2(k_) else (0, 0)
            self.consts = [self.m2, self.m1, k_ - 1]
        elif variant == "LDK":
            self.m, self.s = recip(k_, (64 << 22) + 1) if not is_pow2(k_) else (0, 0)
            self.consts = [self.m]
        elif variant == "LKc":
            self.consts = [lkc_word(k_)]
        elif variant in ("FK", "FS"):
            r, s = fp_mulhu(k_)
            self.fr, self.fs = r, s
            self.consts = [r, fp_magic(k_), dbits(float(k_ - 1)), dbits(1.0 / k_)]
        self.cap_off = None
        if variant != "BK" and cap(k_) >= (1 << 31):     # k = 2: 2^32 - 1 does not fit li
            self.cap_off = 8 * len(self.consts)
            self.consts.append(cap(k_))

    @property
    def n(self):
        return len(self.values)

    @property
    def rel(self):
        return f"configs/k{self.k}_{self.variant}_{self.set_name}.s"

    def setup(self):
        k = self.k
        lines = ["la s0, input", "la s1, output", f"li s3, {self.n}"]
        if self.count:
            lines.insert(2, "la s2, steps")
        if self.variant != "BK" and self.cap_off is None:
            lines.append(f"li s7, {cap(k)}")
        if self.variant == "BK":
            lines += ["la t0, consts", "ld s11, 0(t0)", "ld s10, 8(t0)", "ld s9, 16(t0)"]
        elif self.variant == "LK":
            lines += ["li t0, 1", "fcvt.lns.w l10, t0", f"li t0, {k}", "fcvt.lns.w l11, t0",
                      "ldiv l12, l10, l11"]
            if self.cap_off is not None:
                lines += ["la t0, consts"]
        elif self.variant == "LKc":
            # 1/k as a correctly rounded LNS constant: no fcvt/ldiv rounding on the exponent
            lines += ["la t0, consts", "llw l12, 0(t0)"]
        elif self.variant == "LDK":
            lines += ["la s8, scratch", "la t0, consts", "ld s11, 0(t0)"]
        else:
            lines += ["la t0, consts", "ld s11, 0(t0)", "ld s9, 8(t0)", "ld t1, 16(t0)",
                      "fmv.d.x f10, t1", "ld t1, 24(t0)", "fmv.d.x f11, t1"]
        if self.cap_off is not None:
            lines.append(f"ld s7, {self.cap_off}(t0)")
        return lines

    def body(self):
        k, v = self.k, self.variant
        if v == "BK":
            return bk_blocks(k, self.count, self.m2, self.s2, self.m1, self.s1)
        if v in ("LK", "LKc"):
            pre = [("", ["fcvt.lns.lu l1, s4"], "convert_in"), ("", ["lpow l2, l1, l12"], "lpow"),
                   ("", ["fcvt.lu.lns s5, l2"], "convert_out")]
        elif v == "LDK":
            pre = [("", ["fcvt.lns.lu l1, s4"], "convert_in"),
                   ("", ["beqz s4, ld_done"], "logdiv"),
                   ("", ["lsw l1, 0(s8)", "lwu t0, 0(s8)", "addi t0, t0, 1"] +
                    divk_seq("t0", k, "s11", self.s) + ["sw t0, 0(s8)", "llw l1, 0(s8)"],
                    "logdiv"),
                   ("ld_done", [], "logdiv"),
                   ("", ["fcvt.lu.lns s5, l1"], "convert_out")]
        elif v == "FS":
            pre = [("", ["fcvt.d.lu f1, s4"], "convert_in"), ("", ["fsqrt.d f2, f1"], "fp_newton"),
                   ("", ["fcvt.lu.d s5, f2"], "convert_out")]
        else:  # FK
            r_shift = [f"srli t0, t0, {self.fs}"] if self.fs else []
            body = ["fmv.x.d t0, f1", "mulhu t0, t0, s11"] + r_shift + ["add t0, t0, s9",
                                                                        "fmv.d.x f2, t0"]
            for _ in range(FP_STEPS):
                if k == 2:
                    body += ["fdiv.d f3, f1, f2", "fadd.d f3, f3, f2"]
                else:
                    body += ["fmul.d f3, f2, f2"] + ["fmul.d f3, f3, f2"] * (k - 3)
                    body += ["fdiv.d f3, f1, f3", "fmul.d f4, f2, f10", "fadd.d f3, f3, f4"]
                body += ["fmul.d f2, f3, f11"]
            pre = [("", ["fcvt.d.lu f1, s4"], "convert_in"), ("", body, "fp_newton"),
                   ("", ["fcvt.lu.d s5, f2"], "convert_out")]
        return pre + fixup_blocks(k, self.count)

    def text(self):
        n = self.n
        out = [f"# Integer k-th root sweep: k={self.k} variant={self.variant} set={self.set_name}"
               f" N={n} count_steps={int(self.count)}",
               "# Generated by examples/lns_int_cbrt/scripts/sweep_configs.py; do not edit by hand.",
               ".data", "input:"]
        for i in range(0, n, 4):
            out.append("    .dword " + ", ".join(str(x) for x in self.values[i:i + 4]))
        out.append(f"output: .zero {8 * n}")
        if self.count:
            out.append(f"steps: .zero {8 * n}")
        consts = self.consts
        if consts:
            out.append("consts: .dword " + ", ".join(f"{c:#x}" for c in consts))
        if self.variant == "LDK":
            out.append("scratch: .zero 8")
        out += [".text", "main:"] + ["    " + s for s in self.setup()] + ["root_loop:",
                                                                         "    ld s4, 0(s0)"]
        for lab, ins, _ph in self.body():
            if lab:
                out.append(f"{lab}:")
            out += ["    " + s for s in ins]
        tail = ["sd s5, 0(s1)"] + (["sd s6, 0(s2)", "addi s2, s2, 8"] if self.count else [])
        tail += ["addi s0, s0, 8", "addi s1, s1, 8", "addi s3, s3, -1", "bnez s3, root_loop",
                 "li a7, 10", "ecall"]
        out += ["    " + s for s in tail]
        return "\n".join(out) + "\n"

    def walk(self, a, cand):
        """Block indices for input a (cand = fix-up candidate, from the C++ model)."""
        blocks = self.body()
        if self.variant == "BK":
            nfls = len(g.fls64_blocks())
            return blocks, bk_walk(a, self.k, nfls)
        nfix = 8 + (2 if self.count else 0)
        npre = len(blocks) - nfix
        if self.variant == "LDK":
            pre = [0, 1, 3, 4] if a == 0 else [0, 1, 2, 3, 4]
        else:
            pre = list(range(npre))
        return blocks, pre + [npre + i for i in fixup_walk(a, cand, self.k, self.count)]

    def static_profile(self, cands):
        ph, cls = Counter(), Counter()
        setup_cls = Counter()

        def add(instrs, phase):
            for ins in instrs:
                w, c = words_and_class(ins)
                ph[phase] += w
                cls[c] += w
                if phase == "setup":
                    setup_cls[c] += w
        add(self.setup() + ["li a7, 10", "ecall"], "setup")
        hist = Counter()
        fix_costs = []
        for i, a in enumerate(self.values):
            add(["ld s4, 0(s0)", "sd s5, 0(s1)", "addi s0, s0, 8", "addi s1, s1, 8",
                 "addi s3, s3, -1", "bnez s3, root_loop"], "loop")
            if self.count:
                add(["sd s6, 0(s2)", "addi s2, s2, 8"], "count")
            blocks, path = self.walk(a, cands[i] if cands else None)
            before = ph["fixup"]
            for idx in path:
                _lab, ins, phase = blocks[idx]
                main, cnt = count_split(ins) if self.variant == "BK" else (ins, [])
                add(main, phase)
                add(cnt, "count")
            if self.variant == "BK":
                hist[bk_run(a, self.k)[1]] += 1
            else:
                final = iroot(a, self.k)
                hist[final - (cands[i] & U64)] += 1
                fix_costs.append((ph["fixup"] - before, fixup_cost(self.k, cands[i], final)))
        return {"total": sum(ph.values()), "phases": dict(ph), "classes": dict(cls),
                "setup_classes": dict(setup_cls), "hist": dict(hist), "fix_costs": fix_costs}


def all_kprogs(count=True):
    progs = []
    for k in KS:
        variants = ["BK", "LK", "LKc", "LDK", "FK"] + (["FS"] if k == 2 else [])
        for name, vals in sweep_sets(k).items():
            for v in variants:
                progs.append(KProg(v, k, name, vals, count))
    return progs


# ---------------------------------------------------------------------------------------------
# Simulator sweep
# ---------------------------------------------------------------------------------------------
def fixed_cost(variant, k, zero=False):
    """Per-root instructions outside the fix-up (loop + convert + compute), no step counting,
    for LK / LDK (input a != 0, or a == 0 when zero=True)."""
    p = KProg(variant, k, "x", [0 if zero else 3], count=False)
    blocks, path = p.walk(0 if zero else 3, 0)
    pre = [i for i in path if i < len(blocks) - 8]
    return 6 + sum(words_and_class(ins)[0] for i in pre for ins in blocks[i][1])


def model_args(variant, k):
    if variant in ("LK", "LKc", "LDK"):
        return [str(fixed_cost(variant, k)), str(fixed_cost(variant, k, True))]
    return ["0", "0"]


def model_keval(prog, path, env=None):
    """[(output, fix-up candidate, model instructions for this root)] from the C++ model."""
    e = dict(os.environ)
    e.update(env or {})
    out = subprocess.run([chk.CBRT_MODEL, "--keval", prog.variant, str(prog.k), path] +
                         model_args(prog.variant, prog.k), env=e, check=True,
                         stdout=subprocess.PIPE, text=True).stdout
    return [tuple(int(t) for t in line.split()) for line in out.splitlines()]


SIM_COLUMNS = ("k,variant,representation,input_set,n_roots,log_bits,exp_bits,instr_total,"
               "instr_setup,instr_per_root,instr_per_root_uninstrumented,instr_convert,"
               "instr_compute,instr_fixup,pct_exact,candidate_pct_exact,steps_mean_abs,"
               "steps_max_abs,steps_hist,mul_per_root,div_per_root,lns_per_root,fp_per_root,"
               "code_words,program").split(",")
REPR = {"BK": "integer", "LK": "lns_lpow", "LKc": "lns_lpow_const_exponent", "LDK": "lns_logdiv", "FK": "fp64_newton",
        "FS": "fp64_sqrt"}


def run_one(prog, env, tables):
    path = os.path.join(PROG, prog.rel)
    rows = prog.n * (3 if prog.count else 2)
    total, words, mem = chk.run_program(path, rows, env)
    n = prog.n
    outs, steps = mem[n:2 * n], [chk.signed64(s) for s in mem[2 * n:3 * n]]
    fails = []
    if mem[:n] != prog.values:
        fails.append("input array mismatch")
    model = model_keval(prog, path, env)
    if [m[0] for m in model] != outs:
        fails.append(f"{sum(o != m[0] for o, m in zip(outs, model))} outputs != C++ model")
    exact = [iroot(a, prog.k) for a in prog.values]
    bad = sum(o != e for o, e in zip(outs, exact))
    if bad:
        fails.append(f"{bad} outputs are not the exact floor root")
    cands = [m[1] for m in model]
    prof = prog.static_profile(None if prog.variant == "BK" else cands)
    if prof["total"] != total:
        fails.append(f"retired {total} != static model {prof['total']}")
    if prog.variant == "BK":
        sim_hist = Counter(steps)
    else:
        sim_hist = Counter(steps)
        if any(o - s != c & U64 for o, s, c in zip(outs, steps, cands)):
            fails.append("recorded fix-up candidates != C++ model")
        if any(w != f for w, f in prof["fix_costs"]):
            fails.append("fixup_cost() formula != block walk")
        if prog.variant in ("LK", "LKc", "LDK"):
            ph_ = prof["phases"]
            uninstr = prof["total"] - ph_.get("setup", 0) - ph_.get("count", 0)
            if sum(m[2] for m in model) != uninstr:
                fails.append(f"C++ model cost {sum(m[2] for m in model)} != static {uninstr}")
    if dict(sim_hist) != prof["hist"]:
        fails.append("step histogram != static model")
    ph = prof["phases"]
    setup = ph.get("setup", 0)
    per_root = (total - setup) / n
    cnt = ph.get("count", 0) / n
    hist = prof["hist"]
    cl = Counter(prof["classes"])
    cl.subtract(prof["setup_classes"])
    cand_exact = 100.0 * sum((c & U64) == e for c, e in zip(cands, exact)) / n \
        if prog.variant != "BK" else ""
    row = {
        "k": prog.k, "variant": prog.variant, "representation": REPR[prog.variant],
        "input_set": prog.set_name, "n_roots": n,
        "log_bits": tables[0] if prog.variant in ("LK", "LKc", "LDK") else "",
        "exp_bits": tables[1] if prog.variant in ("LK", "LKc", "LDK") else "",
        "instr_total": total, "instr_setup": setup, "instr_per_root": f"{per_root:.4f}",
        "instr_per_root_uninstrumented": f"{per_root - cnt:.4f}",
        "instr_convert": f"{(ph.get('convert_in', 0) + ph.get('convert_out', 0)) / n:.4f}",
        "instr_compute": f"{sum(ph.get(p, 0) for p in ('newton', 'fls64', 'lpow', 'logdiv', 'fp_newton')) / n:.4f}",
        "instr_fixup": f"{ph.get('fixup', 0) / n:.4f}",
        "pct_exact": f"{100.0 * (n - bad) / n:.2f}",
        "candidate_pct_exact": f"{cand_exact:.2f}" if cand_exact != "" else "",
        "steps_mean_abs": f"{sum(abs(s) * c for s, c in hist.items()) / n:.4f}",
        "steps_max_abs": max(abs(s) for s in hist),
        "steps_hist": ";".join(f"{s}:{c}" for s, c in sorted(hist.items())),
        "mul_per_root": f"{cl['mul'] / n:.3f}", "div_per_root": f"{cl['div'] / n:.3f}",
        "lns_per_root": f"{sum(cl[c] for c in ('lns_convert', 'lns_pow', 'lns_alu', 'lns_mem')) / n:.3f}",
        "fp_per_root": f"{sum(cl[c] for c in cl if c.startswith('fp_')) / n:.3f}",
        "code_words": words, "program": prog.rel,
    }
    return row, fails


def write_programs(progs):
    os.makedirs(OUT_DIR, exist_ok=True)
    for p in progs:
        with open(os.path.join(PROG, p.rel), "w", newline="\n") as f:
            f.write(p.text())


def sim_mode(table_cfgs, only=None):
    """only: list of variants to (re)run; their rows replace the same variants' rows in
    config_sweep.csv and every other row is kept."""
    import concurrent.futures as cf
    progs = all_kprogs()
    write_programs(progs)
    if only:
        progs = [p for p in progs if p.variant in only]
    jobs = []
    for p in progs:
        if p.variant in ("LK", "LKc", "LDK"):
            for lb, eb in table_cfgs:
                if p.k == 2 and lb < K2_MIN_LOG_BITS:
                    continue
                jobs.append((p, {"LNS_LOG_LUT_BITS": str(lb), "LNS_EXP_LUT_BITS": str(eb)},
                             (lb, eb)))
        else:
            jobs.append((p, {}, (11, 11)))
    rows, failures = [], []
    with cf.ThreadPoolExecutor(max_workers=chk.JOBS) as pool:
        futs = {pool.submit(run_one, p, env, t): (p, t) for p, env, t in jobs}
        for i, fut in enumerate(cf.as_completed(futs), 1):
            p, t = futs[fut]
            row, fails = fut.result()
            rows.append(row)
            failures += [f"{p.rel} {t}: {f}" for f in fails]
            if i % 50 == 0:
                print(f"  {i}/{len(jobs)} runs", file=sys.stderr)
    order = {v: i for i, v in enumerate(("BK", "FK", "FS", "LK", "LKc", "LDK"))}
    if only and os.path.exists(SIM_CSV):
        keep = [r for r in csv.DictReader(open(SIM_CSV)) if r["variant"] not in only]
        for r in keep:
            r["k"] = int(r["k"])
        rows += keep
    rows.sort(key=lambda r: (int(r["k"]), order[r["variant"]], r["input_set"],
                             int(r["log_bits"] or 0), int(r["exp_bits"] or 0)))
    with open(SIM_CSV, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=SIM_COLUMNS, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    roots = sum(int(r["n_roots"]) for r in rows)
    print(f"{len(rows)} simulator runs, {roots} verified roots -> {SIM_CSV}")
    if failures:
        print(f"{len(failures)} FAILURE(S):")
        for f in failures[:50]:
            print("  " + f)
        return 1
    print("ALL CHECKS PASSED")
    return 0


def model_mode(n):
    """Large grids in the C++ model: k x table sizes x input sets for LK / LDK (accuracy,
    fix-up steps, and instructions per root from the simulator-validated cost formula)."""
    costs = ",".join(f"{v}{k}={fixed_cost(v, k)}/{fixed_cost(v, k, True)}"
                     for k in MODEL_KS for v in ("LK", "LKc", "LDK"))
    out = subprocess.run([chk.CBRT_MODEL, "--ksweep", str(n), costs], check=True,
                         stdout=subprocess.PIPE, text=True).stdout
    with open(GRID_CSV, "w", newline="") as f:
        f.write(out)
    print(f"{out.count(chr(10)) - 1} model rows -> {GRID_CSV}")
    return 0


# ---------------------------------------------------------------------------------------------
# Summary tables (markdown) for RESULTS.md
# ---------------------------------------------------------------------------------------------
def summary():
    sim = list(csv.DictReader(open(SIM_CSV)))
    grid = list(csv.DictReader(open(GRID_CSV))) if os.path.exists(GRID_CSV) else []
    res = list(csv.DictReader(open(os.path.join(RESD, "results.csv"))))

    def ipr(r):
        return float(r["instr_per_root_uninstrumented"])

    def pick(k, v, st, lb="11", eb="11"):
        for r in sim:
            if (r["k"] == str(k) and r["variant"] == v and r["input_set"] == st and
                    (v not in ("LK", "LDK") or (r["log_bits"] == lb and r["exp_bits"] == eb))):
                return r
        return None

    vs = ("BK", "FK", "FS", "LK", "LDK")
    print("### A. Instructions per root by root degree (default 11/11 tables)\n")
    for st in ("uniform_bits", "uniform_u64"):
        print(f"Input set `{st}`:\n")
        print("| k | " + " | ".join(vs) + " | BK / best LNS | best FP / best LNS |")
        print("|---|" + "---:|" * (len(vs) + 2))
        for k in KS:
            c = {v: pick(k, v, st) for v in vs}
            cells = [f"{ipr(c[v]):.1f}" if c[v] else "-" for v in vs]
            lns = min(ipr(c[v]) for v in ("LK", "LDK"))
            fp = min(ipr(c[v]) for v in ("FK", "FS") if c[v])
            print(f"| {k} | " + " | ".join(cells) +
                  f" | {ipr(c['BK']) / lns:.2f} | {fp / lns:.2f} |")
        print()
    print("### B. Input bit width (default tables): instructions per root\n")
    for k in (2, 3, 6):
        print(f"k = {k}:\n")
        print("| width | " + " | ".join(v for v in vs if k == 2 or v != "FS") +
              " | LK max steps | LDK max steps |")
        print("|---|" + "---:|" * (len(vs) - (0 if k == 2 else 1) + 2))
        for w in WIDTHS:
            st = f"width{w}"
            cells = [f"{ipr(pick(k, v, st)):.1f}" for v in vs if k == 2 or v != "FS"]
            print(f"| {w} | " + " | ".join(cells) +
                  f" | {pick(k, 'LK', st)['steps_max_abs']} | {pick(k, 'LDK', st)['steps_max_abs']} |")
        print()
    print("### C. Fix-up / Newton-iteration distribution, all sweep sets pooled (11/11)\n")
    print("| k | variant | roots | candidate exact | mean abs steps | max abs steps | "
          "share with abs steps <= 1 |")
    print("|---|---|---:|---:|---:|---:|---:|")
    for k in KS:
        for v in vs:
            rows = [r for r in sim if r["k"] == str(k) and r["variant"] == v and
                    (v not in ("LK", "LDK") or r["log_bits"] == "11")]
            if not rows:
                continue
            h = Counter()
            for r in rows:
                for it in r["steps_hist"].split(";"):
                    s_, c_ = it.split(":")
                    h[int(s_)] += int(c_)
            n = sum(h.values())
            ce = sum(float(r["candidate_pct_exact"]) * int(r["n_roots"]) for r in rows) / n \
                if v != "BK" else None
            le1 = 100.0 * sum(c_ for s_, c_ in h.items() if abs(s_) <= 1) / n
            print(f"| {k} | {v}{' (Newton iters)' if v == 'BK' else ''} | {n} | "
                  f"{'-' if ce is None else f'{ce:.1f}%'} | "
                  f"{sum(abs(s_) * c_ for s_, c_ in h.items()) / n:.2f} | "
                  f"{max(abs(s_) for s_ in h)} | {le1:.1f}% |")
    print()
    print("### D. LNS table size (simulator, set uniform_bits): instructions per root\n")
    cfgs = sorted({(int(r["log_bits"]), int(r["exp_bits"])) for r in sim if r["log_bits"]})
    print("| k | variant | " + " | ".join(f"{a}/{b}" for a, b in cfgs) + " | BK |")
    print("|---|---|" + "---:|" * (len(cfgs) + 1))
    for k in KS:
        for v in ("LK", "LDK"):
            cells = []
            for a, b in cfgs:
                r = pick(k, v, "uniform_bits", str(a), str(b))
                cells.append(f"{ipr(r):.1f}" if r else "(model)")
            print(f"| {k} | {v} | " + " | ".join(cells) +
                  f" | {ipr(pick(k, 'BK', 'uniform_bits')):.1f} |")
    print()
    print("### E. Independent log x exp tables, k = 3 (simulator, results.csv, uniform_bits)\n")
    for v in ("L1", "L2"):
        cell = {}
        for r in res:
            if r["variant"] == v and r["input_set"] == "uniform_bits" and r["log_bits"] and \
                    not r["program"].startswith("batch/"):
                cell[(int(r["log_bits"]), int(r["exp_bits"]))] = \
                    float(r["instr_per_root_uninstrumented"])
        logs = sorted({a for a, _b in cell})
        exps = sorted({b for _a, b in cell})
        print(f"{v} instructions per root (rows: log bits, columns: exp bits; B1 = "
              f"{[float(r['instr_per_root_uninstrumented']) for r in res if r['variant'] == 'B1' and r['input_set'] == 'uniform_bits' and not r['program'].startswith('batch/')][0]:.1f}):\n")
        print("| log \\ exp | " + " | ".join(map(str, exps)) + " |")
        print("|---|" + "---:|" * len(exps))
        for a in logs:
            print(f"| {a} | " + " | ".join(f"{cell[(a, b)]:.1f}" if (a, b) in cell else ""
                                           for b in exps) + " |")
        print()
    if grid:
        print("### F. C++ model grid: smallest tables that beat the integer baseline\n")
        print("Per k and input set (200,000 model inputs per set; BK is the simulated cost on "
              "the same-named 128/256-input simulator set): the lowest modelled LK cost over "
              "all 121 table pairs, the smallest table pair (bytes) at which LK is cheaper "
              "than BK, and the worst fix-up even with the largest tables.\n")
        print("| k | input set | BK (sim) | best LK (model) | at tables | smallest tables "
              "with LK < BK | max steps at 23/22 |")
        print("|---|---|---:|---:|---|---|---:|")
        simset = {"uniform_bits": "uniform_bits", "uniform_u64": "uniform_u64",
                  "width16": "width16", "width32": "width32", "width48": "width48",
                  "width64": "width64", "small": "small"}
        for k in KS:
            for st in ("uniform_bits", "uniform_u64", "width16", "width32", "width64"):
                rows = [r for r in grid if r["k"] == str(k) and r["variant"] == "LK" and
                        r["input_set"] == st]
                bk = ipr(pick(k, "BK", simset[st]))
                best = min(rows, key=lambda r: float(r["instr_per_root_model"]))
                win = [r for r in rows if float(r["instr_per_root_model"]) < bk]
                small = min(win, key=lambda r: int(r["table_bytes"])) if win else None
                top = [r for r in rows if r["log_bits"] == "23" and r["exp_bits"] == "22"][0]
                print(f"| {k} | {st} | {bk:.1f} | {float(best['instr_per_root_model']):.1f} | "
                      f"{best['log_bits']}/{best['exp_bits']} | "
                      + (f"{small['log_bits']}/{small['exp_bits']} ({int(small['table_bytes'])} B)"
                         if small else "none") + f" | {top['steps_max_abs']} |")
        print()
    print("### G. Break-even batch size per k (default tables, set uniform_bits)\n")
    print("Total = setup + N x per-root. Break-even N is the smallest batch at which LK's "
          "total is below BK's; 1 means LNS wins even for a single root.\n")
    print("| k | BK setup | BK/root | LK setup | LK/root | break-even N |")
    print("|---|---:|---:|---:|---:|---:|")
    for k in KS:
        b, l_ = pick(k, "BK", "uniform_bits"), pick(k, "LK", "uniform_bits")
        sb, sl = int(b["instr_setup"]), int(l_["instr_setup"])
        pb, pl = ipr(b), ipr(l_)
        # setup rows include the step-count pointer setup (1 la = 2 words) in both: cancels
        be = "1" if sl + pl <= sb + pb else (f"{(sl - sb) / (pb - pl):.1f}" if pb > pl else "never")
        print(f"| {k} | {sb} | {pb:.1f} | {sl} | {pl:.1f} | {be} |")


# ---------------------------------------------------------------------------------------------
# Converter sizing and exponent precision (RESULTS.md section I)
# ---------------------------------------------------------------------------------------------
def extra_summary():
    grid = list(csv.DictReader(open(GRID_CSV)))
    res = list(csv.DictReader(open(os.path.join(RESD, "results.csv"))))
    print("#### I.1 Smallest converter tables within 1% / 5% of the best cost (C++ model, LK)\n")
    print("Over all 144 log x exp pairs (6..16 and 23 / 22 bits). Bytes = log table + exp "
          "table.\n")
    print("| k | input set | best instr/root | at | smallest within 1% | smallest within 5% "
          "| default 11/11 (32 KiB) |")
    print("|---|---|---:|---|---|---|---:|")
    for k in MODEL_KS:
        for st in ("uniform_u64", "width32", "width64"):
            rows = [r for r in grid if r["k"] == str(k) and r["variant"] == "LK" and
                    r["input_set"] == st]
            best = min(rows, key=lambda r: float(r["instr_per_root_model"]))
            b = float(best["instr_per_root_model"])

            def smallest(f):
                ok = [r for r in rows if float(r["instr_per_root_model"]) <= b * f]
                r = min(ok, key=lambda r: (int(r["table_bytes"]), float(r["instr_per_root_model"])))
                return f"{r['log_bits']}/{r['exp_bits']} ({int(r['table_bytes']) // 1024} KiB, " \
                       f"{float(r['instr_per_root_model']):.1f})"
            d = [r for r in rows if r["log_bits"] == "11" and r["exp_bits"] == "11"][0]
            print(f"| {k} | {st} | {b:.1f} | {best['log_bits']}/{best['exp_bits']} | "
                  f"{smallest(1.01)} | {smallest(1.05)} | {float(d['instr_per_root_model']):.1f} |")
    print()
    print("#### I.2 Which table matters: k = 3, uniform_u64, LK instructions per root "
          "(C++ model)\n")
    logs = ["6", "8", "10", "11", "13", "16", "23"]
    exps = ["6", "7", "8", "9", "10", "11", "13", "16", "22"]
    cell = {(r["log_bits"], r["exp_bits"]): float(r["instr_per_root_model"]) for r in grid
            if r["k"] == "3" and r["variant"] == "LK" and r["input_set"] == "uniform_u64"}
    print("| log \\ exp | " + " | ".join(exps) + " |")
    print("|---|" + "---:|" * len(exps))
    for lb in logs:
        print(f"| {lb} | " + " | ".join(f"{cell[(lb, e)]:.1f}" for e in exps) + " |")
    print()
    print("#### I.3 Simulator: converter bits 6..16 (k = 3, uniform_bits, log = exp bits)\n")
    print("| bits | table bytes | L1 instr/root | L1 max steps | L2 instr/root | L2 max steps |")
    print("|---:|---:|---:|---:|---:|---:|")
    by = {}
    for r in res:
        if r["input_set"] == "uniform_bits" and r["variant"] in ("L1", "L2") and \
                r["log_bits"] and (r["log_bits"] == r["exp_bits"] or r["log_bits"] == "23") \
                and not r["program"].startswith("batch/"):
            if r["log_bits"] == "23" and r["exp_bits"] != "22":
                continue
            by[(int(r["log_bits"]), r["variant"])] = r
    for b in sorted({b for b, _v in by}):
        l1, l2 = by[(b, "L1")], by[(b, "L2")]
        print(f"| {b if b != 23 else '23/22'} | {int(l1['table_bytes']):,} | "
              f"{float(l1['instr_per_root_uninstrumented']):.2f} | {l1['fixup_steps_max']} | "
              f"{float(l2['instr_per_root_uninstrumented']):.2f} | {l2['fixup_steps_max']} |")
    print()
    print("#### I.4 Exponent (\"power\") precision: LK (1/k built with ldiv) vs LKc (correctly "
          "rounded constant), C++ model, uniform_u64\n")
    print("| k | exp bits | LK max steps | LKc max steps | LK instr/root | LKc instr/root |")
    print("|---|---:|---:|---:|---:|---:|")
    for k in MODEL_KS:
        for e in ("6", "8", "11", "22"):
            lbits = "23" if e == "22" else "11"
            g = {r["variant"]: r for r in grid if r["k"] == str(k) and r["input_set"] ==
                 "uniform_u64" and r["log_bits"] == lbits and r["exp_bits"] == e}
            print(f"| {k} | {e} | {g['LK']['steps_max_abs']} | {g['LKc']['steps_max_abs']} | "
                  f"{float(g['LK']['instr_per_root_model']):.2f} | "
                  f"{float(g['LKc']['instr_per_root_model']):.2f} |")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mode", choices=("gen", "sim", "model", "summary", "extra"))
    ap.add_argument("--tables", default="6:6,8:8,10:10,11:11,13:13,23:22",
                    help="LOG:EXP list for LK/LDK simulator runs")
    ap.add_argument("--only", default="", help="sim: comma list of variants to re-run")
    ap.add_argument("--n", type=int, default=100000, help="inputs per set (model mode)")
    a = ap.parse_args()
    if a.mode == "gen":
        ps = all_kprogs()
        write_programs(ps)
        print(f"{len(ps)} programs -> {OUT_DIR}")
    elif a.mode == "sim":
        cfgs = [tuple(int(x) for x in c.split(":")) for c in a.tables.split(",")]
        sys.exit(sim_mode(cfgs, a.only.split(",") if a.only else None))
    elif a.mode == "model":
        sys.exit(model_mode(a.n))
    elif a.mode == "extra":
        extra_summary()
    else:
        summary()
