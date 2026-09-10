// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/protocol.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace experiment_fabric {
namespace {

constexpr std::uint8_t kMaxTrialState = static_cast<std::uint8_t>(TrialState::REVALIDATION_REQUIRED);
constexpr std::uint8_t kMaxExperimentState = static_cast<std::uint8_t>(ExperimentState::RECOVERING);
constexpr std::uint8_t kMaxBranchState = static_cast<std::uint8_t>(BranchState::SUPERSEDED);
constexpr std::uint8_t kMaxAttemptState = static_cast<std::uint8_t>(AttemptState::REVALIDATION_REQUIRED);
constexpr std::uint8_t kMaxBranchRole = static_cast<std::uint8_t>(BranchRole::CANDIDATE);
constexpr std::uint8_t kMaxMetricKind = static_cast<std::uint8_t>(MetricKind::CATEGORICAL);
constexpr std::uint8_t kMaxMetricDirection = static_cast<std::uint8_t>(MetricDirection::INFORMATIONAL);
constexpr std::uint8_t kMaxAggregationRule = static_cast<std::uint8_t>(AggregationRule::MAJORITY);
constexpr std::uint8_t kMaxObservationValidity = static_cast<std::uint8_t>(ObservationValidity::UNSUPPORTED);
constexpr std::uint8_t kMaxComparisonOp = static_cast<std::uint8_t>(ComparisonOp::REPORT_ONLY);
constexpr std::uint8_t kMaxDecisionOutcome = static_cast<std::uint8_t>(DecisionOutcome::SUPERSEDED);
constexpr std::uint8_t kMaxFailureKind = static_cast<std::uint8_t>(FailureKind::LIMIT_EXCEEDED);
constexpr std::uint8_t kMaxReproducibilityStatus = static_cast<std::uint8_t>(ReproducibilityStatus::UNKNOWN);
constexpr std::uint8_t kMaxSeedPolicy = static_cast<std::uint8_t>(SeedPolicy::NONE);
constexpr std::uint8_t kMaxRetryPolicy = static_cast<std::uint8_t>(RetryPolicy::NO_RETRY);
constexpr std::uint8_t kMaxPartialFailurePolicy = static_cast<std::uint8_t>(PartialFailurePolicy::RETRY_ALLOWED);
constexpr std::uint8_t kMaxLateEvidencePolicy = static_cast<std::uint8_t>(LateEvidencePolicy::RETAIN_HISTORICAL);
constexpr std::uint8_t kMaxProvenance = static_cast<std::uint8_t>(Provenance::UNSUPPORTED);
constexpr std::uint8_t kMaxParameterKind = static_cast<std::uint8_t>(ParameterKind::BOOLEAN);
constexpr std::uint16_t kMaxMessageType = static_cast<std::uint16_t>(MessageType::REVISE_HYPOTHESIS);

Status protocol_error(std::string reason) {
  return make_error(ErrorCode::PROTOCOL_ERROR, ErrorStage::VALIDATION, std::move(reason));
}

template <typename Enum>
void write_enum(PayloadWriter& writer, Enum value) {
  writer.u8(static_cast<std::uint8_t>(value));
}

Result<std::uint8_t> read_enum_byte(PayloadReader& reader, std::uint8_t max_value, std::string_view what) {
  auto raw = reader.u8();
  if (!raw.ok()) {
    return raw.status();
  }
  if (raw.value() > max_value) {
    std::string reason = "impossible ";
    reason.append(what);
    reason.append(" value");
    return protocol_error(std::move(reason));
  }
  return raw.value();
}

template <typename Enum>
Result<Enum> read_enum(PayloadReader& reader, std::uint8_t max_value, std::string_view what) {
  auto raw = read_enum_byte(reader, max_value, what);
  if (!raw.ok()) {
    return raw.status();
  }
  return static_cast<Enum>(raw.value());
}

void write_id(PayloadWriter& writer, const auto& id) { writer.u64(id.value()); }

template <typename Id>
Result<Id> read_id(PayloadReader& reader) {
  auto raw = reader.u64();
  if (!raw.ok()) {
    return raw.status();
  }
  return Id::from_value(raw.value());
}

void write_optional_u64(PayloadWriter& writer, const std::optional<std::uint64_t>& value) {
  writer.boolean(value.has_value());
  if (value.has_value()) {
    writer.u64(*value);
  }
}

Result<std::optional<std::uint64_t>> read_optional_u64(PayloadReader& reader) {
  auto present = reader.boolean();
  if (!present.ok()) {
    return present.status();
  }
  if (!present.value()) {
    return std::optional<std::uint64_t>{};
  }
  auto value = reader.u64();
  if (!value.ok()) {
    return value.status();
  }
  return std::optional<std::uint64_t>{value.value()};
}

void write_optional_real(PayloadWriter& writer, const std::optional<double>& value) {
  writer.boolean(value.has_value());
  if (value.has_value()) {
    writer.real(*value);
  }
}

Result<std::optional<double>> read_optional_real(PayloadReader& reader) {
  auto present = reader.boolean();
  if (!present.ok()) {
    return present.status();
  }
  if (!present.value()) {
    return std::optional<double>{};
  }
  auto value = reader.real();
  if (!value.ok()) {
    return value.status();
  }
  return std::optional<double>{value.value()};
}

void write_digest(PayloadWriter& writer, const Sha256::Digest& digest) { writer.digest(digest); }

Result<Sha256::Digest> read_digest(PayloadReader& reader) { return reader.digest(); }

template <typename T, typename Fn>
void write_vector(PayloadWriter& writer, const std::vector<T>& values, std::uint32_t max_items, Fn fn) {
  writer.count(static_cast<std::uint32_t>(values.size()), max_items);
  for (const T& value : values) {
    fn(value);
  }
}

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::INVALID: return "INVALID";
    case MessageType::HELLO: return "HELLO";
    case MessageType::HELLO_ACK: return "HELLO_ACK";
    case MessageType::REGISTER_WORKER: return "REGISTER_WORKER";
    case MessageType::REGISTER_ACK: return "REGISTER_ACK";
    case MessageType::CLAIM_TRIAL: return "CLAIM_TRIAL";
    case MessageType::CLAIM_RESULT: return "CLAIM_RESULT";
    case MessageType::PUBLISH_OBSERVATION: return "PUBLISH_OBSERVATION";
    case MessageType::PUBLISH_ARTIFACT: return "PUBLISH_ARTIFACT";
    case MessageType::COMMIT_TRIAL: return "COMMIT_TRIAL";
    case MessageType::FAIL_TRIAL: return "FAIL_TRIAL";
    case MessageType::HEARTBEAT: return "HEARTBEAT";
    case MessageType::ACK: return "ACK";
    case MessageType::CREATE_EXPERIMENT: return "CREATE_EXPERIMENT";
    case MessageType::CREATE_TRIAL: return "CREATE_TRIAL";
    case MessageType::CANCEL_TRIAL: return "CANCEL_TRIAL";
    case MessageType::CANCEL_EXPERIMENT: return "CANCEL_EXPERIMENT";
    case MessageType::FINALIZE_EXPERIMENT: return "FINALIZE_EXPERIMENT";
    case MessageType::ROLLBACK: return "ROLLBACK";
    case MessageType::QUERY: return "QUERY";
    case MessageType::QUERY_RESULT: return "QUERY_RESULT";
    case MessageType::SHUTDOWN: return "SHUTDOWN";
    case MessageType::ERROR_REPLY: return "ERROR_REPLY";
    case MessageType::FORK_BRANCH: return "FORK_BRANCH";
    case MessageType::CREATE_HYPOTHESIS: return "CREATE_HYPOTHESIS";
    case MessageType::REVISE_HYPOTHESIS: return "REVISE_HYPOTHESIS";
  }
  return "UNKNOWN";
}

std::optional<MessageType> parse_message_type(std::string_view name) noexcept {
  for (std::uint16_t value = 0; value <= kMaxMessageType; ++value) {
    const auto type = static_cast<MessageType>(value);
    if (to_string(type) == name) {
      return type;
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// PayloadWriter
// ---------------------------------------------------------------------------

void PayloadWriter::u8(std::uint8_t value) { bytes_.push_back(value); }

void PayloadWriter::u16(std::uint16_t value) {
  bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void PayloadWriter::u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void PayloadWriter::u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void PayloadWriter::boolean(bool value) { u8(value ? 1u : 0u); }

void PayloadWriter::real(double value) {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  u64(bits);
}

void PayloadWriter::integer(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void PayloadWriter::digest(const Sha256::Digest& digest) { raw(digest); }

void PayloadWriter::text(std::string_view value, std::uint32_t max_length) {
  const std::uint32_t length = static_cast<std::uint32_t>(std::min<std::size_t>(value.size(), max_length));
  u32(length);
  bytes_.insert(bytes_.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length));
}

void PayloadWriter::raw(std::span<const std::uint8_t> bytes) {
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

void PayloadWriter::count(std::uint32_t value, std::uint32_t max_value) {
  (void)max_value;
  u32(value);
}

// ---------------------------------------------------------------------------
// PayloadReader
// ---------------------------------------------------------------------------

Status PayloadReader::need(std::size_t count) const {
  if (count > bytes_.size() - offset_) {
    return protocol_error("payload truncated");
  }
  return Status::success();
}

Result<std::uint8_t> PayloadReader::u8() {
  const Status ready = need(1);
  if (!ready.ok()) {
    return ready;
  }
  return bytes_[offset_++];
}

Result<std::uint16_t> PayloadReader::u16() {
  const Status ready = need(2);
  if (!ready.ok()) {
    return ready;
  }
  const std::uint16_t value = static_cast<std::uint16_t>(bytes_[offset_]) |
                              static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes_[offset_ + 1]) << 8);
  offset_ += 2;
  return value;
}

Result<std::uint32_t> PayloadReader::u32() {
  const Status ready = need(4);
  if (!ready.ok()) {
    return ready;
  }
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes_[offset_ + static_cast<std::size_t>(index)]) << (8 * index);
  }
  offset_ += 4;
  return value;
}

Result<std::uint64_t> PayloadReader::u64() {
  const Status ready = need(8);
  if (!ready.ok()) {
    return ready;
  }
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes_[offset_ + static_cast<std::size_t>(index)]) << (8 * index);
  }
  offset_ += 8;
  return value;
}

Result<bool> PayloadReader::boolean() {
  auto raw = u8();
  if (!raw.ok()) {
    return raw.status();
  }
  if (raw.value() > 1u) {
    return protocol_error("boolean out of range");
  }
  return raw.value() != 0u;
}

Result<double> PayloadReader::real() {
  auto raw = u64();
  if (!raw.ok()) {
    return raw.status();
  }
  double value = 0.0;
  std::memcpy(&value, &raw.value(), sizeof(value));
  return value;
}

Result<std::int64_t> PayloadReader::integer() {
  auto raw = u64();
  if (!raw.ok()) {
    return raw.status();
  }
  return static_cast<std::int64_t>(raw.value());
}

Result<Sha256::Digest> PayloadReader::digest() {
  const Status ready = need(Sha256::kDigestBytes);
  if (!ready.ok()) {
    return ready;
  }
  Sha256::Digest value{};
  std::memcpy(value.data(), bytes_.data() + offset_, value.size());
  offset_ += value.size();
  return value;
}

Result<std::string> PayloadReader::text(std::uint32_t max_length) {
  auto length = u32();
  if (!length.ok()) {
    return length.status();
  }
  if (length.value() > max_length || length.value() > limits_.max_persisted_string_bytes) {
    return protocol_error("declared string length exceeds bound");
  }
  const Status ready = need(length.value());
  if (!ready.ok()) {
    return ready;
  }
  std::string value(reinterpret_cast<const char*>(bytes_.data() + offset_), length.value());
  offset_ += length.value();
  return value;
}

Result<std::vector<std::uint8_t>> PayloadReader::raw(std::uint32_t length) {
  const Status ready = need(length);
  if (!ready.ok()) {
    return ready;
  }
  std::vector<std::uint8_t> value(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                                  bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
  offset_ += length;
  return value;
}

Result<std::uint32_t> PayloadReader::count(std::uint32_t max_value) {
  auto value = u32();
  if (!value.ok()) {
    return value.status();
  }
  if (value.value() > max_value) {
    return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS, "declared count exceeds bound");
  }
  return value.value();
}

