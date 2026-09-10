// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_PROTOCOL_HPP
#define EXPERIMENT_FABRIC_PROTOCOL_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "experiment_fabric/authority.hpp"
#include "experiment_fabric/domain.hpp"
#include "experiment_fabric/error.hpp"
#include "experiment_fabric/identity.hpp"
#include "experiment_fabric/limits.hpp"
#include "experiment_fabric/model.hpp"
#include "experiment_fabric/state.hpp"
#include "experiment_fabric/version.hpp"

namespace experiment_fabric {

/// \file
/// Strict, bounded, framed wire protocol.
///
/// Framing is fixed-size and self-describing; payloads are a strict binary
/// encoding with a fully consuming parser. Declared lengths are bounds-checked
/// before any allocation and trailing bytes are rejected.

/// Fixed frame header size in bytes.
inline constexpr std::uint32_t kFrameHeaderBytes = 32;

/// Frame magic: ASCII "EFRM" read little-endian.
inline constexpr std::uint32_t kFrameMagic = 0x4D524645u;

/// Message types. The numeric values are part of the wire contract.
enum class MessageType : std::uint16_t {
  INVALID = 0,
  HELLO = 1,
  HELLO_ACK = 2,
  REGISTER_WORKER = 3,
  REGISTER_ACK = 4,
  CLAIM_TRIAL = 5,
  CLAIM_RESULT = 6,
  PUBLISH_OBSERVATION = 7,
  PUBLISH_ARTIFACT = 8,
  COMMIT_TRIAL = 9,
  FAIL_TRIAL = 10,
  HEARTBEAT = 11,
  ACK = 12,
  CREATE_EXPERIMENT = 13,
  CREATE_TRIAL = 14,
  CANCEL_TRIAL = 15,
  CANCEL_EXPERIMENT = 16,
  FINALIZE_EXPERIMENT = 17,
  ROLLBACK = 18,
  QUERY = 19,
  QUERY_RESULT = 20,
  SHUTDOWN = 21,
  ERROR_REPLY = 22,
  FORK_BRANCH = 23,
  CREATE_HYPOTHESIS = 24,
  REVISE_HYPOTHESIS = 25,
};

[[nodiscard]] std::string_view to_string(MessageType type) noexcept;
[[nodiscard]] std::optional<MessageType> parse_message_type(std::string_view name) noexcept;

/// Query kinds handled by the QUERY / QUERY_RESULT exchange.
enum class QueryKind : std::uint16_t {
  INVALID = 0,
  STATUS = 1,
  LIST_EXPERIMENTS = 2,
  EXPERIMENT_SNAPSHOT = 3,
  BRANCH_LINEAGE = 4,
  TRIALS = 5,
  OBSERVATIONS = 6,
  REJECTED_EVIDENCE = 7,
  ARTIFACTS = 8,
  EXPLAIN_DECISION = 9,
  REPRODUCIBILITY = 10,
  VALIDATE_STATE = 11,
  HYPOTHESIS = 12,
  WORKERS = 13,
};

/// One framed message.
struct Frame {
  std::uint16_t version = kProtocolVersion;
  MessageType type = MessageType::INVALID;
  std::uint64_t correlation = 0;
  std::vector<std::uint8_t> payload;
};

/// Encodes a frame into bytes, including header and integrity fields. Fails
/// with RESOURCE_EXHAUSTED when the payload exceeds the configured bound.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_frame(const Frame& frame, const Limits& limits);

/// Validated header fields of one frame.
struct FrameHeaderInfo {
  std::uint16_t version = 0;
  MessageType type = MessageType::INVALID;
  std::uint64_t correlation = 0;
  std::uint32_t payload_length = 0;
  std::uint32_t payload_crc = 0;
  std::uint32_t header_crc = 0;
  std::uint32_t reserved = 0;
};

/// Validates a 32-byte header without allocating. Rejects bad magic, unknown
/// protocol version, unknown message type, non-zero reserved word, oversized
/// declared payload and header checksum mismatch.
[[nodiscard]] Result<FrameHeaderInfo> parse_frame_header(std::span<const std::uint8_t> header,
                                                         const Limits& limits);

/// Encodes a validated header.
[[nodiscard]] std::vector<std::uint8_t> encode_frame_header(const Frame& frame,
                                                            std::span<const std::uint8_t> payload,
                                                            const Limits& limits);

/// Decodes exactly one frame from a buffer. Returns the number of bytes
/// consumed, or a PROTOCOL_ERROR describing precisely why the bytes were
/// refused. Never allocates more than \c limits.max_frame_payload_bytes.
struct FrameDecodeOutcome {
  Status status;
  std::size_t consumed = 0;
  Frame frame;
};

[[nodiscard]] FrameDecodeOutcome decode_frame(std::span<const std::uint8_t> bytes, const Limits& limits);

// ---------------------------------------------------------------------------
// Payload codec
// ---------------------------------------------------------------------------

/// Strict little-endian binary writer with bounded string lengths.
class PayloadWriter {
 public:
  explicit PayloadWriter(const Limits& limits) : limits_(limits) {}

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void boolean(bool value);
  void real(double value);
  void integer(std::int64_t value);
  void digest(const Sha256::Digest& digest);
  void text(std::string_view value, std::uint32_t max_length);
  void raw(std::span<const std::uint8_t> bytes);
  void count(std::uint32_t value, std::uint32_t max_value);

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(bytes_); }

