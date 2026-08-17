#ifndef FRG_FORMATTING_HPP
#define FRG_FORMATTING_HPP

#include <stdarg.h>
#include <cstddef>
#include <limits.h>
#include <stdint.h>
#include <frg/macros.hpp>
#include <frg/optional.hpp>
#include <frg/string.hpp>
#include <frg/utility.hpp>
#include <frg/tuple.hpp>
#include <frg/string_stub.hpp>

/* the ranges library is not even partially freestanding for some reason */
#if __STDC_HOSTED__ && (!defined(__clang__) || (defined(__clang_major__) && __clang_major__ > 17))
#  if __has_include(<ranges>) && __has_include(<algorithm>)
#    include <ranges>
#    include <algorithm>
#    define FRG_HAS_RANGES
#  endif
#endif

#if __STDC_HOSTED__ || defined(FRG_HAVE_LIBC)
#include <math.h>
#include <wchar.h>
#include <limits>
#endif

namespace frg FRG_VISIBILITY {

// Concept: Sink.
// Supports a single operation: append().
// append() is overloaded for a variety of types.

template<typename T, typename Char = char>
concept SinkFor = requires (T t, const Char *str, Char c, size_t n) {
	t.append(str, n);
	t.append(str);
	t.append(c);
};

template<typename T>
concept Sink = SinkFor<T, typename T::char_type>;

template <typename CharT>
struct FormatterPolicy;

template<>
struct FormatterPolicy<char> {
	static constexpr const char *emptyStr = "";
	static constexpr const char *dotStr = ".";
	static constexpr const char *binPrefix = "0b";
	static constexpr const char *binPrefixUpper = "0B";
	static constexpr const char *hexPrefix = "0x";
	static constexpr const char *hexPrefixUpper = "0X";
	static constexpr const char *inf = "inf";
	static constexpr const char *infUpper = "INF";
	static constexpr const char *nan = "nan";
	static constexpr const char *nanUpper = "NAN";
	static constexpr const char *hexPrefixZero = "0x0";
	static constexpr const char *hexPrefixUpperZero = "0X0";
	static constexpr const char *hexPrefixOne = "0x1";
	static constexpr const char *hexPrefixUpperOne = "0X1";
};

template<>
struct FormatterPolicy<wchar_t> {
	static constexpr const wchar_t *emptyStr = L"";
	static constexpr const wchar_t *dotStr = L".";
	static constexpr const wchar_t *binPrefix = L"0b";
	static constexpr const wchar_t *binPrefixUpper = L"0B";
	static constexpr const wchar_t *hexPrefix = L"0x";
	static constexpr const wchar_t *hexPrefixUpper = L"0X";
	static constexpr const wchar_t *inf = L"inf";
	static constexpr const wchar_t *infUpper = L"INF";
	static constexpr const wchar_t *nan = L"nan";
	static constexpr const wchar_t *nanUpper = L"NAN";
	static constexpr const wchar_t *hexPrefixZero = L"0x0";
	static constexpr const wchar_t *hexPrefixUpperZero = L"0X0";
	static constexpr const wchar_t *hexPrefixOne = L"0x1";
	static constexpr const wchar_t *hexPrefixUpperOne = L"0X1";
};

// ----------------------------------------------------------------------------
// General formatting machinery.
// ----------------------------------------------------------------------------

enum class format_conversion {
	null,
	character,
	binary,
	octal,
	decimal,
	hex
};

struct format_options {
	format_options()
	: conversion{format_conversion::null} { }

	format_options with_conversion(format_conversion c) {
		auto copy = *this;
		copy.conversion = c;
		return copy;
	}

	format_conversion conversion;
	int minimum_width = 0;
	int arg_pos = -1;
	bool dollar_arg_pos = false;
	optional<int> precision;
	bool left_justify = false;
	bool always_sign = false;
	bool plus_becomes_space = false;
	bool alt_conversion = false;
	bool fill_zeros = false;
	bool group_thousands = false;
	bool use_capitals = false;
	bool use_compact = false;
	bool use_exponential = false;
};

template <typename Char>
struct locale_options {
	using Policy = FormatterPolicy<Char>;

	locale_options()
	: decimal_point(Policy::dotStr), thousands_sep(Policy::emptyStr), grouping("") { }

