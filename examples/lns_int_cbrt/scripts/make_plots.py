#!/usr/bin/env python3
"""Slide-ready plots + the tidy CSVs they are drawn from.

Every number comes from files in examples/lns_int_cbrt/ (results.csv, config_sweep.csv,
model_grid.csv, mix.json via latency_model.py) except plot 8's predicted fix-up, which is
printed by `lns_cbrt_accuracy --yerr` (set CBRT_MODEL). Plots 7a/7b are ESTIMATES (paper
latencies + labelled assumptions), not measurements; the figures say so.

Usage (repo root): CBRT_MODEL=./lns_cbrt_accuracy python3 examples/lns_int_cbrt/scripts/make_plots.py
Writes plots/*.png and plots/data/*.csv. Needs matplotlib.
"""
from __future__ import annotations

import csv
import os
import subprocess
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))          # scripts/
ROOT = os.path.dirname(HERE)
PLOTS = os.path.join(ROOT, "plots")
RESD = os.path.join(ROOT, "results")
DATA = os.path.join(PLOTS, "data")
os.makedirs(DATA, exist_ok=True)
sys.path.insert(0, HERE)

COL = {"integer": "#4C72B0", "fp": "#DD8452", "lns": "#2CA02C", "lns2": "#8FD18F",
       "approx": "#B0B0B0", "fsqrt": "#C44E52"}
plt.rcParams.update({"font.size": 12, "axes.spines.top": False, "axes.spines.right": False,
                     "axes.grid": True, "grid.alpha": 0.3, "figure.dpi": 100})


def logy(ax, ticks):
    from matplotlib.ticker import FixedLocator, NullFormatter, ScalarFormatter
    ax.set_yscale("log")
    ax.yaxis.set_major_locator(FixedLocator(ticks))
    ax.yaxis.set_major_formatter(ScalarFormatter())
    ax.yaxis.set_minor_formatter(NullFormatter())


def rd(name):
    return list(csv.DictReader(open(os.path.join(RESD, name))))


def wr(name, rows):
    with open(os.path.join(DATA, name), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()), lineterminator="\n")
        w.writeheader()
        w.writerows(rows)


def save(fig, name, note=None):
    if note:
        fig.text(0.01, 0.01, note, fontsize=9, color="#555555", ha="left", va="bottom")
    fig.tight_layout(rect=(0, 0.04 if note else 0, 1, 1))
    fig.savefig(os.path.join(PLOTS, name), dpi=200)
    plt.close(fig)


RES = rd("results.csv")
CFG = rd("config_sweep.csv")
F = float

# 1. headline, k = 3 ------------------------------------------------------------------------
NAMES = {"B0": "B0 kernel\n(approx)", "B1": "B1 integer\n(exact)", "L0": "L0 LNS\n(approx)",
         "L1": "L1 LNS\n(exact)", "L2": "L2 LNS\nlog-divide", "F0": "F0 FP64\nNewton"}
rows = []
for v in ("B1", "L1", "L2", "F0", "B0", "L0"):
    x = next(r for r in RES if r["variant"] == v and r["input_set"] == "uniform_bits"
             and r["program"].startswith("cbrt_") and r["log_bits"] in ("", "11")
             and r["exp_bits"] in ("", "11"))
    rows.append({"variant": v, "exact": v not in ("B0", "L0"), "instr_per_root":
                 F(x["instr_per_root_uninstrumented"]), "pct_exact": F(x["pct_exact"]),
                 "max_abs_err": x["max_abs_err"], "source": "results.csv (kites-cli, 1024 roots)"})
wr("1_headline_k3.csv", rows)
fig, ax = plt.subplots(figsize=(10, 5.5))
cols = [COL["integer"], COL["lns"], COL["lns2"], COL["fp"], COL["approx"], COL["approx"]]
b = ax.bar([NAMES[r["variant"]] for r in rows], [r["instr_per_root"] for r in rows], color=cols)
for bar, r in zip(b, rows):
    ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 1, f"{r['instr_per_root']:.1f}",
            ha="center", fontweight="bold")
