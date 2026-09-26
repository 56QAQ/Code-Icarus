// Tiny dependency-free test framework.
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace ictest {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();
struct Registrar {
    Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

struct Failure {
    std::string msg;
};

extern int g_checks;
extern int g_failures;
void report_failure(const char* file, int line, const std::string& msg);

}  // namespace ictest

#define IC_CAT2(a, b) a##b
#define IC_CAT(a, b) IC_CAT2(a, b)
#define TEST(name)                                                                   \
    static void IC_CAT(test_fn_, __LINE__)();                                        \
    static ictest::Registrar IC_CAT(test_reg_, __LINE__)(name, IC_CAT(test_fn_, __LINE__)); \
    static void IC_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++ictest::g_checks;                                                          \
        if (!(cond)) ictest::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        ++ictest::g_checks;                                                          \
        auto _va = (a);                                                              \
        auto _vb = (b);                                                              \
        if (!(_va == _vb)) {                                                         \
            std::ostringstream _os;                                                  \
            _os << "CHECK_EQ(" #a ", " #b ") got " << _va << " vs " << _vb;          \
            ictest::report_failure(__FILE__, __LINE__, _os.str());                   \
        }                                                                            \
    } while (0)

#define REQUIRE(cond)                                                                \
    do {                                                                             \
        ++ictest::g_checks;                                                          \
        if (!(cond)) {                                                               \
            ictest::report_failure(__FILE__, __LINE__, "REQUIRE(" #cond ")");        \
            throw ictest::Failure{"require failed"};                                 \
        }                                                                            \
    } while (0)
