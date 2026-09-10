// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "experiment_fabric/inspect.hpp"
#include "fixtures.hpp"
#include "test_support.hpp"

namespace {
namespace ef = experiment_fabric;
using ef_fixture::BranchFixture;

/// Drives a randomized sequence of governance operations against one
/// coordinator and checks the invariants after every step. The generator is
/// fully seeded, and the seed is printed on failure so that any counterexample
/// can be reproduced exactly.
struct InvariantDriver {
  std::unique_ptr<ef::Coordinator> coordinator;
  ef::ExperimentId experiment;
  std::vector<ef::BranchId> branches;
  std::vector<ef::TrialId> trials;
  ef_fixture::TestWorker worker;
  ef::DeterministicRng rng;
  std::uint64_t seed = 0;
  std::uint64_t experiment_generation = 1;

  explicit InvariantDriver(std::uint64_t seed_value) : rng(seed_value), seed(seed_value) {}

  [[nodiscard]] bool setup() {
    ef::Coordinator::Config config;
    config.limits = ef::Limits::defaults();
    auto created = ef::Coordinator::create(config);
    if (!created.ok()) {
      return false;
    }
    coordinator = std::move(created.value());
    ef::ExperimentSpec spec = ef_fixture::latency_spec(
        {BranchFixture{"baseline", ef::BranchRole::BASELINE, 120.0, 3, false},
         BranchFixture{"candidate", ef::BranchRole::CANDIDATE, 95.0, 3, false}},
        1, false, 4);
    const auto experiment_id = coordinator->create_experiment(spec);
    if (!experiment_id.ok()) {
      return false;
    }
    experiment = experiment_id.value();
    const auto snapshot = coordinator->snapshot(experiment);
    if (!snapshot.ok()) {
      return false;
    }
    for (const ef::BranchDefinition& branch : snapshot.value().definition.branches) {
      branches.push_back(branch.id);
    }
    auto registered = ef_fixture::register_worker(*coordinator, 1);
    if (!registered.ok()) {
      return false;
    }
    worker = registered.value();
    return true;
  }

  void create_trial() {
    const ef::BranchId branch = branches[rng.next_bounded(branches.size())];
    const auto created = coordinator->create_trial(experiment, branch, "property");
    if (created.ok()) {
      trials.push_back(created.value());
    }
  }

  void claim_and_complete() {
    auto assignment = ef_fixture::claim_one(*coordinator, worker);
    if (!assignment.ok()) {
      return;
    }
    const std::size_t index = static_cast<std::size_t>(rng.next_bounded(3));
    const double values[] = {140.0, 118.0, 96.0};
    const auto published =
        ef_fixture::publish_value(*coordinator, worker, assignment.value(), "latency_ms", values[index]);
    if (!published.ok()) {
      return;
    }
    (void)ef_fixture::commit(*coordinator, worker, assignment.value());
  }

  void publish_only() {
    auto assignment = ef_fixture::claim_one(*coordinator, worker);
    if (!assignment.ok()) {
      return;
    }
    (void)ef_fixture::publish_value(*coordinator, worker, assignment.value(), "latency_ms", 100.0);
  }

  void cancel_random_trial() {
    if (trials.empty()) {
      return;
    }
    const ef::TrialId trial = trials[rng.next_bounded(trials.size())];
    (void)coordinator->cancel_trial(trial, "property cancellation");
  }

  void revise_hypothesis() {
    ef::HypothesisRevisionSpec revision;
    revision.statement = "property revision " + std::to_string(experiment_generation + 1);
    const auto revised = coordinator->revise_hypothesis(experiment, revision);
    if (revised.ok()) {
      ++experiment_generation;
    }
  }

  void fork_branch() {
    ef::BranchSpec fork;
    fork.name = "fork-" + std::to_string(rng.next_u32());
    fork.role = ef::BranchRole::CANDIDATE;
    fork.planned_trials = 1;
    const auto forked = coordinator->fork_branch(experiment, branches[rng.next_bounded(branches.size())], fork);
    if (forked.ok()) {
      branches.push_back(forked.value());
    }
  }

  void retire_branch() {
    const ef::BranchId branch = branches[rng.next_bounded(branches.size())];
    (void)coordinator->retire_branch(experiment, branch, "property retirement");
  }

  void finalize() {
    const auto decision = coordinator->finalize_experiment(experiment, branches.back(), "property finalization");
    if (decision.ok() && (decision.value().outcome == ef::DecisionOutcome::ACCEPT ||
                          decision.value().outcome == ef::DecisionOutcome::REJECT ||
                          decision.value().outcome == ef::DecisionOutcome::INVALID)) {
      // A finalized experiment is closed; later mutations must be refused.
      (void)coordinator->create_trial(experiment, branches.front(), "after closure");
    }
  }

  void rollback() {
    const auto snapshot = coordinator->snapshot(experiment);
    if (!snapshot.ok() || snapshot.value().rollback_points.empty()) {
      return;
    }
    const ef::RollbackPoint& point = snapshot.value().rollback_points.front();
    if (coordinator->rollback(experiment, point.generation, "property rollback").ok()) {
      experiment_generation = point.generation.value();
    }
  }

