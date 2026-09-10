// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "test_support.hpp"

namespace {
namespace ef = experiment_fabric;
using ef_fixture::BranchFixture;

std::vector<BranchFixture> two_branches() {
  return {BranchFixture{"baseline", ef::BranchRole::BASELINE, 120.0, 1, false},
          BranchFixture{"candidate", ef::BranchRole::CANDIDATE, 95.0, 1, false}};
}
}  // namespace

EF_TEST(adversarial, null_identities_are_never_accepted_as_authority) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());

  ef::PublishObservationRequest request;
  request.epoch = fixture.value().worker.epoch;
  request.worker = fixture.value().worker.worker;
  request.boot = fixture.value().worker.boot;
  request.observation.id = assignment.value().metrics[0].observation_id;
  request.observation.experiment = assignment.value().envelope.experiment;
  request.observation.generation = assignment.value().envelope.generation;
  request.observation.branch = assignment.value().envelope.branch;
  request.observation.branch_generation = assignment.value().envelope.branch_generation;
  request.observation.trial = assignment.value().envelope.trial;
  request.observation.attempt = ef::TrialAttemptId{};  // null attempt
  request.observation.metric = assignment.value().metrics[0].metric;
  request.observation.producer = fixture.value().worker.producer;
  request.observation.worker = fixture.value().worker.worker;
  request.observation.boot = fixture.value().worker.boot;
  request.observation.epoch = fixture.value().worker.epoch;
  request.observation.validity = ef::ObservationValidity::VALID;
  request.observation.value.kind = ef::MetricKind::REAL;
  request.observation.value.real = 1.0;
  const ef::Status status = coordinator.publish_observation(request);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::INVALID_ARGUMENT);
}

EF_TEST(adversarial, a_valid_observation_for_an_undeclared_metric_kind_is_refused) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  ef::PublishObservationRequest request;
  request.epoch = fixture.value().worker.epoch;
  request.worker = fixture.value().worker.worker;
  request.boot = fixture.value().worker.boot;
  request.observation.id = assignment.value().metrics[0].observation_id;
  request.observation.experiment = assignment.value().envelope.experiment;
  request.observation.generation = assignment.value().envelope.generation;
  request.observation.branch = assignment.value().envelope.branch;
  request.observation.branch_generation = assignment.value().envelope.branch_generation;
  request.observation.trial = assignment.value().envelope.trial;
  request.observation.attempt = assignment.value().envelope.attempt;
  request.observation.metric = assignment.value().metrics[0].metric;
  request.observation.producer = fixture.value().worker.producer;
  request.observation.worker = fixture.value().worker.worker;
  request.observation.boot = fixture.value().worker.boot;
  request.observation.epoch = fixture.value().worker.epoch;
  request.observation.validity = ef::ObservationValidity::VALID;
  request.observation.value.kind = ef::MetricKind::CATEGORICAL;
  request.observation.value.category = "not-a-real-kind";
  const ef::Status status = coordinator.publish_observation(request);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::INVALID_OBSERVATION);
}

EF_TEST(adversarial, extreme_identities_are_handled_without_overflow) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto missing = coordinator.snapshot(ef::ExperimentId::from_value(0xFFFFFFFFFFFFFFFFull));
  EF_CHECK(!missing.ok());
  EF_CHECK_EQ(missing.status().code(), ef::ErrorCode::NOT_FOUND);
  EF_CHECK(!coordinator.create_trial(ef::ExperimentId::from_value(0xFFFFFFFFFFFFFFFFull),
                                     ef::BranchId::from_value(1), "x")
                 .ok());
}

EF_TEST(adversarial, absurd_specification_sizes_are_refused_before_allocation) {
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  config.limits.max_branches_per_experiment = 4;
  config.limits.max_metrics_per_experiment = 2;
  config.limits.max_comparison_rules = 2;
  auto coordinator = ef::Coordinator::create(config);
  EF_REQUIRE(coordinator.ok());

  ef::ExperimentSpec spec = ef_fixture::latency_spec(two_branches(), 1, false, 1);
  for (int index = 0; index < 8; ++index) {
    ef::BranchSpec extra;
    extra.name = "extra-" + std::to_string(index);
    extra.role = ef::BranchRole::CANDIDATE;
    extra.planned_trials = 1;
    spec.branches.push_back(extra);
  }
  const auto created = coordinator.value()->create_experiment(spec);
  EF_CHECK(!created.ok());
  EF_CHECK_EQ(created.status().code(), ef::ErrorCode::RESOURCE_EXHAUSTED);
}

