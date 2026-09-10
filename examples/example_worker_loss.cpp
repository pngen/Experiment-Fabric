// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "example_support.hpp"

/// Worker loss followed by a fresh-boot retry.
///
/// A restarted worker must not inherit prior process authority. Registering a
/// fresh boot identity revokes the previous incarnation, transitions its
/// unfinished assignment conservatively, and fences its later traffic.
///
/// This example is SYNTHETIC: the loss is reproduced by replacing the worker
/// incarnation inside one coordinator process. The real operating-system proof
/// lives in the multi-process test suite.

int main() {
  using namespace examples;
  print_banner("worker loss and fresh-boot retry");

  auto session = start("authority");
  if (!session.ok()) {
    print_status("start", session.status());
    return 1;
  }
  Session& runner = session.value();
  auto first_boot = ef::scenarios::join(*runner.coordinator, 1, 0x1111);
  if (!first_boot.ok()) {
    return 1;
  }
  print_status("plan", plan(runner, "authority"));
  auto assignment = ef::scenarios::claim(*runner.coordinator, first_boot.value());
  if (!assignment.ok()) {
    print_status("claim", assignment.status());
    return 1;
  }

  auto second_boot = ef::scenarios::join(*runner.coordinator, 1, 0x2222);
  if (!second_boot.ok()) {
    print_status("rejoin", second_boot.status());
    return 1;
  }
  std::cout << "boot identity changed: " << (second_boot.value().boot != first_boot.value().boot) << "\n";
  print_status("dead incarnation publishes",
               ef::scenarios::publish(*runner.coordinator, first_boot.value(), assignment.value(), "latency_ms", 1.0));

  auto reclaimed = ef::scenarios::claim(*runner.coordinator, second_boot.value());
  if (!reclaimed.ok()) {
    print_status("reclaim", reclaimed.status());
    return 1;
  }
  std::cout << "same logical trial: " << (reclaimed.value().envelope.trial == assignment.value().envelope.trial)
            << ", fresh attempt: " << (reclaimed.value().envelope.attempt != assignment.value().envelope.attempt)
            << "\n\n";
  print_status("fresh incarnation publishes",
               ef::scenarios::publish(*runner.coordinator, second_boot.value(), reclaimed.value(), "latency_ms", 118.0));
  print_status("fresh incarnation commits",
               ef::scenarios::commit(*runner.coordinator, second_boot.value(), reclaimed.value()));

  const auto snapshot = runner.coordinator->snapshot(runner.experiment);
  if (!snapshot.ok()) {
    return 1;
  }
  print_lines(ef::inspect::render_trials(snapshot.value(), ef::Limits::defaults()));
  return 0;
}
