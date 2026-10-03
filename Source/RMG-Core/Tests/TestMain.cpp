#include "TestMain.hpp"

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keep output visible if a test crashes
    for (const TestCase& test : TestRegistry())
    {
        const int before = TestFailures();
        std::printf("[ RUN  ] %s\n", test.name);
        test.fn();
        std::printf("[ %s ] %s\n", TestFailures() == before ? " OK " : "FAIL", test.name);
    }
    std::printf("%d failure(s)\n", TestFailures());
    return TestFailures() == 0 ? 0 : 1;
}
