// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_ERROR_HPP
#define EXPERIMENT_FABRIC_ERROR_HPP

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "experiment_fabric/export.hpp"

namespace experiment_fabric {

/// ile
/// Typed error and result semantics.
///
/// Raw exceptions are never part of the public governance protocol. Every
/// authoritative operation returns a typed c Status; an invalid authoritative
/// operation is never silently downgraded to a no-op.

enum class ErrorCode : std::uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  NOT_FOUND,
  ALREADY_EXISTS,
  STALE_EPOCH,
  STALE_WORKER,
  STALE_GENERATION,
  STALE_HYPOTHESIS,
  STALE_BRANCH,
  STALE_ATTEMPT,
  UNAUTHORIZED,
  INVALID_TRANSITION,
  CANCELLED,
  INSUFFICIENT_EVIDENCE,
  INVALID_OBSERVATION,
  CORRUPT_PERSISTENCE,
  PROTOCOL_ERROR,
  RESOURCE_EXHAUSTED,
  UNSUPPORTED,
  PERSISTENCE_FAILURE,
  TRANSPORT_FAILURE,
  SHUTTING_DOWN,
  INTERNAL,
};

/// Pipeline stage at which an error was produced.
enum class ErrorStage : std::uint8_t {
  NONE = 0,
  VALIDATION,
  IDENTITY,
  DEFINITION,
  AUTHORITY,
  LIFECYCLE,
  OBSERVATION,
  COMPARISON,
  DECISION,
  PERSISTENCE,
  TRANSPORT,
  RECOVERY,
  SHUTDOWN,
  LIMITS,
};

/// Human readable name of an error code. Stable across versions.
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

/// Human readable name of an error stage. Stable across versions.
[[nodiscard]] std::string_view to_string(ErrorStage stage) noexcept;

/// Parses an error code name. Used by inspection tooling and tests.
[[nodiscard]] std::optional<ErrorCode> parse_error_code(std::string_view name) noexcept;

/// A typed failure with stage and reason.
class EF_API Status {
 public:
  Status() = default;

  Status(ErrorCode code, ErrorStage stage, std::string reason)
      : code_(code), stage_(stage), reason_(std::move(reason)) {}

  /// Returns a successful status. Named \c success so that it cannot collide
  /// with the \c ok predicate.
  [[nodiscard]] static Status success() noexcept { return Status{}; }

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] ErrorStage stage() const noexcept { return stage_; }
  [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::OK; }
  [[nodiscard]] bool failed() const noexcept { return code_ != ErrorCode::OK; }

  /// True when the failure is an authority-fencing rejection.
  [[nodiscard]] bool is_stale() const noexcept;

  /// Deterministic single-line rendering used by the inspection CLI.
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Status& lhs, const Status& rhs) noexcept {
    return lhs.code_ == rhs.code_ && lhs.stage_ == rhs.stage_ && lhs.reason_ == rhs.reason_;
  }

 private:
  ErrorCode code_ = ErrorCode::OK;
  ErrorStage stage_ = ErrorStage::NONE;
  std::string reason_;
};

/// Constructs a failure status.
[[nodiscard]] inline Status make_error(ErrorCode code, ErrorStage stage, std::string reason) {
  return Status(code, stage, std::move(reason));
}

/// Wrapper that lets a \c Status travel as the *value* of a \c Result rather
/// than as its error branch. Needed because \c Result<Status> would collapse
/// the two branches into one type.
struct StatusPayload {
  Status status;
};

/// A value-or-status result. c Result<void> is supported.
template <typename T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Status status) : storage_(std::move(status)) {}   // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Status& status() const noexcept {
    static const Status ok_status{};
    return ok() ? ok_status : std::get<Status>(storage_);
  }

  [[nodiscard]] T& value() & { return std::get<T>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
  [[nodiscard]] T&& value() && { return std::move(std::get<T>(storage_)); }

  [[nodiscard]] T* operator->() { return &std::get<T>(storage_); }
  [[nodiscard]] const T* operator->() const { return &std::get<T>(storage_); }
  [[nodiscard]] T& operator*() { return std::get<T>(storage_); }
  [[nodiscard]] const T& operator*() const { return std::get<T>(storage_); }

 private:
  std::variant<T, Status> storage_;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  Status status_;
};

using StatusResult = Result<StatusPayload>;

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_ERROR_HPP
