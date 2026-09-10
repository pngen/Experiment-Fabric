// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "experiment_fabric/canonical.hpp"
#include "fixtures.hpp"
#include "test_support.hpp"

namespace {
namespace ef = experiment_fabric;
using ef_fixture::BranchFixture;
}  // namespace

EF_TEST(experiment, definition_is_created_with_distinct_typed_identities) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  const auto snapshot = fixture.value().coordinator->snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  const ef::ExperimentDefinition& definition = snapshot.value().definition;
  EF_CHECK(definition.id.valid());
  EF_CHECK(definition.generation.valid());
  EF_CHECK(definition.hypothesis.valid());
  EF_CHECK(definition.hypothesis_revision.valid());
  EF_CHECK(definition.policy.id.valid());
  EF_CHECK_EQ(definition.branches.size(), std::size_t{2});
  EF_CHECK(definition.branches[0].id != definition.branches[1].id);
  EF_CHECK(definition.metrics[0].id.valid());
  EF_CHECK_EQ(definition.policy.rules.size(), std::size_t{1});
  EF_CHECK(definition.policy.rules[0].metric == definition.metrics[0].id);
}

EF_TEST(experiment, generator_must_designate_a_baseline_when_policy_requires_it) {
  ef::ExperimentSpec spec = ef_fixture::latency_spec(
      {BranchFixture{"only", ef::BranchRole::CANDIDATE, 100.0, 1, false}}, 1, false, 1);
  spec.require_baseline = true;
  auto coordinator = ef::Coordinator::create([] {
    ef::Coordinator::Config config;
    config.limits = ef::Limits::defaults();
    return config;
  }());
  EF_REQUIRE(coordinator.ok());
  const auto created = coordinator.value()->create_experiment(spec);
  EF_CHECK(!created.ok());
  EF_CHECK_EQ(created.status().code(), ef::ErrorCode::INVALID_ARGUMENT);
}

EF_TEST(experiment, impossible_specifications_are_rejected) {
  auto coordinator = ef::Coordinator::create([] {
    ef::Coordinator::Config config;
    config.limits = ef::Limits::defaults();
    return config;
  }());
  EF_REQUIRE(coordinator.ok());

  ef::ExperimentSpec duplicate_metrics =
      ef_fixture::latency_spec({BranchFixture{"b", ef::BranchRole::BASELINE, 1.0, 1, false}}, 1, false, 1);
  duplicate_metrics.metrics.push_back(duplicate_metrics.metrics.front());
  EF_CHECK(!coordinator.value()->create_experiment(duplicate_metrics).ok());

  ef::ExperimentSpec unknown_rule =
      ef_fixture::latency_spec({BranchFixture{"b", ef::BranchRole::BASELINE, 1.0, 1, false}}, 1, false, 1);
  unknown_rule.rules[0].metric = "does-not-exist";
  EF_CHECK(!coordinator.value()->create_experiment(unknown_rule).ok());

  ef::ExperimentSpec no_metrics =
      ef_fixture::latency_spec({BranchFixture{"b", ef::BranchRole::BASELINE, 1.0, 1, false}}, 1, false, 1);
  no_metrics.metrics.clear();
  EF_CHECK(!coordinator.value()->create_experiment(no_metrics).ok());

  ef::ExperimentSpec bad_bounds =
      ef_fixture::latency_spec({BranchFixture{"b", ef::BranchRole::BASELINE, 1.0, 1, false}}, 1, false, 1);
  bad_bounds.metrics[0].lower_bound = 10.0;
  bad_bounds.metrics[0].upper_bound = 1.0;
  EF_CHECK(!coordinator.value()->create_experiment(bad_bounds).ok());

  ef::ExperimentSpec duplicate_parameters =
      ef_fixture::latency_spec({BranchFixture{"b", ef::BranchRole::BASELINE, 1.0, 1, false}}, 1, false, 1);
  duplicate_parameters.parameters.push_back(ef::make_text_parameter("k", "1"));
  duplicate_parameters.parameters.push_back(ef::make_text_parameter("k", "2"));
  EF_CHECK(!coordinator.value()->create_experiment(duplicate_parameters).ok());
}

