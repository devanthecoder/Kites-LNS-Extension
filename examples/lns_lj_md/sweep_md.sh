#!/usr/bin/env bash
# Simulator sweep for the Lennard-Jones molecular-dynamics benchmark (runs in WSL / Linux).
#
# For every configuration (FP32; LNS with table add/sub at several ALU and converter table sizes;
# LNS with CORDIC add/sub at several converter sizes) and every (dt, steps) pair, this runs
# kites-cli once, reads the energy/position log from memory, checks it bit for bit against
# tools/lns_lj_md_model, and computes its error against the model's FP64 reference.
# Runs are independent and go JOBS at a time. Finished runs are kept in WORK, so an interrupted
# sweep resumes where it stopped.
#
# (dt, steps) pairs: 200 steps at every dt (fixed step count), and 1.0 time units at every dt
# (fixed simulated time: steps = 1.0 / dt). Simulator time per run is recorded but is host time.
#
# Usage (from anywhere):
#   KITES_CLI=/path/to/release/kites-cli MD_MODEL=/path/to/lns_lj_md_model \
#       examples/lns_lj_md/sweep_md.sh
# Environment:
#   KITES_CLI  Release kites-cli             (default: <repo>/build-bench/kites-cli)
#   MD_MODEL   tools/lns_lj_md_model binary  (default: <repo>/lns_lj_md_model)
#   JOBS       parallel runs at most         (default 8)
#   MEM_BUDGET_MB  memory for all running runs (default: MemAvailable - 1.5 GB). kites-cli keeps
#              its whole undo history, about 2.4 MB per simulated step, so a run is only started
#              when its estimated peak (MEM_BASE_MB + MEM_PER_STEP_KB * steps) fits the budget.
#   WORK       work directory, kept to resume (default: $HOME/lns_lj_md_work)
#   RUN_TMP    where kites-cli runs (default: /tmp/lns_lj_md_runs). kites-cli rewrites its vm_state
#              files throughout a run, so on WSL a RAM-backed /tmp makes it about 5x faster than
#              a directory on disk. Only the results are moved to WORK.
#   OUT        results directory             (default: examples/lns_lj_md/results)
#   QUICK=1    smoke test: 3 configurations, 10 steps
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
KITES_CLI="${KITES_CLI:-$REPO/build-bench/kites-cli}"
MD_MODEL="${MD_MODEL:-$REPO/lns_lj_md_model}"
JOBS="${JOBS:-8}"
MEM_BASE_MB="${MEM_BASE_MB:-80}"
MEM_PER_STEP_KB="${MEM_PER_STEP_KB:-2600}" # measured: ~2.35 MB per step for 27 atoms
WORK="${WORK:-$HOME/lns_lj_md_work}"
RUN_TMP="${RUN_TMP:-/tmp/lns_lj_md_runs}"
OUT="${OUT:-$HERE/results}"
ANALYZE="$HERE/analyze_md.py"
LOG_EVERY=5
LOG_ADDR=100003D8 # 'log' in .data: 0x10000000 + dt, nsteps, logint (12 B) + pos, vel, frc (3 x 324 B)
WORDS=84          # words per log record

