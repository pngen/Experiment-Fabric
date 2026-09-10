// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_TESTS_FIXTURES_HPP
#define EXPERIMENT_FABRIC_TESTS_FIXTURES_HPP

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "experiment_fabric/coordinator.hpp"

/// \file
/// Shared fixtures for the Experiment Fabric test suite.

namespace ef_fixture {

namespace ef = experiment_fabric;

/// One registered worker incarnation driven directly against a coordinator.
struct TestWorker {
  ef::WorkerId worker = ef::WorkerId::from_value(1);
  ef::WorkerBootId boot = ef::WorkerBootId::from_value(0xABCDEF01ull);
  ef::ProducerId producer;
  ef::CoordinatorEpoch epoch;
};

inline ef::Result<TestWorker> register_worker(ef::Coordinator& coordinator, std::uint64_t identity,
                                              std::uint64_t boot = 0xABCDEF01ull) {
  ef::RegisterWorkerRequest request;
  request.worker = ef::WorkerId::from_value(identity);
  request.boot = ef::WorkerBootId::from_value(boot);
  request.endpoint = "in-process-" + std::to_string(identity);
  request.max_concurrent_assignments = 32;
  request.fingerprint = "in-process-test-worker";
  auto reply = coordinator.register_worker(request);
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().status.failed()) {
    return reply.value().status;
  }
  TestWorker worker;
  worker.worker = request.worker;
  worker.boot = reply.value().boot;
  worker.producer = reply.value().producer;
  worker.epoch = reply.value().epoch;
  return worker;
}

inline ef::Result<ef::TrialAssignment> claim_one(ef::Coordinator& coordinator, const TestWorker& worker) {
  ef::ClaimTrialRequest request;
  request.epoch = worker.epoch;
  request.worker = worker.worker;
  request.boot = worker.boot;
  request.max_claims = 1;
  auto reply = coordinator.claim_trials(request);
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().status.failed()) {
    return reply.value().status;
  }
  if (reply.value().assignments.empty()) {
    return ef::make_error(ef::ErrorCode::NOT_FOUND, ef::ErrorStage::LIFECYCLE, "no assignment available");
  }
  return reply.value().assignments.front();
}

inline ef::Status publish_value(ef::Coordinator& coordinator, const TestWorker& worker,
                                const ef::TrialAssignment& assignment, const std::string& metric_name,
                                double value, ef::ObservationValidity validity = ef::ObservationValidity::VALID) {
  const ef::AssignedMetric* reservation = nullptr;
  for (const ef::AssignedMetric& candidate : assignment.metrics) {
    if (candidate.name == metric_name) {
      reservation = &candidate;
    }
  }
  if (reservation == nullptr) {
    return ef::make_error(ef::ErrorCode::NOT_FOUND, ef::ErrorStage::OBSERVATION, "metric was not assigned");
  }
  ef::PublishObservationRequest request;
  request.epoch = worker.epoch;
  request.worker = worker.worker;
  request.boot = worker.boot;
  request.observation.id = reservation->observation_id;
  request.observation.experiment = assignment.envelope.experiment;
  request.observation.generation = assignment.envelope.generation;
  request.observation.branch = assignment.envelope.branch;
  request.observation.branch_generation = assignment.envelope.branch_generation;
  request.observation.trial = assignment.envelope.trial;
  request.observation.attempt = assignment.envelope.attempt;
  request.observation.metric = reservation->metric;
  request.observation.producer = worker.producer;
  request.observation.worker = worker.worker;
  request.observation.boot = worker.boot;
  request.observation.epoch = worker.epoch;
  request.observation.validity = validity;
  request.observation.value.kind = reservation->kind;
  request.observation.value.real = value;
  request.observation.value.integer = static_cast<std::int64_t>(value);
  request.observation.value.boolean = value != 0.0;
  return coordinator.publish_observation(request);
}

inline ef::Status commit(ef::Coordinator& coordinator, const TestWorker& worker,
                         const ef::TrialAssignment& assignment) {
  ef::CommitTrialRequest request;
  request.epoch = worker.epoch;
  request.worker = worker.worker;
  request.boot = worker.boot;
  request.envelope = assignment.envelope;
  request.result_payload = "fixture";
  return coordinator.commit_trial(request);
}

/// Declarative description of one branch used by the fixture builders.
struct BranchFixture {
  std::string name = "branch";
  ef::BranchRole role = ef::BranchRole::CANDIDATE;
  double value = 100.0;
  std::uint32_t planned_trials = 1;
  bool invalid = false;
};

/// Builds a single-metric latency-style experiment specification.
inline ef::ExperimentSpec latency_spec(std::vector<BranchFixture> branches,
                                       std::uint32_t min_completed_trials = 1,
                                       bool reject_invalid = false,
                                       std::uint32_t max_attempts = 3) {
  ef::ExperimentSpec spec;
  spec.name = "fixture-latency";
  spec.hypothesis_statement = "The candidate reduces latency.";
  spec.provenance = "SYNTHETIC fixture";
  ef::MetricSpec metric;
  metric.name = "latency_ms";
  metric.unit = "ms";
  metric.kind = ef::MetricKind::REAL;
  metric.direction = ef::MetricDirection::MINIMIZE;
  metric.aggregation = ef::AggregationRule::MEAN;
  metric.required = true;
  spec.metrics.push_back(metric);
  for (const BranchFixture& branch : branches) {
    ef::BranchSpec branch_spec;
    branch_spec.name = branch.name;
    branch_spec.role = branch.role;
    branch_spec.planned_trials = branch.planned_trials;
    branch_spec.environment = std::string("fixture-environment");
    branch_spec.environment_fingerprint = "fixture-x64";
    branch_spec.parameters.push_back(ef::make_real_parameter("latency_ms", branch.value));
    if (branch.invalid) {
      branch_spec.parameters.push_back(ef::make_boolean_parameter("latency_ms_invalid", true));
    }
    spec.branches.push_back(std::move(branch_spec));
  }
  ef::ComparisonRuleSpec rule;
  rule.metric = "latency_ms";
  rule.op = ef::ComparisonOp::MIN_IMPROVEMENT;
  rule.threshold = 1.0;
  rule.priority = 0;
  spec.rules.push_back(rule);
  spec.min_completed_trials_per_branch = min_completed_trials;
  spec.min_valid_observations_per_required_metric = 1;
  spec.reject_on_any_invalid_observation = reject_invalid;
  spec.max_attempts_per_trial = max_attempts;
  spec.experiment_seed = 1234;
  return spec;
}

