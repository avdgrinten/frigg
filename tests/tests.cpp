#include <fenv.h>

#include <frg/string.hpp>
#include <frg/std_compat.hpp>
#include <frg/random.hpp>

#include <gtest/gtest.h>

using string = frg::string<frg::stl_allocator>;

namespace {
void common_startsends_tests(auto ts1) {
	EXPECT_TRUE(ts1.starts_with("abc"));
	EXPECT_TRUE(ts1.ends_with("def"));

	EXPECT_FALSE(ts1.starts_with("def"));
	EXPECT_FALSE(ts1.ends_with("abc"));

	EXPECT_FALSE(ts1.ends_with("this long string should not crash"));

	EXPECT_TRUE(ts1.starts_with(ts1));
	EXPECT_TRUE(ts1.ends_with(ts1));
}
}

TEST(strings, string_starts_ends_with) {
	string ts1 { "abc123def" };

	common_startsends_tests(ts1);
}

TEST(strings, view_starts_ends_with) {
	frg::string_view ts1 { "abc123def" };

	common_startsends_tests(ts1);
}

TEST(strings, operator_equals_comparison) {
	string s1 { "Hello" }, s2 { "World" }, s3 { "Hello" };

	EXPECT_NE(s1, s2);
	EXPECT_NE(s2, s3);
	EXPECT_EQ(s1, s3);
}

TEST(strings, compare_method_comparison) {
	string s1 { "AAA" }, s2 { "AAB" }, s3 { "AA" }, s4 { "AAA" };

	EXPECT_EQ(s1.compare(s2), -1);
	EXPECT_EQ(s2.compare(s1), 1);
	EXPECT_EQ(s1.compare(s3), 1);
	EXPECT_EQ(s3.compare(s1), -1);
	EXPECT_EQ(s1.compare(s4), 0);
}

TEST(strings, generic_string_compare) {
	EXPECT_EQ(frg::generic_strcmp("abc", "abc"), 0);

	EXPECT_LT(frg::generic_strcmp("abc", "abd"), 0);
	EXPECT_GT(frg::generic_strcmp("abd", "abc"), 0);

	EXPECT_EQ(frg::generic_strcmp("a", ""), 97);
	EXPECT_EQ(frg::generic_strcmp("", "a"), -97);

	const char s1[] = {static_cast<char>(0xFF), '\0'};
	const char s2[] = {0x01, '\0'};
	EXPECT_EQ(frg::generic_strcmp(s1, s2), 254);

	const wchar_t w1[] = {static_cast<wchar_t>(std::numeric_limits<std::make_unsigned_t<wchar_t>>::max()), L'\0'};
	const wchar_t w2[] = {0x01, L'\0'};
	EXPECT_EQ(frg::generic_strcmp(w1, w2), std::numeric_limits<std::make_unsigned_t<wchar_t>>::max() - 1);

	const wchar_t w3[] = {L'\0', L'\0'};
	const wchar_t w4[] = {0x01, L'\0'};
	EXPECT_EQ(frg::generic_strcmp(w3, w4), -1);

	EXPECT_EQ(frg::generic_strcmp("\x80", ""), 128);
	EXPECT_EQ(frg::generic_strcmp(L"\u0001", L"\u0000"), 1);
	EXPECT_EQ(frg::generic_strcmp(L"\x10001", L"\x10000"), 1);
}

TEST(string_view, find) {
	frg::string_view s1 { "ABC" };

	EXPECT_EQ(s1.find_first('B'), 1);
	EXPECT_EQ(s1.find_first('D'), -1);
	EXPECT_EQ(s1.find_first("ABC"), 0);
	EXPECT_EQ(s1.find_first("BC"), 1);
	EXPECT_EQ(s1.find_first("ABCD"), -1);
	EXPECT_EQ(s1.find_first_of("CB"), 1);
	EXPECT_EQ(s1.find_first_of("DE"), -1);
}

