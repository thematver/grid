// Minimal self-registering test framework (no external dependencies).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace wtest
{
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
inline const char*& currentName() { static const char* n = ""; return n; }

struct Registrar { Registrar (const char* n, std::function<void()> f) { registry().push_back ({ n, std::move (f) }); } };

inline void fail (const char* file, int line, const std::string& msg)
{
    ++failures();
    std::printf ("  FAIL [%s] %s:%d: %s\n", currentName(), file, line, msg.c_str());
}
} // namespace wtest

#define WT_CAT2(a, b) a##b
#define WT_CAT(a, b) WT_CAT2(a, b)
#define TEST(name) \
    static void WT_CAT(test_fn_, __LINE__)(); \
    static wtest::Registrar WT_CAT(test_reg_, __LINE__) (name, WT_CAT(test_fn_, __LINE__)); \
    static void WT_CAT(test_fn_, __LINE__)()

#define CHECK(cond) do { if (! (cond)) wtest::fail (__FILE__, __LINE__, #cond); } while (0)
#define CHECK_MSG(cond, msg) do { if (! (cond)) wtest::fail (__FILE__, __LINE__, std::string (#cond) + " — " + (msg)); } while (0)
#define CHECK_NEAR(a, b, eps) do { const double _a = (a), _b = (b); if (! (std::abs (_a - _b) <= (eps))) \
    wtest::fail (__FILE__, __LINE__, std::string (#a " ≈ " #b ": ") + std::to_string (_a) + " vs " + std::to_string (_b)); } while (0)
#define CHECK_EQ_STR(a, b) do { const std::string _a = (a), _b = (b); if (_a != _b) \
    wtest::fail (__FILE__, __LINE__, std::string (#a " == " #b ": \"") + _a + "\" vs \"" + _b + "\""); } while (0)
