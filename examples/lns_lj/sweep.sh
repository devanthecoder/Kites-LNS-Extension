#!/usr/bin/env bash
# LUT-size sweep for the Lennard-Jones LNS benchmark (runs in WSL / Linux).
#
# Measures SIMULATOR (host) run time of kites-cli, not hardware latency.
# For each configuration kites-cli runs N_RUNS times (the first is discarded as warm-up); each
# run is timed externally with /usr/bin/time and the median is kept. "start-up" is the median
# of a tiny probe program run with the same table configuration (process start, Qt, assembler,
# table build, dumps); "loop" is total minus start-up. kites-cli prints per-step trace lines to
# stdout; they go to /dev/null, but formatting them is still part of the measured host time.
#
# Usage (from anywhere):
#   KITES_CLI=/path/to/release/kites-cli LJ_MODEL=/path/to/lns_lj_accuracy examples/lns_lj/sweep.sh
# Environment:
#   KITES_CLI  Release kites-cli          (default: <repo>/build-bench/kites-cli)
#   LJ_MODEL   tools/lns_lj_accuracy binary, used for table sizes (default: <repo>/lns_lj_accuracy)
#   N_RUNS     runs per configuration     (default 5, the first is discarded)
#   PIN_CPU    CPU index for taskset      (default: no pinning)
#   OUT        results CSV                (default: examples/lns_lj/results.csv)
# The programs must be generated first: python3 examples/lns_lj/gen_lj.py
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
KITES_CLI="${KITES_CLI:-$REPO/build-bench/kites-cli}"
LJ_MODEL="${LJ_MODEL:-$REPO/lns_lj_accuracy}"
N_RUNS="${N_RUNS:-5}"
OUT="${OUT:-$HERE/results.csv}"
PIN=()
if [[ -n "${PIN_CPU:-}" ]]; then PIN=(taskset -c "$PIN_CPU"); fi

for f in "$KITES_CLI" "$LJ_MODEL" /usr/bin/time; do
    [[ -x "$f" ]] || { echo "missing executable: $f" >&2; exit 1; }
done
for f in lj_fp32.s lj_lns.s lj_lns_cordic.s reference.txt; do
    [[ -f "$HERE/$f" ]] || { echo "missing $HERE/$f (run gen_lj.py)" >&2; exit 1; }
done
(( N_RUNS >= 2 )) || { echo "N_RUNS must be >= 2" >&2; exit 1; }

REF="$(head -n 1 "$HERE/reference.txt")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Each program runs in its own directory, so its vm_state/ dumps never land in the repo.
setup() { # name source-file
    mkdir -p "$WORK/$1"
    cp "$2" "$WORK/$1/prog.s"
}
setup fp32 "$HERE/lj_fp32.s"
setup lns "$HERE/lj_lns.s"
setup cordic "$HERE/lj_lns_cordic.s"
mkdir -p "$WORK/probe_fp32" "$WORK/probe_lns" "$WORK/probe_cordic"
printf '%s\n' "li a0, 0" "li a7, 10" "ecall" > "$WORK/probe_fp32/prog.s"
for v in lns cordic; do
    op=ladd; if [[ $v == cordic ]]; then op=laddc; fi
    printf '%s\n' "li t0, 0x40400000" "fmv.w.x f1, t0" "fcvt.lns.s l1, f1" "$op l2, l1, l1" \
        "li a0, 0" "li a7, 10" "ecall" > "$WORK/probe_$v/prog.s"
done

# median of the numbers on stdin
median() { sort -g | awk '{a[NR]=$1} END {if (NR%2) print a[(NR+1)/2]; else printf "%.4f\n", (a[NR/2]+a[NR/2+1])/2}'; }

# timed DIR [VAR=value ...] -> prints "elapsed user sys peakKB" medians over runs 2..N_RUNS
timed() {
    local dir="$1"; shift
    local times="$WORK/times.txt"; : > "$times"
    for ((r = 1; r <= N_RUNS; r++)); do
        (cd "$WORK/$dir" && env "$@" "${PIN[@]}" /usr/bin/time -f "%e %U %S %M" -o "$WORK/t.txt" \
            "$KITES_CLI" prog.s --vm rvss > /dev/null 2> "$WORK/stderr.txt") || {
            echo "kites-cli failed in $dir ($*):" >&2; cat "$WORK/stderr.txt" >&2; exit 1; }
        if (( r > 1 )); then tail -n 1 "$WORK/t.txt" >> "$times"; fi
    done
    local e u s m
    e=$(awk '{print $1}' "$times" | median); u=$(awk '{print $2}' "$times" | median)
    s=$(awk '{print $3}' "$times" | median); m=$(awk '{print $4}' "$times" | median)
    echo "$e $u $s $m"
}

