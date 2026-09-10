// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "example_support.hpp"

/// Cancelled candidate whose late results are rejected.
///
/// Cancellation is a governed state transition, not a flag. Once the trial is
/// cancelled its attempt can never commit success, and the late evidence is
/// recorded as rejected rather than silently dropped.

int main() {
  using namespace examples;
  print_banner("cancelled candidate rejects late evidence");

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
  auto assignment = ef::scenarios::claim(*runner.coordinator, worker.value());
  if (!assignment.ok()) {
    print_status("claim", assignment.status());
    return 1;
  }
  print_status("cancel trial",
               runner.coordinator->cancel_trial(assignment.value().envelope.trial, "candidate withdrawn"));
  print_status("late observation",
               ef::scenarios::publish(*runner.coordinator, worker.value(), assignment.value(), "latency_ms", 40.0));
  print_status("late completion",
               ef::scenarios::commit(*runner.coordinator, worker.value(), assignment.value()));

  const auto snapshot = runner.coordinator->snapshot(runner.experiment);
  if (!snapshot.ok()) {
    return 1;
  }
  print_lines(ef::inspect::render_trials(snapshot.value(), ef::Limits::defaults()));
  print_lines(ef::inspect::render_rejected_evidence(snapshot.value(), ef::Limits::defaults()));
  explain(*runner.coordinator, runner.experiment, runner.branches[1]);
  return 0;
}