Status PayloadReader::expect_end() const {
  if (offset_ != bytes_.size()) {
    return protocol_error("trailing payload bytes");
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

Result<FrameHeaderInfo> parse_frame_header(std::span<const std::uint8_t> header, const Limits& limits) {
  if (header.size() != kFrameHeaderBytes) {
    return protocol_error("frame header size mismatch");
  }
  const std::uint32_t stored_header_crc = crc32(header.first(24));
  std::uint32_t declared_crc = 0;
  for (int index = 0; index < 4; ++index) {
    declared_crc |= static_cast<std::uint32_t>(header[24 + static_cast<std::size_t>(index)]) << (8 * index);
  }
  if (stored_header_crc != declared_crc) {
    return protocol_error("frame header checksum mismatch");
  }
  std::uint32_t magic = 0;
  for (int index = 0; index < 4; ++index) {
    magic |= static_cast<std::uint32_t>(header[static_cast<std::size_t>(index)]) << (8 * index);
  }
  if (magic != kFrameMagic) {
    return protocol_error("frame magic mismatch");
  }
  const std::uint16_t version = static_cast<std::uint16_t>(header[4]) |
                                static_cast<std::uint16_t>(static_cast<std::uint16_t>(header[5]) << 8);
  if (version != kProtocolVersion) {
    return protocol_error("unsupported protocol version");
  }
  const std::uint16_t raw_type = static_cast<std::uint16_t>(header[6]) |
                                 static_cast<std::uint16_t>(static_cast<std::uint16_t>(header[7]) << 8);
  if (raw_type == 0 || raw_type > kMaxMessageType) {
    return protocol_error("unknown message type");
  }
  std::uint64_t correlation = 0;
  for (int index = 0; index < 8; ++index) {
    correlation |= static_cast<std::uint64_t>(header[8 + static_cast<std::size_t>(index)]) << (8 * index);
  }
  std::uint32_t payload_length = 0;
  for (int index = 0; index < 4; ++index) {
    payload_length |= static_cast<std::uint32_t>(header[16 + static_cast<std::size_t>(index)]) << (8 * index);
  }
  std::uint32_t payload_crc = 0;
  for (int index = 0; index < 4; ++index) {
    payload_crc |= static_cast<std::uint32_t>(header[20 + static_cast<std::size_t>(index)]) << (8 * index);
  }
  std::uint32_t reserved = 0;
  for (int index = 0; index < 4; ++index) {
    reserved |= static_cast<std::uint32_t>(header[28 + static_cast<std::size_t>(index)]) << (8 * index);
  }
  if (reserved != 0) {
    return protocol_error("reserved header word must be zero");
  }
  if (payload_length > limits.max_frame_payload_bytes) {
    return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS, "frame payload exceeds bound");
  }

  FrameHeaderInfo info;
  info.version = version;
  info.type = static_cast<MessageType>(raw_type);
  info.correlation = correlation;
  info.payload_length = payload_length;
  info.payload_crc = payload_crc;
  info.header_crc = stored_header_crc;
  info.reserved = reserved;
  return info;
}

std::vector<std::uint8_t> encode_frame_header(const Frame& frame, std::span<const std::uint8_t> payload,
                                              const Limits& limits) {
  (void)limits;
  std::vector<std::uint8_t> header(kFrameHeaderBytes, 0);
  const auto store_u16 = [&header](std::size_t offset, std::uint16_t value) {
    header[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    header[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  };
  const auto store_u32 = [&header](std::size_t offset, std::uint32_t value) {
    for (int index = 0; index < 4; ++index) {
      header[offset + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
    }
  };
  store_u32(0, kFrameMagic);
  store_u16(4, frame.version);
  store_u16(6, static_cast<std::uint16_t>(frame.type));
  for (int index = 0; index < 8; ++index) {
    header[8 + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>((frame.correlation >> (8 * index)) & 0xFFu);
  }
  store_u32(16, static_cast<std::uint32_t>(payload.size()));
  store_u32(20, crc32(payload));
  store_u32(24, crc32(std::span<const std::uint8_t>(header.data(), 24)));
  store_u32(28, 0);
  return header;
}

Result<std::vector<std::uint8_t>> encode_frame(const Frame& frame, const Limits& limits) {
  if (frame.version != kProtocolVersion) {
    return protocol_error("refusing to encode unsupported protocol version");
  }
  if (frame.type == MessageType::INVALID) {
    return protocol_error("refusing to encode INVALID message type");
  }
  if (frame.payload.size() > limits.max_frame_payload_bytes) {
    return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS, "frame payload exceeds bound");
  }
  std::vector<std::uint8_t> header = encode_frame_header(frame, frame.payload, limits);
  std::vector<std::uint8_t> bytes;
  bytes.reserve(header.size() + frame.payload.size());
  bytes.insert(bytes.end(), header.begin(), header.end());
  bytes.insert(bytes.end(), frame.payload.begin(), frame.payload.end());
  return bytes;
}

FrameDecodeOutcome decode_frame(std::span<const std::uint8_t> bytes, const Limits& limits) {
  FrameDecodeOutcome outcome;
  if (bytes.size() < kFrameHeaderBytes) {
    outcome.status = protocol_error("short frame header");
    return outcome;
  }
  auto header = parse_frame_header(bytes.first(kFrameHeaderBytes), limits);
  if (!header.ok()) {
    outcome.status = header.status();
    return outcome;
  }
  const std::size_t total = kFrameHeaderBytes + static_cast<std::size_t>(header.value().payload_length);
  if (bytes.size() < total) {
    outcome.status = protocol_error("short frame payload");
    return outcome;
  }
  const std::span<const std::uint8_t> payload = bytes.subspan(kFrameHeaderBytes, header.value().payload_length);
  if (crc32(payload) != header.value().payload_crc) {
    outcome.status = protocol_error("frame payload checksum mismatch");
    return outcome;
  }
  outcome.frame.version = header.value().version;
  outcome.frame.type = header.value().type;
  outcome.frame.correlation = header.value().correlation;
  outcome.frame.payload.assign(payload.begin(), payload.end());
  outcome.consumed = total;
  outcome.status = Status::success();
  return outcome;
}

// ---------------------------------------------------------------------------
// Model codecs
// ---------------------------------------------------------------------------

void write_parameters(PayloadWriter& writer, const std::vector<ParameterAssignment>& parameters) {
  writer.count(static_cast<std::uint32_t>(parameters.size()), 4096);
  for (const ParameterAssignment& parameter : parameters) {
    writer.text(parameter.key, 128);
    write_enum(writer, parameter.kind);
    writer.text(parameter.value, 4096);
  }
}

Result<std::vector<ParameterAssignment>> read_parameters(PayloadReader& reader) {
  auto count = reader.count(4096);
  if (!count.ok()) {
    return count.status();
  }
  std::vector<ParameterAssignment> parameters;
  parameters.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    ParameterAssignment parameter;
    auto key = reader.text(128);
    if (!key.ok()) {
      return key.status();
    }
    parameter.key = std::move(key.value());
    auto kind = read_enum<ParameterKind>(reader, kMaxParameterKind, "parameter kind");
    if (!kind.ok()) {
      return kind.status();
    }
    parameter.kind = kind.value();
    auto value = reader.text(4096);
    if (!value.ok()) {
      return value.status();
    }
    parameter.value = std::move(value.value());
    parameters.push_back(std::move(parameter));
  }
  return parameters;
}

void write_metric_value(PayloadWriter& writer, const MetricValue& value) {
  write_enum(writer, value.kind);
  writer.real(value.real);
  writer.integer(value.integer);
  writer.boolean(value.boolean);
  writer.text(value.category, 128);
}

Result<MetricValue> read_metric_value(PayloadReader& reader) {
  MetricValue value;
  auto kind = read_enum<MetricKind>(reader, kMaxMetricKind, "metric kind");
  if (!kind.ok()) {
    return kind.status();
  }
  value.kind = kind.value();
  auto real = reader.real();
  if (!real.ok()) {
    return real.status();
  }
  value.real = real.value();
  auto integer = reader.integer();
  if (!integer.ok()) {
    return integer.status();
  }
  value.integer = integer.value();
  auto boolean = reader.boolean();
  if (!boolean.ok()) {
    return boolean.status();
  }
  value.boolean = boolean.value();
  auto category = reader.text(128);
  if (!category.ok()) {
    return category.status();
  }
  value.category = std::move(category.value());
  return value;
}

void write_envelope(PayloadWriter& writer, const AuthorityEnvelope& envelope) {
  write_id(writer, envelope.epoch);
  write_id(writer, envelope.experiment);
  write_id(writer, envelope.generation);
  write_id(writer, envelope.hypothesis);
  write_id(writer, envelope.hypothesis_revision);
  write_id(writer, envelope.policy);
  write_id(writer, envelope.policy_generation);
  write_id(writer, envelope.branch);
  write_id(writer, envelope.branch_generation);
  write_id(writer, envelope.trial);
  write_id(writer, envelope.attempt);
  write_id(writer, envelope.worker);
  write_id(writer, envelope.boot);
  write_id(writer, envelope.producer);
}

Result<AuthorityEnvelope> read_envelope(PayloadReader& reader) {
  AuthorityEnvelope envelope;
  auto epoch = read_id<CoordinatorEpoch>(reader);
  if (!epoch.ok()) return epoch.status();
  envelope.epoch = epoch.value();
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  envelope.experiment = experiment.value();
  auto generation = read_id<ExperimentGeneration>(reader);
  if (!generation.ok()) return generation.status();
  envelope.generation = generation.value();
  auto hypothesis = read_id<HypothesisId>(reader);
  if (!hypothesis.ok()) return hypothesis.status();
  envelope.hypothesis = hypothesis.value();
  auto hypothesis_revision = read_id<HypothesisRevision>(reader);
  if (!hypothesis_revision.ok()) return hypothesis_revision.status();
  envelope.hypothesis_revision = hypothesis_revision.value();
  auto policy = read_id<PolicyId>(reader);
  if (!policy.ok()) return policy.status();
  envelope.policy = policy.value();
  auto policy_generation = read_id<PolicyGeneration>(reader);
  if (!policy_generation.ok()) return policy_generation.status();
  envelope.policy_generation = policy_generation.value();
  auto branch = read_id<BranchId>(reader);
  if (!branch.ok()) return branch.status();
  envelope.branch = branch.value();
  auto branch_generation = read_id<BranchGeneration>(reader);
  if (!branch_generation.ok()) return branch_generation.status();
  envelope.branch_generation = branch_generation.value();
  auto trial = read_id<TrialId>(reader);
  if (!trial.ok()) return trial.status();
  envelope.trial = trial.value();
  auto attempt = read_id<TrialAttemptId>(reader);
  if (!attempt.ok()) return attempt.status();
  envelope.attempt = attempt.value();
  auto worker = read_id<WorkerId>(reader);
  if (!worker.ok()) return worker.status();
  envelope.worker = worker.value();
  auto boot = read_id<WorkerBootId>(reader);
  if (!boot.ok()) return boot.status();
  envelope.boot = boot.value();
  auto producer = read_id<ProducerId>(reader);
  if (!producer.ok()) return producer.status();
  envelope.producer = producer.value();
  return envelope;
}

void write_observation(PayloadWriter& writer, const Observation& observation) {
  write_id(writer, observation.id);
  write_id(writer, observation.experiment);
  write_id(writer, observation.generation);
  write_id(writer, observation.branch);
  write_id(writer, observation.branch_generation);
  write_id(writer, observation.trial);
  write_id(writer, observation.attempt);
  write_id(writer, observation.metric);
  write_id(writer, observation.producer);
  write_id(writer, observation.worker);
  write_id(writer, observation.boot);
  write_id(writer, observation.epoch);
  write_enum(writer, observation.validity);
  write_metric_value(writer, observation.value);
  writer.text(observation.detail, 512);
  writer.u64(observation.sequence);
}

Result<Observation> read_observation(PayloadReader& reader) {
  Observation observation;
  auto id = read_id<ObservationId>(reader);
  if (!id.ok()) return id.status();
  observation.id = id.value();
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  observation.experiment = experiment.value();
  auto generation = read_id<ExperimentGeneration>(reader);
  if (!generation.ok()) return generation.status();
  observation.generation = generation.value();
  auto branch = read_id<BranchId>(reader);
  if (!branch.ok()) return branch.status();
  observation.branch = branch.value();
  auto branch_generation = read_id<BranchGeneration>(reader);
  if (!branch_generation.ok()) return branch_generation.status();
  observation.branch_generation = branch_generation.value();
  auto trial = read_id<TrialId>(reader);
  if (!trial.ok()) return trial.status();
  observation.trial = trial.value();
  auto attempt = read_id<TrialAttemptId>(reader);
  if (!attempt.ok()) return attempt.status();
  observation.attempt = attempt.value();
  auto metric = read_id<MetricId>(reader);
  if (!metric.ok()) return metric.status();
  observation.metric = metric.value();
  auto producer = read_id<ProducerId>(reader);
  if (!producer.ok()) return producer.status();
  observation.producer = producer.value();
  auto worker = read_id<WorkerId>(reader);
  if (!worker.ok()) return worker.status();
  observation.worker = worker.value();
  auto boot = read_id<WorkerBootId>(reader);
  if (!boot.ok()) return boot.status();
  observation.boot = boot.value();
  auto epoch = read_id<CoordinatorEpoch>(reader);
  if (!epoch.ok()) return epoch.status();
  observation.epoch = epoch.value();
  auto validity = read_enum<ObservationValidity>(reader, kMaxObservationValidity, "observation validity");
  if (!validity.ok()) return validity.status();
  observation.validity = validity.value();
  auto value = read_metric_value(reader);
  if (!value.ok()) return value.status();
  observation.value = value.value();
  auto detail = reader.text(512);
  if (!detail.ok()) return detail.status();
  observation.detail = std::move(detail.value());
  auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.status();
  observation.sequence = sequence.value();
  return observation;
}

void write_artifact(PayloadWriter& writer, const ArtifactRecord& artifact) {
  write_id(writer, artifact.id);
  write_id(writer, artifact.experiment);
  write_id(writer, artifact.generation);
  write_id(writer, artifact.branch);
  write_id(writer, artifact.trial);
  write_id(writer, artifact.attempt);
  writer.text(artifact.category, 128);
  writer.text(artifact.locator, 1024);
  write_digest(writer, artifact.content_digest);
  writer.boolean(artifact.digest_present);
  writer.boolean(artifact.size_present);
  writer.u64(artifact.size_bytes);
  writer.text(artifact.provenance, 512);
  writer.boolean(artifact.current);
  writer.u64(artifact.sequence);
}

Result<ArtifactRecord> read_artifact(PayloadReader& reader) {
  ArtifactRecord artifact;
  auto id = read_id<ArtifactId>(reader);
  if (!id.ok()) return id.status();
  artifact.id = id.value();
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  artifact.experiment = experiment.value();
  auto generation = read_id<ExperimentGeneration>(reader);
  if (!generation.ok()) return generation.status();
  artifact.generation = generation.value();
  auto branch = read_id<BranchId>(reader);
  if (!branch.ok()) return branch.status();
  artifact.branch = branch.value();
  auto trial = read_id<TrialId>(reader);
  if (!trial.ok()) return trial.status();
  artifact.trial = trial.value();
  auto attempt = read_id<TrialAttemptId>(reader);
  if (!attempt.ok()) return attempt.status();
  artifact.attempt = attempt.value();
  auto category = reader.text(128);
  if (!category.ok()) return category.status();
  artifact.category = std::move(category.value());
  auto locator = reader.text(1024);
  if (!locator.ok()) return locator.status();
  artifact.locator = std::move(locator.value());
  auto digest = read_digest(reader);
  if (!digest.ok()) return digest.status();
  artifact.content_digest = digest.value();
  auto digest_present = reader.boolean();
  if (!digest_present.ok()) return digest_present.status();
  artifact.digest_present = digest_present.value();
  auto size_present = reader.boolean();
  if (!size_present.ok()) return size_present.status();
  artifact.size_present = size_present.value();
  auto size_bytes = reader.u64();
  if (!size_bytes.ok()) return size_bytes.status();
  artifact.size_bytes = size_bytes.value();
  auto provenance = reader.text(512);
  if (!provenance.ok()) return provenance.status();
  artifact.provenance = std::move(provenance.value());
  auto current = reader.boolean();
  if (!current.ok()) return current.status();
  artifact.current = current.value();
  auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.status();
  artifact.sequence = sequence.value();
  return artifact;
}

void write_metric_definition(PayloadWriter& writer, const MetricDefinition& metric) {
  write_id(writer, metric.id);
  writer.text(metric.name, 128);
  writer.text(metric.unit, 32);
  write_enum(writer, metric.kind);
  write_enum(writer, metric.direction);
  write_enum(writer, metric.aggregation);
  writer.boolean(metric.required);
  writer.boolean(metric.accepts_non_finite);
  write_optional_real(writer, metric.lower_bound);
  write_optional_real(writer, metric.upper_bound);
  write_optional_real(writer, metric.target_low);
  write_optional_real(writer, metric.target_high);
  writer.u32(metric.weight);
  writer.u32(metric.priority);
  write_vector<std::string>(writer, metric.categories, 256, [&writer](const std::string& value) {
    writer.text(value, 128);
  });
}

Result<MetricDefinition> read_metric_definition(PayloadReader& reader) {
  MetricDefinition metric;
  auto id = read_id<MetricId>(reader);
  if (!id.ok()) return id.status();
  metric.id = id.value();
  auto name = reader.text(128);
  if (!name.ok()) return name.status();
  metric.name = std::move(name.value());
  auto unit = reader.text(32);
  if (!unit.ok()) return unit.status();
  metric.unit = std::move(unit.value());
  auto kind = read_enum<MetricKind>(reader, kMaxMetricKind, "metric kind");
  if (!kind.ok()) return kind.status();
  metric.kind = kind.value();
  auto direction = read_enum<MetricDirection>(reader, kMaxMetricDirection, "metric direction");
  if (!direction.ok()) return direction.status();
  metric.direction = direction.value();
  auto aggregation = read_enum<AggregationRule>(reader, kMaxAggregationRule, "aggregation rule");
  if (!aggregation.ok()) return aggregation.status();
  metric.aggregation = aggregation.value();
  auto required = reader.boolean();
  if (!required.ok()) return required.status();
  metric.required = required.value();
  auto accepts_non_finite = reader.boolean();
  if (!accepts_non_finite.ok()) return accepts_non_finite.status();
  metric.accepts_non_finite = accepts_non_finite.value();
  auto lower = read_optional_real(reader);
  if (!lower.ok()) return lower.status();
  metric.lower_bound = lower.value();
  auto upper = read_optional_real(reader);
  if (!upper.ok()) return upper.status();
  metric.upper_bound = upper.value();
  auto target_low = read_optional_real(reader);
  if (!target_low.ok()) return target_low.status();
  metric.target_low = target_low.value();
  auto target_high = read_optional_real(reader);
  if (!target_high.ok()) return target_high.status();
  metric.target_high = target_high.value();
  auto weight = reader.u32();
  if (!weight.ok()) return weight.status();
  metric.weight = weight.value();
  auto priority = reader.u32();
  if (!priority.ok()) return priority.status();
  metric.priority = priority.value();
  auto count = reader.count(256);
  if (!count.ok()) return count.status();
  metric.categories.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    auto category = reader.text(128);
    if (!category.ok()) return category.status();
    metric.categories.push_back(std::move(category.value()));
  }
  return metric;
}

void write_comparison_rule(PayloadWriter& writer, const ComparisonRule& rule) {
  write_id(writer, rule.metric);
  write_enum(writer, rule.op);
  writer.real(rule.threshold);
  writer.real(rule.target_low);
  writer.real(rule.target_high);
  writer.u32(rule.priority);
}

Result<ComparisonRule> read_comparison_rule(PayloadReader& reader) {
  ComparisonRule rule;
  auto metric = read_id<MetricId>(reader);
  if (!metric.ok()) return metric.status();
  rule.metric = metric.value();
  auto op = read_enum<ComparisonOp>(reader, kMaxComparisonOp, "comparison op");
  if (!op.ok()) return op.status();
  rule.op = op.value();
  auto threshold = reader.real();
  if (!threshold.ok()) return threshold.status();
  rule.threshold = threshold.value();
  auto target_low = reader.real();
  if (!target_low.ok()) return target_low.status();
  rule.target_low = target_low.value();
  auto target_high = reader.real();
  if (!target_high.ok()) return target_high.status();
  rule.target_high = target_high.value();
  auto priority = reader.u32();
  if (!priority.ok()) return priority.status();
  rule.priority = priority.value();
  return rule;
}

void write_comparison_policy(PayloadWriter& writer, const ComparisonPolicy& policy) {
  write_id(writer, policy.id);
  write_id(writer, policy.generation);
  writer.boolean(policy.require_baseline);
  writer.u32(policy.min_completed_trials_per_branch);
  writer.u32(policy.min_valid_observations_per_required_metric);
  writer.boolean(policy.reject_on_any_invalid_observation);
  writer.boolean(policy.require_environment_match);
  writer.u32(policy.majority_wins_min_basis_points);
  writer.boolean(policy.use_weighted_score);
  writer.real(policy.accept_score_threshold);
  write_vector<ComparisonRule>(writer, policy.rules, 256, [&writer](const ComparisonRule& rule) {
    write_comparison_rule(writer, rule);
  });
}

Result<ComparisonPolicy> read_comparison_policy(PayloadReader& reader) {
  ComparisonPolicy policy;
  auto id = read_id<PolicyId>(reader);
  if (!id.ok()) return id.status();
  policy.id = id.value();
  auto generation = read_id<PolicyGeneration>(reader);
  if (!generation.ok()) return generation.status();
  policy.generation = generation.value();
  auto require_baseline = reader.boolean();
  if (!require_baseline.ok()) return require_baseline.status();
  policy.require_baseline = require_baseline.value();
  auto min_trials = reader.u32();
  if (!min_trials.ok()) return min_trials.status();
  policy.min_completed_trials_per_branch = min_trials.value();
  auto min_obs = reader.u32();
  if (!min_obs.ok()) return min_obs.status();
  policy.min_valid_observations_per_required_metric = min_obs.value();
  auto reject_invalid = reader.boolean();
  if (!reject_invalid.ok()) return reject_invalid.status();
  policy.reject_on_any_invalid_observation = reject_invalid.value();
  auto require_env = reader.boolean();
  if (!require_env.ok()) return require_env.status();
  policy.require_environment_match = require_env.value();
  auto majority = reader.u32();
  if (!majority.ok()) return majority.status();
  policy.majority_wins_min_basis_points = majority.value();
  auto use_score = reader.boolean();
  if (!use_score.ok()) return use_score.status();
  policy.use_weighted_score = use_score.value();
  auto threshold = reader.real();
  if (!threshold.ok()) return threshold.status();
  policy.accept_score_threshold = threshold.value();
  auto count = reader.count(256);
  if (!count.ok()) return count.status();
  policy.rules.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    auto rule = read_comparison_rule(reader);
    if (!rule.ok()) return rule.status();
    policy.rules.push_back(rule.value());
  }
  return policy;
}

void write_branch_definition(PayloadWriter& writer, const BranchDefinition& branch) {
  write_id(writer, branch.id);
  writer.text(branch.name, 128);
  write_enum(writer, branch.role);
  write_id(writer, branch.parent);
  write_id(writer, branch.generation);
  write_enum(writer, branch.state);
  write_parameters(writer, branch.parameters);
  write_id(writer, branch.input_set);
  writer.boolean(branch.input_set_present);
  write_id(writer, branch.environment);
  writer.boolean(branch.environment_present);
  writer.text(branch.environment_fingerprint, 512);
  write_optional_u64(writer, branch.seed);
  writer.u32(branch.planned_trials);
  writer.u64(branch.created_sequence);
  writer.boolean(branch.forked);
}

Result<BranchDefinition> read_branch_definition(PayloadReader& reader) {
  BranchDefinition branch;
  auto id = read_id<BranchId>(reader);
  if (!id.ok()) return id.status();
  branch.id = id.value();
  auto name = reader.text(128);
  if (!name.ok()) return name.status();
  branch.name = std::move(name.value());
  auto role = read_enum<BranchRole>(reader, kMaxBranchRole, "branch role");
  if (!role.ok()) return role.status();
  branch.role = role.value();
  auto parent = read_id<BranchId>(reader);
  if (!parent.ok()) return parent.status();
  branch.parent = parent.value();
  auto generation = read_id<BranchGeneration>(reader);
  if (!generation.ok()) return generation.status();
  branch.generation = generation.value();
  auto state = read_enum<BranchState>(reader, kMaxBranchState, "branch state");
  if (!state.ok()) return state.status();
  branch.state = state.value();
  auto parameters = read_parameters(reader);
  if (!parameters.ok()) return parameters.status();
  branch.parameters = std::move(parameters.value());
  auto input_set = read_id<InputSetId>(reader);
  if (!input_set.ok()) return input_set.status();
  branch.input_set = input_set.value();
  auto input_set_present = reader.boolean();
  if (!input_set_present.ok()) return input_set_present.status();
  branch.input_set_present = input_set_present.value();
  auto environment = read_id<EnvironmentId>(reader);
  if (!environment.ok()) return environment.status();
  branch.environment = environment.value();
  auto environment_present = reader.boolean();
  if (!environment_present.ok()) return environment_present.status();
  branch.environment_present = environment_present.value();
  auto fingerprint = reader.text(512);
  if (!fingerprint.ok()) return fingerprint.status();
  branch.environment_fingerprint = std::move(fingerprint.value());
  auto seed = read_optional_u64(reader);
  if (!seed.ok()) return seed.status();
  branch.seed = seed.value();
  auto planned = reader.u32();
  if (!planned.ok()) return planned.status();
  branch.planned_trials = planned.value();
  auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.status();
  branch.created_sequence = sequence.value();
  auto forked = reader.boolean();
  if (!forked.ok()) return forked.status();
  branch.forked = forked.value();
  return branch;
}

void write_experiment_definition(PayloadWriter& writer, const ExperimentDefinition& definition) {
  write_id(writer, definition.id);
  write_id(writer, definition.generation);
  write_id(writer, definition.hypothesis);
  write_id(writer, definition.hypothesis_revision);
  writer.text(definition.name, 128);
  write_vector<BranchDefinition>(writer, definition.branches, 256, [&writer](const BranchDefinition& value) {
    write_branch_definition(writer, value);
  });
  write_vector<MetricDefinition>(writer, definition.metrics, 128, [&writer](const MetricDefinition& value) {
    write_metric_definition(writer, value);
  });
  write_comparison_policy(writer, definition.policy);
  write_enum(writer, definition.seed_policy);
  write_optional_u64(writer, definition.experiment_seed);
  write_enum(writer, definition.retry_policy);
  writer.u32(definition.max_attempts_per_trial);
  write_enum(writer, definition.partial_failure_policy);
  write_enum(writer, definition.late_evidence_policy);
  write_parameters(writer, definition.parameters);
  writer.text(definition.provenance, 1024);
  writer.u64(definition.created_sequence);
}

Result<ExperimentDefinition> read_experiment_definition(PayloadReader& reader) {
  ExperimentDefinition definition;
  auto id = read_id<ExperimentId>(reader);
  if (!id.ok()) return id.status();
  definition.id = id.value();
  auto generation = read_id<ExperimentGeneration>(reader);
  if (!generation.ok()) return generation.status();
  definition.generation = generation.value();
  auto hypothesis = read_id<HypothesisId>(reader);
  if (!hypothesis.ok()) return hypothesis.status();
  definition.hypothesis = hypothesis.value();
  auto revision = read_id<HypothesisRevision>(reader);
  if (!revision.ok()) return revision.status();
  definition.hypothesis_revision = revision.value();
  auto name = reader.text(128);
  if (!name.ok()) return name.status();
  definition.name = std::move(name.value());

  auto branch_count = reader.count(256);
  if (!branch_count.ok()) return branch_count.status();
  definition.branches.reserve(branch_count.value());
  for (std::uint32_t index = 0; index < branch_count.value(); ++index) {
    auto branch = read_branch_definition(reader);
    if (!branch.ok()) return branch.status();
    definition.branches.push_back(std::move(branch.value()));
  }

  auto metric_count = reader.count(128);
  if (!metric_count.ok()) return metric_count.status();
  definition.metrics.reserve(metric_count.value());
  for (std::uint32_t index = 0; index < metric_count.value(); ++index) {
    auto metric = read_metric_definition(reader);
    if (!metric.ok()) return metric.status();
    definition.metrics.push_back(std::move(metric.value()));
  }

  auto policy = read_comparison_policy(reader);
  if (!policy.ok()) return policy.status();
  definition.policy = policy.value();

  auto seed_policy = read_enum<SeedPolicy>(reader, kMaxSeedPolicy, "seed policy");
  if (!seed_policy.ok()) return seed_policy.status();
  definition.seed_policy = seed_policy.value();
  auto experiment_seed = read_optional_u64(reader);
  if (!experiment_seed.ok()) return experiment_seed.status();
  definition.experiment_seed = experiment_seed.value();
  auto retry_policy = read_enum<RetryPolicy>(reader, kMaxRetryPolicy, "retry policy");
  if (!retry_policy.ok()) return retry_policy.status();
  definition.retry_policy = retry_policy.value();
  auto max_attempts = reader.u32();
  if (!max_attempts.ok()) return max_attempts.status();
  definition.max_attempts_per_trial = max_attempts.value();
  auto partial = read_enum<PartialFailurePolicy>(reader, kMaxPartialFailurePolicy, "partial failure policy");
  if (!partial.ok()) return partial.status();
  definition.partial_failure_policy = partial.value();
  auto late = read_enum<LateEvidencePolicy>(reader, kMaxLateEvidencePolicy, "late evidence policy");
  if (!late.ok()) return late.status();
  definition.late_evidence_policy = late.value();
  auto parameters = read_parameters(reader);
  if (!parameters.ok()) return parameters.status();
  definition.parameters = std::move(parameters.value());
  auto provenance = reader.text(1024);
  if (!provenance.ok()) return provenance.status();
  definition.provenance = std::move(provenance.value());
  auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.status();
  definition.created_sequence = sequence.value();
  return definition;
}

void write_hypothesis(PayloadWriter& writer, const Hypothesis& hypothesis) {
  write_id(writer, hypothesis.id);
  write_id(writer, hypothesis.revision);
  writer.text(hypothesis.statement, 4096);
  writer.boolean(hypothesis.expected_direction.has_value());
  if (hypothesis.expected_direction.has_value()) {
    write_enum(writer, *hypothesis.expected_direction);
  }
  write_id(writer, hypothesis.expected_metric);
  write_id(writer, hypothesis.parent);
  write_id(writer, hypothesis.parent_revision);
  writer.boolean(hypothesis.superseded);
  write_id(writer, hypothesis.superseded_by_revision);
  writer.text(hypothesis.provenance, 1024);
  writer.u64(hypothesis.created_sequence);
}

Result<Hypothesis> read_hypothesis(PayloadReader& reader) {
  Hypothesis hypothesis;
  auto id = read_id<HypothesisId>(reader);
  if (!id.ok()) return id.status();
  hypothesis.id = id.value();
  auto revision = read_id<HypothesisRevision>(reader);
  if (!revision.ok()) return revision.status();
  hypothesis.revision = revision.value();
  auto statement = reader.text(4096);
  if (!statement.ok()) return statement.status();
  hypothesis.statement = std::move(statement.value());
  auto has_direction = reader.boolean();
  if (!has_direction.ok()) return has_direction.status();
  if (has_direction.value()) {
    auto direction = read_enum<MetricDirection>(reader, kMaxMetricDirection, "metric direction");
    if (!direction.ok()) return direction.status();
    hypothesis.expected_direction = direction.value();
  }
  auto metric = read_id<MetricId>(reader);
  if (!metric.ok()) return metric.status();
  hypothesis.expected_metric = metric.value();
  auto parent = read_id<HypothesisId>(reader);
  if (!parent.ok()) return parent.status();
  hypothesis.parent = parent.value();
  auto parent_revision = read_id<HypothesisRevision>(reader);
  if (!parent_revision.ok()) return parent_revision.status();
  hypothesis.parent_revision = parent_revision.value();
  auto superseded = reader.boolean();
  if (!superseded.ok()) return superseded.status();
  hypothesis.superseded = superseded.value();
  auto superseded_by = read_id<HypothesisRevision>(reader);
  if (!superseded_by.ok()) return superseded_by.status();
  hypothesis.superseded_by_revision = superseded_by.value();
  auto provenance = reader.text(1024);
  if (!provenance.ok()) return provenance.status();
  hypothesis.provenance = std::move(provenance.value());
  auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.status();
  hypothesis.created_sequence = sequence.value();
  return hypothesis;
}

void write_trial(PayloadWriter& writer, const TrialRecord& trial) {
  write_id(writer, trial.id);
  write_id(writer, trial.experiment);
  write_id(writer, trial.generation);
  write_id(writer, trial.branch);
  write_id(writer, trial.branch_generation);
  write_id(writer, trial.hypothesis);
  write_id(writer, trial.hypothesis_revision);
  write_id(writer, trial.policy);
  write_id(writer, trial.policy_generation);
  write_enum(writer, trial.state);
  write_id(writer, trial.authoritative_attempt);
  write_id(writer, trial.next_attempt_number);
  writer.u32(trial.attempt_count);
  writer.u32(trial.committed_attempts);
  write_enum(writer, trial.failure_kind);
  writer.text(trial.failure_detail, 512);
  write_optional_u64(writer, trial.seed);
  write_id(writer, trial.input_set);
  writer.boolean(trial.input_set_present);
  write_id(writer, trial.environment);
  writer.boolean(trial.environment_present);
  writer.text(trial.environment_fingerprint, 512);
  write_vector<ArtifactId>(writer, trial.artifacts, 256,
                            [&writer](const ArtifactId& value) { write_id(writer, value); });
  writer.text(trial.payload, 4096);
  writer.u64(trial.created_sequence);
  writer.u64(trial.updated_sequence);
}

Result<TrialRecord> read_trial(PayloadReader& reader) {
  TrialRecord trial;
  auto id = read_id<TrialId>(reader);
  if (!id.ok()) return id.status();
  trial.id = id.value();
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  trial.experiment = experiment.value();
  auto generation = read_id<ExperimentGeneration>(reader);
  if (!generation.ok()) return generation.status();
  trial.generation = generation.value();
  auto branch = read_id<BranchId>(reader);
  if (!branch.ok()) return branch.status();
  trial.branch = branch.value();
  auto branch_generation = read_id<BranchGeneration>(reader);
  if (!branch_generation.ok()) return branch_generation.status();
  trial.branch_generation = branch_generation.value();
  auto hypothesis = read_id<HypothesisId>(reader);
  if (!hypothesis.ok()) return hypothesis.status();
  trial.hypothesis = hypothesis.value();
  auto revision = read_id<HypothesisRevision>(reader);
  if (!revision.ok()) return revision.status();
  trial.hypothesis_revision = revision.value();
  auto policy = read_id<PolicyId>(reader);
  if (!policy.ok()) return policy.status();
  trial.policy = policy.value();
  auto policy_generation = read_id<PolicyGeneration>(reader);
  if (!policy_generation.ok()) return policy_generation.status();
  trial.policy_generation = policy_generation.value();
  auto state = read_enum<TrialState>(reader, kMaxTrialState, "trial state");
  if (!state.ok()) return state.status();
  trial.state = state.value();
  auto attempt = read_id<TrialAttemptId>(reader);
  if (!attempt.ok()) return attempt.status();
  trial.authoritative_attempt = attempt.value();
  auto next_attempt = read_id<TrialAttemptNumber>(reader);
  if (!next_attempt.ok()) return next_attempt.status();
  trial.next_attempt_number = next_attempt.value();
  auto attempt_count = reader.u32();
  if (!attempt_count.ok()) return attempt_count.status();
  trial.attempt_count = attempt_count.value();
  auto committed = reader.u32();
  if (!committed.ok()) return committed.status();
  trial.committed_attempts = committed.value();
  auto failure = read_enum<FailureKind>(reader, kMaxFailureKind, "failure kind");
  if (!failure.ok()) return failure.status();
  trial.failure_kind = failure.value();
  auto detail = reader.text(512);
  if (!detail.ok()) return detail.status();
  trial.failure_detail = std::move(detail.value());
  auto seed = read_optional_u64(reader);
  if (!seed.ok()) return seed.status();
  trial.seed = seed.value();
  auto input_set = read_id<InputSetId>(reader);
  if (!input_set.ok()) return input_set.status();
  trial.input_set = input_set.value();
  auto input_set_present = reader.boolean();
  if (!input_set_present.ok()) return input_set_present.status();
  trial.input_set_present = input_set_present.value();
  auto environment = read_id<EnvironmentId>(reader);
  if (!environment.ok()) return environment.status();
  trial.environment = environment.value();
  auto environment_present = reader.boolean();
  if (!environment_present.ok()) return environment_present.status();
  trial.environment_present = environment_present.value();
  auto fingerprint = reader.text(512);
  if (!fingerprint.ok()) return fingerprint.status();
  trial.environment_fingerprint = std::move(fingerprint.value());
  auto artifact_count = reader.count(256);
  if (!artifact_count.ok()) return artifact_count.status();
  trial.artifacts.reserve(artifact_count.value());
  for (std::uint32_t index = 0; index < artifact_count.value(); ++index) {
    auto artifact = read_id<ArtifactId>(reader);
    if (!artifact.ok()) return artifact.status();
    trial.artifacts.push_back(artifact.value());
  }
  auto payload = reader.text(4096);
  if (!payload.ok()) return payload.status();
  trial.payload = std::move(payload.value());
  auto created = reader.u64();
  if (!created.ok()) return created.status();
  trial.created_sequence = created.value();
  auto updated = reader.u64();
  if (!updated.ok()) return updated.status();
  trial.updated_sequence = updated.value();
  return trial;
}

void write_attempt(PayloadWriter& writer, const AttemptRecord& attempt) {
  write_id(writer, attempt.id);
  write_id(writer, attempt.number);
  write_id(writer, attempt.trial);
  write_id(writer, attempt.experiment);
  write_id(writer, attempt.generation);
  write_id(writer, attempt.branch);
  write_id(writer, attempt.branch_generation);
  write_id(writer, attempt.worker);
  write_id(writer, attempt.boot);
  write_id(writer, attempt.epoch);
  write_id(writer, attempt.producer);
  write_enum(writer, attempt.state);
  write_enum(writer, attempt.failure_kind);
  writer.text(attempt.failure_detail, 512);
  writer.boolean(attempt.committed);
  writer.u64(attempt.issued_sequence);
  writer.u64(attempt.updated_sequence);
}

Result<AttemptRecord> read_attempt(PayloadReader& reader) {
  AttemptRecord attempt;
  auto id = read_id<TrialAttemptId>(reader);
  if (!id.ok()) return id.status();
  attempt.id = id.value();
  auto number = read_id<TrialAttemptNumber>(reader);
  if (!number.ok()) return number.status();
  attempt.number = number.value();
  auto trial = read_id<TrialId>(reader);
  if (!trial.ok()) return trial.status();
  attempt.trial = trial.value();
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  attempt.experiment = experiment.value();
  auto generation = read_id<ExperimentGeneration>(reader);
  if (!generation.ok()) return generation.status();
  attempt.generation = generation.value();
  auto branch = read_id<BranchId>(reader);
  if (!branch.ok()) return branch.status();
  attempt.branch = branch.value();
  auto branch_generation = read_id<BranchGeneration>(reader);
  if (!branch_generation.ok()) return branch_generation.status();
  attempt.branch_generation = branch_generation.value();
  auto worker = read_id<WorkerId>(reader);
  if (!worker.ok()) return worker.status();
  attempt.worker = worker.value();
  auto boot = read_id<WorkerBootId>(reader);
  if (!boot.ok()) return boot.status();
  attempt.boot = boot.value();
  auto epoch = read_id<CoordinatorEpoch>(reader);
  if (!epoch.ok()) return epoch.status();
  attempt.epoch = epoch.value();
  auto producer = read_id<ProducerId>(reader);
  if (!producer.ok()) return producer.status();
  attempt.producer = producer.value();
  auto state = read_enum<AttemptState>(reader, kMaxAttemptState, "attempt state");
  if (!state.ok()) return state.status();
  attempt.state = state.value();
  auto failure = read_enum<FailureKind>(reader, kMaxFailureKind, "failure kind");
  if (!failure.ok()) return failure.status();
  attempt.failure_kind = failure.value();
  auto detail = reader.text(512);
  if (!detail.ok()) return detail.status();
  attempt.failure_detail = std::move(detail.value());
  auto committed = reader.boolean();
  if (!committed.ok()) return committed.status();
  attempt.committed = committed.value();
  auto issued = reader.u64();
  if (!issued.ok()) return issued.status();
  attempt.issued_sequence = issued.value();
  auto updated = reader.u64();
  if (!updated.ok()) return updated.status();
  attempt.updated_sequence = updated.value();
  return attempt;
}

void write_decision(PayloadWriter& writer, const Decision& decision) {
  write_id(writer, decision.id);
  write_id(writer, decision.experiment);
  write_id(writer, decision.generation);
  write_id(writer, decision.hypothesis);
  write_id(writer, decision.hypothesis_revision);
  write_id(writer, decision.policy);
  write_id(writer, decision.policy_generation);
  write_vector<BranchId>(writer, decision.compared_branches, 256,
                         [&writer](const BranchId& value) { write_id(writer, value); });
  write_id(writer, decision.baseline);
  writer.boolean(decision.baseline_present);
  write_id(writer, decision.candidate);
  write_enum(writer, decision.outcome);
  write_enum(writer, decision.reason_kind);
  writer.text(decision.reason, 4096);
  write_vector<ConstraintResult>(writer, decision.constraints, 512, [&writer](const ConstraintResult& value) {
    writer.text(value.name, 128);
    writer.boolean(value.passed);
    writer.text(value.detail, 512);
  });
  write_vector<ComparisonFactor>(writer, decision.factors, 512, [&writer](const ComparisonFactor& value) {
    write_id(writer, value.metric);
    write_enum(writer, value.op);
    writer.u32(value.priority);
    writer.real(value.threshold);
    writer.boolean(value.has_values);
    writer.real(value.baseline_value);
    writer.real(value.candidate_value);
    writer.real(value.delta);
    writer.boolean(value.satisfied);
    writer.boolean(value.decisive);
    writer.text(value.detail, 512);
  });
  write_vector<BranchEvidence>(writer, decision.evidence, 256, [&writer](const BranchEvidence& value) {
    write_id(writer, value.branch);
    write_id(writer, value.generation);
    write_enum(writer, value.role);
    write_enum(writer, value.state);
    writer.u32(value.planned_trials);
    writer.u32(value.completed_trials);
    writer.u32(value.failed_trials);
    writer.u32(value.cancelled_trials);
    writer.u32(value.invalid_trials);
    writer.u32(value.superseded_trials);
    writer.u32(value.open_trials);
    write_vector<MetricAggregate>(writer, value.aggregates, 128, [&writer](const MetricAggregate& aggregate) {
      write_id(writer, aggregate.metric);
      write_enum(writer, aggregate.rule);
      writer.boolean(aggregate.present);
      writer.real(aggregate.value);
      writer.text(aggregate.category, 128);
      writer.u32(aggregate.valid_count);
      writer.u32(aggregate.invalid_count);
      writer.u32(aggregate.missing_count);
      writer.u32(aggregate.unsupported_count);
      write_vector<double>(writer, aggregate.raw_values, 4096, [&writer](double raw) { writer.real(raw); });
      write_vector<std::string>(writer, aggregate.trial_order, 4096, [&writer](const std::string& value) {
        writer.text(value, 64);
      });
    });
  });
  write_vector<RejectedEvidence>(writer, decision.rejected_evidence, 4096, [&writer](const RejectedEvidence& value) {
    writer.text(value.kind, 64);
    writer.u64(value.identifier);
    writer.u8(static_cast<std::uint8_t>(value.reason));
    writer.text(value.detail, 512);
  });
  writer.text(decision.tie_break_rule, 256);
  writer.boolean(decision.tied);
  write_digest(writer, decision.canonical_state_digest);
  writer.u64(decision.sequence);
}

Result<Decision> read_decision(PayloadReader& reader) {
  Decision decision;
  auto id = read_id<DecisionId>(reader);
  if (!id.ok()) return id.status();
  decision.id = id.value();
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  decision.experiment = experiment.value();
  auto generation = read_id<ExperimentGeneration>(reader);
  if (!generation.ok()) return generation.status();
  decision.generation = generation.value();
  auto hypothesis = read_id<HypothesisId>(reader);
  if (!hypothesis.ok()) return hypothesis.status();
  decision.hypothesis = hypothesis.value();
  auto revision = read_id<HypothesisRevision>(reader);
  if (!revision.ok()) return revision.status();
  decision.hypothesis_revision = revision.value();
  auto policy = read_id<PolicyId>(reader);
  if (!policy.ok()) return policy.status();
  decision.policy = policy.value();
  auto policy_generation = read_id<PolicyGeneration>(reader);
  if (!policy_generation.ok()) return policy_generation.status();
  decision.policy_generation = policy_generation.value();

  auto branch_count = reader.count(256);
  if (!branch_count.ok()) return branch_count.status();
  decision.compared_branches.reserve(branch_count.value());
  for (std::uint32_t index = 0; index < branch_count.value(); ++index) {
    auto branch = read_id<BranchId>(reader);
    if (!branch.ok()) return branch.status();
    decision.compared_branches.push_back(branch.value());
  }
  auto baseline = read_id<BranchId>(reader);
  if (!baseline.ok()) return baseline.status();
  decision.baseline = baseline.value();
  auto baseline_present = reader.boolean();
  if (!baseline_present.ok()) return baseline_present.status();
  decision.baseline_present = baseline_present.value();
  auto candidate = read_id<BranchId>(reader);
  if (!candidate.ok()) return candidate.status();
  decision.candidate = candidate.value();
  auto outcome = read_enum<DecisionOutcome>(reader, kMaxDecisionOutcome, "decision outcome");
  if (!outcome.ok()) return outcome.status();
  decision.outcome = outcome.value();
  auto reason_kind = read_enum<FailureKind>(reader, kMaxFailureKind, "failure kind");
  if (!reason_kind.ok()) return reason_kind.status();
  decision.reason_kind = reason_kind.value();
  auto reason = reader.text(4096);
  if (!reason.ok()) return reason.status();
  decision.reason = std::move(reason.value());

  auto constraint_count = reader.count(512);
  if (!constraint_count.ok()) return constraint_count.status();
  decision.constraints.reserve(constraint_count.value());
  for (std::uint32_t index = 0; index < constraint_count.value(); ++index) {
    ConstraintResult constraint;
    auto name = reader.text(128);
    if (!name.ok()) return name.status();
    constraint.name = std::move(name.value());
    auto passed = reader.boolean();
    if (!passed.ok()) return passed.status();
    constraint.passed = passed.value();
    auto detail = reader.text(512);
    if (!detail.ok()) return detail.status();
    constraint.detail = std::move(detail.value());
    decision.constraints.push_back(std::move(constraint));
  }

  auto factor_count = reader.count(512);
  if (!factor_count.ok()) return factor_count.status();
  decision.factors.reserve(factor_count.value());
  for (std::uint32_t index = 0; index < factor_count.value(); ++index) {
    ComparisonFactor factor;
    auto metric = read_id<MetricId>(reader);
    if (!metric.ok()) return metric.status();
    factor.metric = metric.value();
    auto op = read_enum<ComparisonOp>(reader, kMaxComparisonOp, "comparison op");
    if (!op.ok()) return op.status();
    factor.op = op.value();
    auto priority = reader.u32();
    if (!priority.ok()) return priority.status();
    factor.priority = priority.value();
    auto threshold = reader.real();
    if (!threshold.ok()) return threshold.status();
    factor.threshold = threshold.value();
    auto has_values = reader.boolean();
    if (!has_values.ok()) return has_values.status();
    factor.has_values = has_values.value();
    auto baseline_value = reader.real();
    if (!baseline_value.ok()) return baseline_value.status();
    factor.baseline_value = baseline_value.value();
    auto candidate_value = reader.real();
    if (!candidate_value.ok()) return candidate_value.status();
    factor.candidate_value = candidate_value.value();
    auto delta = reader.real();
    if (!delta.ok()) return delta.status();
    factor.delta = delta.value();
    auto satisfied = reader.boolean();
    if (!satisfied.ok()) return satisfied.status();
    factor.satisfied = satisfied.value();
    auto decisive = reader.boolean();
    if (!decisive.ok()) return decisive.status();
    factor.decisive = decisive.value();
    auto detail = reader.text(512);
    if (!detail.ok()) return detail.status();
    factor.detail = std::move(detail.value());
    decision.factors.push_back(std::move(factor));
  }

  auto evidence_count = reader.count(256);
  if (!evidence_count.ok()) return evidence_count.status();
  decision.evidence.reserve(evidence_count.value());
  for (std::uint32_t index = 0; index < evidence_count.value(); ++index) {
    BranchEvidence evidence;
    auto branch = read_id<BranchId>(reader);
    if (!branch.ok()) return branch.status();
    evidence.branch = branch.value();
    auto generation_value = read_id<BranchGeneration>(reader);
    if (!generation_value.ok()) return generation_value.status();
    evidence.generation = generation_value.value();
    auto role = read_enum<BranchRole>(reader, kMaxBranchRole, "branch role");
    if (!role.ok()) return role.status();
    evidence.role = role.value();
    auto state = read_enum<BranchState>(reader, kMaxBranchState, "branch state");
    if (!state.ok()) return state.status();
    evidence.state = state.value();
    auto planned = reader.u32();
    if (!planned.ok()) return planned.status();
    evidence.planned_trials = planned.value();
    auto completed = reader.u32();
    if (!completed.ok()) return completed.status();
    evidence.completed_trials = completed.value();
    auto failed = reader.u32();
    if (!failed.ok()) return failed.status();
    evidence.failed_trials = failed.value();
    auto cancelled = reader.u32();
    if (!cancelled.ok()) return cancelled.status();
    evidence.cancelled_trials = cancelled.value();
    auto invalid = reader.u32();
    if (!invalid.ok()) return invalid.status();
    evidence.invalid_trials = invalid.value();
    auto superseded = reader.u32();
    if (!superseded.ok()) return superseded.status();
    evidence.superseded_trials = superseded.value();
    auto open = reader.u32();
    if (!open.ok()) return open.status();
    evidence.open_trials = open.value();

    auto aggregate_count = reader.count(128);
    if (!aggregate_count.ok()) return aggregate_count.status();
    evidence.aggregates.reserve(aggregate_count.value());
    for (std::uint32_t aggregate_index = 0; aggregate_index < aggregate_count.value(); ++aggregate_index) {
      MetricAggregate aggregate;
      auto metric = read_id<MetricId>(reader);
      if (!metric.ok()) return metric.status();
      aggregate.metric = metric.value();
      auto rule = read_enum<AggregationRule>(reader, kMaxAggregationRule, "aggregation rule");
      if (!rule.ok()) return rule.status();
      aggregate.rule = rule.value();
      auto present = reader.boolean();
      if (!present.ok()) return present.status();
      aggregate.present = present.value();
      auto value = reader.real();
      if (!value.ok()) return value.status();
      aggregate.value = value.value();
      auto category = reader.text(128);
      if (!category.ok()) return category.status();
      aggregate.category = std::move(category.value());
      auto valid = reader.u32();
      if (!valid.ok()) return valid.status();
      aggregate.valid_count = valid.value();
      auto invalid_count = reader.u32();
      if (!invalid_count.ok()) return invalid_count.status();
      aggregate.invalid_count = invalid_count.value();
      auto missing = reader.u32();
      if (!missing.ok()) return missing.status();
      aggregate.missing_count = missing.value();
      auto unsupported = reader.u32();
      if (!unsupported.ok()) return unsupported.status();
      aggregate.unsupported_count = unsupported.value();
      auto raw_count = reader.count(4096);
      if (!raw_count.ok()) return raw_count.status();
      aggregate.raw_values.reserve(raw_count.value());
      for (std::uint32_t raw_index = 0; raw_index < raw_count.value(); ++raw_index) {
        auto raw = reader.real();
        if (!raw.ok()) return raw.status();
        aggregate.raw_values.push_back(raw.value());
      }
      auto order_count = reader.count(4096);
      if (!order_count.ok()) return order_count.status();
      aggregate.trial_order.reserve(order_count.value());
      for (std::uint32_t order_index = 0; order_index < order_count.value(); ++order_index) {
        auto text = reader.text(64);
        if (!text.ok()) return text.status();
        aggregate.trial_order.push_back(std::move(text.value()));
      }
      evidence.aggregates.push_back(std::move(aggregate));
    }
    decision.evidence.push_back(std::move(evidence));
  }

  auto rejection_count = reader.count(4096);
  if (!rejection_count.ok()) return rejection_count.status();
  decision.rejected_evidence.reserve(rejection_count.value());
  for (std::uint32_t index = 0; index < rejection_count.value(); ++index) {
    RejectedEvidence rejection;
    auto kind = reader.text(64);
    if (!kind.ok()) return kind.status();
    rejection.kind = std::move(kind.value());
    auto identifier = reader.u64();
    if (!identifier.ok()) return identifier.status();
    rejection.identifier = identifier.value();
    auto reason_byte = reader.u8();
    if (!reason_byte.ok()) return reason_byte.status();
    rejection.reason = static_cast<ErrorCode>(reason_byte.value());
    auto detail = reader.text(512);
    if (!detail.ok()) return detail.status();
    rejection.detail = std::move(detail.value());
    decision.rejected_evidence.push_back(std::move(rejection));
  }
  auto tie_break = reader.text(256);
  if (!tie_break.ok()) return tie_break.status();
  decision.tie_break_rule = std::move(tie_break.value());
  auto tied = reader.boolean();
  if (!tied.ok()) return tied.status();
  decision.tied = tied.value();
  auto digest = read_digest(reader);
  if (!digest.ok()) return digest.status();
  decision.canonical_state_digest = digest.value();
  auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.status();
  decision.sequence = sequence.value();
  return decision;
}

void write_rollback_point(PayloadWriter& writer, const RollbackPoint& point) {
  write_id(writer, point.id);
  write_id(writer, point.experiment);
  write_id(writer, point.generation);
  write_id(writer, point.restored_from);
  write_id(writer, point.decision);
  writer.boolean(point.decision_present);
  write_digest(writer, point.state_digest);
  write_vector<BranchAuthoritySnapshot>(writer, point.authority, 256,
                                        [&writer](const BranchAuthoritySnapshot& snapshot) {
                                          write_id(writer, snapshot.branch);
                                          write_id(writer, snapshot.generation);
                                          write_enum(writer, snapshot.state);
                                        });
  writer.u64(point.sequence);
}

Result<RollbackPoint> read_rollback_point(PayloadReader& reader) {
  RollbackPoint point;
  auto id = read_id<RollbackPointId>(reader);
  if (!id.ok()) return id.status();
  point.id = id.value();
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  point.experiment = experiment.value();
  auto generation = read_id<ExperimentGeneration>(reader);
  if (!generation.ok()) return generation.status();
  point.generation = generation.value();
  auto restored_from = read_id<ExperimentGeneration>(reader);
  if (!restored_from.ok()) return restored_from.status();
  point.restored_from = restored_from.value();
  auto decision = read_id<DecisionId>(reader);
  if (!decision.ok()) return decision.status();
  point.decision = decision.value();
  auto decision_present = reader.boolean();
  if (!decision_present.ok()) return decision_present.status();
  point.decision_present = decision_present.value();
  auto digest = read_digest(reader);
  if (!digest.ok()) return digest.status();
  point.state_digest = digest.value();
  auto snapshot_count = reader.count(256);
  if (!snapshot_count.ok()) return snapshot_count.status();
  point.authority.reserve(snapshot_count.value());
  for (std::uint32_t index = 0; index < snapshot_count.value(); ++index) {
    BranchAuthoritySnapshot snapshot;
    auto branch = read_id<BranchId>(reader);
    if (!branch.ok()) return branch.status();
    snapshot.branch = branch.value();
    auto snapshot_generation = read_id<BranchGeneration>(reader);
    if (!snapshot_generation.ok()) return snapshot_generation.status();
    snapshot.generation = snapshot_generation.value();
    auto state = read_enum<BranchState>(reader, kMaxBranchState, "branch state");
    if (!state.ok()) return state.status();
    snapshot.state = state.value();
    point.authority.push_back(snapshot);
  }
  auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.status();
  point.sequence = sequence.value();
  return point;
}

void write_rejected_evidence(PayloadWriter& writer, const RejectedEvidence& evidence) {
  writer.text(evidence.kind, 64);
  writer.u64(evidence.identifier);
  writer.u8(static_cast<std::uint8_t>(evidence.reason));
  writer.text(evidence.detail, 512);
}

Result<RejectedEvidence> read_rejected_evidence(PayloadReader& reader) {
  RejectedEvidence evidence;
  auto kind = reader.text(64);
  if (!kind.ok()) return kind.status();
  evidence.kind = std::move(kind.value());
  auto identifier = reader.u64();
  if (!identifier.ok()) return identifier.status();
  evidence.identifier = identifier.value();
  auto reason = reader.u8();
  if (!reason.ok()) return reason.status();
  evidence.reason = static_cast<ErrorCode>(reason.value());
  auto detail = reader.text(512);
  if (!detail.ok()) return detail.status();
  evidence.detail = std::move(detail.value());
  return evidence;
}

void write_worker_incarnation(PayloadWriter& writer, const WorkerIncarnation& incarnation) {
  write_id(writer, incarnation.worker);
  write_id(writer, incarnation.boot);
  write_id(writer, incarnation.epoch);
  write_id(writer, incarnation.producer);
  writer.text(incarnation.endpoint, 256);
  writer.text(incarnation.fingerprint, 512);
  writer.u64(incarnation.registered_sequence);
  writer.boolean(incarnation.revoked);
  write_enum(writer, incarnation.revocation_kind);
  writer.u64(incarnation.revoked_sequence);
}

Result<WorkerIncarnation> read_worker_incarnation(PayloadReader& reader) {
  WorkerIncarnation incarnation;
  auto worker = read_id<WorkerId>(reader);
  if (!worker.ok()) return worker.status();
  incarnation.worker = worker.value();
  auto boot = read_id<WorkerBootId>(reader);
  if (!boot.ok()) return boot.status();
  incarnation.boot = boot.value();
  auto epoch = read_id<CoordinatorEpoch>(reader);
  if (!epoch.ok()) return epoch.status();
  incarnation.epoch = epoch.value();
  auto producer = read_id<ProducerId>(reader);
  if (!producer.ok()) return producer.status();
  incarnation.producer = producer.value();
  auto endpoint = reader.text(256);
  if (!endpoint.ok()) return endpoint.status();
  incarnation.endpoint = std::move(endpoint.value());
  auto fingerprint = reader.text(512);
  if (!fingerprint.ok()) return fingerprint.status();
  incarnation.fingerprint = std::move(fingerprint.value());
  auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.status();
  incarnation.registered_sequence = sequence.value();
  auto revoked = reader.boolean();
  if (!revoked.ok()) return revoked.status();
  incarnation.revoked = revoked.value();
  auto kind = read_enum<FailureKind>(reader, kMaxFailureKind, "failure kind");
  if (!kind.ok()) return kind.status();
  incarnation.revocation_kind = kind.value();
  auto revoked_sequence = reader.u64();
  if (!revoked_sequence.ok()) return revoked_sequence.status();
  incarnation.revoked_sequence = revoked_sequence.value();
  return incarnation;
}

void write_experiment_record(PayloadWriter& writer, const ExperimentRecord& record) {
  write_experiment_definition(writer, record.definition);
  write_hypothesis(writer, record.hypothesis);
  write_vector<Hypothesis>(writer, record.hypothesis_history, 4096, [&writer](const Hypothesis& value) {
    write_hypothesis(writer, value);
  });
  write_enum(writer, record.state);
  writer.u64(record.revision_sequence);
  writer.u64(record.created_sequence);

  write_vector<TrialRecord>(writer, [&record]() {
    std::vector<TrialRecord> trials;
    trials.reserve(record.trials.size());
    for (const auto& entry : record.trials) {
      trials.push_back(entry.second);
    }
    return trials;
  }(), 4096, [&writer](const TrialRecord& value) { write_trial(writer, value); });

  std::vector<AttemptRecord> attempts;
  attempts.reserve(record.attempts.size());
  for (const auto& entry : record.attempts) {
    attempts.push_back(entry.second);
  }
  write_vector<AttemptRecord>(writer, attempts, 32768, [&writer](const AttemptRecord& value) {
    write_attempt(writer, value);
  });
  write_vector<Observation>(writer, record.observations, 1u << 20, [&writer](const Observation& value) {
    write_observation(writer, value);
  });
  write_vector<ArtifactRecord>(writer, record.artifacts, 65536, [&writer](const ArtifactRecord& value) {
    write_artifact(writer, value);
  });
  write_vector<Decision>(writer, record.decisions, 1024, [&writer](const Decision& value) {
    write_decision(writer, value);
  });
  write_vector<RollbackPoint>(writer, record.rollback_points, 1024, [&writer](const RollbackPoint& value) {
    write_rollback_point(writer, value);
  });
  write_vector<RejectedEvidence>(writer, record.rejected_evidence, 65536,
                                 [&writer](const RejectedEvidence& value) { write_rejected_evidence(writer, value); });
  write_vector<std::string>(writer, record.lifecycle_log, 65536, [&writer](const std::string& value) {
    writer.text(value, 512);
  });
}

Result<ExperimentRecord> read_experiment_record(PayloadReader& reader) {
  ExperimentRecord record;
  auto definition = read_experiment_definition(reader);
  if (!definition.ok()) return definition.status();
  record.definition = std::move(definition.value());
  auto hypothesis = read_hypothesis(reader);
  if (!hypothesis.ok()) return hypothesis.status();
  record.hypothesis = hypothesis.value();

  auto history_count = reader.count(4096);
  if (!history_count.ok()) return history_count.status();
  record.hypothesis_history.reserve(history_count.value());
  for (std::uint32_t index = 0; index < history_count.value(); ++index) {
    auto entry = read_hypothesis(reader);
    if (!entry.ok()) return entry.status();
    record.hypothesis_history.push_back(entry.value());
  }
  auto state = read_enum<ExperimentState>(reader, kMaxExperimentState, "experiment state");
  if (!state.ok()) return state.status();
  record.state = state.value();
  auto revision_sequence = reader.u64();
  if (!revision_sequence.ok()) return revision_sequence.status();
  record.revision_sequence = revision_sequence.value();
  auto created_sequence = reader.u64();
  if (!created_sequence.ok()) return created_sequence.status();
  record.created_sequence = created_sequence.value();

  auto trial_count = reader.count(4096);
  if (!trial_count.ok()) return trial_count.status();
  for (std::uint32_t index = 0; index < trial_count.value(); ++index) {
    auto trial = read_trial(reader);
    if (!trial.ok()) return trial.status();
    const TrialId key = trial.value().id;
    if (!key.valid()) {
      return make_error(ErrorCode::CORRUPT_PERSISTENCE, ErrorStage::PERSISTENCE, "null trial identity");
    }
    if (!record.trials.emplace(key, trial.value()).second) {
      return make_error(ErrorCode::CORRUPT_PERSISTENCE, ErrorStage::PERSISTENCE, "duplicate trial identity");
    }
  }

  auto attempt_count = reader.count(32768);
  if (!attempt_count.ok()) return attempt_count.status();
  for (std::uint32_t index = 0; index < attempt_count.value(); ++index) {
    auto attempt = read_attempt(reader);
    if (!attempt.ok()) return attempt.status();
    const TrialAttemptId key = attempt.value().id;
    if (!key.valid()) {
      return make_error(ErrorCode::CORRUPT_PERSISTENCE, ErrorStage::PERSISTENCE, "null attempt identity");
    }
    if (!record.attempts.emplace(key, attempt.value()).second) {
      return make_error(ErrorCode::CORRUPT_PERSISTENCE, ErrorStage::PERSISTENCE, "duplicate attempt identity");
    }
  }

  auto observation_count = reader.count(1u << 20);
  if (!observation_count.ok()) return observation_count.status();
  record.observations.reserve(observation_count.value());
  for (std::uint32_t index = 0; index < observation_count.value(); ++index) {
    auto observation = read_observation(reader);
    if (!observation.ok()) return observation.status();
    record.observations.push_back(observation.value());
  }

  auto artifact_count = reader.count(65536);
  if (!artifact_count.ok()) return artifact_count.status();
  record.artifacts.reserve(artifact_count.value());
  for (std::uint32_t index = 0; index < artifact_count.value(); ++index) {
    auto artifact = read_artifact(reader);
    if (!artifact.ok()) return artifact.status();
    record.artifacts.push_back(artifact.value());
  }

  auto decision_count = reader.count(1024);
  if (!decision_count.ok()) return decision_count.status();
  record.decisions.reserve(decision_count.value());
  for (std::uint32_t index = 0; index < decision_count.value(); ++index) {
    auto decision = read_decision(reader);
    if (!decision.ok()) return decision.status();
    record.decisions.push_back(decision.value());
  }

  auto rollback_count = reader.count(1024);
  if (!rollback_count.ok()) return rollback_count.status();
  record.rollback_points.reserve(rollback_count.value());
  for (std::uint32_t index = 0; index < rollback_count.value(); ++index) {
    auto point = read_rollback_point(reader);
    if (!point.ok()) return point.status();
    record.rollback_points.push_back(point.value());
  }

  auto rejection_count = reader.count(65536);
  if (!rejection_count.ok()) return rejection_count.status();
  record.rejected_evidence.reserve(rejection_count.value());
  for (std::uint32_t index = 0; index < rejection_count.value(); ++index) {
    auto rejection = read_rejected_evidence(reader);
    if (!rejection.ok()) return rejection.status();
    record.rejected_evidence.push_back(rejection.value());
  }

  auto log_count = reader.count(65536);
  if (!log_count.ok()) return log_count.status();
  record.lifecycle_log.reserve(log_count.value());
  for (std::uint32_t index = 0; index < log_count.value(); ++index) {
    auto line = reader.text(512);
    if (!line.ok()) return line.status();
    record.lifecycle_log.push_back(std::move(line.value()));
  }
  return record;
}

void write_coordinator_state(PayloadWriter& writer, const CoordinatorState& state) {
  writer.u32(state.format_version);
  writer.u64(state.last_epoch);
  writer.u64(state.last_sequence);
  write_vector<WorkerIncarnation>(writer, state.workers, 4096, [&writer](const WorkerIncarnation& value) {
    write_worker_incarnation(writer, value);
  });
  writer.u64(state.next_worker_registration_sequence);
  writer.u64(state.id_watermark_experiment);
  writer.u64(state.id_watermark_hypothesis);
  writer.u64(state.id_watermark_branch);
  writer.u64(state.id_watermark_metric);
  writer.u64(state.id_watermark_policy);
  writer.u64(state.id_watermark_trial);
  writer.u64(state.id_watermark_attempt);
  writer.u64(state.id_watermark_observation);
  writer.u64(state.id_watermark_artifact);
  writer.u64(state.id_watermark_decision);
  writer.u64(state.id_watermark_rollback);
  writer.count(static_cast<std::uint32_t>(state.experiments.size()), 1u << 22);
  for (const auto& entry : state.experiments) {
    write_id(writer, entry.first);
    write_experiment_record(writer, entry.second);
  }
}

Result<CoordinatorState> read_coordinator_state(PayloadReader& reader) {
  CoordinatorState state;
  auto version = reader.u32();
  if (!version.ok()) return version.status();
  state.format_version = version.value();
  auto last_epoch = reader.u64();
  if (!last_epoch.ok()) return last_epoch.status();
  state.last_epoch = last_epoch.value();
  auto last_sequence = reader.u64();
  if (!last_sequence.ok()) return last_sequence.status();
  state.last_sequence = last_sequence.value();

  auto worker_count = reader.count(4096);
  if (!worker_count.ok()) return worker_count.status();
  state.workers.reserve(worker_count.value());
  for (std::uint32_t index = 0; index < worker_count.value(); ++index) {
    auto incarnation = read_worker_incarnation(reader);
    if (!incarnation.ok()) return incarnation.status();
    state.workers.push_back(incarnation.value());
  }
  auto registration_sequence = reader.u64();
  if (!registration_sequence.ok()) return registration_sequence.status();
  state.next_worker_registration_sequence = registration_sequence.value();
  auto watermark = [&reader](std::uint64_t& target) -> Status {
    auto value = reader.u64();
    if (!value.ok()) {
      return value.status();
    }
    target = value.value();
    return Status::success();
  };
  for (std::uint64_t* target : {&state.id_watermark_experiment, &state.id_watermark_hypothesis,
                                &state.id_watermark_branch, &state.id_watermark_metric,
                                &state.id_watermark_policy, &state.id_watermark_trial,
                                &state.id_watermark_attempt, &state.id_watermark_observation,
                                &state.id_watermark_artifact, &state.id_watermark_decision,
                                &state.id_watermark_rollback}) {
    const Status status = watermark(*target);
    if (!status.ok()) {
      return status;
    }
  }

  auto experiment_count = reader.count(1u << 22);
  if (!experiment_count.ok()) return experiment_count.status();
  for (std::uint32_t index = 0; index < experiment_count.value(); ++index) {
    auto id = read_id<ExperimentId>(reader);
    if (!id.ok()) return id.status();
    if (!id.value().valid()) {
      return make_error(ErrorCode::CORRUPT_PERSISTENCE, ErrorStage::PERSISTENCE, "null experiment identity");
    }
    auto record = read_experiment_record(reader);
    if (!record.ok()) return record.status();
    if (!state.experiments.emplace(id.value(), std::move(record.value())).second) {
      return make_error(ErrorCode::CORRUPT_PERSISTENCE, ErrorStage::PERSISTENCE, "duplicate experiment identity");
    }
  }
  return state;
}

// ---------------------------------------------------------------------------
// Typed messages
// ---------------------------------------------------------------------------

namespace {

std::vector<std::uint8_t> finish(PayloadWriter& writer) { return std::move(writer).take(); }

}  // namespace

std::vector<std::uint8_t> encode(const RegisterWorkerRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.claimed_epoch);
  write_id(writer, value.worker);
  write_id(writer, value.boot);
  write_id(writer, value.producer);
  writer.text(value.endpoint, 256);
  writer.u32(value.max_concurrent_assignments);
  writer.text(value.fingerprint, 512);
  return finish(writer);
}

