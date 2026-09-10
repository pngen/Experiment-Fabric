// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "example_support.hpp"

/// A failed trial followed by a fresh attempt.
///
/// A retry preserves the logical trial identity and fences the individual
/// attempt. The failed attempt is never overwritten, and only one attempt may
/// authoritatively complete the logical trial.

int main() {
  using namespace examples;
  print_banner("failed trial followed by a fresh attempt");

  auto session = start("retry");
  if (!session.ok()) {
    print_status("start", session.status());
    return 1;
  }
  Session& runner = session.value();
  auto worker = ef::scenarios::join(*runner.coordinator, 1);
  if (!worker.ok()) {
    return 1;
  }
  print_status("plan", plan(runner, "retry"));
  print_status("baseline", run_branch(runner, worker.value(), 0, "latency_ms", 120.0, runner.planned(0)));

  auto first = ef::scenarios::claim(*runner.coordinator, worker.value());
  if (!first.ok()) {
    print_status("claim", first.status());
    return 1;
  }
  ef::FailTrialRequest failure;
  failure.epoch = worker.value().epoch;
  failure.worker = worker.value().worker;
  failure.boot = worker.value().boot;
  failure.envelope = first.value().envelope;
  failure.kind = ef::FailureKind::EXECUTION_FAILURE;
  failure.detail = "synthetic transient failure";
  failure.retryable = true;
  print_status("fail attempt 1", runner.coordinator->fail_trial(failure));

  auto second = ef::scenarios::claim(*runner.coordinator, worker.value());
  if (!second.ok()) {
    print_status("re-claim", second.status());
    return 1;
  }
  std::cout << "logical trial preserved: " << (second.value().envelope.trial == first.value().envelope.trial)
            << ", attempt advanced: " << (second.value().envelope.attempt != first.value().envelope.attempt)
            << "\n\n";
  print_status("late result from attempt 1",
               ef::scenarios::publish(*runner.coordinator, worker.value(), first.value(), "latency_ms", 1.0));
  print_status("publish attempt 2",
               ef::scenarios::publish(*runner.coordinator, worker.value(), second.value(), "latency_ms", 95.0));
  print_status("commit attempt 2", ef::scenarios::commit(*runner.coordinator, worker.value(), second.value()));

  const auto snapshot = runner.coordinator->snapshot(runner.experiment);
  if (!snapshot.ok()) {
    return 1;
  }
  print_lines(ef::inspect::render_trials(snapshot.value(), ef::Limits::defaults()));
  return 0;
}
