// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "fixtures.hpp"
#include "test_support.hpp"

namespace {
namespace ef = experiment_fabric;
using ef_fixture::BranchFixture;

/// An observer that re-enters read-only coordinator APIs from inside a
/// mutation callback. If any coordinator lock were held across the callback
/// this would deadlock, so the test proves the documented rule that callbacks
/// run outside every coordinator lock.
class ReentrantObserver : public ef::CoordinatorObserver {
 public:
  explicit ReentrantObserver(ef::Coordinator* coordinator) : coordinator_(coordinator) {}

  void set_experiment(ef::ExperimentId experiment) { experiment_ = experiment; }
  void set_branch(ef::BranchId branch) { branch_ = branch; }

  void on_event(const std::string& event) override {
    ++events;
    (void)event;
    if (coordinator_ == nullptr || !experiment_.valid()) {
      return;
    }
    if (coordinator_->snapshot(experiment_).ok()) {
      ++snapshots;
    }
    (void)coordinator_->list_experiments();
    (void)coordinator_->state_digest_hex();
  }

  void on_decision(const ef::Decision& decision) override {
    ++decisions;
    (void)decision;
    if (coordinator_ == nullptr || !branch_.valid()) {
      return;
    }
    if (coordinator_->evaluate(experiment_, branch_).ok()) {
      ++evaluations;
    }
  }

  std::atomic<std::uint32_t> events{0};
  std::atomic<std::uint32_t> snapshots{0};
  std::atomic<std::uint32_t> decisions{0};
  std::atomic<std::uint32_t> evaluations{0};

 private:
  ef::Coordinator* coordinator_ = nullptr;
  ef::ExperimentId experiment_;
  ef::BranchId branch_;
};
}  // namespace

EF_TEST(concurrency, observer_callbacks_run_without_holding_coordinator_locks) {
  ReentrantObserver observer(nullptr);
  const std::vector<BranchFixture> branches = {
      BranchFixture{"baseline", ef::BranchRole::BASELINE, 120.0, 1, false},
      BranchFixture{"candidate", ef::BranchRole::CANDIDATE, 95.0, 1, false}};
  auto fixture = ef_fixture::make_fixture(&observer, branches, 1, false, 3);
  EF_REQUIRE(fixture.ok());
  observer.set_experiment(fixture.value().experiment);
  observer.set_branch(fixture.value().candidate);
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  EF_REQUIRE(ef_fixture::run_planned_trials(coordinator, fixture.value().experiment, fixture.value().worker, branches)
                 .ok());
  EF_REQUIRE(coordinator.finalize_experiment(fixture.value().experiment, fixture.value().candidate, "reentrant").ok());
  EF_CHECK(observer.events.load() > 0);
  EF_CHECK(observer.snapshots.load() > 0);
  EF_CHECK_EQ(observer.decisions.load(), 1u);
  EF_CHECK(observer.evaluations.load() > 0);
}

EF_TEST(concurrency, concurrent_publication_and_inspection_are_consistent) {
  const std::vector<BranchFixture> branches = {
      BranchFixture{"baseline", ef::BranchRole::BASELINE, 120.0, 8, false},
      BranchFixture{"candidate", ef::BranchRole::CANDIDATE, 95.0, 8, false}};
  auto fixture = ef_fixture::make_fixture(nullptr, branches, 1, false, 3);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  const ef::BranchId baseline = snapshot.value().definition.branches[0].id;
  for (const ef::BranchDefinition& branch : snapshot.value().definition.branches) {
    for (std::uint32_t index = 0; index < branch.planned_trials; ++index) {
      const auto created = coordinator.create_trial(fixture.value().experiment, branch.id, "concurrent");
      EF_REQUIRE_MESSAGE(created.ok(), created.status().to_string());
    }
  }

  std::vector<std::thread> threads;
  std::atomic<bool> stop{false};
  std::atomic<std::uint32_t> published{0};
  std::atomic<std::uint32_t> inspected{0};
  std::atomic<std::uint32_t> failures{0};

  for (std::uint64_t identity = 1; identity <= 4; ++identity) {
    threads.emplace_back([&, identity]() {
      auto worker = ef_fixture::register_worker(coordinator, identity, 0x1000 + identity);
      if (!worker.ok()) {
        ++failures;
        return;
      }
      while (!stop.load()) {
        auto assignment = ef_fixture::claim_one(coordinator, worker.value());
        if (!assignment.ok()) {
          break;
        }
        const double value = assignment.value().envelope.branch == baseline ? 120.0 : 95.0;
        if (!ef_fixture::publish_value(coordinator, worker.value(), assignment.value(), "latency_ms", value).ok()) {
          ++failures;
          continue;
        }
        if (!ef_fixture::commit(coordinator, worker.value(), assignment.value()).ok()) {
          ++failures;
          continue;
        }
        ++published;
      }
    });
  }
  for (int index = 0; index < 2; ++index) {
    threads.emplace_back([&]() {
      // At least one full inspection always runs, so the assertion below never
      // depends on how the scheduler interleaves the two thread groups.
      do {
        const auto view = coordinator.snapshot(fixture.value().experiment);
        if (!view.ok()) {
          ++failures;
        }
        const auto decision = coordinator.evaluate(fixture.value().experiment, fixture.value().candidate);
        if (!decision.ok()) {
          ++failures;
        }
        if (!coordinator.validate_state().ok()) {
          ++failures;
        }
        (void)coordinator.state_digest_hex();
        ++inspected;
      } while (!stop.load());
    });
  }

  const std::uint32_t target = 16;
  while (published.load() < target && failures.load() == 0) {
    std::this_thread::yield();
  }
  stop.store(true);
  for (std::thread& thread : threads) {
    thread.join();
  }
  EF_CHECK_EQ(failures.load(), 0u);
  EF_CHECK_EQ(published.load(), target);
  EF_CHECK(inspected.load() > 0);
  EF_REQUIRE(coordinator.validate_state().ok());
  const auto final_snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(final_snapshot.ok());
  EF_CHECK_EQ(final_snapshot.value().observations.size(), std::size_t{target});
  for (const auto& trial : final_snapshot.value().trials) {
    EF_CHECK(trial.trial.committed_attempts <= 1);
  }
}

