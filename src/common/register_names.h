#pragma once
#include <array>

namespace Kites
{
namespace register_names
{

// x0-x31 canonical names.
inline constexpr std::array<const char *, 32> kGprNames = {
    "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10",
    "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21",
    "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30", "x31"};

// x0-x31 ABI aliases, index-matched with kGprNames.
inline constexpr std::array<const char *, 32> kGprAliases = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2",
    "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10",
    "s11", "t3", "t4", "t5", "t6"};

// f0-f31 canonical names.
inline constexpr std::array<const char *, 32> kFprNames = {
    "f0",  "f1",  "f2",  "f3",  "f4",  "f5",  "f6",  "f7",  "f8",  "f9",  "f10",
    "f11", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21",
    "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31"};

// f0-f31 ABI aliases, index-matched with kFprNames.
inline constexpr std::array<const char *, 32> kFprAliases = {
    "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "fs0", "fs1", "fa0", "fa1",
    "fa2", "fa3", "fa4", "fa5", "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
    "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"};

// l0-l31 canonical names (LNS registers).
inline constexpr std::array<const char *, 32> kLnsNames = {
    "l0",  "l1",  "l2",  "l3",  "l4",  "l5",  "l6",  "l7",  "l8",  "l9",  "l10",
    "l11", "l12", "l13", "l14", "l15", "l16", "l17", "l18", "l19", "l20", "l21",
    "l22", "l23", "l24", "l25", "l26", "l27", "l28", "l29", "l30", "l31"};

// l0-l31 ABI aliases, index-matched with kLnsNames.
inline constexpr std::array<const char *, 32> kLnsAliases = {
    "lzero", "lt0", "lt1", "lt2", "lt3", "lt4", "lt5", "lt6", "ls0", "ls1", "la0",
    "la1",   "la2", "la3", "la4", "la5", "la6", "la7", "ls2", "ls3", "ls4", "ls5",
    "ls6",   "ls7", "ls8", "ls9", "ls10", "ls11", "lt7", "lt8", "lt9", "lt10"};

} // namespace register_names
} // namespace Kites
