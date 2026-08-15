#pragma once

#include <array>
#include <frg/macros.hpp>
#include <frg/expected.hpp>
#include <frg/formatting.hpp>

namespace frg FRG_VISIBILITY {

union arg {
	uintmax_t i;
#ifdef FRG_DONT_USE_LONG_DOUBLE
	double f;
#else
	long double f;
#endif
	void *p;
};

struct va_struct {
	va_list args;
	arg *arg_list;
	int num_args = 0;
};

enum class printf_arg_type {
	INT,
	CHAR,
	WCHAR,
	POINTER,
	DOUBLE,
};

enum class printf_size_mod {
	default_size,
	char_size,
	short_size,
	long_size,
	longlong_size,
#ifndef FRG_DONT_USE_LONG_DOUBLE
	longdouble_size,
#endif
	native_size,
	intmax_size
};

template<typename T>
T pop_arg(va_struct *vsp, format_options *opts) {
	auto pop_va_arg = [&] {
		if constexpr (std::is_same_v<T, unsigned char> ||
				std::is_same_v<T, char> ||
				std::is_same_v<T, signed char> ||
				std::is_same_v<T, unsigned short> ||
				std::is_same_v<T, short> ||
				std::is_same_v<T, signed short>) {
			return static_cast<T>(va_arg(vsp->args, int));
		} else {
			return va_arg(vsp->args, T);
		}
	};

	auto get_union_member = [&] (int pos) {
		if constexpr (std::is_same_v<T, void*>)
			return &vsp->arg_list[pos].p;
		else if constexpr (std::is_floating_point_v<T>)
			return reinterpret_cast<T*>(&vsp->arg_list[pos].f);
		else
			return reinterpret_cast<T*>(&vsp->arg_list[pos].i);
	};

	if (opts->arg_pos == -1)
		return pop_va_arg();

	if(opts->dollar_arg_pos) {
		if(opts->arg_pos >= vsp->num_args) {
			// we copy out all previous and the requested argument into our vsp->arg_list
			for(int i = vsp->num_args; i <= opts->arg_pos; i++) {
				auto arg = pop_va_arg();
				*get_union_member(i) = arg;
			}

			vsp->num_args = opts->arg_pos + 1;
		}

		return *get_union_member(opts->arg_pos);
	}

	auto arg = pop_va_arg();
	*get_union_member(vsp->num_args++) = arg;
	return arg;
}

template <typename A, typename Char>
concept PrintfFormatAgent =
    requires(A agent, Char c, const Char *str, size_t n,
             frg::format_options opts, frg::printf_size_mod szmod) {
      agent(c);
      agent(str, n);
      agent(c, opts, szmod);
      { agent.format_type(c, szmod) } -> std::convertible_to<std::optional<frg::printf_arg_type>>;
    };

template<typename Char, size_t MaxArgs, PrintfFormatAgent<Char> A>
	requires (MaxArgs >= 9)
frg::expected<format_error> printf_format(A agent, const Char *s, va_struct *vsp) {
	FRG_ASSERT(s != nullptr);
	bool dollar_arg_pos = false;

	struct pos_arg {
		bool used = false;
		printf_arg_type type;
		printf_size_mod sz = printf_size_mod::default_size;
	};

	// keep track of positional argument types
	std::array<pos_arg, MaxArgs> pos_args;
	size_t max_pos_arg = 0;

	const Char *p1 = s;
	while (*p1) {
		if (*p1++ != '%')
			continue;

		if (*p1 == '%') {
			p1++;
			continue;
		}

		if (max_pos_arg >= pos_args.size())
			return frg::format_error::too_many_args;

		std::optional<size_t> format_arg_pos;

		// Attempt to parse a positional argument of the form `n$`; on success, return a pair of
		// (one-based value, length including dollar sign).
		auto parse_positional_arg = [] (const Char *p) -> std::optional<std::pair<size_t, size_t>> {
			size_t val = 0;
			const Char *orig = p;
			while(*p >= '0' && *p <= '9') {
				val = val * 10 + (*p - '0');
				++p;
				FRG_ASSERT(*p);
			}

			if (*p++ == '$' && val && val <= MaxArgs)
				return std::make_pair(val, p - orig);
			return std::nullopt;
		};

		// Save a positional argument at position (one-based!) idx
		auto save_positional_argument = [&] (size_t pos, printf_arg_type type) -> size_t {
			FRG_ASSERT(pos);
			pos_args[pos-1].type = type;
			pos_args[pos-1].used = true;
			dollar_arg_pos = true;
			max_pos_arg = frg::max(pos-1, max_pos_arg);
			return pos-1;
		};

		while(true) {
			if (*p1 >= '1' && *p1 <= '9') {
				// Delay figuring out the type of the conversion argument;
				auto pos_arg = parse_positional_arg(p1);
				if (!pos_arg)
					break;
				format_arg_pos = save_positional_argument(pos_arg->first, printf_arg_type::INT);
				p1 += pos_arg->second;
				FRG_ASSERT(*p1);
			} else if(*p1 == '-') {
				++p1;
				FRG_ASSERT(*p1);
			}else if(*p1 == '+') {
				++p1;
				FRG_ASSERT(*p1);
			}else if(*p1 == ' ') {
				++p1;
				FRG_ASSERT(*p1);
			}else if(*p1 == '#') {
				++p1;
				FRG_ASSERT(*p1);
			}else if(*p1 == '0') {
				++p1;
				FRG_ASSERT(*p1);
			}else if(*p1 == '\'') {
				++p1;
				FRG_ASSERT(*p1);
			}else{
				break;
			}
		}

		if(*p1 == '*') {
			++p1;
			FRG_ASSERT(*p1);

			if (*p1 >= '1' && *p1 <= '9') {
				auto pos_arg = parse_positional_arg(p1);
				if (!pos_arg)
					return frg::format_error::invalid_format;
				save_positional_argument(pos_arg->first, printf_arg_type::INT);
				p1 += pos_arg->second;
				FRG_ASSERT(*p1);
			} else {
				FRG_ASSERT(!dollar_arg_pos);
			}
		}else{
			while(*p1 >= '0' && *p1 <= '9') {
				++p1;
				FRG_ASSERT(*p1);
			}
		}

		if(*p1 == '.') {
			++p1;
			FRG_ASSERT(*p1);

			if(*p1 == '*') {
				++p1;
				FRG_ASSERT(*p1);

				if (*p1 >= '1' && *p1 <= '9') {
					auto pos_arg = parse_positional_arg(p1);
					if (!pos_arg)
						return frg::format_error::invalid_format;
					save_positional_argument(pos_arg->first, printf_arg_type::INT);
					p1 += pos_arg->second;
					FRG_ASSERT(*p1);
				} else {
					FRG_ASSERT(!dollar_arg_pos);
				}
			}else{
				while(*p1 >= '0' && *p1 <= '9') {
					++p1;
					FRG_ASSERT(*p1);
				}
			}
		}

		auto szmod = printf_size_mod::default_size;
		if(*p1 == 'l') {
			++p1;
			FRG_ASSERT(*p1);
			if(*p1 == 'l') {
				szmod = printf_size_mod::longlong_size;
				++p1;
				FRG_ASSERT(*p1);
			}else{
				szmod = printf_size_mod::long_size;
			}
		}else if(*p1 == 'z') {
			szmod = printf_size_mod::native_size;
			++p1;
			FRG_ASSERT(*p1);
#ifndef FRG_DONT_USE_LONG_DOUBLE
		} else if(*p1 == 'L') {
			szmod = printf_size_mod::longdouble_size;
			++p1;
			FRG_ASSERT(*p1);
#endif
		} else if(*p1 == 'h') {
			++p1;
			FRG_ASSERT(*p1);
			if(*p1 == 'h') {
				szmod = printf_size_mod::char_size;
				++p1;
				FRG_ASSERT(*p1);
			}else{
				szmod = printf_size_mod::short_size;
			}
		} else if(*p1 == 't') {
			szmod = printf_size_mod::native_size;
			++p1;
			FRG_ASSERT(*p1);
		} else if(*p1 == 'j') {
			szmod = printf_size_mod::intmax_size;
			++p1;
			FRG_ASSERT(*p1);
		} else if(*p1 == 'w') {
			int bits = 0;
			bool fastType = false;
			++p1;
			if(*p1 == 'f') {
				++p1;
				fastType = true;
			}
			while(*p1 >= '0' && *p1 <= '9') {
				bits = bits * 10 + (*p1 - '0');
				++p1;
				FRG_ASSERT(*p1);
			}
			switch(bits) {
				case 8:
					if(!fastType)
						szmod = printf_size_mod::char_size;
					else if constexpr (std::is_same<uint8_t, uint_fast8_t>())
						szmod = printf_size_mod::char_size;
					else if constexpr (std::is_same<uint16_t, uint_fast8_t>())
						szmod = printf_size_mod::short_size;
					else if constexpr (std::is_same<uint32_t, uint_fast8_t>())
						szmod = printf_size_mod::default_size;
					else if constexpr (std::is_same<uint64_t, uint_fast8_t>())
						szmod = printf_size_mod::longlong_size;
					else
						FRG_ASSERT(!"unsupported fast type size");
					break;
				case 16:
					if(!fastType)
						szmod = printf_size_mod::short_size;
					else if constexpr (std::is_same<uint16_t, uint_fast16_t>())
						szmod = printf_size_mod::short_size;
					else if constexpr (std::is_same<uint32_t, uint_fast16_t>())
						szmod = printf_size_mod::default_size;
					else if constexpr (std::is_same<uint64_t, uint_fast16_t>())
						szmod = printf_size_mod::longlong_size;
					else
						FRG_ASSERT(!"unsupported fast type size");
					break;
				case 32:
					if(!fastType)
						szmod = printf_size_mod::default_size;
					else if constexpr (std::is_same<uint32_t, uint_fast32_t>())
						szmod = printf_size_mod::default_size;
					else if constexpr (std::is_same<uint64_t, uint_fast32_t>())
						szmod = printf_size_mod::longlong_size;
					else
						FRG_ASSERT(!"unsupported fast type size");
					break;
				case 64:
					if(!fastType)
						szmod = printf_size_mod::longlong_size;
					else if constexpr (std::is_same<uint64_t, uint_fast64_t>())
						szmod = printf_size_mod::longlong_size;
					else
						FRG_ASSERT(!"unsupported fast type size");
					break;
			}
		}

		// Determine the type of the conversion argument
		if (dollar_arg_pos) {
			FRG_ASSERT(format_arg_pos);
			auto type_info = agent.format_type(*p1, szmod);
			if (!type_info)
				return frg::format_error::agent_error;
			pos_args[*format_arg_pos].type = *type_info;
			pos_args[*format_arg_pos].sz = szmod;
		}
	}

	// Correctly build up the frg::va_struct arg list with the correct argument types
	if (dollar_arg_pos) {
		for (size_t i = 0; i <= max_pos_arg; i++) {
			// Unused positional arguments are UB, so return an error
			if (!pos_args[i].used)
				return frg::format_error::positional_args_gap;

			format_options opts;
			opts.dollar_arg_pos = true;
			opts.arg_pos = i;

			switch(pos_args[i].type) {
				case printf_arg_type::CHAR:
					pop_arg<char>(vsp, &opts);
					break;
				case printf_arg_type::WCHAR:
					pop_arg<__WINT_TYPE__>(vsp, &opts);
					break;
				case printf_arg_type::INT:
					switch (pos_args[i].sz) {
						case printf_size_mod::char_size:
							pop_arg<signed char>(vsp, &opts);
							break;
						case printf_size_mod::short_size:
							pop_arg<short>(vsp, &opts);
							break;
						case printf_size_mod::long_size:
							pop_arg<long>(vsp, &opts);
							break;
						case printf_size_mod::longlong_size:
							pop_arg<long long>(vsp, &opts);
							break;
						case printf_size_mod::native_size:
							pop_arg<intptr_t>(vsp, &opts);
							break;
						case printf_size_mod::intmax_size:
							pop_arg<intmax_t>(vsp, &opts);
							break;
						default:
							FRG_ASSERT(pos_args[i].sz == printf_size_mod::default_size);
							pop_arg<int>(vsp, &opts);
							break;
					}
					break;
				case printf_arg_type::DOUBLE:
#ifndef FRG_DONT_USE_LONG_DOUBLE
					if (pos_args[i].sz == frg::printf_size_mod::longdouble_size)
						pop_arg<long double>(vsp, &opts);
					else
#endif
						pop_arg<double>(vsp, &opts);
					break;
				case printf_arg_type::POINTER:
					pop_arg<void *>(vsp, &opts);
					break;
				default:
					if constexpr (std::is_same_v<Char, char>) {
						(void) agent("frigg: unhandled printf_arg_type ", 33);
						(void) agent('0' + std::to_underlying(pos_args[i].type));
						(void) agent('\0');
					} else {
						(void) agent(L"frigg: unhandled printf_arg_type ", 33);
						(void) agent(L'0' + std::to_underlying(pos_args[i].type));
						(void) agent(L'\0');
					}
					return frg::format_error::agent_error;
			}
		}
	}

	while(*s) {
		if(*s != '%') {
			size_t n = 1;
			while(s[n] && s[n] != '%')
				n++;
			auto res = agent(s, n);
			if (!res)
				return res;
			s += n;
			continue;
		}

		++s;
		FRG_ASSERT(*s);

		if(*s == '%') {
			auto res = agent('%');
			if (!res)
				return res;
			++s;
			continue;
		}

		format_options opts;
		opts.dollar_arg_pos = dollar_arg_pos;
		while(true) {
			if (*s >= '0' && *s <= '9' && s[1] && s[1] == '$') {
				opts.arg_pos = *s - '0' - 1; // args are 1-indexed
				opts.dollar_arg_pos = true;
				dollar_arg_pos = true;
				s += 2;
				FRG_ASSERT(*s);
			} else if(*s == '-') {
				opts.left_justify = true;
				++s;
				FRG_ASSERT(*s);
			}else if(*s == '+') {
				opts.always_sign = true;
				++s;
				FRG_ASSERT(*s);
			}else if(*s == ' ') {
				opts.plus_becomes_space = true;
				++s;
				FRG_ASSERT(*s);
			}else if(*s == '#') {
				opts.alt_conversion = true;
				++s;
				FRG_ASSERT(*s);
			}else if(*s == '0') {
				opts.fill_zeros = true;
				++s;
				FRG_ASSERT(*s);
			}else if(*s == '\'') {
				opts.group_thousands = true;
				++s;
				FRG_ASSERT(*s);
			}else{
				break;
			}
		}

		if(opts.always_sign)
			opts.plus_becomes_space = false;
		// If the '0' and '-' flags both appear, the '0' flag is ignored
		if(opts.left_justify)
			opts.fill_zeros = false;

		if(*s == '*') {
			++s;
			FRG_ASSERT(*s);

			if (*s >= '0' && *s <= '9' && s[1] && s[1] == '$') {
				auto prev_arg_pos = opts.arg_pos;
				opts.arg_pos = *s - '0' - 1; // args are 1-indexed
				opts.dollar_arg_pos = true;
				dollar_arg_pos = true;
				s += 2;
				FRG_ASSERT(*s);
				opts.minimum_width = pop_arg<int>(vsp, &opts);
				opts.arg_pos = prev_arg_pos;
			} else {
				FRG_ASSERT(opts.arg_pos == -1);
				opts.minimum_width = pop_arg<int>(vsp, &opts);
			}

			if(opts.minimum_width < 0) {
				opts.minimum_width *= -1;
				opts.left_justify = true;
			}
		}else{
			int w = 0;
			while(*s >= '0' && *s <= '9') {
				w = w * 10 + (*s - '0');
				++s;
				FRG_ASSERT(*s);
			}
			opts.minimum_width = w;
		}

		if(*s == '.') {
			++s;
			FRG_ASSERT(*s);

			if(*s == '*') {
				++s;
				FRG_ASSERT(*s);

				if (*s >= '0' && *s <= '9' && s[1] && s[1] == '$') {
					auto prev_arg_pos = opts.arg_pos;
					opts.arg_pos = *s - '0' - 1; // args are 1-indexed
					opts.dollar_arg_pos = true;
					dollar_arg_pos = true;
					s += 2;
					FRG_ASSERT(*s);
					opts.precision = pop_arg<int>(vsp, &opts);
					opts.arg_pos = prev_arg_pos;
				} else {
					FRG_ASSERT(opts.arg_pos == -1);
					opts.precision = pop_arg<int>(vsp, &opts);
				}
				// negative precision values are treated as if no precision was specified
				if (opts.precision < 0)
					opts.precision = frg::null_opt;
			}else{
				int value = 0;
				// If no integer follows the '.', then precision is taken to be zero
				while(*s >= '0' && *s <= '9') {
					value = value * 10 + (*s - '0');
					++s;
					FRG_ASSERT(*s);
				}
				opts.precision = value;
			}
		}

		auto szmod = printf_size_mod::default_size;
		if(*s == 'l') {
			++s;
			FRG_ASSERT(*s);
			if(*s == 'l') {
				szmod = printf_size_mod::longlong_size;
				++s;
				FRG_ASSERT(*s);
			}else{
				szmod = printf_size_mod::long_size;
			}
		}else if(*s == 'z') {
			szmod = printf_size_mod::native_size;
			++s;
			FRG_ASSERT(*s);
#ifndef FRG_DONT_USE_LONG_DOUBLE
		} else if(*s == 'L') {
			szmod = printf_size_mod::longdouble_size;
			++s;
			FRG_ASSERT(*s);
#endif
		} else if(*s == 'h') {
			++s;
			FRG_ASSERT(*s);
			if(*s == 'h') {
				szmod = printf_size_mod::char_size;
				++s;
				FRG_ASSERT(*s);
			}else{
				szmod = printf_size_mod::short_size;
			}
		} else if(*s == 't') {
			szmod = printf_size_mod::native_size;
			++s;
			FRG_ASSERT(*s);
		} else if(*s == 'j') {
			szmod = printf_size_mod::intmax_size;
			++s;
			FRG_ASSERT(*s);
		} else if(*s == 'w') {
			int bits = 0;
			bool fastType = false;
			++s;
			if(*s == 'f') {
				++s;
				fastType = true;
			}
			while(*s >= '0' && *s <= '9') {
				bits = bits * 10 + (*s - '0');
				++s;
				FRG_ASSERT(*s);
			}
			switch(bits) {
				case 8:
					if(!fastType)
						szmod = printf_size_mod::char_size;
					else if constexpr (std::is_same<uint8_t, uint_fast8_t>())
						szmod = printf_size_mod::char_size;
					else if constexpr (std::is_same<uint16_t, uint_fast8_t>())
						szmod = printf_size_mod::short_size;
					else if constexpr (std::is_same<uint32_t, uint_fast8_t>())
						szmod = printf_size_mod::default_size;
					else if constexpr (std::is_same<uint64_t, uint_fast8_t>())
						szmod = printf_size_mod::longlong_size;
					else
						FRG_ASSERT(!"unsupported fast type size");
					break;
				case 16:
					if(!fastType)
						szmod = printf_size_mod::short_size;
					else if constexpr (std::is_same<uint16_t, uint_fast16_t>())
						szmod = printf_size_mod::short_size;
					else if constexpr (std::is_same<uint32_t, uint_fast16_t>())
						szmod = printf_size_mod::default_size;
					else if constexpr (std::is_same<uint64_t, uint_fast16_t>())
						szmod = printf_size_mod::longlong_size;
					else
						FRG_ASSERT(!"unsupported fast type size");
					break;
				case 32:
					if(!fastType)
						szmod = printf_size_mod::default_size;
					else if constexpr (std::is_same<uint32_t, uint_fast32_t>())
						szmod = printf_size_mod::default_size;
					else if constexpr (std::is_same<uint64_t, uint_fast32_t>())
						szmod = printf_size_mod::longlong_size;
					else
						FRG_ASSERT(!"unsupported fast type size");
					break;
				case 64:
					if(!fastType)
						szmod = printf_size_mod::longlong_size;
					else if constexpr (std::is_same<uint64_t, uint_fast64_t>())
						szmod = printf_size_mod::longlong_size;
					else
						FRG_ASSERT(!"unsupported fast type size");
					break;
			}
		}

		auto res = agent(*s, opts, szmod);
		if(!res)
			return res;

		++s;
	}

	return {};
}

template<typename Char, SinkFor<Char> S>
void do_printf_chars(S &sink, Char t, format_options opts,
		printf_size_mod szmod, va_struct *vsp) {
	using P = frg::FormatterPolicy<Char>;

	switch(t) {
	case 'p':
		FRG_ASSERT(!opts.fill_zeros);
		FRG_ASSERT(!opts.left_justify);
		FRG_ASSERT(!opts.alt_conversion);
		FRG_ASSERT(opts.minimum_width == 0);
		sink.append(P::hexPrefix);
		_fmt_basics::print_int<S, uintptr_t, Char>(sink, (uintptr_t)pop_arg<void*>(vsp, &opts), 16);
		break;
	case 'c':
		FRG_ASSERT(!opts.fill_zeros);
		FRG_ASSERT(!opts.alt_conversion);
		FRG_ASSERT(szmod == printf_size_mod::default_size);
		FRG_ASSERT(!opts.precision);
		if (opts.left_justify) {
			sink.append(pop_arg<char>(vsp, &opts));
			for (int i = 0; i < opts.minimum_width - 1; i++)
				sink.append(' ');
		} else {
			for (int i = 0; i < opts.minimum_width - 1; i++)
				sink.append(' ');
			sink.append(pop_arg<char>(vsp, &opts));
		}
		break;
	case 's': {
		FRG_ASSERT(!opts.fill_zeros);
		FRG_ASSERT(!opts.alt_conversion);

		if(szmod == printf_size_mod::default_size) {
			if constexpr (SinkFor<S, char>) {
				auto s = (const char *)pop_arg<void*>(vsp, &opts);
				if(!s)
					s = "(null)";

				int length;
				if(opts.precision)
					length = generic_strnlen(s, *opts.precision);
				else
					length = generic_strlen(s);

				if(opts.left_justify) {
					sink.append(s, length);
					for(int i = length; i < opts.minimum_width; i++)
						sink.append(' ');
				}else{
					for(int i = length; i < opts.minimum_width; i++)
						sink.append(' ');
					sink.append(s, length);
				}
			} else {
				FRG_ASSERT(!"Requested char output on Sink that does not support it.");
			}
		}else{
			FRG_ASSERT(szmod == printf_size_mod::long_size);

			if constexpr (SinkFor<S, wchar_t>) {
				auto s = (const wchar_t *)pop_arg<void*>(vsp, &opts);
				if(!s)
					s = L"(null)";

				int length;
				if(opts.precision)
					length = generic_strnlen(s, *opts.precision);
				else
					length = generic_strlen(s);

				if(opts.left_justify) {
					sink.append(s, length);
					for(int i = length; i < opts.minimum_width; i++)
						sink.append(' ');
				}else{
					for(int i = length; i < opts.minimum_width; i++)
						sink.append(' ');
					sink.append(s, length);
				}
			} else {
				FRG_ASSERT(!"Requested wchar_t output on Sink that does not support it.");
			}
		}
	} break;
	default:
		FRG_ASSERT(!"Unexpected printf terminal");
	}
}

template<typename Char, Sink S>
void do_printf_ints(S &sink, Char t, format_options opts,
		printf_size_mod szmod, va_struct *vsp, locale_options<Char> locale_opts = {}) {
	using P = FormatterPolicy<Char>;

	auto pad_to_min = [&] {
		bool put_sign = opts.always_sign;

		if(opts.minimum_width)
			for (int i = 0; i < opts.minimum_width - put_sign; i++)
				sink.append(' ');

		if(put_sign)
			sink.append('+');
	};

	if (opts.precision)
		opts.fill_zeros = false;

	// [C23-7.23.6.1] The '+' and ' ' flags are only defined for signed conversions.
	if(t != 'd' && t != 'i') {
		opts.always_sign = false;
		opts.plus_becomes_space = false;
	}

	switch(t) {
	case 'd':
	case 'i': {
		FRG_ASSERT(!opts.alt_conversion);
		long number;
		if(szmod == printf_size_mod::char_size) {
			number = pop_arg<signed char>(vsp, &opts);
		}else if(szmod == printf_size_mod::short_size) {
			number = pop_arg<short>(vsp, &opts);
		}else if(szmod == printf_size_mod::long_size) {
			number = pop_arg<long>(vsp, &opts);
		}else if(szmod == printf_size_mod::longlong_size) {
			number = pop_arg<long long>(vsp, &opts);
		}else if(szmod == printf_size_mod::native_size) {
			number = pop_arg<intptr_t>(vsp, &opts);
		}else if(szmod == printf_size_mod::intmax_size) {
			number = pop_arg<intmax_t>(vsp, &opts);
		}else{
			FRG_ASSERT(szmod == printf_size_mod::default_size);
			number = pop_arg<int>(vsp, &opts);
		}
		if(opts.precision && *opts.precision == 0 && !number) {
			pad_to_min();
		}else{
			_fmt_basics::print_int<S, long, Char>(sink, number, 10, opts.minimum_width,
					opts.precision ? *opts.precision : 1, opts.fill_zeros ? '0' : ' ',
					opts.left_justify, opts.group_thousands, opts.always_sign,
					opts.plus_becomes_space, false, locale_opts);
		}
	} break;
	case 'b':
	case 'B' : {
		auto print = [&] (auto number) {
			if (number && opts.alt_conversion) {
				opts.minimum_width -= 2;
				sink.append(t == 'b' ? P::binPrefix : P::binPrefixUpper);
			}

			if(opts.precision && *opts.precision == 0 && !number) {
				pad_to_min();
			}else{
				_fmt_basics::print_int<S, decltype(number), Char>(sink, number, 2, opts.minimum_width,
						opts.precision ? *opts.precision : 1, opts.fill_zeros ? '0' : ' ',
						opts.left_justify, false, opts.always_sign, opts.plus_becomes_space,
						false, locale_opts);
			}
		};

		if(szmod == printf_size_mod::char_size) {
			print(pop_arg<unsigned char>(vsp, &opts));
		}else if(szmod == printf_size_mod::short_size) {
			print(pop_arg<unsigned short>(vsp, &opts));
		}else if(szmod == printf_size_mod::long_size) {
			print(pop_arg<unsigned long>(vsp, &opts));
		}else if(szmod == printf_size_mod::longlong_size) {
			print(pop_arg<unsigned long long>(vsp, &opts));
		}else if(szmod == printf_size_mod::native_size) {
			print(pop_arg<size_t>(vsp, &opts));
		}else if(szmod == printf_size_mod::intmax_size) {
			print(pop_arg<uintmax_t>(vsp, &opts));
		}else{
			FRG_ASSERT(szmod == printf_size_mod::default_size);
			print(pop_arg<unsigned int>(vsp, &opts));
		}
	} break;
	case 'o': {
		auto print = [&] (auto number) {
			if (number && opts.alt_conversion) {
				opts.minimum_width -= 1;
				sink.append('0');
			}

			if(opts.precision && *opts.precision == 0 && !number) {
				pad_to_min();
			}else{
				_fmt_basics::print_int<S, decltype(number), Char>(sink, number, 8, opts.minimum_width,
						opts.precision ? *opts.precision : 1, opts.fill_zeros ? '0' : ' ',
						opts.left_justify, false, opts.always_sign, opts.plus_becomes_space,
						false, locale_opts);
			}
		};

		if(szmod == printf_size_mod::char_size) {
			print(pop_arg<unsigned char>(vsp, &opts));
		}else if(szmod == printf_size_mod::short_size) {
			print(pop_arg<unsigned short>(vsp, &opts));
		}else if(szmod == printf_size_mod::long_size) {
			print(pop_arg<unsigned long>(vsp, &opts));
		}else if(szmod == printf_size_mod::longlong_size) {
			print(pop_arg<unsigned long long>(vsp, &opts));
		}else if(szmod == printf_size_mod::native_size) {
			print(pop_arg<size_t>(vsp, &opts));
		}else if(szmod == printf_size_mod::intmax_size) {
			print(pop_arg<uintmax_t>(vsp, &opts));
		}else{
			FRG_ASSERT(szmod == printf_size_mod::default_size);
			print(pop_arg<unsigned int>(vsp, &opts));
		}
	} break;
	case 'x':
	case 'X': {
		auto print = [&] (auto number) {
			if (number && opts.alt_conversion) {
				opts.minimum_width -= 2;
				sink.append(t == 'x' ? P::hexPrefix : P::hexPrefixUpper);
			}

			if(opts.precision && *opts.precision == 0 && !number) {
				pad_to_min();
			}else{
				_fmt_basics::print_int<S, decltype(number), Char>(sink, number, 16, opts.minimum_width,
						opts.precision ? *opts.precision : 1, opts.fill_zeros ? '0' : ' ',
						opts.left_justify, false, opts.always_sign, opts.plus_becomes_space,
						t == 'X', locale_opts);
			}
		};

		if(szmod == printf_size_mod::char_size) {
			print(pop_arg<unsigned char>(vsp, &opts));
		}else if(szmod == printf_size_mod::short_size) {
			print(pop_arg<unsigned short>(vsp, &opts));
		}else if(szmod == printf_size_mod::long_size) {
			print(pop_arg<unsigned long>(vsp, &opts));
		}else if(szmod == printf_size_mod::longlong_size) {
			print(pop_arg<unsigned long long>(vsp, &opts));
		}else if(szmod == printf_size_mod::native_size) {
			print(pop_arg<size_t>(vsp, &opts));
		}else if(szmod == printf_size_mod::intmax_size) {
			print(pop_arg<uintmax_t>(vsp, &opts));
		}else{
			FRG_ASSERT(szmod == printf_size_mod::default_size);
			print(pop_arg<unsigned int>(vsp, &opts));
		}
	} break;
	case 'u': {
		auto print = [&] (auto number) {
			FRG_ASSERT(!opts.alt_conversion);
			if(opts.precision && *opts.precision == 0 && !number) {
				pad_to_min();
			}else{
				_fmt_basics::print_int<S, decltype(number), Char>(sink, number, 10, opts.minimum_width,
						opts.precision ? *opts.precision : 1, opts.fill_zeros ? '0' : ' ',
						opts.left_justify, opts.group_thousands, opts.always_sign,
						opts.plus_becomes_space, false, locale_opts);
			}
		};

		if(szmod == printf_size_mod::char_size) {
			print(pop_arg<unsigned char>(vsp, &opts));
		}else if(szmod == printf_size_mod::short_size) {
			print(pop_arg<unsigned short>(vsp, &opts));
		}else if(szmod == printf_size_mod::long_size) {
			print(pop_arg<unsigned long>(vsp, &opts));
		}else if(szmod == printf_size_mod::longlong_size) {
			print(pop_arg<unsigned long long>(vsp, &opts));
		}else if(szmod == printf_size_mod::native_size) {
			print(pop_arg<size_t>(vsp, &opts));
		}else if(szmod == printf_size_mod::intmax_size) {
			print(pop_arg<uintmax_t>(vsp, &opts));
		}else{
			FRG_ASSERT(szmod == printf_size_mod::default_size);
			print(pop_arg<unsigned int>(vsp, &opts));
		}
	} break;
	default:
		FRG_ASSERT(!"Unexpected printf terminal");
	}
}

#if __STDC_HOSTED__ || defined(FRG_HAVE_LIBC)
template<typename Char, Sink S>
void do_printf_floats(S &sink, Char t, format_options opts,
		printf_size_mod szmod, va_struct *vsp, locale_options<Char> locale_opts = {}) {
	bool use_capitals = true;
	bool use_compact = false;
	bool exponent_form = false;
	bool print_hexfloat = false;

	switch(t) {
	case 'e':
	case 'f':
	case 'g':
	case 'a':
		use_capitals = false;
		[[fallthrough]];
	case 'E':
	case 'F':
	case 'G':
	case 'A':
		if (t == 'g' || t == 'G') {
			if (opts.precision && *opts.precision == 0)
				opts.precision = 1;
			else if (!opts.precision)
				opts.precision = 6;
			use_compact = true;
		} else if (t == 'e' || t == 'E') {
			exponent_form = true;
			if (!opts.precision)
				opts.precision = 6;
		} else if (t == 'a' || t == 'A') {
			print_hexfloat = true;
		} else if (t == 'f' || t == 'F') {
			if (!opts.precision)
				opts.precision = 6;
		}

#ifndef FRG_DONT_USE_LONG_DOUBLE
		if (szmod == printf_size_mod::longdouble_size) {
			_fmt_basics::print_float<S, long double, typename S::char_type>(
					sink, pop_arg<long double>(vsp, &opts),
					opts.minimum_width, opts.precision,
					Char(opts.fill_zeros ? '0' : ' '), opts.left_justify, opts.alt_conversion,
					use_capitals, opts.group_thousands, use_compact, exponent_form, print_hexfloat, locale_opts);
			break;
		}
#endif
		FRG_ASSERT(szmod == printf_size_mod::default_size || szmod == printf_size_mod::long_size);
		_fmt_basics::print_float<S, double, typename S::char_type>(
				sink, pop_arg<double>(vsp, &opts),
				opts.minimum_width, opts.precision,
				Char(opts.fill_zeros ? '0' : ' '), opts.left_justify, opts.alt_conversion, use_capitals,
				opts.group_thousands, use_compact, exponent_form, print_hexfloat, locale_opts);
		break;
	default:
		FRG_ASSERT(!"Unexpected printf terminal");
	}
}
#endif

} // namespace frg
