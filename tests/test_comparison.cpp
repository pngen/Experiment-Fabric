// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <string>

#include "fixtures.hpp"
#include "test_support.hpp"

namespace {
namespace ef = experiment_fabric;
using ef_fixture::BranchFixture;

std::vector<BranchFixture> two_branches(double baseline_value, double candidate_value,
                                        std::uint32_t trials = 2) {
  return {BranchFixture{"baseline", ef::BranchRole::BASELINE, baseline_value, trials, false},
          BranchFixture{"candidate", ef::BranchRole::CANDIDATE, candidate_value, trials, false}};
}
}  // namespace

EF_TEST(comparison, improvement_is_accepted_only_after_hard_constraints_pass) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(120.0, 95.0));
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(ef_fixture::run_planned_trials(coordinator, fixture.value().experiment, fixture.value().worker,
                                            two_branches(120.0, 95.0))
                 .ok());
  const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(decision.ok());
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::ACCEPT);
  EF_CHECK_EQ(decision.value().factors.size(), std::size_t{1});
  EF_CHECK(decision.value().factors[0].satisfied);
  EF_CHECK(decision.value().factors[0].delta < 0.0);
  for (const ef::ConstraintResult& constraint : decision.value().constraints) {
    EF_CHECK(constraint.passed);
  }
}

EF_TEST(comparison, regression_is_rejected) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(120.0, 180.0));
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(ef_fixture::run_planned_trials(coordinator, fixture.value().experiment, fixture.value().worker,
                                            two_branches(120.0, 180.0))
                 .ok());
  const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(decision.ok());
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::REJECT);
  EF_CHECK(decision.value().reason_kind == ef::FailureKind::POLICY_REJECTION);
}

EF_TEST(comparison, a_favorable_number_with_insufficient_evidence_is_not_accepted) {
  // The candidate is dramatically better but the policy demands four completed
  // trials per branch while only one is planned and executed.
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(120.0, 40.0, 1), 4);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(ef_fixture::run_planned_trials(coordinator, fixture.value().experiment, fixture.value().worker,
                                            two_branches(120.0, 40.0, 1))
                 .ok());
  const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(decision.ok());
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::INSUFFICIENT_EVIDENCE);
  EF_CHECK(decision.value().outcome != ef::DecisionOutcome::ACCEPT);
  bool saw_failed_constraint = false;
  for (const ef::ConstraintResult& constraint : decision.value().constraints) {
    if (!constraint.passed) {
      saw_failed_constraint = true;
    }
  }
  EF_CHECK(saw_failed_constraint);
  // Finalization must not force the insufficient result into a binary outcome.
  const auto finalized = coordinator.finalize_experiment(fixture.value().experiment, fixture.value().candidate,
                                                         "attempted");
  EF_REQUIRE(finalized.ok());
  EF_CHECK(finalized.value().outcome == ef::DecisionOutcome::INSUFFICIENT_EVIDENCE);
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK(snapshot.value().state == ef::ExperimentState::OPEN);
}

EF_TEST(comparison, invalid_evidence_invalidates_the_decision) {
  const std::vector<BranchFixture> branches = {BranchFixture{"baseline", ef::BranchRole::BASELINE, 120.0, 2, false},
                                               BranchFixture{"candidate", ef::BranchRole::CANDIDATE, 30.0, 2, true}};
  auto fixture = ef_fixture::make_fixture(nullptr, branches, 2, true);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(ef_fixture::run_planned_trials(coordinator, fixture.value().experiment, fixture.value().worker, branches)
                 .ok());
  const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(decision.ok());
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::INVALID);
  EF_CHECK(decision.value().reason_kind == ef::FailureKind::INVALID_OBSERVATION);
}

