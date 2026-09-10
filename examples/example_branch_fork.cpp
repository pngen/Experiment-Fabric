// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "example_support.hpp"

/// Branch fork with a changed parameter.
///
/// A fork preserves lineage without sharing mutable authority. The parent keeps
/// its own branch generation, and the child inherits immutable configuration
/// while carrying its own parameters.

int main() {
  using namespace examples;
  print_banner("branch fork with a changed parameter");

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
  fork.planned_trials = 2;
  fork.parameters = {ef::make_integer_parameter("batch", 64)};
  const auto forked = runner.coordinator->fork_branch(runner.experiment, runner.branches[1], fork);
  if (!forked.ok()) {
    print_status("fork", forked.status());
    return 1;
  }

  const auto snapshot = runner.coordinator->snapshot(runner.experiment);
  if (!snapshot.ok()) {
    return 1;
  }
  print_lines(ef::inspect::render_lineage(snapshot.value(), ef::Limits::defaults()));
  print_lines(ef::inspect::render_branches(snapshot.value(), ef::Limits::defaults()));
  return 0;
}
