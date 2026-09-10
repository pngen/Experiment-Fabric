// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <fstream>
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

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
}  // namespace

EF_TEST(persistence, clean_save_and_load_round_trip) {
  const std::filesystem::path directory = ef_test::scratch_directory("persistence-round-trip");
  const std::filesystem::path state = directory / "state.efstate";
  ef::ExperimentId experiment;
  {
    auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3, state);
    EF_REQUIRE(fixture.ok());
    experiment = fixture.value().experiment;
    EF_REQUIRE(ef_fixture::run_planned_trials(*fixture.value().coordinator, experiment, fixture.value().worker,
                                              two_branches())
                   .ok());
    EF_REQUIRE(fixture.value().coordinator->finalize_experiment(experiment, fixture.value().candidate, "closed").ok());
  }
  EF_CHECK(std::filesystem::exists(state));

  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  config.state_path = state;
  auto recovered = ef::Coordinator::create(config);
  EF_REQUIRE(recovered.ok());
  EF_REQUIRE(recovered.value()->validate_state().ok());
  const auto snapshot = recovered.value()->snapshot(experiment);
  EF_REQUIRE(snapshot.ok());
  EF_CHECK(snapshot.value().state == ef::ExperimentState::FINALIZED);
  EF_CHECK_EQ(snapshot.value().decisions.size(), std::size_t{1});
  EF_CHECK(snapshot.value().decisions[0].outcome == ef::DecisionOutcome::ACCEPT);
  EF_CHECK_EQ(snapshot.value().rollback_points.size(), std::size_t{1});
  EF_CHECK_EQ(snapshot.value().observations.size(), std::size_t{2});
}

EF_TEST(persistence, every_truncation_is_rejected) {
  const std::filesystem::path directory = ef_test::scratch_directory("persistence-truncation");
  const std::filesystem::path state = directory / "state.efstate";
  {
    auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3, state);
    EF_REQUIRE(fixture.ok());
    EF_REQUIRE(ef_fixture::run_planned_trials(*fixture.value().coordinator, fixture.value().experiment,
                                              fixture.value().worker, two_branches())
                   .ok());
  }
  const std::vector<std::uint8_t> image = read_file(state);
  EF_REQUIRE(image.size() > 64);
  const std::filesystem::path probe = directory / "probe.efstate";
  const std::vector<std::size_t> cuts = {0, 1, 8, 16, 24, 40, 55, 56, 57, 64, image.size() / 2, image.size() - 1};
  for (const std::size_t cut : cuts) {
    std::vector<std::uint8_t> truncated(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(cut));
    write_file(probe, truncated);
    const auto loaded = ef::persistence::load(probe, ef::Limits::defaults());
    EF_CHECK(!loaded.ok());
    EF_CHECK_EQ(loaded.status().code(), ef::ErrorCode::CORRUPT_PERSISTENCE);
  }
}

EF_TEST(persistence, trailing_garbage_is_rejected) {
  const std::filesystem::path directory = ef_test::scratch_directory("persistence-trailing");
  const std::filesystem::path state = directory / "state.efstate";
  {
    auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3, state);
    EF_REQUIRE(fixture.ok());
  }
  std::vector<std::uint8_t> image = read_file(state);
  image.push_back(0x00);
  const std::filesystem::path probe = directory / "probe.efstate";
  write_file(probe, image);
  const auto loaded = ef::persistence::load(probe, ef::Limits::defaults());
  EF_CHECK(!loaded.ok());
  EF_CHECK_EQ(loaded.status().code(), ef::ErrorCode::CORRUPT_PERSISTENCE);
}

