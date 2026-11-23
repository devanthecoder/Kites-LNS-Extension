#!/usr/bin/env python3
"""Generate the Lennard-Jones molecular-dynamics programs for the Kites LNS extension.

Writes, next to this script (or into --out):
  md_fp32.s        FP32 control: flw / fsw / fadd.s / fsub.s / fmul.s / fdiv.s
  md_lns.s         LNS with the table-based add/sub (ladd / lsub)
  md_lns_cordic.s  identical to md_lns.s except ladd -> laddc, lsub -> lsubc

System: 27 atoms on the 3x3x3 lattice of examples/lns_lj (spacing 1.12, same offsets), reduced
units (sigma = eps = mass = 1), free cluster (no boundaries), small random initial velocities
with zero total momentum, scaled to temperature T. All inputs are rounded to float32 and stored
in .data, so every variant (and the FP64 reference in tools/lns_lj_md_model.cpp) starts from the
same values.

Velocity Verlet, dt and the step count read from .data:
  F = force(x)                                   (with PE; log sample 0)
  repeat nsteps:  v += h*F;  x += dt*v;  F = force(x);  v += h*F      (h = dt/2)
                  every log_every steps: log sample (PE computed in that force call)
Pair force for i < j, d = x_i - x_j:
  r2 = (dx*dx + dy*dy) + dz*dz
  FP32: inv2 = 1/r2;  s6 = (inv2*inv2)*inv2      LNS: inv2 = r2^-1;  s6 = r2^-3   (lpown)
  s12 = s6*s6;  f = (48*s12 - 24*s6) * inv2;  F_i += f*d;  F_j -= f*d
  on log steps:  PE += 4*(s12 - s6)
KE = 0.5 * sum(v*v) over the 81 velocity components, E = KE + PE.

Log record (one per sample, 84 words): KE, PE, E, then the 81 position words, stored in the
variant's own format (FP32 bits or raw LNS words). The LNS programs convert pos/vel to LNS in
place at start-up. The final E is also left in f10 (FP32) or l10 (LNS).

Plain Python, float64 only (no numpy).
"""

import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True  # no __pycache__ in examples/lns_lj
sys.path.insert(0, os.path.join(HERE, "..", "lns_lj"))
from gen_lj import f32, f32_literal, positions  # noqa: E402  (same lattice as the benchmark)

N_SIDE = 3
LOG_WORDS = 84  # KE, PE, E + 81 position words


def velocities(n, temperature):
    """Deterministic velocities: uniform in [-1, 1] (LCG), zero total momentum, KE = 0.5*(3n-3)*T."""
    state = 987654321
    vel = []
    for _ in range(n):
        atom = []
        for _ in range(3):
            state = (1103515245 * state + 12345) % (1 << 31)
            atom.append(((state >> 8) % 2001 - 1000) / 1000.0)
        vel.append(atom)
    for k in range(3):
        mean = sum(v[k] for v in vel) / n
        for v in vel:
            v[k] -= mean
    ke = 0.5 * sum(c * c for v in vel for c in v)
    scale = math.sqrt(0.5 * (3 * n - 3) * temperature / ke)
    return [tuple(f32(c * scale) for c in v) for v in vel]


def float_block(label, values):
    lits = [f32_literal(c) for atom in values for c in atom]
    lines = ["%s .float %s" % ((label + ":").ljust(8), ", ".join(lits[:3]))]
    for k in range(3, len(lits), 3):
        lines.append("         .float " + ", ".join(lits[k:k + 3]))
    return lines


def data_section(pos, vel, dt, steps, log_every):
    samples = steps // log_every + 1
    lines = [".data",
             "dt:      .float %s" % f32_literal(f32(dt)),
             "nsteps:  .word %d" % steps,
             "logint:  .word %d" % log_every]
    lines += float_block("pos", pos)
    lines += float_block("vel", vel)
    lines += ["frc:     .zero %d" % (12 * len(pos)),
              "log:     .zero %d                # %d samples x %d words" % (
                  samples * LOG_WORDS * 4, samples, LOG_WORDS)]
    return lines