	locale_options(const Char *d_p, const Char *t_s, const char *grp)
	: decimal_point(d_p), thousands_sep(t_s), grouping(grp) {
		thousands_sep_size = generic_strlen(thousands_sep);
	}

	const Char *decimal_point;
	const Char *thousands_sep;
	const char *grouping;
	size_t thousands_sep_size;
};

enum class format_error {
	success,
	agent_error,
	invalid_format,
	too_many_args,
	positional_args_gap,
};

// ----------------------------------------------------------------------------
// Formatting primitives for built-in types.
// ----------------------------------------------------------------------------

namespace _fmt_basics {
	// width: Minimum width of the output (padded with spaces by default).
	// precision: Minimum number of digits in the output (always padded with zeros).
	template<Sink S, typename T, typename Char>
	void print_digits(S &sink, T number, bool negative, int radix,
			int width, int precision, char padding, bool left_justify,
			bool group_thousands, bool always_sign, bool plus_becomes_space,
			bool use_capitals, locale_options<Char> locale_opts) {
		const char *digits = use_capitals ? "0123456789ABCDEF" : "0123456789abcdef";
		char buffer[64];

		if (!locale_opts.grouping)
			locale_opts.grouping = "";

		int k = 0; // number of digits
		int c = 0; // number of chars since last grouping
		int g = 0; // grouping index
		int r = 0; // amount of times we repeated the last grouping
		size_t extra = 0; // extra chars printed due to seperator

		auto step_grouping = [&] () {
			if (!group_thousands)
				return;

			if (++c == locale_opts.grouping[g]) {
				if (locale_opts.grouping[g + 1] != 0 && locale_opts.grouping[g + 1] != CHAR_MAX)
					g++;
				else
					r++;
				c = 0;
				extra += locale_opts.thousands_sep_size;
			}
		};

		auto emit_grouping = [&] () {
			if (!group_thousands)
				return;

			if (--c == 0) {
				sink.append(locale_opts.thousands_sep);
				if (!r || !--r)
					g--;
				c = locale_opts.grouping[g];
			}
		};

		// print the number in reverse order and determine #digits.
		do {
			FRG_ASSERT(k < 64); // TODO: variable number of digits
			buffer[k++] = digits[number % radix];
			number /= radix;
			step_grouping();
		} while(number);

		if (k < precision)
			for (int i = 0; i < precision - k; i++)
				step_grouping();

		if (!c) {
			c = locale_opts.grouping[g];
			if (group_thousands)
				extra--;
		}

		if(negative || always_sign || plus_becomes_space)
			extra++;

		int final_width = max(k, precision) + extra;

		if(!left_justify && final_width < width && padding != '0')
			for(int i = 0; i < width - final_width; i++)
				sink.append(padding);

		if(negative)
			sink.append('-');
		else if(always_sign)
			sink.append('+');
		else if(plus_becomes_space)
			sink.append(' ');

		if(!left_justify && final_width < width && padding == '0')
			for(int i = 0; i < width - final_width; i++)
				sink.append(padding);

		if(k < precision) {
			for(int i = 0; i < precision - k; i++) {
				sink.append('0');
				emit_grouping();
			}
		}

		for(int i = k - 1; i >= 0; i--) {
			sink.append(buffer[i]);
			if (i)
				emit_grouping();
		}

		if(left_justify && final_width < width)
			for(int i = final_width; i < width; i++)
				sink.append(padding);
	}

	// Signed integer formatting. We cannot print -x as that might not fit into the signed type.
	// Strategy: Cast to unsigned first (i.e. obtain 2s complement) and negate manually by
	// computing (~x + 1).
	template<Sink S, typename T, typename Char>
	void print_int(S &sink, T number, int radix, int width = 0,
			int precision = 1, char padding = ' ', bool left_justify = false,
			bool group_thousands = false, bool always_sign = false,
			bool plus_becomes_space = false, bool use_capitals = false,
			locale_options<Char> locale_opts = {}) {
		if(number < 0) {
			// This is valid in C (N3220 6.2.6.2) and C++ ([N4950 basic.fundamental 6.8.2.3])
			using UnsignedT = std::make_unsigned_t<T>;
			auto absv = ~static_cast<UnsignedT>(number) + 1;
			print_digits<S, UnsignedT, Char>(sink, absv, true, radix, width, precision, padding,
					left_justify, group_thousands, always_sign, plus_becomes_space, use_capitals,
					locale_opts);
		}else{
			print_digits<S, T, Char>(sink, number, false, radix, width, precision, padding,
					left_justify, group_thousands, always_sign, plus_becomes_space, use_capitals,
					locale_opts);
		}
	}

