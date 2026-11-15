// Minimal test harness for the host-side practice-client tests (no framework
// dependency). TEST(name) registers a case; CHECK/CHECK_EQ record failures
// and keep going so one run reports everything that's broken.
#ifndef TEST_MAIN_HPP
#define TEST_MAIN_HPP

#include <cstdio>
#include <functional>
#include <vector>

struct TestCase
{
    const char*           name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& TestRegistry()
{
    static std::vector<TestCase> registry;
    return registry;
}

inline int& TestFailures()
{
    static int failures = 0;
    return failures;
}

struct TestRegistrar
{
    TestRegistrar(const char* name, std::function<void()> fn) { TestRegistry().push_back({name, std::move(fn)}); }
};

#define TEST(name)                                       \
    static void name();                                  \
    static TestRegistrar registrar_##name(#name, name);  \
    static void name()

#define CHECK(cond)                                                                \
    do                                                                             \
    {                                                                              \
        if (!(cond))                                                               \
        {                                                                          \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++TestFailures();                                                      \
        }                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                         \
    do                                                                                         \
    {                                                                                          \
        const auto check_a = (a);                                                              \
        const auto check_b = (b);                                                              \
        if (!(check_a == check_b))                                                             \
        {                                                                                      \
            std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);              \
            ++TestFailures();                                                                  \
        }                                                                                      \
    } while (0)

#endif // TEST_MAIN_HPP
