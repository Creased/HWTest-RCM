/*
 * hwtest RAM-build compatibility layer for Atmosphere's public Mariko MTC.
 *
 * The MTC implementation and Tegra register definitions remain GPLv2
 * Atmosphere-NX material.  This header only supplies the small subset of
 * vapours/exosphere primitives that the standalone BPMP diagnostic needs.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <algorithm>

using u8  = uint8_t;
using s8  = int8_t;
using u16 = uint16_t;
using s16 = int16_t;
using u32 = uint32_t;
using s32 = int32_t;
using u64 = uint64_t;
using s64 = int64_t;
extern "C" void usleep(u32 us);

#ifndef BITSIZEOF
#define BITSIZEOF(t) (sizeof(t) * 8u)
#endif
#ifndef ALWAYS_INLINE
#define ALWAYS_INLINE inline __attribute__((always_inline))
#endif
#ifndef NORETURN
#define NORETURN __attribute__((noreturn))
#endif
#ifndef AMS_ABORT_UNLESS
#define AMS_ABORT_UNLESS(cond) do { if (!(cond)) __builtin_trap(); } while (0)
#endif
#ifndef AMS_UNUSED
#define AMS_UNUSED(...) ((void)(__VA_ARGS__))
#endif

namespace ams {
namespace reg {

struct BitsMask {
    u32 offset;
    u32 width;
};
struct BitsValue {
    u32 offset;
    u32 width;
    u32 value;
};

constexpr ALWAYS_INLINE u32 EncodeMask(BitsMask v) {
    return v.width >= 32 ? 0xFFFFFFFFu : (((1u << v.width) - 1u) << v.offset);
}
constexpr ALWAYS_INLINE u32 EncodeMask(BitsValue v) {
    return v.width >= 32 ? 0xFFFFFFFFu : (((1u << v.width) - 1u) << v.offset);
}
constexpr ALWAYS_INLINE u32 EncodeValue(BitsValue v) {
    return (v.value << v.offset) & EncodeMask(v);
}
constexpr ALWAYS_INLINE u32 GetField(u32 value, BitsMask mask) {
    return (value & EncodeMask(mask)) >> mask.offset;
}
ALWAYS_INLINE void SetField(u32 &value, BitsValue v) {
    value = (value & ~EncodeMask(v)) | EncodeValue(v);
}
ALWAYS_INLINE u32 Read(uintptr_t address) {
    return *reinterpret_cast<volatile u32 *>(address);
}
ALWAYS_INLINE void Write(uintptr_t address, u32 value) {
    *reinterpret_cast<volatile u32 *>(address) = value;
}
template<typename... Values>
ALWAYS_INLINE void Write(uintptr_t address, BitsValue first, Values... rest) {
    u32 value = EncodeValue(first);
    ((value |= EncodeValue(rest)), ...);
    Write(address, value);
}
ALWAYS_INLINE u32 GetValue(uintptr_t address, BitsMask mask) {
    return GetField(Read(address), mask);
}
template<typename... Values>
ALWAYS_INLINE bool HasValue(uintptr_t address, BitsValue first, Values... rest) {
    u32 mask = EncodeMask(first);
    u32 want = EncodeValue(first);
    ((mask |= EncodeMask(rest), want |= EncodeValue(rest)), ...);
    return (Read(address) & mask) == want;
}
ALWAYS_INLINE void SetBits(uintptr_t address, u32 mask) {
    Write(address, Read(address) | mask);
}
ALWAYS_INLINE void ClearBits(uintptr_t address, u32 mask) {
    Write(address, Read(address) & ~mask);
}
ALWAYS_INLINE void ReadWrite(uintptr_t address, u32 value, u32 mask) {
    Write(address, (Read(address) & ~mask) | (value & mask));
}
ALWAYS_INLINE void ReadWrite(uintptr_t address, BitsValue v) {
    ReadWrite(address, EncodeValue(v), EncodeMask(v));
}

} // namespace reg

namespace util {
template<typename T, size_t N>
constexpr ALWAYS_INLINE size_t size(const T (&)[N]) { return N; }
constexpr ALWAYS_INLINE u32 DivideUp(u32 a, u32 b) {
    return (a + b - 1u) / b;
}
ALWAYS_INLINE void WaitMicroSeconds(u32 us) {
    ::usleep(us);
}
} // namespace util

namespace secmon {
struct FixedRegion {
    uintptr_t address;
    constexpr uintptr_t GetAddress() const { return address; }
};
constexpr inline FixedRegion MemoryRegionPhysicalDeviceClkRst{0x60006000u};
} // namespace secmon

namespace nxboot {}
} // namespace ams

#define REG_BITS_MASK(OFFSET, WIDTH)         ::ams::reg::BitsMask{OFFSET, WIDTH}
#define REG_BITS_VALUE(OFFSET, WIDTH, VALUE) ::ams::reg::BitsValue{OFFSET, WIDTH, VALUE}
#define REG_NAMED_BITS_MASK(PREFIX, NAME)         REG_BITS_MASK(PREFIX##_##NAME##_OFFSET, PREFIX##_##NAME##_WIDTH)
#define REG_NAMED_BITS_VALUE(PREFIX, NAME, VALUE) REG_BITS_VALUE(PREFIX##_##NAME##_OFFSET, PREFIX##_##NAME##_WIDTH, VALUE)
#define REG_NAMED_BITS_ENUM(PREFIX, NAME, ENUM)   REG_BITS_VALUE(PREFIX##_##NAME##_OFFSET, PREFIX##_##NAME##_WIDTH, PREFIX##_##NAME##_##ENUM)
#define REG_NAMED_BITS_ENUM_SEL(PREFIX, NAME, COND, TRUE_ENUM, FALSE_ENUM) \
    REG_BITS_VALUE(PREFIX##_##NAME##_OFFSET, PREFIX##_##NAME##_WIDTH, (COND) ? PREFIX##_##NAME##_##TRUE_ENUM : PREFIX##_##NAME##_##FALSE_ENUM)

#define REG_DEFINE_NAMED_REG(PREFIX, NAME, OFFSET, WIDTH) \
    constexpr inline u32 PREFIX##_##NAME##_OFFSET = OFFSET; \
    constexpr inline u32 PREFIX##_##NAME##_WIDTH  = WIDTH

#define REG_DEFINE_NAMED_BIT_ENUM(PREFIX, NAME, OFFSET, ZERO, ONE) \
    REG_DEFINE_NAMED_REG(PREFIX, NAME, OFFSET, 1); \
    enum PREFIX##_##NAME { PREFIX##_##NAME##_##ZERO = 0, PREFIX##_##NAME##_##ONE = 1 }

#define REG_DEFINE_NAMED_TWO_BIT_ENUM(PREFIX, NAME, OFFSET, ZERO, ONE, TWO, THREE) \
    REG_DEFINE_NAMED_REG(PREFIX, NAME, OFFSET, 2); \
    enum PREFIX##_##NAME { PREFIX##_##NAME##_##ZERO = 0, PREFIX##_##NAME##_##ONE = 1, PREFIX##_##NAME##_##TWO = 2, PREFIX##_##NAME##_##THREE = 3 }

#define REG_DEFINE_NAMED_THREE_BIT_ENUM(PREFIX, NAME, OFFSET, ZERO, ONE, TWO, THREE, FOUR, FIVE, SIX, SEVEN) \
    REG_DEFINE_NAMED_REG(PREFIX, NAME, OFFSET, 3); \
    enum PREFIX##_##NAME { PREFIX##_##NAME##_##ZERO = 0, PREFIX##_##NAME##_##ONE = 1, PREFIX##_##NAME##_##TWO = 2, PREFIX##_##NAME##_##THREE = 3, PREFIX##_##NAME##_##FOUR = 4, PREFIX##_##NAME##_##FIVE = 5, PREFIX##_##NAME##_##SIX = 6, PREFIX##_##NAME##_##SEVEN = 7 }

#define REG_DEFINE_NAMED_FOUR_BIT_ENUM(PREFIX, NAME, OFFSET, ZERO, ONE, TWO, THREE, FOUR, FIVE, SIX, SEVEN, EIGHT, NINE, TEN, ELEVEN, TWELVE, THIRTEEN, FOURTEEN, FIFTEEN) \
    REG_DEFINE_NAMED_REG(PREFIX, NAME, OFFSET, 4); \
    enum PREFIX##_##NAME { PREFIX##_##NAME##_##ZERO = 0, PREFIX##_##NAME##_##ONE = 1, PREFIX##_##NAME##_##TWO = 2, PREFIX##_##NAME##_##THREE = 3, PREFIX##_##NAME##_##FOUR = 4, PREFIX##_##NAME##_##FIVE = 5, PREFIX##_##NAME##_##SIX = 6, PREFIX##_##NAME##_##SEVEN = 7, PREFIX##_##NAME##_##EIGHT = 8, PREFIX##_##NAME##_##NINE = 9, PREFIX##_##NAME##_##TEN = 10, PREFIX##_##NAME##_##ELEVEN = 11, PREFIX##_##NAME##_##TWELVE = 12, PREFIX##_##NAME##_##THIRTEEN = 13, PREFIX##_##NAME##_##FOURTEEN = 14, PREFIX##_##NAME##_##FIFTEEN = 15 }

#include "tegra_clkrst.hpp"
#include "tegra_mc.hpp"
#include "tegra_emc.hpp"
