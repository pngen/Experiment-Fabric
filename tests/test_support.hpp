// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_TESTS_TEST_SUPPORT_HPP
#define EXPERIMENT_FABRIC_TESTS_TEST_SUPPORT_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

/// \file
/// Minimal deterministic test harness.
///
/// The harness runs every registered test in declaration order, reports failing
/// assertions with their location, and exits non-zero on any failure. It applies
/// no timeouts of any kind: a hanging test is a defect to diagnose, not
/// something to abandon.

namespace ef_test {

using TestFunction = void (*)();

/// Registers one test. Called through the EF_TEST macro at namespace scope.
void register_test(const char* suite, const char* name, TestFunction function);

/// Runs every registered test and returns the process exit code.
int run_all(int argc, char** argv);

/// Records an assertion failure for the running test.
void fail(const char* file, int line, const std::string& message);

/// Records a check result.
void check(bool condition, const char* file, int line, const std::string& message);

/// Name of the running test.
[[nodiscard]] const std::string& current_test();

/// Creates (or recreates) a scratch directory for one test.
[[nodiscard]] std::filesystem::path scratch_directory(const std::string& name);

/// Registers a path for cleanup at the end of the run.
void register_cleanup(std::filesystem::path path);

/// Registers a child process for guaranteed cleanup. The harness forces every
/// registered child to terminate before it reports, so a test can never leave a
/// stray runtime process behind even if its own teardown path is skipped.
void register_spawned_process(std::uint64_t process_id);

/// Terminates every registered child that is still alive. Returns the number
/// that had to be forced.
std::uint32_t terminate_spawned_processes();

/// Compares two values of the same type.
template <typename T>
void check_equal(const T& lhs, const T& rhs, const char* lhs_text, const char* rhs_text, const char* file,
                 int line) {
  if (!(lhs == rhs)) {
    std::string message = std::string(lhs_text) + " == " + rhs_text;
    check(false, file, line, message);
  }
}

}  // namespace ef_test

#define EF_TEST(suite_name, test_name)                                                      \
  static void ef_test_##suite_name##_##test_name();                                         \
  namespace {                                                                               \
  const bool ef_registered_##suite_name##_##test_name = []() {                              \
    ::ef_test::register_test(#suite_name, #test_name, &ef_test_##suite_name##_##test_name); \
    return true;                                                                            \
  }();                                                                                      \
  }                                                                                         \
  static void ef_test_##suite_name##_##test_name()

#define EF_CHECK(condition) ::ef_test::check((condition), __FILE__, __LINE__, #condition)

#define EF_CHECK_MESSAGE(condition, message) \
  ::ef_test::check((condition), __FILE__, __LINE__, std::string(message))

#define EF_CHECK_EQ(lhs, rhs) ::ef_test::check_equal((lhs), (rhs), #lhs, #rhs, __FILE__, __LINE__)

#define EF_REQUIRE(condition)                                     \
  do {                                                            \
    ::ef_test::check((condition), __FILE__, __LINE__, #condition); \
    if (!(condition)) {                                           \
      return;                                                     \
    }                                                             \
  } while (false)

#define EF_REQUIRE_MESSAGE(condition, message)                                        \
  do {                                                                                \
    ::ef_test::check((condition), __FILE__, __LINE__, std::string(message));           \
    if (!(condition)) {                                                               \
      return;                                                                         \
    }                                                                                 \
  } while (false)

#endif  // EXPERIMENT_FABRIC_TESTS_TEST_SUPPORT_HPP
