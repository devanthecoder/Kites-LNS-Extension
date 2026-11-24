#!/usr/bin/env python3
"""Estimated LNS add/sub latency for each table size, from CACTI, for the MD cycle estimate.

The paper latencies (Parhami 2020, Table 3: LNS add 3 cycles, subtract 3.5 on average) belong to
one particular table. A bigger table is slower to read, so this script scales the table-read part
of that latency with the access time CACTI estimates for each size:

  * Table size: entries x 8 bytes (a value and a slope, 32 bits each, for linear interpolation).
  * CACTI 7 (https://github.com/HewlettPackard/cacti): a single-ported RAM array of that size,
    45 nm, 8-byte reads, optimised for access time. A ROM would be somewhat smaller and faster;
    only the ratio between sizes is used.
  * Anchor: the paper's latencies belong to the 2048-entry table. The unit they come from
    (Coleman & Chester, "A 32-Bit Logarithmic Arithmetic Unit and Its Performance Compared to
    Floating-Point", ARITH-14, 1999, section 2.6; docs/Coleman1999_32bit_LNS_unit_ARITH14.pdf)
    uses 256-word F, D and E tables per segment over 6 segments (1,536 interpolation intervals)
    plus a 1,024-word P table: 108,032 bits, about 13 KB, for addition. That lies between our
    1024-entry (8 KB) and 2048-entry (16 KB) tables; the nearer power of two is 2048.
  * Only the table read scales. In Parhami 2020 (section 4.2, Table 1) the 28 ns LNS add includes
    an assumed 5 ns ROM access, so the read is 5/28 of the add and the rest (comparator, adders)
    stays fixed:
        cycles(size) = paper cycles x (1 - f + f x access(size) / access(2048)),  f = 5/28
The conversion tables are sized the same way (2^k entries, two tables) and their access times
are reported, but not turned into cycles: the paper has no conversion latency to anchor to.

Writes results/md_table_latency.csv. Needs a built CACTI: CACTI=/path/to/cacti (default
~/cacti/cacti; build with `git clone https://github.com/HewlettPackard/cacti && make`).
Usage: python3 examples/lns_lj_md/table_latency.py
"""

import csv
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")
CACTI = os.environ.get("CACTI", os.path.expanduser("~/cacti/cacti"))
ENTRY_BYTES = 8
ANCHOR_ENTRIES = 2048  # the paper's table size (Coleman & Chester 1999), see above
ROM_FRACTION = 5.0 / 28.0  # Parhami 2020, section 4.2: 5 ns ROM access in a 28 ns add
PAPER_ADD, PAPER_SUB = 3.0, 3.5
ALU_ENTRIES = [256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536]
CONV_BITS = [6, 7, 8, 9, 10, 11, 12, 13]


def config(size_bytes):
    """CACTI's sample cache.cfg turned into a direct-mapped 45 nm RAM of size_bytes."""
    text = open(os.path.join(os.path.dirname(CACTI), "cache.cfg")).read()
    values = {
        "-size (bytes)": str(size_bytes),
        "-block size (bytes)": str(ENTRY_BYTES),
        "-associativity": "1",
        "-technology (u)": "0.045",
        "-output/input bus width": str(8 * ENTRY_BYTES),
        "-cache type": '"ram"',
        "-design objective (weight delay, dynamic power, leakage power, cycle time, area)":
            "100:0:0:0:0",
        "-deviate (delay, dynamic power, leakage power, cycle time, area)":
            "10:100000:100000:100000:100000",
        "-Optimize ED or ED^2 (ED, ED^2, NONE):": '"NONE"',
    }
    for key, value in values.items():
        pattern = re.compile("^" + re.escape(key) + ".*$", re.M)
        if len(pattern.findall(text)) != 1:
            raise SystemExit("unexpected cache.cfg layout at " + key)
        text = pattern.sub(lambda _: key + " " + value, text)
    return text


def cacti(size_bytes):
    """Returns (access time ns, area mm^2) for a RAM of size_bytes."""
    with tempfile.NamedTemporaryFile("w", suffix=".cfg", delete=False) as f:
        f.write(config(size_bytes))
        path = f.name
    try:
        out = subprocess.run([CACTI, "-infile", path], capture_output=True, text=True,
                             cwd=os.path.dirname(CACTI)).stdout
    finally:
        os.remove(path)
    access = re.search(r"Access time \(ns\):\s*([0-9.eE+-]+)", out)
    area = re.search(r"Data array: Area \(mm2\):\s*([0-9.eE+-]+)", out)
    if not access or not area:
        raise SystemExit("CACTI gave no result for %d bytes:\n%s" % (size_bytes, out[-2000:]))
    return float(access.group(1)), float(area.group(1))


def main():
    if not os.access(CACTI, os.X_OK):
        raise SystemExit("CACTI not found at %s (set CACTI=/path/to/cacti)" % CACTI)
    rows = []
    alu = {n: cacti(n * ENTRY_BYTES) for n in ALU_ENTRIES}
    base = alu[ANCHOR_ENTRIES][0]
    for n in ALU_ENTRIES:
        access, area = alu[n]
        scale = 1 - ROM_FRACTION + ROM_FRACTION * access / base
        rows.append(["addsub", n, n * ENTRY_BYTES, "%.4f" % access, "%.4f" % area,
                     "%.3f" % (access / base), "%.3f" % (PAPER_ADD * scale),
                     "%.3f" % (PAPER_SUB * scale)])
    conv = {k: cacti(2 ** k * ENTRY_BYTES) for k in CONV_BITS}
    conv_base = conv[11][0]
    for k in CONV_BITS:
        access, area = conv[k]
        rows.append(["conversion", 2 ** k, 2 * 2 ** k * ENTRY_BYTES, "%.4f" % access,
                     "%.4f" % (2 * area), "%.3f" % (access / conv_base), "", ""])
    os.makedirs(RES, exist_ok=True)
    path = os.path.join(RES, "md_table_latency.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["table", "entries", "bytes", "access_ns", "area_mm2", "access_vs_default",
                    "ladd_cycles", "lsub_cycles"])
        w.writerows(rows)
    print("wrote results/md_table_latency.csv")
    for r in rows:
        print("  %-10s %6d entries %8d B  access %s ns (%sx)  area %s mm2  ladd %s lsub %s" %
              (r[0], r[1], r[2], r[3], r[5], r[4], r[6] or "-", r[7] or "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
