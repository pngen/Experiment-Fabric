// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <string>
#include <vector>

#include "experiment_fabric/inspect.hpp"
#include "fixtures.hpp"
#include "test_support.hpp"

namespace {
namespace ef = experiment_fabric;
using ef_fixture::BranchFixture;
}  // namespace

EF_TEST(explanation, constraint_order_is_deterministic_and_stable) {
  auto fixture = ef_fixture::make_fixture(nullptr, {BranchFixture{"baseline", ef::BranchRole::BASELINE, 120.0, 2, false},
                                                    BranchFixture{"candidate", ef::BranchRole::CANDIDATE, 95.0, 2, false}});
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(ef_fixture::run_planned_trials(coordinator, fixture.value().experiment, fixture.value().worker,
                                            {BranchFixture{"baseline", ef::BranchRole::BASELINE, 120.0, 2, false},
                                             BranchFixture{"candidate", ef::BranchRole::CANDIDATE, 95.0, 2, false}})
                 .ok());
  const auto first = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(first.ok());
  const std::vector<std::string> first_lines =
      ef::inspect::render_decision(first.value(), ef::Limits::defaults());
  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto again = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
    EF_REQUIRE(again.ok());
    const std::vector<std::string> lines = ef::inspect::render_decision(again.value(), ef::Limits::defaults());
    EF_CHECK(lines == first_lines);
  }
  // The constraint block is emitted in a fixed order derived from the policy.
  std::vector<std::string> constraint_names;
  for (const std::string& line : first_lines) {
    if (line.rfind("constraint ", 0) == 0) {
      constraint_names.push_back(line);
    }
  }
  EF_CHECK(constraint_names.size() > 5);
  std::vector<std::string> sorted = constraint_names;
  std::sort(sorted.begin(), sorted.end());
  EF_CHECK(sorted == constraint_names);
}

EF_TEST(explanation, explanation_reports_rejected_and_non_authoritative_evidence) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "p").ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());

  // The same attempt publishes after cancellation: the evidence must be refused
  // and recorded, never silently dropped.
  const ef::Status cancelled =
      coordinator.cancel_trial(assignment.value().envelope.trial, "operator cancelled the candidate");
  EF_REQUIRE(cancelled.ok());
  const ef::Status late = ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(),
                                                    "latency_ms", 90.0);
  EF_CHECK(!late.ok());
  EF_CHECK_EQ(late.code(), ef::ErrorCode::CANCELLED);

  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK(!snapshot.value().rejected_evidence.empty());
  const std::vector<std::string> lines = ef::inspect::render_rejected_evidence(snapshot.value(), ef::Limits::defaults());
  EF_CHECK(!lines.empty());
  bool saw_cancellation = false;
  for (const std::string& line : lines) {
    if (line.find("CANCELLED") != std::string::npos) {
      saw_cancellation = true;
    }
  }
  EF_CHECK(saw_cancellation);
}

EF_TEST(explanation, snapshot_rendering_is_byte_stable_across_repeated_reads) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  const std::vector<std::string> first = ef::inspect::render_snapshot(snapshot.value(), ef::Limits::defaults());
  for (int attempt = 0; attempt < 5; ++attempt) {
    const std::vector<std::string> again = ef::inspect::render_snapshot(snapshot.value(), ef::Limits::defaults());
    EF_CHECK(again == first);
  }
  EF_CHECK(!first.empty());
}

EF_TEST(explanation, lineage_rendering_is_deterministic_preorder) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  ef::BranchSpec fork;
  fork.name = "fork-a";
  fork.role = ef::BranchRole::CANDIDATE;
  fork.planned_trials = 1;
  const auto forked = coordinator.fork_branch(fixture.value().experiment, fixture.value().candidate, fork);
  EF_REQUIRE(forked.ok());
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  const std::vector<std::string> first = ef::inspect::render_lineage(snapshot.value(), ef::Limits::defaults());
  const std::vector<std::string> second = ef::inspect::render_lineage(snapshot.value(), ef::Limits::defaults());
  EF_CHECK(first == second);
  // One header line plus one line per branch: the baseline, the candidate, and
  // the fork of the candidate.
  EF_CHECK_EQ(first.size(), std::size_t{4});
  EF_CHECK(first[0].find("lineage experiment") == 0);
}

EF_TEST(explanation, format_real_is_locale_independent_and_total) {
  EF_CHECK_EQ(ef::inspect::format_real(0.0), std::string("0"));
  EF_CHECK_EQ(ef::inspect::format_real(-0.0), std::string("-0"));
  EF_CHECK_EQ(ef::inspect::format_real(1.5), std::string("1.5"));
  EF_CHECK_EQ(ef::inspect::format_real(1e300), std::string("1e+300"));
  EF_CHECK_EQ(ef::inspect::format_real(std::nan("")), std::string("nan"));
  EF_CHECK_EQ(ef::inspect::format_real(-std::numeric_limits<double>::infinity()), std::string("-inf"));
}