	template<typename T, Sink S>
	void format_integer(T object, format_options fo, S &sink) {
		int radix = 10;
		if(fo.conversion == format_conversion::hex) {
			radix = 16;
		}else if(fo.conversion == format_conversion::octal) {
			radix = 8;
		}else if(fo.conversion == format_conversion::binary) {
			radix = 2;
		}else{
			FRG_ASSERT(fo.conversion == format_conversion::null
					|| fo.conversion == format_conversion::decimal);
		}

		print_int<S, T, typename S::char_type>(sink, object, radix,
				fo.minimum_width, fo.precision ? *fo.precision : 1,
				fo.fill_zeros ? '0' : ' ', fo.left_justify, fo.group_thousands,
				fo.always_sign, fo.plus_becomes_space, fo.use_capitals);
	}

	template<typename T, typename Char>
	int grouping_extra_characters(T num, int precision, locale_options<Char> opts) {
		if (!opts.grouping)
			opts.grouping = "";

		int digits = 0; // number of digits
		int chars_since_last_grouping = 0; // number of chars since last grouping
		int group_index = 0; // grouping index into the grouping rule (opts.grouping)
		int extra = 0; // extra chars printed due to seperator

		auto step_grouping = [&] () {
			if (++chars_since_last_grouping == opts.grouping[group_index]) {
				if (opts.grouping[group_index + 1] != 0 && opts.grouping[group_index + 1] != CHAR_MAX)
					group_index++;
				chars_since_last_grouping = 0;
				extra += opts.thousands_sep_size;
			}
		};

		// print the number in reverse order and determine #digits.
		do {
			FRG_ASSERT(digits < 64); // TODO: variable number of digits
			digits++;
			num /= 10;
			step_grouping();
		} while(num);

		if (digits < precision)
			for (int i = 0; i < precision - digits; i++)
				step_grouping();

		if (!chars_since_last_grouping) {
			chars_since_last_grouping = opts.grouping[group_index];
			extra--;
		}

		return extra;
	}

#if __STDC_HOSTED__ || defined(FRG_HAVE_LIBC)
	template<Sink S, typename T, typename Char>
		requires (std::is_floating_point_v<T>)
	void print_float(S &sink, T number, int width = 0, optional<int> precision = 6,
			Char padding = ' ', bool left_justify = false, bool alt_conversion = false,
			bool use_capitals = false, bool group_thousands = false, bool use_compact = false,
			bool exponential_form = false, bool print_hexfloat = false, locale_options<Char> locale_opts = {}) {
		using P = frg::FormatterPolicy<Char>;

		auto textLength = [](int i, int base = 10, bool ignoreSign = false) {
			int length = (i < 0 && !ignoreSign) ? 1 : 0;
			do {
				i /= base;
				length++;
			} while (i != 0);
			return length;
		};

		bool has_sign = false;
		if (__builtin_signbit(number)) {
			has_sign = true;
		}

		bool inf = __builtin_isinf(number), nan = __builtin_isnan(number);
		if (inf || nan) {
			auto total_length = 3 + has_sign;
			auto pad_length = width > total_length ? width - total_length : 0;
			if (!left_justify) {
				while (pad_length > 0) {
					sink.append(' '); // for infs and nan's, always pad with spaces
					pad_length--;
				}
			}

			if (has_sign)
				sink.append('-');

			if (inf)
				sink.append(use_capitals ? P::infUpper : P::inf);
			else
				sink.append(use_capitals ? P::nanUpper : P::nan);

			if (left_justify) {
				while (pad_length > 0) {
					sink.append(' '); // for infs and nan's, always pad with spaces
					pad_length--;
				}
			}

			return;
		}

		// At this point, we've already printed the sign, so pretend it's positive.
		if (__builtin_signbit(number))
			number = -number;

		if (print_hexfloat) {
			int exp = 0;
			double frac = frexp(number, &exp);

			if (frac != 0.0 || exp != 0) {
				frac = (frac * 2.0) - 1.0;
				exp--;
			}

			FRG_ASSERT(frac >= 0.0 && frac < 1.0);

			auto precision_or_default = precision ? *precision : (std::numeric_limits<T>::digits10 + 1);
			constexpr auto mantissa_bits = std::numeric_limits<T>::digits - 1;
			uint64_t shift_by = frg::min(mantissa_bits, precision_or_default << 2);
			auto scaled = frac * (uint64_t{1} << shift_by);
			uint64_t fracIntegral = static_cast<uint64_t>(scaled);

			if((precision_or_default << 2) < mantissa_bits) {
				auto prev_byte = fracIntegral & 0x1;
				auto last_digit = static_cast<uint64_t>(scaled * 16) & 0xF;

				if (last_digit > 8)
					fracIntegral++;
				else if (last_digit == 8 && prev_byte) // round ties to even
					fracIntegral++;
			}

			int trailingZeroes = 0;

			if (fracIntegral) {
				while (fracIntegral % 16 == 0) {
					fracIntegral /= 16;
					trailingZeroes++;
				}
			}

			auto int_length = has_sign + 3;
			int frac_length = (shift_by >> 2) - trailingZeroes;
			auto exp_length = 2 + textLength(exp, 10, true);

			// digits after the radix or the alternative form force a decimal point
			bool print_decimal_point = ((precision && precision.value()) || fracIntegral || alt_conversion);

			auto decimal_point_length = [&]() -> size_t {
				if (print_decimal_point) {
					if constexpr (std::is_same_v<Char, char>)
						return strlen(locale_opts.decimal_point);
					else
						return 1;
				}
				return 0;
			}();
			int total_length = int_length + decimal_point_length + frac_length + exp_length;

			auto pad_length = width > total_length ? width - total_length : 0;

			if (!left_justify) {
				while (pad_length > 0) {
					sink.append(padding);
					pad_length--;
				}
			}

			if (has_sign)
				sink.append('-');

			if (use_capitals)
				sink.append(number == 0.0 ? (P::hexPrefixUpperZero) : (P::hexPrefixUpperOne));
			else
				sink.append(number == 0.0 ? (P::hexPrefixZero) : (P::hexPrefixOne));

			if (print_decimal_point)
				sink.append(locale_opts.decimal_point);
			if (fracIntegral)
				print_int<S, decltype(fracIntegral), Char>(sink, fracIntegral, 16, {}, frac_length, '0', {}, {}, {}, {}, use_capitals);
			if (precision && *precision > frac_length)
				print_int<S, decltype(fracIntegral), Char>(sink, 0, 10, {}, *precision - frac_length);

			sink.append(use_capitals ? 'P' : 'p');

			print_int<S, decltype(exp), Char>(sink, exp, 10, {}, {}, {}, {}, {}, true);

			if (left_justify) {
				while (pad_length > 0) {
					sink.append(padding);
					pad_length--;
				}
			}

			return;
		}

		// declare the variables here, but defer initialization to when (if) we need it.
		bool exp_mantissa_init = false;
		int exponent;
		double mantissa;

		// defer floating-point calculations for as long as possible
		auto deferredMantissaExpInit = [&] () {
			if (number == 0.0) {
				mantissa = 0.0;
				exponent = 0;
				exp_mantissa_init = true;
				return;
			}

			exponent = (int) floor(log10(number));
			mantissa = number / pow(10, exponent);

			if (__builtin_isinf(mantissa)) {
				mantissa = 0.0;
				exponent = 0;
				exp_mantissa_init = true;
				return;
			}

			while (mantissa >= 10.0) {
				mantissa /= 10.0;
				exponent++;

				if (mantissa < 1.0) {
					mantissa = 1.0;
					break;
				}
			}

			exp_mantissa_init = true;
		};

		if (use_compact) {
			deferredMantissaExpInit();

			if (*precision > exponent && exponent >= -4) {
				exponential_form = false;
				*precision = *precision - 1 - exponent;
			} else {
				exponential_form = true;
				*precision = *precision - 1;
			}
		}

		if (exponential_form) {
			if (!exp_mantissa_init)
				deferredMantissaExpInit();

			number = mantissa;
		}

		// do rounding
		double intpart, fracpart;
		fracpart = modf(number, &intpart);
		uint64_t integralDigits = static_cast<uint64_t>(intpart);
		// TODO: improve perf with a lookup table? precision would need clamping then
		double shift = pow(10, *precision);
		// safe because we ensure above that `number` (and therefore `fracpart`) are non-negative
		uint64_t fracDigits = static_cast<uint64_t>(rint(fracpart * shift));

		if (fracDigits == shift) {
			integralDigits++;
			fracDigits = 0;
		}

		if (use_compact && !alt_conversion) {
			if (fracDigits == 0) {
				*precision = 0;
			} else {
				int trailingZeroes = 0;

				while (fracDigits % 10 == 0) {
					fracDigits /= 10;
					trailingZeroes++;
				}

				*precision = *precision - trailingZeroes;
			}
		}

		// Compute the number of decimal digits in the integer part of n
		// TODO: Don't assume base 10
		auto int_length = textLength(integralDigits);

		// if we do grouping, calculate how many additional characters that consumes
		auto group_sep_length = group_thousands ? grouping_extra_characters(integralDigits, *precision, locale_opts) : 0;

		// digits after the radix or the alternative form force a decimal point
		bool print_decimal_point = (*precision > 0 || alt_conversion);
		auto decimal_point_length = print_decimal_point ? generic_strlen(locale_opts.decimal_point) : 0;

		// Plus one for the decimal point
		int total_length = has_sign + int_length + group_sep_length + decimal_point_length + *precision;

		// Handle the exponent in the style of `e+09`
		if (exponential_form)
			total_length += 2 + frg::max(2, textLength(exponent, 10, true));

		auto pad_length = width > total_length ? width - total_length : 0;

		if (!left_justify) {
			while (pad_length > 0) {
				sink.append(padding);
				pad_length--;
			}
		}

		if (has_sign)
			sink.append('-');

		print_int<S, decltype(integralDigits), Char>(sink, integralDigits, 10, 0, 1, {}, false, group_thousands, false, false, false, locale_opts);

		if (print_decimal_point)
			sink.append(locale_opts.decimal_point);
		if (*precision > 0)
			print_int<S, decltype(fracDigits), Char>(sink, fracDigits, 10, *precision, *precision, '0');

		if (exponential_form) {
			sink.append(use_capitals ? 'E' : 'e');

			print_int<S, decltype(exponent), Char>(sink, exponent, 10, 2, 2, '0', false, false, true);
		}

		if (left_justify) {
			while (pad_length > 0) {
				sink.append(padding);
				pad_length--;
			}
		}
	}

