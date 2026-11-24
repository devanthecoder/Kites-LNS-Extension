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
LNS root kernel depends on, so both are given one fixed value and swept over a low / high range for sensitivity.

Fixed values: the middle entry of each (low, fixed, high) tuple is the one value used in the
report (LNS convert 4 and lpown-style multiply 4 as in the Lennard-Jones table, so lpow = 4 + 4 = 8;
divu 20). low/high are
only for the sensitivity section.

Model: serial sum of latencies of the dynamic instruction mix (no pipelining, no overlap, no
cache misses, no branch penalty). Two scenarios:
  A  cycles  - Table 3 latencies (ELM for LNS ops, TMS for FP ops), U74 for integer mul/div.
  B  ns      - Table 1 gate delays (fixed point for integer ops, FP column for FP ops, LNS
               column for LNS ops), Table 3 ns for memory.
Table-size scaling (scenario A and B, section I.G of RESULTS.md): the two LNS operations that
read a converter table get slower as the table grows. Following the Lennard-Jones MD benchmark
(examples/lns_lj_md/table_latency.py on branch Devadath), only the table-read share scales:
Parhami 2020 section 4.2 / Table 1 puts a 5 ns ROM access inside the 28 ns LNS add, so
  s(k) = 23/28 + 5/28 * access(k) / access(11),
with access(k) the CACTI 7 (commit 1ffd8df) access time of a 2^k-entry table, read from
results/cacti_table_latency.csv. fcvt.lns.* reads the LOG table, fcvt.*.lns and lpow the EXP
table:  to_lns = 4 s(log), from_lns = 4 s(exp), lns_convert = (to_lns + from_lns) / 2,
lns_pow = 4 s(exp) + 4 (only the exp-table stage scales, not the multiply). At 11/11 bits this
gives exactly 4 and 8. Tables above 16 bits have no CACTI value and are "not modelled".
The 4-cycle convert is itself an ASSUMPTION (the paper has no converter latency).

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