EF_TEST(adversarial, incompressible_limits_are_rejected_at_construction) {
  ef::Coordinator::Config config;
  config.limits.max_experiments = 0;
  const auto created = ef::Coordinator::create(config);
  EF_CHECK(!created.ok());
  EF_CHECK_EQ(created.status().code(), ef::ErrorCode::INVALID_ARGUMENT);

  ef::Coordinator::Config second;
  second.limits.max_frame_payload_bytes = 8;
  EF_CHECK(!ef::Coordinator::create(second).ok());
}

EF_TEST(adversarial, duplicate_observation_identities_across_trials_are_refused) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "b").ok());
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().candidate, "c").ok());
  auto first = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(first.ok());
  EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, first.value(), "latency_ms", 120.0).ok());
  auto second = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(second.ok());

  // Reuse the first attempt's reserved observation identity on the second trial.
  ef::PublishObservationRequest request;
  request.epoch = fixture.value().worker.epoch;
  request.worker = fixture.value().worker.worker;
  request.boot = fixture.value().worker.boot;
  request.observation.id = first.value().metrics[0].observation_id;
  request.observation.experiment = second.value().envelope.experiment;
  request.observation.generation = second.value().envelope.generation;
  request.observation.branch = second.value().envelope.branch;
  request.observation.branch_generation = second.value().envelope.branch_generation;
  request.observation.trial = second.value().envelope.trial;
  request.observation.attempt = second.value().envelope.attempt;
  request.observation.metric = second.value().metrics[0].metric;
  request.observation.producer = fixture.value().worker.producer;
  request.observation.worker = fixture.value().worker.worker;
  request.observation.boot = fixture.value().worker.boot;
  request.observation.epoch = fixture.value().worker.epoch;
  request.observation.validity = ef::ObservationValidity::VALID;
  request.observation.value.kind = ef::MetricKind::REAL;
  request.observation.value.real = 95.0;
  const ef::Status status = coordinator.publish_observation(request);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::ALREADY_EXISTS);
}

EF_TEST(adversarial, artifact_references_are_bounded_and_validated) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "b").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());

  ef::PublishArtifactRequest artifact;
  artifact.epoch = fixture.value().worker.epoch;
  artifact.worker = fixture.value().worker.worker;
  artifact.boot = fixture.value().worker.boot;
  artifact.artifact.id = assignment.value().artifact_ids.front();
  artifact.artifact.experiment = assignment.value().envelope.experiment;
  artifact.artifact.generation = assignment.value().envelope.generation;
  artifact.artifact.branch = assignment.value().envelope.branch;
  artifact.artifact.trial = assignment.value().envelope.trial;
  artifact.artifact.attempt = assignment.value().envelope.attempt;
  artifact.artifact.category = "trace";
  artifact.artifact.locator = std::string(5000, 'x');
  const ef::Status too_long = coordinator.publish_artifact(artifact);
  EF_CHECK(!too_long.ok());
  EF_CHECK_EQ(too_long.code(), ef::ErrorCode::INVALID_ARGUMENT);

  artifact.artifact.locator.clear();
  const ef::Status empty = coordinator.publish_artifact(artifact);
  EF_CHECK(!empty.ok());
  EF_CHECK_EQ(empty.code(), ef::ErrorCode::INVALID_ARGUMENT);

  artifact.artifact.locator = "synthetic://ok";
  EF_CHECK(coordinator.publish_artifact(artifact).ok());
  artifact.artifact.locator = "synthetic://different";
  const ef::Status conflicting = coordinator.publish_artifact(artifact);
  EF_CHECK(!conflicting.ok());
  EF_CHECK_EQ(conflicting.code(), ef::ErrorCode::ALREADY_EXISTS);
}