ax.axvline(3.5, color="#999", ls="--")
ax.text(3.55, 58, "approximate\n(not exact)", color="#777", fontsize=10)
ax.set_ylabel("instructions per root")
ax.set_title("Exact integer cube root (TCP CUBIC): LNS needs 3.4x fewer instructions")
save(fig, "1_headline_k3.png", "Measured on KITES rvss, uniform_bits (1,024 inputs), 11/11-bit "
     "tables. Instruction count, not time. All exact variants verified 100% exact.")

# 2. root degree ------------------------------------------------------------------------------
VAR = [("BK", "integer Newton", COL["integer"]), ("FK", "FP64 Newton", COL["fp"]),
       ("FS", "FP64 fsqrt.d", COL["fsqrt"]), ("LK", "LNS lpow", COL["lns"]),
       ("LDK", "LNS log-divide", COL["lns2"])]


def cfg(k, v, s, lb="11", eb="11"):
    for r in CFG:
        if (r["k"] == str(k) and r["variant"] == v and r["input_set"] == s and
                (r["log_bits"] in ("", lb)) and (r["exp_bits"] in ("", eb))):
            return F(r["instr_per_root_uninstrumented"])
    return None


rows = []
for k in range(2, 7):
    for v, lab, _c in VAR:
        y = cfg(k, v, "uniform_u64")
        if y is not None:
            rows.append({"k": k, "variant": v, "representation": lab, "input_set": "uniform_u64",
                         "instr_per_root": y, "source": "config_sweep.csv (kites-cli)"})
wr("2_root_degree.csv", rows)
fig, ax = plt.subplots(figsize=(10, 5.5))
w = 0.17
for i, (v, lab, c) in enumerate(VAR):
    pts = [(r["k"], r["instr_per_root"]) for r in rows if r["variant"] == v]
    ax.bar([k + (i - 2) * w for k, _ in pts], [y for _, y in pts], w, label=lab, color=c)
logy(ax, [10, 20, 50, 100, 200, 500])
ax.set_xticks(range(2, 7), [f"k = {k}" + (" (sqrt)" if k == 2 else "") for k in range(2, 7)])
ax.set_ylabel("instructions per root (log scale)")
ax.set_title("Root degree: LNS wins for every k >= 3, loses badly on square roots")
ax.legend(ncol=3, fontsize=10)
save(fig, "2_root_degree.png", "Measured on KITES rvss, uniform 64-bit inputs, 11/11-bit tables. "
     "k = 2 LNS cost comes from the Q9.22 format (22 fraction bits), not table size.")

# 3. input width ------------------------------------------------------------------------------
rows = []
widths = [8, 16, 24, 32, 40, 48, 56, 64]
for k in (2, 3):
    for v, lab, _c in VAR:
        for wd in widths:
            y = cfg(k, v, f"width{wd}")
            if y is not None:
                rows.append({"k": k, "variant": v, "representation": lab, "input_bits": wd,
                             "instr_per_root": y, "source": "config_sweep.csv (kites-cli)"})
wr("3_input_width.csv", rows)
fig, axs = plt.subplots(1, 2, figsize=(12, 5), sharey=True)
for ax, k in zip(axs, (2, 3)):
    for v, lab, c in VAR:
        pts = [(r["input_bits"], r["instr_per_root"]) for r in rows if r["k"] == k and r["variant"] == v]
        if pts:
            ax.plot(*zip(*pts), "o-", label=lab, color=c, lw=2)
    logy(ax, [10, 20, 50, 100, 200, 500])
    ax.set_xlabel("input width (bits)")
    ax.set_title("square root (k = 2)" if k == 2 else "cube root (k = 3)")
    ax.set_xticks(widths)
axs[0].set_ylabel("instructions per root (log scale)")
axs[1].legend(fontsize=10)
fig.suptitle("Input width: LNS square roots collapse above ~44-bit inputs; cube roots never do")
save(fig, "3_input_width.png", "Measured on KITES rvss, 11/11-bit tables.")

