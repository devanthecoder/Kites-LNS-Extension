#!/usr/bin/env python3
"""Generate RV64 assembly and exact references for the TCP CUBIC cube-root benchmark.

The integer baseline follows Linux v6.12 net/ipv4/tcp_cubic.c cubic_root().
Source: https://github.com/torvalds/linux/blob/v6.12/net/ipv4/tcp_cubic.c
"""

from __future__ import annotations

import argparse
import json
import os

KERNEL_TAG = "v6.12"
KERNEL_URL = "https://github.com/torvalds/linux/blob/v6.12/net/ipv4/tcp_cubic.c"
V = [0, 54, 54, 54, 118, 118, 118, 118, 123, 129, 134, 138, 143, 147, 151, 156,
     157, 161, 164, 168, 170, 173, 176, 179, 181, 185, 187, 190, 192, 194, 197, 199,
     200, 202, 204, 206, 209, 211, 213, 215, 217, 219, 221, 222, 224, 225, 227, 229,
     231, 232, 234, 236, 237, 239, 240, 242, 244, 245, 246, 248, 250, 251, 252, 254]
SETS = ("small", "near2^22", "cubes", "u32max", "u64max", "uniform_bits")
VARIANTS = ("cubic", "cubic_exact", "lns", "lns_exact", "lns_logdiv")


