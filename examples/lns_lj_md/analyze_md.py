#!/usr/bin/env python3
"""Decode and compare Lennard-Jones MD log records (examples/lns_lj_md).

Records come from either source, one line or memory block per log sample, 84 values each
(KE, PE, E, then the 81 position components):
  * the model (tools/lns_lj_md_model.cpp): "<sample> <step> v0 ... v83", hex words for
    fp32/lns/cordic, decimal numbers for fp64;
  * the simulator: vm_state/memory_dump.json from `kites-cli ... --mem-dump <log addr> <rows>`.

Usage:
  analyze_md.py check   <memory_dump.json> <model.txt>
        bit-exact check: every logged word of the simulator equals the model's
  analyze_md.py metrics <fmt> <records> <fp64 model.txt> <log_every>
        <fmt> fp64 | fp32 | lns; <records> is a memory_dump.json or a model .txt.
        Prints CSV: step, E, energy error vs FP64, own energy drift, position deviation vs FP64
          energy_err(t) = |E(t) - E64(t)| / |E64(0)|
          drift(t)      = (E(t) - E(0)) / |E(0)|
          pos_dev(t)    = sqrt(mean over atoms of |x_i(t) - x64_i(t)|^2)
  kites-cli <prog.s> --vm rvss 2>&1 >/dev/null | analyze_md.py count <vm_state/disassembly.txt>
        executed-instruction counts per mnemonic from the simulator's trace (stderr)
"""

import json
import math
import struct
import sys

WORDS = 84


def fp32_value(word):
    return struct.unpack("<f", struct.pack("<I", word))[0]


def lns_value(word):
    """Exact value of an LNS word: sign bit, 31-bit signed log2 with 22 fraction bits."""
    field = word & 0x7FFFFFFF
    if field & (1 << 30):
        field -= 1 << 31
    if field == -(1 << 30):
        return 0.0
    if (field >> 22) == 255:  # integer part 255: Inf (fraction 0) or NaN
        return math.nan if field & 0x3FFFFF else math.copysign(math.inf, -1.0 if word >> 31 else 1.0)
    return math.copysign(2.0 ** (field / 2.0 ** 22), -1.0 if word >> 31 else 1.0)


def read_model_words(path):
    return [[int(w, 16) for w in line.split()[2:]] for line in open(path) if line.strip()]


def read_dump_words(path, samples):
    dump = json.load(open(path))
    words = []
    for addr in sorted(dump, key=lambda a: int(a, 16)):
        v = int(dump[addr], 16)
        words += [v & 0xFFFFFFFF, v >> 32]
    need = samples * WORDS
    if len(words) < need:
        raise SystemExit("memory dump too short: %d words, need %d" % (len(words), need))
    return [words[k * WORDS:(k + 1) * WORDS] for k in range(samples)]


def read_fp64(path):
    return [[float(w) for w in line.split()[2:]] for line in open(path) if line.strip()]


def check(dump_path, model_path):
    model = read_model_words(model_path)
    sim = read_dump_words(dump_path, len(model))
    bad = sum(a != b for ms, ss in zip(model, sim) for a, b in zip(ms, ss))
    print("samples=%d words=%d mismatches=%d" % (len(model), len(model) * WORDS, bad))
    return 0 if bad == 0 else 1


def metrics(fmt, records_path, fp64_path, log_every):
    ref = read_fp64(fp64_path)
    if fmt == "fp64":
        words = []
    elif records_path.endswith(".json"):
        words = read_dump_words(records_path, len(ref))
    else:
        words = read_model_words(records_path)
    decode = fp32_value if fmt == "fp32" else lns_value
    recs = [[decode(w) for w in r] for r in words] if fmt != "fp64" else read_fp64(records_path)
    if len(recs) != len(ref):
        raise SystemExit("sample count differs from the FP64 reference")
    e0, e64_0 = recs[0][2], ref[0][2]
    print("step,E,energy_err,drift,pos_dev")
    for k, (r, f) in enumerate(zip(recs, ref)):
        n = (WORDS - 3) // 3
        sq = sum((r[3 + c] - f[3 + c]) ** 2 for c in range(3 * n))
        print("%d,%.10g,%.6e,%.6e,%.6e" % (k * log_every, r[2], abs(r[2] - f[2]) / abs(e64_0),
                                          (r[2] - e0) / abs(e0), math.sqrt(sq / n)))
    return 0


def count(disassembly_path):
    """Counts executed instructions from the 'Last executed PC' trace lines on stdin.

    The disassembly is read after the trace ends, because kites-cli writes it during the run.
    """
    by_pc, last = {}, None
    for line in sys.stdin:
        if not line.startswith("Processor clocked."):
            continue
        fields = line.replace(",", " ").split()
        pc = int(fields[fields.index("PC:") + 1])
        number = int(fields[fields.index("number:") + 1])
        if number == last:  # the final instruction is reported twice
            continue
        last = number
        by_pc[pc] = by_pc.get(pc, 0) + 1
    mnemonic = {}
    for line in open(disassembly_path):
        parts = line.split()
        if len(parts) >= 3 and parts[0].endswith(":") and len(parts[1]) == 8:
            mnemonic[int(parts[0][:-1], 16)] = parts[2]
    counts = {}
    for pc, c in by_pc.items():
        name = mnemonic.get(pc, "?")
        counts[name] = counts.get(name, 0) + c
    print("mnemonic,count")
    for name, c in sorted(counts.items(), key=lambda kv: -kv[1]):
        print("%s,%d" % (name, c))
    print("total,%d" % sum(counts.values()))
    return 0


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "count":
        return count(sys.argv[2])
    if len(sys.argv) == 4 and sys.argv[1] == "check":
        return check(sys.argv[2], sys.argv[3])
    if len(sys.argv) == 6 and sys.argv[1] == "metrics" and sys.argv[2] in ("fp64", "fp32", "lns"):
        return metrics(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]))
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
