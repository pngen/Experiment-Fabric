// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

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

EF_TEST(authority, stale_coordinator_epoch_is_refused) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());

  ef::PublishObservationRequest request;
  request.epoch = ef::CoordinatorEpoch::from_value(coordinator.epoch().value() + 999);
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
  request.observation.epoch = request.epoch;
  request.observation.validity = ef::ObservationValidity::VALID;
  request.observation.value.kind = ef::MetricKind::REAL;
  request.observation.value.real = 10.0;

  const ef::Status status = coordinator.publish_observation(request);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::STALE_EPOCH);
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK(snapshot.value().observations.empty());
}

EF_TEST(authority, stale_worker_boot_is_refused) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());

  ef_fixture::TestWorker stale = fixture.value().worker;
  stale.boot = ef::WorkerBootId::from_value(0xDEADBEEF);
  const ef::Status status =
      ef_fixture::publish_value(coordinator, stale, assignment.value(), "latency_ms", 10.0);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::STALE_WORKER);
}

EF_TEST(authority, stale_experiment_generation_is_refused) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());

  ef::HypothesisRevisionSpec revision;
  revision.statement = "A revision that advances the experiment generation.";
  EF_REQUIRE(coordinator.revise_hypothesis(fixture.value().experiment, revision).ok());

  const ef::Status status =
      ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 10.0);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::STALE_GENERATION);
}

EF_TEST(authority, stale_generation_is_refused_when_only_the_generation_changes) {
  // Recover a coordinator so that its epoch advances, then present an envelope
  // built from the previous epoch.
  const std::filesystem::path directory = ef_test::scratch_directory("stale-generation");
  const std::filesystem::path state = directory / "state.efstate";
  ef::ExperimentId experiment;
  ef::TrialAssignment stale_assignment;
  ef::ProducerId stale_producer;
  ef::CoordinatorEpoch stale_epoch;
  ef::WorkerBootId stale_boot;
  {
    auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3, state);
    EF_REQUIRE(fixture.ok());
    ef::Coordinator& coordinator = *fixture.value().coordinator;
    experiment = fixture.value().experiment;
    EF_REQUIRE(coordinator.save().ok());
    EF_REQUIRE(coordinator.create_trial(experiment, fixture.value().baseline, "p").ok());
    auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
    EF_REQUIRE(assignment.ok());
    stale_assignment = assignment.value();
    stale_producer = fixture.value().worker.producer;
    stale_epoch = fixture.value().worker.epoch;
    stale_boot = fixture.value().worker.boot;
  }
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  config.state_path = state;
  auto recovered = ef::Coordinator::create(config);
  EF_REQUIRE(recovered.ok());
  EF_CHECK(recovered.value()->epoch() != stale_epoch);

  ef::PublishObservationRequest request;
  request.epoch = stale_epoch;
  request.worker = ef::WorkerId::from_value(1);
  request.boot = stale_boot;
  request.observation = ef::Observation{};
  request.observation.id = stale_assignment.metrics[0].observation_id;
  request.observation.experiment = stale_assignment.envelope.experiment;
  request.observation.generation = stale_assignment.envelope.generation;
  request.observation.branch = stale_assignment.envelope.branch;
  request.observation.branch_generation = stale_assignment.envelope.branch_generation;
  request.observation.trial = stale_assignment.envelope.trial;
  request.observation.attempt = stale_assignment.envelope.attempt;
  request.observation.metric = stale_assignment.metrics[0].metric;
  request.observation.producer = stale_producer;
  request.observation.worker = ef::WorkerId::from_value(1);
  request.observation.boot = stale_boot;
  request.observation.epoch = stale_epoch;
  request.observation.validity = ef::ObservationValidity::VALID;
  request.observation.value.kind = ef::MetricKind::REAL;
  request.observation.value.real = 42.0;
  const ef::Status status = recovered.value()->publish_observation(request);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::STALE_EPOCH);

  // The recovered trial was conservatively marked for revalidation and resolved
  // to a re-executable state, not silently continued.
  auto worker = ef_fixture::register_worker(*recovered.value(), 1, 0x1234);
  EF_REQUIRE(worker.ok());
  auto fresh = ef_fixture::claim_one(*recovered.value(), worker.value());
  EF_REQUIRE(fresh.ok());
  EF_CHECK(fresh.value().envelope.trial == stale_assignment.envelope.trial);
  EF_CHECK(fresh.value().envelope.attempt != stale_assignment.envelope.attempt);
  EF_CHECK_EQ(fresh.value().attempt_number.value(), 2ull);
  EF_REQUIRE(ef_fixture::publish_value(*recovered.value(), worker.value(), fresh.value(), "latency_ms", 120.0).ok());
  EF_REQUIRE(ef_fixture::commit(*recovered.value(), worker.value(), fresh.value()).ok());
}

