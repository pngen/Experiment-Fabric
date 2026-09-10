// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_CLIENT_HPP
#define EXPERIMENT_FABRIC_CLIENT_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "experiment_fabric/error.hpp"
#include "experiment_fabric/limits.hpp"
#include "experiment_fabric/protocol.hpp"
#include "experiment_fabric/transport.hpp"

namespace experiment_fabric {

/// \file
/// Control-plane client.
///
/// The client is a thin, strictly parsing wrapper over the framed protocol.
/// It never retries an authoritative mutation implicitly: a retry is an explicit
/// caller decision because duplicate delivery must remain harmless but visible.

class EF_API Client {
 public:
  struct Config {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    Limits limits{};
    /// Receives the exact bytes of every frame this client sends. Used by the
    /// worker to preserve real pre-restart traffic for stale-authority proofs.
    std::function<void(std::span<const std::uint8_t>)> outbound_observer;
  };

  [[nodiscard]] static Result<std::unique_ptr<Client>> connect(Config config);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  [[nodiscard]] Status close();

  /// Exchanges one request frame for one reply frame of the expected type.
  [[nodiscard]] Result<Frame> request(MessageType type, std::span<const std::uint8_t> payload);

  [[nodiscard]] Result<RegisterWorkerReply> register_worker(const RegisterWorkerRequest& request);
  [[nodiscard]] Result<ClaimTrialReply> claim_trials(const ClaimTrialRequest& request);
  [[nodiscard]] Status publish_observation(const PublishObservationRequest& request);
  [[nodiscard]] Status publish_artifact(const PublishArtifactRequest& request);
  [[nodiscard]] Status commit_trial(const CommitTrialRequest& request);
  [[nodiscard]] Status fail_trial(const FailTrialRequest& request);
  [[nodiscard]] Status heartbeat(const HeartbeatRequest& request);
  [[nodiscard]] Status cancel_trial(const CancelTrialRequest& request);
  [[nodiscard]] Status cancel_experiment(const CancelExperimentRequest& request);
  [[nodiscard]] Result<Decision> finalize_experiment(const FinalizeExperimentRequest& request);
  [[nodiscard]] Status rollback(const RollbackRequest& request);
  [[nodiscard]] Result<std::vector<std::string>> query(QueryKind kind, const std::string& argument);
  [[nodiscard]] Status shutdown();

  /// Sends raw pre-encoded bytes and reads one reply frame. Used only by tests
  /// that replay preserved traffic from a previous process incarnation.
  [[nodiscard]] Result<Frame> send_raw_and_receive(std::span<const std::uint8_t> bytes);
  [[nodiscard]] Status send_raw(std::span<const std::uint8_t> bytes);

  [[nodiscard]] const Limits& limits() const noexcept { return limits_; }
  [[nodiscard]] std::uint64_t next_correlation() noexcept { return ++correlation_; }

 private:
  explicit Client(Config config);

  std::unique_ptr<TcpConnection> connection_;
  Limits limits_{};
  std::uint64_t correlation_ = 0;
  std::function<void(std::span<const std::uint8_t>)> outbound_observer_;
};

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_CLIENT_HPP
