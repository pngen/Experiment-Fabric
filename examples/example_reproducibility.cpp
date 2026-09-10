// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "example_support.hpp"

/// Reproducibility inspection.
///
/// A reproducibility record is explicit evidence, not a claim: it names the
/// definition digest, the branch definition digest, the policy digest, the
/// input-set identity, the environment fingerprint, the seed, the producer and
/// the artifacts. Missing material is named, and a stored seed alone never
/// makes an experiment reproducible.

int main() {
  using namespace examples;
  print_banner("reproducibility inspection");

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

  const auto record = runner.coordinator->reproducibility(runner.experiment, runner.branches[0]);
  if (!record.ok()) {
    print_status("reproducibility", record.status());
    return 1;
  }
  print_lines(ef::inspect::render_reproducibility(record.value(), ef::Limits::defaults()));

  const auto snapshot = runner.coordinator->snapshot(runner.experiment);
  if (!snapshot.ok()) {
    return 1;
  }
  std::cout << "the digest of an identical definition is stable across builds; the runtime\n"
            << "classifies the record from the material actually present.\n";
  return 0;
}
