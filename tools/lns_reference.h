/**
 * @file lns_reference.h
 * @brief Host-math (log2/exp2) reference converters. NOT part of the simulator build;
 *        used by tools/lns_conversion_check.cpp as the accuracy reference for the LUT converters.
 *
 * Same rules as LnsConverter: FP/int zero -> +0, NaN -> NaN, Inf -> +/-Inf, log rounded to
 * nearest in Q9.22 (ties away from zero), LNS -> FP via host round-to-nearest, LNS -> int via
 * nearest-even with RISC-V style saturation.
 */
#ifndef LNS_REFERENCE_H
#define LNS_REFERENCE_H

#include "lns_format.h"

#include <cstdint>

namespace Kites
{
namespace lns
{
namespace reference
{
LnsWord fromFloatBits(uint32_t bits);
LnsWord fromDoubleBits(uint64_t bits);
LnsWord fromInt32(int32_t v);
LnsWord fromUInt32(uint32_t v);
LnsWord fromInt64(int64_t v);
LnsWord fromUInt64(uint64_t v);

uint32_t toFloatBits(LnsWord w);
uint64_t toDoubleBits(LnsWord w);
int32_t toInt32(LnsWord w);
uint32_t toUInt32(LnsWord w);
int64_t toInt64(LnsWord w);
uint64_t toUInt64(LnsWord w);
} // namespace reference
} // namespace lns
} // namespace Kites

#endif // LNS_REFERENCE_H