	template<typename T, Sink S>
	void format_float(T object, format_options fo, S &sink) {
		int precision_or_default = fo.precision.has_value() ? *fo.precision : 6;
		print_float(sink, object, fo.minimum_width, precision_or_default,
				fo.fill_zeros ? '0' : ' ', fo.left_justify, fo.alt_conversion,
				fo.use_capitals, fo.group_thousands, fo.use_compact, fo.use_exponential);
	}
#endif /* __STDC_HOSTED__ */
};

template<Sink S>
void format_object(unsigned int object, format_options fo, S &sink) {
	_fmt_basics::format_integer(object, fo, sink);
}

template<Sink S>
void format_object(unsigned long object, format_options fo, S &sink) {
	_fmt_basics::format_integer(object, fo, sink);
}

template<Sink S>
void format_object(unsigned long long object, format_options fo, S &sink) {
	_fmt_basics::format_integer(object, fo, sink);
}

template<Sink S>
void format_object(int object, format_options fo, S &sink) {
	_fmt_basics::format_integer(object, fo, sink);
}

template<Sink S>
void format_object(long object, format_options fo, S &sink) {
	_fmt_basics::format_integer(object, fo, sink);
}

template<Sink S>
void format_object(long long object, format_options fo, S &sink) {
	_fmt_basics::format_integer(object, fo, sink);
}

#if __STDC_HOSTED__ || defined(FRG_HAVE_LIBC)
template<Sink S>
void format_object(float object, format_options fo, S &sink) {
	_fmt_basics::format_float(object, fo, sink);
}

template<Sink S>
void format_object(double object, format_options fo, S &sink) {
	_fmt_basics::format_float(object, fo, sink);
}
#endif


template<Sink F>
void format_object(char object, format_options fo, F &formatter) {
	if(fo.conversion == format_conversion::character) {
		formatter.append(object);
		return;
	}
	_fmt_basics::format_integer(object, fo, formatter);
}

template<Sink S>
void format_object(const char *object, format_options, S &sink) {
	sink.append(object);
}

template<Sink S>
void format_object(const frg::string_view &object, format_options, S &sink) {
	for(size_t i = 0; i < object.size(); ++i)
		sink.append(object[i]);
}

template<Sink S, typename Allocator>
void format_object(const frg::string<Allocator> &object, format_options, S &sink) {
	sink.append(object.data());
}

template<Sink S>
void format_object(const void *object, format_options fo, S &sink) {
	sink.append("0x");
	_fmt_basics::format_integer(reinterpret_cast<uintptr_t>(object),
			fo.with_conversion(format_conversion::hex), sink);
}

template<Sink S>
void format_object(std::nullptr_t, format_options fo, S &sink) {
	format_object(static_cast<const void *>(nullptr), fo, sink);
}

// ----------------------------------------------------------------------------

struct char_fmt {
	template<Sink S>
	friend void format_object(char_fmt self, format_options fo, S &sink) {
		(void)fo;
		sink.append(self.c_);
	}

