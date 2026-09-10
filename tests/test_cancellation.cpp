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

EF_TEST(cancellation, cancelled_trial_can_never_later_commit_success) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto trial = coordinator.create_trial(fixture.value().experiment, fixture.value().candidate, "p");
  EF_REQUIRE(trial.ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  EF_REQUIRE(coordinator.cancel_trial(trial.value(), "candidate withdrawn").ok());

  const ef::Status published =
      ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 10.0);
  EF_CHECK(!published.ok());
  EF_CHECK_EQ(published.code(), ef::ErrorCode::CANCELLED);
  const ef::Status committed = ef_fixture::commit(coordinator, fixture.value().worker, assignment.value());
  EF_CHECK(!committed.ok());
  EF_CHECK_EQ(committed.code(), ef::ErrorCode::CANCELLED);

  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK(snapshot.value().trials[0].trial.state == ef::TrialState::CANCELLED);
  EF_CHECK_EQ(snapshot.value().trials[0].trial.committed_attempts, 0u);
  // Cancellation is idempotent and reaches a terminal state.
  EF_CHECK(coordinator.cancel_trial(trial.value(), "again").ok());
}

EF_TEST(cancellation, cancelled_attempt_is_recorded_as_rejected_evidence) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto trial = coordinator.create_trial(fixture.value().experiment, fixture.value().candidate, "p");
  EF_REQUIRE(trial.ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  EF_REQUIRE(coordinator.cancel_trial(trial.value(), "withdrawn").ok());
  (void)ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 10.0);
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_REQUIRE(!snapshot.value().rejected_evidence.empty());
  EF_CHECK_EQ(snapshot.value().rejected_evidence[0].reason, ef::ErrorCode::CANCELLED);
  EF_CHECK_EQ(snapshot.value().rejected_evidence[0].kind, std::string("observation"));
}

EF_TEST(cancellation, experiment_cancellation_closes_every_open_scope) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "b").ok());
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().candidate, "c").ok());
  auto baseline = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(baseline.ok());
  EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, baseline.value(), "latency_ms", 120.0).ok());
  auto candidate = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(candidate.ok());

  EF_REQUIRE(coordinator.cancel_experiment(fixture.value().experiment, "review abandoned").ok());
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK(snapshot.value().state == ef::ExperimentState::CANCELLED);
  for (const auto& trial : snapshot.value().trials) {
    EF_CHECK(trial.trial.state == ef::TrialState::CANCELLED);
    for (const ef::AttemptRecord& attempt : trial.attempts) {
      EF_CHECK(ef::is_attempt_terminal(attempt.state));
      EF_CHECK(attempt.state == ef::AttemptState::CANCELLED);
    }
  }
  // Decision-making on a cancelled experiment yields CANCELLED, never ACCEPT.
  const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(decision.ok());
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::CANCELLED);
  // No further work can be claimed and no new trials can be created.
  const auto claims = ef_fixture::claim_all(coordinator, fixture.value().worker, 4);
  EF_REQUIRE(claims.ok());
  EF_CHECK(claims.value().assignments.empty());
  EF_CHECK(!coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "late").ok());
}

EF_TEST(cancellation, cancellation_after_completion_is_not_a_state_change) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto trial = coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "b");
  EF_REQUIRE(trial.ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 120.0).ok());
  EF_REQUIRE(ef_fixture::commit(coordinator, fixture.value().worker, assignment.value()).ok());
  const ef::Status status = coordinator.cancel_trial(trial.value(), "too late");
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::INVALID_TRANSITION);
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK(snapshot.value().trials[0].trial.state == ef::TrialState::COMPLETED);
}

EF_TEST(cancellation, cancelling_an_unknown_trial_is_reported) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  const ef::Status status = fixture.value().coordinator->cancel_trial(ef::TrialId::from_value(99999), "unknown");
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::NOT_FOUND);
}