# ---- one run (started by the scheduler below) ----
if [[ "${1:-}" == "--one" ]]; then
    name="$2" variant="$3" lb="$4" eb="$5" ns="$6" dt="$7" steps="$8"
    progs="$WORK/progs/dt${dt}_n${steps}"
    dir="$WORK/runs/${name}__dt${dt}_n${steps}"
    [[ -f "$dir/done" ]] && exit 0
    rm -rf "$dir" && mkdir -p "$dir"
    tmp="$RUN_TMP/$(basename "$dir")"
    rm -rf "$tmp" && mkdir -p "$tmp"
    case "$variant" in
        fp32) src=md_fp32.s fmt=fp32 ;;
        lns) src=md_lns.s fmt=lns ;;
        cordic) src=md_lns_cordic.s fmt=lns ;;
    esac
    cp "$progs/$src" "$tmp/prog.s"
    rows=$(((steps / LOG_EVERY + 1) * WORDS / 2))
    count=0
    if [[ "$dt" == "0.005" && "$steps" == "200" ]] &&
        [[ "$name" == fp32 || "$name" == lns_alu4096 || "$name" == cordic_conv11 ]]; then
        count=1 # instruction counts at the main setting, from the trace on stderr
    fi
    (
        cd "$tmp"
        export LNS_LOG_LUT_BITS="$lb" LNS_EXP_LUT_BITS="$eb" LNS_ALU_CORRECTION_SAMPLES="$ns"
        start=$(date +%s.%N)
        if ((count)); then
            "$KITES_CLI" prog.s --vm rvss --mem-dump "$LOG_ADDR" "$rows" 2>&1 >/dev/null |
                python3 "$ANALYZE" count vm_state/disassembly.txt >counts.csv
        else
            "$KITES_CLI" prog.s --vm rvss --mem-dump "$LOG_ADDR" "$rows" >/dev/null 2>/dev/null
        fi
        awk -v a="$start" -v b="$(date +%s.%N)" 'BEGIN {printf "%.2f\n", b - a}' >seconds.txt
    ) || { echo "kites-cli failed: $tmp" >&2; exit 1; }
    mv "$tmp/vm_state/memory_dump.json" "$dir/dump.json"
    mv "$tmp/seconds.txt" "$dir/"
    [[ -f "$tmp/counts.csv" ]] && mv "$tmp/counts.csv" "$dir/"
    rm -rf "$tmp"
    "$MD_MODEL" "$progs/md_fp32.s" "$variant" "$lb" "$eb" "$ns" >"$dir/model.txt"
    if python3 "$ANALYZE" check "$dir/dump.json" "$dir/model.txt" >"$dir/check.txt"; then
        echo yes >"$dir/bitexact.txt"
    else
        echo no >"$dir/bitexact.txt"
    fi
    python3 "$ANALYZE" metrics "$fmt" "$dir/dump.json" "$progs/fp64.txt" "$LOG_EVERY" >"$dir/metrics.csv"
    rm -f "$dir/model.txt"
    touch "$dir/done"
    echo "done $(basename "$dir") $(cat "$dir/seconds.txt")s bitexact=$(cat "$dir/bitexact.txt")"
    exit 0
fi

for f in "$KITES_CLI" "$MD_MODEL"; do
    [[ -x "$f" ]] || { echo "missing executable: $f" >&2; exit 1; }
done

# ---- configurations: name variant log_bits exp_bits alu_samples ----
CONFIGS=("fp32 fp32 11 11 4096")
for s in 256 512 1024 2048 4096 8192 16384 32768 65536; do CONFIGS+=("lns_alu$s lns 11 11 $s"); done
for k in 6 7 8 9 10 12 13; do CONFIGS+=("lns_conv$k lns $k $k 4096"); done
CONFIGS+=("lns_conv23 lns 23 22 4096")
for k in 6 7 8 9 10 11 12 13; do CONFIGS+=("cordic_conv$k cordic $k $k 4096"); done
RUNS=("0.001 200" "0.002 200" "0.005 200" "0.01 200" "0.02 200"
      "0.001 1000" "0.002 500" "0.01 100" "0.02 50")
if [[ "${QUICK:-0}" == 1 ]]; then
    CONFIGS=("fp32 fp32 11 11 4096" "lns_alu4096 lns 11 11 4096" "cordic_conv11 cordic 11 11 4096")
    RUNS=("0.005 10")
    WORK="$WORK/quick" OUT="$OUT/quick"
fi
export WORK RUN_TMP KITES_CLI MD_MODEL ANALYZE LOG_EVERY LOG_ADDR WORDS

# ---- programs and the FP64 reference for every (dt, steps) ----
mkdir -p "$WORK/progs" "$OUT"
for r in "${RUNS[@]}"; do
    read -r dt steps <<<"$r"
    p="$WORK/progs/dt${dt}_n${steps}"
    if [[ ! -f "$p/fp64.txt" ]]; then
        python3 "$HERE/gen_md.py" --dt "$dt" --steps "$steps" --log-every "$LOG_EVERY" --out "$p" >/dev/null
        "$MD_MODEL" "$p/md_fp32.s" fp64 >"$p/fp64.txt"
    fi
done

# ---- all runs, longest first so the parallel slots finish together ----
jobs_file="$WORK/jobs.txt"
for c in "${CONFIGS[@]}"; do
    for r in "${RUNS[@]}"; do echo "$c $r"; done
