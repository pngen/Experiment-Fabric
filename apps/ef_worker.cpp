// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#include "experiment_fabric/version.hpp"
#include "experiment_fabric/worker.hpp"
#include "scenarios.hpp"

namespace {

void print_usage() {
  std::cout << "ef_worker " << experiment_fabric::version_string() << "\n"
            << "  --host <address>          coordinator address (default 127.0.0.1)\n"
            << "  --port <n>                coordinator control-plane port\n"
            << "  --worker <n>              logical worker identity\n"
            << "  --workload <name>         reference workload name\n"
            << "  --claims <n>              maximum concurrent assignments (default 1)\n"
            << "  --cycles <n>              number of claim cycles before exiting (0 = until idle)\n"
            << "  --frame-log <path>        record every frame this process sends\n"
            << "  --fingerprint <text>      producer fingerprint recorded at registration\n"
            << "  --die-after-publish <n>   terminate the process without cleanup after n published observations\n"
            << "  --list-workloads          print the built-in workload names and exit\n";
}

bool take_value(int argc, char** argv, int& index, std::string& out) {
  if (index + 1 >= argc) {
    return false;
  }
  out = argv[++index];
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::string host = "127.0.0.1";
    std::string workload = "parameter";
    std::string fingerprint = "reference-worker";
    std::string frame_log;
    std::uint16_t port = 0;
    std::uint64_t worker_identity = 1;
    std::uint32_t claims = 1;
    std::uint64_t cycles = 1;
    std::uint64_t die_after_publish = 0;

    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--list-workloads") {
        for (const std::string& name : experiment_fabric::scenarios::workload_names()) {
          std::cout << name << "\n";
        }
        return 0;
      }
      if (argument == "--help" || argument == "-h") {
        print_usage();
        return 0;
      }
      std::string value;
      if (argument == "--host") {
        if (!take_value(argc, argv, index, value)) return 2;
        host = value;
      } else if (argument == "--workload") {
        if (!take_value(argc, argv, index, value)) return 2;
        workload = value;
      } else if (argument == "--fingerprint") {
        if (!take_value(argc, argv, index, value)) return 2;
        fingerprint = value;
      } else if (argument == "--frame-log") {
        if (!take_value(argc, argv, index, value)) return 2;
        frame_log = value;
      } else if (argument == "--port") {
        if (!take_value(argc, argv, index, value)) return 2;
        port = static_cast<std::uint16_t>(std::stoul(value));
      } else if (argument == "--worker") {
        if (!take_value(argc, argv, index, value)) return 2;
        worker_identity = std::stoull(value);
      } else if (argument == "--claims") {
        if (!take_value(argc, argv, index, value)) return 2;
        claims = static_cast<std::uint32_t>(std::stoul(value));
      } else if (argument == "--cycles") {
        if (!take_value(argc, argv, index, value)) return 2;
        cycles = std::stoull(value);
      } else if (argument == "--die-after-publish") {
        if (!take_value(argc, argv, index, value)) return 2;
        die_after_publish = std::stoull(value);
      } else {
        std::cerr << "unknown argument: " << argument << "\n";
        print_usage();
        return 2;
      }
    }

    experiment_fabric::Worker::Config config;
    config.host = host;
    config.port = port;
    config.worker = experiment_fabric::WorkerId::from_value(worker_identity);
    config.endpoint = host + ":" + std::to_string(port);
    config.fingerprint = fingerprint;
    config.max_concurrent_assignments = claims;
    config.limits = experiment_fabric::Limits::defaults();
    if (!frame_log.empty()) {
      config.frame_log_path = std::filesystem::path(frame_log);
    }
    if (die_after_publish != 0) {
      // Real process death at an exact lifecycle point: std::_Exit performs no
      // cleanup, runs no destructor, and closes no socket gracefully.
      config.progress_hook = [die_after_publish](std::uint64_t observations, std::uint32_t trials) {
        (void)trials;
        if (observations >= die_after_publish) {
          std::cout << "worker-terminating-after-publish " << observations << "\n";
          std::cout.flush();
          // Exit code 9 is reserved for a deliberate, unclean process death.
          std::_Exit(9);
        }
      };
    }

    auto worker = experiment_fabric::Worker::create(std::move(config));
    if (!worker.ok()) {
      std::cerr << "worker creation failed: " << worker.status().to_string() << "\n";
      return 3;
    }
    const experiment_fabric::Status registered = worker.value()->connect_and_register();
    if (!registered.ok()) {
      std::cerr << "registration failed: " << registered.to_string() << "\n";
      return 4;
    }
    std::cout << "worker-registered worker=" << worker_identity
              << " boot=" << worker.value()->boot().to_string()
              << " epoch=" << worker.value()->epoch().to_string()
              << " producer=" << worker.value()->producer().to_string() << "\n";
    std::cout.flush();

    const auto task = [&workload](const experiment_fabric::TrialAssignment& assignment,
                                  const experiment_fabric::WorkerTaskContext& context) {
      return experiment_fabric::scenarios::run_workload(workload, assignment, context);
    };

    std::uint64_t completed_cycles = 0;
    while (true) {
      const experiment_fabric::Status status = worker.value()->run_once(task);
      if (!status.ok()) {
        if (status.code() == experiment_fabric::ErrorCode::NOT_FOUND) {
          break;
        }
        std::cerr << "worker run_once failed: " << status.to_string() << "\n";
        return 5;
      }
      ++completed_cycles;
      if (cycles != 0 && completed_cycles >= cycles) {
        break;
      }
    }
    std::cout << "worker-finished published=" << worker.value()->published_trials()
              << " failed=" << worker.value()->failed_trials()
              << " boot=" << worker.value()->boot().to_string() << "\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "fatal: " << exception.what() << "\n";
    return 4;
  }
}
