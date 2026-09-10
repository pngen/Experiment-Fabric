// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "example_support.hpp"

/// A numerically favorable candidate that is not accepted.
///
/// The candidate reports a dramatically better latency, but the policy demands
/// four authoritative completed trials per branch while only one is planned and
/// executed. Insufficient evidence cannot become ACCEPT, and finalization does
/// not force the result into a binary outcome.

int main() {
  using namespace examples;
  print_banner("favorable candidate with insufficient evidence");

  auto session = start("insufficient");
  if (!session.ok()) {
    print_status("start", session.status());
    return 1;
  }
  Session& runner = session.value();
  auto worker = ef::scenarios::join(*runner.coordinator, 1);
  if (!worker.ok()) {
    return 1;
  }
  print_status("plan", plan(runner, "insufficient"));
  print_status("baseline", run_branch(runner, worker.value(), 0, "latency_ms", 120.0, runner.planned(0)));
  print_status("candidate", run_branch(runner, worker.value(), 1, "latency_ms", 40.0, runner.planned(1)));

  const auto snapshot = runner.coordinator->snapshot(runner.experiment);
  if (!snapshot.ok()) {
    return 1;
  }
  explain(*runner.coordinator, runner.experiment, runner.branches[1]);

  const auto finalized =
      runner.coordinator->finalize_experiment(runner.experiment, runner.branches[1], "attempted acceptance");
  if (!finalized.ok()) {
    print_status("finalize", finalized.status());
    return 1;
  }
  const auto after = runner.coordinator->snapshot(runner.experiment);
  if (!after.ok()) {
    return 1;
  }
  std::cout << "finalized outcome: " << ef::to_string(finalized.value().outcome) << "\n";
  std::cout << "experiment state: " << ef::to_string(after.value().state) << "\n";
  std::cout << "rollback points: " << after.value().rollback_points.size() << "\n";
  return 0;
}
