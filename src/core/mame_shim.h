// The few MAME framework names the V60 core files use, so they compile unchanged.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <string>
#include <type_traits>
#include <cstring>

using u8 = uint8_t;   using s8 = int8_t;
using u16 = uint16_t; using s16 = int16_t;
using u32 = uint32_t; using s32 = int32_t;
using u64 = uint64_t; using s64 = int64_t;
using offs_t = uint32_t;

enum { CLEAR_LINE = 0, ASSERT_LINE = 1, HOLD_LINE = 2 };

// A float to an integer as x86-64 converts it, on every machine: in range, towards zero; out of range or not a
// number, the most negative one (x86-64's "integer indefinite"). C++ leaves those undefined, and an Arm CPU saturates
// instead (and gives 0 for not a number): two machines in a netplay match (a Mac and a PC or a Steam Deck) ran
// different games from there. The TGP's cfxd and the V60's cvt.sw go through these (as VF2's).
inline int32_t f2s32(double v) { return v >= -2147483648.0 && v < 2147483648.0 ? int32_t(v) : INT32_MIN; }
inline int64_t f2s64(double v) { return v >= -9223372036854775808.0 && v < 9223372036854775808.0 ? int64_t(v) : INT64_MIN; }
template <typename T, typename U> constexpr T BIT(T x, U n) noexcept { return (x >> n) & T(1); }
inline int64_t mul_32x32(int32_t a, int32_t b) { return int64_t(a) * int64_t(b); }
inline uint64_t mulu_32x32(uint32_t a, uint32_t b) { return uint64_t(a) * uint64_t(b); }
inline int32_t div_64x32_rem(int64_t a, int32_t b, int32_t &rem) { int32_t q = int32_t(a / b); rem = int32_t(a - int64_t(q) * b); return q; }
inline uint32_t divu_64x32_rem(uint64_t a, uint32_t b, uint32_t &rem) { uint32_t q = uint32_t(a / b); rem = uint32_t(a - uint64_t(q) * b); return q; }
inline int32_t div_64x32_rem(int64_t a, int32_t b, uint32_t &rem) { int32_t r; int32_t q = div_64x32_rem(a, b, r); rem = uint32_t(r); return q; }
inline uint32_t divu_64x32_rem(uint64_t a, uint32_t b, int32_t &rem) { uint32_t r; uint32_t q = divu_64x32_rem(a, b, r); rem = int32_t(r); return q; }
template <typename T, typename U> constexpr T bitswap_one(T val, U b) noexcept { return BIT(val, b); }
template <typename T, typename U, typename... V> constexpr T bitswap(T val, U b, V... c) noexcept
{
	if constexpr (sizeof...(c) > 0) return (BIT(val, b) << sizeof...(c)) | bitswap(val, c...);
	else return BIT(val, b);
}
template <unsigned B, typename T, typename... U> constexpr T bitswap(T val, U... b) noexcept { static_assert(sizeof...(b) == B); return bitswap(val, b...); }
namespace util {
template <typename T, typename U> constexpr std::make_signed_t<T> sext(T value, U width) noexcept
{ return std::make_signed_t<T>(value << (8 * sizeof(value) - width)) >> (8 * sizeof(value) - width); }
}
constexpr uint32_t swapendian_int32(uint32_t v) { return __builtin_bswap32(v); }
// (a NaN stored the same on every machine: x86-64's default, 0xffc00000. ARM64 makes 0x7fc00000 for an invalid
// operation (0/0, inf - inf) and the V60 takes its S flag from the bits: a Mac and a PC online went out of step. Every
// float the V60, the TGP and its glue store comes through here; as VF2's i960 and VF3's PowerPC have it)
inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return (u & 0x7fffffffu) > 0x7f800000u ? 0xffc00000u : u; }
inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

[[noreturn]] void v60_fatal(const char *fmt, ...);
void v60_log(const char *fmt, ...);
#define fatalerror(...) v60_fatal(__VA_ARGS__)
#define logerror(...) v60_log(__VA_ARGS__)
#define osd_printf_debug(...) ((void)0)

// The V60's two address spaces. Model 1 wires a 16-bit data bus, 24-bit addresses.
struct v60_bus
{
	virtual ~v60_bus() = default;
	virtual uint8_t read8(uint32_t a) = 0;
	virtual void write8(uint32_t a, uint8_t v) = 0;
	virtual uint16_t read16(uint32_t a) { return read8(a) | (read8(a + 1) << 8); }
	virtual void write16(uint32_t a, uint16_t v) { write8(a, v); write8(a + 1, v >> 8); }
	virtual uint32_t read32(uint32_t a) { return read16(a) | (uint32_t(read16(a + 2)) << 16); }
	virtual void write32(uint32_t a, uint32_t v) { write16(a, v); write16(a + 2, v >> 16); }
	virtual uint8_t io_read8(uint32_t a) { return 0xff; }
	virtual void io_write8(uint32_t a, uint8_t v) {}
	virtual uint16_t io_read16(uint32_t a) { return 0xffff; }
	virtual void io_write16(uint32_t a, uint16_t v) {}
	virtual uint32_t io_read32(uint32_t a) { return 0xffffffff; }
	virtual void io_write32(uint32_t a, uint32_t v) {}
};

// MAME's address_space calls, forwarded to the bus.
struct v60_space
{
	v60_bus &bus;
	bool io;
	static constexpr uint32_t mask = 0xffffff;
	uint8_t read_byte(uint32_t a) { return io ? bus.io_read8(a & mask) : bus.read8(a & mask); }
	uint16_t read_word_unaligned(uint32_t a) { return io ? bus.io_read16(a & mask) : bus.read16(a & mask); }
	uint32_t read_dword_unaligned(uint32_t a) { return io ? bus.io_read32(a & mask) : bus.read32(a & mask); }
	uint32_t read_dword(uint32_t a) { return read_dword_unaligned(a); }
	uint64_t read_qword_unaligned(uint32_t a) { return read_dword_unaligned(a) | (uint64_t(read_dword_unaligned(a + 4)) << 32); }
	void write_byte(uint32_t a, uint8_t v) { io ? bus.io_write8(a & mask, v) : bus.write8(a & mask, v); }
	void write_word_unaligned(uint32_t a, uint16_t v) { io ? bus.io_write16(a & mask, v) : bus.write16(a & mask, v); }
	void write_dword_unaligned(uint32_t a, uint32_t v) { io ? bus.io_write32(a & mask, v) : bus.write32(a & mask, v); }
	void write_qword_unaligned(uint32_t a, uint64_t v) { write_dword_unaligned(a, uint32_t(v)); write_dword_unaligned(a + 4, uint32_t(v >> 32)); }
};
