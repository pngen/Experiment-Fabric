// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "experiment_fabric/protocol.hpp"
#include "test_support.hpp"

namespace {
namespace ef = experiment_fabric;

ef::Limits default_limits() { return ef::Limits::defaults(); }

std::vector<std::uint8_t> bytes_of(const std::string& text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}
}  // namespace

EF_TEST(protocol, frame_round_trip) {
  ef::Frame frame;
  frame.type = ef::MessageType::HEARTBEAT;
  frame.correlation = 4242;
  frame.payload = {1, 2, 3, 4, 5};
  const auto encoded = ef::encode_frame(frame, default_limits());
  EF_REQUIRE(encoded.ok());
  EF_CHECK_EQ(encoded.value().size(), std::size_t{ef::kFrameHeaderBytes + 5});
  const auto decoded = ef::decode_frame(encoded.value(), default_limits());
  EF_REQUIRE(decoded.status.ok());
  EF_CHECK(decoded.frame.type == ef::MessageType::HEARTBEAT);
  EF_CHECK_EQ(decoded.frame.correlation, 4242ull);
  EF_CHECK(decoded.frame.payload == frame.payload);
  EF_CHECK_EQ(decoded.consumed, encoded.value().size());
}

EF_TEST(protocol, malformed_headers_are_refused) {
  ef::Frame frame;
  frame.type = ef::MessageType::QUERY;
  frame.correlation = 1;
  frame.payload = bytes_of("abc");
  const auto encoded = ef::encode_frame(frame, default_limits());
  EF_REQUIRE(encoded.ok());
  const std::vector<std::uint8_t>& image = encoded.value();

  EF_CHECK(!ef::decode_frame(std::span<const std::uint8_t>(image.data(), 10), default_limits()).status.ok());

  for (std::size_t offset = 0; offset < ef::kFrameHeaderBytes; ++offset) {
    std::vector<std::uint8_t> corrupted = image;
    corrupted[offset] = static_cast<std::uint8_t>(corrupted[offset] ^ 0x80u);
    const auto decoded = ef::decode_frame(corrupted, default_limits());
    EF_CHECK_MESSAGE(!decoded.status.ok(), "header byte " + std::to_string(offset) + " was accepted");
  }
}

EF_TEST(protocol, oversized_declared_payload_is_refused_before_allocation) {
  ef::Limits limits = default_limits();
  limits.max_frame_payload_bytes = 64;
  ef::Frame frame;
  frame.type = ef::MessageType::QUERY;
  frame.payload.assign(200, 0);
  const auto encoded = ef::encode_frame(frame, limits);
  EF_CHECK(!encoded.ok());
  EF_CHECK_EQ(encoded.status().code(), ef::ErrorCode::RESOURCE_EXHAUSTED);

  // A header that declares an impossible length is refused without allocating.
  ef::Frame small;
  small.type = ef::MessageType::QUERY;
  small.payload = {7};
  std::vector<std::uint8_t> image = ef::encode_frame(small, default_limits()).value();
  for (int index = 0; index < 4; ++index) {
    image[16 + static_cast<std::size_t>(index)] = 0xFFu;
  }
  image[24] = 0;  // invalidate the header checksum on purpose
  const auto decoded = ef::decode_frame(image, default_limits());
  EF_CHECK(!decoded.status.ok());
}