Result<RegisterWorkerRequest> decode_register_worker(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  RegisterWorkerRequest value;
  auto epoch = read_id<CoordinatorEpoch>(reader);
  if (!epoch.ok()) return epoch.status();
  value.claimed_epoch = epoch.value();
  auto worker = read_id<WorkerId>(reader);
  if (!worker.ok()) return worker.status();
  value.worker = worker.value();
  auto boot = read_id<WorkerBootId>(reader);
  if (!boot.ok()) return boot.status();
  value.boot = boot.value();
  auto producer = read_id<ProducerId>(reader);
  if (!producer.ok()) return producer.status();
  value.producer = producer.value();
  auto endpoint = reader.text(256);
  if (!endpoint.ok()) return endpoint.status();
  value.endpoint = std::move(endpoint.value());
  auto max_concurrent = reader.u32();
  if (!max_concurrent.ok()) return max_concurrent.status();
  value.max_concurrent_assignments = max_concurrent.value();
  auto fingerprint = reader.text(512);
  if (!fingerprint.ok()) return fingerprint.status();
  value.fingerprint = std::move(fingerprint.value());
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const RegisterWorkerReply& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.epoch);
  write_id(writer, value.boot);
  write_id(writer, value.producer);
  writer.boolean(value.accepted);
  const std::vector<std::uint8_t> status = encode(value.status, limits);
  writer.u32(static_cast<std::uint32_t>(status.size()));
  writer.raw(status);
  return finish(writer);
}

