// Plain-C++ model of the Lennard-Jones benchmark (examples/lns_lj): accuracy vs LNS table size.
//
// Runs the same pair kernel as the generated programs (same op order) with LnsAlu, LnsConverter
// and LnsCordicAddSub directly, sweeping the converter LUT bits and the ALU correction samples,
// plus the CORDIC add/sub variant. Prints per config the table memory, the final LNS word of the
// energy, and its relative error against the FP64 reference and against the FP32 control.
// The final words at the default sizes must match l10 from kites-cli bit for bit.
//
// Build (from the repo root, one command line):
//   g++ -std=c++20 -O2 -I src/processor/lns tools/lns_lj_accuracy.cpp src/processor/lns/lns_alu.cpp
//       src/processor/lns/lns_conversion.cpp src/processor/lns/lns_cordic_addsub.cpp
//       src/processor/lns/lns_pow.cpp -o lns_lj_accuracy
// Run:  ./lns_lj_accuracy [examples/lns_lj/lj_fp32.s] [examples/lns_lj/reference.txt]
//   Positions are read from the .float lines of the program, so they match the simulator input.
//       ./lns_lj_accuracy --size <log bits> <exp bits> <ALU samples>
//   prints "<converter bytes> <ALU table bytes>" for one configuration (used by sweep.sh).
#include "lns_alu.h"
#include "lns_conversion.h"
#include "lns_cordic_addsub.h"
#include "lns_pow.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Kites::lns;

namespace
{
using Atom = std::array<float, 3>;

/// Reads every value of the ".float" directives in @p path (std::stof, as the assembler does).
std::vector<Atom> readPositions(const std::string &path)
{
    std::ifstream in(path);
    if (!in)
    {
        throw std::runtime_error("cannot open " + path);
    }
    std::vector<float> values;
    std::string line;
    while (std::getline(in, line))
    {
        const auto at = line.find(".float");
        if (at == std::string::npos)
        {
            continue;
        }
        std::string list = line.substr(at + 6);
        for (char &ch : list)
        {
            if (ch == ',')
            {
                ch = ' ';
            }
        }
        std::istringstream fields(list);
        std::string token;
        while (fields >> token)
        {
            values.push_back(std::stof(token));
        }
    }
    if (values.empty() || values.size() % 3 != 0)
    {
        throw std::runtime_error("expected 3 floats per atom in " + path);
    }
    std::vector<Atom> atoms(values.size() / 3);
    for (size_t k = 0; k < values.size(); ++k)
    {
        atoms[k / 3][k % 3] = values[k];
    }
    return atoms;
}

double readReference(const std::string &path)
{
    std::ifstream in(path);
    double value = 0.0;
    if (!(in >> value))
    {
        throw std::runtime_error("cannot read the reference energy from " + path);
    }
    return value;
}

/// Exact value of an LNS word (no table): (-1)^S * 2^(field / 2^22).
double lnsValue(LnsWord w)
{
    if (isZero(w))
    {
        return 0.0;
    }
    if (isNaN(w))
    {
        return std::nan("");
    }
    const double magnitude = isInf(w) ? INFINITY : std::exp2(fieldOf(w) / kScale);
    return signOf(w) ? -magnitude : magnitude;
}

/// FP32 control: the same kernel and op order as lj_fp32.s.
float energyFp32(const std::vector<Atom> &pos)
{
    float energy = 0.0f;
    for (size_t i = 0; i < pos.size(); ++i)
    {
        for (size_t j = i + 1; j < pos.size(); ++j)
        {
            const float dx = pos[i][0] - pos[j][0];
            const float dy = pos[i][1] - pos[j][1];
            const float dz = pos[i][2] - pos[j][2];
            float r2 = dx * dx;
            r2 = r2 + dy * dy;
            r2 = r2 + dz * dz;
            const float inv = 1.0f / r2;
            float s6 = inv * inv;
            s6 = s6 * inv;
            const float s12 = s6 * s6;
            const float v = 4.0f * (s12 - s6);
            energy = energy + v;
        }
    }
    return energy;
}

/// LNS kernel, same op order as lj_lns.s / lj_lns_cordic.s (s6 = r2^-3 with lpown).
template <typename Add, typename Sub>
LnsWord energyLns(const std::vector<Atom> &pos, const LnsConverter &conv, Add add, Sub sub)
{
    std::vector<std::array<LnsWord, 3>> l(pos.size());
    for (size_t i = 0; i < pos.size(); ++i)
    {
        for (int k = 0; k < 3; ++k)
        {
            l[i][k] = conv.fromFloatBits(std::bit_cast<uint32_t>(pos[i][k]));
        }
    }
    const LnsWord four = conv.fromInt32(4); // fcvt.lns.w

    LnsWord energy = add(kPosZero, kPosZero);
    for (size_t i = 0; i < pos.size(); ++i)
    {
        for (size_t j = i + 1; j < pos.size(); ++j)
        {
            const LnsWord dx = sub(l[i][0], l[j][0]);
            const LnsWord dy = sub(l[i][1], l[j][1]);
            const LnsWord dz = sub(l[i][2], l[j][2]);
            LnsWord r2 = add(LnsAlu::mul(dx, dx), LnsAlu::mul(dy, dy));
            r2 = add(r2, LnsAlu::mul(dz, dz));
            const LnsWord s6 = powerN(r2, -3);
            const LnsWord s12 = LnsAlu::mul(s6, s6);
            const LnsWord v = LnsAlu::mul(four, sub(s12, s6));
            energy = add(energy, v);
        }
    }
    return energy;
}

double relErr(double value, double reference)
{
    return std::fabs(value - reference) / std::fabs(reference);
}

struct Context
{
    std::vector<Atom> pos;
    double reference;
    float fp32;
};

void printHeader()
{
    std::printf("%-26s %10s %10s %10s  %-10s  %-14s %-11s %-11s %-11s\n", "config", "conv KiB",
                "alu KiB", "total KiB", "l10 word", "energy", "rel err/f64", "rel err/f32",
                "f11 rel/f64");
}

void printRow(const Context &ctx, const char *name, const LnsConverter &conv, size_t aluBytes,
              LnsWord word)
{
    const double convKiB = (conv.logLutBytes() + conv.expLutBytes()) / 1024.0;
    const double aluKiB = aluBytes / 1024.0;
    const double value = lnsValue(word);
    const double f11 = std::bit_cast<float>(conv.toFloatBits(word));
    std::printf("%-26s %10.2f %10.2f %10.2f  0x%08X  %-14.8g %-11.3e %-11.3e %-11.3e\n", name,
                convKiB, aluKiB, convKiB + aluKiB, word, value, relErr(value, ctx.reference),
                relErr(value, ctx.fp32), relErr(f11, ctx.reference));
}

void runTable(const Context &ctx, const char *name, int convBits, size_t samples)
{
    const LnsConverter conv(convBits, std::min(convBits, kMaxExpLutBits));
    const LnsAlu alu(samples);
    const LnsWord word = energyLns(
        ctx.pos, conv, [&](LnsWord a, LnsWord b) { return alu.add(a, b); },
        [&](LnsWord a, LnsWord b) { return alu.sub(a, b); });
    printRow(ctx, name, conv, alu.correctionBytes(), word);
}

void runCordic(const Context &ctx, const char *name, int convBits)
{
    const LnsConverter conv(convBits, std::min(convBits, kMaxExpLutBits));
    const LnsWord word = energyLns(ctx.pos, conv, LnsCordicAddSub::add, LnsCordicAddSub::sub);
    printRow(ctx, name, conv, 0, word);
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 5 && std::string(argv[1]) == "--size")
        {
            // Table bytes of one configuration, for sweep.sh: log bits, exp bits, ALU samples.
            const LnsConverter conv(std::stoi(argv[2]), std::stoi(argv[3]));
            const LnsAlu alu(std::stoull(argv[4]));
            std::printf("%zu %zu\n", conv.logLutBytes() + conv.expLutBytes(), alu.correctionBytes());
            return 0;
        }
        const std::string program = argc > 1 ? argv[1] : "examples/lns_lj/lj_fp32.s";
        const std::string reference = argc > 2 ? argv[2] : "examples/lns_lj/reference.txt";
        Context ctx{readPositions(program), readReference(reference), 0.0f};
        ctx.fp32 = energyFp32(ctx.pos);

