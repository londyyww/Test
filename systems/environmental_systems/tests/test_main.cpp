#include <cstring>

#include "TestFramework.h"

namespace aistest {

namespace {
int g_failures = 0;
int g_checks = 0;
const char* g_current = "";
}  // namespace

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void Fail(const char* file, int line, const char* expr, const std::string& detail) {
    ++g_failures;
    std::printf("  FAIL [%s] %s:%d: %s %s\n", g_current, file, line, expr, detail.c_str());
}

int CheckCount() { return g_checks; }
void CountCheck() { ++g_checks; }

}  // namespace aistest

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failedTests = 0;
    for (const auto& t : aistest::Registry()) {
        if (filter != nullptr && std::strstr(t.name, filter) == nullptr) continue;
        aistest::g_current = t.name;
        const int before = aistest::g_failures;
        t.fn();
        ++run;
        const bool ok = aistest::g_failures == before;
        if (!ok) ++failedTests;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
    }
    std::printf("\n%d tests, %d checks, %d failed tests, %d failed checks\n", run, aistest::CheckCount(), failedTests,
                aistest::g_failures);
    return (failedTests == 0 && run > 0) ? 0 : 1;
}
