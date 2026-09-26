#include <chrono>
#include <cstring>
#include <exception>

#include "test_framework.h"

namespace ictest {
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
int g_checks = 0;
int g_failures = 0;
void report_failure(const char* file, int line, const std::string& msg) {
    ++g_failures;
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, msg.c_str());
}
}  // namespace ictest

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failed_tests = 0;
    for (auto& tc : ictest::registry()) {
        if (filter && !std::strstr(tc.name, filter)) continue;
        int before = ictest::g_failures;
        auto t0 = std::chrono::steady_clock::now();
        std::printf("[ RUN  ] %s\n", tc.name);
        std::fflush(stdout);
        try {
            tc.fn();
        } catch (const ictest::Failure&) {
        } catch (const std::exception& e) {
            ictest::report_failure(tc.name, 0, std::string("exception: ") + e.what());
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        bool ok = ictest::g_failures == before;
        std::printf("[ %s ] %s (%.0f ms)\n", ok ? " OK " : "FAIL", tc.name, ms);
        ++ran;
        if (!ok) ++failed_tests;
    }
    std::printf("\n%d tests, %d checks, %d failed tests\n", ran, ictest::g_checks, failed_tests);
    return failed_tests == 0 ? 0 : 1;
}
