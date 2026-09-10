// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "example_support.hpp"

/// Rollback to an earlier authoritative experiment generation.
///
/// Rollback restores authority, not evidence. Later history is preserved, the
/// restored generation holds no current evidence until it is executed again,
/// and the rollback itself is recorded as a new authoritative point.

int main() {
  using namespace examples;
  print_banner("rollback to an earlier authoritative generation");

  auto session = start("latency");
  if (!session.ok()) {
    print_status("start", session.status());
    return 1;
  }
  Session& runner = session.value();
  auto worker = ef::scenarios::join(*runner.coordinator, 1);
  if (!worker.ok()) {
    return 1;
  }
  print_status("plan", plan(runner, "latency"));
  print_status("baseline", run_branch(runner, worker.value(), 0, "latency_ms", 120.0, runner.planned(0)));
  print_status("candidate", run_branch(runner, worker.value(), 1, "latency_ms", 95.0, runner.planned(1)));

  const auto finalized =
      runner.coordinator->finalize_experiment(runner.experiment, runner.branches[1], "accepted at generation 1");
  if (!finalized.ok()) {
    print_status("finalize", finalized.status());
    return 1;
  }
  std::cout << "authoritative outcome: " << ef::to_string(finalized.value().outcome) << "\n\n";

  ef::HypothesisRevisionSpec revision;
  revision.statement = "A revision that changes the experiment semantics.";
  print_status("revise hypothesis", runner.coordinator->revise_hypothesis(runner.experiment, revision).status());

  const auto before = runner.coordinator->snapshot(runner.experiment);
  if (!before.ok()) {
    return 1;
  }
  std::cout << "generation after the revision: " << before.value().definition.generation.to_string() << "\n";
  std::cout << "rollback points before: " << before.value().rollback_points.size() << "\n\n";

  print_status("rollback",
               runner.coordinator->rollback(runner.experiment, finalized.value().generation, "restore"));

  const auto after = runner.coordinator->snapshot(runner.experiment);
  if (!after.ok()) {
    return 1;
  }
  std::cout << "generation after the rollback: " << after.value().definition.generation.to_string() << "\n";
  std::cout << "trials preserved: " << after.value().trials.size() << "\n";
  std::cout << "decisions preserved: " << after.value().decisions.size() << "\n";
  std::cout << "rollback points: " << after.value().rollback_points.size() << "\n\n";
  explain(*runner.coordinator, runner.experiment, runner.branches[1]);
  return 0;
}
