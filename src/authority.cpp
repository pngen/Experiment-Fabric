// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/authority.hpp"

namespace experiment_fabric {

AuthorityVerdict validate_envelope_structure(const AuthorityEnvelope& envelope) {
  if (!envelope.epoch.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null coordinator epoch");
  }
  if (!envelope.experiment.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null experiment identity");
  }
  if (!envelope.generation.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null experiment generation");
  }
  if (!envelope.hypothesis.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null hypothesis identity");
  }
  if (!envelope.hypothesis_revision.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null hypothesis revision");
  }
  if (!envelope.policy.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null policy identity");
  }
  if (!envelope.policy_generation.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null policy generation");
  }
  if (!envelope.branch.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null branch identity");
  }
  if (!envelope.branch_generation.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null branch generation");
  }
  if (!envelope.trial.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null trial identity");
  }
  if (!envelope.attempt.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null attempt identity");
  }
  if (!envelope.worker.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null worker identity");
  }
  if (!envelope.boot.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null worker boot identity");
  }
  if (!envelope.producer.valid()) {
    return AuthorityVerdict::reject(ErrorCode::INVALID_ARGUMENT, ErrorStage::AUTHORITY,
                                    "envelope carries a null producer identity");
  }
  return AuthorityVerdict::accept();
}

}  // namespace experiment_fabric