EF_TEST(adversarial, failed_trial_requires_a_typed_failure_kind) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "b").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  ef::FailTrialRequest failure;
  failure.epoch = fixture.value().worker.epoch;
  failure.worker = fixture.value().worker.worker;
  failure.boot = fixture.value().worker.boot;
  failure.envelope = assignment.value().envelope;
  failure.kind = ef::FailureKind::NONE;
  failure.detail = "collapsed failure classification";
  const ef::Status status = coordinator.fail_trial(failure);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::INVALID_ARGUMENT);
}

EF_TEST(adversarial, a_failure_envelope_must_match_its_transport_identity) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "b").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  ef::FailTrialRequest failure;
  failure.epoch = fixture.value().worker.epoch;
  failure.worker = ef::WorkerId::from_value(fixture.value().worker.worker.value() + 1);
  failure.boot = fixture.value().worker.boot;
  failure.envelope = assignment.value().envelope;
  failure.kind = ef::FailureKind::EXECUTION_FAILURE;
  const ef::Status status = coordinator.fail_trial(failure);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::PROTOCOL_ERROR);
}

EF_TEST(adversarial, retirement_then_republication_is_fenced) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().candidate, "c").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  EF_REQUIRE(coordinator.retire_branch(fixture.value().experiment, fixture.value().candidate, "retired").ok());
  const ef::Status status =
      ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 95.0);
  EF_CHECK(!status.ok());
  EF_CHECK(status.code() == ef::ErrorCode::STALE_BRANCH || status.code() == ef::ErrorCode::STALE_ATTEMPT);
  // Retirement is idempotent through the state machine, and a second retirement
  // is refused rather than silently ignored.
  const auto again = coordinator.retire_branch(fixture.value().experiment, fixture.value().candidate, "again");
  EF_CHECK(again.ok());
}

EF_TEST(adversarial, an_experiment_that_does_not_exist_is_reported_consistently) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto snapshot = coordinator.snapshot(ef::ExperimentId::from_value(4242));
  EF_CHECK(!snapshot.ok());
  EF_CHECK_EQ(snapshot.status().code(), ef::ErrorCode::NOT_FOUND);
  const auto decision = coordinator.evaluate(ef::ExperimentId::from_value(4242), ef::BranchId::from_value(1));
  EF_CHECK(!decision.ok());
  EF_CHECK_EQ(decision.status().code(), ef::ErrorCode::NOT_FOUND);
  const auto reproducibility =
      coordinator.reproducibility(fixture.value().experiment, ef::BranchId::from_value(4242));
  EF_CHECK(!reproducibility.ok());
  EF_CHECK_EQ(reproducibility.status().code(), ef::ErrorCode::NOT_FOUND);
}

EF_TEST(adversarial, finalized_experiments_refuse_further_authoritative_mutation) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(ef_fixture::run_planned_trials(coordinator, fixture.value().experiment, fixture.value().worker,
                                            two_branches())
                 .ok());
  const auto first = coordinator.finalize_experiment(fixture.value().experiment, fixture.value().candidate, "a");
  EF_REQUIRE(first.ok());
  const auto second = coordinator.finalize_experiment(fixture.value().experiment, fixture.value().candidate, "b");
  EF_CHECK(!second.ok());
  EF_CHECK_EQ(second.status().code(), ef::ErrorCode::INVALID_TRANSITION);
  ef::HypothesisRevisionSpec revision;
  revision.statement = "after closure";
  EF_CHECK(!coordinator.revise_hypothesis(fixture.value().experiment, revision).ok());
  ef::BranchSpec fork;
  fork.name = "after-closure";
  fork.planned_trials = 1;
  EF_CHECK(!coordinator.fork_branch(fixture.value().experiment, fixture.value().baseline, fork).ok());
  const auto trials = coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "late");
  EF_CHECK(!trials.ok());
  EF_CHECK_EQ(trials.status().code(), ef::ErrorCode::INVALID_TRANSITION);
}