	template<typename T>
	char_fmt(const T &x)
	: c_{static_cast<char>(x)} { }

private:
	const char c_;
};

#ifdef FRG_HAS_RANGES
template<std::ranges::input_range R, Sink F>
requires requires {
    typename std::char_traits<std::ranges::range_value_t<R>>;
}
void format_object(const R &range, format_options fo, F &formatter) {
	/* TODO(arsen): figure out what to do about wchar (and if we care...)
	 */
	std::ranges::for_each(range, [&] (const auto &x) {
		format_object(char_fmt { x }, fo, formatter);
	});
}
#endif

template<typename T>
struct hex_fmt {
	template<Sink F>
	friend void format_object(hex_fmt self, format_options fo, F &formatter) {
		format(*self._xp, fo.with_conversion(format_conversion::hex), formatter);
	}

	hex_fmt(const T &x)
	: _xp{&x} { }

private:
	const T *_xp;
};

static inline char *strchr(const char *s, int c) {
	while (*s) {
		if (*s == c)
			return const_cast<char *>(s);

		s++;
	}

	return nullptr;
}

struct escape_fmt {
	template<Sink S>
	friend void format_object(escape_fmt self, format_options fo, S &sink) {
		auto p = reinterpret_cast<const unsigned char *>(self._buffer);
		for(size_t i = 0; i < self._size; i++) {
			auto c = p[i];
			if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
				sink.append(c);
			}else if(c >= '0' && c <= '9') {
				sink.append(c);
			}else if(c == ' ') {
				sink.append(' ');
			}else if(strchr("!#$%&()*+,-./:;<=>?@[]^_`{|}~", c)) {
				sink.append(c);
			}else if(c == '\\') {
				sink.append("\\\\");
			}else if(c == '\"') {
				sink.append("\\\"");
			}else if(c == '\'') {
				sink.append("\\\'");
			}else if(c == '\n') {
				sink.append("\\n");
			}else if(c == '\t') {
				sink.append("\\t");
			}else{
				sink.append("\\x{");
				format((unsigned int)c, fo.with_conversion(format_conversion::hex), sink);
				sink.append('}');
			}
		}
	}

