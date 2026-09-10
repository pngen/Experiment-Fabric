// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_AUTHORITY_HPP
#define EXPERIMENT_FABRIC_AUTHORITY_HPP

#include <cstdint>
#include <string>

#include "experiment_fabric/domain.hpp"
#include "experiment_fabric/error.hpp"
#include "experiment_fabric/identity.hpp"

namespace experiment_fabric {

/// \file
/// The authority envelope.
///
/// Evidence is never accepted merely because every identifier parses. A
/// publication is accepted only when the complete envelope matches the live
/// authority of the coordinator that receives it, and only while that
/// coordinator is still admitting work.

/// Everything a producer must present before its evidence may become
/// authoritative. Producers never mint authority: they present the envelope
/// they were issued and the coordinator re-validates every field against
/// current state.
struct AuthorityEnvelope {
  CoordinatorEpoch epoch;
  ExperimentId experiment;
  ExperimentGeneration generation;
  HypothesisId hypothesis;
  HypothesisRevision hypothesis_revision;
  PolicyId policy;
  PolicyGeneration policy_generation;
  BranchId branch;
  BranchGeneration branch_generation;
  TrialId trial;
  TrialAttemptId attempt;
  WorkerId worker;
  WorkerBootId boot;
  ProducerId producer;

  friend bool operator==(const AuthorityEnvelope&, const AuthorityEnvelope&) = default;
};

/// Result of envelope validation. Carries the precise rejection reason so that
/// a producer learns *why* its evidence was refused, and so that tests can
/// assert the exact fencing rule that fired.
struct AuthorityVerdict {
  bool accepted = false;
  ErrorCode code = ErrorCode::OK;
  ErrorStage stage = ErrorStage::NONE;
  std::string detail;

  [[nodiscard]] static AuthorityVerdict accept() noexcept { return AuthorityVerdict{true, ErrorCode::OK, ErrorStage::NONE, {}}; }

  [[nodiscard]] static AuthorityVerdict reject(ErrorCode code, ErrorStage stage, std::string detail) {
    return AuthorityVerdict{false, code, stage, std::move(detail)};
  }
};

/// Structural checks that do not require coordinator state.
///
/// Every field of the envelope must be a non-null identity. A structurally
/// invalid envelope is rejected with INVALID_ARGUMENT before any state lookup,
/// so null identities can never collide into valid authority.
[[nodiscard]] AuthorityVerdict validate_envelope_structure(const AuthorityEnvelope& envelope);

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_AUTHORITY_HPP