/// A fully wired in-process experiment used by most tests.
struct Fixture {
  std::unique_ptr<ef::Coordinator> coordinator;
  ef::ExperimentId experiment;
  ef::BranchId baseline;
  ef::BranchId candidate;
  ef::MetricId metric;
  TestWorker worker;

  [[nodiscard]] const ef::BranchDefinition* branch(ef::BranchId id) const {
    const auto snapshot = coordinator->snapshot(experiment);
    if (!snapshot.ok()) {
      return nullptr;
    }
    for (const ef::BranchDefinition& definition : snapshot.value().definition.branches) {
      if (definition.id == id) {
        return &definition;
      }
    }
    return nullptr;
  }
};

inline ef::Result<Fixture> make_fixture(ef::CoordinatorObserver* observer = nullptr,
                                        std::vector<BranchFixture> branches =
                                            {BranchFixture{"baseline", ef::BranchRole::BASELINE, 120.0, 2, false},
                                             BranchFixture{"candidate", ef::BranchRole::CANDIDATE, 95.0, 2, false}},
                                        std::uint32_t min_completed_trials = 2, bool reject_invalid = false,
                                        std::uint32_t max_attempts = 3,
                                        std::optional<std::filesystem::path> state_path = std::nullopt) {
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  config.observer = observer;
  config.persist_on_mutation = true;
  if (state_path.has_value()) {
    config.state_path = *state_path;
  }
  const auto planned = branches;
  const std::uint32_t required = min_completed_trials;
  const bool reject = reject_invalid;
  const std::uint32_t attempts = max_attempts;
  auto coordinator = ef::Coordinator::create(std::move(config));
  if (!coordinator.ok()) {
    return coordinator.status();
  }
  Fixture fixture;
  fixture.coordinator = std::move(coordinator.value());
  auto created = fixture.coordinator->create_experiment(latency_spec(planned, required, reject, attempts));
  if (!created.ok()) {
    return created.status();
  }
  fixture.experiment = created.value();
  const auto snapshot = fixture.coordinator->snapshot(fixture.experiment);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  fixture.metric = snapshot.value().definition.metrics.front().id;
  for (const ef::BranchDefinition& branch : snapshot.value().definition.branches) {
    if (branch.role == ef::BranchRole::BASELINE) {
      fixture.baseline = branch.id;
    } else {
      fixture.candidate = branch.id;
    }
  }
  auto worker = register_worker(*fixture.coordinator, 1);
  if (!worker.ok()) {
    return worker.status();
  }
  fixture.worker = worker.value();
  return fixture;
}

/// Completes one authoritative trial on a branch with the given metric value.
inline ef::Status complete_one_trial(ef::Coordinator& coordinator, const TestWorker& worker,
                                     double value, ef::ObservationValidity validity = ef::ObservationValidity::VALID) {
  auto assignment = claim_one(coordinator, worker);
  if (!assignment.ok()) {
    return assignment.status();
  }
  const ef::Status published = publish_value(coordinator, worker, assignment.value(), "latency_ms", value, validity);
  if (!published.ok()) {
    return published;
  }
  return commit(coordinator, worker, assignment.value());
}

/// Runs every planned trial of every branch with the configured branch values.
inline ef::Status run_planned_trials(ef::Coordinator& coordinator, ef::ExperimentId experiment,
                                     const TestWorker& worker, const std::vector<BranchFixture>& branches) {
  const auto planned = coordinator.snapshot(experiment);
  if (!planned.ok()) {
    return planned.status();
  }
  for (const ef::BranchDefinition& branch : planned.value().definition.branches) {
    if (!ef::is_branch_eligible(branch.state)) {
      continue;
    }
    for (std::uint32_t index = 0; index < branch.planned_trials; ++index) {
      const auto created = coordinator.create_trial(experiment, branch.id, "fixture");
      if (!created.ok()) {
        return created.status();
      }
    }
  }
  for (const BranchFixture& branch : branches) {
    const std::uint32_t trials = branch.planned_trials;
    for (std::uint32_t index = 0; index < trials; ++index) {
      const ef::Status status =
          complete_one_trial(coordinator, worker, branch.value,
                             branch.invalid ? ef::ObservationValidity::INVALID : ef::ObservationValidity::VALID);
      if (!status.ok()) {
        return status;
      }
    }
  }
  return ef::Status::success();
}

inline ef::Result<ef::ClaimTrialReply> claim_all(ef::Coordinator& coordinator, const TestWorker& worker,
                                                 std::uint32_t max_claims) {
  ef::ClaimTrialRequest request;
  request.epoch = worker.epoch;
  request.worker = worker.worker;
  request.boot = worker.boot;
  request.max_claims = max_claims;
  return coordinator.claim_trials(request);
}

}  // namespace ef_fixture

#endif  // EXPERIMENT_FABRIC_TESTS_FIXTURES_HPP
