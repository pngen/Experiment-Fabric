// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_EXAMPLES_EXAMPLE_SUPPORT_HPP
#define EXPERIMENT_FABRIC_EXAMPLES_EXAMPLE_SUPPORT_HPP

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "experiment_fabric/coordinator.hpp"
#include "experiment_fabric/inspect.hpp"
#include "experiment_fabric/version.hpp"
#include "scenarios.hpp"

/// \file
/// Shared plumbing for the reference examples.
///
/// Every example is a SYNTHETIC systems experiment. The workloads are
/// deterministic simulations carried as branch parameters, so the same examples
/// apply unchanged to compiler-optimization, scheduling, model-selection,
/// inference-configuration, kernel-tuning, agent-strategy and
/// systems-performance experiments.

namespace examples {

namespace ef = experiment_fabric;

inline void print_banner(const std::string& title) {
  std::cout << "=== " << title << " ===\n";
  std::cout << "experiment-fabric " << ef::version_string() << " (SYNTHETIC reference workload)\n\n";
}

inline void print_lines(const std::vector<std::string>& lines) {
  for (const std::string& line : lines) {
    std::cout << line << "\n";
  }
  std::cout << "\n";
}

inline void print_status(const std::string& label, const ef::Status& status) {
  std::cout << label << ": " << (status.ok() ? std::string("OK") : status.to_string()) << "\n";
}

inline ef::Coordinator::Config default_config() {
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  return config;
}

/// A running reference experiment.
struct Session {
  std::unique_ptr<ef::Coordinator> coordinator;
  ef::ExperimentId experiment;
  std::vector<ef::BranchId> branches;
  ef::MetricId metric;

  [[nodiscard]] const ef::BranchDefinition* branch(std::size_t index) const {
    const auto snapshot = coordinator->snapshot(experiment);
    if (!snapshot.ok() || index >= snapshot.value().definition.branches.size()) {
      return nullptr;
    }
    return &snapshot.value().definition.branches[index];
  }

  [[nodiscard]] std::uint32_t planned(std::size_t index) const {
    const ef::BranchDefinition* definition = branch(index);
    return definition == nullptr ? 0 : definition->planned_trials;
  }
};

/// Creates a coordinator and one scenario experiment.
inline ef::Result<Session> start(const std::string& scenario,
                                 std::optional<std::filesystem::path> state_path = std::nullopt) {
  ef::Coordinator::Config config = default_config();
  if (state_path.has_value()) {
    config.state_path = *state_path;
  }
  auto coordinator = ef::Coordinator::create(std::move(config));
  if (!coordinator.ok()) {
    return coordinator.status();
  }
  ef::ExperimentSpec spec;
  std::string error;
  if (!ef::scenarios::build_scenario(scenario, spec, error)) {
    return ef::make_error(ef::ErrorCode::NOT_FOUND, ef::ErrorStage::VALIDATION, error);
  }
  auto experiment = coordinator.value()->create_experiment(spec);
  if (!experiment.ok()) {
    return experiment.status();
  }
  Session session;
  session.coordinator = std::move(coordinator.value());
  session.experiment = experiment.value();
  const auto snapshot = session.coordinator->snapshot(session.experiment);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  for (const ef::BranchDefinition& branch : snapshot.value().definition.branches) {
    session.branches.push_back(branch.id);
  }
  if (!snapshot.value().definition.metrics.empty()) {
    session.metric = snapshot.value().definition.metrics.front().id;
  }
  // An explicit move: the session owns its coordinator, so it is move-only.
  return ef::Result<Session>(std::move(session));
}

/// Creates the planned trials of every eligible branch.
inline ef::Status plan(Session& session, const std::string& payload) {
  return ef::scenarios::create_planned_trials(*session.coordinator, session.experiment, payload);
}

/// Completes the planned trials of one branch with a fixed SYNTHETIC value.
inline ef::Status run_branch(Session& session, const ef::scenarios::Session& worker, std::size_t branch_index,
                             std::string_view metric, double value, std::uint32_t trials) {
  for (std::uint32_t index = 0; index < trials; ++index) {
    const ef::Status status = ef::scenarios::complete_one(*session.coordinator, worker, metric, value);
    if (!status.ok()) {
      return status;
    }
  }
  (void)branch_index;
  return ef::Status::success();
}

/// Prints the deterministic explanation of one comparison.
inline void explain(ef::Coordinator& coordinator, ef::ExperimentId experiment, ef::BranchId candidate) {
  const auto decision = coordinator.evaluate(experiment, candidate);
  if (!decision.ok()) {
    print_status("evaluate", decision.status());
    return;
  }
  print_lines(ef::inspect::render_decision(decision.value(), ef::Limits::defaults()));
}

}  // namespace examples

#endif  // EXPERIMENT_FABRIC_EXAMPLES_EXAMPLE_SUPPORT_HPP