Result<RegisterWorkerReply> decode_register_reply(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  RegisterWorkerReply value;
  auto epoch = read_id<CoordinatorEpoch>(reader);
  if (!epoch.ok()) return epoch.status();
  value.epoch = epoch.value();
  auto boot = read_id<WorkerBootId>(reader);
  if (!boot.ok()) return boot.status();
  value.boot = boot.value();
  auto producer = read_id<ProducerId>(reader);
  if (!producer.ok()) return producer.status();
  value.producer = producer.value();
  auto accepted = reader.boolean();
  if (!accepted.ok()) return accepted.status();
  value.accepted = accepted.value();
  auto length = reader.count(limits.max_frame_payload_bytes);
  if (!length.ok()) return length.status();
  auto bytes = reader.raw(length.value());
  if (!bytes.ok()) return bytes.status();
  auto status = decode_status(bytes.value(), limits);
  if (!status.ok()) return status.status();
  value.status = status.value().status;
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const ClaimTrialRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.epoch);
  write_id(writer, value.worker);
  write_id(writer, value.boot);
  writer.u32(value.max_claims);
  return finish(writer);
}

Result<ClaimTrialRequest> decode_claim_trial(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  ClaimTrialRequest value;
  auto epoch = read_id<CoordinatorEpoch>(reader);
  if (!epoch.ok()) return epoch.status();
  value.epoch = epoch.value();
  auto worker = read_id<WorkerId>(reader);
  if (!worker.ok()) return worker.status();
  value.worker = worker.value();
  auto boot = read_id<WorkerBootId>(reader);
  if (!boot.ok()) return boot.status();
  value.boot = boot.value();
  auto max_claims = reader.u32();
  if (!max_claims.ok()) return max_claims.status();
  value.max_claims = max_claims.value();
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const ClaimTrialReply& value, const Limits& limits) {
  PayloadWriter writer(limits);
  const std::vector<std::uint8_t> status = encode(value.status, limits);
  writer.u32(static_cast<std::uint32_t>(status.size()));
  writer.raw(status);
  write_vector<TrialAssignment>(writer, value.assignments, limits.max_batch_items, [&writer](const TrialAssignment& assignment) {
    write_envelope(writer, assignment.envelope);
    writer.text(assignment.trial_payload, 4096);
    write_parameters(writer, assignment.parameters);
    write_id(writer, assignment.attempt_number);
    write_vector<AssignedMetric>(writer, assignment.metrics, 128, [&writer](const AssignedMetric& metric) {
      write_id(writer, metric.metric);
      write_id(writer, metric.observation_id);
      writer.text(metric.name, 128);
      write_enum(writer, metric.kind);
      write_enum(writer, metric.direction);
      writer.boolean(metric.required);
    });
    write_vector<ArtifactId>(writer, assignment.artifact_ids, 256,
                             [&writer](const ArtifactId& id) { write_id(writer, id); });
    writer.u32(assignment.planned_observations);
  });
  return finish(writer);
}

