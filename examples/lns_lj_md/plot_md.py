#!/usr/bin/env python3
"""Plots and the cycle estimate for the Lennard-Jones MD benchmark (examples/lns_lj_md).

Reads results/md_runs.csv, md_samples.csv, md_counts.csv (from sweep_md.sh) and
md_software_time.csv (from time_md.sh). Writes results/md_cycles.csv and PDF figures in
results/plots/.

Cycle estimate: cycles = sum over executed instructions of count x cycles per instruction, with
the counts measured in the simulator (dt 0.005, 200 steps). Two estimates:
  full latency  every instruction waits for the previous one to finish (the method of
                Parhami 2020, section 8);
  overlapped    pipelined units start one instruction per cycle; only the divider and CORDIC,
                which are iterative, keep their full latency.
Latencies (B. Parhami, "Computing with logarithmic number system arithmetic: implementation
methods and performance benefits", Computers and Electrical Engineering 87 (2020) 106800):
  Table 3 (TMS320C6711 FP / ELM LNS, cycles): FP add, sub, mul 4; FP divide ~30; LNS add 3,
  sub 3 (4 when the range shifter is used); LNS multiply/divide 1.
  Section 4.2: range shifter used in half of all subtractions, so LNS sub averages 3.5.
  Table 1: 32-bit fixed-point multiply 32 ns at the ELM's 8 ns cycle = 4 cycles (lpown).
Not in the paper (assumed): CORDIC add/sub 106 cycles (our implementation: 2 passes of 51
iterations, one per cycle, plus normalisation and scaling) or 55 (sized for 22 fraction bits);
conversions 4; loads, stores, integer and branch instructions 1 for both formats.
"""

import csv
import math
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")
PLOTS = os.path.join(RES, "plots")

