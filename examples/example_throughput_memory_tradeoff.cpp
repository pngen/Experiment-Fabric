// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "example_support.hpp"

/// Throughput versus memory tradeoff.
///
/// Two metrics with opposite directions are compared under one policy: the
/// throughput gain must be real and the memory regression must stay inside a
/// bounded tolerance. No unrelated units are collapsed into a single score.

namespace {

namespace ef = experiment_fabric;

void run_scenario(double memory, const std::string& label) {
  auto session = examples::start("throughput-memory");
  if (!session.ok()) {
    examples::print_status("start", session.status());
    return;
  }
  examples::Session& runner = session.value();
  auto worker = ef::scenarios::join(*runner.coordinator, 1);
  if (!worker.ok()) {
    return;
  }
  // The candidate parameters from the scenario are replaced by the requested
  // memory footprint so that both the accepted and the rejected case are shown.
  const auto snapshot = runner.coordinator->snapshot(runner.experiment);
  if (!snapshot.ok()) {
    return;
  }
  const ef::BranchId candidate = snapshot.value().definition.branches[1].id;
  ef::BranchSpec replacement;
  replacement.name = "candidate-strict";
  replacement.role = ef::BranchRole::CANDIDATE;
  replacement.planned_trials = 2;
  replacement.parameters = {ef::make_real_parameter("throughput", 1250.0),
                            ef::make_real_parameter("peak_memory_mb", memory)};
  if (!runner.coordinator->retire_branch(runner.experiment, candidate, "replaced by the strict candidate").ok()) {
    return;
  }
  const auto forked = runner.coordinator->fork_branch(runner.experiment, runner.branches[0], replacement);
  if (!forked.ok()) {
    examples::print_status("fork", forked.status());
    return;
  }
  if (!examples::plan(runner, "tradeoff").ok()) {
    return;
  }
  std::cout << "--- " << label << " ---\n";
  examples::print_status("baseline", examples::run_branch(runner, worker.value(), 0, "throughput", 1000.0, 2));
  for (int index = 0; index < 2; ++index) {
    const auto assignment = ef::scenarios::claim(*runner.coordinator, worker.value());
    if (!assignment.ok()) {
      break;
    }
    (void)ef::scenarios::publish(*runner.coordinator, worker.value(), assignment.value(), "throughput", 1250.0);
    (void)ef::scenarios::publish(*runner.coordinator, worker.value(), assignment.value(), "peak_memory_mb", memory);
    (void)ef::scenarios::commit(*runner.coordinator, worker.value(), assignment.value());
  }
  examples::explain(*runner.coordinator, runner.experiment, forked.value());
}

}  // namespace

int main() {
  examples::print_banner("throughput versus memory tradeoff");
  // Inside the 32 MiB tolerance: accepted.
  run_scenario(530.0, "memory regression inside the configured tolerance");
  // Beyond the tolerance: rejected even though throughput improved.
  run_scenario(700.0, "memory regression beyond the configured tolerance");
  return 0;
}
