// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "example_support.hpp"

/// Coordinator restart and stale-epoch rejection.
///
/// Durable experiment and lineage state survives an independent coordinator
/// start, but live process authority does not: the coordinator epoch changes,
/// workers must re-register, and traffic preserved from the previous epoch is
/// rejected.
///
/// This example is SYNTHETIC with respect to *process death*: it creates a
/// second coordinator in the same process to model the restart. The real
/// operating-system restart proof lives in the multi-process test suite.

int main() {
  using namespace examples;
  print_banner("coordinator restart and stale-epoch rejection");

  std::error_code error;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path(error) / "experiment-fabric-restart-example";
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  const std::filesystem::path state = directory / "state.efstate";

  ef::ExperimentId experiment;
  ef::CoordinatorEpoch first_epoch;
  ef::TrialAssignment stale_assignment;
  ef::scenarios::Session stale_worker;
  std::uint64_t completed_before = 0;
  {
    auto session = start("authority", state);
    if (!session.ok()) {
      print_status("start", session.status());
      return 1;
    }
    Session& runner = session.value();
    experiment = runner.experiment;
    auto worker = ef::scenarios::join(*runner.coordinator, 1, 0xABCD);
    if (!worker.ok()) {
      return 1;
    }
    stale_worker = worker.value();
    first_epoch = worker.value().epoch;
    print_status("plan", plan(runner, "authority"));
    auto assignment = ef::scenarios::claim(*runner.coordinator, worker.value());
    if (!assignment.ok()) {
      return 1;
    }
    stale_assignment = assignment.value();
    print_status("complete one trial", ef::scenarios::complete_one(*runner.coordinator, worker.value(),
                                                                   "latency_ms", 120.0));
    const auto snapshot = runner.coordinator->snapshot(experiment);
    if (snapshot.ok()) {
      for (const auto& trial : snapshot.value().trials) {
        if (trial.trial.state == ef::TrialState::COMPLETED) {
          ++completed_before;
        }
      }
    }
  }

  auto restarted = start("authority", state);
  if (!restarted.ok()) {
    print_status("restart", restarted.status());
    return 1;
  }
  Session& runner = restarted.value();
  const ef::CoordinatorEpoch second_epoch = runner.coordinator->epoch();
  std::cout << "epoch before restart: " << first_epoch.to_string() << "\n";
  std::cout << "epoch after restart:  " << second_epoch.to_string() << "\n";
  std::cout << "epoch changed: " << (first_epoch != second_epoch) << "\n\n";

  const auto restored = runner.coordinator->snapshot(experiment);
  if (!restored.ok()) {
    return 1;
  }
  std::uint64_t completed_after = 0;
  for (const auto& trial : restored.value().trials) {
    if (trial.trial.state == ef::TrialState::COMPLETED) {
      ++completed_after;
    }
  }
  std::cout << "completed trials preserved: " << completed_before << " -> " << completed_after << "\n\n";

  ef::PublishObservationRequest replay;
  replay.epoch = stale_worker.epoch;
  replay.worker = stale_worker.worker;
  replay.boot = stale_worker.boot;
  replay.observation.id = stale_assignment.metrics.front().observation_id;
  replay.observation.experiment = stale_assignment.envelope.experiment;
  replay.observation.generation = stale_assignment.envelope.generation;
  replay.observation.branch = stale_assignment.envelope.branch;
  replay.observation.branch_generation = stale_assignment.envelope.branch_generation;
  replay.observation.trial = stale_assignment.envelope.trial;
  replay.observation.attempt = stale_assignment.envelope.attempt;
  replay.observation.metric = stale_assignment.metrics.front().metric;
  replay.observation.producer = stale_worker.producer;
  replay.observation.worker = stale_worker.worker;
  replay.observation.boot = stale_worker.boot;
  replay.observation.epoch = stale_worker.epoch;
  replay.observation.validity = ef::ObservationValidity::VALID;
  replay.observation.value.kind = ef::MetricKind::REAL;
  replay.observation.value.real = 42.0;
  print_status("replay previous-epoch evidence", runner.coordinator->publish_observation(replay));

  // Fresh work under the new epoch reaches an authoritative decision.
  auto worker = ef::scenarios::join(*runner.coordinator, 2);
  if (!worker.ok()) {
    return 1;
  }
  print_status("recover", ef::scenarios::complete_one(*runner.coordinator, worker.value(), "latency_ms", 118.0));
  print_status("plan more", plan(runner, "post-restart"));
  print_status("candidate", run_branch(runner, worker.value(), 1, "latency_ms", 95.0, runner.planned(1)));
  explain(*runner.coordinator, experiment, runner.branches[1]);
  std::filesystem::remove_all(directory, error);
  return 0;
}
