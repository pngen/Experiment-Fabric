// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cmath>
#include <limits>
#include <string>

#include "fixtures.hpp"
#include "test_support.hpp"

namespace {
namespace ef = experiment_fabric;
using ef_fixture::BranchFixture;
}  // namespace

EF_TEST(metrics, valid_observation_advances_the_trial_to_observing) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto trial = coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p");
  EF_REQUIRE(trial.ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  EF_CHECK(assignment.value().metrics.size() == 1);
  const ef::Status published = ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(),
                                                         "latency_ms", 118.0);
  EF_REQUIRE(published.ok());
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK(snapshot.value().trials[0].trial.state == ef::TrialState::OBSERVING);
  EF_CHECK_EQ(snapshot.value().observations.size(), std::size_t{1});
  EF_CHECK(snapshot.value().observations[0].validity == ef::ObservationValidity::VALID);
}

EF_TEST(metrics, non_finite_values_are_rejected_unless_the_metric_accepts_them) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  const ef::Status nan_status = ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(),
                                                          "latency_ms", std::nan(""));
  EF_CHECK(!nan_status.ok());
  EF_CHECK_EQ(nan_status.code(), ef::ErrorCode::INVALID_OBSERVATION);
  const ef::Status inf_status = ef_fixture::publish_value(
      coordinator, fixture.value().worker, assignment.value(), "latency_ms",
      std::numeric_limits<double>::infinity());
  EF_CHECK(!inf_status.ok());
  EF_CHECK_EQ(inf_status.code(), ef::ErrorCode::INVALID_OBSERVATION);
}

EF_TEST(metrics, validity_bounds_are_enforced) {
  ef::ExperimentSpec spec = ef_fixture::latency_spec(
      {BranchFixture{"baseline", ef::BranchRole::BASELINE, 100.0, 1, false},
       BranchFixture{"candidate", ef::BranchRole::CANDIDATE, 100.0, 1, false}},
      1, false, 1);
  spec.metrics[0].lower_bound = 1.0;
  spec.metrics[0].upper_bound = 1000.0;
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  auto coordinator = ef::Coordinator::create(config);
  EF_REQUIRE(coordinator.ok());
  const auto experiment = coordinator.value()->create_experiment(spec);
  EF_REQUIRE(experiment.ok());
  const auto snapshot = coordinator.value()->snapshot(experiment.value());
  EF_REQUIRE(snapshot.ok());
  const ef::BranchId branch = snapshot.value().definition.branches[0].id;
  EF_REQUIRE(coordinator.value()->create_trial(experiment.value(), branch, "p").ok());
  auto worker = ef_fixture::register_worker(*coordinator.value(), 1);
  EF_REQUIRE(worker.ok());
  auto assignment = ef_fixture::claim_one(*coordinator.value(), worker.value());
  EF_REQUIRE(assignment.ok());
  EF_CHECK(ef_fixture::publish_value(*coordinator.value(), worker.value(), assignment.value(), "latency_ms", 2000.0)
               .code() == ef::ErrorCode::INVALID_OBSERVATION);
  EF_CHECK(ef_fixture::publish_value(*coordinator.value(), worker.value(), assignment.value(), "latency_ms", 0.5)
               .code() == ef::ErrorCode::INVALID_OBSERVATION);
  EF_CHECK(ef_fixture::publish_value(*coordinator.value(), worker.value(), assignment.value(), "latency_ms", 500.0).ok());
}

EF_TEST(metrics, duplicate_identical_publication_is_harmless_and_conflicting_is_rejected) {
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
  request.observation.value.kind = ef::MetricKind::REAL;
  request.observation.value.real = 100.0;

  EF_CHECK(coordinator.publish_observation(request).ok());
  EF_CHECK(coordinator.publish_observation(request).ok());
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK_EQ(snapshot.value().observations.size(), std::size_t{1});

  request.observation.value.real = 101.0;
  const ef::Status conflicting = coordinator.publish_observation(request);
  EF_CHECK(!conflicting.ok());
  EF_CHECK_EQ(conflicting.code(), ef::ErrorCode::ALREADY_EXISTS);
}

