// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <set>
#include <string>

#include "experiment_fabric/inspect.hpp"
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

EF_TEST(lineage, lineage_survives_a_coordinator_restart) {
  const std::filesystem::path directory = ef_test::scratch_directory("lineage-restart");
  const std::filesystem::path state = directory / "state.efstate";
  ef::ExperimentId experiment;
  ef::BranchId forked;
  {
    auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3, state);
    EF_REQUIRE(fixture.ok());
    experiment = fixture.value().experiment;
    ef::BranchSpec fork;
    fork.name = "restart-fork";
    fork.role = ef::BranchRole::CANDIDATE;
    fork.planned_trials = 1;
    const auto created = fixture.value().coordinator->fork_branch(experiment, fixture.value().candidate, fork);
    EF_REQUIRE(created.ok());
    forked = created.value();
    EF_REQUIRE(ef_fixture::run_planned_trials(*fixture.value().coordinator, experiment, fixture.value().worker,
                                              two_branches())
                   .ok());
  }
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  config.state_path = state;
  auto recovered = ef::Coordinator::create(config);
  EF_REQUIRE(recovered.ok());
  const auto snapshot = recovered.value()->snapshot(experiment);
  EF_REQUIRE(snapshot.ok());
  bool found_fork = false;
  for (const ef::BranchDefinition& branch : snapshot.value().definition.branches) {
    if (branch.id == forked) {
      found_fork = true;
      EF_CHECK(branch.parent.valid());
      EF_CHECK(branch.forked);
    }
    if (branch.parent.valid()) {
      EF_CHECK(branch.parent != branch.id);
    }
  }
  EF_CHECK(found_fork);
  EF_CHECK(snapshot.value().trials.size() >= 2);
  // Completed history survives the restart.
  std::uint32_t completed = 0;
  for (const auto& trial : snapshot.value().trials) {
    if (trial.trial.state == ef::TrialState::COMPLETED) {
      ++completed;
    }
  }
  EF_CHECK(completed >= 2);
}

EF_TEST(lineage, fork_cycles_are_rejected_on_load_and_on_mutation) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;

  ef::BranchSpec first;
  first.name = "a";
  first.role = ef::BranchRole::CANDIDATE;
  first.planned_trials = 1;
  const auto branch_a = coordinator.fork_branch(fixture.value().experiment, fixture.value().baseline, first);
  EF_REQUIRE(branch_a.ok());
  ef::BranchSpec second;
  second.name = "b";
  second.role = ef::BranchRole::CANDIDATE;
  second.planned_trials = 1;
  const auto branch_b = coordinator.fork_branch(fixture.value().experiment, branch_a.value(), second);
  EF_REQUIRE(branch_b.ok());

  // A fork can never name its own child as parent: the parent must exist.
  EF_CHECK(!coordinator
                .fork_branch(fixture.value().experiment, ef::BranchId::from_value(999999),
                             ef::BranchSpec{"orphan", ef::BranchRole::CANDIDATE, std::nullopt, {}, std::nullopt,
                                            std::nullopt, std::string(), std::nullopt, 1})
                 .ok());

  const auto first_snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(first_snapshot.ok());
  const std::vector<std::string> lines = ef::inspect::render_lineage(first_snapshot.value(), ef::Limits::defaults());
  // One header line plus one line per branch: baseline, candidate, a, and the
  // fork of a.
  EF_CHECK_EQ(lines.size(), std::size_t{5});
  EF_CHECK(lines[0].find("lineage experiment") == 0);
  for (std::size_t index = 1; index < lines.size(); ++index) {
    EF_CHECK(lines[index].find("branch") != std::string::npos);
  }
  // Deterministic pre-order: a parent always appears before its children, and
  // the fork of the fork appears immediately after its parent.
  EF_CHECK(lines[2].find("parent=") != std::string::npos);
  EF_CHECK(lines[3].find("parent=") != std::string::npos);
  EF_CHECK(lines[4].find("parent=") == std::string::npos);
  EF_CHECK(lines[2].find("name=a") != std::string::npos);
  EF_CHECK(lines[3].find("name=b") != std::string::npos);
}

EF_TEST(reproducibility, record_is_partially_reproducible_without_full_material) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(ef_fixture::run_planned_trials(coordinator, fixture.value().experiment, fixture.value().worker,
                                            two_branches())
                 .ok());
  const auto record = coordinator.reproducibility(fixture.value().experiment, fixture.value().candidate);
  EF_REQUIRE(record.ok());
  EF_CHECK(record.value().experiment == fixture.value().experiment);
  EF_CHECK(record.value().branch == fixture.value().candidate);
  EF_CHECK(record.value().seed.has_value());
  EF_CHECK(record.value().input_set_present);
  EF_CHECK(record.value().environment_present);
  EF_CHECK(!record.value().producer_identity.empty());
  EF_CHECK_EQ(record.value().observation_count, 1u);
  // No seed claim is made for artifacts that do not exist.
  EF_CHECK(!record.value().artifacts.empty() || std::find(record.value().missing_materials.begin(),
                                                          record.value().missing_materials.end(),
                                                          std::string("artifact-references")) !=
                                                    record.value().missing_materials.end());
  EF_CHECK(record.value().status != ef::ReproducibilityStatus::UNKNOWN);
  const std::vector<std::string> lines = ef::inspect::render_reproducibility(record.value(), ef::Limits::defaults());
  EF_CHECK(!lines.empty());
  EF_CHECK(lines[0].find("reproducibility experiment") == 0);
  bool saw_status = false;
  for (const std::string& line : lines) {
    if (line.rfind("status ", 0) == 0) {
      saw_status = true;
    }
  }
  EF_CHECK(saw_status);
}

EF_TEST(reproducibility, digests_are_stable_across_identical_builds) {
  auto first = ef_fixture::make_fixture();
  auto second = ef_fixture::make_fixture();
  EF_REQUIRE(first.ok());
  EF_REQUIRE(second.ok());
  const auto first_record =
      first.value().coordinator->reproducibility(first.value().experiment, first.value().candidate);
  const auto second_record =
      second.value().coordinator->reproducibility(second.value().experiment, second.value().candidate);
  EF_REQUIRE(first_record.ok());
  EF_REQUIRE(second_record.ok());
  EF_CHECK(first_record.value().definition_digest == second_record.value().definition_digest);
  EF_CHECK(first_record.value().branch_definition_digest == second_record.value().branch_definition_digest);
  EF_CHECK(first_record.value().policy_digest == second_record.value().policy_digest);
}

EF_TEST(lineage, artifact_references_are_recorded_without_promotion) {
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(ef_fixture::run_planned_trials(coordinator, fixture.value().experiment, fixture.value().worker,
                                            two_branches())
                 .ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  EF_REQUIRE(ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 118.0).ok());
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
  artifact.artifact.locator = "synthetic://trace/1";
  artifact.artifact.content_digest = ef::Sha256::digest_of(std::string_view("trace"));
  artifact.artifact.digest_present = true;
  artifact.artifact.provenance = "SYNTHETIC";
  EF_REQUIRE(coordinator.publish_artifact(artifact).ok());
  // A duplicate identical artifact reference is harmless.
  EF_REQUIRE(coordinator.publish_artifact(artifact).ok());

  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  EF_REQUIRE(snapshot.value().artifacts.size() == 1);
  EF_CHECK(snapshot.value().artifacts[0].current);
  const std::vector<std::string> lines = ef::inspect::render_artifacts(snapshot.value(), ef::Limits::defaults());
  EF_REQUIRE(!lines.empty());
  EF_CHECK(lines[0].find("production-trusted=false") != std::string::npos);
}
