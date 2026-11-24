#!/usr/bin/env bash
# Measured software run time of the whole MD simulation (dt 0.005, 200 steps) on the host CPU.
#
# Times tools/lns_lj_md_model, which runs the same operations in the same order as the simulator
# programs: FP64 and FP32 use the host's floating-point hardware, LNS (table and CORDIC) runs the
# simulator's own C++ arithmetic. This is the cost of LNS done in software on an FP machine, not
# of LNS hardware (see the cycle estimate in plot_md.py for that).
#
# Usage: examples/lns_lj_md/time_md.sh   (MD_MODEL, CORE and OUT can be set; CORE pins one CPU core)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
MD_MODEL="${MD_MODEL:-$REPO/lns_lj_md_model}"
CORE="${CORE:-2}"
OUT="${OUT:-$HERE/results/md_software_time.csv}"
PROG="$HERE/md_fp32.s" # supplies the inputs; dt 0.005, 200 steps

[[ -x "$MD_MODEL" ]] || { echo "missing executable: $MD_MODEL" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"

# variant, model arguments, timed runs (more runs for the fast variants)
RUNS=("fp64|fp64|501" "fp32|fp32|501" "lns_alu4096|lns 11 11 4096|201" "cordic_conv11|cordic 11 11 4096|31")
echo "config,runs,median_ms,min_ms,p10_ms,p90_ms,max_ms" >"$OUT"
for r in "${RUNS[@]}"; do
    IFS='|' read -r name args runs <<<"$r"
    # shellcheck disable=SC2086
    taskset -c "$CORE" "$MD_MODEL" "$PROG" $args --time "$runs" | tail -n 1 | sed "s/^[^,]*/$name/" >>"$OUT"
done
cat "$OUT"
