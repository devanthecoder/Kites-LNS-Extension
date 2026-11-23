// Plain-C++ model of the Lennard-Jones molecular-dynamics programs (examples/lns_lj_md), no Qt.
//
// Runs the same velocity-Verlet simulation as md_fp32.s / md_lns.s / md_lns_cordic.s, with every
// arithmetic operation in the same order, so the FP32 and LNS log records must match the
// simulator's memory bit for bit. It also runs the FP64 reference (C++ double), which starts from
// the same float32 inputs and uses the exact decimal time step.
//
// Variants:  fp64 | fp32 | lns (table add/sub) | cordic (CORDIC add/sub)
// LNS table sizes: log LUT bits, exp LUT bits, ALU correction samples (defaults as the simulator).
//
// Output: one line per log sample: "<sample> <step>" then 84 values: KE, PE, E and the 81
// position components. FP32 and LNS values are printed as 8-digit hex words (the bits the
// program stores), FP64 values with %.17g.
//
// Build (from the repo root, one command line):
//   g++ -std=c++20 -O2 -ffp-contract=off -I src/processor/lns tools/lns_lj_md_model.cpp
//       src/processor/lns/lns_alu.cpp src/processor/lns/lns_conversion.cpp
//       src/processor/lns/lns_cordic_addsub.cpp src/processor/lns/lns_pow.cpp -o lns_lj_md_model
// Run:  ./lns_lj_md_model examples/lns_lj_md/md_fp32.s <variant> [log_bits exp_bits alu_samples]
//   The program file only supplies the inputs (dt, nsteps, logint, pos, vel); any of the three
//   generated programs works.
#include "lns_alu.h"
#include "lns_conversion.h"
#include "lns_cordic_addsub.h"
#include "lns_pow.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Kites::lns;