# 4. converter size (simulator) -------------------------------------------------------------
rows, seen = [], set()
for r in RES:
    if (r["variant"] in ("L1", "L2") and r["input_set"] == "uniform_bits" and r["log_bits"]
            and r["program"].startswith("cbrt_") and
            (r["log_bits"] == r["exp_bits"] or (r["log_bits"], r["exp_bits"]) == ("23", "22"))):
        key = (r["variant"], r["log_bits"])
        if key in seen:
            continue
        seen.add(key)
        rows.append({"variant": r["variant"], "log_bits": int(r["log_bits"]),
                     "exp_bits": int(r["exp_bits"]), "table_bytes": int(r["table_bytes"]),
                     "instr_per_root": F(r["instr_per_root_uninstrumented"]),
                     "fixup_steps_max": r["fixup_steps_max"], "source": "results.csv (kites-cli)"})
rows.sort(key=lambda r: (r["variant"], r["log_bits"]))
wr("4_converter_size.csv", rows)
fig, ax = plt.subplots(figsize=(10, 5.5))
for v, c, lab in (("L1", COL["lns"], "L1 LNS lpow (exact)"), ("L2", COL["lns2"], "L2 LNS log-divide")):
    pts = [(r["log_bits"] if r["log_bits"] != 23 else 18, r["instr_per_root"]) for r in rows
           if r["variant"] == v]
    ax.plot(*zip(*pts), "o-", color=c, lw=2.5, label=lab)
ax.axhline(62.31, color=COL["integer"], ls="--", label="B1 integer (62.3)")
ax.axhline(36.51, color=COL["fp"], ls="--", label="F0 FP64 (36.5)")
ax.axvspan(9.5, 10.5, color="#2CA02C", alpha=0.1)
ax.text(10.1, 120, "past 10 bits:\nno further gain", fontsize=10)
logy(ax, [10, 20, 50, 100, 150])
xt = list(range(6, 17)) + [18]
ax.set_xticks(xt, [str(x) for x in range(6, 17)] + ["23/22"])
ax.set_xlabel("converter table bits (log = exp)")
ax.set_ylabel("instructions per root (log scale)")
ax.set_title("Converter size, k = 3: 9-10 bits is enough, 6 bits loses to integer")
sec = ax.secondary_xaxis("top", functions=(lambda x: x, lambda x: x))
sec.set_xticks(xt, ["1K", "2K", "4K", "8K", "16K", "32K", "64K", "128K", "256K", "512K", "1M", "96M"],
               fontsize=8)
sec.set_xlabel("total table bytes", fontsize=9)
ax.legend(fontsize=10)
save(fig, "4_converter_size.png", "Measured on KITES rvss, uniform_bits (1,024 inputs). "
     "Axis break: last point is the full 23/22-bit tables (96 MiB).")

# 5. log x exp grid (C++ model) -------------------------------------------------------------
grid = [r for r in rd("model_grid.csv") if r["k"] == "3" and r["variant"] == "LK"
        and r["input_set"] == "uniform_u64"]
LB = sorted({int(r["log_bits"]) for r in grid})
EB = sorted({int(r["exp_bits"]) for r in grid})
val = {(int(r["log_bits"]), int(r["exp_bits"])): F(r["instr_per_root_model"]) for r in grid}
wr("5_table_grid_k3.csv", [{"log_bits": lb, "exp_bits": eb, "table_bytes": (4 << lb) + (4 << eb),
                            "instr_per_root_model": val[(lb, eb)],
                            "source": "model_grid.csv (C++ bit-exact model, 200k inputs)"}
                           for lb in LB for eb in EB if (lb, eb) in val])
import math  # noqa: E402

fig, ax = plt.subplots(figsize=(10, 6))
M = [[math.log10(val.get((lb, eb), float("nan"))) for eb in EB] for lb in LB]
im = ax.imshow(M, cmap="RdYlGn_r", aspect="auto", origin="lower")
for i, lb in enumerate(LB):
    for j, eb in enumerate(EB):
        v = val.get((lb, eb))
        if v is not None:
            ax.text(j, i, f"{v:.0f}", ha="center", va="center", fontsize=7)