# (value, source) per instruction class. Ranges are (low, fixed, high).
SCEN_A = {  # cycles
    "alu": (1, "ASSUMED: 1-cycle integer ALU (ELM/TMS single-cycle integer units)"),
    "branch": (1, "ASSUMED: no branch penalty"),
    "load": (1, "paper T3: ELM load 1 cycle (cache hit)"),
    "store": (1, "ASSUMED: integer store 1 cycle, same core for every format (as in the LJ table)"),
    "mul": (3, "U74: integer mul 3 cycles (not in paper)"),
    "div": ((6, 20, 68), "U74: integer div 6-68 cycles, operand-dependent (not in paper); fixed 20"),
    "lns_convert": ((1, 4, 8), "ASSUMED: the paper gives no converter latency; fixed 4 = LJ table"),
    "lns_pow": ((5, 8, 16), "DERIVED (not in paper): KITES lpow = exp-table stage (lookup + "
                            "interpolating multiply-add, same as an LNS->int convert: 4) + Lx*m "
                            "multiply (same as LJ's lpown: 4) = 8. 5 = lookup 1 + multiply 4 "
                            "(no interpolation); 16 = twice as slow"),
    "lns_alu": (1, "paper T3: ELM divide 1 cycle (ldiv, setup only)"),
    "lns_mem": (1, "ASSUMED: LNS load/store 1 like the LJ table (ELM T3: load 1, store 2); setup only"),
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
    "lns_convert": ((9, 32, 64), "ASSUMED: fixed 32 = 4 ELM cycles of 8 ns (matches scenario A)"),
    "lns_pow": ((40, 64, 128), "DERIVED: fixed 64 = convert stage 32 + fixed-point multiply 32 "
                               "(T1); 40 = ROM 8 + multiply 32; 128 = twice as slow"),
    "lns_alu": (4, "paper T1: LNS divide 4 ns"),
    "lns_mem": (8, "paper T3: ELM load 8 ns (store taken equal, as in LJ); setup only"),
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


# ---- converter table size -> LNS convert / lpow latency (CACTI) ------------------------------
ROM_SHARE = 5.0 / 28.0       # Parhami 2020 section 4.2 / Table 1: 5 ns ROM in a 28 ns LNS add
ANCHOR_BITS = 11             # table size the fixed 4 / 8 cycles refer to (KITES default 11/11)
CONVERT_AT_ANCHOR = 4.0      # ASSUMED (no converter latency in the paper), as in the LJ table
MULTIPLY = 4.0               # lpow's log(x) * y multiply, = LJ's lpown; does not scale
CONVERT_NS_AT_ANCHOR = 32.0  # scenario B: 4 ELM cycles of 8 ns
MULTIPLY_NS = 32.0           # scenario B: 32-bit fixed-point multiply (paper T1)


def cacti_access():
    """bits -> CACTI access time (ns) of one 2^bits-entry converter table."""
    out = {}
    with open(os.path.join(RESD, "cacti_table_latency.csv")) as f:
        for r in csv.DictReader(f):
            if r["table"] == "conversion":
                n = int(r["entries"])
                out[n.bit_length() - 1] = float(r["access_ns"])
    return out


ACCESS = cacti_access()


def s(bits):
    """Latency scale of a table-reading LNS op; None when CACTI has no value (> 16 bits)."""
    if bits not in ACCESS:
        return None
    return (1.0 - ROM_SHARE) + ROM_SHARE * ACCESS[bits] / ACCESS[ANCHOR_BITS]


def lns_latencies(log_bits, exp_bits, scen="A"):
    """{lns_convert, lns_pow} for these table sizes, or None if not modelled."""
    sl, se = s(log_bits), s(exp_bits)
    if sl is None or se is None:
        return None
    conv, mult = (CONVERT_AT_ANCHOR, MULTIPLY) if scen == "A" else (CONVERT_NS_AT_ANCHOR, MULTIPLY_NS)
    to_lns, from_lns = conv * sl, conv * se
    return {"lns_convert": (to_lns + from_lns) / 2, "lns_pow": conv * se + mult}


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
    levels = ("low", "fixed", "high")
    print("ESTIMATE, not a measurement: serial latency sum per root from the paper's Tables 1/3 "
          "plus labelled assumptions.\n")
    for scen_name, scen, unit in (("A", SCEN_A, "cycles"), ("B", SCEN_B, "ns")):
        print(f"Scenario {scen_name} ({unit}) sources:")
        for cls, (v, src) in scen.items():
            print(f"  {cls:12s} {str(v):16s} {src}")
        print()
    anchor = lns_latencies(ANCHOR_BITS, ANCHOR_BITS)
    if anchor != {"lns_convert": pick(SCEN_A["lns_convert"], 1), "lns_pow": pick(SCEN_A["lns_pow"], 1)}:
        raise SystemExit(f"CACTI scaling at 11/11 gives {anchor}, not the fixed 4 / 8")
    print(f"CACTI scaling check: at 11/11 bits lns_convert = {anchor['lns_convert']}, "
          f"lns_pow = {anchor['lns_pow']} (= the fixed values)\n")
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
        rows.append({"k": 3, "variant": v, "input_set": "uniform_bits", "kind": "default",
                     "log_bits": 11, "exp_bits": 11,
                     "instr_per_root": f"{sum(mix.values()):.3f}",
                     **{f"A_cycles_{lv}": f"{ca[i]:.2f}" for i, lv in enumerate(levels)},
                     **{f"B_ns_{lv}": f"{cb[i]:.1f}" for i, lv in enumerate(levels)}})
    # break-even lpow latency for L1 vs B1 / F0, other classes at their fixed value
    for scen_name, scen in (("A", SCEN_A), ("B", SCEN_B)):
        for rival in ("B1", "F0"):
            rest = cost({c: n for c, n in k3["L1"].items() if c != "lns_pow"}, scen, 1)
            target = cost(k3[rival], scen, 1)
            be = (target - rest) / k3["L1"]["lns_pow"]
            print(f"scenario {scen_name}: L1 beats {rival} while one lpow costs < {be:.1f} "
                  f"{'cycles' if scen_name == 'A' else 'ns'} (other classes at their fixed value)")
    print()
    for set_name in ("uniform_u64", "width32"):
        kroot_block(set_name, rows, levels)
    table_block(rows, k3)
    kroot_table_block(rows)
    out = os.path.join(RESD, "latency_estimates.csv")
    with open(out, "w", newline="") as f:
        fields = list(rows[0].keys()) + [c for c in rows[-1] if c not in rows[0]]
        w = csv.DictWriter(f, fieldnames=fields, lineterminator="\n", restval="")
        w.writeheader()
        w.writerows(rows)
    print(f"\n-> {out}")


def table_mix(variant, log_bits, exp_bits):
    """Per-root class mix of a k = 3 LNS variant at these converter tables: the static model of
    the N = 1024 uniform_bits batch program with the C++ model's fix-up candidates at the same
    table sizes (no simulator run). Checked below against results.csv."""
    import check as chk
    import gen_cbrt as g
    rel = f"batch/{g.VARIANTS[variant][0]}_n{max(g.BATCH_SIZES)}.s"
    prog = dict(g.all_programs())[rel]
    path = os.path.join(PROG, rel)
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(prog.text())
    env = {"LNS_LOG_LUT_BITS": str(log_bits), "LNS_EXP_LUT_BITS": str(exp_bits)}
    cands = [c for _o, c in chk.model_eval(variant, path, env)]
    prof = prog.static_profile(cands)
    return {c: (prof["classes"].get(c, 0) - prof["classes_setup"].get(c, 0)) / prog.n
            for c in g.CLASSES if prof["classes"].get(c, 0) - prof["classes_setup"].get(c, 0)}


TABLE_BITS = list(range(6, 17)) + [23]


def table_block(rows, k3):
    """k = 3, uniform_bits, converter tables 6..16 bits (log = exp) and 23/22: L0/L1/L2 cycles
    with CACTI-scaled LNS latencies, next to accuracy from results.csv (simulator)."""
    res = {}
    with open(os.path.join(RESD, "results.csv")) as f:
        for r in csv.DictReader(f):
            if r["input_set"] == "uniform_bits" and r["program"].startswith("cbrt_") and r["log_bits"]:
                res[(r["variant"], int(r["log_bits"]), int(r["exp_bits"]))] = r
    b1 = cost(k3["B1"], SCEN_A, 1)
    f0 = cost(k3["F0"], SCEN_A, 1)
    print("\nk = 3, uniform_bits, converter table size (log = exp), CACTI-scaled convert / lpow:")
    print(f"{'bits':>5s} {'var':4s} {'instr':>7s} {'conv':>6s} {'lpow':>6s} {'cycles':>8s} "
          f"{'maxfix':>6s} {'exact%':>7s}   (B1 {b1:.1f}, F0 {f0:.1f})")
    for bits in TABLE_BITS:
        eb = min(bits, 22)
        for v in ("L0", "L1", "L2"):
            r = res[(v, bits, eb)]
            mix = table_mix(v, bits, eb)
            n_instr = sum(mix.values())
            want = float(r["instr_per_root_uninstrumented"])
            if abs(n_instr - want) > 5e-4:
                raise SystemExit(f"{v} {bits}/{eb}: static model {n_instr:.4f} != results.csv {want}")
            lat_a, lat_b = lns_latencies(bits, eb, "A"), lns_latencies(bits, eb, "B")
            row = {"k": 3, "variant": v, "input_set": "uniform_bits", "kind": "table_sweep",
                   "log_bits": bits, "exp_bits": eb, "instr_per_root": f"{n_instr:.3f}",
                   "A_cycles_low": "", "A_cycles_fixed": "", "A_cycles_high": "",
                   "B_ns_low": "", "B_ns_fixed": "", "B_ns_high": "",
                   "lns_convert_cycles": "", "lns_pow_cycles": "",
                   "fixup_steps_max": r["fixup_steps_max"], "fixup_steps_mean": r["fixup_steps_mean"],
                   "pct_exact": r["pct_exact"], "note": ""}
            if lat_a is None:
                row["note"] = "not modelled: no CACTI value above 16 bits"
                print(f"{bits:5d} {v:4s} {n_instr:7.2f}   not modelled (no CACTI value)")
            else:
                ca = cost(mix, SCEN_A, 1, lat_a)
                cb = cost(mix, SCEN_B, 1, lat_b)
                row.update({"A_cycles_fixed": f"{ca:.2f}", "B_ns_fixed": f"{cb:.1f}",
                            "lns_convert_cycles": f"{lat_a['lns_convert']:.3f}",
                            "lns_pow_cycles": f"{lat_a['lns_pow']:.3f}"})
                print(f"{bits:5d} {v:4s} {n_instr:7.2f} {lat_a['lns_convert']:6.3f} "
                      f"{lat_a['lns_pow']:6.3f} {ca:8.2f} {r['fixup_steps_max']:>6s} "
                      f"{float(r['pct_exact']):7.2f}")
            rows.append(row)


def kroot_table_block(rows, set_name="uniform_bits"):
    """k = 2..6 LNS variants (LK, LDK) at converter tables 6..16 bits (log = exp): CACTI-scaled
    cycles per root, from the static model with the C++ model's fix-up candidates at each table
    size (checked against config_sweep.csv where the simulator ran that size)."""
    import sweep_configs as s
    sim = {}
    with open(os.path.join(RESD, "config_sweep.csv")) as f:
        for r in csv.DictReader(f):
            if r["input_set"] == set_name and r["log_bits"]:
                sim[(int(r["k"]), r["variant"], int(r["log_bits"]), int(r["exp_bits"]))] = r
    progs = [p for p in s.all_kprogs() if p.set_name == set_name and p.variant in ("BK", "LK", "LDK")]
    base = kroot_mixes(set_name)
    print(f"\nk = 2..6, {set_name}, converter table size (log = exp), CACTI-scaled convert / lpow:")
    print(f"{'k':>2s} {'var':4s} {'bits':>4s} {'instr':>8s} {'cycles':>9s} {'BK cycles':>9s} {'maxfix':>7s}")
    for p in sorted(progs, key=lambda q: (q.k, q.variant)):
        if p.variant == "BK":
            continue
        bk = cost(base[(p.k, "BK")], SCEN_A, 1)
        path = os.path.join(PROG, p.rel)
        if not os.path.exists(path):
            s.write_programs([p])
        q = s.KProg(p.variant, p.k, p.set_name, p.values, count=False)
        for bits in range(6, 17):
            env = {"LNS_LOG_LUT_BITS": str(bits), "LNS_EXP_LUT_BITS": str(bits)}
            evals = s.model_keval(p, path, env)
            prof = q.static_profile([m[1] for m in evals])
            mix = {c: (prof["classes"].get(c, 0) - prof["setup_classes"].get(c, 0)) / q.n
                   for c in prof["classes"]}
            mix = {c: v for c, v in mix.items() if v}
            n_instr = sum(mix.values())
            ref = sim.get((p.k, p.variant, bits, bits))
            if ref is not None and abs(n_instr - float(ref["instr_per_root_uninstrumented"])) > 5e-3:
                raise SystemExit(f"k={p.k} {p.variant} {bits}: static model {n_instr:.4f} != "
                                 f"config_sweep.csv {ref['instr_per_root_uninstrumented']}")
            lat = lns_latencies(bits, bits, "A")
            ca = cost(mix, SCEN_A, 1, lat)
            steps = max(abs(m[1] - m[0]) for m in evals)
            print(f"{p.k:2d} {p.variant:4s} {bits:4d} {n_instr:8.2f} {ca:9.2f} {bk:9.2f} {steps:7d}")
            rows.append({"k": p.k, "variant": p.variant, "input_set": set_name,
                         "kind": "kroot_table_sweep", "log_bits": bits, "exp_bits": bits,
                         "instr_per_root": f"{n_instr:.3f}", "A_cycles_low": "",
                         "A_cycles_fixed": f"{ca:.2f}", "A_cycles_high": "", "B_ns_low": "",
                         "B_ns_fixed": "", "B_ns_high": "",
                         "lns_convert_cycles": f"{lat['lns_convert']:.3f}",
                         "lns_pow_cycles": f"{lat['lns_pow']:.3f}", "fixup_steps_max": steps,
                         "fixup_steps_mean": "", "pct_exact": "",
                         "note": f"integer BK at 11/11: {bk:.2f} cycles; "
                                 + ("checked against simulator" if ref is not None else "model only")})


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
            rows.append({"k": k, "variant": v, "input_set": set_name, "kind": "default",
                         "log_bits": 11, "exp_bits": 11,
                         "instr_per_root": f"{sum(mix.values()):.3f}",
                         **{f"A_cycles_{lv}": f"{ca[i]:.2f}" for i, lv in enumerate(levels)},
                         **{f"B_ns_{lv}": f"{cb[i]:.1f}" for i, lv in enumerate(levels)}})


if __name__ == "__main__":
    main()
