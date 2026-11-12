// Minimal dependency-free test harness (same pattern as AIS / power grid).
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace aistest {

using TestFn = void (*)();
struct TestCase {
    const char* name;
    TestFn fn;
};

std::vector<TestCase>& Registry();
void Fail(const char* file, int line, const char* expr, const std::string& detail);
int CheckCount();
void CountCheck();

struct Registrar {
    Registrar(const char* name, TestFn fn) { Registry().push_back({name, fn}); }
};

inline std::string Num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

}  // namespace aistest

#define AIS_TEST(name)                                                   \
    static void name();                                                  \
    static const ::aistest::Registrar name##_registrar(#name, &name);    \
    static void name()

#define CHECK(cond)                                                      \
    do {                                                                 \
        ::aistest::CountCheck();                                         \
        if (!(cond)) ::aistest::Fail(__FILE__, __LINE__, #cond, "");     \
    } while (0)

#define CHECK_MSG(cond, msg)                                             \
    do {                                                                 \
        ::aistest::CountCheck();                                         \
        if (!(cond)) ::aistest::Fail(__FILE__, __LINE__, #cond, (msg));  \
    } while (0)

#define CHECK_EQ(a, b)                                                                           \
    do {                                                                                         \
        ::aistest::CountCheck();                                                                 \
        if (!((a) == (b))) ::aistest::Fail(__FILE__, __LINE__, #a " == " #b, "values differ");   \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                    \
    do {                                                                                         \
        ::aistest::CountCheck();                                                                 \
        const double ais_a_ = static_cast<double>(a);                                            \
        const double ais_b_ = static_cast<double>(b);                                            \
        if (!(std::fabs(ais_a_ - ais_b_) <= (eps)))                                              \
            ::aistest::Fail(__FILE__, __LINE__, #a " ~= " #b,                                    \
                            ::aistest::Num(ais_a_) + " vs " + ::aistest::Num(ais_b_));           \
    } while (0)