ax.set_xticks(range(len(EB)), EB)
ax.set_yticks(range(len(LB)), LB)
ax.set_xlabel("exp table bits")
ax.set_ylabel("log table bits")
cb = fig.colorbar(im, ax=ax)
cb.set_label("log10(instructions per root)")
ax.grid(False)
ax.set_title("k = 3: the exp table decides the cost, the log table barely matters")
save(fig, "5_table_grid_k3.png", "C++ bit-exact model (matches the simulator), LK variant, "
     "uniform 64-bit inputs, 200,000 inputs per cell.")

# 6. batch size -------------------------------------------------------------------------------
rows = []
for r in RES:
    if r["program"].startswith("batch/"):
        n = int(r["n_roots"])
        rows.append({"variant": r["variant"], "batch_N": n, "setup_instr": int(r["instr_setup"]),
                     "per_root_instr": F(r["instr_per_root_uninstrumented"]),
                     "amortised_per_root": int(r["instr_setup"]) / n +
                     F(r["instr_per_root_uninstrumented"]), "source": "results.csv (kites-cli)"})
rows.sort(key=lambda r: (r["variant"], r["batch_N"]))
wr("6_batch_size.csv", rows)
fig, ax = plt.subplots(figsize=(10, 5.5))
for v, c in (("B1", COL["integer"]), ("F0", COL["fp"]), ("L1", COL["lns"]), ("L2", COL["lns2"])):
    pts = [(r["batch_N"], r["amortised_per_root"]) for r in rows if r["variant"] == v]
    ax.plot(*zip(*pts), "o-", color=c, lw=2.5, label=v)
ax.set_xscale("log", base=2)
ax.set_xlabel("roots per batch N")
ax.set_ylabel("instructions per root incl. setup")
ax.set_title("Batch size: LNS is ahead from a single root (break-even N = 1)")
ax.legend()
save(fig, "6_batch_size.png", "Measured on KITES rvss, uniform_bits prefixes, 11/11-bit tables.")

# 7. time estimate (ESTIMATE) -----------------------------------------------------------------
import latency_model as lm  # noqa: E402

est = [r for r in rd("latency_estimates.csv") if r["k"] == "3" and r["input_set"] == "uniform_bits"
       and r.get("kind", "default") == "default"]
wr("7a_time_estimate_k3.csv", [{**r, "note": "ESTIMATE: Parhami 2020 Tables 1/3 + assumptions "
                                "(convert, lpow latency); serial sum, no pipelining"} for r in est])
order = ["B1", "F0", "L1", "L2", "B0", "L0"]
est = sorted(est, key=lambda r: order.index(r["variant"]))
fig, ax = plt.subplots(figsize=(10, 5.5))
fixed = {r["variant"]: F(r["A_cycles_fixed"]) for r in est}
cmap = {"B1": COL["integer"], "F0": COL["fp"], "L1": COL["lns"], "L2": COL["lns2"],
        "B0": COL["approx"], "L0": COL["approx"]}
bars = ax.bar([NAMES[r["variant"]] for r in est], [fixed[r["variant"]] for r in est],
              color=[cmap[r["variant"]] for r in est])
for bar, r in zip(bars, est):
    m = fixed[r["variant"]]
    ax.text(bar.get_x() + bar.get_width() / 2, m + 3, f"{m:.1f}", ha="center", fontweight="bold")
ax.axvline(3.5, color="#999", ls="--")
ax.set_ylabel("estimated cycles per root")
ax.set_title(f"ESTIMATE: L1 {fixed['B1'] / fixed['L1']:.1f}x faster than integer, "
             f"{fixed['F0'] / fixed['L1']:.1f}x faster than FP64 (k = 3)")
save(fig, "7a_time_estimate_k3.png", "ESTIMATE, not measured. One fixed latency per instruction "
     "(results/latency_assumptions.csv): Parhami (2020) Table 3 where it has one; LNS convert 4\n"
     "and lpow 8 (4 table stage + 4 multiply) as in the LJ table; integer mul 3 / divu 20 from the "
     "SiFive U74 manual. Serial sum, no pipelining. Sensitivity: plot 7b.")