TEST(pcg32, pcg32_brief_test) {
	frg::pcg_basic32 x { 12345, 6 };

	EXPECT_EQ(x(), 1985316396);
	EXPECT_EQ(x(), 1977560913);
	EXPECT_EQ(x(), 3056590845);
	EXPECT_EQ(x(), 1569990246);
	EXPECT_EQ(x(), 1699592177);
	EXPECT_EQ(x(), 1974316228);
	EXPECT_EQ(x(), 4283859071);
	EXPECT_EQ(x(), 3435412947);
	EXPECT_EQ(x(), 821999472);
	EXPECT_EQ(x(), 3498119420);

	EXPECT_EQ(x(10), 5);
	EXPECT_EQ(x(20), 12);
	EXPECT_EQ(x(30), 29);
	EXPECT_EQ(x(40), 6);
	EXPECT_EQ(x(50), 35);
	EXPECT_EQ(x(60), 46);
	EXPECT_EQ(x(70), 36);
	EXPECT_EQ(x(80), 69);
	EXPECT_EQ(x(90), 76);
	EXPECT_EQ(x(100), 68);
}

#include <frg/tuple.hpp>
#include <tuple> /* std::tuple_size and std::tuple_element */

TEST(tuples, basic_test) {
	int x = 5;
	int y = 7;

	frg::tuple<int, int> t { x, y };
	EXPECT_EQ(x, t.get<0>());
	EXPECT_EQ(y, t.get<1>());

	auto t2 = frg::make_tuple(x, y);
	EXPECT_EQ(x, t2.get<0>());
	EXPECT_EQ(y, t2.get<1>());
	t2.get<0>() = 1;
	t2.get<1>() = 2;
	EXPECT_EQ(t2.get<0>(), 1);
	EXPECT_EQ(t2.get<1>(), 2);
	EXPECT_NE(x, t2.get<0>());
	EXPECT_NE(y, t2.get<1>());

	static_assert(
		std::tuple_size_v<decltype(t)> == 2,
		"tuple_size produces wrong result"
	);
	static_assert(
		std::is_same_v<std::tuple_element_t<1, decltype(t)>, int>,
		"tuple_element produces wrong result"
	);
}

struct uncopyable {
	uncopyable() {}
	uncopyable(const uncopyable &) = delete;
	uncopyable(uncopyable &&) {}
	uncopyable &operator =(const uncopyable &) = delete;
	uncopyable &operator =(uncopyable &&other) { return *this; }
};

struct immovable {
	immovable() {}
	immovable(const immovable &) {}
	immovable(immovable &&) = delete;
	immovable &operator=(const immovable &) { return *this; }
	immovable &operator=(immovable &&) = delete;
};

TEST(tuples, reference_test) {
	int x = 5;
	uncopyable y {};
	immovable z {};

	frg::tuple<int&, uncopyable&, immovable&> t { x, y, z };
	EXPECT_EQ(&x, &t.get<0>());
	EXPECT_EQ(&y, &t.get<1>());
	EXPECT_EQ(&z, &t.get<2>());

	auto t2 = std::move(t);
	EXPECT_EQ(&x, &t2.get<0>());
	EXPECT_EQ(&y, &t2.get<1>());
	EXPECT_EQ(&z, &t2.get<2>());
}

#include <frg/array.hpp>

TEST(array, basic_test) {
    constexpr int N = 4;
    frg::array<int, N> arr{0, 1, 2, 3};
    for (int i = 0; i < N; i++)
        EXPECT_EQ(arr[i], i);

    const auto [a, b, c, d] = arr;
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 1);
    EXPECT_EQ(c, 2);
    EXPECT_EQ(d, 3);

    arr[0] = 1;
    EXPECT_NE(a, arr[0]); // Make sure a doesn't change when array changes

    const auto& [e, f, g, h] = arr;
    EXPECT_EQ(e, 1);
    arr[0] = 2;
    EXPECT_EQ(e, 2); // Make sure e does change when array changes

    static_assert(std::tuple_size_v<decltype(arr)> == N, "tuple_size produces wrong result");
    static_assert(std::is_same_v<std::tuple_element_t<N, decltype(arr)>, int>, "tuple_element produces wrong result");
}

