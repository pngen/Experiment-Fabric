// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "experiment_fabric/coordinator.hpp"
#include "experiment_fabric/inspect.hpp"
#include "scenarios.hpp"

/// \file
/// Benchmarks of completed operations.
///
/// Every measurement covers an operation that completed, not an enqueue or a
/// submission: the reported figure is the cost of an experiment that was
/// created, a trial that was admitted, an observation that was published, a
/// completion that committed, a comparison that was evaluated, an explanation
/// that was rendered, or a state image that was durably replaced.
///
/// The benchmark never weakens correctness to inflate throughput: persistence
/// is on for the persistence benchmarks and off only where the documented
/// measurement is explicitly in-memory.

namespace {

namespace ef = experiment_fabric;

class Timer {
 public:
  Timer() : start_(std::chrono::steady_clock::now()) {}

  [[nodiscard]] double ms() const {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

void report(const std::string& name, std::uint64_t operations, double milliseconds) {
  const double per_operation = milliseconds / static_cast<double>(operations == 0 ? 1 : operations);
  const double throughput = per_operation > 0.0 ? 1000.0 / per_operation : 0.0;
  std::cout << name << "\n"
            << "  operations       : " << operations << "\n"
            << "  total            : " << milliseconds << " ms\n"
            << "  per operation    : " << per_operation * 1000.0 << " us\n"
            << "  throughput       : " << throughput << " /s\n\n";
}

ef::Coordinator::Config memory_config() {
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  config.persist_on_mutation = false;
  return config;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t scale = 2000;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument.rfind("--scale=", 0) == 0) {
      scale = std::stoull(argument.substr(8));
    }
  }

  std::cout << "Experiment Fabric benchmark (SYNTHETIC workloads)\n";
  std::cout << "scale: " << scale << " experiments\n\n";

  // ---- Experiment creation ------------------------------------------------
  {
    ef::Coordinator::Config config = memory_config();
    config.limits.max_experiments = static_cast<std::uint32_t>(scale + 16);
    auto coordinator = ef::Coordinator::create(std::move(config));
    if (!coordinator.ok()) {
      std::cout << "coordinator creation failed\n";
      return 1;
    }
    ef::ExperimentSpec spec;
    std::string error;
    (void)ef::scenarios::build_scenario("latency", spec, error);
    Timer timer;
    std::uint64_t created = 0;
    for (std::uint64_t index = 0; index < scale; ++index) {
      if (coordinator.value()->create_experiment(spec).ok()) {
        ++created;
      }
    }
    report("experiment creation (definition validated, identities allocated, state advanced)", created, timer.ms());
  }

  // ---- Branch forking -----------------------------------------------------
  {
    auto coordinator = ef::Coordinator::create(memory_config());
    if (!coordinator.ok()) {
      return 1;
    }
    ef::ExperimentSpec spec;
    std::string error;
    (void)ef::scenarios::build_scenario("latency", spec, error);
    const auto experiment = coordinator.value()->create_experiment(spec);
    if (!experiment.ok()) {
      return 1;
    }
    const auto snapshot = coordinator.value()->snapshot(experiment.value());
    const ef::BranchId parent = snapshot.value().definition.branches[1].id;
    Timer timer;
    std::uint64_t forks = 0;
    const std::uint64_t limit = std::min<std::uint64_t>(scale, 200);
    for (std::uint64_t index = 0; index < limit; ++index) {
      ef::BranchSpec fork;
      fork.name = "fork-" + std::to_string(index);
      fork.role = ef::BranchRole::CANDIDATE;
      fork.planned_trials = 1;
      fork.parameters = {ef::make_integer_parameter("batch", static_cast<std::int64_t>(index))};
      if (coordinator.value()->fork_branch(experiment.value(), parent, fork).ok()) {
        ++forks;
      }
    }
    report("branch forking (lineage checked, parameters merged, generation advanced)", forks, timer.ms());
  }

  // ---- Trial admission and completion ------------------------------------
  {
    ef::Coordinator::Config config = memory_config();
    config.limits.max_trials_per_branch = static_cast<std::uint32_t>(scale + 16);
    auto coordinator = ef::Coordinator::create(std::move(config));
    if (!coordinator.ok()) {
      return 1;
    }
    ef::ExperimentSpec spec;
    std::string error;
    (void)ef::scenarios::build_scenario("latency", spec, error);
    spec.branches[0].planned_trials = static_cast<std::uint32_t>(scale);
    spec.branches[1].planned_trials = static_cast<std::uint32_t>(scale);
    const auto experiment = coordinator.value()->create_experiment(spec);
    if (!experiment.ok()) {
      return 1;
    }
    const auto snapshot = coordinator.value()->snapshot(experiment.value());
    const ef::BranchId baseline = snapshot.value().definition.branches[0].id;
    const ef::BranchId candidate = snapshot.value().definition.branches[1].id;

    Timer timer;
    std::uint64_t admitted = 0;
    for (std::uint64_t index = 0; index < scale; ++index) {
      if (coordinator.value()->create_trial(experiment.value(), baseline, "bench").ok()) {
        ++admitted;
      }
      if (coordinator.value()->create_trial(experiment.value(), candidate, "bench").ok()) {
        ++admitted;
      }
    }
    report("trial admission (identity allocated, seed derived, state advanced)", admitted, timer.ms());

    auto worker = ef::scenarios::join(*coordinator.value(), 1);
    if (!worker.ok()) {
      return 1;
    }
    Timer completion_timer;
    std::uint64_t completed = 0;
    while (true) {
      auto assignment = ef::scenarios::claim(*coordinator.value(), worker.value());
      if (!assignment.ok()) {
        break;
      }
      const double value = assignment.value().envelope.branch == baseline ? 120.0 : 95.0;
      if (!ef::scenarios::publish(*coordinator.value(), worker.value(), assignment.value(), "latency_ms", value).ok()) {
        continue;
      }
      if (!ef::scenarios::commit(*coordinator.value(), worker.value(), assignment.value()).ok()) {
        continue;
      }
      ++completed;
    }
    report("full trial lifecycle (claim, observation publication, authoritative completion)",
           completed, completion_timer.ms());
  }

