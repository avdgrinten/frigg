#ifndef FRG_HASH_HPP
#define FRG_HASH_HPP

#include <stdint.h>
#include <concepts>
#include <frg/macros.hpp>

namespace frg FRG_VISIBILITY {

namespace bit_mixers {

// Chris Wellons' lowbias32() function.
// Source: https://nullprogram.com/blog/2018/07/31/
constexpr uint32_t lowbias32(uint32_t v) {
	v ^= v >> 16;
	v *= 0x7feb352d;
	v ^= v >> 15;
	v *= 0x846ca68b;
	v ^= v >> 16;
	return v;
}

// Pelle Evensen's moremur() function.
// Source: https://mostlymangling.blogspot.com/2019/12/stronger-better-morer-moremur-better.html
constexpr uint64_t moremur(uint64_t v) {
	v ^= v >> 27;
	v *= 0x3c79ac492ba7b653;
	v ^= v >> 33;
	v *= 0x1c69b3f74ac4ae35;
	v ^= v >> 27;
	return v;
}

} // namespace bit_mixers

// Mixes the bits of an integer such that each output bit depends on many of the input bits.
template<typename T>
requires std::unsigned_integral<T>
constexpr T mix_bits(T v) {
	if constexpr (sizeof(T) == 4) {
		return bit_mixers::lowbias32(v);
	} else {
		static_assert(sizeof(T) == 8);
		return bit_mixers::moremur(v);
	}
}

// Apply mix_bits(), then narrow to 32-bits.
template<typename T>
requires std::unsigned_integral<T>
constexpr uint32_t mix_bits_narrow32(T v) {
	if constexpr (sizeof(T) == 4) {
		return mix_bits(v);
	} else {
		static_assert(sizeof(T) == 8);
		return static_cast<uint32_t>(mix_bits(v) >> 32);
	}
}

template<typename T>
class hash;

// Unsigned types.

template<>
class hash<unsigned int> {
public:
	constexpr unsigned int operator() (unsigned int v) const {
		return mix_bits_narrow32(v);
	}
};

template<>
class hash<unsigned long> {
public:
	constexpr unsigned int operator() (unsigned long v) const {
		return mix_bits_narrow32(v);
	}
};

// Signed types.

template<>
class hash<int> {
public:
	constexpr unsigned int operator() (int v) const {
		return mix_bits_narrow32(static_cast<unsigned int>(v));
	}
};

template<>
class hash<long> {
public:
	constexpr unsigned int operator() (long v) const {
		return mix_bits_narrow32(static_cast<unsigned long>(v));
	}
};

// Other types.

template<typename T>
class hash<T *> {
public:
	constexpr unsigned int operator() (T *p) const {
		auto v = reinterpret_cast<uintptr_t>(p);
		return mix_bits_narrow32(v);
	}
};

class CStringHash {
public:
	constexpr unsigned int operator() (const char *str) const {
		unsigned int value = 0;
		while(*str != 0) {
			value = (value << 8) | (value >> 24);
			value += *str++;
		}
		return value;
	}
};

} // namespace frg

#endif // FRG_HASH_HPP