#include <frg/formatting.hpp>
#include <frg/logging.hpp>
#include <string> // std::string
#include <vector> // std::vector

TEST(formatting, basic_output_to) {
	std::string std_str;
	frg::output_to(std_str) << 10;
	EXPECT_EQ(std_str, "10");

	string frg_str;
	frg::output_to(frg_str) << 10;
	std::cout << frg_str.size() << std::endl;
	EXPECT_EQ(frg_str, "10");

	std::vector<char> std_vec;
	std::vector<char> expected_vec{'1', '0'};
	frg::output_to(std_vec) << 10;
	EXPECT_EQ(std_vec, expected_vec);
}

TEST(formatting, fmt) {
	std::string str;
	frg::output_to(str) << frg::fmt("Hello {}!", "world");
	EXPECT_EQ(str, "Hello world!");
	str.clear();

	frg::output_to(str) << frg::fmt("{} {:x}", 1234, 0x3456);
	EXPECT_EQ(str, "1234 3456");
	str.clear();

	int x = 10;
	frg::output_to(str) << frg::fmt("{} {}", x, x + 20);
	EXPECT_EQ(str, "10 30");
	str.clear();

	x = 20;
	frg::output_to(str) << frg::fmt("{:d} {:i}", x, x + 20);
	EXPECT_EQ(str, "20 40");
	str.clear();

	frg::output_to(str) << frg::fmt("{:08X}", 0xAAABBB);
	EXPECT_EQ(str, "00AAABBB");
	str.clear();

	frg::output_to(str) << frg::fmt("{:b}", 0b101010);
	EXPECT_EQ(str, "101010");
	str.clear();

	frg::output_to(str) << frg::fmt("{:08b}", 0b101010);
	EXPECT_EQ(str, "00101010");
	str.clear();

	frg::output_to(str) << frg::fmt("{:o}", 0777);
	EXPECT_EQ(str, "777");
	str.clear();

	frg::output_to(str) << frg::fmt("{:03o}", 077);
	EXPECT_EQ(str, "077");
	str.clear();

	frg::output_to(str) << frg::fmt("{1} {0}", 3, 4);
	EXPECT_EQ(str, "4 3");
	str.clear();

	frg::output_to(str) << frg::fmt("{1}", 1);
	EXPECT_EQ(str, "{1}");
	str.clear();

	frg::output_to(str) << frg::fmt("{{}", 1);
	EXPECT_EQ(str, "{}");
	str.clear();

	frg::output_to(str) << frg::fmt("{:h}", 1);
	EXPECT_EQ(str, "{:h}");
	str.clear();

	frg::output_to(str) << frg::fmt("{}", 1.5);
	EXPECT_EQ(str, "1.500000");
	str.clear();

	frg::output_to(str) << frg::fmt("{}", 1.44f);
	EXPECT_EQ(str, "1.440000");
	str.clear();

	frg::output_to(str) << frg::fmt("{:f}", 1.43f);
	EXPECT_EQ(str, "1.430000");
	str.clear();

	frg::output_to(str) << frg::fmt("{:g}", 1.43f);
	EXPECT_EQ(str, "1.43");
	str.clear();

	frg::output_to(str) << frg::fmt("{:g}", 20.0 / 3.0);
	EXPECT_EQ(str, "6.66667");
	str.clear();

	frg::output_to(str) << frg::fmt("{:e}", 20.0 / 3.0);
	EXPECT_EQ(str, "6.666667e+00");
	str.clear();

#ifdef FRG_HAS_RANGES
	std::string abc_def { "abc def" };
	std::vector<char> abc_def_v { abc_def.cbegin(), abc_def.cend() };

	frg::output_to(str) << frg::fmt("testing! {}", abc_def);
	EXPECT_EQ(str, "testing! abc def");
	str.clear();

	frg::output_to(str) << frg::fmt("testing2! {}", abc_def_v);
	EXPECT_EQ(str, "testing2! abc def");
	str.clear();
#endif // FRG_HAS_RANGES
}