 private:
  const Limits& limits_;
  std::vector<std::uint8_t> bytes_;
};

/// Strict reader that must consume the buffer exactly.
class PayloadReader {
 public:
  PayloadReader(std::span<const std::uint8_t> bytes, const Limits& limits)
      : bytes_(bytes), limits_(limits) {}

  [[nodiscard]] Result<std::uint8_t> u8();
  [[nodiscard]] Result<std::uint16_t> u16();
  [[nodiscard]] Result<std::uint32_t> u32();
  [[nodiscard]] Result<std::uint64_t> u64();
  [[nodiscard]] Result<bool> boolean();
  [[nodiscard]] Result<double> real();
  [[nodiscard]] Result<std::int64_t> integer();
  [[nodiscard]] Result<Sha256::Digest> digest();
  [[nodiscard]] Result<std::string> text(std::uint32_t max_length);
  [[nodiscard]] Result<std::vector<std::uint8_t>> raw(std::uint32_t length);
  [[nodiscard]] Result<std::uint32_t> count(std::uint32_t max_value);

  /// Fails when any bytes remain. Used to reject trailing payload garbage.
  [[nodiscard]] Status expect_end() const;
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }

 private:
  [[nodiscard]] Status need(std::size_t count) const;

  std::span<const std::uint8_t> bytes_;
  const Limits& limits_;
  std::size_t offset_ = 0;
};

// ---------------------------------------------------------------------------
// Typed messages
// ---------------------------------------------------------------------------

/// Worker registration request.
struct RegisterWorkerRequest {
  CoordinatorEpoch claimed_epoch;
  WorkerId worker;
  WorkerBootId boot;
  ProducerId producer;
  std::string endpoint;
  std::uint32_t max_concurrent_assignments = 1;
  std::string fingerprint;
};

struct RegisterWorkerReply {
  CoordinatorEpoch epoch;
  WorkerBootId boot;
  ProducerId producer;
  bool accepted = false;
  Status status;
};

struct ClaimTrialRequest {
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId boot;
  std::uint32_t max_claims = 1;
};

/// One metric reservation inside an assignment.
///
/// Observation identities are allocated by the coordinator, never by the
/// worker: a producer process cannot mint identities in the coordinator's
/// namespace.
struct AssignedMetric {
  MetricId metric;
  ObservationId observation_id;
  std::string name;
  MetricKind kind = MetricKind::REAL;
  MetricDirection direction = MetricDirection::MINIMIZE;
  bool required = true;
};

/// One issued assignment, including its complete authority envelope.
struct TrialAssignment {
  AuthorityEnvelope envelope;
  TrialAttemptNumber attempt_number;
  std::string trial_payload;   ///< opaque producer payload carried through
  std::vector<ParameterAssignment> parameters;
  std::vector<AssignedMetric> metrics;
  std::vector<ArtifactId> artifact_ids;
  std::uint32_t planned_observations = 0;
};

struct ClaimTrialReply {
  Status status;
  std::vector<TrialAssignment> assignments;
};

struct PublishObservationRequest {
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId boot;
  Observation observation;
};

struct PublishArtifactRequest {
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId boot;
  ArtifactRecord artifact;
};

struct CommitTrialRequest {
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId boot;
  AuthorityEnvelope envelope;
  std::vector<ArtifactRecord> artifacts;
  std::string result_payload;
};

struct FailTrialRequest {
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId boot;
  AuthorityEnvelope envelope;
  FailureKind kind = FailureKind::EXECUTION_FAILURE;
  std::string detail;
  bool retryable = false;
};

struct CancelTrialRequest {
  TrialId trial;
  std::string reason;
};

struct CancelExperimentRequest {
  ExperimentId experiment;
  std::string reason;
};

struct FinalizeExperimentRequest {
  ExperimentId experiment;
  BranchId candidate;
  std::string reason;
};

struct RollbackRequest {
  ExperimentId experiment;
  ExperimentGeneration target_generation;
  std::string reason;
};