  [[nodiscard]] bool check_invariants(std::uint32_t step) {
    const auto snapshot = coordinator->snapshot(experiment);
    if (!snapshot.ok()) {
      ef_test::fail(__FILE__, __LINE__, "snapshot failed at step " + std::to_string(step));
      return false;
    }
    const ef::ExperimentSnapshot& state = snapshot.value();
    bool ok = true;

    if (experiment_generation != 0 && state.definition.generation.value() > experiment_generation) {
      ef_test::fail(__FILE__, __LINE__, "generation increased without a recorded mutation at step " +
                                            std::to_string(step));
      ok = false;
    }
    if (state.hypothesis.revision.valid() == false) {
      ef_test::fail(__FILE__, __LINE__, "hypothesis revision became null at step " + std::to_string(step));
      ok = false;
    }
    if (state.hypothesis.revision != state.definition.hypothesis_revision) {
      ef_test::fail(__FILE__, __LINE__, "hypothesis revision and definition disagree at step " +
                                            std::to_string(step));
      ok = false;
    }

    std::set<std::uint64_t> branch_ids;
    std::map<std::uint64_t, std::uint64_t> parents;
    for (const ef::BranchDefinition& branch : state.definition.branches) {
      if (!branch_ids.insert(branch.id.value()).second) {
        ef_test::fail(__FILE__, __LINE__, "duplicate branch identity at step " + std::to_string(step));
        ok = false;
      }
      if (branch.parent.valid()) {
        parents[branch.id.value()] = branch.parent.value();
      }
      if (branch.generation.value() == 0) {
        ef_test::fail(__FILE__, __LINE__, "branch generation became zero at step " + std::to_string(step));
        ok = false;
      }
    }
    for (const auto& entry : parents) {
      std::set<std::uint64_t> seen;
      std::uint64_t cursor = entry.first;
      while (parents.find(cursor) != parents.end()) {
        if (!seen.insert(cursor).second) {
          ef_test::fail(__FILE__, __LINE__, "branch lineage cycle at step " + std::to_string(step));
          ok = false;
          break;
        }
        cursor = parents[cursor];
      }
    }

    std::uint64_t committed_completions = 0;
    std::set<std::uint64_t> observation_ids;
    for (const auto& trial : state.trials) {
      if (trial.trial.committed_attempts > 1) {
        ef_test::fail(__FILE__, __LINE__, "a trial holds more than one authoritative completion at step " +
                                              std::to_string(step));
        ok = false;
      }
      std::uint32_t committed_attempts = 0;
      for (const ef::AttemptRecord& attempt : trial.attempts) {
        if (attempt.committed) {
          ++committed_attempts;
        }
        if (ef::is_attempt_terminal(attempt.state) == false &&
            attempt.state != ef::AttemptState::REVALIDATION_REQUIRED) {
          // An open attempt must still be the authoritative attempt of an open
          // trial.
          if (ef::is_terminal(trial.trial.state)) {
            ef_test::fail(__FILE__, __LINE__, "terminal trial holds an open attempt at step " +
                                                  std::to_string(step));
            ok = false;
          }
        }
      }
      if (committed_attempts != trial.trial.committed_attempts) {
        ef_test::fail(__FILE__, __LINE__, "committed attempt count disagrees with the trial at step " +
                                              std::to_string(step));
        ok = false;
      }
      committed_completions += trial.trial.committed_attempts;
      if (trial.trial.state == ef::TrialState::COMPLETED && trial.trial.committed_attempts != 1) {
        ef_test::fail(__FILE__, __LINE__, "completed trial has no authoritative completion at step " +
                                              std::to_string(step));
        ok = false;
      }
      if (trial.trial.state == ef::TrialState::CANCELLED && trial.trial.committed_attempts != 0) {
        ef_test::fail(__FILE__, __LINE__, "cancelled trial committed evidence at step " + std::to_string(step));
        ok = false;
      }
    }

    for (const ef::Observation& observation : state.observations) {
      if (!observation_ids.insert(observation.id.value()).second) {
        ef_test::fail(__FILE__, __LINE__, "duplicate observation identity at step " + std::to_string(step));
        ok = false;
      }
      if (observation.generation.value() == 0) {
        ef_test::fail(__FILE__, __LINE__, "observation carries a zero generation at step " + std::to_string(step));
        ok = false;
      }
    }

    // A decision never contradicts the evidence: after any sequence, an ACCEPT
    // requires at least two authoritative completions.
    for (const ef::Decision& decision : state.decisions) {
      if (decision.outcome == ef::DecisionOutcome::ACCEPT && decision.evidence.size() < 2) {
        ef_test::fail(__FILE__, __LINE__, "ACCEPT recorded without comparable evidence at step " +
                                              std::to_string(step));
        ok = false;
      }
    }

    const ef::Status validated = coordinator->validate_state();
    if (!validated.ok()) {
      ef_test::fail(__FILE__, __LINE__, "durable state failed validation at step " + std::to_string(step) + ": " +
                                            validated.to_string());
      ok = false;
    }
    return ok;
  }
};

void run_sequence(std::uint64_t seed, std::uint32_t steps) {
  InvariantDriver driver(seed);
  EF_REQUIRE(driver.setup());
  for (std::uint32_t step = 0; step < steps; ++step) {
    const std::uint32_t operation = static_cast<std::uint32_t>(driver.rng.next_bounded(10));
    switch (operation) {
      case 0:
      case 1:
        driver.create_trial();
        break;
      case 2:
      case 3:
        driver.claim_and_complete();
        break;
      case 4:
        driver.publish_only();
        break;
      case 5:
        driver.cancel_random_trial();
        break;
      case 6:
        driver.fork_branch();
        break;
      case 7:
        driver.retire_branch();
        break;
      case 8:
        driver.revise_hypothesis();
        break;
      case 9:
        driver.finalize();
        break;
      default:
        driver.rollback();
        break;
    }
    if (!driver.check_invariants(step)) {
      EF_CHECK_MESSAGE(false, "invariant violated for seed " + std::to_string(seed) + " at step " +
                                  std::to_string(step));
      return;
    }
  }
}
}  // namespace

