// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_APPS_SCENARIOS_HPP
#define EXPERIMENT_FABRIC_APPS_SCENARIOS_HPP

#include <string>
#include <vector>

#include "experiment_fabric/coordinator.hpp"
#include "experiment_fabric/worker.hpp"

/// \file
/// Reference experiment catalogue.
///
/// The scenarios are deliberately domain-neutral: each one is a small
/// deterministic systems experiment whose numbers are carried as branch
/// parameters, so the same catalogue exercises the governance boundary for
/// compiler-optimization, scheduling, model-selection, inference-configuration,
/// kernel-tuning, agent-strategy and systems-performance experiments alike.
///
/// Every scenario is SYNTHETIC: the workloads are deterministic simulations, not
/// measurements of a physical system.

namespace experiment_fabric::scenarios {

/// Names of the built-in reference experiments.
[[nodiscard]] std::vector<std::string> scenario_names();

/// Names of the built-in reference workloads.
[[nodiscard]] std::vector<std::string> workload_names();

/// Builds one scenario specification. Returns false and fills \c error when the
/// name is unknown.
[[nodiscard]] bool build_scenario(const std::string& name, ExperimentSpec& out, std::string& error);

/// Creates the planned trials of every active branch of an experiment.
[[nodiscard]] Status create_planned_trials(Coordinator& coordinator, ExperimentId experiment, std::string payload);

/// Runs one reference workload. Returns the outcome the worker should publish.
[[nodiscard]] TrialOutcome run_workload(const std::string& name, const TrialAssignment& assignment,
                                        const WorkerTaskContext& context);

// ---------------------------------------------------------------------------
// In-process driver helpers
// ---------------------------------------------------------------------------
//
// These helpers drive the same public governance API the networked worker uses.
// They exist so that the reference examples read as experiments rather than as
// plumbing, and they contain no authority of their own: every call is validated
// by the coordinator exactly as a remote call would be.

/// One registered worker incarnation driven directly against a coordinator.
struct Session {
  WorkerId worker;
  WorkerBootId boot;
  ProducerId producer;
  CoordinatorEpoch epoch;
};

/// Registers a worker incarnation. When \c boot is zero a deterministic
/// identity is derived from \c identity so that examples are reproducible.
[[nodiscard]] Result<Session> join(Coordinator& coordinator, std::uint64_t identity, std::uint64_t boot = 0);

/// Claims one trial. Fails with NOT_FOUND when no work is available.
[[nodiscard]] Result<TrialAssignment> claim(Coordinator& coordinator, const Session& session,
                                            std::uint32_t max_claims = 1);

/// Publishes one metric value under the authority the coordinator issued.
[[nodiscard]] Status publish(Coordinator& coordinator, const Session& session, const TrialAssignment& assignment,
                             std::string_view metric, double value,
                             ObservationValidity validity = ObservationValidity::VALID);

/// Commits the authoritative logical completion of a claimed trial.
[[nodiscard]] Status commit(Coordinator& coordinator, const Session& session, const TrialAssignment& assignment);

/// Claims one trial, publishes one metric value and commits it.
[[nodiscard]] Status complete_one(Coordinator& coordinator, const Session& session, std::string_view metric,
                                  double value, ObservationValidity validity = ObservationValidity::VALID);

}  // namespace experiment_fabric::scenarios

#endif  // EXPERIMENT_FABRIC_APPS_SCENARIOS_HPP
