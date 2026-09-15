// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NODE_HEALTH_TESTS_CHECK_H_
#define NODE_HEALTH_TESTS_CHECK_H_

#include <cstdio>
#include <string>

namespace test
{

inline int failures = 0;
inline int checks = 0;

inline void check(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

inline int finish()
{
    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace test

#endif  // NODE_HEALTH_TESTS_CHECK_H_