k3 = lm.k3_mixes()
L1 = k3["L1"]
b1m, f0m = lm.cost(k3["B1"], lm.SCEN_A, 1), lm.cost(k3["F0"], lm.SCEN_A, 1)
rows = []
for cls, xs in (("lns_pow", range(1, 161)), ("lns_convert", range(1, 81))):
    for x in xs:
        rows.append({"swept_class": cls, "latency_cycles": x,
                     "L1_cycles": lm.cost(L1, lm.SCEN_A, 1, {cls: x}),
                     "B1_cycles": b1m, "F0_cycles": f0m,
                     "note": "ESTIMATE, all other classes at their fixed value"})
wr("7b_sensitivity.csv", rows)
fig, axs = plt.subplots(1, 2, figsize=(12, 5), sharey=True)
for ax, cls, lab, n in ((axs[0], "lns_pow", "one lpow latency (cycles)", L1["lns_pow"]),
                        (axs[1], "lns_convert", "one int<->LNS convert latency (cycles)",
                         L1["lns_convert"])):
    pts = [(r["latency_cycles"], r["L1_cycles"]) for r in rows if r["swept_class"] == cls]
    ax.plot(*zip(*pts), color=COL["lns"], lw=2.5, label="L1 LNS exact")
    ax.axhline(b1m, color=COL["integer"], ls="--", label=f"B1 integer ({b1m:.0f})")
    ax.axhline(f0m, color=COL["fp"], ls="--", label=f"F0 FP64 ({f0m:.0f})")
    rest = lm.cost({c: v for c, v in L1.items() if c != cls}, lm.SCEN_A, 1)
    for t, c in ((b1m, COL["integer"]), (f0m, COL["fp"])):
        be = (t - rest) / n
        ax.axvline(be, color=c, ls=":", alpha=0.7)
        ax.annotate(f"break-even\n{be:.0f} cycles", (be, t), xytext=(5, -40),
                    textcoords="offset points", fontsize=9, color=c)
    fx = lm.pick(lm.SCEN_A[cls], 1)
    ax.axvline(fx, color=COL["lns"], lw=1.5)
    ax.annotate(f"fixed value\n{fx} cycles", (fx, lm.cost(L1, lm.SCEN_A, 1)), xytext=(8, 25),
                textcoords="offset points", fontsize=9, color=COL["lns"])
    ax.set_xlabel(lab)
axs[0].set_ylabel("estimated cycles per root")
axs[0].legend(fontsize=10, loc="upper left")
fig.suptitle("ESTIMATE: how slow could the unmeasured LNS ops be before L1 loses?")
save(fig, "7b_sensitivity.png", "ESTIMATE. Solid green line = the fixed value used in the "
     "headline (the paper gives no lpow or converter latency). Every other latency at its fixed value.")

# 9. converter table size: estimated time vs accuracy (ESTIMATE, CACTI-scaled) ----------------
sw = [r for r in rd("latency_estimates.csv") if r.get("kind") == "table_sweep"]
b1c, f0c = fixed["B1"], fixed["F0"]
rows = [{"log_bits": r["log_bits"], "exp_bits": r["exp_bits"], "variant": r["variant"],
         "instr_per_root": r["instr_per_root"], "lns_convert_cycles": r["lns_convert_cycles"],
         "lpow_cycles": r["lns_pow_cycles"], "est_cycles_per_root": r["A_cycles_fixed"],
         "B1_cycles": f"{b1c:.2f}", "F0_cycles": f"{f0c:.2f}",
         "fixup_steps_max": r["fixup_steps_max"], "pct_exact": r["pct_exact"],
         "note": r["note"] or "ESTIMATE: convert 4 / lpow 8 at 11 bits, table-read share 5/28 "
                              "scaled by CACTI 7 access time"} for r in sw]
wr("9_table_size_time_vs_accuracy.csv", rows)
fig, (ax, ax2) = plt.subplots(2, 1, figsize=(10, 7.5), sharex=True,
                              gridspec_kw={"height_ratios": [3, 1.3]})
