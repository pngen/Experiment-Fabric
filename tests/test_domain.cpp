// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "experiment_fabric/domain.hpp"
#include "test_support.hpp"

namespace {
namespace ef = experiment_fabric;
}  // namespace

EF_TEST(domain, names_round_trip) {
  for (std::uint8_t value = 0; value <= static_cast<std::uint8_t>(ef::TrialState::REVALIDATION_REQUIRED); ++value) {
    const auto state = static_cast<ef::TrialState>(value);
    const std::string_view name = ef::to_string(state);
    EF_CHECK(name != std::string_view("UNKNOWN"));
    const auto parsed = ef::parse_trial_state(name);
    EF_REQUIRE(parsed.has_value());
    EF_CHECK(*parsed == state);
  }
  EF_CHECK(!ef::parse_trial_state("NOT_A_STATE").has_value());
  EF_CHECK(!ef::parse_trial_state("completed").has_value());
}

EF_TEST(domain, terminal_states_are_final) {
  EF_CHECK(ef::is_terminal(ef::TrialState::COMPLETED));
  EF_CHECK(ef::is_terminal(ef::TrialState::FAILED));
  EF_CHECK(ef::is_terminal(ef::TrialState::CANCELLED));
  EF_CHECK(ef::is_terminal(ef::TrialState::INVALID));
  EF_CHECK(ef::is_terminal(ef::TrialState::SUPERSEDED));
  EF_CHECK(!ef::is_terminal(ef::TrialState::REVALIDATION_REQUIRED));
  EF_CHECK(!ef::is_terminal(ef::TrialState::OBSERVING));
  // A terminal state accepts no transition except the SUPERSEDED audit marker.
  EF_CHECK(!ef::is_valid_transition(ef::TrialState::COMPLETED, ef::TrialState::RUNNING));
  EF_CHECK(!ef::is_valid_transition(ef::TrialState::COMPLETED, ef::TrialState::COMPLETED));
  EF_CHECK(ef::is_valid_transition(ef::TrialState::COMPLETED, ef::TrialState::SUPERSEDED));
  EF_CHECK(!ef::is_valid_transition(ef::TrialState::SUPERSEDED, ef::TrialState::COMPLETED));
}

EF_TEST(domain, trial_lifecycle_transitions_are_explicit) {
  EF_CHECK(ef::is_valid_transition(ef::TrialState::CREATED, ef::TrialState::READY));
  EF_CHECK(ef::is_valid_transition(ef::TrialState::READY, ef::TrialState::ASSIGNED));
  EF_CHECK(ef::is_valid_transition(ef::TrialState::ASSIGNED, ef::TrialState::RUNNING));
  EF_CHECK(ef::is_valid_transition(ef::TrialState::RUNNING, ef::TrialState::OBSERVING));
  EF_CHECK(ef::is_valid_transition(ef::TrialState::OBSERVING, ef::TrialState::COMPLETED));
  EF_CHECK(ef::is_valid_transition(ef::TrialState::ASSIGNED, ef::TrialState::READY));
  EF_CHECK(!ef::is_valid_transition(ef::TrialState::CREATED, ef::TrialState::COMPLETED));
  EF_CHECK(!ef::is_valid_transition(ef::TrialState::READY, ef::TrialState::COMPLETED));
  EF_CHECK(ef::is_valid_transition(ef::TrialState::REVALIDATION_REQUIRED, ef::TrialState::READY));
  EF_CHECK(!ef::is_valid_transition(ef::TrialState::REVALIDATION_REQUIRED, ef::TrialState::COMPLETED));
}

EF_TEST(domain, attempt_and_branch_transitions_are_explicit) {
  EF_CHECK(ef::is_valid_transition(ef::AttemptState::ISSUED, ef::AttemptState::COMPLETED));
  EF_CHECK(!ef::is_valid_transition(ef::AttemptState::COMPLETED, ef::AttemptState::RUNNING));
  EF_CHECK(!ef::is_valid_transition(ef::AttemptState::ABANDONED, ef::AttemptState::COMPLETED));
  EF_CHECK(ef::is_attempt_terminal(ef::AttemptState::ABANDONED));
  EF_CHECK(ef::is_valid_transition(ef::BranchState::ACTIVE, ef::BranchState::RETIRED));
  EF_CHECK(ef::is_valid_transition(ef::BranchState::RETIRED, ef::BranchState::ACTIVE));
  EF_CHECK(!ef::is_valid_transition(ef::BranchState::SUPERSEDED, ef::BranchState::ACTIVE));
  EF_CHECK(ef::is_branch_eligible(ef::BranchState::ACTIVE));
  EF_CHECK(!ef::is_branch_eligible(ef::BranchState::RETIRED));
}

EF_TEST(domain, experiment_transitions_are_explicit) {
  EF_CHECK(ef::is_valid_transition(ef::ExperimentState::OPEN, ef::ExperimentState::FINALIZED));
  EF_CHECK(ef::is_valid_transition(ef::ExperimentState::OPEN, ef::ExperimentState::CANCELLED));
  EF_CHECK(!ef::is_valid_transition(ef::ExperimentState::CANCELLED, ef::ExperimentState::OPEN));
  EF_CHECK(ef::is_experiment_mutable(ef::ExperimentState::OPEN));
  EF_CHECK(!ef::is_experiment_mutable(ef::ExperimentState::FINALIZED));
}

EF_TEST(domain, error_names_round_trip) {
  for (std::uint8_t value = 0; value <= static_cast<std::uint8_t>(ef::ErrorCode::INTERNAL); ++value) {
    const auto code = static_cast<ef::ErrorCode>(value);
    const std::string_view name = ef::to_string(code);
    EF_CHECK(name != std::string_view("UNKNOWN_ERROR"));
    const auto parsed = ef::parse_error_code(name);
    EF_REQUIRE(parsed.has_value());
    EF_CHECK(*parsed == code);
  }
  EF_CHECK(ef::Status(ef::ErrorCode::STALE_EPOCH, ef::ErrorStage::AUTHORITY, "x").is_stale());
  EF_CHECK(!ef::Status(ef::ErrorCode::NOT_FOUND, ef::ErrorStage::LIFECYCLE, "x").is_stale());
}