EF_TEST(persistence, every_single_byte_flip_in_the_header_is_rejected) {
  const std::filesystem::path directory = ef_test::scratch_directory("persistence-header-flip");
  const std::filesystem::path state = directory / "state.efstate";
  {
    auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3, state);
    EF_REQUIRE(fixture.ok());
  }
  const std::vector<std::uint8_t> image = read_file(state);
  const std::filesystem::path probe = directory / "probe.efstate";
  for (std::size_t offset = 0; offset < 56 && offset < image.size(); ++offset) {
    std::vector<std::uint8_t> corrupted = image;
    corrupted[offset] = static_cast<std::uint8_t>(corrupted[offset] ^ 0xFFu);
    write_file(probe, corrupted);
    const auto loaded = ef::persistence::load(probe, ef::Limits::defaults());
    EF_CHECK_MESSAGE(!loaded.ok(), "header byte " + std::to_string(offset) + " was accepted after corruption");
  }
}

EF_TEST(persistence, body_corruption_is_detected) {
  const std::filesystem::path directory = ef_test::scratch_directory("persistence-body-corruption");
  const std::filesystem::path state = directory / "state.efstate";
  {
    auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3, state);
    EF_REQUIRE(fixture.ok());
    EF_REQUIRE(ef_fixture::run_planned_trials(*fixture.value().coordinator, fixture.value().experiment,
                                              fixture.value().worker, two_branches())
                   .ok());
  }
  const std::vector<std::uint8_t> image = read_file(state);
  const std::filesystem::path probe = directory / "probe.efstate";
  std::uint32_t rejected = 0;
  std::uint32_t accepted = 0;
  for (std::size_t offset = 56; offset < image.size(); offset += 7) {
    std::vector<std::uint8_t> corrupted = image;
    corrupted[offset] = static_cast<std::uint8_t>(corrupted[offset] ^ 0x5Au);
    write_file(probe, corrupted);
    const auto loaded = ef::persistence::load(probe, ef::Limits::defaults());
    if (loaded.ok()) {
      ++accepted;
    } else {
      ++rejected;
    }
  }
  EF_CHECK_EQ(accepted, 0u);
  EF_CHECK(rejected > 10);
}

EF_TEST(persistence, atomic_replacement_leaves_no_partial_image) {
  const std::filesystem::path directory = ef_test::scratch_directory("persistence-atomic");
  const std::filesystem::path state = directory / "state.efstate";
  auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3, state);
  EF_REQUIRE(fixture.ok());
  for (int round = 0; round < 8; ++round) {
    EF_REQUIRE(fixture.value().coordinator->save().ok());
    const auto loaded = ef::persistence::load(state, ef::Limits::defaults());
    EF_REQUIRE(loaded.ok());
  }
  std::uint32_t temporaries = 0;
  for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().find(".tmp.") != std::string::npos) {
      ++temporaries;
    }
  }
  EF_CHECK_EQ(temporaries, 0u);
}

EF_TEST(persistence, leftover_temporary_images_are_discarded_on_start) {
  const std::filesystem::path directory = ef_test::scratch_directory("persistence-temp-cleanup");
  const std::filesystem::path state = directory / "state.efstate";
  {
    auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3, state);
    EF_REQUIRE(fixture.ok());
  }
  write_file(directory / "state.efstate.tmp.77", std::vector<std::uint8_t>{1, 2, 3});
  write_file(directory / "state.efstate.tmp.78", std::vector<std::uint8_t>{4, 5, 6});
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  config.state_path = state;
  auto recovered = ef::Coordinator::create(config);
  EF_REQUIRE(recovered.ok());
  EF_CHECK(!std::filesystem::exists(directory / "state.efstate.tmp.77"));
  EF_CHECK(!std::filesystem::exists(directory / "state.efstate.tmp.78"));
  // The committed image is untouched.
  const auto loaded = ef::persistence::load(state, ef::Limits::defaults());
  EF_CHECK(loaded.ok());
}

EF_TEST(persistence, corrupt_state_is_refused_at_startup) {
  const std::filesystem::path directory = ef_test::scratch_directory("persistence-refuse");
  const std::filesystem::path state = directory / "state.efstate";
  {
    std::ofstream stream(state, std::ios::binary | std::ios::trunc);
    const std::string garbage = "not an experiment fabric state image at all";
    stream.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
  }
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  config.state_path = state;
  const auto created = ef::Coordinator::create(config);
  EF_CHECK(!created.ok());
  EF_CHECK_EQ(created.status().code(), ef::ErrorCode::CORRUPT_PERSISTENCE);
}