EXIT = [
    "    li a0, 0",
    "    li a7, 10                  # exit",
    "    ecall",
]

# x registers used by both variants
#   s0 &pos  s1 &vel  s2 &frc  s3 log cursor  s4 N  s5 PE flag  s6 steps left  s7 log countdown
#   s8 3N  s9 log interval  s10 -3  s11 -1  (lpown exponents, LNS only)


def common_main(n, lines_setup, final):
    """Main program shared by both variants (n atoms); final = per-format closing lines."""
    return [
        ".text",
        "    la s0, pos",
        "    la s1, vel",
        "    la s2, frc",
        "    la s3, log",
        "    li s4, %d                  # N" % n,
        "    li s8, %d                  # 3N" % (3 * n),
    ] + lines_setup + [
        "    la t0, nsteps",
        "    lw s6, 0(t0)               # steps",
        "    la t0, logint",
        "    lw s9, 0(t0)               # log interval",
        "    mv s7, s9",
        "    li s5, 1                   # PE needed for sample 0",
        "    jal ra, force",
        "    jal ra, logsample",
        "    beqz s6, done",
        "step_loop:",
        "    jal ra, kick               # v += h*F",
        "    jal ra, drift              # x += dt*v",
        "    addi s7, s7, -1",
        "    seqz s5, s7                # log step?",
        "    jal ra, force",
        "    jal ra, kick",
        "    beqz s5, no_log",
        "    jal ra, logsample",
        "    mv s7, s9",
        "no_log:",
        "    addi s6, s6, -1",
        "    bnez s6, step_loop",
        "done:",
    ] + final + EXIT


