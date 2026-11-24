#!/usr/bin/env bash
# Builds the final-submission zip, CO24BTECH11006_ES24BTECH11030_KITES_LNS.zip:
#   README.md        tools/submission_README.md (how to build, test and run everything)
#   LNS_CHANGES.md   files added / changed relative to the upstream KITES we started from
#   report/          the report PDF given as REPORT
#   code/            `git archive` of a commit (default HEAD), without docs/LNS_PPT_CONTEXT.md
#
# Only committed files go into code/, so commit (and ideally tag) first.
# Usage (from anywhere inside the repo):
#   REPORT=/path/to/report.pdf tools/make_submission.sh [ref]
# Environment:
#   REPORT      the report PDF, copied into report/ (without it the zip has no report: test only)
#   OUT         output zip (default: <repo>/../CO24BTECH11006_ES24BTECH11030_KITES_LNS.zip)
set -euo pipefail

NAME=CO24BTECH11006_ES24BTECH11030_KITES_LNS
UPSTREAM_BASE=8da397341af803514da26a1e5ace33177b4a7ae2 # upstream KITES main we last merged
EXCLUDE=(docs/LNS_PPT_CONTEXT.md)

REPO="$(git rev-parse --show-toplevel)"
REF="${1:-HEAD}"
COMMIT="$(git -C "$REPO" rev-parse --verify "$REF^{commit}")"
SHORT="$(git -C "$REPO" rev-parse --short "$COMMIT")"
OUT="${OUT:-$(dirname "$REPO")/$NAME.zip}"

if [[ "$REF" == HEAD && -n "$(git -C "$REPO" status --porcelain --untracked-files=no)" ]]; then
    echo "warning: uncommitted changes are NOT included (code/ is commit $SHORT)" >&2
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
ROOT="$STAGE/$NAME"
mkdir -p "$ROOT/code" "$ROOT/report"

# code/
PATHSPEC=(.)
for f in "${EXCLUDE[@]}"; do PATHSPEC+=(":(exclude)$f"); done
git -C "$REPO" archive --format=tar "$COMMIT" -- "${PATHSPEC[@]}" | tar -x -C "$ROOT/code"

# README.md
sed "s/@COMMIT@/$SHORT/g" "$ROOT/code/tools/submission_README.md" > "$ROOT/README.md"

# LNS_CHANGES.md
{
    echo "# Files added or changed for the LNS extension"
    echo
    echo "Compared with upstream KITES at \`${UPSTREAM_BASE:0:7}\` (the last upstream commit merged)."
    echo "Paths are relative to \`code/\`. A = added, M = modified, D = deleted, R = renamed."
    echo
    echo "| Status | File |"
    echo "| --- | --- |"
    git -C "$REPO" diff --name-status -M "$UPSTREAM_BASE" "$COMMIT" -- "${PATHSPEC[@]}" |
        awk -F'\t' '{s=substr($1,1,1); f=(NF>2)? $2" -> "$3 : $2; printf "| %s | `%s` |\n", s, f}'
} > "$ROOT/LNS_CHANGES.md"

# report/
if [[ -n "${REPORT:-}" ]]; then
    [[ -f "$REPORT" ]] || { echo "REPORT not found: $REPORT" >&2; exit 1; }
    cp "$REPORT" "$ROOT/report/"
else
    echo "warning: REPORT not set, the zip has no report" >&2
fi
[[ -n "$(ls -A "$ROOT/report")" ]] || rmdir "$ROOT/report"

# zip (Python's zipfile, so no zip package is needed)
rm -f "$OUT"
python3 - "$STAGE" "$NAME" "$OUT" <<'EOF'
import os, sys, zipfile
stage, name, out = sys.argv[1:]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, dirs, files in os.walk(os.path.join(stage, name)):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, stage))
EOF

echo "wrote $OUT (code at $SHORT, $(du -h "$OUT" | cut -f1))"