struct HeartbeatRequest {
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId boot;
  std::uint32_t active_assignments = 0;
};

struct AckReply {
  Status status;
};

struct ErrorReply {
  Status status;
  std::string detail;
};

/// Reply to a QUERY message: a status plus deterministic text lines.
struct QueryResult {
  Status status;
  std::vector<std::string> lines;
};

/// Reply carrying a status and one identity value (created trial, branch, or
/// hypothesis revision).
struct ValueReply {
  Status status;
  std::uint64_t value = 0;
};

/// Creates a logical trial under a branch.
struct CreateTrialRequest {
  ExperimentId experiment;
  BranchId branch;
  std::string payload;
};

/// Forks a branch, preserving lineage without sharing mutable authority.
struct ForkBranchRequest {
  ExperimentId experiment;
  BranchId parent;
  std::string name;
  BranchRole role = BranchRole::CANDIDATE;
  std::vector<ParameterAssignment> parameters;
  std::optional<std::uint64_t> seed;
  std::uint32_t planned_trials = 1;
};

/// Revises the hypothesis of an experiment. Increments the hypothesis revision
/// and the experiment generation.
struct ReviseHypothesisRequest {
  ExperimentId experiment;
  std::string statement;
  bool has_direction = false;
  MetricDirection direction = MetricDirection::MINIMIZE;
  MetricId metric;
  std::string provenance;
};

/// Encoding and decoding of each typed message. These are the only functions
/// permitted to construct payload bytes, so the wire format stays in one place.
[[nodiscard]] std::vector<std::uint8_t> encode(const RegisterWorkerRequest& value, const Limits& limits);
[[nodiscard]] Result<RegisterWorkerRequest> decode_register_worker(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const RegisterWorkerReply& value, const Limits& limits);
[[nodiscard]] Result<RegisterWorkerReply> decode_register_reply(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const ClaimTrialRequest& value, const Limits& limits);
[[nodiscard]] Result<ClaimTrialRequest> decode_claim_trial(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const ClaimTrialReply& value, const Limits& limits);
[[nodiscard]] Result<ClaimTrialReply> decode_claim_reply(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const PublishObservationRequest& value, const Limits& limits);
[[nodiscard]] Result<PublishObservationRequest> decode_publish_observation(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const PublishArtifactRequest& value, const Limits& limits);
[[nodiscard]] Result<PublishArtifactRequest> decode_publish_artifact(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const CommitTrialRequest& value, const Limits& limits);
[[nodiscard]] Result<CommitTrialRequest> decode_commit_trial(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const FailTrialRequest& value, const Limits& limits);
[[nodiscard]] Result<FailTrialRequest> decode_fail_trial(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const CancelTrialRequest& value, const Limits& limits);
[[nodiscard]] Result<CancelTrialRequest> decode_cancel_trial(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const CancelExperimentRequest& value, const Limits& limits);
[[nodiscard]] Result<CancelExperimentRequest> decode_cancel_experiment(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const FinalizeExperimentRequest& value, const Limits& limits);
[[nodiscard]] Result<FinalizeExperimentRequest> decode_finalize_experiment(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const RollbackRequest& value, const Limits& limits);
[[nodiscard]] Result<RollbackRequest> decode_rollback(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const HeartbeatRequest& value, const Limits& limits);
[[nodiscard]] Result<HeartbeatRequest> decode_heartbeat(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const AckReply& value, const Limits& limits);
[[nodiscard]] Result<AckReply> decode_ack(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const ErrorReply& value, const Limits& limits);
[[nodiscard]] Result<ErrorReply> decode_error_reply(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const QueryResult& value, const Limits& limits);
[[nodiscard]] Result<QueryResult> decode_query_result(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const ValueReply& value, const Limits& limits);
[[nodiscard]] Result<ValueReply> decode_value_reply(std::span<const std::uint8_t> payload, const Limits& limits);

/// Reply carrying a status and a complete decision.
[[nodiscard]] std::vector<std::uint8_t> encode_decision_reply(const Status& status, const Decision& decision,
                                                              const Limits& limits);
[[nodiscard]] Result<std::pair<Status, Decision>> decode_decision_reply(std::span<const std::uint8_t> payload,
                                                                       const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const CreateTrialRequest& value, const Limits& limits);
[[nodiscard]] Result<CreateTrialRequest> decode_create_trial(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const ForkBranchRequest& value, const Limits& limits);
[[nodiscard]] Result<ForkBranchRequest> decode_fork_branch(std::span<const std::uint8_t> payload, const Limits& limits);