EF_TEST(comparison, exact_tie_is_inconclusive_not_accepted) {
  ef::ExperimentSpec spec = ef_fixture::latency_spec(two_branches(100.0, 100.0), 1, false, 1);
  // A policy that requires no improvement at all: exact equality is then a tie.
  spec.rules[0].threshold = 0.0;
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  auto coordinator_handle = ef::Coordinator::create(config);
  EF_REQUIRE(coordinator_handle.ok());
  const auto experiment = coordinator_handle.value()->create_experiment(spec);
  EF_REQUIRE(experiment.ok());
  const auto initial = coordinator_handle.value()->snapshot(experiment.value());
  EF_REQUIRE(initial.ok());
  struct LocalFixture {
    ef::Coordinator* coordinator = nullptr;
    ef::ExperimentId experiment;
    ef::BranchId baseline;
    ef::BranchId candidate;
    ef_fixture::TestWorker worker;
  } fixture_holder;
  fixture_holder.coordinator = coordinator_handle.value().get();
  fixture_holder.experiment = experiment.value();
  for (const ef::BranchDefinition& branch : initial.value().definition.branches) {
    if (branch.role == ef::BranchRole::BASELINE) {
      fixture_holder.baseline = branch.id;
    } else {
      fixture_holder.candidate = branch.id;
    }
  }
  auto registered = ef_fixture::register_worker(*coordinator_handle.value(), 1);
  EF_REQUIRE(registered.ok());
  fixture_holder.worker = registered.value();
  auto fixture = ef::Result<LocalFixture>(fixture_holder);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "b").ok());
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().candidate, "c").ok());
  EF_REQUIRE(ef_fixture::complete_one_trial(coordinator, fixture.value().worker, 100.0).ok());
  EF_REQUIRE(ef_fixture::complete_one_trial(coordinator, fixture.value().worker, 100.0).ok());
  const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(decision.ok());
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::INCONCLUSIVE);
  EF_CHECK(decision.value().tied);
  EF_CHECK(!decision.value().tie_break_rule.empty());
}

EF_TEST(comparison, a_policy_without_decisive_factors_reports_rather_than_decides) {
  ef::ExperimentSpec spec = ef_fixture::latency_spec(two_branches(120.0, 95.0), 1, false, 1);
  spec.rules[0].op = ef::ComparisonOp::REPORT_ONLY;
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  auto coordinator = ef::Coordinator::create(config);
  EF_REQUIRE(coordinator.ok());
  const auto experiment = coordinator.value()->create_experiment(spec);
  EF_REQUIRE(experiment.ok());
  auto worker = ef_fixture::register_worker(*coordinator.value(), 1);
  EF_REQUIRE(worker.ok());
  const auto initial = coordinator.value()->snapshot(experiment.value());
  EF_REQUIRE(initial.ok());
  EF_REQUIRE(coordinator.value()->create_trial(experiment.value(), initial.value().definition.branches[0].id, "b").ok());
  EF_REQUIRE(coordinator.value()->create_trial(experiment.value(), initial.value().definition.branches[1].id, "c").ok());
  EF_REQUIRE(ef_fixture::complete_one_trial(*coordinator.value(), worker.value(), 120.0).ok());
  EF_REQUIRE(ef_fixture::complete_one_trial(*coordinator.value(), worker.value(), 95.0).ok());
  const auto snapshot = coordinator.value()->snapshot(experiment.value());
  EF_REQUIRE(snapshot.ok());
  const auto decision = coordinator.value()->evaluate(experiment.value(), snapshot.value().definition.branches[1].id);
  EF_REQUIRE(decision.ok());
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::INCONCLUSIVE);
}

