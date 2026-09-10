// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_INSPECT_HPP
#define EXPERIMENT_FABRIC_INSPECT_HPP

#include <string>
#include <vector>

#include "experiment_fabric/coordinator.hpp"
#include "experiment_fabric/limits.hpp"
#include "experiment_fabric/model.hpp"

namespace experiment_fabric {

/// \file
/// Deterministic rendering of authoritative state.
///
/// Every renderer produces a stable line order derived from identity values and
/// explicit priority keys. No renderer iterates an unordered container.

namespace inspect {

[[nodiscard]] std::string_view provenance_label(Provenance provenance) noexcept;

/// Renders a complete experiment snapshot.
[[nodiscard]] std::vector<std::string> render_snapshot(const ExperimentSnapshot& snapshot, const Limits& limits);

/// Renders the hypothesis and its revision history.
[[nodiscard]] std::vector<std::string> render_hypothesis(const ExperimentSnapshot& snapshot, const Limits& limits);

/// Renders branches with their lineage and evidence summary.
[[nodiscard]] std::vector<std::string> render_branches(const ExperimentSnapshot& snapshot, const Limits& limits);

/// Renders the branch parentage tree in deterministic pre-order.
[[nodiscard]] std::vector<std::string> render_lineage(const ExperimentSnapshot& snapshot, const Limits& limits);

/// Renders trials with their attempt history.
[[nodiscard]] std::vector<std::string> render_trials(const ExperimentSnapshot& snapshot, const Limits& limits);

/// Renders metric observations in canonical order.
[[nodiscard]] std::vector<std::string> render_observations(const ExperimentSnapshot& snapshot, const Limits& limits);

/// Renders stale, invalid, missing and rejected evidence.
[[nodiscard]] std::vector<std::string> render_rejected_evidence(const ExperimentSnapshot& snapshot, const Limits& limits);

/// Renders artifact references.
[[nodiscard]] std::vector<std::string> render_artifacts(const ExperimentSnapshot& snapshot, const Limits& limits);

/// Renders one decision and its complete explanation.
[[nodiscard]] std::vector<std::string> render_decision(const Decision& decision, const Limits& limits);

/// Renders the reproducibility record.
[[nodiscard]] std::vector<std::string> render_reproducibility(const ReproducibilityRecord& record, const Limits& limits);

/// Renders one branch aggregate view.
[[nodiscard]] std::vector<std::string> render_evidence(const BranchEvidence& evidence, const Limits& limits);

/// Renders a single line summary of an experiment.
[[nodiscard]] std::string summarize(const ExperimentSnapshot& snapshot);

/// Formats a digest as lowercase hex.
[[nodiscard]] std::string digest_hex(const Sha256::Digest& digest);

/// Formats a double deterministically: fixed precision, no locale dependence.
[[nodiscard]] std::string format_real(double value);

}  // namespace inspect
}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_INSPECT_HPP