	escape_fmt(const void *buffer, size_t size)
	: _buffer{buffer}, _size{size} { }

private:
	const void *_buffer;
	size_t _size;
};

static inline bool isdigit(char c) {
	return c >= '0' && c <= '9';
}

namespace detail_ {
	template <typename ...Ts>
	struct fmt_impl {
		frg::string_view fmt;
		frg::tuple<Ts...> args;

		template <typename F> requires (sizeof...(Ts) > 0)
		bool format_nth(size_t n, format_options fo, F &formatter) const {
			if (n >= sizeof...(Ts))
				return false;

			return ![&]<size_t ...I>(std::index_sequence<I...>) -> bool {
				return ((I == n
					? (format(args.template get<I>(), fo, formatter), false)
					: true) && ...);
			}(std::make_index_sequence<sizeof...(Ts)>{});
		}

		template <typename F>
		bool format_nth(size_t, format_options, F &) const {
			return false;
		}

		// Format specifier syntax:
		// ([0-9]+)?(:0?[0-9]*[bcdioXx]?)?
		bool parse_fmt_spec(frg::string_view spec, size_t &pos, format_options &fo) const {
			enum class modes {
				pos, fill, width, conv
			} mode = modes::pos;
			bool pos_set = false;
			size_t tmp_pos = 0;

			fo.minimum_width = 0;

			for (size_t i = 0; i < spec.size(); i++) {
				char c = spec[i];

				switch (mode) {
					case modes::pos:
						if (isdigit(c)) {
							pos_set = true;
							tmp_pos *= 10;
							tmp_pos += c - '0';
						} else if (c == ':') {
							mode = modes::fill;
						} else {
							return false;
						}

						break;

					case modes::fill:
						if (c == '0')
							fo.fill_zeros = true;

						mode = modes::width;

						[[fallthrough]];

					case modes::width:
						if (isdigit(c)) {
							fo.minimum_width *= 10;
							fo.minimum_width += spec[i] - '0';
						} else {
							switch (spec[i]) {
								case 'b': fo.conversion = format_conversion::binary; break;
								case 'c': fo.conversion = format_conversion::character; break;
								case 'o': fo.conversion = format_conversion::octal; break;
								case 'i':
								case 'd': fo.conversion = format_conversion::decimal; break;
								case 'X': fo.use_capitals = true; [[fallthrough]];
								case 'x': fo.conversion = format_conversion::hex; break;
								case 'f': break;
								case 'g': fo.use_compact = true; break;
								case 'e': fo.use_exponential = true; break;
								default: return false;
							}

							mode = modes::conv;
						}

						break;

					// Anything after the conversion specifier is illegal.
					case modes::conv:
						return false;
				}
			}

			if (pos_set)
				pos = tmp_pos;

			return true;
		}