Result<ClaimTrialReply> decode_claim_reply(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  ClaimTrialReply value;
  auto status_length = reader.count(limits.max_frame_payload_bytes);
  if (!status_length.ok()) return status_length.status();
  auto status_bytes = reader.raw(status_length.value());
  if (!status_bytes.ok()) return status_bytes.status();
  auto status = decode_status(status_bytes.value(), limits);
  if (!status.ok()) return status.status();
  value.status = status.value().status;
  auto count = reader.count(limits.max_batch_items);
  if (!count.ok()) return count.status();
  value.assignments.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    TrialAssignment assignment;
    auto envelope = read_envelope(reader);
    if (!envelope.ok()) return envelope.status();
    assignment.envelope = envelope.value();
    auto trial_payload = reader.text(4096);
    if (!trial_payload.ok()) return trial_payload.status();
    assignment.trial_payload = std::move(trial_payload.value());
    auto parameters = read_parameters(reader);
    if (!parameters.ok()) return parameters.status();
    assignment.parameters = std::move(parameters.value());
    auto attempt_number = read_id<TrialAttemptNumber>(reader);
    if (!attempt_number.ok()) return attempt_number.status();
    assignment.attempt_number = attempt_number.value();
    auto metric_count = reader.count(128);
    if (!metric_count.ok()) return metric_count.status();
    assignment.metrics.reserve(metric_count.value());
    for (std::uint32_t metric_index = 0; metric_index < metric_count.value(); ++metric_index) {
      AssignedMetric metric;
      auto metric_id = read_id<MetricId>(reader);
      if (!metric_id.ok()) return metric_id.status();
      metric.metric = metric_id.value();
      auto observation_id = read_id<ObservationId>(reader);
      if (!observation_id.ok()) return observation_id.status();
      metric.observation_id = observation_id.value();
      auto name = reader.text(128);
      if (!name.ok()) return name.status();
      metric.name = std::move(name.value());
      auto kind = read_enum<MetricKind>(reader, kMaxMetricKind, "metric kind");
      if (!kind.ok()) return kind.status();
      metric.kind = kind.value();
      auto direction = read_enum<MetricDirection>(reader, kMaxMetricDirection, "metric direction");
      if (!direction.ok()) return direction.status();
      metric.direction = direction.value();
      auto required = reader.boolean();
      if (!required.ok()) return required.status();
      metric.required = required.value();
      assignment.metrics.push_back(std::move(metric));
    }
    auto artifact_count = reader.count(256);
    if (!artifact_count.ok()) return artifact_count.status();
    assignment.artifact_ids.reserve(artifact_count.value());
    for (std::uint32_t artifact_index = 0; artifact_index < artifact_count.value(); ++artifact_index) {
      auto artifact_id = read_id<ArtifactId>(reader);
      if (!artifact_id.ok()) return artifact_id.status();
      assignment.artifact_ids.push_back(artifact_id.value());
    }
    auto planned = reader.u32();
    if (!planned.ok()) return planned.status();
    assignment.planned_observations = planned.value();
    value.assignments.push_back(std::move(assignment));
  }
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const PublishObservationRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.epoch);
  write_id(writer, value.worker);
  write_id(writer, value.boot);
  write_observation(writer, value.observation);
  return finish(writer);
}

