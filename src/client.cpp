// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/client.hpp"

#include <utility>

namespace experiment_fabric {

Client::Client(Config config) : limits_(config.limits), outbound_observer_(std::move(config.outbound_observer)) {
  auto connection = TcpConnection::connect(config.host, config.port);
  if (connection.ok()) {
    connection.value()->set_no_delay(true);
    connection_ = std::move(connection.value());
  }
}

Client::~Client() { (void)close(); }

Result<std::unique_ptr<Client>> Client::connect(Config config) {
  std::unique_ptr<Client> client(new Client(std::move(config)));
  if (client->connection_ == nullptr) {
    return make_error(ErrorCode::NOT_FOUND, ErrorStage::TRANSPORT, "cannot connect to the coordinator");
  }
  return client;
}

Status Client::close() {
  if (connection_ != nullptr) {
    connection_->close();
    connection_.reset();
  }
  return Status::success();
}

Result<Frame> Client::request(MessageType type, std::span<const std::uint8_t> payload) {
  if (connection_ == nullptr) {
    return make_error(ErrorCode::NOT_FOUND, ErrorStage::TRANSPORT, "client is not connected");
  }
  Frame frame;
  frame.type = type;
  frame.correlation = next_correlation();
  frame.payload.assign(payload.begin(), payload.end());
  auto encoded = encode_frame(frame, limits_);
  if (!encoded.ok()) {
    return encoded.status();
  }
  if (outbound_observer_) {
    outbound_observer_(encoded.value());
  }
  const Status sent = connection_->send_all(encoded.value());
  if (!sent.ok()) {
    return sent;
  }
  auto reply = connection_->receive_frame(limits_);
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().correlation != frame.correlation) {
    return make_error(ErrorCode::PROTOCOL_ERROR, ErrorStage::TRANSPORT,
                      "reply correlation does not match the outstanding request");
  }
  if (reply.value().type == MessageType::ERROR_REPLY) {
    auto decoded = decode_error_reply(reply.value().payload, limits_);
    if (!decoded.ok()) {
      return decoded.status();
    }
    return decoded.value().status;
  }
  return reply;
}

Result<RegisterWorkerReply> Client::register_worker(const RegisterWorkerRequest& value) {
  auto reply = request(MessageType::REGISTER_WORKER, encode(value, limits_));
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::REGISTER_ACK) {
    return make_error(ErrorCode::PROTOCOL_ERROR, ErrorStage::TRANSPORT, "unexpected reply type for registration");
  }
  return decode_register_reply(reply.value().payload, limits_);
}

Result<ClaimTrialReply> Client::claim_trials(const ClaimTrialRequest& value) {
  auto reply = request(MessageType::CLAIM_TRIAL, encode(value, limits_));
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::CLAIM_RESULT) {
    return make_error(ErrorCode::PROTOCOL_ERROR, ErrorStage::TRANSPORT, "unexpected reply type for claim");
  }
  return decode_claim_reply(reply.value().payload, limits_);
}

namespace {

Status decode_ack(const Frame& frame, const Limits& limits) {
  if (frame.type != MessageType::ACK) {
    return make_error(ErrorCode::PROTOCOL_ERROR, ErrorStage::TRANSPORT, "unexpected reply type for acknowledge");
  }
  auto ack = decode_ack(frame.payload, limits);
  if (!ack.ok()) {
    return ack.status();
  }
  return ack.value().status;
}

}  // namespace

Status Client::publish_observation(const PublishObservationRequest& value) {
  auto reply = request(MessageType::PUBLISH_OBSERVATION, encode(value, limits_));
  if (!reply.ok()) {
    return reply.status();
  }
  return decode_ack(reply.value(), limits_);
}

Status Client::publish_artifact(const PublishArtifactRequest& value) {
  auto reply = request(MessageType::PUBLISH_ARTIFACT, encode(value, limits_));
  if (!reply.ok()) {
    return reply.status();
  }
  return decode_ack(reply.value(), limits_);
}