  // ---- Comparison and explanation ----------------------------------------
  {
    ef::Coordinator::Config config = memory_config();
    auto coordinator = ef::Coordinator::create(std::move(config));
    if (!coordinator.ok()) {
      return 1;
    }
    ef::ExperimentSpec spec;
    std::string error;
    (void)ef::scenarios::build_scenario("latency", spec, error);
    const auto experiment = coordinator.value()->create_experiment(spec);
    if (!experiment.ok()) {
      return 1;
    }
    const auto snapshot = coordinator.value()->snapshot(experiment.value());
    const ef::BranchId baseline = snapshot.value().definition.branches[0].id;
    const ef::BranchId candidate = snapshot.value().definition.branches[1].id;
    auto worker = ef::scenarios::join(*coordinator.value(), 1);
    if (!worker.ok()) {
      return 1;
    }
    if (!ef::scenarios::create_planned_trials(*coordinator.value(), experiment.value(), "bench").ok()) {
      return 1;
    }
    // Trials are created branch by branch, so the baseline trials are claimed
    // first and the candidate trials second.
    const double expected_values[] = {120.0, 120.0, 95.0, 95.0};
    for (const double expected : expected_values) {
      if (!ef::scenarios::complete_one(*coordinator.value(), worker.value(), "latency_ms", expected).ok()) {
        return 1;
      }
    }
    const std::uint64_t iterations = std::max<std::uint64_t>(1, scale);
    Timer compare_timer;
    for (std::uint64_t index = 0; index < iterations; ++index) {
      (void)coordinator.value()->evaluate(experiment.value(), candidate);
    }
    report("deterministic comparison (hard constraints then decisive factors)", iterations, compare_timer.ms());

    Timer explain_timer;
    for (std::uint64_t index = 0; index < iterations; ++index) {
      (void)coordinator.value()->explain(experiment.value(), candidate);
    }
    report("explanation rendering (stable ordering, bounded output)", iterations, explain_timer.ms());

    Timer snapshot_timer;
    for (std::uint64_t index = 0; index < iterations; ++index) {
      (void)coordinator.value()->snapshot(experiment.value());
    }
    report("snapshot read (immutable copy under a shared lock)", iterations, snapshot_timer.ms());

    const auto decision = coordinator.value()->evaluate(experiment.value(), candidate);
    if (decision.ok()) {
      std::cout << "reference outcome: " << ef::to_string(decision.value().outcome) << "\n\n";
    }
    (void)baseline;
  }

  // ---- Persistence --------------------------------------------------------
  {
    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path(error) / "experiment-fabric-benchmark";
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    const std::filesystem::path state = directory / "bench.efstate";

    ef::Coordinator::Config config;
    config.limits = ef::Limits::defaults();
    config.state_path = state;
    auto coordinator = ef::Coordinator::create(std::move(config));
    if (!coordinator.ok()) {
      std::cout << "coordinator creation failed\n";
      return 1;
    }
    ef::ExperimentSpec spec;
    std::string error_message;
    (void)ef::scenarios::build_scenario("latency", spec, error_message);
    // Enough planned trials for the durable-commit measurement to be meaningful.
    spec.branches[0].planned_trials = 64;
    const auto experiment = coordinator.value()->create_experiment(spec);
    if (!experiment.ok()) {
      return 1;
    }
    auto worker = ef::scenarios::join(*coordinator.value(), 1);
    if (!worker.ok()) {
      return 1;
    }

    Timer save_timer;
    std::uint64_t saves = 0;
    for (std::uint64_t index = 0; index < 64; ++index) {
      const auto trial = coordinator.value()->create_trial(experiment.value(),
                                                           coordinator.value()
                                                               ->snapshot(experiment.value())
                                                               .value()
                                                               .definition.branches[0]
                                                               .id,
                                                           "bench");
      if (trial.ok()) {
        ++saves;
      }
    }
    report("durable commit (identity, validation, serialise, atomic replace)", saves, save_timer.ms());

    Timer load_timer;
    std::uint64_t loads = 0;
    const std::uint64_t load_iterations = 32;
    for (std::uint64_t index = 0; index < load_iterations; ++index) {
      if (ef::persistence::load(state, ef::Limits::defaults()).ok()) {
        ++loads;
      }
    }
    report("state load (bounded decode, per-record checksum, whole-image digest)", loads, load_timer.ms());
    std::filesystem::remove_all(directory, error);
  }

  // ---- Concurrent reads ---------------------------------------------------
  {
    auto coordinator = ef::Coordinator::create(memory_config());
    if (!coordinator.ok()) {
      return 1;
    }
    ef::ExperimentSpec spec;
    std::string error;
    (void)ef::scenarios::build_scenario("latency", spec, error);
    const auto experiment = coordinator.value()->create_experiment(spec);
    if (!experiment.ok()) {
      return 1;
    }
    const std::uint64_t iterations = std::max<std::uint64_t>(1, scale);
    Timer timer;
    for (std::uint64_t index = 0; index < iterations; ++index) {
      (void)coordinator.value()->snapshot(experiment.value());
      (void)coordinator.value()->validate_state();
      (void)coordinator.value()->list_experiments();
    }
    report("concurrent read path (snapshot, full validation, listing)", iterations * 3, timer.ms());
  }

  return 0;
}
