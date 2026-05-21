// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – Minimal Test Runner
//  No external test framework dependency.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>
#include <string>

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

static std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> v;
    return v;
}

struct RegisterTest {
    RegisterTest(const char* name, std::function<void()> fn) {
        test_registry().push_back({name, std::move(fn)});
    }
};

#define TEST(name) \
    static void test_##name(); \
    static RegisterTest _reg_##name(#name, test_##name); \
    static void test_##name()

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) { \
        std::fprintf(stderr, "  FAIL: %s:%d  ASSERT_TRUE(%s)\n", __FILE__, __LINE__, #expr); \
        std::exit(1); \
    } } while(0)

#define ASSERT_EQ(a, b) \
    do { if (!((a) == (b))) { \
        std::fprintf(stderr, "  FAIL: %s:%d  ASSERT_EQ(%s, %s)\n", __FILE__, __LINE__, #a, #b); \
        std::exit(1); \
    } } while(0)

#define ASSERT_GT(a, b) \
    do { if (!((a) > (b))) { \
        std::fprintf(stderr, "  FAIL: %s:%d  ASSERT_GT(%s, %s)\n", __FILE__, __LINE__, #a, #b); \
        std::exit(1); \
    } } while(0)

#define ASSERT_LT(a, b) \
    do { if (!((a) < (b))) { \
        std::fprintf(stderr, "  FAIL: %s:%d  ASSERT_LT(%s, %s)\n", __FILE__, __LINE__, #a, #b); \
        std::exit(1); \
    } } while(0)

// Expose symbols to test files
int g_pass = 0, g_fail = 0;

int main() {
    std::printf("NanoMatch Unit Tests\n");
    std::printf("════════════════════\n");

    int pass = 0, fail = 0;
    for (auto& tc : test_registry()) {
        std::printf("  %-50s ", tc.name.c_str());
        std::fflush(stdout);
        try {
            tc.fn();
            std::printf("PASS\n");
            ++pass;
        } catch (const std::exception& e) {
            std::printf("FAIL (%s)\n", e.what());
            ++fail;
        } catch (...) {
            std::printf("FAIL (unknown exception)\n");
            ++fail;
        }
    }

    std::printf("════════════════════\n");
    std::printf("Results: %d passed, %d failed\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