def kernel_cubic_root(a: int) -> int:
    """Unsigned 64-bit port of Linux v6.12 cubic_root(), including its approximation."""
    b = a.bit_length()
    if b < 7:
        return (V[a] + 35) >> 6
    b = ((b * 84) >> 8) - 1
    shift = a >> (b * 3)
    x = ((V[shift] + 10) << b) >> 6
    x = (2 * x + a // (x * (x - 1))) & 0xFFFFFFFF
    return ((x * 341) >> 10) & 0xFFFFFFFF


def exact_cbrt(a: int) -> int:
    """floor(cuberoot(a)), using integer Newton iteration and integer fix-up only."""
    if a < 2:
        return a
    x = 1 << ((a.bit_length() + 2) // 3)
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


def lcg(state: int) -> tuple[int, int]:
    state = (6364136223846793005 * state + 1442695040888963407) & ((1 << 64) - 1)
    return state, state


def input_sets() -> dict[str, list[int]]:
    near = {1 << 21, (1 << 21) - 1, (1 << 24), (1 << 24) - 1}
    near.update((1 << 22) + d for d in range(-64, 65))
    near.update((1 << 23) + d for d in range(-64, 65))
    state = 0xC0B1C
    for _ in range(256):
        state, value = lcg(state)
        near.add((1 << 21) + value % ((1 << 23) + 1))

    ns = {1, 2, 7, 8, 63, 64, 65, 1024, 1 << 10, 1 << 15,
          1 << 20, (1 << 21), (1 << 22), 2642245}
    # Deterministic spread over the full cube-root domain, including both endpoints.
    ns.update(1 + (i * (2642245 - 1)) // 199 for i in range(200))
    cubes = sorted({n ** 3 + d for n in ns for d in (-1, 0, 1) if 0 <= n ** 3 + d < (1 << 64)})

    state = 0x5EEDC0DE
    uniform = set()
    for i in range(256):
        state, r = lcg(state)
        bits = 1 + (i % 64)
        mask = (1 << bits) - 1
        uniform.add((r & mask) | (1 << (bits - 1)))

    return {
        "small": list(range(1001)),
        "near2^22": sorted(near),
        "cubes": cubes,
        "u32max": sorted({(1 << 32) + d for d in range(-16, 17)} | {0xFFFFFFFF}),
        "u64max": sorted({(1 << 64) - 1 - d for d in range(32)} |
                         {(1 << 63) + d for d in (-1, 0, 1)}),
        "uniform_bits": sorted(uniform),
    }


EXIT = ["    li a0, 0", "    li a7, 10", "    ecall"]


def kernel_body() -> list[str]:
    return [
        "    mv t1, s4", "    li t0, 0", "    beqz t1, fls_done", "fls_loop:",
        "    addi t0, t0, 1", "    srli t1, t1, 1", "    bnez t1, fls_loop", "fls_done:",
        "    sltiu t1, t0, 7", "    bnez t1, cubic_direct",
        "    li t1, 84", "    mul t0, t0, t1", "    srli t0, t0, 8", "    addi t0, t0, -1",
        "    slli t1, t0, 1", "    add t1, t1, t0", "    srl t2, s4, t1",
        "    la t1, vtab", "    add t1, t1, t2", "    lbu t2, 0(t1)", "    addi t2, t2, 10",
        "    sll t2, t2, t0", "    srli t2, t2, 6", "    mv s5, t2", "    j cubic_newton",
        "cubic_direct:", "    la t1, vtab", "    add t1, t1, t0", "    lbu t2, 0(t1)",
        "    addi t2, t2, 35", "    srli s5, t2, 6", "    j cubic_done",
        "cubic_newton:", "    addi t1, s5, -1", "    mul t2, s5, t1", "    divu t3, s4, t2",
        "    slli t4, s5, 1", "    add t4, t4, t3", "    li t5, 341", "    mul t4, t4, t5",
        "    srli s5, t4, 10", "cubic_done:",
    ]


def fixup_body() -> list[str]:
    return [
        "    mv s7, s5                  # original approximate candidate",
        "fix_down_check:", "    beqz s5, fix_up_check", "    divu t0, s4, s5", "    mul t1, s5, s5",
        "    bltu t0, t1, fix_down", "    j fix_up_check", "fix_down:", "    addi s5, s5, -1",
        "    addi s6, s6, -1", "    j fix_down_check", "fix_up_check:", "    addi t2, s5, 1",
        "    divu t0, s4, t2", "    mul t1, t2, t2", "    bltu t0, t1, fix_done",
        "    mv s5, t2", "    addi s6, s6, 1", "    j fix_up_check", "fix_done:",
    ]


def program(variant: str, name: str, values: list[int]) -> list[str]:
    n = len(values)
    out_bytes = n * 8
    lines = [
        f"# TCP CUBIC cube-root benchmark; variant={variant}; set={name}; N={n}",
        f"# Kernel: Linux {KERNEL_TAG} cubic_root() ({KERNEL_URL})",
        ".data", "input:",
    ]
    for start in range(0, n, 4):
        lines.append("    .dword " + ", ".join(str(x) for x in values[start:start + 4]))
    lines += [f"output: .zero {out_bytes}", f"steps: .zero {n * 4}"]
    if variant == "lns_logdiv":
        lines.append("scratch: .zero 4")
    if variant.startswith("cubic"):
        lines.append("vtab: .byte " + ", ".join(map(str, V)))
    lines += [".text", "main:", "    la s0, input", "    la s1, output", "    la s2, steps",
              f"    li s3, {n}"]
    if variant.startswith("lns"):
        lines += ["    li t0, 1", "    fcvt.lns.w l10, t0", "    li t0, 3",
                  "    fcvt.lns.w l11, t0", "    ldiv l12, l10, l11"]
    lines += ["root_loop:", "    ld s4, 0(s0)", "    li s6, 0"]
    if variant.startswith("cubic"):
        lines += kernel_body()
    elif variant in ("lns", "lns_exact"):
        lines += ["    fcvt.lns.lu l8, s4", "    lpow l9, l8, l12", "    fcvt.lu.lns s5, l9"]
    elif variant == "lns_logdiv":
        lines += ["    fcvt.lns.lu l8, s4", "    la t5, scratch", "    lsw l8, 0(t5)",
                  "    lw t0, 0(t5)", "    slli t0, t0, 1", "    srai t0, t0, 1", "    li t1, 3",
                  "    divw t0, t0, t1", "    slli t0, t0, 1", "    srli t0, t0, 1",
                  "    sw t0, 0(t5)", "    llw l9, 0(t5)", "    fcvt.lu.lns s5, l9"]
    if variant in ("cubic_exact", "lns_exact", "lns_logdiv"):
        lines += fixup_body()
    lines += ["    sd s5, 0(s1)", "    sw s6, 0(s2)", "    addi s0, s0, 8", "    addi s1, s1, 8",
              "    addi s2, s2, 4", "    addi s3, s3, -1", "    bnez s3, root_loop"]
    lines += EXIT
    return lines


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=here)
    ap.add_argument("--set", choices=("all",) + SETS, default="all")
    ap.add_argument("--limit", type=int, default=0, help="cap each set for quick simulator runs")
    args = ap.parse_args()
    if args.limit < 0:
        ap.error("--limit must be nonnegative")
    os.makedirs(args.out, exist_ok=True)
    selected = input_sets()
    if args.set != "all":
        selected = {args.set: selected[args.set]}
    if args.limit:
        selected = {k: v[:args.limit] for k, v in selected.items()}

    references = {}
    for name, values in selected.items():
        references[name] = [{"input": str(a), "exact": exact_cbrt(a), "kernel": kernel_cubic_root(a)}
                            for a in values]
        for variant in VARIANTS:
            path = os.path.join(args.out, f"cbrt_{variant}_{name.replace('^', '')}.s")
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(program(variant, name, values)) + "\n")
    with open(os.path.join(args.out, "reference.json"), "w", encoding="utf-8") as f:
        json.dump({"kernel_tag": KERNEL_TAG, "kernel_url": KERNEL_URL, "sets": references}, f,
                  indent=2)
        f.write("\n")
    counts = ", ".join(f"{k}={len(v)}" for k, v in selected.items())
    print(f"Generated {len(selected) * len(VARIANTS)} programs; {counts}; output={args.out}")


if __name__ == "__main__":
    main()
