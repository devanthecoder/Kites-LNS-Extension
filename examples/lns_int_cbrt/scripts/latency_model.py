#!/usr/bin/env python3
"""ESTIMATED time per root from the research paper's hardware numbers. NOT a measurement.

KITES rvss retires one instruction per cycle, so everything measured in this folder is an
instruction count. This script turns the measured per-root instruction mix of every variant
into an estimated latency, using the hardware numbers in

  B. Parhami, "Computing with logarithmic number system arithmetic: Implementation methods
  and performance benefits", Computers and Electrical Engineering 87 (2020) 106800,
  doi:10.1016/j.compeleceng.2020.106800

which reports (citing Coleman et al.):
  Table 1  gate-level latencies in ns of 32-bit fixed-point, FP and LNS units (0.7 um cells,
           5 ns ROM access assumed): add 4 / 28 / 28, subtract 4 / 28 / 28-42,
           multiply 32 / 22 / 4, divide - / - / 4; "division typically takes 2-3 times as long
           as multiplication".
  Table 3  measured latency in cycles of the ELM LNS processor (125 MHz) and the TI
           TMS320C6711 FP DSP (150 MHz), both 0.18 um: ELM add 3, subtract 3 (4), multiply 1,
           divide 1, square root 1, load 1, store 2; TMS add 4, subtract 4, multiply 4,
           divide ~30, square root ~40, load 4, store 4.

Every latency below carries a source tag: "paper T1", "paper T3", "U74" (SiFive U74 Core
Complex Manual, Table 6: integer mul 3 cycles, div 6-68 cycles) or "ASSUMED". The paper has NO
number for integer<->LNS conversion or for lpow (x^y), and those are exactly the operations the
LNS root kernel depends on, so both are swept over a low / mid / high range.

Model: serial sum of latencies of the dynamic instruction mix (no pipelining, no overlap, no
cache misses, no branch penalty). Two scenarios:
  A  cycles  - Table 3 latencies (ELM for LNS ops, TMS for FP ops), U74 for integer mul/div.
  B  ns      - Table 1 gate delays (fixed point for integer ops, FP column for FP ops, LNS
               column for LNS ops), Table 3 ns for memory.
Usage: python3 examples/lns_int_cbrt/scripts/latency_model.py   (needs mix.json from check.py, and
config_sweep.csv + the C++ model binary for the k = 2..6 variants; see RESULTS.md section I)
"""

from __future__ import annotations

import csv
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))          # scripts/
ROOT = os.path.dirname(HERE)                                  # examples/lns_int_cbrt/
PROG = os.path.join(ROOT, "programs")                         # .s programs, reference.json
RESD = os.path.join(ROOT, "results")                          # CSV / JSON results
sys.path.insert(0, HERE)

# (value, source) per instruction class. Ranges are (low, mid, high).
SCEN_A = {  # cycles
    "alu": (1, "ASSUMED: 1-cycle integer ALU (ELM/TMS single-cycle integer units)"),
    "branch": (1, "ASSUMED: no branch penalty"),
    "load": (1, "paper T3: ELM load 1 cycle (cache hit)"),
    "store": (1, "ASSUMED: integer store 1 cycle (ELM LNS store is 2, TMS 4)"),
    "mul": (3, "U74: integer mul 3 cycles (not in paper)"),
    "div": ((6, 20, 68), "U74: integer div 6-68 cycles, operand-dependent (not in paper)"),
    "lns_convert": ((1, 2, 4), "ASSUMED: the paper gives no converter latency"),
    "lns_pow": ((1, 4, 8), "ASSUMED: not in paper. 1 = like ELM square root (shift); "
                           "4 = exp-table lookup + 32x32 multiply (KITES lpow); 8 = slow"),
    "lns_alu": (1, "paper T3: ELM divide 1 cycle (ldiv, setup only)"),
    "lns_mem": (1.5, "paper T3: ELM load 1 / store 2 (every use here is one lsw + one llw)"),
    "fp_add": (4, "paper T3: TMS add 4 cycles"),
    "fp_mul": (4, "paper T3: TMS multiply 4 cycles"),
    "fp_div": (30, "paper T3: TMS divide ~30 cycles"),
    "fp_sqrt": (40, "paper T3: TMS square root ~40 cycles"),
    "fp_convert": (4, "ASSUMED: = TMS add pipeline (not in paper)"),
    "fp_move": (1, "ASSUMED: register move"),
    "system": (1, "ASSUMED"),
}
SCEN_B = {  # ns
    "alu": (4, "paper T1: fixed-point add 4 ns"),
    "branch": (4, "ASSUMED: = fixed-point add (compare)"),
    "load": (8, "paper T3: ELM load 8 ns"),
    "store": (8, "ASSUMED: = load (ELM LNS store is 16 ns)"),
    "mul": (32, "paper T1: fixed-point multiply 32 ns"),
    "div": ((64, 80, 96), "paper T1 text: division 2-3x multiplication"),
    "lns_convert": ((9, 17, 32), "ASSUMED: ROM 5 + add 4 / ROM + 3 adds / = a multiply"),
    "lns_pow": ((4, 37, 64), "ASSUMED: 4 = LNS multiply (T1); 37 = ROM 5 + 32x32 multiply 32; "
                             "64 = 2 multiplies"),
    "lns_alu": (4, "paper T1: LNS divide 4 ns"),
    "lns_mem": (12, "paper T3: ELM load 8 / store 16 ns, mean"),
    "fp_add": (28, "paper T1: FP add 28 ns"),
    "fp_mul": (22, "paper T1: FP multiply 22 ns"),
    "fp_div": ((44, 55, 66), "paper T1 text: division 2-3x multiplication"),
    "fp_sqrt": ((59, 73, 88), "ASSUMED: 4/3 of FP divide (TMS sqrt 40 vs divide 30 cycles, T3)"),
    "fp_convert": (28, "ASSUMED: = FP add"),
    "fp_move": (4, "ASSUMED: register move"),
    "system": (4, "ASSUMED"),
}


