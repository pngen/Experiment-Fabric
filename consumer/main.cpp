// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <iostream>
#include <string>
#include <vector>

#include "experiment_fabric/coordinator.hpp"
#include "experiment_fabric/inspect.hpp"
#include "experiment_fabric/version.hpp"

/// \file
/// Independent downstream consumer.
///
/// The consumer builds an experiment through the installed public API, drives
/// one trial to an authoritative completion, evaluates the comparison and reads
/// the reproducibility record. It is executed as part of package validation.

namespace {

namespace ef = experiment_fabric;

ef::ExperimentSpec build_spec() {
  ef::ExperimentSpec spec;
  spec.name = "consumer-latency";
  spec.hypothesis_statement = "The candidate reduces latency for the consuming project.";
  spec.provenance = "SYNTHETIC consumer validation";
  ef::MetricSpec metric;
  metric.name = "latency_ms";
  metric.unit = "ms";
  metric.direction = ef::MetricDirection::MINIMIZE;
  metric.kind = ef::MetricKind::REAL;
  metric.required = true;
  spec.metrics.push_back(metric);
  ef::BranchSpec baseline;
  baseline.name = "baseline";
  baseline.role = ef::BranchRole::BASELINE;
  baseline.planned_trials = 1;
  baseline.environment_fingerprint = "consumer-x64";
  baseline.parameters = {ef::make_real_parameter("latency_ms", 120.0)};
  ef::BranchSpec candidate = baseline;
  candidate.name = "candidate";
  candidate.role = ef::BranchRole::CANDIDATE;
  candidate.parameters = {ef::make_real_parameter("latency_ms", 95.0)};
  spec.branches = {baseline, candidate};
  ef::ComparisonRuleSpec rule;
  rule.metric = "latency_ms";
  rule.op = ef::ComparisonOp::MIN_IMPROVEMENT;
  rule.threshold = 1.0;
  spec.rules = {rule};
  spec.min_completed_trials_per_branch = 1;
  spec.experiment_seed = 2026;
  return spec;
}

/// Registers a worker, claims one trial, publishes a value and commits.
ef::Status complete_trial(ef::Coordinator& coordinator, double value) {
  ef::RegisterWorkerRequest registration;
  registration.worker = ef::WorkerId::from_value(1);
  registration.boot = ef::WorkerBootId::from_value(0xC0FFEE);
  registration.endpoint = "consumer";
  registration.fingerprint = "consumer";
  auto reply = coordinator.register_worker(registration);
  if (!reply.ok() || reply.value().status.failed()) {
    return reply.ok() ? reply.value().status : reply.status();
  }
  ef::ClaimTrialRequest claim;
  claim.epoch = reply.value().epoch;
  claim.worker = registration.worker;
  claim.boot = reply.value().boot;
  claim.max_claims = 1;
  auto claimed = coordinator.claim_trials(claim);
  if (!claimed.ok() || claimed.value().status.failed() || claimed.value().assignments.empty()) {
    return ef::make_error(ef::ErrorCode::NOT_FOUND, ef::ErrorStage::LIFECYCLE, "no assignment");
  }
  const ef::TrialAssignment assignment = claimed.value().assignments.front();
  ef::PublishObservationRequest publication;
  publication.epoch = reply.value().epoch;
  publication.worker = registration.worker;
  publication.boot = reply.value().boot;
  publication.observation.id = assignment.metrics.front().observation_id;
  publication.observation.experiment = assignment.envelope.experiment;
  publication.observation.generation = assignment.envelope.generation;
  publication.observation.branch = assignment.envelope.branch;
  publication.observation.branch_generation = assignment.envelope.branch_generation;
  publication.observation.trial = assignment.envelope.trial;
  publication.observation.attempt = assignment.envelope.attempt;
  publication.observation.metric = assignment.metrics.front().metric;
  publication.observation.producer = reply.value().producer;
  publication.observation.worker = registration.worker;
  publication.observation.boot = reply.value().boot;
  publication.observation.epoch = reply.value().epoch;
  publication.observation.validity = ef::ObservationValidity::VALID;
  publication.observation.value.kind = ef::MetricKind::REAL;
  publication.observation.value.real = value;
  const ef::Status published = coordinator.publish_observation(publication);
  if (!published.ok()) {
    return published;
  }
  ef::CommitTrialRequest commit;
  commit.epoch = reply.value().epoch;
  commit.worker = registration.worker;
  commit.boot = reply.value().boot;
  commit.envelope = assignment.envelope;
  return coordinator.commit_trial(commit);
}

}  // namespace

int main() {
  std::cout << "Experiment Fabric consumer against " << ef::version_string() << "\n";

  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  auto coordinator = ef::Coordinator::create(std::move(config));
  if (!coordinator.ok()) {
    std::cerr << "coordinator creation failed: " << coordinator.status().to_string() << "\n";
    return 1;
  }
  const auto experiment = coordinator.value()->create_experiment(build_spec());
  if (!experiment.ok()) {
    std::cerr << "experiment creation failed: " << experiment.status().to_string() << "\n";
    return 1;
  }
  const auto snapshot = coordinator.value()->snapshot(experiment.value());
  if (!snapshot.ok()) {
    return 1;
  }
  const ef::BranchId baseline = snapshot.value().definition.branches[0].id;
  const ef::BranchId candidate = snapshot.value().definition.branches[1].id;
  if (!coordinator.value()->create_trial(experiment.value(), baseline, "consumer").ok()) {
    return 1;
  }
  if (!coordinator.value()->create_trial(experiment.value(), candidate, "consumer").ok()) {
    return 1;
  }
  if (!complete_trial(*coordinator.value(), 118.0).ok()) {
    return 1;
  }
  if (!complete_trial(*coordinator.value(), 95.0).ok()) {
    return 1;
  }
  const auto decision = coordinator.value()->finalize_experiment(experiment.value(), candidate, "consumer");
  if (!decision.ok()) {
    std::cerr << "finalization failed: " << decision.status().to_string() << "\n";
    return 1;
  }
  for (const std::string& line : ef::inspect::render_decision(decision.value(), ef::Limits::defaults())) {
    std::cout << line << "\n";
  }
  const auto record = coordinator.value()->reproducibility(experiment.value(), candidate);
  if (record.ok()) {
    for (const std::string& line : ef::inspect::render_reproducibility(record.value(), ef::Limits::defaults())) {
      std::cout << line << "\n";
    }
  }
  return decision.value().outcome == ef::DecisionOutcome::ACCEPT ? 0 : 1;
}