Result<PublishObservationRequest> decode_publish_observation(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  PublishObservationRequest value;
  auto epoch = read_id<CoordinatorEpoch>(reader);
  if (!epoch.ok()) return epoch.status();
  value.epoch = epoch.value();
  auto worker = read_id<WorkerId>(reader);
  if (!worker.ok()) return worker.status();
  value.worker = worker.value();
  auto boot = read_id<WorkerBootId>(reader);
  if (!boot.ok()) return boot.status();
  value.boot = boot.value();
  auto observation = read_observation(reader);
  if (!observation.ok()) return observation.status();
  value.observation = observation.value();
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const PublishArtifactRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.epoch);
  write_id(writer, value.worker);
  write_id(writer, value.boot);
  write_artifact(writer, value.artifact);
  return finish(writer);
}

Result<PublishArtifactRequest> decode_publish_artifact(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  PublishArtifactRequest value;
  auto epoch = read_id<CoordinatorEpoch>(reader);
  if (!epoch.ok()) return epoch.status();
  value.epoch = epoch.value();
  auto worker = read_id<WorkerId>(reader);
  if (!worker.ok()) return worker.status();
  value.worker = worker.value();
  auto boot = read_id<WorkerBootId>(reader);
  if (!boot.ok()) return boot.status();
  value.boot = boot.value();
  auto artifact = read_artifact(reader);
  if (!artifact.ok()) return artifact.status();
  value.artifact = artifact.value();
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const CommitTrialRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.epoch);
  write_id(writer, value.worker);
  write_id(writer, value.boot);
  write_envelope(writer, value.envelope);
  write_vector<ArtifactRecord>(writer, value.artifacts, 256, [&writer](const ArtifactRecord& artifact) {
    write_artifact(writer, artifact);
  });
  writer.text(value.result_payload, 4096);
  return finish(writer);
}