#include <frg/printf.hpp>

TEST(formatting, printf) {
	std::string buf{};
	frg::container_logger sink{buf};

	struct test_agent {
		frg::expected<frg::format_error> operator() (char c) {
			sink_->append(c);
			return frg::success;
		}

		frg::expected<frg::format_error> operator() (const char *c, size_t n) {
			sink_->append(c, n);
			return frg::success;
		}

		frg::expected<frg::format_error> operator() (char t, frg::format_options opts,
				frg::printf_size_mod szmod) {
			switch(t) {
				case 'c':
					frg::do_printf_chars(*sink_, t, opts, szmod, vsp_);
					break;
				case 'p': case 's':
					frg::do_printf_chars(*sink_, t, opts, szmod, vsp_);
					break;
				case 'd': case 'i': case 'o': case 'x': case 'X': case 'b': case 'B': case 'u':
					frg::do_printf_ints(*sink_, t, opts, szmod, vsp_, loc_);
					break;
				case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': case 'a': case 'A':
					frg::do_printf_floats(*sink_, t, opts, szmod, vsp_, loc_);
					break;
				default:
					// Should not be reached
					ADD_FAILURE();
			}

			return frg::success;
		}

		std::optional<frg::printf_arg_type> format_type(char t, frg::printf_size_mod szmod) {
			switch(t) {
				case 's':
					return frg::printf_arg_type::POINTER;
				case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': case 'a': case 'A':
					return frg::printf_arg_type::DOUBLE;
				case 'c': case 'd': case 'i': case 'b': case 'B': case 'o': case 'x': case 'X': case 'u':
					return frg::printf_arg_type::INT;
				default:
					sink_->append("unknown format '");
					sink_->append(t);
					sink_->append('\'');
					return std::nullopt;
			}
		}

		frg::container_logger<std::string> *sink_;
		frg::va_struct *vsp_;
		frg::locale_options<char> loc_;
	};

	auto do_test = [] (const char *expected, const char *format, ...) {
		va_list args;
		va_start(args, format);

		frg::va_struct vs;
		frg::arg arg_list[NL_ARGMAX + 1];
		vs.arg_list = arg_list;
		va_copy(vs.args, args);

		std::string buf;
		frg::container_logger<std::string> sink{buf};

		auto res = frg::printf_format<char, 9>(test_agent{&sink, &vs}, format, &vs);
		ASSERT_TRUE(res);

		ASSERT_STREQ(expected, buf.data());
	};

	auto do_test_locale = [] (frg::locale_options<char> loc, const char *expected, const char *format, ...) {
		va_list args;
		va_start(args, format);

		frg::va_struct vs;
		frg::arg arg_list[NL_ARGMAX + 1];
		vs.arg_list = arg_list;
		va_copy(vs.args, args);

		std::string buf;
		frg::container_logger<std::string> sink{buf};

		auto res = frg::printf_format<char, 9>(test_agent{&sink, &vs, loc}, format, &vs);
		ASSERT_TRUE(res);

		ASSERT_STREQ(expected, buf.data());
	};

	frg::locale_options en_US{".", ",", "\x3"};
	frg::locale_options en_IN{".", ",", "\x3\x2"};

	do_test("12", "%d", 12);
	do_test_locale(en_US, "123,456,789", "%'d", 123456789);
	do_test_locale(en_US, "0000123,456,789", "%'015d", 123456789);
	do_test_locale(en_IN, "12,34,56,789", "%'d", 123456789);
	do_test_locale(en_IN, "00012,34,56,789", "%'015d", 123456789);

	// Test %c right padding.
	do_test("a ", "%-2c", 'a');

	// Test %c left padding.
	do_test(" a", "%2c", 'a');

	// Test %d right padding.
	do_test("1 ", "%-2d", 1);
	do_test("01", "%-2.2d", 1);
	do_test("01 ", "%-3.2d", 1);
	do_test("12 ", "%-3.2d", 12);
	do_test("123", "%-3.2d", 123);
	do_test("12 ", "%-3.2u", 12);

	// Test %d left padding.
	do_test(" 1", "%2d", 1);
	do_test(" 01", "%3.2d", 1);
	do_test(" 12", "%3.2d", 12);
	do_test("123", "%3.2d", 123);
	do_test(" 12", "%3.2u", 12);
	do_test(" 012", "%04.3u", 12);
	do_test("+000", "%+04d", 0);

	// Test '+' and ' ' flags.
	do_test("+12", "%+d", 12);
	do_test(" 12", "% d", 12);
	do_test("+12", "% +d", 12);
	do_test("+12", "%+ d", 12);
	do_test("-12", "%+d", -12);
	do_test("-12", "% d", -12);
	do_test("-12", "% +d", -12);
	do_test("-12", "%+ d", -12);

	// Test '#' flag.
	do_test("0xc", "%#x", 12);
	do_test("0XC", "%#X", 12);
	do_test("0b1100", "%#b", 12);
	do_test("0B1100", "%#B", 12);
	do_test("014", "%#o", 12);
	do_test("0", "%#x", 0);
	do_test("0", "%#X", 0);
	do_test("0", "%#o", 0);

	do_test("-0x1.p+5", "%#.a", -42.0);
	do_test("-0X1.P+5", "%#.A", -42.0);
	do_test("1.", "%#.f", 1.0);
	do_test("1.", "%#.F", 1.0);
	do_test("1.", "%#.g", 1.0);
	do_test("4.e+01", "%#.g", 42.0);
	do_test("1.", "%#.G", 1.0);
	do_test("4.E+01", "%#.G", 42.0);
	do_test("-4.e+01", "%#.0e", -42.0);
	do_test("-4.E+01", "%#.0E", -42.0);

	// Test 'f'.
	do_test("1.100000", "%f", 1.1);
	do_test("3.140000", "%f", 3.14);
	do_test("0.0", "%.1f", 0.0);

	// Test %g
	do_test("0", "%g", 0.0);
	do_test("0.012", "%.2g", 0.01234);
	do_test("1.1", "%.3g", 1.1);
	do_test("1.1000", "%#.5g", 1.1);
	do_test("1.23e-05", "%g", 0.0000123);

	// Test %e
	do_test("0.000e+00", "%4.3e", 0.0);
	do_test("6.900e+27", "%.3e", 6.9e27);

	// Test %f rounding
	do_test(" 1.20", "%5.2f", 1.2);
	do_test(" 1.23", "%5.2f", 1.23);
	do_test(" 1.23", "%5.2f", 1.234);
	do_test("12.35", "%5.2f", 12.345);
	do_test("1.20 ", "%-5.2f", 1.2);

	// More extensive floating point tests.
	// Test zero and negative zero
	do_test("0.000000", "%f", 0.0);
	do_test("-0.000000", "%f", -0.0);
	do_test("0.000000e+00", "%e", 0.0);
	do_test("-0.000000e+00", "%e", -0.0);
	do_test("0", "%g", 0.0);
	do_test("-0", "%g", -0.0);

	// Test rounding
	do_test("1.24", "%.2f", 1.235);
	do_test("-1.24", "%.2f", -1.235);
	// 1.2349999999999998 is a double literal that is slightly less than 1.235
	do_test("1.23", "%.2f", 1.2349999999999998);
	do_test("0", "%.0f", 0.49);
	do_test("-0", "%.0f", -0.49);

	// Test round-to-even behavior
	fesetround(FE_TONEAREST);

	do_test("0", "%.0f", 0.5);
	do_test("-0", "%.0f", -0.5);
	do_test("1", "%.0f", 0.51);
	do_test("-1", "%.0f", -0.51);
	do_test("2", "%.0f", 2.5);
	do_test("-2", "%.0f", -2.5);
	do_test("3", "%.0f", 2.51);
	do_test("-3", "%.0f", -2.51);

	// Test %e/%E
	do_test("1.234e+04", "%.3e", 12345.0);
	do_test("1.234E+04", "%.3E", 12345.0);
	do_test("1.234e-04", "%.3e", 0.00012345);
	do_test("-1.234E-04", "%.3E", -0.00012345);

	// Test %g/%G behavior
	do_test("123456", "%g", 123456.0); // Should not use e-notation
	do_test("1.23457e+06", "%g", 1234567.0); // Should use e-notation
	do_test("12345", "%g", 12345.0);
	do_test("1.2345", "%g", 1.2345);
	do_test("0.00012345", "%g", 0.00012345); // Should not use e-notation
	do_test("1.2345e-05", "%g", 0.000012345); // Should use e-notation
	do_test("     -1.2345", "%12g", -1.2345);

	// %g precision
	do_test("1.23e+04", "%.3g", 12345.0);
	do_test("1.23E+04", "%.3G", 12345.0);
	do_test("1.23", "%.3g", 1.234);
	do_test("0.00123", "%.3g", 0.001234);
	do_test("0.000123", "%.3g", 0.0001234); // Should not use e-notation
	do_test("1.23e-05", "%.3g", 0.00001234); // Should use e-notation
	do_test("0007e+01","%08.1g", 69.2);
	do_test("000069.2","%08.3g", 69.2);
	do_test("00069.30","%#08.4g", 69.29854);

	// %g trailing zeros
	do_test("1.2", "%g", 1.200);
	do_test("1.20000", "%#g", 1.200);
	do_test("1200", "%g", 1200.0);
	do_test("1200.00", "%#g", 1200.0);

	// test negative precision and width
	do_test("15.1235","%.*g", -2, 15.1234567);
	do_test("15.1 ","%*g", -5, 15.1);
	// test supplying both '0' and '-', where the '0' flag should be ignored
	do_test("69.2  ","%0-6g", 69.2);

	// Test 'd' with different size mods to see
	// if they work
	do_test("12", "%d", 12);
	do_test("12", "%ld", 12L);
	do_test("12", "%lld", 12LL);
	do_test("12", "%zd", (size_t)12);
	do_test("12", "%hd", 12);
	do_test("12", "%hhd", 12);
	do_test("12", "%jd", (uintmax_t)12);

	// Test 'x' with different size mods to see
	// if they work
	do_test("c", "%x", 12);
	do_test("c", "%lx", 12L);
	do_test("c", "%llx", 12LL);
	do_test("c", "%zx", (size_t)12);
	do_test("c", "%hx", 12);
	do_test("c", "%hhx", 12);
	do_test("c", "%jx", (uintmax_t)12);

	// Test 'X' with different size mods to see
	// if they work
	do_test("C", "%X", 12);
	do_test("C", "%lX", 12L);
	do_test("C", "%llX", 12LL);
	do_test("C", "%zX", (size_t)12);
	do_test("C", "%hX", 12);
	do_test("C", "%hhX", 12);
	do_test("C", "%jX", (uintmax_t)12);

	// Test 'b' with different size mods to see
	// if they work
	do_test("1100", "%b", 12);
	do_test("1100", "%lb", 12L);
	do_test("1100", "%llb", 12LL);
	do_test("1100", "%zb", (size_t)12);
	do_test("1100", "%hb", 12);
	do_test("1100", "%hhb", 12);
	do_test("1100", "%jb", (uintmax_t)12);

	// Test 'o' with different size mods to see
	// if they work
	do_test("14", "%o", 12);
	do_test("14", "%lo", 12L);
	do_test("14", "%llo", 12LL);
	do_test("14", "%zo", (size_t)12);
	do_test("14", "%ho", 12);
	do_test("14", "%hho", 12);
	do_test("14", "%jo", (uintmax_t)12);

	do_test("0x1.91eb851eb851fp+1", "%a", 3.14);
	do_test("0X1.91EB851EB851FP+1", "%A", 3.14);
	do_test("0x1.92p+1", "%.2a", 3.14);
	do_test("0X1.92P+1", "%.2A", 3.14);
	do_test("0x1.81c8p+13", "%a", 12345.0);
	do_test("0X1.81C8P+13", "%A", 12345.0);
	do_test("0x1.81cp+13", "%.3a", 12345.0);
	do_test("0X1.81CP+13", "%.3A", 12345.0);
	do_test("0x1.02e4b6ce5dc68p-13", "%a", 0.00012345);
	do_test("-0X1.02E4B6CE5DC68P-13", "%A", -0.00012345);
	do_test("0x0p+0", "%a", 0.0);
	do_test("-0X0P+0", "%A", -0.0);
	do_test("inf", "%a", INFINITY);
	do_test("INF", "%A", INFINITY);
	do_test("-nan", "%a", -NAN);
	do_test("NAN", "%A", NAN);

	do_test("                0x1.48p+3", "%25a", 10.25);
	do_test("0x1.48p+3                ", "%-25a", 10.25);
	do_test("                0X1.48P+3", "%25A", 10.25);
	do_test("0X1.48P+3                ", "%-25A", 10.25);
	do_test("               -0x1.48p+3", "%25a", -10.25);
	do_test("-0x1.48p+3               ", "%-25a", -10.25);

	// Test 's' with precision.
	do_test("hello world", "%s", "hello world");
	do_test("hello", "%.5s", "hello world");
	do_test("hello", "%.*s", 5, "hello world");
	do_test("hello", "%.10s", "hello\0!!!!");

	do_test("55 33", "%2$d %1$d", 33, 55);

	do_test("ef", "%w8x", 0x1234567890abcdef);
	do_test("cdef", "%w16x", 0x1234567890abcdef);
	do_test("90abcdef", "%w32x", 0x1234567890abcdef);
	do_test("1234567890abcdef", "%w64x", 0x1234567890abcdef);

	do_test("ef cdef 90abcdef 1234567890abcdef", "%wf8x %wf16x %wf32x %wf64x",
		(uint_fast8_t) 0xef, (uint_fast16_t) 0xcdef, (uint_fast32_t) 0x90abcdef,
		(uint_fast64_t) 0x1234567890abcdef);

	do_test("001.00d", "%1$0*4$.*2$f%3$s", 1.0, 2, "d", 6);
	do_test("01234.568", "%2$0*1$.*3$Lf", 9, 1234.56789L, 3);
}