        std::printf("Lennard-Jones energy, N=%zu atoms, %zu pairs\n", ctx.pos.size(),
                    ctx.pos.size() * (ctx.pos.size() - 1) / 2);
        std::printf("FP64 reference : %.17g\n", ctx.reference);
        std::printf("FP32 control   : %.9g (0x%08X), rel err %.3e\n\n", ctx.fp32,
                    std::bit_cast<uint32_t>(ctx.fp32), relErr(ctx.fp32, ctx.reference));
        std::printf("Converter bits k: log LUT = k, exp LUT = min(k, 22). 'f11' = energy after "
                    "fcvt.s.lns.\n\n");

        const size_t defaultSamples = LNS_ALU_CORRECTION_SAMPLES;
        const int defaultBits = LNS_LOG_LUT_BITS;
        static_assert(LNS_LOG_LUT_BITS == LNS_EXP_LUT_BITS, "the sweep assumes equal defaults");
        char name[64];

        std::printf("-- converter sweep (ALU samples %zu) --\n", defaultSamples);
        printHeader();
        for (int bits : {6, 7, 8, 9, 10, 11, 12, 13, 16, 23})
        {
            std::snprintf(name, sizeof name, "conv %d/%d%s", bits, std::min(bits, kMaxExpLutBits),
                          bits == defaultBits ? " (default)" : "");
            runTable(ctx, name, bits, defaultSamples);
        }

        std::printf("\n-- ALU correction-samples sweep (converter %d/%d) --\n", defaultBits,
                    defaultBits);
        printHeader();
        for (size_t samples = 256; samples <= 65536; samples *= 2)
        {
            std::snprintf(name, sizeof name, "alu %zu%s", samples,
                          samples == defaultSamples ? " (default)" : "");
            runTable(ctx, name, defaultBits, samples);
        }

        std::printf("\n-- CORDIC add/sub (no ALU table) --\n");
        printHeader();
        std::snprintf(name, sizeof name, "cordic, conv %d/%d", defaultBits, defaultBits);
        runCordic(ctx, name, defaultBits);
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
