// A minimal test framework: TEST(name) { CHECK(cond); CHECK_EQ(a, b); mctest::check(cond, text); }, with
// skip()/skip_part() and run(), the runner (src/main.cpp wraps it in a main()).
#pragma once

#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mctest {
struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}

inline int& failures() {
    static int f = 0;
    return f;
}

// Skips are counted apart from passes: a test that could not run for want of an input says so through skip() (the
// whole test) or skip_part() (a half of it), and the runner reports both.
inline int& skips() {
    static int s = 0;
    return s;
}

inline void skip(const char* reason) {
    std::fprintf(stderr, "  (%s: skipped)\n", reason);
    skips()++;
}

inline void skip_part(const char* reason) {
    std::fprintf(stderr, "  (%s: that part skipped)\n", reason);
    skips()++;
}

struct Reg {
    Reg(const char* n, std::function<void()> f) {
        cases().push_back({n, std::move(f)});
    }
};
// a == b; integers of different signedness compare as the usual arithmetic conversions give (as the
// built-in operator would), without the sign-compare warning
template <class A, class B>
bool equal(const A& a, const B& b) {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        using Common = std::common_type_t<A, B>;
        return static_cast<Common>(a) == static_cast<Common>(b);
    } else {
        return a == b;
    }
}

// A value as a failed check prints it: numbers (and enums) in hex, strings quoted.
template <class T>
void print_value(std::ostringstream& os, const T& value) {
    if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        os << '"' << std::string_view(value) << '"';
    } else {
        os << "0x" << std::hex << static_cast<unsigned long long>(value) << std::dec;
    }
}

template <class A, class B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (!equal(a, b)) {
        std::ostringstream os;
        print_value(os, a);
        os << " != ";
        print_value(os, b);
        std::fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed: %s\n", file, line, ea, eb, os.str().c_str());
        failures()++;
    }
}

// A check with a message, as a function (an expression with commas in braces does not pass through a macro).
inline void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        failures()++;
    }
}

// The runner: every TEST, or those whose name contains the first argument. Returns the exit status.
inline int run(int argc, char** argv) {
    int run_count = 0;
    int skipped_tests = 0;

    for (const Case& test_case : cases()) {
        if (argc > 1 && std::strstr(test_case.name, argv[1]) == nullptr) {
            continue;
        }

        const int failures_before = failures();
        const int skips_before = skips();
        test_case.fn();
        run_count++;

        const bool skipped = skips() != skips_before;
        skipped_tests += skipped ? 1 : 0;

        const char* status = "ok";
        if (failures() != failures_before) {
            status = "FAILED";
        } else if (skipped) {
            status = "SKIPPED";
        }

        std::printf("%-48s %s\n", test_case.name, status);
    }

    std::printf("%d tests, %d skipped, %d failures\n", run_count, skipped_tests, failures());
    if (failures() != 0 || run_count == 0) {
        return 1;
    }
    // Every test that ran skipped: say so to ctest, rather than passing on a
    // machine that lacks what the tests need.
#ifdef PROJECT_TEST_SKIP_CODE
    if (skipped_tests == run_count) {
        return PROJECT_TEST_SKIP_CODE;
    }
#endif
    return 0;
}
}  // namespace mctest

// Macros because the checks need the caller's __FILE__ and __LINE__ and the stringified expression.
#define MCTEST_CAT2(a, b) a##b
#define MCTEST_CAT(a, b) MCTEST_CAT2(a, b)
#define TEST(name)                                          \
    static void name();                                     \
    static mctest::Reg MCTEST_CAT(reg_, name)(#name, name); \
    static void name()
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
            mctest::failures()++;                                                      \
        }                                                                              \
    } while (0)
#define CHECK_EQ(a, b) mctest::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
