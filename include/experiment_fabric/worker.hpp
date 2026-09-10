// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_WORKER_HPP
#define EXPERIMENT_FABRIC_WORKER_HPP

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "experiment_fabric/client.hpp"
#include "experiment_fabric/domain.hpp"
#include "experiment_fabric/error.hpp"
#include "experiment_fabric/limits.hpp"
#include "experiment_fabric/model.hpp"
#include "experiment_fabric/protocol.hpp"

namespace experiment_fabric {

/// \file
/// Experiment worker.
///
/// A worker owns no authority. It presents the envelope it was issued and the
/// coordinator re-validates every field. A worker process that restarts
/// receives a fresh WorkerBootId and can never inherit authority held by its
/// previous incarnation.

/// A metric value produced by a task, addressed by metric name. The worker
/// resolves the name to the coordinator-reserved identity.
struct NamedObservation {
  std::string metric;
  ObservationValidity validity = ObservationValidity::VALID;
  MetricValue value;
  std::string detail;
};

/// An artifact reference produced by a task.
struct NamedArtifact {
  std::string category;
  std::string locator;
  Sha256::Digest content_digest{};
  bool digest_present = false;
  bool size_present = false;
  std::uint64_t size_bytes = 0;
  std::string provenance;
};

/// What a worker task decided about one assignment.
struct TrialOutcome {
  bool succeeded = false;
  FailureKind failure_kind = FailureKind::EXECUTION_FAILURE;
  std::string failure_detail;
  bool retryable = false;
  std::vector<NamedObservation> observations;
  std::vector<NamedArtifact> artifacts;
  std::string result_payload;
};

/// Context handed to a worker task.
struct WorkerTaskContext {
  WorkerId worker;
  WorkerBootId boot;
  CoordinatorEpoch epoch;
  ProducerId producer;
  TrialAttemptNumber attempt;
  std::vector<ParameterAssignment> parameters;

  /// Returns the value of a named assignment parameter, if present.
  [[nodiscard]] const std::string* parameter(std::string_view key) const noexcept;
};

/// A worker that claims, executes and publishes experiments against a
/// coordinator over the framed TCP control plane.
class EF_API Worker {
 public:
  struct Config {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    WorkerId worker;
    /// Absent means "mint a fresh boot identity for this process".
    std::optional<WorkerBootId> forced_boot;
    std::string endpoint;
    std::string fingerprint;
    Limits limits{};
    std::uint32_t max_concurrent_assignments = 1;
    /// Optional path that receives every frame this process sends, framed and
    /// length-prefixed, so a restarted incarnation can replay real prior
    /// traffic when proving stale-authority fencing.
    std::filesystem::path frame_log_path;
    /// Invoked after each successful observation publication with the running
    /// counts of published observations and committed trials. Used by the
    /// process-death proof to terminate a worker at an exact lifecycle point.
    std::function<void(std::uint64_t observations, std::uint32_t trials)> progress_hook;
  };

  using TaskFunction = std::function<TrialOutcome(const TrialAssignment&, const WorkerTaskContext&)>;

  [[nodiscard]] static Result<std::unique_ptr<Worker>> create(Config config);
  ~Worker();
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  /// Connects and registers. Fails when the coordinator refuses registration.
  [[nodiscard]] Status connect_and_register();

  /// Runs one claim/execute/publish cycle. Returns NOT_FOUND when no work was
  /// available.
  [[nodiscard]] Status run_once(const TaskFunction& task);

  /// Runs cycles until \c stop is set, no work remains, or an unrecoverable
  /// transport error occurs.
  [[nodiscard]] Status run_until_idle(const TaskFunction& task, const std::atomic<bool>& stop);

  [[nodiscard]] WorkerBootId boot() const noexcept;
  [[nodiscard]] CoordinatorEpoch epoch() const noexcept;
  [[nodiscard]] ProducerId producer() const noexcept;
  [[nodiscard]] std::uint32_t published_trials() const noexcept;
  [[nodiscard]] std::uint32_t failed_trials() const noexcept;
  [[nodiscard]] const std::string& last_error() const noexcept;
  [[nodiscard]] const std::filesystem::path& frame_log_path() const noexcept;

  /// Sends preserved raw bytes and returns the reply frame. Used by tests that
  /// replay traffic captured from a previous process incarnation.
  [[nodiscard]] Result<Frame> replay_raw(std::span<const std::uint8_t> bytes);
  [[nodiscard]] Status send_raw(std::span<const std::uint8_t> bytes);

  [[nodiscard]] Client& client();

 private:
  explicit Worker(Config config);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_WORKER_HPP