namespace
{
struct Input
{
    std::string dt;                    ///< time-step literal as written in .data
    long steps = -1;
    long logint = -1;
    std::vector<std::string> pos, vel; ///< float literals, 3 per atom
};

std::string trim(const std::string &s)
{
    const auto b = s.find_first_not_of(" \t\r");
    const auto e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

/// Reads the .data section of a generated program (labels dt, nsteps, logint, pos, vel).
Input readInput(const std::string &path)
{
    std::ifstream file(path);
    if (!file)
    {
        throw std::runtime_error("cannot open " + path);
    }
    std::map<std::string, std::vector<std::string>> values;
    std::string line, label;
    while (std::getline(file, line))
    {
        line = trim(line.substr(0, line.find('#')));
        if (line == ".text")
        {
            break;
        }
        const auto colon = line.find(':');
        if (colon != std::string::npos && line[0] != '.')
        {
            label = trim(line.substr(0, colon));
            line = trim(line.substr(colon + 1));
        }
        for (const char *directive : {".float", ".word"})
        {
            if (line.rfind(directive, 0) == 0)
            {
                std::stringstream items(line.substr(std::string(directive).size()));
                std::string item;
                while (std::getline(items, item, ','))
                {
                    values[label].push_back(trim(item));
                }
            }
        }
    }
    Input in;
    auto single = [&](const char *name) {
        if (values[name].size() != 1)
        {
            throw std::runtime_error(std::string("expected one value for ") + name);
        }
        return values[name][0];
    };
    in.dt = single("dt");
    in.steps = std::stol(single("nsteps"));
    in.logint = std::stol(single("logint"));
    in.pos = values["pos"];
    in.vel = values["vel"];
    if (in.pos.empty() || in.pos.size() % 3 || in.vel.size() != in.pos.size() || in.logint < 1 ||
        in.steps < 0 || in.steps % in.logint)
    {
        throw std::runtime_error("unexpected .data layout in " + path);
    }
    return in;
}

using Record = std::vector<std::string>;

/// FP32 / FP64 simulation with T = float / double; same operation order as md_fp32.s.
template <class T> std::vector<Record> runFloat(const Input &in, T dt, T (*fromLiteral)(const std::string &))
{
    const size_t m = in.pos.size(), n = m / 3;
    std::vector<T> x(m), v(m), F(m);
    for (size_t k = 0; k < m; ++k)
    {
        x[k] = fromLiteral(in.pos[k]);
        v[k] = fromLiteral(in.vel[k]);
    }
    const T half = 0.5, one = 1, c24 = 24, c48 = 48, c4 = 4;
    const T h = half * dt;
    T PE = 0;

    auto force = [&](bool pe) {
        for (T &f : F)
        {
            f = T(0);
        }
        PE = T(0);
        for (size_t i = 0; i < n; ++i)
        {
            const T xi = x[3 * i], yi = x[3 * i + 1], zi = x[3 * i + 2];
            T Fx = T(0), Fy = T(0), Fz = T(0);
            for (size_t j = i + 1; j < n; ++j)
            {
                const T dx = xi - x[3 * j], dy = yi - x[3 * j + 1], dz = zi - x[3 * j + 2];
                T r2 = dx * dx;
                T t = dy * dy;
                r2 = r2 + t;
                t = dz * dz;
                r2 = r2 + t;
                const T inv2 = one / r2;
                T s6 = inv2 * inv2;
                s6 = s6 * inv2;
                const T s12 = s6 * s6;
                T a = c48 * s12;
                const T b = c24 * s6;
                a = a - b;
                const T f = a * inv2;
                const T fx = f * dx, fy = f * dy, fz = f * dz;
                Fx = Fx + fx;
                Fy = Fy + fy;
                Fz = Fz + fz;
                F[3 * j] = F[3 * j] - fx;
                F[3 * j + 1] = F[3 * j + 1] - fy;
                F[3 * j + 2] = F[3 * j + 2] - fz;
                if (pe)
                {
                    T u = s12 - s6;
                    u = c4 * u;
                    PE = PE + u;
                }
            }
            F[3 * i] = F[3 * i] + Fx;
            F[3 * i + 1] = F[3 * i + 1] + Fy;
            F[3 * i + 2] = F[3 * i + 2] + Fz;
        }
    };
    auto kick = [&] {
        for (size_t k = 0; k < m; ++k)
        {
            const T t = h * F[k];
            v[k] = v[k] + t;
        }
    };
    auto drift = [&] {
        for (size_t k = 0; k < m; ++k)
        {
            const T t = dt * v[k];
            x[k] = x[k] + t;
        }
    };
    auto fmt = [](T value) {
        char buf[32];
        if constexpr (sizeof(T) == 4)
        {
            std::snprintf(buf, sizeof buf, "%08X", std::bit_cast<uint32_t>(value));
        }
        else
        {
            std::snprintf(buf, sizeof buf, "%.17g", value);
        }
        return std::string(buf);
    };
    std::vector<Record> log;
    auto sample = [&] {
        T sum = T(0);
        for (size_t k = 0; k < m; ++k)
        {
            const T t = v[k] * v[k];
            sum = sum + t;
        }
        const T KE = half * sum;
        const T E = KE + PE;
        Record r{fmt(KE), fmt(PE), fmt(E)};
        for (T c : x)
        {
            r.push_back(fmt(c));
        }
        log.push_back(r);
    };

    force(true);
    sample();
    long countdown = in.logint;
    for (long s = 0; s < in.steps; ++s)
    {
        kick();
        drift();
        const bool pe = --countdown == 0;
        force(pe);
        kick();
        if (pe)
        {
            sample();
            countdown = in.logint;
        }
    }
    return log;
}

/// LNS simulation; same operation order as md_lns.s (add/sub = table or CORDIC).
std::vector<Record> runLns(const Input &in, const LnsConverter &conv, LnsWord (*add)(LnsWord, LnsWord),
                           LnsWord (*sub)(LnsWord, LnsWord))
{
    const size_t m = in.pos.size(), n = m / 3;
    auto fromLiteral = [&](const std::string &s) {
        return conv.fromFloatBits(std::bit_cast<uint32_t>(std::stof(s))); // fcvt.lns.s
    };
    std::vector<LnsWord> x(m), v(m), F(m);
    for (size_t k = 0; k < m; ++k)
    {
        x[k] = fromLiteral(in.pos[k]);
        v[k] = fromLiteral(in.vel[k]);
    }
    const LnsWord zero = kPosZero; // what lzero reads
    const LnsWord two = conv.fromInt32(2);
    const LnsWord half = powerN(two, -1);
    const LnsWord c24 = conv.fromInt32(24), c48 = conv.fromInt32(48), c4 = conv.fromInt32(4);
    const LnsWord dt = fromLiteral(in.dt);
    const LnsWord h = LnsAlu::mul(half, dt);
    auto mul = LnsAlu::mul;
    LnsWord PE = zero;

    auto force = [&](bool pe) {
        for (LnsWord &f : F)
        {
            f = zero;
        }
        PE = add(zero, zero);
        for (size_t i = 0; i < n; ++i)
        {
            const LnsWord xi = x[3 * i], yi = x[3 * i + 1], zi = x[3 * i + 2];
            LnsWord Fx = add(zero, zero), Fy = add(zero, zero), Fz = add(zero, zero);
            for (size_t j = i + 1; j < n; ++j)
            {
                const LnsWord dx = sub(xi, x[3 * j]), dy = sub(yi, x[3 * j + 1]),
                              dz = sub(zi, x[3 * j + 2]);
                LnsWord r2 = mul(dx, dx);
                LnsWord t = mul(dy, dy);
                r2 = add(r2, t);
                t = mul(dz, dz);
                r2 = add(r2, t);
                const LnsWord inv2 = powerN(r2, -1);
                const LnsWord s6 = powerN(r2, -3);
                const LnsWord s12 = mul(s6, s6);
                LnsWord a = mul(c48, s12);
                const LnsWord b = mul(c24, s6);
                a = sub(a, b);
                const LnsWord f = mul(a, inv2);
                const LnsWord fx = mul(f, dx), fy = mul(f, dy), fz = mul(f, dz);
                Fx = add(Fx, fx);
                Fy = add(Fy, fy);
                Fz = add(Fz, fz);
                F[3 * j] = sub(F[3 * j], fx);
                F[3 * j + 1] = sub(F[3 * j + 1], fy);
                F[3 * j + 2] = sub(F[3 * j + 2], fz);
                if (pe)
                {
                    LnsWord u = sub(s12, s6);
                    u = mul(c4, u);
                    PE = add(PE, u);
                }
            }
            F[3 * i] = add(F[3 * i], Fx);
            F[3 * i + 1] = add(F[3 * i + 1], Fy);
            F[3 * i + 2] = add(F[3 * i + 2], Fz);
        }
    };
    auto fmt = [](LnsWord w) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08X", w);
        return std::string(buf);
    };
    std::vector<Record> log;
    auto sample = [&] {
        LnsWord sum = add(zero, zero);
        for (size_t k = 0; k < m; ++k)
        {
            sum = add(sum, mul(v[k], v[k]));
        }
        const LnsWord KE = mul(half, sum);
        const LnsWord E = add(KE, PE);
        Record r{fmt(KE), fmt(PE), fmt(E)};
        for (LnsWord c : x)
        {
            r.push_back(fmt(c));
        }
        log.push_back(r);
    };

    force(true);
    sample();
    long countdown = in.logint;
    for (long s = 0; s < in.steps; ++s)
    {
        for (size_t k = 0; k < m; ++k) // kick
        {
            v[k] = add(v[k], mul(h, F[k]));
        }
        for (size_t k = 0; k < m; ++k) // drift
        {
            x[k] = add(x[k], mul(dt, v[k]));
        }
        const bool pe = --countdown == 0;
        force(pe);
        for (size_t k = 0; k < m; ++k) // kick
        {
            v[k] = add(v[k], mul(h, F[k]));
        }
        if (pe)
        {
            sample();
            countdown = in.logint;
        }
    }
    return log;
}

const LnsAlu *gAlu = nullptr;
LnsWord tableAdd(LnsWord a, LnsWord b) { return gAlu->add(a, b); }
LnsWord tableSub(LnsWord a, LnsWord b) { return gAlu->sub(a, b); }
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc != 3 && argc != 6)
        {
            std::fprintf(stderr, "usage: %s <program.s> fp64|fp32|lns|cordic "
                                 "[log_bits exp_bits alu_samples]\n", argv[0]);
            return 2;
        }
        const Input in = readInput(argv[1]);
        const std::string variant = argv[2];
        const int logBits = argc == 6 ? std::stoi(argv[3]) : LNS_LOG_LUT_BITS;
        const int expBits = argc == 6 ? std::stoi(argv[4]) : LNS_EXP_LUT_BITS;
        const size_t samples = argc == 6 ? std::stoull(argv[5]) : LNS_ALU_CORRECTION_SAMPLES;

        std::vector<Record> log;
        if (variant == "fp64")
        {
            log = runFloat<double>(in, std::stod(in.dt),
                                   [](const std::string &s) { return double(std::stof(s)); });
        }
        else if (variant == "fp32")
        {
            log = runFloat<float>(in, std::stof(in.dt), [](const std::string &s) { return std::stof(s); });
        }
        else if (variant == "lns" || variant == "cordic")
        {
            const LnsConverter conv(logBits, expBits);
            if (variant == "lns")
            {
                static const LnsAlu alu(samples);
                gAlu = &alu;
                log = runLns(in, conv, tableAdd, tableSub);
            }
            else
            {
                log = runLns(in, conv, LnsCordicAddSub::add, LnsCordicAddSub::sub);
            }
        }
        else
        {
            throw std::runtime_error("unknown variant " + variant);
        }
        for (size_t k = 0; k < log.size(); ++k)
        {
            std::printf("%zu %ld", k, static_cast<long>(k) * in.logint);
            for (const std::string &value : log[k])
            {
                std::printf(" %s", value.c_str());
            }
            std::printf("\n");
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
