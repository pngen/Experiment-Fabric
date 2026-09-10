// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ef_test {
namespace {

struct Registration {
  std::string suite;
  std::string name;
  TestFunction function;
};

std::vector<Registration>& registry() {
  static std::vector<Registration> entries;
  return entries;
}

std::vector<std::filesystem::path>& cleanups() {
  static std::vector<std::filesystem::path> entries;
  return entries;
}

std::vector<std::uint64_t>& spawned_processes() {
  static std::vector<std::uint64_t> entries;
  return entries;
}

std::string g_current;
std::uint64_t g_failures = 0;
std::uint64_t g_checks = 0;
bool g_current_failed = false;
std::filesystem::path g_scratch_root;

}  // namespace

void register_test(const char* suite, const char* name, TestFunction function) {
  registry().push_back(Registration{suite, name, function});
}

void fail(const char* file, int line, const std::string& message) {
  ++g_failures;
  g_current_failed = true;
  std::cout << "  FAIL " << file << ":" << line << " in " << g_current << ": " << message << "\n";
}

void check(bool condition, const char* file, int line, const std::string& message) {
  ++g_checks;
  if (!condition) {
    fail(file, line, message);
  }
}

const std::string& current_test() { return g_current; }

void register_cleanup(std::filesystem::path path) { cleanups().push_back(std::move(path)); }

void register_spawned_process(std::uint64_t process_id) {
  if (process_id != 0) {
    spawned_processes().push_back(process_id);
  }
}

std::uint32_t terminate_spawned_processes() {
  std::uint32_t forced = 0;
  for (const std::uint64_t identifier : spawned_processes()) {
    HANDLE handle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                static_cast<DWORD>(identifier));
    if (handle == nullptr) {
      continue;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(handle, &code) != FALSE && code == STILL_ACTIVE) {
      (void)TerminateProcess(handle, 9);
      (void)WaitForSingleObject(handle, INFINITE);
      ++forced;
    }
    (void)CloseHandle(handle);
  }
  spawned_processes().clear();
  return forced;
}

std::filesystem::path scratch_directory(const std::string& name) {
  const std::filesystem::path base = g_scratch_root / name;
  std::error_code error;
  std::filesystem::remove_all(base, error);
  std::filesystem::create_directories(base, error);
  register_cleanup(base);
  return base;
}

std::string render(std::uint64_t value) { return std::to_string(value); }
std::string render(std::int64_t value) { return std::to_string(value); }
std::string render(double value) { return std::to_string(value); }
std::string render(const std::string& value) { return value; }
std::string render(bool value) { return value ? "true" : "false"; }

int run_all(int argc, char** argv) {
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    }
  }

  std::error_code error;
  g_scratch_root = std::filesystem::temp_directory_path(error) / "experiment-fabric-tests";
  std::filesystem::remove_all(g_scratch_root, error);
  std::filesystem::create_directories(g_scratch_root, error);

  std::uint64_t executed = 0;
  std::uint64_t failed_tests = 0;
  for (const Registration& entry : registry()) {
    const std::string full = entry.suite + "." + entry.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    g_current = full;
    g_current_failed = false;
    ++executed;
    std::cout << "RUN  " << full << "\n";
    try {
      entry.function();
    } catch (const std::exception& exception) {
      fail(__FILE__, __LINE__, std::string("unexpected exception: ") + exception.what());
    } catch (...) {
      fail(__FILE__, __LINE__, "unexpected non-standard exception");
    }
    if (g_current_failed) {
      ++failed_tests;
      std::cout << "FAILED " << full << "\n";
    }
  }

  const std::uint32_t forced = terminate_spawned_processes();
  if (forced != 0) {
    std::cout << "harness terminated " << forced << " surviving child processes\n";
  }

  for (const std::filesystem::path& path : cleanups()) {
    std::filesystem::remove_all(path, error);
  }
  std::filesystem::remove_all(g_scratch_root, error);

  std::cout << "executed " << executed << " tests, " << g_checks << " assertions, " << failed_tests
            << " failed tests, " << g_failures << " failed assertions\n";
  return failed_tests == 0 ? 0 : 1;
}

}  // namespace ef_test

int main(int argc, char** argv) { return ef_test::run_all(argc, argv); }