#include <frg/bitset.hpp>

TEST(bitset, bitwise) {
	constexpr auto A = 12346;
	constexpr auto B = 56789;
	constexpr auto C = 957929475;
	constexpr auto D = 9393939;
	frg::bitset<45> a = A;
	frg::bitset<45> b = B;

	a |= b;

	EXPECT_TRUE(a == frg::bitset<45>(A | B));

	frg::bitset<45> c = C;
	a &= c;

	EXPECT_TRUE(a == frg::bitset<45>(C & (A | B)));

	b ^= D;
	a |= b;

	EXPECT_TRUE(a == frg::bitset<45>((C & (A | B)) | (B ^ D)));
}

TEST(bitset, shift_left) {
	constexpr auto A = 1234;
	frg::bitset<64> sample = A;
	EXPECT_TRUE((sample >> 23) == frg::bitset<64>(A >> 23));
	frg::bitset<253> bs1;
	bs1.set(23).set(124).set(32).set(123).set(1).set(252);

	frg::bitset<253> bs2;
	bs2.set(23 + 12).set(124 + 12).set(32 + 12).set(123 + 12).set(1 + 12);

	auto bs3 = bs1 << 12;

	EXPECT_TRUE(bs3 == bs2);
}

TEST(bitset, setters_and_getters) {
	frg::bitset<12> a;
	a.set(13);
	EXPECT_TRUE(a.test(13));
	a.set(15);
	EXPECT_TRUE(a.test(15));
	a.reset(15);
	EXPECT_FALSE(a.test(15));

	a.set();
	for (std::size_t i = 0; i < 12; i++)
		EXPECT_TRUE(a.test(i));
	a.reset();
	for (std::size_t i = 0; i < 12; i++)
		EXPECT_FALSE(a.test(i));

	decltype(a)::reference r = a[4];
	r = true;

	EXPECT_TRUE(a.test(4));

	a.flip();
	for (std::size_t i = 0; i < 12; i++) {
		if (i == 4)
			EXPECT_FALSE(a.test(i));
		else
			EXPECT_TRUE(a[i]);
	}

	r.flip();
	for (std::size_t i = 0; i < 12; i++)
		EXPECT_TRUE(a.test(i));

	frg::bitset<50> b{1ull << 45};
	EXPECT_TRUE(b[45]);
	b.set(47);
	EXPECT_TRUE(b[47]);
}