Result<CommitTrialRequest> decode_commit_trial(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  CommitTrialRequest value;
  auto epoch = read_id<CoordinatorEpoch>(reader);
  if (!epoch.ok()) return epoch.status();
  value.epoch = epoch.value();
  auto worker = read_id<WorkerId>(reader);
  if (!worker.ok()) return worker.status();
  value.worker = worker.value();
  auto boot = read_id<WorkerBootId>(reader);
  if (!boot.ok()) return boot.status();
  value.boot = boot.value();
  auto envelope = read_envelope(reader);
  if (!envelope.ok()) return envelope.status();
  value.envelope = envelope.value();
  auto count = reader.count(256);
  if (!count.ok()) return count.status();
  value.artifacts.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    auto artifact = read_artifact(reader);
    if (!artifact.ok()) return artifact.status();
    value.artifacts.push_back(artifact.value());
  }
  auto payload_text = reader.text(4096);
  if (!payload_text.ok()) return payload_text.status();
  value.result_payload = std::move(payload_text.value());
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const FailTrialRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.epoch);
  write_id(writer, value.worker);
  write_id(writer, value.boot);
  write_envelope(writer, value.envelope);
  write_enum(writer, value.kind);
  writer.text(value.detail, 512);
  writer.boolean(value.retryable);
  return finish(writer);
}