# Fixed colour per entity (validated categorical order) and text/axis tokens.
INK, INK2, MUTED, GRID, AXIS = "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7"
SERIES = {
    "fp64": (INK2, "FP64", "s"),
    "fp32": ("#2a78d6", "FP32", "o"),
    "lns_alu4096": ("#eb6834", "LNS table (4K)", "D"),
    "cordic_conv11": ("#1baf7a", "LNS CORDIC", "^"),
    "lns_alu65536": ("#eda100", "LNS table (64K)", "v"),
}
CATS = ["load/store", "add/sub", "multiply", "divide", "power", "integer, branch, other"]
CAT_COLOR = dict(zip(CATS, ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#898781"]))


def category(m):
    if m in ("flw", "fsw", "llw", "lsw", "lw", "sw"):
        return "load/store"
    if m in ("fadd.s", "fsub.s", "ladd", "lsub", "laddc", "lsubc"):
        return "add/sub"
    if m in ("fmul.s", "lmul"):
        return "multiply"
    if m == "fdiv.s":
        return "divide"
    if m == "lpown":
        return "power"
    return "integer, branch, other"


def full_latency(m, cordic):
    table = {"fadd.s": 4, "fsub.s": 4, "fmul.s": 4, "fdiv.s": 30, "lmul": 1, "ladd": 3,
             "lsub": 3.5, "lpown": 4, "laddc": cordic, "lsubc": cordic}
    if m.startswith("fcvt"):
        return 4
    return table.get(m, 1)


def overlapped(m, cordic):
    return {"fdiv.s": 30, "laddc": cordic, "lsubc": cordic}.get(m, 1)


def style():
    plt.rcParams.update({
        "font.size": 8, "axes.titlesize": 8.5, "axes.labelsize": 8, "legend.fontsize": 7,
        "xtick.labelsize": 7, "ytick.labelsize": 7, "axes.edgecolor": AXIS,
        "axes.labelcolor": INK2, "xtick.color": MUTED, "ytick.color": MUTED, "text.color": INK,
        "axes.titlecolor": INK, "axes.spines.top": False, "axes.spines.right": False,
        "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "axes.axisbelow": True,
        "lines.linewidth": 1.6, "lines.markersize": 4.5, "legend.frameon": False,
        "figure.facecolor": "white", "axes.facecolor": "white", "savefig.bbox": "tight",
        "pdf.fonttype": 42,
    })


def read(name):
    with open(os.path.join(RES, name)) as f:
        return list(csv.DictReader(f))


def save(fig, name):
    fig.savefig(os.path.join(PLOTS, name))
    plt.close(fig)
    print("wrote", os.path.join("results", "plots", name))


def run(runs, config, dt, steps):
    for r in runs:
        if r["config"] == config and r["dt"] == dt and r["steps"] == steps:
            return r
    raise SystemExit("missing run %s dt %s steps %s" % (config, dt, steps))


def series(samples, config, dt, steps, col):
    rows = [r for r in samples if r["config"] == config and r["dt"] == dt and r["steps"] == steps]
    return [int(r["step"]) for r in rows], [float(r[col]) for r in rows]


def line(ax, key, x, y, **kw):
    color, label, marker = SERIES[key]
    kw.setdefault("marker", marker)
    ax.plot(x, y, color=color, label=label, **kw)


def ref_line(ax, y, key, text=None):
    color, label = SERIES[key][0], SERIES[key][1] if text is None else text
    ax.axhline(y, color=color, linestyle="--", linewidth=1.2, label=label)


# ---------------------------------------------------------------- accuracy plots
def plot_alu_table(runs, samples):
    sizes = [256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536]
    fp64_err = max(abs(float(r["drift"])) for r in samples
                   if r["config"] == "fp64" and r["dt"] == "0.005" and r["steps"] == "200")
    fig, axes = plt.subplots(1, 2, figsize=(6.6, 2.6))
    for ax, col, title in ((axes[0], "max_energy_err", "Energy error vs FP64 (max over run)"),
                           (axes[1], "final_pos_dev", "Position deviation vs FP64 (end of run)")):
        y = [float(run(runs, "lns_alu%d" % s, "0.005", "200")[col]) for s in sizes]
        ax.plot(sizes, y, color=SERIES["lns_alu4096"][0], marker="D", label="LNS table")
        ref_line(ax, float(run(runs, "fp32", "0.005", "200")[col]), "fp32")
        ref_line(ax, float(run(runs, "cordic_conv11", "0.005", "200")[col]), "cordic_conv11")
        if col == "max_energy_err":
            ax.axhline(fp64_err, color=INK2, linestyle=":", linewidth=1.2,
                       label="FP64 time-step error")
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(sizes)
        ax.set_xticklabels(["256", "512", "1K", "2K", "4K", "8K", "16K", "32K", "64K"])
        ax.set_xlabel("Add/sub table entries")
        ax.set_title(title, loc="left")
    axes[0].legend(loc="upper right")
    fig.suptitle("Accuracy against add/sub table size (dt 0.005, 200 steps)", x=0.01, ha="left",
                 fontsize=9)
    fig.tight_layout()
    save(fig, "md_vs_addsub_table.pdf")


def plot_conv_bits(runs):
    bits = [6, 7, 8, 9, 10, 11, 12, 13]
    pos = list(range(len(bits))) + [len(bits) + 0.6]
    labels = [str(b) for b in bits] + ["23"]
    fig, axes = plt.subplots(1, 2, figsize=(6.6, 2.6))
    for ax, col, title in ((axes[0], "max_energy_err", "Energy error vs FP64 (max over run)"),
                           (axes[1], "final_pos_dev", "Position deviation vs FP64 (end of run)")):
        tab = [float(run(runs, "lns_alu4096" if b == 11 else "lns_conv%d" % b, "0.005", "200")[col])
               for b in bits + [23]]
        cor = [float(run(runs, "cordic_conv%d" % b, "0.005", "200")[col]) for b in bits]
        ax.plot(pos, tab, color=SERIES["lns_alu4096"][0], marker="D", label="LNS table (4K add/sub)")
        ax.plot(pos[:len(bits)], cor, color=SERIES["cordic_conv11"][0], marker="^", label="LNS CORDIC")
        ref_line(ax, float(run(runs, "fp32", "0.005", "200")[col]), "fp32")
        ax.set_yscale("log")
        ax.set_xticks(pos)
        ax.set_xticklabels(labels)
        ax.set_xlabel("Conversion table bits")
        ax.set_title(title, loc="left")
    axes[1].legend(loc="upper right")
    fig.suptitle("Accuracy against conversion table size (dt 0.005, 200 steps)", x=0.01, ha="left",
                 fontsize=9)
    fig.tight_layout()
    save(fig, "md_vs_conversion_table.pdf")


MAIN4 = ["fp32", "lns_alu4096", "lns_alu65536", "cordic_conv11"]


def plot_pos_vs_step(samples):
    fig, ax = plt.subplots(figsize=(4.8, 3.0))
    for key in MAIN4:
        x, y = series(samples, key, "0.001", "1000", "pos_dev")
        pts = [(a, b) for a, b in zip(x, y) if b > 0]
        line(ax, key, [a for a, _ in pts], [b for _, b in pts], marker=None)
    ax.set_yscale("log")
    ax.set_xlabel("Step")
    ax.set_ylabel("RMS position deviation vs FP64")
    ax.set_title("Position deviation over a run (dt 0.001, 1000 steps)", loc="left")
    ax.legend(loc="lower right")
    fig.tight_layout()
    save(fig, "md_pos_dev_vs_step.pdf")


def plot_pos_vs_dt(runs):
    dts = ["0.001", "0.002", "0.005", "0.01", "0.02"]
    fixed_time = {"0.001": "1000", "0.002": "500", "0.005": "200", "0.01": "100", "0.02": "50"}
    fig, axes = plt.subplots(1, 2, figsize=(6.6, 2.7), sharey=True)
    for ax, steps_of, title in ((axes[0], lambda d: "200", "Fixed step count (200 steps)"),
                                (axes[1], lambda d: fixed_time[d], "Fixed simulated time (1.0)")):
        for key in MAIN4:
            y = [float(run(runs, key, d, steps_of(d))["final_pos_dev"]) for d in dts]
            line(ax, key, [float(d) for d in dts], y)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xticks([float(d) for d in dts])
        ax.set_xticklabels(dts)
        ax.minorticks_off()
        ax.set_xlabel("Time step dt")
        ax.set_title(title, loc="left")
    axes[0].set_ylabel("Final RMS position deviation vs FP64")
    axes[0].legend(loc="upper left")
    fig.tight_layout()
    save(fig, "md_pos_dev_vs_dt.pdf")


def plot_energy(samples):
    fig, axes = plt.subplots(1, 2, figsize=(6.6, 2.7))
    _, e64 = series(samples, "fp64", "0.005", "200", "E")
    for key in ["fp64", "fp32", "lns_alu4096", "cordic_conv11"]:
        x, e = series(samples, key, "0.005", "200", "E")
        t = [s * 0.005 for s in x]
        width = 3.2 if key == "fp64" else 1.4  # FP64 drawn wide underneath: FP32 and CORDIC overlap it
        line(axes[0], key, t, [(v - e64[0]) / abs(e64[0]) for v in e], marker=None, linewidth=width)
        if key != "fp64":
            _, err = series(samples, key, "0.005", "200", "energy_err")
            pts = [(a, b) for a, b in zip(t, err) if b > 0]
            line(axes[1], key, [a for a, _ in pts], [b for _, b in pts], marker=None)
    axes[0].set_title("Total energy, relative change from start", loc="left")
    axes[0].set_ylabel("(E - E$_0$) / |E$_0$|")
    axes[1].set_title("Energy error vs FP64", loc="left")
    axes[1].set_ylabel("|E - E$_{FP64}$| / |E$_0$|")
    axes[1].set_yscale("log")
    for ax in axes:
        ax.set_xlabel("Simulated time")
    axes[0].legend(loc="lower left")
    fig.tight_layout()
    save(fig, "md_energy.pdf")


# ---------------------------------------------------------------- instructions and cycles
def by_config(counts):
    out = {}
    for r in counts:
        if r["mnemonic"] != "total":
            out.setdefault(r["config"], {})[r["mnemonic"]] = int(r["count"])
    return out


def plot_mix(counts):
    names = [("fp32", "FP32"), ("lns_alu4096", "LNS")]
    fig, ax = plt.subplots(figsize=(4.8, 2.6))
    h = 0.38
    for k, (cfg, label) in enumerate(names):
        tot = {c: 0 for c in CATS}
        for m, n in counts[cfg].items():
            tot[category(m)] += n
        y = [i + (k - 0.5) * h for i in range(len(CATS))]
        color = SERIES[cfg][0]
        ax.barh(y, [tot[c] / 1e3 for c in CATS], height=h - 0.04, color=color, label=label)
        for yy, c in zip(y, CATS):
            ax.text(tot[c] / 1e3 + 15, yy, "%.0fK" % (tot[c] / 1e3) if tot[c] else "0", va="center",
                    fontsize=6.5, color=INK2)
    ax.set_yticks(range(len(CATS)))
    ax.set_yticklabels(CATS)
    ax.invert_yaxis()
    ax.set_xlabel("Executed instructions (thousands)")
    ax.set_title("Instruction mix (dt 0.005, 200 steps; CORDIC as LNS)", loc="left")
    ax.grid(axis="y", visible=False)
    ax.legend(loc="lower right")
    fig.tight_layout()
    save(fig, "md_instruction_mix.pdf")


def cycles(counts, cfg, model, cordic):
    tot = {c: 0.0 for c in CATS}
    for m, n in counts[cfg].items():
        tot[category(m)] += n * model(m, cordic)
    return tot


def write_cycles(counts):
    rows = []
    for est, model in (("full latency", full_latency), ("overlapped", overlapped)):
        for cfg, label, cordic in (("fp32", "FP32", 0), ("lns_alu4096", "LNS table", 0),
                                   ("cordic_conv11", "LNS CORDIC (106-cycle add/sub)", 106),
                                   ("cordic_conv11", "LNS CORDIC (55-cycle add/sub)", 55)):
            tot = cycles(counts, cfg, model, cordic)
            rows.append([est, label, sum(counts[cfg].values())] + [round(tot[c]) for c in CATS]
                        + [round(sum(tot.values()))])
    with open(os.path.join(RES, "md_cycles.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["estimate", "config", "instructions"] + ["cycles_" + c for c in CATS]
                   + ["cycles_total"])
        w.writerows(rows)
    print("wrote results/md_cycles.csv")
    for r in rows:
        fp = next(x for x in rows if x[0] == r[0] and x[1] == "FP32")[-1]
        print("  %-13s %-32s %12.0f cycles  %5.2fx FP32" % (r[0], r[1], r[-1], r[-1] / fp))
    return rows


def plot_cycle_breakdown(counts):
    bars = [("Full latency", "FP32", cycles(counts, "fp32", full_latency, 0)),
            ("Full latency", "LNS table", cycles(counts, "lns_alu4096", full_latency, 0)),
            ("Overlapped", "FP32", cycles(counts, "fp32", overlapped, 0)),
            ("Overlapped", "LNS table", cycles(counts, "lns_alu4096", overlapped, 0))]
    fig, ax = plt.subplots(figsize=(6.0, 2.6))
    ypos = [0, 1, 2.4, 3.4]
    shown = set()
    for y, (est, label, tot) in zip(ypos, bars):
        left = 0.0
        for c in CATS:
            w = tot[c] / 1e6
            if w > 0:
                ax.barh(y, w, left=left, height=0.72, color=CAT_COLOR[c], edgecolor="white",
                        linewidth=1.0, label=None if c in shown else c)
                shown.add(c)
            left += w
        ax.text(left + 0.1, y, "%.2f M" % left, va="center", fontsize=7, color=INK)
    ax.set_yticks(ypos)
    ax.set_yticklabels(["%s\n%s" % (label, est.lower()) for est, label, _ in bars])
    ax.invert_yaxis()
    ax.set_xlabel("Estimated cycles (millions)")
    ax.set_title("Estimated cycles by instruction type (dt 0.005, 200 steps)", loc="left")
    ax.grid(axis="y", visible=False)
    handles, labels = ax.get_legend_handles_labels()
    order = [labels.index(c) for c in CATS]
    ax.legend([handles[k] for k in order], CATS, loc="upper center", bbox_to_anchor=(0.5, -0.22),
              ncol=3)
    fig.tight_layout()
    save(fig, "md_cycles_breakdown.pdf")


def plot_breakeven(counts):
    lns = counts["lns_alu4096"]
    addsub = lns["ladd"] + lns["lsub"]
    lat = [2 ** (k / 8) for k in range(0, 57)]  # 1 .. 128 cycles
    fig, axes = plt.subplots(1, 2, figsize=(6.6, 2.7), sharey=True)
    for ax, model, title in ((axes[0], full_latency, "Full latency"),
                             (axes[1], overlapped, "Overlapped")):
        rest = sum(n * model(m, 0) for m, n in lns.items() if m not in ("ladd", "lsub"))
        fp = sum(n * model(m, 0) for m, n in counts["fp32"].items())
        ax.plot(lat, [(rest + addsub * L) / 1e6 for L in lat], color=SERIES["lns_alu4096"][0],
                label="LNS")
        ax.axhline(fp / 1e6, color=SERIES["fp32"][0], linestyle="--", linewidth=1.2, label="FP32")
        even = (fp - rest) / addsub
        ax.axvline(even, color=MUTED, linewidth=0.8, linestyle=":")
        ax.text(even * 1.08, fp / 1e6 * 0.55, "break-even\n%.1f cycles" % even, fontsize=6.5,
                color=INK2)
        tab = 3.25 if model is full_latency else 1  # average of add 3 and sub 3.5
        for L, name, mk in ((tab, "table", "D"), (55, "CORDIC 55", "^"), (106, "CORDIC 106", "^")):
            yv = (rest + addsub * L) / 1e6
            ax.plot([L], [yv], marker=mk, color=INK, markersize=5, linestyle="none")
            ax.annotate(name, (L, yv), textcoords="offset points", xytext=(5, -10) if L < 4 else (-4, 6),
                        fontsize=6.5, ha="left" if L < 4 else "right", color=INK2)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xlim(0.7, 180)
        ax.set_xticks([1, 2, 4, 8, 16, 32, 64, 128])
        ax.set_xticklabels(["1", "2", "4", "8", "16", "32", "64", "128"])
        ax.minorticks_off()
        ax.set_xlabel("Cycles per LNS add/sub")
        ax.set_title(title, loc="left")
    axes[0].set_ylabel("Estimated cycles (millions)")
    axes[0].legend(loc="upper left")
    fig.suptitle("When does LNS beat FP32? (dt 0.005, 200 steps)", x=0.01, ha="left", fontsize=9)
    fig.tight_layout()
    save(fig, "md_cycles_breakeven.pdf")


def plot_software_time():
    path = os.path.join(RES, "md_software_time.csv")
    if not os.path.exists(path):
        print("skipped md_software_time.pdf: run time_md.sh first")
        return
    rows = read("md_software_time.csv")
    fig, ax = plt.subplots(figsize=(4.8, 1.9))
    for y, r in enumerate(rows):
        color, label, marker = SERIES[r["config"]]
        med, lo, hi = float(r["median_ms"]), float(r["p10_ms"]), float(r["p90_ms"])
        ax.plot([lo, hi], [y, y], color=color, linewidth=1.6)
        ax.plot([med], [y], marker=marker, color=color, markersize=6)
        ax.text(hi * 1.25, y, "%.3g ms" % med, va="center", fontsize=7, color=INK)
    ax.set_yticks(range(len(rows)))
    ax.set_yticklabels([SERIES[r["config"]][1] for r in rows])
    ax.invert_yaxis()
    ax.set_xscale("log")
    ax.set_xlim(0.2, 3000)
    ax.set_xticks([1, 10, 100, 1000])
    ax.set_xticklabels(["1", "10", "100", "1000"])
    ax.set_xlabel("Host run time per simulation (ms, median; bar = 10th-90th percentile)")
    ax.set_title("Measured software run time (dt 0.005, 200 steps)", loc="left")
    ax.grid(axis="y", visible=False)
    fig.tight_layout()
    save(fig, "md_software_time.pdf")


def main():
    os.makedirs(PLOTS, exist_ok=True)
    style()
    runs = read("md_runs.csv")
    samples = read("md_samples.csv")
    counts = by_config(read("md_counts.csv"))
    plot_alu_table(runs, samples)
    plot_conv_bits(runs)
    plot_pos_vs_step(samples)
    plot_pos_vs_dt(runs)
    plot_energy(samples)
    plot_mix(counts)
    write_cycles(counts)
    plot_cycle_breakdown(counts)
    plot_breakeven(counts)
    plot_software_time()
    return 0


if __name__ == "__main__":
    sys.exit(main())