TEST(bitset, count) {
	frg::bitset<24> a;
	EXPECT_TRUE(a.count() == 0);
	EXPECT_TRUE(a.none());
	EXPECT_FALSE(a.any());
	EXPECT_FALSE(a.all());
	a.flip();
	EXPECT_TRUE(a.count() == 24);
	EXPECT_TRUE(a.any());
	EXPECT_TRUE(a.all());
	a.reset();
	EXPECT_TRUE(a.set(13).set(4).count() == 2);
	EXPECT_TRUE(a.any());
	EXPECT_FALSE(a.all());
	EXPECT_FALSE(a.none());
}

#include <frg/array.hpp>
#include <frg/cmdline.hpp>

TEST(cmdline, basic_cmdline) {
	bool foo = false, bar = false;
	frg::string_view v1{};
	uint32_t v2 = 0;
	frg::string_view v3{};
	frg::string_view v4{};

	frg::array args = {
		frg::option{"foo", frg::store_true(foo)},
		frg::option{"bar", frg::store_true(bar)},
		frg::option{"baz", frg::as_string_view(v1)},
		frg::option{"qux", frg::as_number(v2)},
		frg::option{"path1", frg::as_string_view(v3)},
		frg::option{"path2", frg::as_string_view(v4)},
	};

	frg::parse_arguments("\"path1=a space/nospace\" foo baz=yoo qux=1234 \"path2=/a/b c/d\"", args);

	ASSERT_TRUE(foo);
	ASSERT_FALSE(bar);
	ASSERT_EQ(v1, "yoo");
	ASSERT_EQ(v2, 1234);
	ASSERT_EQ(v3, "a space/nospace");
	ASSERT_EQ(v4, "/a/b c/d");
}

#ifdef FRG_HAS_RANGES
TEST(cmdline, multiple_option_spans) {
	bool nosmp = false;
	frg::string_view init_exec{};

	frg::array cpu_args = {
		frg::option{"x86.nosmp", frg::store_true(nosmp)}
	};

	frg::array init_args = {
		frg::option{"init.exec", frg::as_string_view(init_exec)}
	};

	frg::array combined = { init_args, cpu_args };

	frg::parse_arguments("x86.nosmp init.exec=/sbin/posix-subsystem", std::views::join(combined));

	ASSERT_TRUE(nosmp);
	ASSERT_EQ(init_exec, "/sbin/posix-subsystem");
}
#endif // FRG_HAS_RANGES