EF_TEST(concurrency, completion_races_cancellation_without_double_commit) {
  const std::vector<BranchFixture> branches = {
      BranchFixture{"baseline", ef::BranchRole::BASELINE, 120.0, 32, false},
      BranchFixture{"candidate", ef::BranchRole::CANDIDATE, 95.0, 1, false}};
  auto fixture = ef_fixture::make_fixture(nullptr, branches, 1, false, 4);
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(snapshot.ok());
  std::vector<ef::TrialId> trials;
  for (const ef::BranchDefinition& branch : snapshot.value().definition.branches) {
    if (branch.role != ef::BranchRole::BASELINE) {
      continue;
    }
    for (std::uint32_t index = 0; index < branch.planned_trials; ++index) {
      const auto created = coordinator.create_trial(fixture.value().experiment, branch.id, "race");
      EF_REQUIRE(created.ok());
      trials.push_back(created.value());
    }
  }

  std::atomic<std::uint32_t> committed{0};
  std::atomic<std::uint32_t> published{0};
  std::thread publisher([&]() {
    while (true) {
      auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
      if (!assignment.ok()) {
        break;
      }
      if (!ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 118.0).ok()) {
        continue;
      }
      ++published;
      if (ef_fixture::commit(coordinator, fixture.value().worker, assignment.value()).ok()) {
        ++committed;
      }
    }
  });
  std::thread canceller([&]() {
    for (const ef::TrialId& trial : trials) {
      (void)coordinator.cancel_trial(trial, "race");
    }
  });
  publisher.join();
  canceller.join();

  const auto final_snapshot = coordinator.snapshot(fixture.value().experiment);
  EF_REQUIRE(final_snapshot.ok());
  std::uint32_t completed = 0;
  std::uint32_t cancelled_trials = 0;
  for (const auto& trial : final_snapshot.value().trials) {
    if (trial.trial.state == ef::TrialState::COMPLETED) {
      ++completed;
      EF_CHECK_EQ(trial.trial.committed_attempts, 1u);
    }
    if (trial.trial.state == ef::TrialState::CANCELLED) {
      ++cancelled_trials;
      EF_CHECK_EQ(trial.trial.committed_attempts, 0u);
    }
  }
  EF_CHECK_EQ(completed, committed.load());
  EF_CHECK_EQ(cancelled_trials + completed, static_cast<std::uint32_t>(trials.size()));
  EF_REQUIRE(coordinator.validate_state().ok());
}

EF_TEST(concurrency, repeated_start_and_stop_is_stable) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  for (int round = 0; round < 5; ++round) {
    EF_REQUIRE(coordinator.start_server().ok());
    EF_CHECK(coordinator.server_running());
    EF_CHECK(coordinator.port() != 0);
    EF_REQUIRE(coordinator.stop_server().ok());
    EF_CHECK(!coordinator.server_running());
  }
}

EF_TEST(concurrency, shutdown_revokes_authority_for_late_completions) {
  auto fixture = ef_fixture::make_fixture();
  EF_REQUIRE(fixture.ok());
  ef::Coordinator& coordinator = *fixture.value().coordinator;
  const auto trial = coordinator.create_trial(fixture.value().experiment, fixture.value().baseline, "late");
  EF_REQUIRE(trial.ok());
  auto assignment = ef_fixture::claim_one(coordinator, fixture.value().worker);
  EF_REQUIRE(assignment.ok());
  EF_REQUIRE(coordinator.start_server().ok());
  coordinator.request_shutdown();
  const ef::Status published =
      ef_fixture::publish_value(coordinator, fixture.value().worker, assignment.value(), "latency_ms", 118.0);
  EF_CHECK(!published.ok());
  EF_CHECK_EQ(published.code(), ef::ErrorCode::SHUTTING_DOWN);
  EF_REQUIRE(coordinator.stop_server().ok());
}