EF_TEST(metrics, a_second_valid_observation_for_one_metric_on_one_trial_is_refused) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 118.0).ok());

  ef::PublishObservationRequest request;
  request.epoch = fixture.value().worker.epoch;
  request.worker = fixture.value().worker.worker;
  request.boot = fixture.value().worker.boot;
  request.observation.id = ef::ObservationId::from_value(999999);
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
  request.observation.value.kind = ef::MetricKind::REAL;
  request.observation.value.real = 90.0;
  const ef::Status status = coordinator.publish_observation(request);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::ALREADY_EXISTS);
}

EF_TEST(metrics, invalid_observation_never_becomes_a_numeric_value) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms",
                                       118.0, ef::ObservationValidity::INVALID)
                 .ok());
  EF_REQUIRE(ef_fixture::commit(coordinator, fixture.value().worker, assignment.value()).ok());

  const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(decision.ok());
  bool found = false;
  for (const ef::BranchEvidence& evidence : decision.value().evidence) {
    if (evidence.branch != fixture.value().baseline) {
      continue;
    }
    for (const ef::MetricAggregate& aggregate : evidence.aggregates) {
      found = true;
      EF_CHECK(!aggregate.present);
      EF_CHECK_EQ(aggregate.valid_count, 0u);
      EF_CHECK_EQ(aggregate.invalid_count, 1u);
      EF_CHECK_EQ(aggregate.missing_count, 1u);
    }
  }
  EF_CHECK(found);
}

EF_TEST(metrics, missing_metric_is_not_zero) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  // A MISSING observation is published explicitly rather than omitted.
  EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms",
                                       0.0, ef::ObservationValidity::MISSING)
                 .ok());
  EF_REQUIRE(ef_fixture::commit(coordinator, fixture.value().worker, assignment.value()).ok());
  const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(decision.ok());
  for (const ef::BranchEvidence& evidence : decision.value().evidence) {
    if (evidence.branch == fixture.value().baseline) {
      EF_CHECK(!evidence.aggregates[0].present);
      EF_CHECK_EQ(evidence.aggregates[0].value, 0.0);
      EF_CHECK_EQ(evidence.aggregates[0].valid_count, 0u);
      EF_CHECK_EQ(evidence.aggregates[0].missing_count, 1u);
    }
  }
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::INSUFFICIENT_EVIDENCE);
}

EF_TEST(metrics, unsupported_observation_is_distinct_from_invalid_and_missing) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms",
                                       0.0, ef::ObservationValidity::UNSUPPORTED)
                 .ok());
  EF_REQUIRE(ef_fixture::commit(coordinator, fixture.value().worker, assignment.value()).ok());
  const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(decision.ok());
  for (const ef::BranchEvidence& evidence : decision.value().evidence) {
    if (evidence.branch == fixture.value().baseline) {
      EF_CHECK_EQ(evidence.aggregates[0].unsupported_count, 1u);
      EF_CHECK_EQ(evidence.aggregates[0].invalid_count, 0u);
      EF_CHECK_EQ(evidence.aggregates[0].missing_count, 1u);
    }
  }
}

EF_TEST(metrics, commit_requires_at_least_one_observation) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  const ef::Status status = ef_fixture::commit(coordinator, fixture.value().worker, assignment.value());
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::INSUFFICIENT_EVIDENCE);
}

EF_TEST(metrics, observation_for_an_undefined_metric_is_refused) {
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
  request.observation.metric = ef::MetricId::from_value(424242);
  request.observation.producer = fixture.value().worker.producer;
  request.observation.worker = fixture.value().worker.worker;
  request.observation.boot = fixture.value().worker.boot;
  request.observation.epoch = fixture.value().worker.epoch;
  request.observation.validity = ef::ObservationValidity::VALID;
  request.observation.value.kind = ef::MetricKind::REAL;
  request.observation.value.real = 1.0;
  const ef::Status status = coordinator.publish_observation(request);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::INVALID_OBSERVATION);
}
