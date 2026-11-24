#!/usr/bin/env python3
"""Latency-weighted cost MODEL for the cube-root benchmark. NOT a measurement.

KITES rvss retires one instruction per cycle, so the measured metric of this benchmark is the
instruction count. Real hardware does not charge every instruction one cycle: divides are slow,
multiplies take a few cycles, and an LNS convert or lpow would have some latency of its own that
nobody has built yet. This script re-weights the measured dynamic instruction mix (mix.json,
written by examples/lns_int_cbrt/scripts/check.py from the static model it validates against
instructions_retired) with a per-class latency table and prints "cycles per root" for each
variant, sweeping the classes whose latency is least known.

Every latency here is an ASSUMPTION. The defaults (tools/cbrt_cost_latencies.json) use:
  * integer mul 3 cycles, integer div 6..68 cycles (operand-dependent): SiFive U74 Core Complex
    Manual, Table 6 "U7 Instruction Latency" (MUL/MULH/MULHU/MULHSU three-cycle latency;
    DIV/DIVU/REM/REMU "between six-cycle to 68-cycle latency, depending on operand values").
    https://www.scs.stanford.edu/~zyedidia/docs/sifive/sifive-u74.pdf
  * everything else (ALU 1, branch 1, load/store 1 = cache hit, FP latencies, and all LNS
    latencies) is a round-number guess, not taken from any datasheet. The LNS converter and lpow
    latencies are swept because the project's hardware cost model is still pending.
It is a serial latency sum: no pipelining, no overlap, no branch-miss penalty. Treat the output
as a sensitivity analysis ("for which latencies does the conclusion flip?"), never as a speed.

Usage: python3 tools/cbrt_cost_model.py [--mix examples/lns_int_cbrt/results/mix.json]
                                        [--latencies tools/cbrt_cost_latencies.json]
"""

import argparse
import itertools
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def cost(per_root, lat):
    return sum(count * lat[cls] for cls, count in per_root.items())


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mix", default=os.path.join(REPO, "examples", "lns_int_cbrt", "results", "mix.json"))
    ap.add_argument("--latencies", default=os.path.join(HERE, "cbrt_cost_latencies.json"))
    args = ap.parse_args()
    mix = json.load(open(args.mix))["variants"]
    cfg = json.load(open(args.latencies))
    base = cfg["latencies"]
    sweep = cfg["sweep"]
    variants = [v for v in ("B0", "B1", "L0", "L1", "L2", "F0") if v in mix]

    missing = {c for v in variants for c in mix[v]["per_root"]} - set(base)
    if missing:
        raise SystemExit(f"no latency for classes: {sorted(missing)}")

    print("MODEL, not a measurement: serial latency sum per root, latencies are assumptions.")
    print("Per-root dynamic mix (instructions/root, uniform_bits N=1024, uninstrumented):")
    classes = sorted({c for v in variants for c in mix[v]["per_root"]})
    print(f"  {'class':12s} " + " ".join(f"{v:>8s}" for v in variants))
    for c in classes:
        print(f"  {c:12s} " + " ".join(f"{mix[v]['per_root'].get(c, 0):8.3f}" for v in variants))
    print(f"  {'TOTAL':12s} " + " ".join(f"{sum(mix[v]['per_root'].values()):8.3f}"
                                         for v in variants))
    print()
    keys = list(sweep)
    print(" ".join(f"{k:>11s}" for k in keys) + " | " +
          " ".join(f"{v:>7s}" for v in variants) + " | L1/B1  B1-vs-L1")
    for combo in itertools.product(*(sweep[k] for k in keys)):
        lat = dict(base)
        lat.update(dict(zip(keys, combo)))
        c = {v: cost(mix[v]["per_root"], lat) for v in variants}
        ratio = c["L1"] / c["B1"]
        verdict = "L1 cheaper" if ratio < 1 else "B1 cheaper"
        print(" ".join(f"{x:>11}" for x in combo) + " | " +
              " ".join(f"{c[v]:7.1f}" for v in variants) + f" | {ratio:5.3f}  {verdict}")
    # Break-even: how slow would one lpow have to be (all else default) for B1 to win?
    lat = dict(base)
    other = {k: v for k, v in mix["L1"]["per_root"].items() if k != "lns_pow"}
    n_pow = mix["L1"]["per_root"].get("lns_pow", 0)
    for div in sweep.get("div", [base["div"]]):
        lat["div"] = div
        b1 = cost(mix["B1"]["per_root"], lat)
        rest = cost(other, lat)
        print(f"div={div:>3}: B1 = {b1:6.1f}; L1 stays cheaper while one lpow costs < "
              f"{(b1 - rest) / n_pow:6.1f} cycles (other L1 classes at their default latency)")


if __name__ == "__main__":
    main()