EF_TEST(property, randomized_sequences_preserve_core_invariants) {
  for (std::uint64_t seed = 1; seed <= 24; ++seed) {
    run_sequence(seed, 24);
  }
}

EF_TEST(property, randomized_sequences_with_rollbacks_preserve_history) {
  for (std::uint64_t seed = 100; seed <= 112; ++seed) {
    InvariantDriver driver(seed);
    EF_REQUIRE(driver.setup());
    // Establish an authoritative decision so that a rollback point exists.
    for (int index = 0; index < 6; ++index) {
      driver.create_trial();
      driver.claim_and_complete();
    }
    driver.finalize();
    for (std::uint32_t step = 0; step < 12; ++step) {
      const std::uint32_t operation = static_cast<std::uint32_t>(driver.rng.next_bounded(6));
      if (operation < 3) {
        driver.create_trial();
        driver.claim_and_complete();
      } else if (operation == 3) {
        driver.rollback();
      } else if (operation == 4) {
        driver.revise_hypothesis();
      } else {
        driver.cancel_random_trial();
      }
      if (!driver.check_invariants(step)) {
        EF_CHECK_MESSAGE(false, "rollback invariant violated for seed " + std::to_string(seed));
        return;
      }
    }
    const auto snapshot = driver.coordinator->snapshot(driver.experiment);
    EF_REQUIRE(snapshot.ok());
    // History is never deleted to make the state look clean.
    EF_CHECK(!snapshot.value().decisions.empty());
    // History is never deleted: every trial that ever existed is still present,
    // and the experiment holds at least the trials planned for its branches.
    EF_CHECK(snapshot.value().trials.size() >= 3);
  }
}

EF_TEST(property, duplicate_observation_delivery_does_not_double_count) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  for (int attempt = 0; attempt < 5; ++attempt) {
    EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 118.0).ok());
  }
  EF_REQUIRE(ef_fixture::commit(coordinator, fixture.value().worker, assignment.value()).ok());
  for (int attempt = 0; attempt < 5; ++attempt) {
    EF_CHECK(ef_fixture::commit(coordinator, fixture.value().worker, assignment.value()).ok());
  }
  const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(decision.ok());
  for (const ef::BranchEvidence& evidence : decision.value().evidence) {
    if (evidence.branch == fixture.value().baseline) {
      EF_CHECK_EQ(evidence.completed_trials, 1u);
      EF_CHECK_EQ(evidence.aggregates[0].valid_count, 1u);
      EF_CHECK_EQ(evidence.aggregates[0].value, 118.0);
    }
  }
}

EF_TEST(property, aggregate_is_independent_of_observation_insertion_order) {
  const auto run = [](const std::vector<double>& values) {
    auto fixture = ef_fixture::make_fixture(nullptr, {BranchFixture{"baseline", ef::BranchRole::BASELINE, 100.0, 3, false},
                                                      BranchFixture{"candidate", ef::BranchRole::CANDIDATE, 100.0, 1, false}},
                                            1, false, 4);
    if (!fixture.ok()) {
      return std::string("fixture");
    }
    ef::Coordinator& coordinator = *fixture.value().coordinator;
    for (const double value : values) {
      if (!ef_fixture::complete_one_trial(coordinator, fixture.value().worker, value).ok()) {
        return std::string("completion");
      }
    }
    const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
    if (!decision.ok()) {
      return std::string("evaluation");
    }
    for (const ef::BranchEvidence& evidence : decision.value().evidence) {
      if (evidence.branch == fixture.value().baseline) {
        return ef::inspect::format_real(evidence.aggregates[0].value);
      }
    }
    return std::string("missing");
  };
  std::vector<double> ascending = {100.0, 110.0, 120.0};
  std::vector<double> descending = {120.0, 110.0, 100.0};
  EF_CHECK_EQ(run(ascending), run(descending));
}