# energy DIR KIND -> "value rel_err word" (FP32: f10; LNS: exact value of the l10 word)
energy() {
    python3 - "$WORK/$1/vm_state/registers_dump.json" "$2" "$REF" <<'EOF'
import json, math, struct, sys
d = json.load(open(sys.argv[1])); kind = sys.argv[2]; ref = float(sys.argv[3])
if kind == "fp32":
    word = int(d["fp_registers"]["f10"], 16) & 0xFFFFFFFF
    value = struct.unpack("<f", struct.pack("<I", word))[0]
else:
    word = int(d["lns_registers"]["l10"], 16)
    field = word & 0x7FFFFFFF
    field = field - (1 << 31) if field & (1 << 30) else field
    value = 0.0 if field == -(1 << 30) else math.copysign(2.0 ** (field / 2.0 ** 22), -1.0 if word >> 31 else 1.0)
print("%.9g %.3e 0x%08X" % (value, abs(value - ref) / abs(ref), word))
EOF
}

echo "config,log_bits,exp_bits,alu_samples,table_kib,total_s,startup_s,loop_s,user_s,sys_s,peak_mib,energy,rel_err,word" > "$OUT"
printf "%-22s %10s %10s %9s %9s %14s %10s %9s\n" config "table KiB" "start-up s" "loop s" "total s" energy "rel err" "peak MiB"

# run NAME DIR PROBE_DIR KIND LOG EXP SAMPLES  (LOG/EXP/SAMPLES empty = default, not exported)
run() {
    local name="$1" dir="$2" probe="$3" kind="$4" lb="$5" eb="$6" ns="$7"
    local vars=()
    if [[ -n "$lb" ]]; then vars+=("LNS_LOG_LUT_BITS=$lb"); fi
    if [[ -n "$eb" ]]; then vars+=("LNS_EXP_LUT_BITS=$eb"); fi
    if [[ -n "$ns" ]]; then vars+=("LNS_ALU_CORRECTION_SAMPLES=$ns"); fi
    local t p kib="0"
    t=($(timed "$dir" "${vars[@]}"))
    p=($(timed "$probe" "${vars[@]}"))
    local res=($(energy "$dir" "$kind"))
    if [[ $kind != fp32 ]]; then
        local sz=($("$LJ_MODEL" --size "${lb:-11}" "${eb:-11}" "${ns:-4096}"))
        if [[ $kind == cordic ]]; then sz[1]=0; fi
        kib=$(awk -v a="${sz[0]}" -v b="${sz[1]}" 'BEGIN {printf "%.2f", (a + b) / 1024}')
    fi
    local loop peak
    loop=$(awk -v a="${t[0]}" -v b="${p[0]}" 'BEGIN {printf "%.3f", a - b}')
    peak=$(awk -v m="${t[3]}" 'BEGIN {printf "%.1f", m / 1024}')
    echo "$name,${lb:-11},${eb:-11},${ns:-4096},$kib,${t[0]},${p[0]},$loop,${t[1]},${t[2]},$peak,${res[0]},${res[1]},${res[2]}" >> "$OUT"
    printf "%-22s %10s %10s %9s %9s %14s %10s %9s\n" "$name" "$kib" "${p[0]}" "$loop" "${t[0]}" "${res[0]}" "${res[1]}" "$peak"
}

run "fp32 control" fp32 probe_fp32 fp32 "" "" ""
for k in 6 7 8 9 10 11 12 13 23; do
    e=$k; if (( e > 22 )); then e=22; fi
    run "conv $k/$e" lns probe_lns lns "$k" "$e" ""
done
for s in 256 512 1024 2048 4096 8192 16384 32768 65536; do
    run "alu $s" lns probe_lns lns "" "" "$s"
done
run "cordic (conv 11/11)" cordic probe_cordic cordic "" "" ""

echo
echo "Simulator (host) time, median of $((N_RUNS - 1)) runs after one warm-up; reference E = $REF"
echo "CSV: $OUT"