EF_TEST(protocol, impossible_message_type_is_refused) {
  ef::Frame frame;
  frame.type = ef::MessageType::QUERY;
  frame.payload = {1};
  std::vector<std::uint8_t> image = ef::encode_frame(frame, default_limits()).value();
  image[6] = 0xEEu;
  image[7] = 0xEEu;
  // Recompute the header checksum so that only the type is wrong.
  const std::uint32_t checksum = ef::crc32(std::span<const std::uint8_t>(image.data(), 24));
  for (int index = 0; index < 4; ++index) {
    image[24 + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>((checksum >> (8 * index)) & 0xFFu);
  }
  const auto decoded = ef::decode_frame(image, default_limits());
  EF_CHECK(!decoded.status.ok());
  EF_CHECK_EQ(decoded.status.code(), ef::ErrorCode::PROTOCOL_ERROR);
}

EF_TEST(protocol, payload_checksum_mismatch_is_refused) {
  ef::Frame frame;
  frame.type = ef::MessageType::QUERY;
  frame.payload = {9, 9, 9};
  std::vector<std::uint8_t> image = ef::encode_frame(frame, default_limits()).value();
  image[ef::kFrameHeaderBytes] ^= 0xFFu;
  const auto decoded = ef::decode_frame(image, default_limits());
  EF_CHECK(!decoded.status.ok());
}

EF_TEST(protocol, trailing_payload_bytes_are_refused_by_every_decoder) {
  ef::CancelTrialRequest request;
  request.trial = ef::TrialId::from_value(1);
  request.reason = "x";
  std::vector<std::uint8_t> payload = ef::encode(request, default_limits());
  payload.push_back(0);
  const auto decoded = ef::decode_cancel_trial(payload, default_limits());
  EF_CHECK(!decoded.ok());
  EF_CHECK_EQ(decoded.status().code(), ef::ErrorCode::PROTOCOL_ERROR);

  ef::HeartbeatRequest heartbeat;
  heartbeat.epoch = ef::CoordinatorEpoch::from_value(1);
  heartbeat.worker = ef::WorkerId::from_value(1);
  heartbeat.boot = ef::WorkerBootId::from_value(1);
  std::vector<std::uint8_t> heartbeat_bytes = ef::encode(heartbeat, default_limits());
  heartbeat_bytes.push_back(0xAB);
  EF_CHECK(!ef::decode_heartbeat(heartbeat_bytes, default_limits()).ok());
}

EF_TEST(protocol, truncated_payload_is_refused) {
  ef::RegisterWorkerRequest request;
  request.worker = ef::WorkerId::from_value(3);
  request.boot = ef::WorkerBootId::from_value(4);
  request.endpoint = "endpoint";
  request.fingerprint = "fingerprint";
  const std::vector<std::uint8_t> payload = ef::encode(request, default_limits());
  for (std::size_t cut = 0; cut + 1 < payload.size(); ++cut) {
    const auto decoded =
        ef::decode_register_worker(std::span<const std::uint8_t>(payload.data(), cut), default_limits());
    EF_CHECK(!decoded.ok());
  }
}

EF_TEST(protocol, declared_string_length_cannot_exceed_the_bound) {
  // Hand-build a payload whose declared string length is absurd.
  ef::PayloadWriter writer(default_limits());
  writer.u8(0);
  writer.u64(1);
  writer.u64(2);
  writer.u64(3);
  writer.u64(4);
  writer.u32(0xFFFFFFF0u);
  const std::vector<std::uint8_t> payload = std::move(writer).take();
  const auto decoded = ef::decode_register_worker(payload, default_limits());
  EF_CHECK(!decoded.ok());
}

EF_TEST(protocol, impossible_enum_values_are_refused) {
  ef::Observation observation;
  observation.id = ef::ObservationId::from_value(1);
  observation.experiment = ef::ExperimentId::from_value(1);
  observation.generation = ef::ExperimentGeneration::from_value(1);
  observation.branch = ef::BranchId::from_value(1);
  observation.branch_generation = ef::BranchGeneration::from_value(1);
  observation.trial = ef::TrialId::from_value(1);
  observation.attempt = ef::TrialAttemptId::from_value(1);
  observation.metric = ef::MetricId::from_value(1);
  observation.producer = ef::ProducerId::from_value(1);
  observation.worker = ef::WorkerId::from_value(1);
  observation.boot = ef::WorkerBootId::from_value(1);
  observation.epoch = ef::CoordinatorEpoch::from_value(1);
  observation.validity = ef::ObservationValidity::INVALID;
  observation.value.kind = ef::MetricKind::REAL;
  observation.value.real = 1.0;
  ef::PayloadWriter writer(default_limits());
  ef::write_observation(writer, observation);
  std::vector<std::uint8_t> payload = std::move(writer).take();
  // The validity byte sits after twelve identities, one of which is the epoch.
  const std::size_t validity_offset = 8 * 12;
  EF_REQUIRE(payload.size() > validity_offset);
  payload[validity_offset] = 0x7Fu;
  ef::PayloadReader reader(payload, default_limits());
  const auto decoded = ef::read_observation(reader);
  EF_CHECK(!decoded.ok());
}

EF_TEST(protocol, authority_envelope_codec_round_trip) {
  ef::AuthorityEnvelope envelope;
  envelope.epoch = ef::CoordinatorEpoch::from_value(9);
  envelope.experiment = ef::ExperimentId::from_value(8);
  envelope.generation = ef::ExperimentGeneration::from_value(7);
  envelope.hypothesis = ef::HypothesisId::from_value(6);
  envelope.hypothesis_revision = ef::HypothesisRevision::from_value(5);
  envelope.policy = ef::PolicyId::from_value(4);
  envelope.policy_generation = ef::PolicyGeneration::from_value(3);
  envelope.branch = ef::BranchId::from_value(2);
  envelope.branch_generation = ef::BranchGeneration::from_value(1);
  envelope.trial = ef::TrialId::from_value(11);
  envelope.attempt = ef::TrialAttemptId::from_value(12);
  envelope.worker = ef::WorkerId::from_value(13);
  envelope.boot = ef::WorkerBootId::from_value(14);
  envelope.producer = ef::ProducerId::from_value(15);
  EF_CHECK(ef::validate_envelope_structure(envelope).accepted);
  ef::PayloadWriter writer(default_limits());
  ef::write_envelope(writer, envelope);
  const std::vector<std::uint8_t> payload = std::move(writer).take();
  ef::PayloadReader reader(payload, default_limits());
  const auto decoded = ef::read_envelope(reader);
  EF_REQUIRE(decoded.ok());
  EF_CHECK(decoded.value() == envelope);
}

EF_TEST(protocol, a_null_identity_in_an_envelope_is_never_authority) {
  ef::AuthorityEnvelope envelope;
  envelope.epoch = ef::CoordinatorEpoch::from_value(1);
  envelope.experiment = ef::ExperimentId::from_value(1);
  envelope.generation = ef::ExperimentGeneration::from_value(1);
  envelope.hypothesis = ef::HypothesisId::from_value(1);
  envelope.hypothesis_revision = ef::HypothesisRevision::from_value(1);
  envelope.policy = ef::PolicyId::from_value(1);
  envelope.policy_generation = ef::PolicyGeneration::from_value(1);
  envelope.branch = ef::BranchId::from_value(1);
  envelope.branch_generation = ef::BranchGeneration::from_value(1);
  envelope.trial = ef::TrialId::from_value(1);
  envelope.worker = ef::WorkerId::from_value(1);
  envelope.boot = ef::WorkerBootId::from_value(1);
  envelope.producer = ef::ProducerId::from_value(1);
  const ef::AuthorityVerdict with_null_attempt = ef::validate_envelope_structure(envelope);
  EF_CHECK(!with_null_attempt.accepted);
  EF_CHECK_EQ(with_null_attempt.code, ef::ErrorCode::INVALID_ARGUMENT);
  envelope.attempt = ef::TrialAttemptId::from_value(1);
  const ef::AuthorityVerdict complete = ef::validate_envelope_structure(envelope);
  EF_CHECK(complete.accepted);
  EF_CHECK(complete.code == ef::ErrorCode::OK);
}

EF_TEST(protocol, query_kinds_round_trip_and_reject_unknown_values) {
  for (std::uint16_t value = 1; value <= static_cast<std::uint16_t>(ef::QueryKind::WORKERS); ++value) {
    const std::vector<std::uint8_t> payload =
        ef::encode_query_request(static_cast<ef::QueryKind>(value), "1:2", default_limits());
    const auto decoded = ef::decode_query_request(payload, default_limits());
    EF_REQUIRE(decoded.ok());
    EF_CHECK_EQ(static_cast<std::uint16_t>(decoded.value().first), value);
    EF_CHECK_EQ(decoded.value().second, std::string("1:2"));
  }
  ef::PayloadWriter writer(default_limits());
  writer.u16(9999);
  writer.text("x", 4);
  const std::vector<std::uint8_t> payload = std::move(writer).take();
  EF_CHECK(!ef::decode_query_request(payload, default_limits()).ok());
}

EF_TEST(protocol, assignment_metrics_are_reserved_by_the_coordinator) {
  ef::TrialAssignment assignment;
  assignment.envelope.epoch = ef::CoordinatorEpoch::from_value(1);
  assignment.envelope.experiment = ef::ExperimentId::from_value(1);
  assignment.envelope.generation = ef::ExperimentGeneration::from_value(1);
  assignment.envelope.hypothesis = ef::HypothesisId::from_value(1);
  assignment.envelope.hypothesis_revision = ef::HypothesisRevision::from_value(1);
  assignment.envelope.policy = ef::PolicyId::from_value(1);
  assignment.envelope.policy_generation = ef::PolicyGeneration::from_value(1);
  assignment.envelope.branch = ef::BranchId::from_value(1);
  assignment.envelope.branch_generation = ef::BranchGeneration::from_value(1);
  assignment.envelope.trial = ef::TrialId::from_value(1);
  assignment.envelope.attempt = ef::TrialAttemptId::from_value(1);
  assignment.envelope.worker = ef::WorkerId::from_value(1);
  assignment.envelope.boot = ef::WorkerBootId::from_value(1);
  assignment.envelope.producer = ef::ProducerId::from_value(1);
  assignment.attempt_number = ef::TrialAttemptNumber::from_value(2);
  assignment.trial_payload = "payload";
  ef::AssignedMetric metric;
  metric.metric = ef::MetricId::from_value(5);
  metric.observation_id = ef::ObservationId::from_value(6);
  metric.name = "latency_ms";
  assignment.metrics.push_back(metric);
  assignment.artifact_ids.push_back(ef::ArtifactId::from_value(7));
  ef::ClaimTrialReply reply;
  reply.status = ef::Status::success();
  reply.assignments.push_back(assignment);
  const std::vector<std::uint8_t> payload = ef::encode(reply, default_limits());
  const auto decoded = ef::decode_claim_reply(payload, default_limits());
  EF_REQUIRE(decoded.ok());
  EF_REQUIRE(decoded.value().assignments.size() == 1);
  EF_CHECK(decoded.value().assignments[0].metrics[0].observation_id == metric.observation_id);
  EF_CHECK_EQ(decoded.value().assignments[0].attempt_number.value(), 2ull);
}