def pick(entry, level):
    v = entry[0]
    if isinstance(v, tuple):
        return v[level]
    return v


def cost(mix, scen, level, override=None):
    total = 0.0
    for cls, n in mix.items():
        if cls not in scen:
            raise KeyError(f"no latency for class {cls}")
        lat = pick(scen[cls], level)
        if override and cls in override:
            lat = override[cls]
        total += n * lat
    return total


def k3_mixes():
    d = json.load(open(os.path.join(RESD, "mix.json")))["variants"]
    return {v: d[v]["per_root"] for v in ("B0", "B1", "L0", "L1", "L2", "F0")}


def kroot_mixes(set_name="uniform_u64", tables=("11", "11")):
    """Per-root class mix (uninstrumented) of every k-root variant, from the static model with
    the C++ model's fix-up candidates (static model == instructions_retired, checked)."""
    import sweep_configs as s
    out = {}
    for p in s.all_kprogs():
        if p.set_name != set_name:
            continue
        path = os.path.join(PROG, p.rel)
        if not os.path.exists(path):
            s.write_programs([p])
        cands = None
        if p.variant != "BK":
            cands = [m[1] for m in s.model_keval(p, path)]
        q = s.KProg(p.variant, p.k, p.set_name, p.values, count=False)
        prof = q.static_profile(cands)
        mix = {c: (prof["classes"].get(c, 0) - prof["setup_classes"].get(c, 0)) / q.n
               for c in prof["classes"]}
        out[(p.k, p.variant)] = {c: v for c, v in mix.items() if v}
    return out


def main():
    rows = []
    levels = ("low", "mid", "high")
    print("ESTIMATE, not a measurement: serial latency sum per root from the paper's Tables 1/3 "
          "plus labelled assumptions.\n")
    for scen_name, scen, unit in (("A", SCEN_A, "cycles"), ("B", SCEN_B, "ns")):
        print(f"Scenario {scen_name} ({unit}) sources:")
        for cls, (v, src) in scen.items():
            print(f"  {cls:12s} {str(v):16s} {src}")
        print()
    k3 = k3_mixes()
    print("k = 3, uniform_bits (TCP CUBIC benchmark variants):")
    print(f"{'variant':8s} {'instr':>7s} | " +
          " ".join(f"A-{lv:>4s}" for lv in levels) + " | " +
          " ".join(f"B-{lv:>4s}" for lv in levels))
    for v, mix in k3.items():
        ca = [cost(mix, SCEN_A, i) for i in range(3)]
        cb = [cost(mix, SCEN_B, i) for i in range(3)]
        print(f"{v:8s} {sum(mix.values()):7.2f} | " + " ".join(f"{x:6.1f}" for x in ca) +
              " | " + " ".join(f"{x:6.0f}" for x in cb))
        rows.append({"k": 3, "variant": v, "input_set": "uniform_bits",
                     "instr_per_root": f"{sum(mix.values()):.3f}",
                     **{f"A_cycles_{lv}": f"{ca[i]:.2f}" for i, lv in enumerate(levels)},
                     **{f"B_ns_{lv}": f"{cb[i]:.1f}" for i, lv in enumerate(levels)}})
    # break-even lpow latency for L1 vs B1 / F0, scenario A, other classes at mid
    for scen_name, scen in (("A", SCEN_A), ("B", SCEN_B)):
        for rival in ("B1", "F0"):
            rest = cost({c: n for c, n in k3["L1"].items() if c != "lns_pow"}, scen, 1)
            target = cost(k3[rival], scen, 1)
            be = (target - rest) / k3["L1"]["lns_pow"]
            print(f"scenario {scen_name}: L1 beats {rival} while one lpow costs < {be:.1f} "
                  f"{'cycles' if scen_name == 'A' else 'ns'} (other classes at mid)")
    print()
    for set_name in ("uniform_u64", "width32"):
        kroot_block(set_name, rows, levels)
    out = os.path.join(RESD, "latency_estimates.csv")
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()), lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    print(f"\n-> {out}")


def kroot_block(set_name, rows, levels):
    km = kroot_mixes(set_name)
    print(f"\nk = 2..6, {set_name} (configuration-sweep variants, tables 11/11):")
    for k in sorted({k for k, _v in km}):
        for v in ("BK", "FK", "FS", "LK", "LDK"):
            if (k, v) not in km:
                continue
            mix = km[(k, v)]
            ca = [cost(mix, SCEN_A, i) for i in range(3)]
            cb = [cost(mix, SCEN_B, i) for i in range(3)]
            print(f"k={k} {v:4s} {sum(mix.values()):7.2f} | " +
                  " ".join(f"{x:7.1f}" for x in ca) + " | " + " ".join(f"{x:7.0f}" for x in cb))
            rows.append({"k": k, "variant": v, "input_set": set_name,
                         "instr_per_root": f"{sum(mix.values()):.3f}",
                         **{f"A_cycles_{lv}": f"{ca[i]:.2f}" for i, lv in enumerate(levels)},
                         **{f"B_ns_{lv}": f"{cb[i]:.1f}" for i, lv in enumerate(levels)}})


if __name__ == "__main__":
    main()
