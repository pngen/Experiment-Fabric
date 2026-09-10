// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "example_support.hpp"

/// Lineage inspection.
///
/// Lineage is durable: experiment, hypothesis revision, branch, trial, attempt,
/// observation, artifact, comparison and decision remain queryable and acyclic.
/// A rollback shows that lineage survives supersession.

int main() {
  using namespace examples;
  print_banner("lineage inspection");

  auto session = start("forked-parameter");
  if (!session.ok()) {
    print_status("start", session.status());
    return 1;
  }
  Session& runner = session.value();
  auto worker = ef::scenarios::join(*runner.coordinator, 1);
  if (!worker.ok()) {
    return 1;
  }

  ef::BranchSpec fork;
  fork.name = "batch-64";
  fork.role = ef::BranchRole::CANDIDATE;
  fork.planned_trials = 1;
  fork.parameters = {ef::make_integer_parameter("batch", 64)};
  const auto forked = runner.coordinator->fork_branch(runner.experiment, runner.branches[1], fork);
  if (!forked.ok()) {
    print_status("fork", forked.status());
    return 1;
  }
  ef::BranchSpec grandchild;
  grandchild.name = "batch-128";
  grandchild.role = ef::BranchRole::CANDIDATE;
  grandchild.planned_trials = 1;
  grandchild.parameters = {ef::make_integer_parameter("batch", 128)};
  const auto second = runner.coordinator->fork_branch(runner.experiment, forked.value(), grandchild);
  if (!second.ok()) {
    print_status("second fork", second.status());
    return 1;
  }

  print_status("plan", plan(runner, "lineage"));
  print_status("baseline", run_branch(runner, worker.value(), 0, "throughput", 1000.0, runner.planned(0)));

  const auto snapshot = runner.coordinator->snapshot(runner.experiment);
  if (!snapshot.ok()) {
    return 1;
  }
  print_lines(ef::inspect::render_lineage(snapshot.value(), ef::Limits::defaults()));
  print_lines(ef::inspect::render_hypothesis(snapshot.value(), ef::Limits::defaults()));
  print_lines(ef::inspect::render_trials(snapshot.value(), ef::Limits::defaults()));
  return 0;
}