for v, c, lab in (("L1", COL["lns"], "L1 LNS lpow (exact)"), ("L2", COL["lns2"], "L2 LNS log-divide (exact)")):
    pts = [(int(r["log_bits"]), F(r["A_cycles_fixed"])) for r in sw
           if r["variant"] == v and r["A_cycles_fixed"]]
    ax.plot(*zip(*pts), "o-", color=c, lw=2.5, label=lab)
    for x, y in pts:
        if v == "L1" and x in (6, 10, 16):
            ax.annotate(f"{y:.1f}", (x, y), xytext=(0, 8), textcoords="offset points",
                        ha="center", fontsize=9, color=c)
ax.axhline(b1c, color=COL["integer"], ls="--", label=f"B1 integer ({b1c:.1f})")
ax.axhline(f0c, color=COL["fp"], ls="--", label=f"F0 FP64 ({f0c:.1f})")
logy(ax, [20, 30, 50, 100, 200, 300])
ax.set_ylabel("estimated cycles per root (log scale)")
ax.set_title("ESTIMATE: converter table size, time vs accuracy (k = 3)")
ax.legend(fontsize=9, ncol=2)
pts = [(int(r["log_bits"]), int(r["fixup_steps_max"])) for r in sw if r["variant"] == "L1"
       and r["A_cycles_fixed"]]
ax2.bar([x for x, _ in pts], [y for _, y in pts], color=COL["lns"], alpha=0.8)
for x, y in pts:
    ax2.text(x, y * 1.15, str(y), ha="center", fontsize=8)
ax2.set_yscale("log")
ax2.set_ylabel("L1 worst\nfix-up steps")
ax2.set_xlabel("converter table bits (log = exp)")
ax2.set_xticks(range(6, 17))
save(fig, "9_table_size_time_vs_accuracy.png", "ESTIMATE. Convert 4 / lpow 8 cycles at 11 bits "
     "(assumed / derived); only the table-read share (5/28, Parhami 2020 s.4.2) scales with the\n"
     "CACTI 7 access time. Instruction counts and fix-up steps measured on KITES (uniform_bits). "
     "23/22-bit tables not modelled (no CACTI value).")

# 8. exponent precision predicts fix-up ------------------------------------------------------
model = os.environ.get("CBRT_MODEL", "/home/user/buildsrc/lns_cbrt_accuracy")
mg = rd("model_grid.csv")
rows = []
for e in range(6, 17):
    out = subprocess.run([model, "--yerr", "3", str(e)], capture_output=True, text=True).stdout
    parts = out.split()
    pred = F(parts[4])
    meas = max(int(r["steps_max_abs"]) for r in mg if r["k"] == "3" and r["variant"] == "LK"
               and r["exp_bits"] == str(e) and r["log_bits"] == "23")
    rows.append({"exp_bits": e, "rel_error_of_1_3": F(parts[3]), "predicted_worst_fixup": pred,
                 "measured_worst_fixup": meas,
                 "source": "predicted: lns_cbrt_accuracy --yerr; measured: model_grid.csv, "
                           "log 23 bits, max over 7 input sets"})
wr("8_exponent_precision.csv", rows)
fig, ax = plt.subplots(figsize=(10, 5.5))
ax.plot([r["exp_bits"] for r in rows], [r["predicted_worst_fixup"] for r in rows], "o-",
        color="#555", lw=2, label="predicted from error of 1/3 (formula)")
ax.plot([r["exp_bits"] for r in rows], [r["measured_worst_fixup"] for r in rows], "s--",
        color=COL["lns"], lw=2, label="measured worst fix-up")
logy(ax, [1, 3, 10, 30, 100, 300, 600])
ax.set_xlabel("exp table bits (sets the precision of the exponent 1/3 inside lpow)")
ax.set_ylabel("worst fix-up steps (log scale)")
ax.set_title("The exponent's precision predicts the fix-up cost (k = 3)")
ax.legend()
save(fig, "8_exponent_precision.png", "worst ~ cap x ln2 x 64 x |y_eff - 1/3|. Floor of 3 steps = "
     "Q9.22 rounding of log2(3).")

print("wrote", sorted(os.listdir(PLOTS)), sorted(os.listdir(DATA)))