EF_TEST(experiment, hypothesis_revision_advances_and_supersedes_history) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;

  ef::HypothesisRevisionSpec revision;
  revision.statement = "The candidate reduces latency and must not regress throughput.";
  revision.expected_direction = ef::MetricDirection::MINIMIZE;
  const auto revised = coordinator.revise_hypothesis(fixture.value().experiment, revision);
  EF_REQUIRE(revised.ok());
  EF_CHECK_EQ(revised.value().value(), 2ull);

  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK_EQ(snapshot.value().hypothesis.revision.value(), 2ull);
  EF_CHECK_EQ(snapshot.value().definition.generation.value(), 2ull);
  EF_REQUIRE(snapshot.value().hypothesis_history.size() == 1);
  EF_CHECK(snapshot.value().hypothesis_history[0].superseded);
  EF_CHECK_EQ(snapshot.value().hypothesis_history[0].superseded_by_revision.value(), 2ull);
  EF_CHECK_EQ(snapshot.value().hypothesis_history[0].revision.value(), 1ull);
}

EF_TEST(experiment, hypothesis_revision_fences_open_trials) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto trial = coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "payload");
  EF_REQUIRE(trial.ok());

  ef::HypothesisRevisionSpec revision;
  revision.statement = "A revised hypothesis invalidates every trial derived from revision 1.";
  const auto revised = coordinator.revise_hypothesis(fixture.value().experiment, revision);
  EF_REQUIRE(revised.ok());

  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_REQUIRE(snapshot.value().trials.size() == 1);
  EF_CHECK(snapshot.value().trials[0].trial.state == ef::TrialState::SUPERSEDED);
  EF_CHECK(snapshot.value().trials[0].trial.failure_kind == ef::FailureKind::EXPERIMENT_INVALIDATION);
}

EF_TEST(experiment, forking_a_branch_preserves_lineage_without_sharing_authority) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;

  ef::BranchSpec fork;
  fork.name = "candidate-batch-64";
  fork.role = ef::BranchRole::CANDIDATE;
  fork.parameters.push_back(ef::make_real_parameter("latency_ms", 88.0));
  fork.parameters.push_back(ef::make_integer_parameter("batch", 64));
  fork.planned_trials = 2;
  const auto forked = coordinator.fork_branch(fixture.value().experiment, fixture.value().candidate, fork);
  EF_REQUIRE(forked.ok());

  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  const ef::BranchDefinition* parent = nullptr;
  const ef::BranchDefinition* child = nullptr;
  for (const ef::BranchDefinition& branch : snapshot.value().definition.branches) {
    if (branch.id == fixture.value().candidate) {
      parent = &branch;
    }
    if (branch.id == forked.value()) {
      child = &branch;
    }
  }
  EF_REQUIRE(parent != nullptr);
  EF_REQUIRE(child != nullptr);
  EF_CHECK(child->parent == parent->id);
  EF_CHECK(child->forked);
  EF_CHECK_EQ(child->generation.value(), 1ull);
  // The parent keeps its own generation: a fork is a branch-local change.
  EF_CHECK_EQ(parent->generation.value(), 1ull);
  // Parameters are merged: inherited values are preserved, overrides win.
  bool found_override = false;
  bool found_inherited = false;
  for (const ef::ParameterAssignment& assignment : child->parameters) {
    if (assignment.key == "latency_ms") {
      found_override = assignment.value == "88";
    }
    if (assignment.key == "batch") {
      found_inherited = assignment.value == "64";
    }
  }
  EF_CHECK(found_override);
  EF_CHECK(found_inherited);
}

