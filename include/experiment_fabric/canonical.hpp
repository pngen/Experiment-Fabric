// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_CANONICAL_HPP
#define EXPERIMENT_FABRIC_CANONICAL_HPP

#include <string>
#include <vector>

#include "experiment_fabric/hash.hpp"
#include "experiment_fabric/model.hpp"

namespace experiment_fabric {

/// \file
/// Canonical encodings.
///
/// Two logically equal objects must produce identical bytes, so every digest is
/// computed over a normalised encoding: sorted collections, explicit lengths,
/// and no platform-dependent representation. Digests are the basis of
/// reproducibility evidence and of the deterministic decision record.

namespace canonical {

/// Appends a canonical encoding of a value to a hasher.
void write(Sha256& hasher, std::string_view tag, const ParameterAssignment& value);
void write(Sha256& hasher, std::string_view tag, const MetricDefinition& value);
void write(Sha256& hasher, std::string_view tag, const ComparisonRule& value);
void write(Sha256& hasher, std::string_view tag, const ComparisonPolicy& value);
void write(Sha256& hasher, std::string_view tag, const BranchDefinition& value);
void write(Sha256& hasher, std::string_view tag, const BranchEvidence& value);
void write(Sha256& hasher, std::string_view tag, const Observation& value);
void write(Sha256& hasher, std::string_view tag, const TrialRecord& value);
void write(Sha256& hasher, std::string_view tag, const Decision& value);

/// Digest of a complete experiment definition, including the comparison policy.
[[nodiscard]] Sha256::Digest definition_digest(const ExperimentDefinition& definition);

/// Digest of one branch definition.
[[nodiscard]] Sha256::Digest branch_digest(const BranchDefinition& branch);

/// Digest of a comparison policy.
[[nodiscard]] Sha256::Digest policy_digest(const ComparisonPolicy& policy);

/// Digest of a hypothesis revision.
[[nodiscard]] Sha256::Digest hypothesis_digest(const Hypothesis& hypothesis);

/// Canonical digest over branch evidence, used as the decision state digest.
[[nodiscard]] Sha256::Digest evidence_digest(const std::vector<BranchEvidence>& evidence,
                                             const ExperimentDefinition& definition);

/// Normalises a parameter list: sorted by key, duplicates rejected.
[[nodiscard]] std::vector<ParameterAssignment> normalize_parameters(
    const std::vector<ParameterAssignment>& parameters);

/// True when the parameter list contains a duplicate key.
[[nodiscard]] bool has_duplicate_parameter_key(const std::vector<ParameterAssignment>& parameters);

/// Encodes a double in a canonical, locale-independent, total-order form.
[[nodiscard]] std::string encode_real(double value);

}  // namespace canonical
}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_CANONICAL_HPP
