#include "CppUnitTest.h"

#include <cstdio>
#include <exception>

int main() {
    using namespace Microsoft::VisualStudio::CppUnitTestFramework;
    int failed = 0;
    for (const TestCase& t : Registry()) {
        try {
            t.run();
            std::printf("[ PASS ] %s\n", t.name.c_str());
        } catch (const std::exception& ex) {
            ++failed;
            std::printf("[ FAIL ] %s: %s\n", t.name.c_str(), ex.what());
        }
    }
    std::printf("%zu tests, %d failed\n", Registry().size(), failed);
    return failed == 0 ? 0 : 1;
}