EF_TEST(experiment, forking_rejects_unknown_parent_duplicate_name_and_second_baseline) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;

  ef::BranchSpec fork;
  fork.name = "second-baseline";
  fork.role = ef::BranchRole::BASELINE;
  fork.planned_trials = 1;
  const auto second_baseline =
      coordinator.fork_branch(fixture.value().experiment, fixture.value().baseline, fork);
  EF_CHECK(!second_baseline.ok());
  EF_CHECK_EQ(second_baseline.status().code(), ef::ErrorCode::INVALID_ARGUMENT);

  ef::BranchSpec duplicate = fork;
  duplicate.role = ef::BranchRole::CANDIDATE;
  duplicate.name = "baseline";
  EF_CHECK(!coordinator.fork_branch(fixture.value().experiment, fixture.value().baseline, duplicate).ok());

  EF_CHECK(!coordinator
                .fork_branch(fixture.value().experiment, ef::BranchId::from_value(9999), duplicate)
                .ok());
}

EF_TEST(experiment, retiring_a_branch_advances_only_that_branch_generation) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto retired = coordinator.retire_branch(fixture.value().experiment, fixture.value().candidate, "no longer needed");
  EF_REQUIRE(retired.ok());
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  for (const ef::BranchDefinition& branch : snapshot.value().definition.branches) {
    if (branch.id == fixture.value().candidate) {
      EF_CHECK(branch.state == ef::BranchState::RETIRED);
      EF_CHECK_EQ(branch.generation.value(), 2ull);
    } else {
      EF_CHECK(branch.state == ef::BranchState::ACTIVE);
      EF_CHECK_EQ(branch.generation.value(), 1ull);
    }
  }
  const auto trial = coordinator.create_trial(fixture.value().experiment, fixture.value().candidate, "x");
  EF_CHECK(!trial.ok());
  EF_CHECK_EQ(trial.status().code(), ef::ErrorCode::STALE_BRANCH);
}

EF_TEST(experiment, seeds_are_derived_deterministically_per_trial) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto first = coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "a");
  const auto second = coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "b");
  EF_REQUIRE(first.ok());
  EF_REQUIRE(second.ok());
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK(snapshot.value().trials[0].trial.seed.has_value());
  EF_CHECK(snapshot.value().trials[1].trial.seed.has_value());
  EF_CHECK(snapshot.value().trials[0].trial.seed != snapshot.value().trials[1].trial.seed);
  EF_CHECK_EQ(*snapshot.value().trials[0].trial.seed,
              ef::derive_trial_seed(*snapshot.value().definition.experiment_seed,
                                    snapshot.value().trials[0].trial.id.value()));
}

EF_TEST(experiment, planned_trial_count_is_enforced) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  for (std::uint32_t index = 0; index < 2; ++index) {
    EF_CHECK(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "x").ok());
  }
  const auto overflow = coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "x");
  EF_CHECK(!overflow.ok());
  EF_CHECK_EQ(overflow.status().code(), ef::ErrorCode::RESOURCE_EXHAUSTED);
}

EF_TEST(experiment, definition_digest_is_stable_and_sensitive) {
  auto first = ef_fixture::make_fixture();
  auto second = ef_fixture::make_fixture();
  EF_REQUIRE(first.ok());
  EF_REQUIRE(second.ok());
  const auto first_snapshot = first.value().coordinator->snapshot(first.value().experiment);
  const auto second_snapshot = second.value().coordinator->snapshot(second.value().experiment);
  EF_REQUIRE(first_snapshot.ok());
  EF_REQUIRE(second_snapshot.ok());
  EF_CHECK(ef::canonical::definition_digest(first_snapshot.value().definition) ==
           ef::canonical::definition_digest(second_snapshot.value().definition));

  auto third = ef_fixture::make_fixture(nullptr, {BranchFixture{"baseline", ef::BranchRole::BASELINE, 120.0, 2, false},
                                                  BranchFixture{"candidate", ef::BranchRole::CANDIDATE, 94.0, 2, false}});
  EF_REQUIRE(third.ok());
  const auto third_snapshot = third.value().coordinator->snapshot(third.value().experiment);
  EF_REQUIRE(third_snapshot.ok());
  EF_CHECK(ef::canonical::definition_digest(first_snapshot.value().definition) !=
           ef::canonical::definition_digest(third_snapshot.value().definition));
}