[[nodiscard]] std::vector<std::uint8_t> encode(const ReviseHypothesisRequest& value, const Limits& limits);
[[nodiscard]] Result<ReviseHypothesisRequest> decode_revise_hypothesis(std::span<const std::uint8_t> payload,
                                                                      const Limits& limits);

/// Encodes a query request: the query kind plus a canonical text argument.
[[nodiscard]] std::vector<std::uint8_t> encode_query_request(QueryKind kind, std::string_view argument,
                                                             const Limits& limits);
[[nodiscard]] Result<std::pair<QueryKind, std::string>> decode_query_request(std::span<const std::uint8_t> payload,
                                                                            const Limits& limits);

/// Decodes a QUERY_RESULT status-and-lines outcome as a result carrying the
/// reply status. Fails when the reply reports a failure.
[[nodiscard]] Result<std::vector<std::string>> unwrap_query_lines(const QueryResult& reply);

[[nodiscard]] std::vector<std::uint8_t> encode(const Status& value, const Limits& limits);
[[nodiscard]] StatusResult decode_status(std::span<const std::uint8_t> payload, const Limits& limits);

/// Authority envelope codec, shared by every message that carries one.
void write_envelope(PayloadWriter& writer, const AuthorityEnvelope& envelope);
[[nodiscard]] Result<AuthorityEnvelope> read_envelope(PayloadReader& reader);

/// Canonical model codecs. Persistence and transport share exactly these
/// encoders so that a decoded object is identical on both paths.
void write_parameters(PayloadWriter& writer, const std::vector<ParameterAssignment>& parameters);
[[nodiscard]] Result<std::vector<ParameterAssignment>> read_parameters(PayloadReader& reader);

void write_metric_value(PayloadWriter& writer, const MetricValue& value);
[[nodiscard]] Result<MetricValue> read_metric_value(PayloadReader& reader);

void write_observation(PayloadWriter& writer, const Observation& observation);
[[nodiscard]] Result<Observation> read_observation(PayloadReader& reader);

void write_artifact(PayloadWriter& writer, const ArtifactRecord& artifact);
[[nodiscard]] Result<ArtifactRecord> read_artifact(PayloadReader& reader);

void write_metric_definition(PayloadWriter& writer, const MetricDefinition& metric);
[[nodiscard]] Result<MetricDefinition> read_metric_definition(PayloadReader& reader);

void write_comparison_rule(PayloadWriter& writer, const ComparisonRule& rule);
[[nodiscard]] Result<ComparisonRule> read_comparison_rule(PayloadReader& reader);

void write_comparison_policy(PayloadWriter& writer, const ComparisonPolicy& policy);
[[nodiscard]] Result<ComparisonPolicy> read_comparison_policy(PayloadReader& reader);

void write_branch_definition(PayloadWriter& writer, const BranchDefinition& branch);
[[nodiscard]] Result<BranchDefinition> read_branch_definition(PayloadReader& reader);

void write_experiment_definition(PayloadWriter& writer, const ExperimentDefinition& definition);
[[nodiscard]] Result<ExperimentDefinition> read_experiment_definition(PayloadReader& reader);

void write_hypothesis(PayloadWriter& writer, const Hypothesis& hypothesis);
[[nodiscard]] Result<Hypothesis> read_hypothesis(PayloadReader& reader);

void write_trial(PayloadWriter& writer, const TrialRecord& trial);
[[nodiscard]] Result<TrialRecord> read_trial(PayloadReader& reader);

void write_attempt(PayloadWriter& writer, const AttemptRecord& attempt);
[[nodiscard]] Result<AttemptRecord> read_attempt(PayloadReader& reader);

void write_decision(PayloadWriter& writer, const Decision& decision);
[[nodiscard]] Result<Decision> read_decision(PayloadReader& reader);

void write_rollback_point(PayloadWriter& writer, const RollbackPoint& point);
[[nodiscard]] Result<RollbackPoint> read_rollback_point(PayloadReader& reader);

void write_rejected_evidence(PayloadWriter& writer, const RejectedEvidence& evidence);
[[nodiscard]] Result<RejectedEvidence> read_rejected_evidence(PayloadReader& reader);

void write_worker_incarnation(PayloadWriter& writer, const WorkerIncarnation& incarnation);
[[nodiscard]] Result<WorkerIncarnation> read_worker_incarnation(PayloadReader& reader);

void write_experiment_record(PayloadWriter& writer, const ExperimentRecord& record);
[[nodiscard]] Result<ExperimentRecord> read_experiment_record(PayloadReader& reader);

void write_coordinator_state(PayloadWriter& writer, const CoordinatorState& state);
[[nodiscard]] Result<CoordinatorState> read_coordinator_state(PayloadReader& reader);

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_PROTOCOL_HPP
