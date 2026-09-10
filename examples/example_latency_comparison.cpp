// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "example_support.hpp"

/// Baseline versus candidate latency.
///
/// Both branches execute the same SYNTHETIC workload; only the branch parameter
/// differs. The candidate becomes authoritative only because its trials were
/// completed, committed and compared as current evidence.

int main() {
  using namespace examples;
  print_banner("baseline versus candidate latency");

  auto session = start("latency");
  if (!session.ok()) {
    print_status("start", session.status());
    return 1;
  }
  Session& runner = session.value();
  auto worker = ef::scenarios::join(*runner.coordinator, 1);
  if (!worker.ok()) {
    print_status("join", worker.status());
    return 1;
  }
  print_status("plan", plan(runner, "latency"));
  print_status("baseline", run_branch(runner, worker.value(), 0, "latency_ms", 120.0, runner.planned(0)));
  print_status("candidate", run_branch(runner, worker.value(), 1, "latency_ms", 95.0, runner.planned(1)));

  const auto snapshot = runner.coordinator->snapshot(runner.experiment);
  if (!snapshot.ok()) {
    return 1;
  }
  print_lines(ef::inspect::render_trials(snapshot.value(), ef::Limits::defaults()));
  explain(*runner.coordinator, runner.experiment, runner.branches[1]);
  return 0;
}