def fp32_program(pos, vel, dt, steps, log_every):
    lines = ["# Lennard-Jones molecular dynamics, FP32 control (generated by gen_md.py)",
             "# N=27, dt=%r, steps=%d, log every %d steps. Log at 'log': KE, PE, E, 81 positions."
             % (dt, steps, log_every),
             "# FP registers: f20 0.5, f21 1.0, f22 24, f23 48, f24 4, f25 dt, f26 dt/2,",
             "#   f0-f2 xi, f12-f14 Fi, f15 PE, others temporaries. Final E in f10."]
    lines += data_section(pos, vel, dt, steps, log_every)
    setup = [
        "    li t0, 0x3F000000",
        "    fmv.w.x f20, t0            # 0.5",
        "    li t0, 0x3F800000",
        "    fmv.w.x f21, t0            # 1.0",
        "    li t0, 0x41C00000",
        "    fmv.w.x f22, t0            # 24.0",
        "    li t0, 0x42400000",
        "    fmv.w.x f23, t0            # 48.0",
        "    li t0, 0x40800000",
        "    fmv.w.x f24, t0            # 4.0",
        "    fmv.w.x f31, zero          # 0.0",
        "    la t0, dt",
        "    flw f25, 0(t0)             # dt",
        "    fmul.s f26, f20, f25       # h = 0.5*dt",
    ]
    final = ["    fsgnj.s f10, f16, f16      # final E"]
    body = common_main(len(pos), setup, final)
    body += [
        "",
        "# ---- force: F = sum of pair forces; PE accumulated when s5 != 0 ----",
        "force:",
        "    mv t0, s2",
        "    mv t1, s8",
        "fz_loop:",
        "    fsw f31, 0(t0)             # F = 0",
        "    addi t0, t0, 4",
        "    addi t1, t1, -1",
        "    bnez t1, fz_loop",
        "    fsgnj.s f15, f31, f31      # PE = 0",
        "    li t1, 0                   # i",
        "    mv t3, s0                  # &pos[i]",
        "    mv a2, s2                  # &frc[i]",
        "fi_loop:",
        "    flw f0, 0(t3)",
        "    flw f1, 4(t3)",
        "    flw f2, 8(t3)",
        "    fsgnj.s f12, f31, f31      # Fi = 0",
        "    fsgnj.s f13, f31, f31",
        "    fsgnj.s f14, f31, f31",
        "    addi t2, t1, 1             # j = i + 1",
        "    addi t4, t3, 12            # &pos[j]",
        "    addi a3, a2, 12            # &frc[j]",
        "    bge t2, s4, fi_store",
        "fj_loop:",
        "    flw f3, 0(t4)",
        "    flw f4, 4(t4)",
        "    flw f5, 8(t4)",
        "    fsub.s f3, f0, f3          # dx",
        "    fsub.s f4, f1, f4          # dy",
        "    fsub.s f5, f2, f5          # dz",
        "    fmul.s f6, f3, f3",
        "    fmul.s f7, f4, f4",
        "    fadd.s f6, f6, f7",
        "    fmul.s f7, f5, f5",
        "    fadd.s f6, f6, f7          # r2",
        "    fdiv.s f7, f21, f6         # inv2 = 1/r2",
        "    fmul.s f8, f7, f7",
        "    fmul.s f8, f8, f7          # s6",
        "    fmul.s f9, f8, f8          # s12",
        "    fmul.s f10, f23, f9        # 48*s12",
        "    fmul.s f11, f22, f8        # 24*s6",
        "    fsub.s f10, f10, f11",
        "    fmul.s f10, f10, f7        # f = (48*s12 - 24*s6)*inv2",
        "    fmul.s f3, f10, f3         # fx",
        "    fmul.s f4, f10, f4         # fy",
        "    fmul.s f5, f10, f5         # fz",
        "    fadd.s f12, f12, f3        # Fi += f*d",
        "    fadd.s f13, f13, f4",
        "    fadd.s f14, f14, f5",
        "    flw f6, 0(a3)              # Fj -= f*d",
        "    fsub.s f6, f6, f3",
        "    fsw f6, 0(a3)",
        "    flw f6, 4(a3)",
        "    fsub.s f6, f6, f4",
        "    fsw f6, 4(a3)",
        "    flw f6, 8(a3)",
        "    fsub.s f6, f6, f5",
        "    fsw f6, 8(a3)",
        "    beqz s5, fj_next",
        "    fsub.s f9, f9, f8          # s12 - s6",
        "    fmul.s f9, f24, f9         # 4*(s12 - s6)",
        "    fadd.s f15, f15, f9        # PE +=",
        "fj_next:",
        "    addi t2, t2, 1",
        "    addi t4, t4, 12",
        "    addi a3, a3, 12",
        "    blt t2, s4, fj_loop",
        "fi_store:",
        "    flw f6, 0(a2)              # F[i] += Fi",
        "    fadd.s f6, f6, f12",
        "    fsw f6, 0(a2)",
        "    flw f6, 4(a2)",
        "    fadd.s f6, f6, f13",
        "    fsw f6, 4(a2)",
        "    flw f6, 8(a2)",
        "    fadd.s f6, f6, f14",
        "    fsw f6, 8(a2)",
        "    addi t1, t1, 1",
        "    addi t3, t3, 12",
        "    addi a2, a2, 12",
        "    blt t1, s4, fi_loop",
        "    ret",
        "",
        "# ---- kick: v += h*F (all 3N components) ----",
        "kick:",
        "    mv t0, s1",
        "    mv t1, s2",
        "    mv t2, s8",
        "kick_loop:",
        "    flw f0, 0(t0)",
        "    flw f1, 0(t1)",
        "    fmul.s f1, f26, f1",
        "    fadd.s f0, f0, f1",
        "    fsw f0, 0(t0)",
        "    addi t0, t0, 4",
        "    addi t1, t1, 4",
        "    addi t2, t2, -1",
        "    bnez t2, kick_loop",
        "    ret",
        "",
        "# ---- drift: x += dt*v ----",
        "drift:",
        "    mv t0, s0",
        "    mv t1, s1",
        "    mv t2, s8",
        "drift_loop:",
        "    flw f0, 0(t0)",
        "    flw f1, 0(t1)",
        "    fmul.s f1, f25, f1",
        "    fadd.s f0, f0, f1",
        "    fsw f0, 0(t0)",
        "    addi t0, t0, 4",
        "    addi t1, t1, 4",
        "    addi t2, t2, -1",
        "    bnez t2, drift_loop",
        "    ret",
        "",
        "# ---- logsample: KE = 0.5*sum(v*v); store KE, PE, E and the positions ----",
        "logsample:",
        "    fsgnj.s f0, f31, f31       # sum = 0",
        "    mv t0, s1",
        "    mv t2, s8",
        "ke_loop:",
        "    flw f1, 0(t0)",
        "    fmul.s f1, f1, f1",
        "    fadd.s f0, f0, f1",
        "    addi t0, t0, 4",
        "    addi t2, t2, -1",
        "    bnez t2, ke_loop",
        "    fmul.s f0, f20, f0         # KE",
        "    fadd.s f16, f0, f15        # E = KE + PE",
        "    fsw f0, 0(s3)",
        "    fsw f15, 4(s3)",
        "    fsw f16, 8(s3)",
    ] + copy_positions() + ["    ret"]
    return lines + body