		template <Sink S>
		friend void format_object(const fmt_impl &self, format_options fo, S &sink) {
			size_t current_arg = 0;
			size_t arg_fmt_start = 0, arg_fmt_end = 0;

			enum class modes {
				str, arg
			} mode = modes::str;

			for (size_t i = 0; i < self.fmt.size(); i++) {
				auto c = self.fmt[i];
				auto next = (i + 1) < self.fmt.size() ? self.fmt[i + 1] : 0;

				switch (mode) {
					case modes::str: {
						if (c == '{' && next != '{') {
							mode = modes::arg;
							arg_fmt_start = i;
						} else {
							if (c == '{')
								i++;
							sink.append(c);
						}

						break;
					}

					case modes::arg:
						if (c == '}') {
							mode = modes::str;
							arg_fmt_end = i;

							format_options fo{};
							size_t pos = current_arg++;

							if (!self.parse_fmt_spec(self.fmt.sub_string(arg_fmt_start + 1,
											arg_fmt_end - arg_fmt_start - 1), pos, fo)) {
								// Failed to parse format specifier, print it as is
								format_object(self.fmt.sub_string(arg_fmt_start,
										arg_fmt_end - arg_fmt_start + 1),
									fo, sink);

								break;
							}

							if (!self.format_nth(pos, fo, sink)) {
								// Failed to print argument (arg index out of bounds), print format specifier instead
								format_object(self.fmt.sub_string(arg_fmt_start,
										arg_fmt_end - arg_fmt_start + 1),
									fo, sink);
							}
						}
						break;
				}
			}

			// Unclosed format specifier, print it as is
			if (mode != modes::str) {
				format_object(self.fmt.sub_string(arg_fmt_start,
						self.fmt.size() - arg_fmt_start),
					fo, sink);
			}
		}
	};
} // namespace detail_

template <typename ...Ts>
auto fmt(frg::string_view fmt, Ts &&...ts) {
	return detail_::fmt_impl<Ts...>{fmt, frg::tuple<Ts...>{std::forward<Ts>(ts)...}};
}

// ----------------------------------------------------------------------------
// Formatting entry points.
// ----------------------------------------------------------------------------

// Internally calls format_object() with default options to format an object.
// format_object() is an ADL customization point of this formatting library.
template<typename T, Sink S>
void format(const T &object, S &sink) {
	format_object(object, format_options{}, sink);
}

template<typename T, Sink S>
void format(const T &object, format_options fo, S &sink) {
	format_object(object, fo, sink);
}

} // namespace frg

#endif // FRG_FORMATTING_HPP