done | sort -k7,7nr >"$jobs_file"
if [[ -z "${MEM_BUDGET_MB:-}" ]]; then
    MEM_BUDGET_MB=$(($(awk '/MemAvailable/ {print int($2 / 1024)}' /proc/meminfo) - 1536))
fi
echo "$(wc -l <"$jobs_file") runs, up to $JOBS in parallel, memory budget ${MEM_BUDGET_MB} MB; work in $WORK"

# Memory-aware scheduler: start the first pending run (longest first) that fits both the job
# limit and the memory budget; otherwise wait for a run to finish.
mapfile -t pending <"$jobs_file"
declare -A job_mem=()
used=0 running=0 failures=0
while ((${#pending[@]} > 0 || running > 0)); do
    started=0
    if ((running < JOBS)); then
        for idx in "${!pending[@]}"; do
            line="${pending[$idx]}"
            need=$((MEM_BASE_MB + ${line##* } * MEM_PER_STEP_KB / 1024))
            if ((used + need <= MEM_BUDGET_MB || running == 0)); then
                # shellcheck disable=SC2086
                "$0" --one $line &
                job_mem[$!]=$need
                used=$((used + need)) running=$((running + 1)) started=1
                unset "pending[$idx]"
                break
            fi
        done
    fi
    ((started)) && continue
    finished=""
    if ! wait -n -p finished; then
        failures=$((failures + 1))
    fi
    if [[ -n "$finished" ]]; then
        used=$((used - job_mem[$finished])) running=$((running - 1))
        unset "job_mem[$finished]"
    fi
done
((failures == 0)) || echo "$failures run(s) failed; rerun the script to retry them" >&2

# ---- collect ----
samples="$OUT/md_samples.csv"
runs="$OUT/md_runs.csv"
echo "config,variant,log_bits,exp_bits,alu_samples,dt,steps,step,E,energy_err,drift,pos_dev,KE,PE" >"$samples"
echo "config,variant,log_bits,exp_bits,alu_samples,dt,steps,sim_seconds,bit_exact,max_energy_err,max_abs_drift,final_pos_dev" >"$runs"
for r in "${RUNS[@]}"; do # FP64 reference rows (from the model)
    read -r dt steps <<<"$r"
    p="$WORK/progs/dt${dt}_n${steps}"
    python3 "$ANALYZE" metrics fp64 "$p/fp64.txt" "$p/fp64.txt" "$LOG_EVERY" |
        tail -n +2 | sed "s/^/fp64,fp64,,,,$dt,$steps,/" >>"$samples"
done
while read -r name variant lb eb ns dt steps; do
    dir="$WORK/runs/${name}__dt${dt}_n${steps}"
    pre="$name,$variant,$lb,$eb,$ns,$dt,$steps"
    fmt=lns; [[ "$variant" == fp32 ]] && fmt=fp32
    python3 "$ANALYZE" metrics "$fmt" "$dir/dump.json" "$WORK/progs/dt${dt}_n${steps}/fp64.txt" \
        "$LOG_EVERY" >"$dir/metrics.csv" # recomputed from the dump, so it follows analyze_md.py
    tail -n +2 "$dir/metrics.csv" | sed "s/^/$pre,/" >>"$samples"
    tail -n +2 "$dir/metrics.csv" | awk -F, -v pre="$pre" -v t="$(cat "$dir/seconds.txt")" \
        -v b="$(cat "$dir/bitexact.txt")" '
        {e = $3 + 0; d = $4 < 0 ? -$4 : $4 + 0; if (e > me) me = e; if (d > md) md = d; p = $5}
        END {printf "%s,%s,%s,%.6e,%.6e,%s\n", pre, t, b, me, md, p}' >>"$runs"
done < <(sort "$jobs_file")
counts="$OUT/md_counts.csv"
echo "config,mnemonic,count" >"$counts"
for name in fp32 lns_alu4096 cordic_conv11; do
    f="$WORK/runs/${name}__dt0.005_n200/counts.csv"
    [[ -f "$f" ]] && tail -n +2 "$f" | sed "s/^/$name,/" >>"$counts"
done
echo "bit-exact: $(grep -c ',yes,' "$runs") of $(($(wc -l <"$runs") - 1)) runs"
echo "results: $samples, $runs, $counts"