def copy_positions():
    """Raw word copy of the 3N positions into the log record (format independent)."""
    return [
        "    mv t0, s0",
        "    addi t1, s3, 12",
        "    mv t2, s8",
        "copy_loop:",
        "    lw t3, 0(t0)",
        "    sw t3, 0(t1)",
        "    addi t0, t0, 4",
        "    addi t1, t1, 4",
        "    addi t2, t2, -1",
        "    bnez t2, copy_loop",
        "    addi s3, s3, %d             # next record" % (LOG_WORDS * 4),
    ]


def lns_program(pos, vel, dt, steps, log_every, add, sub, label):
    a, s = add.ljust(5), sub.ljust(5)
    lines = ["# Lennard-Jones molecular dynamics, LNS %s (generated by gen_md.py)" % label,
             "# N=27, dt=%r, steps=%d, log every %d steps. Log at 'log': KE, PE, E, 81 positions"
             % (dt, steps, log_every),
             "#   as raw LNS words. pos/vel are converted to LNS in place at start-up.",
             "# L registers: ls0 dt/2, ls1 dt, ls2 24, ls3 48, ls4 4, ls5 0.5, ls6-ls8 xi,",
             "#   ls9-ls11 Fi, la1 PE, la0 E (final), lt0-lt10 temporaries.",
             "# x registers s10 = -3, s11 = -1 (lpown exponents)."]
    lines += data_section(pos, vel, dt, steps, log_every)
    setup = [
        "# convert pos and vel (2 x 3N words, contiguous) to LNS in place",
        "    mv t0, s0",
        "    li t1, %d" % (2 * 81),
        "cvt_loop:",
        "    flw f0, 0(t0)",
        "    fcvt.lns.s lt0, f0",
        "    lsw lt0, 0(t0)",
        "    addi t0, t0, 4",
        "    addi t1, t1, -1",
        "    bnez t1, cvt_loop",
        "    li s10, -3",
        "    li s11, -1",
        "    li t0, 2",
        "    fcvt.lns.w lt0, t0         # 2",
        "    lpown ls5, lt0, s11        # 0.5 = 2^-1 (exact)",
        "    li t0, 24",
        "    fcvt.lns.w ls2, t0         # 24",
        "    li t0, 48",
        "    fcvt.lns.w ls3, t0         # 48",
        "    li t0, 4",
        "    fcvt.lns.w ls4, t0         # 4",
        "    la t0, dt",
        "    flw f0, 0(t0)",
        "    fcvt.lns.s ls1, f0         # dt",
        "    lmul  ls0, ls5, ls1        # h = 0.5*dt (exact)",
    ]
    final = ["    fcvt.s.lns f10, la0        # final E, also as FP32 in f10"]
    body = common_main(len(pos), setup, final)
    body += [
        "",
        "# ---- force: F = sum of pair forces; PE accumulated when s5 != 0 ----",
        "force:",
        "    mv t0, s2",
        "    mv t1, s8",
        "fz_loop:",
        "    lsw lzero, 0(t0)           # F = 0",
        "    addi t0, t0, 4",
        "    addi t1, t1, -1",
        "    bnez t1, fz_loop",
        "    %s la1, lzero, lzero     # PE = 0" % a,
        "    li t1, 0                   # i",
        "    mv t3, s0                  # &pos[i]",
        "    mv a2, s2                  # &frc[i]",
        "fi_loop:",
        "    llw ls6, 0(t3)",
        "    llw ls7, 4(t3)",
        "    llw ls8, 8(t3)",
        "    %s ls9, lzero, lzero     # Fi = 0" % a,
        "    %s ls10, lzero, lzero" % a,
        "    %s ls11, lzero, lzero" % a,
        "    addi t2, t1, 1             # j = i + 1",
        "    addi t4, t3, 12            # &pos[j]",
        "    addi a3, a2, 12            # &frc[j]",
        "    bge t2, s4, fi_store",
        "fj_loop:",
        "    llw lt0, 0(t4)",
        "    llw lt1, 4(t4)",
        "    llw lt2, 8(t4)",
        "    %s lt0, ls6, lt0         # dx" % s,
        "    %s lt1, ls7, lt1         # dy" % s,
        "    %s lt2, ls8, lt2         # dz" % s,
        "    lmul  lt3, lt0, lt0",
        "    lmul  lt4, lt1, lt1",
        "    %s lt3, lt3, lt4" % a,
        "    lmul  lt4, lt2, lt2",
        "    %s lt3, lt3, lt4         # r2" % a,
        "    lpown lt4, lt3, s11        # inv2 = r2^-1",
        "    lpown lt5, lt3, s10        # s6 = r2^-3",
        "    lmul  lt6, lt5, lt5        # s12",
        "    lmul  lt7, ls3, lt6        # 48*s12",
        "    lmul  lt8, ls2, lt5        # 24*s6",
        "    %s lt7, lt7, lt8" % s,
        "    lmul  lt7, lt7, lt4        # f = (48*s12 - 24*s6)*inv2",
        "    lmul  lt0, lt7, lt0        # fx",
        "    lmul  lt1, lt7, lt1        # fy",
        "    lmul  lt2, lt7, lt2        # fz",
        "    %s ls9, ls9, lt0         # Fi += f*d" % a,
        "    %s ls10, ls10, lt1" % a,
        "    %s ls11, ls11, lt2" % a,
        "    llw lt3, 0(a3)             # Fj -= f*d",
        "    %s lt3, lt3, lt0" % s,
        "    lsw lt3, 0(a3)",
        "    llw lt3, 4(a3)",
        "    %s lt3, lt3, lt1" % s,
        "    lsw lt3, 4(a3)",
        "    llw lt3, 8(a3)",
        "    %s lt3, lt3, lt2" % s,
        "    lsw lt3, 8(a3)",
        "    beqz s5, fj_next",
        "    %s lt6, lt6, lt5         # s12 - s6" % s,
        "    lmul  lt6, ls4, lt6        # 4*(s12 - s6)",
        "    %s la1, la1, lt6         # PE +=" % a,
        "fj_next:",
        "    addi t2, t2, 1",
        "    addi t4, t4, 12",
        "    addi a3, a3, 12",
        "    blt t2, s4, fj_loop",
        "fi_store:",
        "    llw lt3, 0(a2)             # F[i] += Fi",
        "    %s lt3, lt3, ls9" % a,
        "    lsw lt3, 0(a2)",
        "    llw lt3, 4(a2)",
        "    %s lt3, lt3, ls10" % a,
        "    lsw lt3, 4(a2)",
        "    llw lt3, 8(a2)",
        "    %s lt3, lt3, ls11" % a,
        "    lsw lt3, 8(a2)",
        "    addi t1, t1, 1",
        "    addi t3, t3, 12",
        "    addi a2, a2, 12",
        "    blt t1, s4, fi_loop",
        "    ret",
        "",
        "# ---- kick: v += h*F (all 3N components) ----",
        "kick:",
        "    mv t0, s1",
        "    mv t1, s2",
        "    mv t2, s8",
        "kick_loop:",
        "    llw lt0, 0(t0)",
        "    llw lt1, 0(t1)",
        "    lmul  lt1, ls0, lt1",
        "    %s lt0, lt0, lt1" % a,
        "    lsw lt0, 0(t0)",
        "    addi t0, t0, 4",
        "    addi t1, t1, 4",
        "    addi t2, t2, -1",
        "    bnez t2, kick_loop",
        "    ret",
        "",
        "# ---- drift: x += dt*v ----",
        "drift:",
        "    mv t0, s0",
        "    mv t1, s1",
        "    mv t2, s8",
        "drift_loop:",
        "    llw lt0, 0(t0)",
        "    llw lt1, 0(t1)",
        "    lmul  lt1, ls1, lt1",
        "    %s lt0, lt0, lt1" % a,
        "    lsw lt0, 0(t0)",
        "    addi t0, t0, 4",
        "    addi t1, t1, 4",
        "    addi t2, t2, -1",
        "    bnez t2, drift_loop",
        "    ret",
        "",
        "# ---- logsample: KE = 0.5*sum(v*v); store KE, PE, E and the positions ----",
        "logsample:",
        "    %s lt2, lzero, lzero     # sum = 0" % a,
        "    mv t0, s1",
        "    mv t2, s8",
        "ke_loop:",
        "    llw lt1, 0(t0)",
        "    lmul  lt1, lt1, lt1",
        "    %s lt2, lt2, lt1" % a,
        "    addi t0, t0, 4",
        "    addi t2, t2, -1",
        "    bnez t2, ke_loop",
        "    lmul  lt2, ls5, lt2        # KE",
        "    %s la0, lt2, la1         # E = KE + PE" % a,
        "    lsw lt2, 0(s3)",
        "    lsw la1, 4(s3)",
        "    lsw la0, 8(s3)",
    ] + copy_positions() + ["    ret"]
    return lines + body


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dt", type=float, default=0.005, help="time step (default 0.005)")
    ap.add_argument("--steps", type=int, default=200, help="number of steps (default 200)")
    ap.add_argument("--log-every", type=int, default=5, help="log interval in steps (default 5)")
    ap.add_argument("--temperature", type=float, default=0.1,
                    help="initial temperature in reduced units (default 0.1)")
    ap.add_argument("--out", default=HERE, help="output directory (default: next to this script)")
    args = ap.parse_args()
    if args.steps < 0 or args.log_every < 1 or args.steps % args.log_every:
        ap.error("--steps must be a non-negative multiple of --log-every")
    if not args.dt > 0:
        ap.error("--dt must be positive")

    pos = positions(N_SIDE)
    vel = velocities(len(pos), args.temperature)
    os.makedirs(args.out, exist_ok=True)

    def write(name, lines):
        with open(os.path.join(args.out, name), "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")

    common = (pos, vel, args.dt, args.steps, args.log_every)
    write("md_fp32.s", fp32_program(*common))
    write("md_lns.s", lns_program(*common, "ladd", "lsub", "table add/sub"))
    write("md_lns_cordic.s", lns_program(*common, "laddc", "lsubc", "CORDIC add/sub"))
    print("N=%d dt=%r steps=%d log_every=%d T=%r -> %s"
          % (len(pos), args.dt, args.steps, args.log_every, args.temperature, args.out))


if __name__ == "__main__":
    main()