EF_TEST(comparison, maximum_regression_tolerance_admits_a_bounded_tradeoff) {
  ef::ExperimentSpec spec;
  spec.name = "tradeoff";
  spec.hypothesis_statement = "Throughput improves without an unbounded memory regression.";
  spec.provenance = "SYNTHETIC test";
  ef::MetricSpec throughput;
  throughput.name = "throughput";
  throughput.unit = "ops/s";
  throughput.direction = ef::MetricDirection::MAXIMIZE;
  ef::MetricSpec memory;
  memory.name = "peak_memory_mb";
  memory.unit = "MiB";
  memory.direction = ef::MetricDirection::MINIMIZE;
  spec.metrics = {throughput, memory};
  ef::BranchSpec baseline;
  baseline.name = "baseline";
  baseline.role = ef::BranchRole::BASELINE;
  baseline.planned_trials = 1;
  baseline.parameters = {ef::make_real_parameter("throughput", 1000.0), ef::make_real_parameter("peak_memory_mb", 512.0)};
  ef::BranchSpec candidate = baseline;
  candidate.name = "candidate";
  candidate.role = ef::BranchRole::CANDIDATE;
  candidate.parameters = {ef::make_real_parameter("throughput", 1250.0), ef::make_real_parameter("peak_memory_mb", 530.0)};
  spec.branches = {baseline, candidate};
  ef::ComparisonRuleSpec gain;
  gain.metric = "throughput";
  gain.op = ef::ComparisonOp::MIN_IMPROVEMENT;
  gain.threshold = 50.0;
  gain.priority = 0;
  ef::ComparisonRuleSpec tolerance;
  tolerance.metric = "peak_memory_mb";
  tolerance.op = ef::ComparisonOp::MAX_REGRESSION_TOLERANCE;
  tolerance.threshold = 32.0;
  tolerance.priority = 1;
  spec.rules = {gain, tolerance};
  spec.min_completed_trials_per_branch = 1;
  spec.experiment_seed = 3;

  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  auto coordinator = ef::Coordinator::create(config);
  EF_REQUIRE(coordinator.ok());
  const auto experiment = coordinator.value()->create_experiment(spec);
  EF_REQUIRE(experiment.ok());
  const auto snapshot = coordinator.value()->snapshot(experiment.value());
  EF_REQUIRE(snapshot.ok());
  const ef::BranchId baseline_id = snapshot.value().definition.branches[0].id;
  const ef::BranchId candidate_id = snapshot.value().definition.branches[1].id;
  EF_REQUIRE(coordinator.value()->create_trial(experiment.value(), baseline_id, "b").ok());
  EF_REQUIRE(coordinator.value()->create_trial(experiment.value(), candidate_id, "c").ok());
  auto worker = ef_fixture::register_worker(*coordinator.value(), 1);
  EF_REQUIRE(worker.ok());

  auto baseline_assignment = ef_fixture::claim_one(*coordinator.value(), worker.value());
  EF_REQUIRE(baseline_assignment.ok());
  EF_REQUIRE(ef_fixture::publish_value(*coordinator.value(), worker.value(), baseline_assignment.value(),
                                       "throughput", 1000.0)
                 .ok());
  EF_REQUIRE(ef_fixture::publish_value(*coordinator.value(), worker.value(), baseline_assignment.value(),
                                       "peak_memory_mb", 512.0)
                 .ok());
  EF_REQUIRE(ef_fixture::commit(*coordinator.value(), worker.value(), baseline_assignment.value()).ok());

  auto candidate_assignment = ef_fixture::claim_one(*coordinator.value(), worker.value());
  EF_REQUIRE(candidate_assignment.ok());
  EF_REQUIRE(ef_fixture::publish_value(*coordinator.value(), worker.value(), candidate_assignment.value(),
                                       "throughput", 1250.0)
                 .ok());
  EF_REQUIRE(ef_fixture::publish_value(*coordinator.value(), worker.value(), candidate_assignment.value(),
                                       "peak_memory_mb", 530.0)
                 .ok());
  EF_REQUIRE(ef_fixture::commit(*coordinator.value(), worker.value(), candidate_assignment.value()).ok());

  const auto decision = coordinator.value()->evaluate(experiment.value(), candidate_id);
  EF_REQUIRE(decision.ok());
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::ACCEPT);

  // A memory regression beyond the tolerance must reject the candidate.
  ef::ExperimentSpec strict = spec;
  strict.rules[1].threshold = 5.0;
  auto second = ef::Coordinator::create([] {
    ef::Coordinator::Config config;
    config.limits = ef::Limits::defaults();
    return config;
  }());
  EF_REQUIRE(second.ok());
  const auto second_experiment = second.value()->create_experiment(strict);
  EF_REQUIRE(second_experiment.ok());
  const auto second_snapshot = second.value()->snapshot(second_experiment.value());
  EF_REQUIRE(second_snapshot.ok());
  EF_REQUIRE(second.value()
                 ->create_trial(second_experiment.value(), second_snapshot.value().definition.branches[0].id, "b")
                 .ok());
  EF_REQUIRE(second.value()
                 ->create_trial(second_experiment.value(), second_snapshot.value().definition.branches[1].id, "c")
                 .ok());
  auto second_worker = ef_fixture::register_worker(*second.value(), 1);
  EF_REQUIRE(second_worker.ok());
  auto second_baseline = ef_fixture::claim_one(*second.value(), second_worker.value());
  EF_REQUIRE(second_baseline.ok());
  EF_REQUIRE(ef_fixture::publish_value(*second.value(), second_worker.value(), second_baseline.value(), "throughput",
                                       1000.0).ok());
  EF_REQUIRE(ef_fixture::publish_value(*second.value(), second_worker.value(), second_baseline.value(),
                                       "peak_memory_mb", 512.0).ok());
  EF_REQUIRE(ef_fixture::commit(*second.value(), second_worker.value(), second_baseline.value()).ok());
  auto second_candidate = ef_fixture::claim_one(*second.value(), second_worker.value());
  EF_REQUIRE(second_candidate.ok());
  EF_REQUIRE(ef_fixture::publish_value(*second.value(), second_worker.value(), second_candidate.value(), "throughput",
                                       1250.0).ok());
  EF_REQUIRE(ef_fixture::publish_value(*second.value(), second_worker.value(), second_candidate.value(),
                                       "peak_memory_mb", 530.0).ok());
  EF_REQUIRE(ef_fixture::commit(*second.value(), second_worker.value(), second_candidate.value()).ok());
  const auto strict_decision =
      second.value()->evaluate(second_experiment.value(), second_snapshot.value().definition.branches[1].id);
  EF_REQUIRE(strict_decision.ok());
  EF_CHECK(strict_decision.value().outcome == ef::DecisionOutcome::REJECT);
}

