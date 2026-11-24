# KITES LNS extension: final submission

Computer Architecture project, IIT Hyderabad.
Devadath Sabarish (CO24BTECH11006), Sheshadri Rathan C S (ES24BTECH11030).

We extended the KITES RISC-V simulator with a Logarithmic Number System (LNS) instruction-set
extension (14 instructions on the single-cycle RVSS core) and compared it with FP32 and integer
arithmetic on a floating-point application (Lennard-Jones molecular dynamics) and an integer one
(the TCP CUBIC cube root, generalised to k-th roots).

## Contents

| Path | What it is |
| --- | --- |
| `report/` | The final report (PDF) and `demo.mp4`, a screen recording of the simulator running LNS code |
| `code/` | The full repository at commit `@COMMIT@`: KITES with the LNS extension, tests, tools, benchmarks and their committed results |
| `LNS_CHANGES.md` | Every file we added or changed relative to the upstream KITES we started from |

Inside `code/`:

| Path | What it is |
| --- | --- |
| `docs/LNS_EXTENSION.md` | **Start here.** Number format, registers, every instruction, table sizes, tests and benchmarks |
| `src/processor/lns/` | The LNS arithmetic library (conversions, table and CORDIC add/sub, powers) |
| `test/lns_instruction_test.cpp` | 25 GoogleTest cases for the LNS instructions |
| `test/lns_cbrt_kernel_test.cpp` | 21 GoogleTest edge cases for the integer cube-root kernels |
| `tools/` | Accuracy checks, microbenchmarks and the bit-exact C++ models (build line in each file's header) |
| `examples/lns_lj/` | Lennard-Jones energy benchmark (README inside) |
| `examples/lns_lj_md/` | Lennard-Jones molecular dynamics, the main FP application (README inside) |
| `examples/lns_int_cbrt/` | Integer cube root and k-th roots (`RESULTS.md`, `NOTES.md` inside) |

Every result in the report is committed under `examples/*/results/` (CSV) and
`examples/lns_lj_md/results/plots/`, `examples/lns_int_cbrt/plots/` (figures), so nothing has to
be re-run to check a number.

## 1. Build

All commands are run from `code/`. Qt 6.9.2 is needed even for the command-line build, because
the simulator core links Qt.

### Windows (GUI, PowerShell)

Needs Python 3. Open PowerShell in the `code` folder and run these blocks in order, in the
same window.

**1. Install Qt 6.9.2, the MinGW 13.1 toolchain, CMake and Ninja** (once per machine):

```powershell
python -m pip install aqtinstall cmake ninja
python -m aqt install-qt windows desktop 6.9.2 win64_mingw -O C:\Qt
python -m aqt install-tool windows desktop tools_mingw1310 -O C:\Qt
```

If either `aqt` download times out, run that line again with a different mirror:

```powershell
python -m aqt install-qt windows desktop 6.9.2 win64_mingw -O C:\Qt -b https://mirrors.dotsrc.org/qt/
python -m aqt install-tool windows desktop tools_mingw1310 -O C:\Qt -b https://mirrors.dotsrc.org/qt/
```

**2. Set up the window** (in every new PowerShell window). This puts pip's CMake/Ninja, MinGW and
Qt on `PATH`, and maps the folder to drive `K:` so that spaces in the path cannot break the build:

```powershell
$pyScripts = python -c "import sysconfig, os; print(sysconfig.get_path('scripts') + ';' + sysconfig.get_path('scripts', os.name + '_user'))"
$env:PATH = "$pyScripts;C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.9.2\mingw_64\bin;$env:PATH"
if ((Get-Location).Drive.Name -ne 'K') { subst K: /D 2>$null; subst K: "$PWD"; cd K:\ }
```

If Windows later says *"An Application Control policy has blocked this file"*, Smart App Control
is blocking the unsigned pip CMake. Install the signed CMake, then open a new PowerShell window,
`cd` to the `code` folder and run step 2 again:

```powershell
winget install Kitware.CMake
```

If it still blocks the compiler, use the Linux / WSL steps below (not affected by that policy).

**3. Build and start the GUI** (about 5–10 minutes):

```powershell
cmake -S . -B build\gui -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.9.2/mingw_64 `
  -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe `
  -DDISABLE_GUI=OFF -DTEST_DIR=test
cmake --build build\gui --target Kites -j4
.\build\gui\Kites.exe
```

In the GUI, select the single-cycle (RVSS) core; the LNS registers `l0`–`l31` appear in the
register view. `examples/lns_lj/lj_lns.s` is a ready program to open and run.

**Tests on Windows** (same window, after step 2):

```powershell
cmake -S . -B build\test -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.9.2/mingw_64 `
  -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe `
  -DDISABLE_GUI=ON -DENABLE_TESTS=ON -DTEST_DIR=test
cmake --build build\test --target kites_test -j4
.\build\test\test\kites_test.exe --gtest_filter="LnsInstructions.*:*LnsCbrtKernel*"
```

### Linux / WSL (command line, used for all benchmarks)

Always pass `-DTEST_DIR=test` (as below): without it, the upstream `CMakeLists.txt` searches the
whole disk for test files and configuring takes very long.

Install the tools and Qt once (Ubuntu / WSL; Qt goes to `~/Qt`, the Python tools to a virtual
environment `~/kites-venv`):

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build python3 python3-venv git libgl1-mesa-dev libxkbcommon-dev
python3 -m venv ~/kites-venv
~/kites-venv/bin/pip install aqtinstall matplotlib
~/kites-venv/bin/aqt install-qt linux desktop 6.9.2 linux_gcc_64 -O ~/Qt
```

Then, in each new terminal, from `code/`:

```bash
source ~/kites-venv/bin/activate
cmake -S . -B ~/kites-bench -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=$HOME/Qt/6.9.2/gcc_64 -DDISABLE_GUI=ON -DTEST_DIR=test
cmake --build ~/kites-bench --target kites_cli -j4
export KITES_CLI=~/kites-bench/kites-cli
```

## 2. Tests

The upstream `test/test_main.cpp` includes `windows.h`, so on Linux an empty stand-in header is
needed; on Windows the tests build as they are.

```bash
mkdir -p ~/kites-shim && touch ~/kites-shim/windows.h
cmake -S . -B ~/kites-build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=$HOME/Qt/6.9.2/gcc_64 -DENABLE_TESTS=ON -DDISABLE_GUI=ON -DTEST_DIR=test \
  -DCMAKE_CXX_FLAGS="-isystem $HOME/kites-shim"
cmake --build ~/kites-build --target kites_test -j4
~/kites-build/test/kites_test --gtest_filter='LnsInstructions.*:*LnsCbrtKernel*'
```

Expected: 46 tests pass (25 LNS instruction tests, 21 cube-root edge cases). The full suite also
runs upstream tests; 5 upstream `FpFmaConversion.DoublePrecisionFma` cases fail without our
changes too and are unrelated to LNS.

On Windows, see "Tests on Windows" in section 1.

## 3. Benchmarks: run on the simulator and check against the C++ models

Each benchmark has an independent C++ model, and the simulator must match it bit for bit. Each
README has the full instructions; these are the quick checks (Linux / WSL, from `code/`, in the
terminal where `kites-cli` was built in section 1):

```bash
source ~/kites-venv/bin/activate
export KITES_CLI=~/kites-bench/kites-cli
```

**Lennard-Jones energy** (`examples/lns_lj/README.md`), seconds:
```bash
g++ -std=c++20 -O2 -I src/processor/lns tools/lns_lj_accuracy.cpp \
    src/processor/lns/lns_alu.cpp src/processor/lns/lns_conversion.cpp \
    src/processor/lns/lns_cordic_addsub.cpp src/processor/lns/lns_pow.cpp -o lns_lj_accuracy
(cd examples/lns_lj && $KITES_CLI lj_lns.s --vm rvss > /dev/null)
./lns_lj_accuracy | grep "alu 16384 (default)"
python3 -c "import json; w = json.load(open('examples/lns_lj/vm_state/registers_dump.json'))['lns_registers']['l10']; print('simulator l10:', '0x' + w[2:].upper())"
```
Expected: the model's line shows `0x818C259B`, and the simulator prints the same word.

**Lennard-Jones molecular dynamics** (`examples/lns_lj_md/README.md`), about a minute:
```bash
g++ -std=c++20 -O2 -ffp-contract=off -I src/processor/lns tools/lns_lj_md_model.cpp \
    src/processor/lns/lns_alu.cpp src/processor/lns/lns_conversion.cpp \
    src/processor/lns/lns_cordic_addsub.cpp src/processor/lns/lns_pow.cpp -o lns_lj_md_model
QUICK=1 OUT=/tmp/md_quick WORK=/tmp/md_quick_work MD_MODEL=./lns_lj_md_model \
    bash examples/lns_lj_md/sweep_md.sh
```
`QUICK=1` runs FP32, LNS table and LNS CORDIC for 10 steps and reports each as bit-exact with the
model. The full sweep (234 runs, 3–4 hours on 8 cores) is the same command without `QUICK`,
`OUT` and `WORK`.

**Integer cube root** (`examples/lns_int_cbrt/RESULTS.md`, "Reproduce"), about 2 minutes:
```bash
g++ -std=c++20 -O2 -I src/processor/lns tools/lns_cbrt_accuracy.cpp src/processor/lns/lns_alu.cpp \
    src/processor/lns/lns_conversion.cpp src/processor/lns/lns_pow.cpp -o lns_cbrt_accuracy
python3 examples/lns_int_cbrt/scripts/gen_cbrt.py
CBRT_MODEL=./lns_cbrt_accuracy python3 examples/lns_int_cbrt/scripts/check.py
```
Expected: `ALL CHECKS PASSED` (125 simulator runs, 20,868 per-input checks).

## 4. Results, estimates and plots

These only re-derive the committed CSVs and figures (Linux / WSL, from `code/`, after
`source ~/kites-venv/bin/activate`, which provides matplotlib).

- **MD cycle estimate, table-size trade-off and plots:** `python3 examples/lns_lj_md/plot_md.py`
  (reads the committed sweep results).
- **MD software time:** `bash examples/lns_lj_md/time_md.sh` (needs `lns_lj_md_model`).
- **Table access times (CACTI 7):**
  ```bash
  git clone https://github.com/HewlettPackard/cacti ~/cacti && make -C ~/cacti
  CACTI=~/cacti/cacti python3 examples/lns_lj_md/table_latency.py
  ```
- **Integer cycle estimate and plots:**
  `CBRT_MODEL=./lns_cbrt_accuracy python3 examples/lns_int_cbrt/scripts/latency_model.py`, then
  `CBRT_MODEL=./lns_cbrt_accuracy python3 examples/lns_int_cbrt/scripts/make_plots.py`.

## Notes

- On WSL, run `kites-cli` from a folder under `/tmp`, not from `/mnt/c`: it rewrites its state
  files during the run, which is several times slower on a Windows drive. The sweep scripts
  already do this.
- Table sizes can be changed at run time with `LNS_LOG_LUT_BITS`, `LNS_EXP_LUT_BITS` and
  `LNS_ALU_CORRECTION_SAMPLES` (see `docs/LNS_EXTENSION.md`).
- Simulator run time is not a measure of LNS speed (the simulator emulates LNS in C++); the report
  uses estimated cycles and measured software time instead.