EF_TEST(persistence, missing_state_file_starts_a_fresh_coordinator) {
  const std::filesystem::path directory = ef_test::scratch_directory("persistence-fresh");
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  config.state_path = directory / "does-not-exist.efstate";
  auto created = ef::Coordinator::create(config);
  EF_REQUIRE(created.ok());
  const auto listed = created.value()->list_experiments();
  EF_REQUIRE(listed.ok());
  EF_CHECK(listed.value().empty());
}

EF_TEST(persistence, epochs_strictly_increase_across_restarts) {
  const std::filesystem::path directory = ef_test::scratch_directory("persistence-epochs");
  const std::filesystem::path state = directory / "state.efstate";
  ef::CoordinatorEpoch previous = ef::CoordinatorEpoch::from_value(0);
  for (int round = 0; round < 5; ++round) {
    ef::Coordinator::Config config;
    config.limits = ef::Limits::defaults();
    config.state_path = state;
    auto created = ef::Coordinator::create(config);
    EF_REQUIRE(created.ok());
    const ef::CoordinatorEpoch epoch = created.value()->epoch();
    EF_CHECK(previous < epoch);
    previous = epoch;
    EF_REQUIRE(created.value()->save().ok());
  }
}

EF_TEST(persistence, recovered_dynamic_authority_is_never_silently_live) {
  const std::filesystem::path directory = ef_test::scratch_directory("persistence-recovery");
  const std::filesystem::path state = directory / "state.efstate";
  ef::ExperimentId experiment;
  ef::TrialId open_trial;
  ef::TrialAttemptId open_attempt;
  {
    auto fixture = ef_fixture::make_fixture(nullptr, two_branches(), 1, false, 3, state);
    EF_REQUIRE(fixture.ok());
    experiment = fixture.value().experiment;
    const auto trial = fixture.value().coordinator->create_trial(experiment, fixture.value().baseline, "open");
    EF_REQUIRE(trial.ok());
    open_trial = trial.value();
    auto assignment = ef_fixture::claim_one(*fixture.value().coordinator, fixture.value().worker);
    EF_REQUIRE(assignment.ok());
    open_attempt = assignment.value().envelope.attempt;
    EF_REQUIRE(ef_fixture::publish_value(*fixture.value().coordinator, fixture.value().worker, assignment.value(),
                                         "latency_ms", 118.0).ok());
  }
  ef::Coordinator::Config config;
  config.limits = ef::Limits::defaults();
  config.state_path = state;
  auto recovered = ef::Coordinator::create(config);
  EF_REQUIRE(recovered.ok());
  const auto snapshot = recovered.value()->snapshot(experiment);
  EF_REQUIRE(snapshot.ok());
  for (const auto& trial : snapshot.value().trials) {
    if (trial.trial.id != open_trial) {
      continue;
    }
    EF_CHECK(trial.trial.state == ef::TrialState::READY);
    EF_CHECK(!trial.trial.authoritative_attempt.valid());
    EF_CHECK(trial.trial.failure_kind == ef::FailureKind::PRODUCER_LOSS);
    EF_CHECK(!trial.attempts.empty());
    EF_CHECK(trial.attempts[0].id == open_attempt);
    EF_CHECK(ef::is_attempt_terminal(trial.attempts[0].state));
  }
  // The prior process authority does not survive: a worker must register again.
  const ef::ClaimTrialRequest claim;
  ef::ClaimTrialRequest stale = claim;
  stale.epoch = ef::CoordinatorEpoch::from_value(recovered.value()->epoch().value() - 1);
  stale.worker = ef::WorkerId::from_value(1);
  stale.boot = ef::WorkerBootId::from_value(0xABCDEF01);
  stale.max_claims = 1;
  const auto reply = recovered.value()->claim_trials(stale);
  EF_REQUIRE(reply.ok());
  EF_CHECK_EQ(reply.value().status.code(), ef::ErrorCode::STALE_EPOCH);
}