Result<FailTrialRequest> decode_fail_trial(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  FailTrialRequest value;
  auto epoch = read_id<CoordinatorEpoch>(reader);
  if (!epoch.ok()) return epoch.status();
  value.epoch = epoch.value();
  auto worker = read_id<WorkerId>(reader);
  if (!worker.ok()) return worker.status();
  value.worker = worker.value();
  auto boot = read_id<WorkerBootId>(reader);
  if (!boot.ok()) return boot.status();
  value.boot = boot.value();
  auto envelope = read_envelope(reader);
  if (!envelope.ok()) return envelope.status();
  value.envelope = envelope.value();
  auto kind = read_enum<FailureKind>(reader, kMaxFailureKind, "failure kind");
  if (!kind.ok()) return kind.status();
  value.kind = kind.value();
  auto detail = reader.text(512);
  if (!detail.ok()) return detail.status();
  value.detail = std::move(detail.value());
  auto retryable = reader.boolean();
  if (!retryable.ok()) return retryable.status();
  value.retryable = retryable.value();
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const CancelTrialRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.trial);
  writer.text(value.reason, 512);
  return finish(writer);
}

Result<CancelTrialRequest> decode_cancel_trial(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  CancelTrialRequest value;
  auto trial = read_id<TrialId>(reader);
  if (!trial.ok()) return trial.status();
  value.trial = trial.value();
  auto reason = reader.text(512);
  if (!reason.ok()) return reason.status();
  value.reason = std::move(reason.value());
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const CancelExperimentRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.experiment);
  writer.text(value.reason, 512);
  return finish(writer);
}

Result<CancelExperimentRequest> decode_cancel_experiment(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  CancelExperimentRequest value;
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  value.experiment = experiment.value();
  auto reason = reader.text(512);
  if (!reason.ok()) return reason.status();
  value.reason = std::move(reason.value());
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const FinalizeExperimentRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.experiment);
  write_id(writer, value.candidate);
  writer.text(value.reason, 512);
  return finish(writer);
}

Result<FinalizeExperimentRequest> decode_finalize_experiment(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  FinalizeExperimentRequest value;
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  value.experiment = experiment.value();
  auto candidate = read_id<BranchId>(reader);
  if (!candidate.ok()) return candidate.status();
  value.candidate = candidate.value();
  auto reason = reader.text(512);
  if (!reason.ok()) return reason.status();
  value.reason = std::move(reason.value());
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const RollbackRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.experiment);
  write_id(writer, value.target_generation);
  writer.text(value.reason, 512);
  return finish(writer);
}

Result<RollbackRequest> decode_rollback(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  RollbackRequest value;
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  value.experiment = experiment.value();
  auto target = read_id<ExperimentGeneration>(reader);
  if (!target.ok()) return target.status();
  value.target_generation = target.value();
  auto reason = reader.text(512);
  if (!reason.ok()) return reason.status();
  value.reason = std::move(reason.value());
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const HeartbeatRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.epoch);
  write_id(writer, value.worker);
  write_id(writer, value.boot);
  writer.u32(value.active_assignments);
  return finish(writer);
}

Result<HeartbeatRequest> decode_heartbeat(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  HeartbeatRequest value;
  auto epoch = read_id<CoordinatorEpoch>(reader);
  if (!epoch.ok()) return epoch.status();
  value.epoch = epoch.value();
  auto worker = read_id<WorkerId>(reader);
  if (!worker.ok()) return worker.status();
  value.worker = worker.value();
  auto boot = read_id<WorkerBootId>(reader);
  if (!boot.ok()) return boot.status();
  value.boot = boot.value();
  auto active = reader.u32();
  if (!active.ok()) return active.status();
  value.active_assignments = active.value();
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const Status& value, const Limits& limits) {
  PayloadWriter writer(limits);
  writer.u8(static_cast<std::uint8_t>(value.code()));
  writer.u8(static_cast<std::uint8_t>(value.stage()));
  writer.text(value.reason(), 1024);
  return finish(writer);
}

StatusResult decode_status(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  auto code = reader.u8();
  if (!code.ok()) return code.status();
  auto stage = reader.u8();
  if (!stage.ok()) return stage.status();
  if (stage.value() > static_cast<std::uint8_t>(ErrorStage::LIMITS)) {
    return protocol_error("impossible error stage value");
  }
  auto reason = reader.text(1024);
  if (!reason.ok()) return reason.status();
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  if (code.value() > static_cast<std::uint8_t>(ErrorCode::INTERNAL)) {
    return protocol_error("impossible error code value");
  }
  StatusPayload outcome;
  outcome.status =
      Status(static_cast<ErrorCode>(code.value()), static_cast<ErrorStage>(stage.value()), std::move(reason.value()));
  return outcome;
}

std::vector<std::uint8_t> encode(const AckReply& value, const Limits& limits) {
  return encode(value.status, limits);
}

Result<AckReply> decode_ack(std::span<const std::uint8_t> payload, const Limits& limits) {
  auto status = decode_status(payload, limits);
  if (!status.ok()) {
    return status.status();
  }
  AckReply reply;
  reply.status = status.value().status;
  return reply;
}

std::vector<std::uint8_t> encode(const ErrorReply& value, const Limits& limits) {
  PayloadWriter writer(limits);
  const std::vector<std::uint8_t> status = encode(value.status, limits);
  writer.u32(static_cast<std::uint32_t>(status.size()));
  writer.raw(status);
  writer.text(value.detail, 1024);
  return finish(writer);
}

std::vector<std::uint8_t> encode(const QueryResult& value, const Limits& limits) {
  PayloadWriter writer(limits);
  const std::vector<std::uint8_t> status = encode(value.status, limits);
  writer.u32(static_cast<std::uint32_t>(status.size()));
  writer.raw(status);
  write_vector<std::string>(writer, value.lines, 65536,
                            [&writer](const std::string& line) { writer.text(line, 512); });
  return finish(writer);
}

Result<QueryResult> decode_query_result(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  QueryResult value;
  auto length = reader.count(limits.max_frame_payload_bytes);
  if (!length.ok()) return length.status();
  auto bytes = reader.raw(length.value());
  if (!bytes.ok()) return bytes.status();
  auto status = decode_status(bytes.value(), limits);
  if (!status.ok()) return status.status();
  value.status = status.value().status;
  auto count = reader.count(65536);
  if (!count.ok()) return count.status();
  value.lines.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    auto line = reader.text(512);
    if (!line.ok()) return line.status();
    value.lines.push_back(std::move(line.value()));
  }
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const ValueReply& value, const Limits& limits) {
  PayloadWriter writer(limits);
  const std::vector<std::uint8_t> status = encode(value.status, limits);
  writer.u32(static_cast<std::uint32_t>(status.size()));
  writer.raw(status);
  writer.u64(value.value);
  return finish(writer);
}

Result<ValueReply> decode_value_reply(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  ValueReply value;
  auto length = reader.count(limits.max_frame_payload_bytes);
  if (!length.ok()) return length.status();
  auto bytes = reader.raw(length.value());
  if (!bytes.ok()) return bytes.status();
  auto status = decode_status(bytes.value(), limits);
  if (!status.ok()) return status.status();
  value.status = status.value().status;
  auto raw = reader.u64();
  if (!raw.ok()) return raw.status();
  value.value = raw.value();
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode_decision_reply(const Status& status, const Decision& decision,
                                                const Limits& limits) {
  PayloadWriter writer(limits);
  const std::vector<std::uint8_t> status_bytes = encode(status, limits);
  writer.u32(static_cast<std::uint32_t>(status_bytes.size()));
  writer.raw(status_bytes);
  write_decision(writer, decision);
  return finish(writer);
}

Result<std::pair<Status, Decision>> decode_decision_reply(std::span<const std::uint8_t> payload,
                                                          const Limits& limits) {
  PayloadReader reader(payload, limits);
  auto length = reader.count(limits.max_frame_payload_bytes);
  if (!length.ok()) return length.status();
  auto bytes = reader.raw(length.value());
  if (!bytes.ok()) return bytes.status();
  auto status = decode_status(bytes.value(), limits);
  if (!status.ok()) return status.status();
  auto decision = read_decision(reader);
  if (!decision.ok()) return decision.status();
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return std::make_pair(status.value().status, decision.value());
}

std::vector<std::uint8_t> encode(const CreateTrialRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.experiment);
  write_id(writer, value.branch);
  writer.text(value.payload, 4096);
  return finish(writer);
}

Result<CreateTrialRequest> decode_create_trial(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  CreateTrialRequest value;
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  value.experiment = experiment.value();
  auto branch = read_id<BranchId>(reader);
  if (!branch.ok()) return branch.status();
  value.branch = branch.value();
  auto text = reader.text(4096);
  if (!text.ok()) return text.status();
  value.payload = std::move(text.value());
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const ForkBranchRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.experiment);
  write_id(writer, value.parent);
  writer.text(value.name, 128);
  write_enum(writer, value.role);
  write_parameters(writer, value.parameters);
  write_optional_u64(writer, value.seed);
  writer.u32(value.planned_trials);
  return finish(writer);
}

Result<ForkBranchRequest> decode_fork_branch(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  ForkBranchRequest value;
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  value.experiment = experiment.value();
  auto parent = read_id<BranchId>(reader);
  if (!parent.ok()) return parent.status();
  value.parent = parent.value();
  auto name = reader.text(128);
  if (!name.ok()) return name.status();
  value.name = std::move(name.value());
  auto role = read_enum<BranchRole>(reader, kMaxBranchRole, "branch role");
  if (!role.ok()) return role.status();
  value.role = role.value();
  auto parameters = read_parameters(reader);
  if (!parameters.ok()) return parameters.status();
  value.parameters = std::move(parameters.value());
  auto seed = read_optional_u64(reader);
  if (!seed.ok()) return seed.status();
  value.seed = seed.value();
  auto planned = reader.u32();
  if (!planned.ok()) return planned.status();
  value.planned_trials = planned.value();
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode(const ReviseHypothesisRequest& value, const Limits& limits) {
  PayloadWriter writer(limits);
  write_id(writer, value.experiment);
  writer.text(value.statement, 4096);
  writer.boolean(value.has_direction);
  write_enum(writer, value.direction);
  write_id(writer, value.metric);
  writer.text(value.provenance, 1024);
  return finish(writer);
}

Result<ReviseHypothesisRequest> decode_revise_hypothesis(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  ReviseHypothesisRequest value;
  auto experiment = read_id<ExperimentId>(reader);
  if (!experiment.ok()) return experiment.status();
  value.experiment = experiment.value();
  auto statement = reader.text(4096);
  if (!statement.ok()) return statement.status();
  value.statement = std::move(statement.value());
  auto has_direction = reader.boolean();
  if (!has_direction.ok()) return has_direction.status();
  value.has_direction = has_direction.value();
  auto direction = read_enum<MetricDirection>(reader, kMaxMetricDirection, "metric direction");
  if (!direction.ok()) return direction.status();
  value.direction = direction.value();
  auto metric = read_id<MetricId>(reader);
  if (!metric.ok()) return metric.status();
  value.metric = metric.value();
  auto provenance = reader.text(1024);
  if (!provenance.ok()) return provenance.status();
  value.provenance = std::move(provenance.value());
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

std::vector<std::uint8_t> encode_query_request(QueryKind kind, std::string_view argument, const Limits& limits) {
  PayloadWriter writer(limits);
  writer.u16(static_cast<std::uint16_t>(kind));
  writer.text(argument, 128);
  return finish(writer);
}

Result<std::pair<QueryKind, std::string>> decode_query_request(std::span<const std::uint8_t> payload,
                                                              const Limits& limits) {
  PayloadReader reader(payload, limits);
  auto kind = reader.u16();
  if (!kind.ok()) return kind.status();
  if (kind.value() == 0 || kind.value() > static_cast<std::uint16_t>(QueryKind::WORKERS)) {
    return protocol_error("unknown query kind");
  }
  auto argument = reader.text(128);
  if (!argument.ok()) return argument.status();
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return std::make_pair(static_cast<QueryKind>(kind.value()), std::move(argument.value()));
}

Result<std::vector<std::string>> unwrap_query_lines(const QueryResult& reply) {
  if (reply.status.failed()) {
    return reply.status;
  }
  if (reply.lines.size() > Limits::defaults().max_explanation_lines) {
    return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS, "query reply returns too many lines");
  }
  return reply.lines;
}

Result<ErrorReply> decode_error_reply(std::span<const std::uint8_t> payload, const Limits& limits) {
  PayloadReader reader(payload, limits);
  ErrorReply value;
  auto length = reader.count(limits.max_frame_payload_bytes);
  if (!length.ok()) return length.status();
  auto bytes = reader.raw(length.value());
  if (!bytes.ok()) return bytes.status();
  auto status = decode_status(bytes.value(), limits);
  if (!status.ok()) return status.status();
  value.status = status.value().status;
  auto detail = reader.text(1024);
  if (!detail.ok()) return detail.status();
  value.detail = std::move(detail.value());
  const Status end = reader.expect_end();
  if (!end.ok()) return end;
  return value;
}

}  // namespace experiment_fabric