Status Client::commit_trial(const CommitTrialRequest& value) {
  auto reply = request(MessageType::COMMIT_TRIAL, encode(value, limits_));
  if (!reply.ok()) {
    return reply.status();
  }
  return decode_ack(reply.value(), limits_);
}

Status Client::fail_trial(const FailTrialRequest& value) {
  auto reply = request(MessageType::FAIL_TRIAL, encode(value, limits_));
  if (!reply.ok()) {
    return reply.status();
  }
  return decode_ack(reply.value(), limits_);
}

Status Client::heartbeat(const HeartbeatRequest& value) {
  auto reply = request(MessageType::HEARTBEAT, encode(value, limits_));
  if (!reply.ok()) {
    return reply.status();
  }
  return decode_ack(reply.value(), limits_);
}

Status Client::cancel_trial(const CancelTrialRequest& value) {
  auto reply = request(MessageType::CANCEL_TRIAL, encode(value, limits_));
  if (!reply.ok()) {
    return reply.status();
  }
  return decode_ack(reply.value(), limits_);
}

Status Client::cancel_experiment(const CancelExperimentRequest& value) {
  auto reply = request(MessageType::CANCEL_EXPERIMENT, encode(value, limits_));
  if (!reply.ok()) {
    return reply.status();
  }
  return decode_ack(reply.value(), limits_);
}

Result<Decision> Client::finalize_experiment(const FinalizeExperimentRequest& value) {
  auto reply = request(MessageType::FINALIZE_EXPERIMENT, encode(value, limits_));
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::FINALIZE_EXPERIMENT) {
    return make_error(ErrorCode::PROTOCOL_ERROR, ErrorStage::TRANSPORT, "unexpected reply type for finalization");
  }
  auto decoded = decode_decision_reply(reply.value().payload, limits_);
  if (!decoded.ok()) {
    return decoded.status();
  }
  if (decoded.value().first.failed()) {
    return decoded.value().first;
  }
  return decoded.value().second;
}

Status Client::rollback(const RollbackRequest& value) {
  auto reply = request(MessageType::ROLLBACK, encode(value, limits_));
  if (!reply.ok()) {
    return reply.status();
  }
  return decode_ack(reply.value(), limits_);
}

Result<std::vector<std::string>> Client::query(QueryKind kind, const std::string& argument) {
  auto reply = request(MessageType::QUERY, encode_query_request(kind, argument, limits_));
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::QUERY_RESULT) {
    return make_error(ErrorCode::PROTOCOL_ERROR, ErrorStage::TRANSPORT, "unexpected reply type for query");
  }
  auto decoded = decode_query_result(reply.value().payload, limits_);
  if (!decoded.ok()) {
    return decoded.status();
  }
  return unwrap_query_lines(decoded.value());
}

Status Client::shutdown() {
  Frame frame;
  frame.type = MessageType::SHUTDOWN;
  frame.correlation = next_correlation();
  const Status sent = connection_ == nullptr ? Status(make_error(ErrorCode::NOT_FOUND, ErrorStage::TRANSPORT,
                                                                "client is not connected"))
                                            : connection_->send_frame(frame, limits_);
  if (!sent.ok()) {
    return sent;
  }
  auto reply = connection_->receive_frame(limits_);
  if (!reply.ok()) {
    return reply.status();
  }
  return decode_ack(reply.value(), limits_);
}

Result<Frame> Client::send_raw_and_receive(std::span<const std::uint8_t> bytes) {
  if (connection_ == nullptr) {
    return make_error(ErrorCode::NOT_FOUND, ErrorStage::TRANSPORT, "client is not connected");
  }
  const Status sent = connection_->send_all(bytes);
  if (!sent.ok()) {
    return sent;
  }
  return connection_->receive_frame(limits_);
}

Status Client::send_raw(std::span<const std::uint8_t> bytes) {
  if (connection_ == nullptr) {
    return make_error(ErrorCode::NOT_FOUND, ErrorStage::TRANSPORT, "client is not connected");
  }
  return connection_->send_all(bytes);
}

}  // namespace experiment_fabric
