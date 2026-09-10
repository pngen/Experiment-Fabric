// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/error.hpp"

#include "experiment_fabric/version.hpp"

namespace experiment_fabric {
namespace {

struct ErrorCodeName {
  ErrorCode code;
  std::string_view name;
};

constexpr ErrorCodeName kErrorCodeNames[] = {
    {ErrorCode::OK, "OK"},
    {ErrorCode::INVALID_ARGUMENT, "INVALID_ARGUMENT"},
    {ErrorCode::NOT_FOUND, "NOT_FOUND"},
    {ErrorCode::ALREADY_EXISTS, "ALREADY_EXISTS"},
    {ErrorCode::STALE_EPOCH, "STALE_EPOCH"},
    {ErrorCode::STALE_WORKER, "STALE_WORKER"},
    {ErrorCode::STALE_GENERATION, "STALE_GENERATION"},
    {ErrorCode::STALE_HYPOTHESIS, "STALE_HYPOTHESIS"},
    {ErrorCode::STALE_BRANCH, "STALE_BRANCH"},
    {ErrorCode::STALE_ATTEMPT, "STALE_ATTEMPT"},
    {ErrorCode::UNAUTHORIZED, "UNAUTHORIZED"},
    {ErrorCode::INVALID_TRANSITION, "INVALID_TRANSITION"},
    {ErrorCode::CANCELLED, "CANCELLED"},
    {ErrorCode::INSUFFICIENT_EVIDENCE, "INSUFFICIENT_EVIDENCE"},
    {ErrorCode::INVALID_OBSERVATION, "INVALID_OBSERVATION"},
    {ErrorCode::CORRUPT_PERSISTENCE, "CORRUPT_PERSISTENCE"},
    {ErrorCode::PROTOCOL_ERROR, "PROTOCOL_ERROR"},
    {ErrorCode::RESOURCE_EXHAUSTED, "RESOURCE_EXHAUSTED"},
    {ErrorCode::UNSUPPORTED, "UNSUPPORTED"},
    {ErrorCode::PERSISTENCE_FAILURE, "PERSISTENCE_FAILURE"},
    {ErrorCode::TRANSPORT_FAILURE, "TRANSPORT_FAILURE"},
    {ErrorCode::SHUTTING_DOWN, "SHUTTING_DOWN"},
    {ErrorCode::INTERNAL, "INTERNAL"},
};

struct ErrorStageName {
  ErrorStage stage;
  std::string_view name;
};

constexpr ErrorStageName kErrorStageNames[] = {
    {ErrorStage::NONE, "NONE"},
    {ErrorStage::VALIDATION, "VALIDATION"},
    {ErrorStage::IDENTITY, "IDENTITY"},
    {ErrorStage::DEFINITION, "DEFINITION"},
    {ErrorStage::AUTHORITY, "AUTHORITY"},
    {ErrorStage::LIFECYCLE, "LIFECYCLE"},
    {ErrorStage::OBSERVATION, "OBSERVATION"},
    {ErrorStage::COMPARISON, "COMPARISON"},
    {ErrorStage::DECISION, "DECISION"},
    {ErrorStage::PERSISTENCE, "PERSISTENCE"},
    {ErrorStage::TRANSPORT, "TRANSPORT"},
    {ErrorStage::RECOVERY, "RECOVERY"},
    {ErrorStage::SHUTDOWN, "SHUTDOWN"},
    {ErrorStage::LIMITS, "LIMITS"},
};

}  // namespace

std::string_view version_string() noexcept { return "1.0.0"; }

std::string_view to_string(ErrorCode code) noexcept {
  for (const ErrorCodeName& entry : kErrorCodeNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "UNKNOWN_ERROR";
}

std::string_view to_string(ErrorStage stage) noexcept {
  for (const ErrorStageName& entry : kErrorStageNames) {
    if (entry.stage == stage) {
      return entry.name;
    }
  }
  return "UNKNOWN_STAGE";
}

std::optional<ErrorCode> parse_error_code(std::string_view name) noexcept {
  for (const ErrorCodeName& entry : kErrorCodeNames) {
    if (entry.name == name) {
      return entry.code;
    }
  }
  return std::nullopt;
}

bool Status::is_stale() const noexcept {
  switch (code_) {
    case ErrorCode::STALE_EPOCH:
    case ErrorCode::STALE_WORKER:
    case ErrorCode::STALE_GENERATION:
    case ErrorCode::STALE_HYPOTHESIS:
    case ErrorCode::STALE_BRANCH:
    case ErrorCode::STALE_ATTEMPT:
    case ErrorCode::UNAUTHORIZED:
      return true;
    default:
      return false;
  }
}

std::string Status::to_string() const {
  std::string result;
  result.append(experiment_fabric::to_string(code_));
  result.append(" stage=");
  result.append(experiment_fabric::to_string(stage_));
  if (!reason_.empty()) {
    result.append(" reason=");
    result.append(reason_);
  }
  return result;
}

}  // namespace experiment_fabric
