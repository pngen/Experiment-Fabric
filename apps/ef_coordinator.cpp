// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "experiment_fabric/coordinator.hpp"
#include "experiment_fabric/version.hpp"
#include "scenarios.hpp"

namespace {

void print_usage() {
  std::cout << "ef_coordinator " << experiment_fabric::version_string() << "\n"
            << "  --state <path>        durable state image (empty for in-memory)\n"
            << "  --port <n>            control-plane port (0 chooses an ephemeral port)\n"
            << "  --bind <address>      bind address (default 127.0.0.1)\n"
            << "  --scenario <name>     create a reference experiment at start\n"
            << "  --trials <n>          create the planned trials of every branch (0 or 1)\n"
            << "  --epoch-seed <n>      lower bound for the coordinator epoch\n"
            << "  --port-file <path>    write the bound port once the control plane is ready\n"
            << "  --list-scenarios      print the built-in scenario names and exit\n";
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
    std::string state_path;
    std::string bind_address = "127.0.0.1";
    std::string scenario;
    std::string port_file;
    std::uint16_t port = 0;
    std::uint64_t epoch_seed = 0;
    bool create_trials = true;

    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--list-scenarios") {
        for (const std::string& name : experiment_fabric::scenarios::scenario_names()) {
          std::cout << name << "\n";
        }
        return 0;
      }
      if (argument == "--help" || argument == "-h") {
        print_usage();
        return 0;
      }
      std::string value;
      if (argument == "--state") {
        if (!take_value(argc, argv, index, value)) return 2;
        state_path = value;
      } else if (argument == "--bind") {
        if (!take_value(argc, argv, index, value)) return 2;
        bind_address = value;
      } else if (argument == "--scenario") {
        if (!take_value(argc, argv, index, value)) return 2;
        scenario = value;
      } else if (argument == "--port-file") {
        if (!take_value(argc, argv, index, value)) return 2;
        port_file = value;
      } else if (argument == "--port") {
        if (!take_value(argc, argv, index, value)) return 2;
        port = static_cast<std::uint16_t>(std::stoul(value));
      } else if (argument == "--epoch-seed") {
        if (!take_value(argc, argv, index, value)) return 2;
        epoch_seed = std::stoull(value);
      } else if (argument == "--trials") {
        if (!take_value(argc, argv, index, value)) return 2;
        create_trials = std::stoul(value) != 0;
      } else {
        std::cerr << "unknown argument: " << argument << "\n";
        print_usage();
        return 2;
      }
    }

    experiment_fabric::Coordinator::Config config;
    config.limits = experiment_fabric::Limits::defaults();
    config.bind_address = bind_address;
    config.listen_port = port;
    config.epoch_seed = epoch_seed;
    if (!state_path.empty()) {
      config.state_path = std::filesystem::path(state_path);
    }

    auto coordinator = experiment_fabric::Coordinator::create(std::move(config));
    if (!coordinator.ok()) {
      std::cerr << "coordinator creation failed: " << coordinator.status().to_string() << "\n";
      return 3;
    }

    experiment_fabric::ExperimentId experiment;
    if (!scenario.empty()) {
      experiment_fabric::ExperimentSpec spec;
      std::string error;
      if (!experiment_fabric::scenarios::build_scenario(scenario, spec, error)) {
        std::cerr << error << "\n";
        return 2;
      }
      auto created = coordinator.value()->create_experiment(spec);
      if (!created.ok()) {
        std::cerr << "experiment creation failed: " << created.status().to_string() << "\n";
        return 3;
      }
      experiment = created.value();
      if (create_trials) {
        const experiment_fabric::Status status =
            experiment_fabric::scenarios::create_planned_trials(*coordinator.value(), experiment, "scenario-trial");
        if (!status.ok()) {
          std::cerr << "trial creation failed: " << status.to_string() << "\n";
          return 3;
        }
      }
    }

    const experiment_fabric::Status started = coordinator.value()->start_server();
    if (!started.ok()) {
      std::cerr << "control plane failed to start: " << started.to_string() << "\n";
      return 3;
    }

    if (!port_file.empty()) {
      std::ofstream stream(port_file, std::ios::trunc);
      stream << coordinator.value()->port() << "\n";
      stream.flush();
      stream.close();
    }
    std::cout << "coordinator-ready epoch=" << coordinator.value()->epoch().to_string()
              << " port=" << coordinator.value()->port()
              << " experiment=" << (experiment.valid() ? experiment.to_string() : std::string("none")) << "\n";
    std::cout.flush();

    // A supervisor blocks on a real rendezvous; the runtime never polls.
    coordinator.value()->wait_for_shutdown_request();
    (void)coordinator.value()->stop_server();
    std::cout << "coordinator-stopped\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "fatal: " << exception.what() << "\n";
    return 4;
  }
}
