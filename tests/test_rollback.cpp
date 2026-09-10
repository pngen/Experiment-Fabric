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

EF_TEST(rollback, rollback_without_an_authoritative_point_is_refused) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const ef::Status status =
      coordinator.rollback(fixture.value().experiment, ef::ExperimentGeneration::from_value(1), "no point exists");
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::NOT_FOUND);
  const ef::Status null_target =
      coordinator.rollback(fixture.value().experiment, ef::ExperimentGeneration{}, "null");
  EF_CHECK(!null_target.ok());
  EF_CHECK_EQ(null_target.code(), ef::ErrorCode::INVALID_ARGUMENT);
}

EF_TEST(rollback, rollback_restores_authority_and_requires_fresh_execution) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(ef_fixture::run_planned_trials(coordinator, fixture.value().experiment, fixture.value().worker,
                                            two_branches())
                 .ok());
  const auto decision = coordinator.finalize_experiment(fixture.value().experiment, fixture.value().candidate,
                                                        "accepted at generation 1");
  EF_REQUIRE(decision.ok());
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::ACCEPT);
  const ef::ExperimentGeneration accepted_generation = decision.value().generation;
  EF_CHECK_EQ(accepted_generation.value(), 1ull);

  // A hypothesis revision advances the generation and fences the evidence.
  ef::HypothesisRevisionSpec revision;
  revision.statement = "A later revision that changes the experiment semantics.";
  EF_REQUIRE(coordinator.revise_hypothesis(fixture.value().experiment, revision).ok());
  const auto revised = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(revised.ok());
  EF_CHECK_EQ(revised.value().definition.generation.value(), 2ull);
  EF_CHECK_EQ(revised.value().decisions.size(), std::size_t{1});
  for (const auto& trial : revised.value().trials) {
    EF_CHECK(trial.trial.state == ef::TrialState::SUPERSEDED);
  }

  const ef::Status rolled =
      coordinator.rollback(fixture.value().experiment, accepted_generation, "restore the accepted generation");
  EF_REQUIRE(rolled.ok());
  const auto restored = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(restored.ok());
  EF_CHECK_EQ(restored.value().definition.generation.value(), accepted_generation.value());
  EF_CHECK(restored.value().state == ef::ExperimentState::OPEN);
  // History survives: the superseded trials, the accepted decision, and both
  // rollback points are still present.
  EF_CHECK_EQ(restored.value().decisions.size(), std::size_t{1});
  EF_CHECK(restored.value().trials.size() >= 2);
  EF_CHECK_EQ(restored.value().rollback_points.size(), std::size_t{2});
  EF_CHECK(restored.value().rollback_points.back().restored_from.valid());
  EF_CHECK_EQ(restored.value().rollback_points.back().restored_from.value(), 2ull);
  for (const auto& trial : restored.value().trials) {
    EF_CHECK(trial.trial.state == ef::TrialState::SUPERSEDED);
  }
  // Rollback restores authority, not evidence: the restored generation holds no
  // current authoritative evidence until it is executed again.
  const auto re_evaluated = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(re_evaluated.ok());
  EF_CHECK(re_evaluated.value().outcome == ef::DecisionOutcome::INSUFFICIENT_EVIDENCE);
  // Rolling back to the generation already in force is refused.
  const ef::Status repeated =
      coordinator.rollback(fixture.value().experiment, accepted_generation, "already there");
  EF_CHECK(!repeated.ok());
  EF_CHECK_EQ(repeated.code(), ef::ErrorCode::INVALID_TRANSITION);
}

EF_TEST(rollback, rollback_fences_live_attempts_derived_from_superseded_state) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(ef_fixture::run_planned_trials(coordinator, fixture.value().experiment, fixture.value().worker,
                                            two_branches())
                 .ok());
  EF_REQUIRE(coordinator.finalize_experiment(fixture.value().experiment, fixture.value().candidate, "accepted").ok());

  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "open").ok());
  auto open_assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(open_assignment.ok());

  EF_REQUIRE(coordinator
                 .rollback(fixture.value().experiment, ef::ExperimentGeneration::from_value(1), "fence the live work")
                 .ok());
  const ef::Status status =
      ef_fixture::publish_value(coordinator, fixture.value().worker, open_assignment.value(), "latency_ms", 1.0);
  EF_CHECK(!status.ok());
  EF_CHECK_EQ(status.code(), ef::ErrorCode::STALE_ATTEMPT);
  const ef::Status commit_status =
      ef_fixture::commit(coordinator, fixture.value().worker, open_assignment.value());
  EF_CHECK(!commit_status.ok());

  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  for (const auto& trial : snapshot.value().trials) {
    if (trial.trial.id == open_assignment.value().envelope.trial) {
      EF_CHECK(trial.trial.state == ef::TrialState::SUPERSEDED);
    }
    for (const ef::AttemptRecord& attempt : trial.attempts) {
      EF_CHECK(ef::is_attempt_terminal(attempt.state));
    }
  }
}

EF_TEST(rollback, a_branch_created_after_the_target_is_retired_by_the_rollback) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(ef_fixture::run_planned_trials(coordinator, fixture.value().experiment, fixture.value().worker,
                                            two_branches())
                 .ok());
  EF_REQUIRE(coordinator.finalize_experiment(fixture.value().experiment, fixture.value().candidate, "accepted").ok());

  ef::BranchSpec fork;
  fork.name = "later-branch";
  fork.role = ef::BranchRole::CANDIDATE;
  fork.planned_trials = 1;
  const auto forked = coordinator.fork_branch(fixture.value().experiment, fixture.value().baseline, fork);
  EF_REQUIRE(forked.ok());

  EF_REQUIRE(coordinator
                 .rollback(fixture.value().experiment, ef::ExperimentGeneration::from_value(1), "drop the later branch")
                 .ok());
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  for (const ef::BranchDefinition& branch : snapshot.value().definition.branches) {
    if (branch.id == forked.value()) {
      EF_CHECK(branch.state == ef::BranchState::RETIRED);
    } else {
      EF_CHECK(branch.state == ef::BranchState::ACTIVE);
      EF_CHECK_EQ(branch.generation.value(), 1ull);
    }
  }
  // The retired branch cannot accept new trials without an explicit reactivation.
  EF_CHECK(!coordinator.create_trial(fixture.value().experiment, forked.value(), "x").ok());
}
