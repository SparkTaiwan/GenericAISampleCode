#pragma once

// Minimal stand-in for Microsoft's CppUnitTest.h so the GenericAI.Native.Tests
// sources compile and run on Linux unchanged (ctest -> gai_native_tests).
// Covers only what those tests use: TEST_CLASS / TEST_METHOD and
// Assert::AreEqual / IsTrue / IsFalse.

#include <cxxabi.h>

#include <cmath>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

namespace Microsoft { namespace VisualStudio { namespace CppUnitTestFramework {

struct TestCase {
    std::string name;
    std::function<void()> run;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline std::string Demangle(const char* mangled) {
    int status = 0;
    char* p = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    std::string s = (status == 0 && p) ? p : mangled;
    std::free(p);
    return s;
}

inline void RegisterTest(std::string name, std::function<void()> run) {
    Registry().push_back({std::move(name), std::move(run)});
}

// TEST_CLASS(Name) derives from TestClass<Name> so TEST_METHOD can name the
// enclosing class (TestSelf) without the macro being told it.
template <typename T>
struct TestClass {
    using TestSelf = T;
};

class Assert {
public:
    template <typename T>
    static void AreEqual(const T& expected, const T& actual) {
        if (!(expected == actual)) Fail("AreEqual failed: expected <" + Str(expected) + "> actual <" + Str(actual) + ">");
    }

    static void AreEqual(float expected, float actual, float tolerance) {
        if (!(std::fabs(expected - actual) <= tolerance))
            Fail("AreEqual failed: expected <" + Str(expected) + "> actual <" + Str(actual) +
                 "> tolerance <" + Str(tolerance) + ">");
    }

    static void AreEqual(double expected, double actual, double tolerance) {
        if (!(std::fabs(expected - actual) <= tolerance))
            Fail("AreEqual failed: expected <" + Str(expected) + "> actual <" + Str(actual) +
                 "> tolerance <" + Str(tolerance) + ">");
    }

    static void IsTrue(bool condition) {
        if (!condition) Fail("IsTrue failed");
    }

    static void IsFalse(bool condition) {
        if (condition) Fail("IsFalse failed");
    }

private:
    template <typename T>
    static std::string Str(const T& v) {
        std::ostringstream os;
        os << v;
        return os.str();
    }

    [[noreturn]] static void Fail(const std::string& msg) { throw std::runtime_error(msg); }
};

}}}  // namespace Microsoft::VisualStudio::CppUnitTestFramework

#define TEST_CLASS(className) \
    class className : public ::Microsoft::VisualStudio::CppUnitTestFramework::TestClass<className>

// The nested registrar's constructor body is a complete-class context of the
// enclosing test class, so it may instantiate it and call the method that is
// only declared further down.
#define TEST_METHOD(methodName)                                                          \
    struct methodName##_Registrar {                                                      \
        methodName##_Registrar() {                                                       \
            ::Microsoft::VisualStudio::CppUnitTestFramework::RegisterTest(               \
                ::Microsoft::VisualStudio::CppUnitTestFramework::Demangle(               \
                    typeid(TestSelf).name()) + "::" #methodName,                         \
                [] { TestSelf t; t.methodName(); });                                     \
        }                                                                                \
    };                                                                                   \
    static inline methodName##_Registrar methodName##_registrar_{};                      \
public:                                                                                  \
    void methodName()
