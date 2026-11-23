#!/usr/bin/env python3
"""End-to-end verification (and the run engine of sweep.sh) for the TCP CUBIC cube-root benchmark.

For every generated program (main variants x input sets, ablation programs, batch programs) this
runs kites-cli --vm rvss in its own temporary directory, reads memory_dump.json and
vm_state_dump.json, and asserts:
  * the input array in memory is the generator's input set (the dump is aligned);
  * B0 == Python port of Linux cubic_root() on every input;
  * B1, L1, L2, F0 == exact floor cube root (Python integers) on every input;
  * every simulator output (and every fix-up candidate = output - steps) == the C++ model
    tools/lns_cbrt_accuracy --eval, bit for bit (default table sizes);
  * instructions_retired == the generator's static model (blocks x path counts) for every
    program, including ablation and batch programs;
  * with LNS_ALU_CORRECTION_SAMPLES=2 (smallest legal add/sub table) the LNS outputs and
    instruction counts are identical, i.e. the kernel never reads the add/sub table.
Exit status is non-zero on any failure. A pass/fail table and test-vector counts are printed.

Usage (from the repo root, after building kites-cli and the model):
  python3 examples/lns_int_cbrt/gen_cbrt.py
  KITES_CLI=build/kites-cli CBRT_MODEL=./lns_cbrt_accuracy python3 examples/lns_int_cbrt/check.py
  ... check.py --rows LOG EXP [--sets all] > rows.csv   (used by sweep.sh: CSV rows only)
Environment: KITES_CLI, CBRT_MODEL, JOBS (parallel kites-cli runs, default = CPU count),
WORK_ROOT (where temp run directories go; default /dev/shm if present: kites-cli opens and
closes two files per retired instruction, so a RAM disk makes runs ~4x faster).
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import gen_cbrt as g  # noqa: E402

KITES_CLI = os.environ.get("KITES_CLI", os.path.join(REPO, "build", "kites-cli"))
CBRT_MODEL = os.environ.get("CBRT_MODEL", os.path.join(REPO, "lns_cbrt_accuracy"))
JOBS = int(os.environ.get("JOBS", os.cpu_count() or 1))
WORK_ROOT = os.environ.get("WORK_ROOT", "/dev/shm" if os.path.isdir("/dev/shm") else None)

CSV_COLUMNS = ("variant,input_set,n_roots,log_bits,exp_bits,table_bytes,instr_total,instr_setup,"
               "instr_per_root,instr_loop,instr_convert_in,instr_compute,instr_convert_out,"
               "instr_fixup,instr_count,instr_per_root_uninstrumented,fixup_steps_mean,"
               "fixup_steps_max,pct_exact,pct_nearest,max_abs_err,mean_abs_err,mean_rel_err,"
               "code_words,program").split(",")
COMPUTE_PHASES = {"fls64", "lookup", "newton", "refine", "lpow", "logdiv", "fp_newton"}


def run_program(path, mem_rows, env_extra=None):
    """Runs one program; returns (instructions_retired, code words, [u64 memory words])."""
    work = tempfile.mkdtemp(prefix="cbrt_", dir=WORK_ROOT)
    try:
        shutil.copy(path, os.path.join(work, "prog.s"))
        env = dict(os.environ)
        env.update(env_extra or {})
        proc = subprocess.run([KITES_CLI, "prog.s", "--vm", "rvss", "--mem-dump",
                               f"{g.DATA_BASE:#x}", str(mem_rows)], cwd=work, env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              timeout=float(os.environ.get("RUN_TIMEOUT", "900")))
        tail = proc.stdout[-4000:]
        m = re.search(r"Assembled .*?: (\d+) instruction word", tail)
        if proc.returncode != 0 or not m:
            raise RuntimeError(f"kites-cli failed on {path}:\n{tail}")
        with open(os.path.join(work, "vm_state", "vm_state_dump.json")) as f:
            state = json.load(f)
        if state.get("output_status") != "VM_EXIT":
            raise RuntimeError(f"{path}: did not exit cleanly ({state.get('output_status')})")
        with open(os.path.join(work, "vm_state", "memory_dump.json")) as f:
            mem = json.load(f)
        words = [int(v, 16) for _k, v in sorted(mem.items(), key=lambda kv: int(kv[0], 16))]
        return int(state["instructions_retired"]), int(m.group(1)), words
    finally:
        shutil.rmtree(work, ignore_errors=True)


def model_eval(variant, path, env_extra=None):
    env = dict(os.environ)
    env.update(env_extra or {})
    out = subprocess.run([CBRT_MODEL, "--eval", variant, path], env=env, check=True,
                         stdout=subprocess.PIPE, text=True).stdout
    return [tuple(int(t) for t in line.split()) for line in out.splitlines()]


def signed64(v):
    return v - (1 << 64) if v >> 63 else v


def accuracy(values, outputs):
    n = len(values)
    exact = [g.exact_cbrt(a) for a in values]
    errs = [abs(o - e) for o, e in zip(outputs, exact)]
    rel = [d / e for d, e in zip(errs, exact) if e > 0]
    return {"pct_exact": 100.0 * sum(d == 0 for d in errs) / n,
            "pct_nearest": 100.0 * sum(o == g.nearest_cbrt(a) for a, o in zip(values, outputs)) / n,
            "max_abs_err": max(errs), "mean_abs_err": sum(errs) / n,
            "mean_rel_err": sum(rel) / max(1, len(rel))}


class Job:
    def __init__(self, rel, prog, env_extra=None, tag=""):
        self.rel, self.prog, self.env, self.tag = rel, prog, env_extra or {}, tag
        self.path = os.path.join(HERE, rel)


def execute(job):
    prog = job.prog
    rows = prog.n * (3 if prog.exact else 2)
    total, words, mem = run_program(job.path, rows, job.env)
    n = prog.n
    res = {"total": total, "code_words": words, "inputs": mem[:n], "outputs": mem[n:2 * n],
           "steps": [signed64(s) for s in mem[2 * n:3 * n]] if prog.count else None}
    return job, res


def needs_model(prog):
    return prog.variant in ("L0", "L1", "L2", "F0")


def verify(job, res, failures):
    """All assertions for one run. Returns (static profile, accuracy dict)."""
    prog, name = job.prog, job.rel + (f" [{job.tag}]" if job.tag else "")
    ok = True

    def fail(msg):
        nonlocal ok
        ok = False
        failures.append(f"{name}: {msg}")

    if res["inputs"] != prog.values:
        fail("input array in memory does not match the generator")
    full = prog.phases == g.VARIANTS[prog.variant][1]
    candidates = None
    if needs_model(prog):
        model = model_eval(prog.variant if full else
                           {"L0": "L0", "L1": "L1", "L2": "L2", "F0": "F0"}[prog.variant],
                           job.path, job.env)
        candidates = [c for _o, c in model]
        if full:
            mismatch = sum(o != m for o, (m, _c) in zip(res["outputs"], model))
            if mismatch:
                fail(f"{mismatch} outputs differ from the C++ model")
            if prog.count:
                cmis = sum(o - s != c for o, s, c in
                           zip(res["outputs"], res["steps"], candidates))
                if cmis:
                    fail(f"{cmis} fix-up candidates differ from the C++ model")
        elif "convert_out" in prog.phases and "fixup" not in prog.phases:
            mismatch = sum(o != c for o, c in zip(res["outputs"], candidates))
            if mismatch:
                fail(f"{mismatch} raw (pre-fix-up) outputs differ from the C++ model")
    if full:
        if prog.variant == "B0":
            bad = sum(o != g.kernel_cubic_root(a) for a, o in zip(prog.values, res["outputs"]))
            if bad:
                fail(f"{bad} outputs differ from the Python port of cubic_root()")
        elif prog.exact:
            bad = sum(o != g.exact_cbrt(a) for a, o in zip(prog.values, res["outputs"]))
            if bad:
                fail(f"{bad} outputs are not the exact floor cube root")
    prof = prog.static_profile(candidates)
    if prof["total"] != res["total"]:
        fail(f"instructions_retired {res['total']} != static model {prof['total']}")
    if prog.count:
        sim_hist = Counter(res["steps"])
        if {str(k): v for k, v in sorted(sim_hist.items())} != prof["steps_hist"]:
            fail("fix-up step histogram differs from the static model")
    acc = accuracy(prog.values, res["outputs"]) if full else None
    return ok, prof, acc


def csv_row(job, res, prof, acc, log_bits, exp_bits, table_bytes):
    prog = job.prog
    ph = prof["phases"]
    n = prog.n
    setup = ph.get("setup", 0)
    per_root = (res["total"] - setup) / n
    steps = res["steps"] or []
    hist = prof["steps_hist"]
    mean_steps = (sum(abs(int(k)) * v for k, v in hist.items()) / n) if hist else ""
    max_steps = max((abs(int(k)) for k in hist), default="") if hist else ""
    compute = sum(v for k, v in ph.items() if k in COMPUTE_PHASES)
    is_lns = prog.variant.startswith("L")
    row = {
        "variant": prog.variant, "input_set": prog.set_name, "n_roots": n,
        "log_bits": log_bits if is_lns else "", "exp_bits": exp_bits if is_lns else "",
        "table_bytes": table_bytes if is_lns else (64 if prog.variant.startswith("B") else 0),
        "instr_total": res["total"], "instr_setup": setup, "instr_per_root": f"{per_root:.4f}",
        "instr_loop": f"{ph.get('loop', 0) / n:.4f}",
        "instr_convert_in": f"{ph.get('convert_in', 0) / n:.4f}",
        "instr_compute": f"{compute / n:.4f}",
        "instr_convert_out": f"{ph.get('convert_out', 0) / n:.4f}",
        "instr_fixup": f"{ph.get('fixup', 0) / n:.4f}",
        "instr_count": f"{ph.get('count', 0) / n:.4f}",
        "instr_per_root_uninstrumented": f"{per_root - ph.get('count', 0) / n:.4f}",
        "fixup_steps_mean": f"{mean_steps:.6f}" if mean_steps != "" else "",
        "fixup_steps_max": max_steps,
        "pct_exact": f"{acc['pct_exact']:.4f}" if acc else "",
        "pct_nearest": f"{acc['pct_nearest']:.4f}" if acc else "",
        "max_abs_err": acc["max_abs_err"] if acc else "",
        "mean_abs_err": f"{acc['mean_abs_err']:.6f}" if acc else "",
        "mean_rel_err": f"{acc['mean_rel_err']:.4e}" if acc else "",
        "code_words": res["code_words"],
        "program": job.rel,
    }
    del steps
    return ",".join(str(row[c]) for c in CSV_COLUMNS)


def table_bytes(log_bits, exp_bits):
    out = subprocess.run([CBRT_MODEL, "--size", str(log_bits), str(exp_bits)], check=True,
                         stdout=subprocess.PIPE, text=True).stdout
    return int(out.split()[0])


def run_jobs(jobs):
    with cf.ThreadPoolExecutor(max_workers=JOBS) as pool:
        futures = [pool.submit(execute, j) for j in jobs]
        for fut in cf.as_completed(futures):
            yield fut.result()


def main_programs(variants=None, which="main"):
    def kind(rel):
        return rel.split("/")[0] if "/" in rel else "main"
    return [(rel, p) for rel, p in g.all_programs()
            if kind(rel) == which and (variants is None or p.variant in variants)]


def rows_mode(log_bits, exp_bits, variants, which="main"):
    env = {"LNS_LOG_LUT_BITS": str(log_bits), "LNS_EXP_LUT_BITS": str(exp_bits)}
    jobs = [Job(rel, p, env) for rel, p in main_programs(variants, which)]
    tb = table_bytes(log_bits, exp_bits)
    failures = []
    out = []
    for job, res in run_jobs(jobs):
        _ok, prof, acc = verify(job, res, failures)
        out.append(csv_row(job, res, prof, acc, log_bits, exp_bits, tb))
    for line in sorted(out):
        print(line)
    for f in failures:
        # Off-default tables: outputs still have to match the model; exactness of L1/L2 too.
        print(f"FAIL {f}", file=sys.stderr)
    return 1 if failures else 0


def write_mix(progs):
    """Dynamic instruction mix per root (static model, validated above) of the uninstrumented
    N=1024 uniform_bits batch programs -> mix.json, the input of tools/cbrt_cost_model.py."""
    mix = {}
    for rel, p in progs:
        if rel.startswith("batch/") and rel.endswith(f"_n{max(g.BATCH_SIZES)}.s"):
            cands = [c for _o, c in model_eval(p.variant, os.path.join(HERE, rel))] \
                if needs_model(p) else None
            prof = p.static_profile(cands)
            per_root = {c: (prof["classes"].get(c, 0) - prof["classes_setup"].get(c, 0)) / p.n
                        for c in g.CLASSES}
            mix[p.variant] = {"program": rel, "n_roots": p.n, "setup": prof["classes_setup"],
                              "per_root": {c: v for c, v in per_root.items() if v}}
    with open(os.path.join(HERE, "mix.json"), "w") as f:
        json.dump({"note": "per-root dynamic instruction mix (excludes setup/exit), "
                           "uniform_bits, N=1024, no step counting; from the static model "
                           "that check.py validates against instructions_retired",
                   "variants": mix}, f, indent=1)
        f.write("\n")


def check_mode():
    for exe in (KITES_CLI, CBRT_MODEL):
        if not os.access(exe, os.X_OK):
            sys.exit(f"missing executable: {exe} (set KITES_CLI / CBRT_MODEL)")
    progs = g.all_programs()
    for rel, _p in progs:
        if not os.path.exists(os.path.join(HERE, rel)):
            sys.exit(f"missing {rel}: run gen_cbrt.py first")
    jobs = [Job(rel, p) for rel, p in progs]
    alu_env = {"LNS_ALU_CORRECTION_SAMPLES": "2"}
    jobs += [Job(rel, p, alu_env, tag="ALU table 2 samples") for rel, p in progs
             if "/" not in rel and p.variant.startswith("L")]
    failures = []
    results = {}
    table = []
    for job, res in run_jobs(jobs):
        ok, prof, acc = verify(job, res, failures)
        results[(job.rel, job.tag)] = res
        table.append((job.rel, job.tag, job.prog.n, res["total"], prof["total"], ok))
    # ALU-table independence: identical outputs and counts with the 2-sample add/sub table.
    for (rel, tag), res in list(results.items()):
        if tag:
            base = results[(rel, "")]
            if base["outputs"] != res["outputs"] or base["total"] != res["total"]:
                failures.append(f"{rel}: result changes with LNS_ALU_CORRECTION_SAMPLES=2")
    write_mix(progs)
    table.sort()
    print(f"{'program':44s} {'tag':22s} {'N':>5s} {'retired':>9s} {'static':>9s}  result")
    for rel, tag, n, sim, st, ok in table:
        print(f"{rel:44s} {tag:22s} {n:5d} {sim:9d} {st:9d}  {'PASS' if ok else 'FAIL'}")
    sets = g.input_sets()
    main = [p for rel, p in progs if "/" not in rel]
    vectors = sum(len(v) for v in sets.values())
    print()
    print("Test vectors per input set: " + ", ".join(f"{k}={len(v)}" for k, v in sets.items())
          + f" (total {vectors} distinct-set inputs)")
    checked = sum(p.n for p in main)
    print(f"Programs run: {len(jobs)} ({len(main)} main, "
          f"{len(progs) - len(main)} ablation/batch, "
          f"{len(jobs) - len(progs)} ALU-table re-runs); "
          f"per-input output checks on main programs: {checked}")
    if failures:
        print(f"\n{len(failures)} FAILURE(S):")
        for f in failures:
            print("  " + f)
        return 1
    print("\nALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rows", nargs=2, type=int, metavar=("LOG_BITS", "EXP_BITS"),
                    help="run the main programs with these table sizes and print CSV rows")
    ap.add_argument("--variants", default="all", help="comma list (rows mode), default all")
    ap.add_argument("--which", default="main", choices=("main", "batch", "ablation"),
                    help="program group for --rows (default main)")
    ap.add_argument("--header", action="store_true", help="print the CSV header and exit")
    args = ap.parse_args()
    if args.header:
        print(",".join(CSV_COLUMNS))
        sys.exit(0)
    if args.rows:
        vs = None if args.variants == "all" else set(args.variants.split(","))
        sys.exit(rows_mode(args.rows[0], args.rows[1], vs, args.which))
    sys.exit(check_mode())