EF_TEST(comparison, decision_is_independent_of_trial_completion_order) {
  const std::vector<double> values = {118.0, 122.0, 96.0, 94.0};
  std::vector<double> first_order = values;
  std::vector<double> second_order = values;
  std::reverse(second_order.begin(), second_order.end());

  const auto run = [](const std::vector<double>& order) {
    auto fixture = ef_fixture::make_fixture(nullptr, two_branches(120.0, 95.0));
    if (!fixture.ok()) {
      return std::string("fixture failed");
    }
    ef::Coordinator& coordinator = *fixture.value().coordinator;
    for (const double value : order) {
      if (!ef_fixture::complete_one_trial(coordinator, fixture.value().worker, value).ok()) {
        return std::string("completion failed");
      }
    }
    const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
    if (!decision.ok()) {
      return std::string("evaluation failed");
    }
    std::string text = std::string(ef::to_string(decision.value().outcome));
    for (const ef::ComparisonFactor& factor : decision.value().factors) {
      text.append("|");
      text.append(std::to_string(factor.delta));
    }
    return text;
  };
  EF_CHECK_EQ(run(first_order), run(second_order));
}

EF_TEST(comparison, environment_mismatch_invalidates_the_comparison) {
  ef::ExperimentSpec spec = ef_fixture::latency_spec(two_branches(120.0, 95.0), 1, false, 1);
  spec.branches[1].environment_fingerprint = "different-x64";
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  auto coordinator = ef::Coordinator::create(config);
  EF_REQUIRE(coordinator.ok());
  const auto experiment = coordinator.value()->create_experiment(spec);
  EF_REQUIRE(experiment.ok());
  const auto snapshot = coordinator.value()->snapshot(experiment.value());
  EF_REQUIRE(snapshot.ok());
  EF_REQUIRE(coordinator.value()->create_trial(experiment.value(), snapshot.value().definition.branches[0].id, "b").ok());
  EF_REQUIRE(coordinator.value()->create_trial(experiment.value(), snapshot.value().definition.branches[1].id, "c").ok());
  auto worker = ef_fixture::register_worker(*coordinator.value(), 1);
  EF_REQUIRE(worker.ok());
  EF_REQUIRE(ef_fixture::complete_one_trial(*coordinator.value(), worker.value(), 120.0).ok());
  EF_REQUIRE(ef_fixture::complete_one_trial(*coordinator.value(), worker.value(), 95.0).ok());
  const auto decision = coordinator.value()->evaluate(experiment.value(), snapshot.value().definition.branches[1].id);
  EF_REQUIRE(decision.ok());
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::INVALID);
}
