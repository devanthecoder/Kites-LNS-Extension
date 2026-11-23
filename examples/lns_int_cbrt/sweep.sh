#!/usr/bin/env bash
# Table-size and batch-size sweep for the TCP CUBIC cube-root benchmark (runs in WSL / Linux).
#
# Primary metrics are INSTRUCTION COUNTS (instructions_retired on rvss, where cycle_count ==
# instructions_retired) and ACCURACY against the exact floor cube root. Simulator wall-clock time
# is deliberately not recorded: KITES runs integer/FP ops natively and emulates LNS ops in C++,
# so host time says nothing about LNS vs integer hardware.
#
# Every program runs in its own temporary directory (see check.py), so vm_state/ never lands in
# the repo. Every run is also verified (outputs vs Python and vs the C++ model, exactness of the
# exact variants, instruction count vs the static model); a failed check aborts the sweep.
#
# Rows written to $OUT (header from check.py --header):
#   * B0, B1, F0 on all input sets (they do not use the LNS tables);
#   * L0, L1, L2 on all input sets for converter bits 6..13 and the 23/22 extreme
#     (LNS_LOG_LUT_BITS = LNS_EXP_LUT_BITS = k, the exp table capped at 22, as in lns_lj);
#   * L0, L1, L2 on all input sets for every OFF-diagonal pair LOG x EXP from $GRID_LOG x
#     $GRID_EXP (log table and exp table sized independently);
#   * every variant's batch programs (uniform_bits prefixes, N = 1 .. 1024, no step counting)
#     at the default tables, for the break-even table.
#
# Usage (from anywhere):
#   KITES_CLI=/path/to/kites-cli CBRT_MODEL=/path/to/lns_cbrt_accuracy examples/lns_int_cbrt/sweep.sh
# Environment:
#   KITES_CLI   kites-cli                       (default: <repo>/build/kites-cli)
#   CBRT_MODEL  tools/lns_cbrt_accuracy binary  (default: <repo>/lns_cbrt_accuracy)
#   OUT         results CSV                     (default: examples/lns_int_cbrt/results.csv)
#   JOBS        parallel kites-cli runs         (default: CPU count)
#   BITS        converter sizes to sweep        (default: "6 7 8 9 10 11 12 13 23")
#   GRID_LOG    log-table bits, independent grid (default: "6 8 10 11 13 23"; "" = no grid)
#   GRID_EXP    exp-table bits, independent grid (default: "6 8 10 11 13 22")
# The k-th-root / input-width / representation sweep is separate: sweep_configs.py.
# The programs must be generated first: python3 examples/lns_int_cbrt/gen_cbrt.py
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
export KITES_CLI="${KITES_CLI:-$REPO/build/kites-cli}"
export CBRT_MODEL="${CBRT_MODEL:-$REPO/lns_cbrt_accuracy}"
OUT="${OUT:-$HERE/results.csv}"
BITS="${BITS:-6 7 8 9 10 11 12 13 23}"
GRID_LOG="${GRID_LOG-6 8 10 11 13 23}"
GRID_EXP="${GRID_EXP-6 8 10 11 13 22}"

for f in "$KITES_CLI" "$CBRT_MODEL"; do
    [[ -x "$f" ]] || { echo "missing executable: $f" >&2; exit 1; }
done
[[ -f "$HERE/reference.json" ]] || { echo "missing $HERE/reference.json (run gen_cbrt.py)" >&2; exit 1; }

CHECK=(python3 "$HERE/check.py")
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

"${CHECK[@]}" --header > "$TMP"
echo "table-independent variants (B0 B1 F0), default tables" >&2
"${CHECK[@]}" --rows 11 11 --variants B0,B1,F0 >> "$TMP"
for k in $BITS; do
    e=$k; if (( e > 22 )); then e=22; fi
    echo "LNS variants, converter $k/$e" >&2
    "${CHECK[@]}" --rows "$k" "$e" --variants L0,L1,L2 >> "$TMP"
done
for l in $GRID_LOG; do
    for e in $GRID_EXP; do
        d=$l; if (( d > 22 )); then d=22; fi
        if (( e == d )); then continue; fi       # diagonal already done above
        echo "LNS variants, converter $l/$e (independent grid)" >&2
        "${CHECK[@]}" --rows "$l" "$e" --variants L0,L1,L2 >> "$TMP"
    done
done
echo "batch programs (N = 1 .. 1024), default tables" >&2
"${CHECK[@]}" --rows 11 11 --which batch >> "$TMP"
mv "$TMP" "$OUT"
trap - EXIT
echo "CSV: $OUT ($(($(wc -l < "$OUT") - 1)) rows)" >&2