EF_TEST(authority, stale_attempt_is_refused_after_a_retry_takes_over) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto first = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(first.ok());

  ef::FailTrialRequest failure;
  failure.epoch = fixture.value().worker.epoch;
  failure.worker = fixture.value().worker.worker;
  failure.boot = fixture.value().worker.boot;
  failure.envelope = first.value().envelope;
  failure.kind = ef::FailureKind::EXECUTION_FAILURE;
  failure.detail = "synthetic failure";
  failure.retryable = true;
  EF_REQUIRE(coordinator.fail_trial(failure).ok());

  auto second = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(second.ok());
  EF_CHECK(second.value().envelope.trial == first.value().envelope.trial);
  EF_CHECK(second.value().envelope.attempt != first.value().envelope.attempt);
  EF_CHECK_EQ(second.value().attempt_number.value(), 2ull);

  const ef::Status late =
      ef_fixture::publish_value(coordinator, fixture.value().worker, first.value(), "latency_ms", 1.0);
  EF_CHECK(!late.ok());
  EF_CHECK_EQ(late.code(), ef::ErrorCode::STALE_ATTEMPT);

  EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, second.value(), "latency_ms", 120.0).ok());
  EF_REQUIRE(ef_fixture::commit(coordinator, fixture.value().worker, second.value()).ok());
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK_EQ(snapshot.value().trials[0].trial.committed_attempts, 1u);
  std::uint32_t committed = 0;
  for (const ef::AttemptRecord& attempt : snapshot.value().trials[0].attempts) {
    if (attempt.committed) {
      ++committed;
    }
  }
  EF_CHECK_EQ(committed, 1u);
}

EF_TEST(authority, exactly_one_logical_completion_per_trial_is_enforced) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 120.0).ok());
  EF_REQUIRE(ef_fixture::commit(coordinator, fixture.value().worker, assignment.value()).ok());
  // Duplicate delivery of the same authoritative completion is harmless.
  EF_CHECK(ef_fixture::commit(coordinator, fixture.value().worker, assignment.value()).ok());
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK_EQ(snapshot.value().trials[0].trial.committed_attempts, 1u);
  EF_CHECK_EQ(snapshot.value().decisions.size(), std::size_t{0});
}

EF_TEST(authority, publication_after_branch_retirement_is_refused) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().candidate, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  EF_REQUIRE(coordinator.retire_branch(fixture.value().experiment, fixture.value().candidate, "retired").ok());
  const ef::Status status =
      ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 5.0);
  EF_CHECK(!status.ok());
  EF_CHECK(status.code() == ef::ErrorCode::STALE_BRANCH || status.code() == ef::ErrorCode::STALE_ATTEMPT);
}

EF_TEST(authority, publication_after_experiment_closure_is_refused) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "b").ok());
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().candidate, "c").ok());
  auto baseline = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(baseline.ok());
  auto candidate = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(candidate.ok());

  EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, baseline.value(), "latency_ms", 120.0).ok());
  EF_REQUIRE(ef_fixture::commit(coordinator, fixture.value().worker, baseline.value()).ok());
  EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, candidate.value(), "latency_ms", 95.0).ok());
  EF_REQUIRE(ef_fixture::commit(coordinator, fixture.value().worker, candidate.value()).ok());

  const auto finalized = coordinator.finalize_experiment(fixture.value().experiment, fixture.value().candidate, "closed");
  EF_REQUIRE(finalized.ok());
  EF_CHECK(finalized.value().outcome == ef::DecisionOutcome::ACCEPT);

  // A fresh attempt cannot be issued after closure, and no late evidence can
  // reach the experiment.
  EF_CHECK(!coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "late").ok());
  const auto claims = ef_fixture::claim_all(coordinator, fixture.value().worker, 4);
  EF_REQUIRE(claims.ok());
  EF_CHECK(claims.value().assignments.empty());
}

EF_TEST(authority, unregistered_worker_cannot_claim_or_publish) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  ef_fixture::TestWorker unknown;
  unknown.worker = ef::WorkerId::from_value(77);
  unknown.boot = ef::WorkerBootId::from_value(99);
  unknown.epoch = coordinator.epoch();
  unknown.producer = ef::ProducerId::from_value(1234);
  const auto claims = ef_fixture::claim_all(coordinator, unknown, 1);
  EF_REQUIRE(claims.ok());
  EF_CHECK(claims.value().status.code() == ef::ErrorCode::STALE_WORKER);
}

EF_TEST(authority, producer_identity_must_match_the_registered_incarnation) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  ef_fixture::TestWorker impostor = fixture.value().worker;
  impostor.producer = ef::ProducerId::from_value(fixture.value().worker.producer.value() + 500);
  const ef::Status status =
      ef_fixture::publish_value(coordinator, impostor, assignment.value(), "latency_ms", 1.0);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::UNAUTHORIZED);
}

EF_TEST(authority, replacing_a_worker_incarnation_revokes_the_previous_boot) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());

  auto replacement = ef_fixture::register_worker(coordinator, 1, 0xFEEDFACE);
  EF_REQUIRE(replacement.ok());
  EF_CHECK(replacement.value().boot != fixture.value().worker.boot);

  const ef::Status status =
      ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 3.0);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::STALE_WORKER);

  // The unfinished assignment was transitioned conservatively and can be
  // re-executed by the fresh incarnation.
  auto fresh = ef_fixture::claim_one(coordinator, replacement.value());
  EF_REQUIRE(fresh.ok());
  EF_CHECK(fresh.value().envelope.trial == assignment.value().envelope.trial);
  EF_REQUIRE(ef_fixture::publish_value(coordinator, replacement.value(), fresh.value(), "latency_ms", 118.0).ok());
  EF_REQUIRE(ef_fixture::commit(coordinator, replacement.value(), fresh.value()).ok());

  // Replaying the dead incarnation's registration is refused.
  ef::RegisterWorkerRequest replay;
  replay.worker = fixture.value().worker.worker;
  replay.boot = fixture.value().worker.boot;
  replay.endpoint = "in-process-1";
  replay.max_concurrent_assignments = 4;
  const auto reply = coordinator.register_worker(replay);
  EF_REQUIRE(reply.ok());
  EF_CHECK(!reply.value().accepted);
  EF_CHECK_EQ(reply.value().status.code(), ef::ErrorCode::STALE_WORKER);
}
